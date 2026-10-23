"""Prepare/inspect owned quality fixtures without importing Torch or starting jobs.

GPU commands are plans only. This CLI intentionally has no execute subcommand.
Python 3.8-compatible; uses the existing CPU camera/initialization contracts.
"""
import argparse
import copy
import hashlib
import json
import os
import random
from pathlib import Path, PurePosixPath
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

DEFAULT_PROTOCOL = ROOT / "docs/quality_upgrade/benchmark_protocol.json"
CASE_NAMES = ("T0", "T1-base", "T1-distance-near", "T1-distance-far", "T1-focal", "T1-resize")
PREPARED_ACCEPTANCE = {"cpu_preparation": "pass", "gpu": "not_run", "worker": "not_run", "native": "not_run",
    "quality": "not_evaluable", "geometry": "not_run", "real_world": "not_run", "promotion": "not_allowed"}


def validate_protocol(protocol):
    if protocol.get("schema") != "gsw.quality_benchmark_protocol/1":
        raise ValueError("benchmark_protocol_schema")
    if protocol.get("seeds") != [17, 29]:
        raise ValueError("benchmark_seed_policy")
    if protocol.get("profile", {}).get("scheduled_steps") != 64:
        raise ValueError("baseline_step_cap")
    profile = protocol["profile"]
    supported = {"strategy": "standard", "checkpoint_steps": [64], "resolution_policy": "preserve",
        "data_device": "cpu", "initial_point_count": 128, "initial_color_policy": "neutral_0.5_rgb",
        "initialization_policy": "independent_volume_random_not_reference_geometry",
        "densify_from_iter": 500, "densification_interval": 100, "densify_until_iter": 64,
        "densify_grad_threshold": .0002, "three_dgs_filter": "legacy_ewa_antialiasing_true",
        "two_dgs_filter": "surfel_default", "depth_supervision": False, "mask_supervision": False,
        "postprocess": "none", "gpu_shared_preview": False, "network_viewer": False}
    if any(profile.get(key) != value or (isinstance(value, bool) and type(profile.get(key)) is not bool)
           for key, value in supported.items()):
        raise ValueError("benchmark_unsupported_baseline_profile")
    if protocol.get("backends") != ["3dgs", "2dgs"]:
        raise ValueError("benchmark_backend_matrix_changed")
    budget = protocol.get("budget", {})
    if (budget.get("total_gpu_seconds") != 600 or budget.get("total_wall_seconds") != 1200
            or budget.get("maximum_trials") != 4):
        raise ValueError("benchmark_budget_changed")
    if protocol.get("promotion_allowed") is not False:
        raise ValueError("baseline_cannot_promote")
    thresholds = protocol.get("quality_thresholds", {})
    if (thresholds.get("status") != "not_evaluable_in_p0" or any(thresholds.get(key) is not None for key in
            ("minimum_primary_gain_db", "maximum_other_roi_drop_db", "maximum_lpips_regression", "maximum_geometry_regression"))):
        raise ValueError("benchmark_baseline_has_no_quality_acceptance_threshold")
    return copy.deepcopy(protocol)


def load_protocol(path=DEFAULT_PROTOCOL):
    with Path(path).open(encoding="utf-8") as stream:
        return validate_protocol(json.load(stream))


def json_digest(value):
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False, allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")


def _fresh_output(output):
    path = Path(output)
    if not path.is_absolute() or path.drive.upper() != "D:":
        raise ValueError("benchmark_D_output_required")
    if path.exists():
        raise ValueError("benchmark_fresh_output_required")
    for ancestor in path.parents:
        if ancestor.exists() and (ancestor.is_symlink() or
                getattr(ancestor.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("benchmark_reparse_output_rejected")
    path.mkdir(parents=True, exist_ok=False)
    return path.resolve()


def _camera(position, width=128, height=96, focal_factor=1.):
    import numpy as np
    centre = np.asarray(position, dtype="float64")
    forward = -centre / np.linalg.norm(centre)
    right = np.cross([0., 1., 0.], forward)
    right /= np.linalg.norm(right)
    down = np.cross(forward, right)
    pose = np.eye(4)
    pose[:3, :3] = np.column_stack((right, down, forward))
    pose[:3, 3] = centre
    return {"size": [width, height], "K": [[width * .91 * focal_factor, 0., width * .46],
        [0., height * 1.16 * focal_factor, height * .48], [0., 0., 1.]], "c2w_opencv": pose.tolist()}


def _render(camera):
    """Analytic ray/plane intersections, no Gaussian renderer or GT initializer."""
    import numpy as np
    width, height = camera["size"]
    k, pose = np.asarray(camera["K"]), np.asarray(camera["c2w_opencv"])
    columns, rows = np.meshgrid(np.arange(width) + .5, np.arange(height) + .5)
    rays = np.stack(((columns - k[0, 2]) / k[0, 0], (rows - k[1, 2]) / k[1, 1],
                     np.ones_like(rows)), axis=-1) @ pose[:3, :3].T
    centre = pose[:3, 3]
    depth = np.full((height, width), np.inf)
    labels = np.zeros((height, width), dtype="uint8")
    pixels = np.empty((height, width, 3), dtype="uint8")
    pixels[:] = [18, 22, 28]
    for label, z in ((1, 0.), (2, -.45), (3, -.7)):
        distance = (z - centre[2]) / rays[:, :, 2]
        hit = centre + distance[:, :, None] * rays
        x, y = hit[:, :, 0], hit[:, :, 1]
        if label == 1:
            inside = (np.abs(x) <= 1.5) & (np.abs(y) <= 1.1)
            checker = ((np.floor((x + 1.5) / .12) + np.floor((y + 1.1) / .12)) % 2) == 0
            gray = np.where(checker, 218, 42)
            fine = (np.abs(x - .13) < .012) | (np.abs(y + .18) < .012)
            color = np.stack((gray, np.where(fine, 10, gray), np.where(fine, 235, gray)), axis=-1)
        elif label == 2:
            inside = (((x >= -.75) & (x <= -.50) & (y >= -.6) & (y <= .6)) |
                      ((x >= -.75) & (x <= .5) & (y >= .35) & (y <= .6)))
            color = np.empty_like(pixels); color[:] = [218, 68, 32]
        else:
            inside = (np.abs(x - .65) < .018) & (y >= -.65) & (y <= .7)
            color = np.empty_like(pixels); color[:] = [32, 215, 115]
        visible = inside & (distance > 0) & (distance < depth)
        pixels[visible] = color[visible]
        depth[visible], labels[visible] = distance[visible], label
    depth[labels == 0] = np.nan
    return pixels, depth.astype("<f8"), labels


def _save_array(path, array):
    import numpy as np
    with Path(path).open("xb") as stream:
        np.save(stream, array, allow_pickle=False)


def reference_rng_trace(seed, view_ids, steps=64, noise_draws_per_step=0):
    """A CPU reference contract, never evidence of actual trainer independence."""
    if type(seed) is not int or seed not in (17, 29) or type(steps) is not int or not 1 <= steps <= 64:
        raise ValueError("benchmark_rng_reference_bounds")
    if type(noise_draws_per_step) is not int or not 0 <= noise_draws_per_step <= 1024:
        raise ValueError("benchmark_rng_reference_bounds")
    names = sorted(view_ids)
    if not names or len(set(names)) != len(names) or any(not isinstance(name, str) for name in names):
        raise ValueError("benchmark_rng_view_ids")
    sampler = random.Random(seed)
    noise_seed = int(hashlib.sha256(("strategy-noise:%d" % seed).encode("ascii")).hexdigest()[:16], 16)
    noise = random.Random(noise_seed)
    initial = {"sampler": json_digest(sampler.getstate()), "strategy_noise": json_digest(noise.getstate())}
    pending, sequence = [], []
    for iteration in range(1, steps + 1):
        if not pending:
            pending = list(names)
        sequence.append({"iteration": iteration, "view_id": pending.pop(sampler.randrange(len(pending)))})
        for unused in range(noise_draws_per_step):
            noise.random()
    return {"schema": "gsw.quality_rng_reference/1", "seed": seed,
        "view_sequence": sequence, "view_sequence_sha256": json_digest(sequence),
        "initial_state_sha256": initial, "sampler_final_state_sha256": json_digest(sampler.getstate()),
        "noise_final_state_sha256": json_digest(noise.getstate()), "noise_draws_per_step": noise_draws_per_step,
        "sampler_implementation": "isolated_python_mt19937_without_replacement_reference",
        "actual_training_trace_status": "not_run", "strategy_noise_status": "reference_only_not_mcmc_implementation"}


def _generate_case(output, name, protocol):
    import numpy as np
    from PIL import Image
    from native.backend import camera_dataset
    from native.backend.initialization import resolve_initialization_points
    from native.worker.canonical_training import prepare_training_dataset
    positions = ((-.55, -.2, -2.5), (.5, -.1, -2.7), (.05, .35, -2.6),
                 (-.3, .25, -2.8), (.3, -.35, -2.45))
    layout_rng = random.Random(protocol["fixture_generation"]["layout_seed"])
    dimensions = [(128, 96)]
    for unused in range(4):
        width, height = layout_rng.randrange(60, 81) * 2, layout_rng.randrange(40, 56) * 2
        dimensions.append((width, height))
    cameras = [_camera(position, *size) for position, size in zip(positions, dimensions)]
    for camera in cameras:
        if name in ("T1-distance-near", "T1-distance-far"):
            factor = .75 if name == "T1-distance-near" else 1.5
            for axis in range(3):
                camera["c2w_opencv"][axis][3] *= factor
        elif name == "T1-focal":
            camera["K"][0][0] *= 1.4
            camera["K"][1][1] *= 1.4
        elif name == "T1-resize":
            camera["size"] = [value // 2 for value in camera["size"]]
            for row in (0, 1):
                camera["K"][row] = [value * .5 for value in camera["K"][row]]
    references = output / "references" / name
    references.mkdir(parents=True)
    images = []
    for index, camera in enumerate(cameras):
        rgb, depth, labels = _render(camera)
        images.append(rgb)
        Image.fromarray((labels > 0).astype("uint8") * 255).save(str(references / ("object-%02d.png" % index)))
        Image.fromarray((labels > 1).astype("uint8") * 255).save(str(references / ("detail-%02d.png" % index)))
        Image.fromarray((labels == 0).astype("uint8") * 255).save(str(references / ("background-%02d.png" % index)))
        _save_array(references / ("camera-z-%02d.npy" % index), depth)
    write_json(references / "reference-role.json", {
        "role": "evaluation_only_never_initialization_or_training_supervision",
        "depth": "camera_z", "unit": "m", "invalid": "nan", "pixel_convention": "pixel_border",
        "scene_features": ["plane", "checkerboard", "l_structure", "thin_geometry"],
        "real_geometry_accuracy": "unmeasured", "oracle_training": False})
    bindings = []
    for seed in protocol["seeds"]:
        dataset = output / "datasets" / name / ("seed-%d" % seed)
        dataset.mkdir(parents=True)
        views = []
        for index, (camera, rgb) in enumerate(zip(cameras, images)):
            split = "train" if index < 3 else ("val" if index == 3 else "test")
            relative = "%s/frame-%03d.png" % (split, index * 7 + 3)
            target = dataset / relative
            target.parent.mkdir(exist_ok=True)
            Image.fromarray(rgb).save(str(target))
            width, height = camera["size"]
            views.append({"view_id": "view-%03d" % index, "image_path": relative,
                "width": width, "height": height, "K": camera["K"],
                "c2w_opencv": camera["c2w_opencv"], "pixel_coordinates": "pixel_border", "split": split})
        raw = {"schema": "gsw.camera_dataset/1", "pose_policy": "locked",
            "source": {"kind": "quality_owned_analytic_fixture", "fixture_id": name,
                "claim": "software_reference_not_physical_geometry_accuracy"},
            "scale": {"status": "source_declared", "source_unit": "m", "meters_per_unit": 1.,
                "evidence": [{"kind": "generated_coordinate_definition", "description": "analytic units only"}]},
            "views": views, "initialization": {"kind": "random", "count": protocol["profile"]["initial_point_count"],
                "seed": seed, "frame": "source", "priors": [], "bounds": [[-1.2, -.9, -.85], [1.2, .9, .15]],
                "provenance": {"source": "declared_coarse_volume_not_reference_surface"}}}
        camera_dataset.write_camera_dataset(raw, dataset / "camera_dataset.json")
        data = camera_dataset.load_camera_dataset(dataset / "camera_dataset.json")
        camera_dataset.write_camera_dataset(data, dataset / "camera_dataset.json")
        points, colors, resolved = resolve_initialization_points(data, dataset)
        evidence = output / "initialization" / name / ("seed-%d" % seed)
        evidence.mkdir(parents=True)
        _save_array(evidence / "canonical-points.npy", np.ascontiguousarray(points, dtype="<f8"))
        _save_array(evidence / "canonical-colors.npy", np.ascontiguousarray(colors, dtype="<f8"))
        prepared = prepare_training_dataset(dataset, output / "prepared" / name / ("seed-%d" % seed),
                                            {"resolutionPolicy": "preserve"})
        trace_path = output / "rng-reference" / name / ("seed-%d.json" % seed)
        reference = reference_rng_trace(seed, data["splits"]["train"])
        changed_noise = reference_rng_trace(seed, data["splits"]["train"], noise_draws_per_step=13)
        write_json(trace_path, {"reference": reference, "changed_noise": changed_noise,
            "cpu_reference_invariant": reference["view_sequence"] == changed_noise["view_sequence"],
            "production_training_invariant_status": "not_run"})
        camera_records = [{"view_id": v["view_id"], "size": [v["width"], v["height"]],
                           "K": v["K"], "c2w_opencv": v["c2w_opencv"], "split": v["split"]} for v in data["views"]]
        bindings.append({"fixture_id": name, "seed": seed, "data_identity": data["data_identity"],
            "dataset_root": dataset.relative_to(output).as_posix(),
            "manifest_sha256": file_digest(dataset / "camera_dataset.json"),
            "camera_sha256": json_digest(camera_records), "split_sha256": json_digest(data["splits"]),
            "split_counts": [len(data["splits"][split]) for split in ("train", "val", "test")],
            "image_set_sha256": json_digest({v["image_path"]: v["sha256"] for v in data["views"]}),
            "features": ["plane", "checkerboard", "l_structure", "thin_geometry"], "cameras": camera_records,
            "reference_root": references.relative_to(output).as_posix(), "reference_role": "evaluation_only",
            "prepared_manifest": Path(prepared["manifestPath"]).relative_to(output).as_posix(),
            "prepared_contract_sha256": file_digest(prepared["contractPath"]),
            "training_normalization": prepared["contract"]["transforms"]["canonical_to_training"],
            "initialization": {"kind": "random_volume", "point_count": len(points), "seed": seed,
                "recipe_points_colors_sha256": resolved["point_sha256"],
                "points_sha256": hashlib.sha256(np.ascontiguousarray(points, dtype="<f8").tobytes()).hexdigest(),
                "colors_sha256": hashlib.sha256(np.ascontiguousarray(colors, dtype="<f8").tobytes()).hexdigest(),
                "training_artifact_sha256": file_digest(prepared["initializationPath"]),
                "evidence_root": evidence.relative_to(output).as_posix()},
            "sampler_trace": {"actual_status": "not_run", "reference_path": trace_path.relative_to(output).as_posix(),
                "reference_sequence_sha256": reference["view_sequence_sha256"]},
            "strategy_noise_trace": {"actual_status": "not_run", "reason": "no_method_kernel_executed"}})
    return bindings


def _gpu_plan(output, fixtures, protocol):
    runs = []
    for backend in protocol["backends"]:
        prefix = (os.environ.get("GAUSSIAN_SPLATTING_CONDA_PREFIX", "D:/Apps/GSW/runtime/envs/gaussian_splatting")
                  if backend == "3dgs" else str(Path(os.environ.get("TWO_DGS_PYTHON", "D:/Apps/GSW/runtime/envs/two_dgs/python.exe")).parent))
        python = Path(prefix) / "python.exe"
        cwd = ROOT / "gaussian-splatting" if backend == "3dgs" else Path(os.environ.get("TWO_DGS_DIR", "D:/Apps/GSW/runtime/2dgs"))
        entry = ROOT / "gaussian-splatting/train.py" if backend == "3dgs" else ROOT / "native/worker/two_dgs_train.py"
        for binding in fixtures:
            if binding["fixture_id"] != "T0":
                continue
            seed = binding["seed"]
            model = output / "planned-runs" / backend / ("seed-%d" % seed)
            profile = protocol["profile"]
            argv = [str(python), "-B", str(entry), "-s", str(output / binding["dataset_root"]), "-m", str(model),
                "-r", "1", "--data_device", "cpu", "--iterations", "64", "--training_seed", str(seed),
                "--save_iterations", "64", "--test_iterations", "64", "--checkpoint_iterations", "64",
                "--densify_from_iter", str(profile["densify_from_iter"]),
                "--densification_interval", str(profile["densification_interval"]),
                "--densify_until_iter", str(profile["densify_until_iter"]),
                "--densify_grad_threshold", str(profile["densify_grad_threshold"])]
            if backend == "3dgs":
                argv += ["--disable_viewer", "--antialiasing"]
            control = {"output": str(model), "request": str(model / "pause-request.json"),
                "session": "quality-p0-%s-%d" % (backend, seed), "backend": backend, "resume": False}
            environment = {"GSW_CAMERA_DATASET": str(output / binding["prepared_manifest"]),
                "GSW_CAMERA_IMAGE_ROOT": str(output / binding["dataset_root"]), "GSW_POSE_POLICY": "locked",
                "GSW_NATIVE_TRAINING_CONTROL": json.dumps(control, separators=(",", ":")),
                "PYTHONUTF8": "1", "PYTHONIOENCODING": "utf-8", "TEMP": "D:/CodexData/Temp", "TMP": "D:/CodexData/Temp",
                "PYTHONPATH": str(ROOT), "CONDA_PREFIX": str(python.parent)}
            if backend == "2dgs":
                environment["GSW_TWO_DGS_SOURCE"] = str(cwd)
            runs.append({"backend": backend, "fixture_id": "T0", "seed": seed, "argv": argv,
                "cwd": str(cwd), "environment_overrides": environment,
                "environment_inheritance": "existing_runtime_contract_no_resource_override",
                "status": "not_run", "full_state_resume_accepted": False,
                "checkpoint_kind": "legacy_scheduled_not_complete_state",
                "expected_outputs": {"model_root": str(model), "samples": "training-camera-samples.jsonl",
                    "ply": "point_cloud/iteration_64/point_cloud.ply", "legacy_checkpoint": "chkpnt64.pth"}})
    return {"executed": False, "requires_separate_root_approval": True, "runs": runs,
        "budget": copy.deepcopy(protocol["budget"]), "worker_path_status": "not_run_minimum_1000_steps",
        "operator_requirement": "Run serially under a budget supervisor; reserve <=600 total child wall seconds as conservative GPU upper bound, <=1200 total wall seconds. No automatic retries or extra methods."}


def _code_identity():
    names = ["scripts/run_quality_benchmark.py", "native/backend/camera_dataset.py", "native/backend/initialization.py",
        "native/worker/canonical_training.py", "native/worker/training_seed.py", "native/worker/training_checkpoint.py",
        "native/worker/training_runtime.py", "native/worker/two_dgs_train.py", "gaussian-splatting/train.py"]
    return {"hash_mode": "text-lf-v1", "files": {name: hashlib.sha256((ROOT / name).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        for name in names}}


def _runtime_candidates():
    result = {"acceptance": "metadata_only_not_import_abi_gpu_or_render_acceptance", "backends": {}}
    for backend, prefix in (("3dgs", Path(os.environ.get("GAUSSIAN_SPLATTING_CONDA_PREFIX", "D:/Apps/GSW/runtime/envs/gaussian_splatting"))),
            ("2dgs", Path(os.environ.get("TWO_DGS_PYTHON", "D:/Apps/GSW/runtime/envs/two_dgs/python.exe")).parent)):
        files = [prefix / "python.exe", prefix / "Lib/site-packages/torch/version.py"]
        module = "diff_gaussian_rasterization" if backend == "3dgs" else "diff_surfel_rasterization"
        for name in (module, "simple_knn"):
            files += sorted((prefix / "Lib/site-packages" / name).glob("*.pyd"))
        source = ROOT / "gaussian-splatting" if backend == "3dgs" else Path(os.environ.get("TWO_DGS_DIR", "D:/Apps/GSW/runtime/2dgs"))
        files += [source / name for name in ("scene/gaussian_model.py", "scene/__init__.py", "scene/dataset_readers.py",
            "arguments/__init__.py", "gaussian_renderer/__init__.py")]
        result["backends"][backend] = {"python": str(prefix / "python.exe"), "source_root": str(source),
            "files_sha256": {str(path): file_digest(path) for path in files if path.is_file()},
            "missing_files": [str(path) for path in files if not path.is_file()]}
    proof = Path("D:/Apps/GSW/runtime/kernels/camera-pinhole3d-v1/camera-rasterizer-proof.json")
    if proof.is_file():
        result["known_camera_3dgs_proof"] = {"path": str(proof), "sha256": file_digest(proof),
            "acceptance": "file_identity_only_see_environment_audit_for_source_closure"}
    return result


def prepare_benchmark(output, protocol_path=DEFAULT_PROTOCOL, case_names=None):
    protocol = load_protocol(protocol_path)
    names = list(CASE_NAMES if case_names is None else case_names)
    if not names or len(set(names)) != len(names) or set(names) - set(CASE_NAMES):
        raise ValueError("benchmark_fixture_not_available")
    destination = _fresh_output(output)
    write_json(destination / "benchmark_protocol.json", protocol)
    fixtures = []
    for name in names:
        fixtures.extend(_generate_case(destination, name, protocol))
    files = {path.relative_to(destination).as_posix(): file_digest(path)
             for path in sorted(destination.rglob("*")) if path.is_file()}
    manifest = {"schema": "gsw.quality_benchmark_baseline/1", "status": "prepared", "protocol_sha256": json_digest(protocol),
        "artifact_root": str(destination), "fixtures": fixtures, "files": files,
        "code_identity": _code_identity(), "runtime_candidates": _runtime_candidates(),
        "gpu_execution_plan": _gpu_plan(destination, fixtures, protocol),
        "acceptance": copy.deepcopy(PREPARED_ACCEPTANCE)}
    write_json(destination / "baseline_manifest.json", manifest)
    return manifest


def _artifact_path(root, relative):
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
        raise ValueError("benchmark_artifact_path_unsafe")
    parts = PurePosixPath(relative)
    if parts.is_absolute() or ".." in parts.parts:
        raise ValueError("benchmark_artifact_path_unsafe")
    target = root.joinpath(*parts.parts)
    for path in (target, *target.parents):
        if path == root.parent:
            break
        if path.exists() and (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("benchmark_artifact_path_unsafe")
    try:
        target.resolve().relative_to(root.resolve())
    except ValueError:
        raise ValueError("benchmark_artifact_path_unsafe")
    return target


def inspect_benchmark(manifest_path, expected_manifest_sha256=None):
    from native.backend import camera_dataset
    from native.worker.canonical_training import load_prepared_training_dataset
    path = Path(manifest_path).resolve()
    if path.drive.upper() != "D:":
        raise ValueError("benchmark_D_output_required")
    if expected_manifest_sha256 and file_digest(path) != expected_manifest_sha256.lower():
        raise ValueError("benchmark_manifest_hash_mismatch")
    with path.open(encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema") != "gsw.quality_benchmark_baseline/1" or manifest.get("status") != "prepared":
        raise ValueError("benchmark_baseline_schema")
    if manifest.get("acceptance") != PREPARED_ACCEPTANCE:
        raise ValueError("benchmark_prepared_acceptance_changed")
    root = path.parent
    protocol = load_protocol(root / "benchmark_protocol.json")
    if json_digest(protocol) != manifest["protocol_sha256"]:
        raise ValueError("benchmark_protocol_hash_mismatch")
    if manifest.get("code_identity") != _code_identity():
        raise ValueError("benchmark_code_identity_changed_reprepare_before_execution")
    if manifest.get("runtime_candidates") != _runtime_candidates():
        raise ValueError("benchmark_runtime_identity_changed")
    for relative, expected in manifest["files"].items():
        artifact = _artifact_path(root, relative)
        if not artifact.is_file() or file_digest(artifact) != expected:
            raise ValueError("benchmark_artifact_hash_mismatch: " + relative)
    for binding in manifest["fixtures"]:
        dataset = _artifact_path(root, binding["dataset_root"])
        data = camera_dataset.load_camera_dataset(dataset / "camera_dataset.json")
        if data["data_identity"] != binding["data_identity"] or json_digest(data["splits"]) != binding["split_sha256"]:
            raise ValueError("benchmark_dataset_identity_changed")
        prepared = load_prepared_training_dataset(_artifact_path(root, binding["prepared_manifest"]), dataset)
        if prepared["input_data_identity"] != data["data_identity"]:
            raise ValueError("benchmark_prepared_identity_changed")
    return {"schema": "gsw.quality_benchmark_inspection/1", "input_integrity": "pass",
        "manifest_sha256": file_digest(path), "protocol_sha256": manifest["protocol_sha256"],
        "fixture_bindings": len(manifest["fixtures"]), "files_verified": len(manifest["files"]),
        "acceptance": manifest["acceptance"], "gpu_execution_performed": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare", help="CPU-only fixture generation; export GPU plans without executing them")
    prepare.add_argument("--output", required=True)
    prepare.add_argument("--protocol", default=str(DEFAULT_PROTOCOL))
    prepare.add_argument("--case", action="append", choices=CASE_NAMES, dest="cases")
    inspect = commands.add_parser("inspect", help="Verify frozen inputs and current source identity without execution")
    inspect.add_argument("--manifest", required=True)
    inspect.add_argument("--expected-manifest-sha256")
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            result = prepare_benchmark(args.output, args.protocol, args.cases)
            response = {"status": result["status"], "manifest": str(Path(args.output) / "baseline_manifest.json"),
                "manifest_sha256": file_digest(Path(args.output) / "baseline_manifest.json"),
                "protocol_sha256": result["protocol_sha256"], "fixture_bindings": len(result["fixtures"]),
                "gpu_execution_performed": False, "acceptance": result["acceptance"]}
        else:
            response = inspect_benchmark(args.manifest, args.expected_manifest_sha256)
        print(json.dumps(response, ensure_ascii=True, sort_keys=True, allow_nan=False))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(json.dumps({"status": "failed", "reason": str(error), "gpu_execution_performed": False}, ensure_ascii=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
