"""Opt-in, bounded native CUDA training for an original synthetic README demo.

The supervisor imports only CPU modules. Each backend has its own sequential,
owned child, source copy, logs, native pause control and saved model snapshots.
The allocator target is not an OS GPU-memory hard limit or a quality benchmark.
Nothing is executed merely by importing this module.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
GIB = 1024 ** 3
MIB = 1024 ** 2


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(MIB), b""):
            result.update(chunk)
    return result.hexdigest()


def ordinary_d_path(value, *, exists=True):
    path = Path(value).absolute()
    if path.drive.lower() != "d:":
        raise ValueError("Demo inputs, outputs and repository must be on D: " + str(path))
    for part in (path, *path.parents):
        if part.is_symlink() or part.exists() and getattr(part.lstat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("Links/junctions are not demo-owned ordinary paths: " + str(part))
    return path.resolve(strict=exists)


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
    os.replace(temporary, path)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def gpu_memory():
    executable = shutil.which("nvidia-smi")
    if not executable:
        raise RuntimeError("nvidia-smi unavailable; GPU headroom was not verified")
    result = subprocess.run([executable, "--id=0", "--query-gpu=memory.free,memory.total",
                             "--format=csv,noheader,nounits"], capture_output=True,
                            text=True, timeout=8, check=True)
    values = result.stdout.strip().splitlines()[0].split(",")
    free, total = (int(part.strip()) for part in values)
    if not 0 <= free <= total or total <= 0:
        raise RuntimeError("Invalid nvidia-smi memory observation")
    return {"device_index": 0, "free_mib": free, "total_mib": total}


def clone_dataset(source, output):
    """Copy only explicit original-demo camera/image/recipe inputs, not meshes."""
    from native.backend import camera_dataset
    source = ordinary_d_path(source)
    manifest = source / "camera_dataset.json"
    raw = read_json(manifest)
    initialization = raw.get("initialization", {})
    provenance = raw.get("demo_provenance", {})
    if (raw.get("schema") != "gsw.camera_dataset/1"
            or raw.get("source", {}).get("kind") != "original_procedural_readme_demo"
            or provenance.get("initialization_independent_of_render_geometry") is not True
            or provenance.get("truth_depth_normals_mesh_exported") is not False
            or initialization.get("kind") != "random"
            or initialization.get("priors") != [] or initialization.get("path")
            or initialization.get("provenance", {}).get("geometry_or_depth_consumed") is not False
            or initialization.get("bounds") is None):
        raise ValueError("Only the explicit original no-geometry-prior synthetic demo is admitted")
    relatives = {"camera_dataset.json"}
    for view in raw.get("views", []):
        relatives.add(view["image_path"])
        if view.get("mask_path"):
            relatives.add(view["mask_path"])
    relatives.update(item["path"] for item in raw.get("source", {}).get("files", []))
    copied = output / "dataset"
    copied.mkdir()
    hashes = {}
    for relative in sorted(relatives):
        item = Path(relative)
        if item.is_absolute() or ".." in item.parts or ":" in relative or "\\" in relative:
            raise ValueError("Invalid relative demo input path: " + relative)
        original = ordinary_d_path(source / item)
        original.relative_to(source)
        if not original.is_file():
            raise ValueError("Demo input is not an ordinary file: " + relative)
        target = copied / item
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        hashes[relative] = digest(original)
        if digest(target) != hashes[relative]:
            raise ValueError("Copied demo input hash differs: " + relative)
    shutil.copyfile(manifest, output / "input-manifest.raw.json")
    normalized = camera_dataset.load_camera_dataset(copied / "camera_dataset.json")
    if normalized["pose_policy"] != "locked" or normalized["initialization"]["kind"] != "random":
        raise ValueError("Camera normalization changed the required locked/random policy")
    camera_dataset.write_camera_dataset(normalized, copied / "camera_dataset.json")
    write_json(output / "input-copy.json", {"source": str(source), "copy": str(copied),
        "files": hashes, "input_data_identity": normalized["data_identity"],
        "initialization": normalized["initialization"], "scale": normalized["scale"],
        "split_counts": {key: len(value) for key, value in normalized["splits"].items()}})
    return copied, normalized, hashes


def ply_record(path):
    path = Path(path)
    count = None
    with path.open("rb") as stream:
        header = bytearray()
        while len(header) <= 128 * 1024:
            line = stream.readline()
            if not line:
                break
            header.extend(line)
            if line.startswith(b"element vertex "):
                count = int(line.split()[2])
            if line.strip() == b"end_header":
                break
        else:
            raise ValueError("Oversized PLY header")
    if count is None or count <= 0:
        raise ValueError("Invalid/empty saved model: " + str(path))
    return {"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size,
            "gaussian_count": count}


def audit_cameras(model, prepared):
    contract = read_json(model / "training-camera-contract.json")
    expected = prepared["contract"]
    if contract["input_data_identity"] != expected["input_data_identity"] or contract["splits"] != expected["splits"]:
        raise ValueError("Actual training camera identity/splits differ from the prepared input")
    planned = {item["view_id"]: item for item in expected["views"]}
    for actual in contract["loaded_cameras"]:
        view = planned[actual["view_id"]]
        if (actual["K"] != view["training_K"] or actual["image_size"] != view["training_size"]
                or actual["calibration_requires_grad"] is not False):
            raise ValueError("Actual full-K locked camera contract differs")
    samples_path = model / "training-camera-samples.jsonl"
    samples = [json.loads(line) for line in samples_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    forbidden = set(contract["splits"]["val"] + contract["splits"]["test"])
    if not samples or any(row["split"] != "train" or row["view_id"] in forbidden
                          or row["view_id"] not in contract["splits"]["train"] for row in samples):
        raise ValueError("Non-training or unidentified view entered actual optimization")
    return {"passed": True, "sample_count": len(samples),
        "last_observed_iteration": max(int(row["iteration"]) for row in samples),
        "sampled_training_views": sorted({row["view_id"] for row in samples}),
        "validation_test_excluded_from_optimization": True, "full_K_locked": True,
        "camera_contract_sha256": digest(model / "training-camera-contract.json"),
        "sample_log_sha256": digest(samples_path)}


def child(args):
    """The only CUDA entry; unchanged native trainer is executed with runpy."""
    import torch
    repository = ordinary_d_path(args.repository)
    run_root = ordinary_d_path(args.run_root)
    control = read_json(run_root / "native-control.json")
    torch.cuda.set_device(0)
    total = torch.cuda.get_device_properties(0).total_memory
    torch.cuda.set_per_process_memory_fraction(min(1., args.allocator_mib * MIB / total), 0)
    write_json(run_root / "child-runtime.json", {"python": sys.version, "python_executable": sys.executable,
        "torch": torch.__version__, "torch_cuda": torch.version.cuda,
        "gpu_name": torch.cuda.get_device_name(0), "device_total_bytes": total,
        "allocator_limit_bytes": args.allocator_mib * MIB, "os_gpu_hard_limit": False})
    stop = threading.Event()
    observations = run_root / "allocator-observations.jsonl"
    started = time.monotonic()

    def observe():
        with observations.open("x", encoding="utf-8") as stream:
            while not stop.is_set():
                try:
                    free, total_bytes = torch.cuda.mem_get_info(0)
                    allocated, reserved = torch.cuda.memory_allocated(0), torch.cuda.memory_reserved(0)
                    record = {"seconds": time.monotonic() - started, "allocated_bytes": allocated,
                        "reserved_bytes": reserved, "device_free_bytes": free,
                        "device_total_bytes": total_bytes, "os_hard_limit": False}
                    if reserved >= args.allocator_mib * MIB * .90 or free < 3 * GIB:
                        write_json(control["request"], {"session": control["session"]})
                        record["cooperative_pause_requested"] = True
                except Exception as error:
                    write_json(control["request"], {"session": control["session"]})
                    record = {"seconds": time.monotonic() - started,
                              "observation_error": str(error), "cooperative_pause_requested": True}
                stream.write(json.dumps(record, allow_nan=False) + "\n")
                stream.flush()
                stop.wait(1.)

    observer = threading.Thread(target=observe, name="owned-demo-allocator-observer", daemon=True)
    observer.start()
    entry = (repository / "gaussian-splatting/train.py" if args.backend == "3dgs"
             else repository / "native/worker/two_dgs_train.py")
    sys.path.insert(0, str(entry.parent))
    sys.argv = [str(entry), *read_json(run_root / "trainer-arguments.json")]
    try:
        runpy.run_path(str(entry), run_name="__main__")
    finally:
        stop.set()
        observer.join(timeout=5)


def run_backend(args, backend, dataset, data, output):
    from native.worker.canonical_training import prepare_training_dataset
    from native.worker.training_checkpoint import read_manifest
    run_root = output / backend
    run_root.mkdir()
    prepared = prepare_training_dataset(dataset, run_root / "prepared", {"resolutionPolicy": "preserve"})
    model = run_root / "model"
    model.mkdir()
    session = uuid.uuid4().hex
    pause_path = run_root / "pause-request.json"
    runtime = {"version": 2,
        "resourceGuard": {"enabled": True, "mode": "pause", "usage_fraction": .9,
                          "minimum_spare_mib": 512, "max_consecutive_denials": 3},
        "execution": {"image_staging": True, "camera_prefetch": True, "capacity_storage": True,
                      "surfel_auxiliary": True, "loss_mode": "stock", "engine": "stock",
                      "gpu_slots": 1, "pinned_mib": 32, "gpu_mib": 64},
        "monitoring": {"profile_interval": 100}}
    control = {"output": str(model), "request": str(pause_path), "session": session,
               "backend": backend, "resume": False, "training_seed": args.seed,
               "runtimeOptions": runtime}
    write_json(run_root / "native-control.json", control)
    milestones = sorted({1, args.iterations, *(x for x in (100, 300, 600, 1000, 1500) if x <= args.iterations)})
    train = ["-s", str(dataset), "-m", str(model), "-r", "1", "--data_device", "cpu",
             "--iterations", str(args.iterations), "--test_iterations", "0",
             "--save_iterations", *map(str, milestones), "--checkpoint_iterations", str(args.iterations),
             "--training_seed", str(args.seed), "--densify_until_iter", "450" if args.densify else "0"]
    if args.densify:
        train.extend(["--densify_from_iter", "100", "--densification_interval", "100",
                      "--densify_grad_threshold", "0.0002", "--opacity_reset_interval", "3000"])
    if backend == "3dgs":
        train.extend(["--disable_viewer", "--enable_gpu_preview", "--optimizer_type", "default"])
    write_json(run_root / "trainer-arguments.json", train)
    environment = dict(os.environ)
    python = ordinary_d_path(args.three_dgs_python if backend == "3dgs" else args.two_dgs_python)
    prefix = python.parent
    environment.update(PYTHONUTF8="1", PYTHONIOENCODING="utf-8", PYTHONDONTWRITEBYTECODE="1",
        PYTHONNOUSERSITE="1", TEMP=str(output / "tmp"), TMP=str(output / "tmp"),
        CUDA_CACHE_PATH=str(output / "cache/cuda"), TORCH_HOME=str(output / "cache/torch"),
        TORCH_EXTENSIONS_DIR=str(output / "cache/torch-extensions"),
        TRITON_CACHE_DIR=str(output / "cache/triton"), XDG_CACHE_HOME=str(output / "cache"),
        MPLCONFIGDIR=str(output / "cache/matplotlib"),
        MPLBACKEND="Agg", GS_EDITOR_WORKSPACE_ROOT=str(run_root / "workspace"),
        GAUSSIAN_SPLATTING_CONDA_PREFIX=str(ordinary_d_path(args.three_dgs_python).parent),
        TWO_DGS_PYTHON=str(ordinary_d_path(args.two_dgs_python)),
        TWO_DGS_DIR=str(ordinary_d_path(args.two_dgs_source)),
        GSW_TWO_DGS_SOURCE=str(ordinary_d_path(args.two_dgs_source)),
        GSW_CAMERA_DATASET=prepared["manifestPath"], GSW_CAMERA_IMAGE_ROOT=prepared["imageRoot"],
        GSW_POSE_POLICY="locked", GSW_TRAINING_SEED=str(args.seed),
        GSW_NATIVE_TRAINING_CONTROL=json.dumps(control),
        GSW_CAMERA_RASTERIZER_ROOT=str(ordinary_d_path(args.camera_rasterizer_root)))
    environment.pop("PYTHONPATH", None)
    for key in ("GSW_QUALITY_PLAN", "GSW_QUALITY_KERNEL_ROOT", "GSW_TRAINING_LOSS_MODULE_PATH"):
        environment.pop(key, None)
    environment["PATH"] = os.pathsep.join((str(prefix), str(prefix / "Library/bin"),
        str(prefix / "Scripts"), "D:/Apps/CUDA/11.8/bin", environment.get("PATH", "")))
    before = gpu_memory()
    if before["free_mib"] < args.minimum_free_mib:
        raise RuntimeError("Insufficient verified GPU headroom before " + backend + ": " + str(before))
    command = [str(python), "-B", str(Path(__file__).resolve()), "--child", "--backend", backend,
        "--repository", str(ROOT), "--run-root", str(run_root), "--allocator-mib", str(args.allocator_mib)]
    snapshots = run_root / "snapshots"
    snapshots.mkdir()
    initial = []
    errors = []
    log_path = run_root / "training.log"
    start = time.monotonic()
    process = subprocess.Popen(command, cwd=str(ROOT), env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def retain_log():
        with log_path.open("x", encoding="utf-8") as stream:
            for line in process.stdout:
                stream.write(line)
                stream.flush()
                prefix_text = "[gsw-training-preview]"
                if prefix_text not in line:
                    continue
                try:
                    event, _ = json.JSONDecoder().raw_decode(line.split(prefix_text, 1)[1].lstrip())
                    if event.get("preview_kind") == "gaussian_initial" and not initial:
                        source = ordinary_d_path(event["point_cloud_path"])
                        source.relative_to(model)
                        target = snapshots / "initial-iteration-000000.ply"
                        shutil.copyfile(source, target)
                        initial.append(dict(ply_record(target), iteration=int(event["iteration"]),
                            kind="native_gaussian_initial_observation_not_optimizer_checkpoint"))
                except (OSError, ValueError, KeyError) as error:
                    errors.append(str(error))

    reader = threading.Thread(target=retain_log, name="owned-demo-log-reader", daemon=True)
    reader.start()
    pause_reason = None
    observations = [{"seconds": 0., **before}]
    next_probe = start + 2.
    termination = False
    try:
        while process.poll() is None:
            now = time.monotonic()
            if pause_reason is None and now - start >= max(1., args.seconds_per_backend - args.pause_grace):
                pause_reason = "bounded_wall_time"
                write_json(pause_path, {"session": session})
            if now >= next_probe:
                try:
                    observation = {"seconds": now - start, **gpu_memory()}
                    observations.append(observation)
                    if observation["free_mib"] < 3072 and pause_reason is None:
                        pause_reason = "device_headroom_pressure"
                        write_json(pause_path, {"session": session})
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                    errors.append("GPU observation: " + str(error))
                    if pause_reason is None:
                        pause_reason = "gpu_observation_unavailable"
                        write_json(pause_path, {"session": session})
                next_probe = now + 2.
            if time.monotonic() - start >= args.seconds_per_backend and process.poll() is None:
                termination = True
                process.terminate()  # Only this exact child handle; never enumerate/kill other processes.
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
                break
            time.sleep(.1)
    except BaseException:
        # Interruption also gets an iteration-boundary save opportunity. Never
        # leave this harness's CUDA child orphaned or touch unrelated processes.
        if process.poll() is None:
            write_json(pause_path, {"session": session})
            try:
                process.wait(timeout=args.pause_grace)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        reader.join(timeout=10)
        raise
    reader.join(timeout=10)
    if reader.is_alive():
        errors.append("Owned log reader did not finish")
    saved = []
    for path in sorted((model / "point_cloud").glob("iteration_*/point_cloud.ply")):
        saved.append(dict(ply_record(path), iteration=int(path.parent.name.split("_")[-1]),
                          kind="saved_native_model_not_complete_optimizer_checkpoint"))
    record = {"backend": backend, "command": command, "native_trainer_arguments": train,
        "elapsed_seconds": time.monotonic() - start, "returncode": process.returncode,
        "cooperative_pause_reason": pause_reason, "forced_owned_child_termination": termination,
        "allocator_target_mib": args.allocator_mib, "os_gpu_hard_limit": False,
        "preflight_minimum_free_mib": args.minimum_free_mib, "gpu_observations": observations,
        "training_log": str(log_path), "training_log_sha256": digest(log_path),
        "initial_snapshots": initial, "saved_model_snapshots": saved, "warnings": errors,
        "input_data_identity": data["data_identity"], "quality_or_accuracy_accepted": False,
        "densification_requested": args.densify}
    try:
        allocator_path = run_root / "allocator-observations.jsonl"
        allocator = [json.loads(line) for line in allocator_path.read_text(encoding="utf-8").splitlines() if line.strip()]
        if not allocator or any("observation_error" in row for row in allocator):
            raise ValueError("Own allocator observations are missing or failed")
        record["allocator_observations"] = {"path": str(allocator_path), "sha256": digest(allocator_path),
            "sample_count": len(allocator), "peak_allocated_bytes": max(row["allocated_bytes"] for row in allocator),
            "peak_reserved_bytes": max(row["reserved_bytes"] for row in allocator),
            "device_headroom_min_bytes": min(row["device_free_bytes"] for row in allocator),
            "driver_total_per_process_memory": "not_measured_not_an_os_limit"}
        record["actual_runtime"] = read_json(run_root / "child-runtime.json")
        record["camera_audit"] = audit_cameras(model, prepared)
        if process.returncode == 75:
            manifest, state = read_manifest(model)
            record["complete_state_pause"] = {"iteration": manifest["iteration"],
                "manifest": str(model / ".gsw-resume/ready.json"), "state": str(state),
                "state_sha256": digest(state), "resume_executed": False}
        record["passed"] = process.returncode in (0, 75) and bool(saved) and not termination and not errors
    except (OSError, ValueError, KeyError) as error:
        record.update(passed=False, verification_error=str(error))
    write_json(run_root / "RESULTS.json", record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset")
    parser.add_argument("--output")
    parser.add_argument("--backend", choices=("both", "3dgs", "2dgs"), default="both")
    parser.add_argument("--iterations", type=int, default=1500)
    parser.add_argument("--seconds-per-backend", type=int, default=180)
    parser.add_argument("--pause-grace", type=int, default=25)
    parser.add_argument("--allocator-mib", type=int, default=3072)
    parser.add_argument("--minimum-free-mib", type=int, default=6144)
    parser.add_argument("--seed", type=int, default=73129)
    parser.add_argument("--densify", action="store_true")
    parser.add_argument("--three-dgs-python", default="D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe")
    parser.add_argument("--two-dgs-python", default="D:/Apps/GSW/runtime/envs/two_dgs/python.exe")
    parser.add_argument("--two-dgs-source", default="D:/Apps/GSW/runtime/2dgs")
    parser.add_argument("--camera-rasterizer-root", default="D:/Apps/GSW/runtime/kernels/camera-pinhole3d-quality-20261023-v2")
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--repository", help=argparse.SUPPRESS)
    parser.add_argument("--run-root", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        if args.backend == "both" or not args.repository or not args.run_root:
            parser.error("Incomplete owned child contract")
        return child(args)
    if not args.dataset or not args.output:
        parser.error("--dataset and --output are required")
    if not (1 <= args.iterations <= 3000 and 30 <= args.seconds_per_backend <= 300
            and 10 <= args.pause_grace < args.seconds_per_backend
            and 512 <= args.allocator_mib <= 3072 and args.minimum_free_mib >= 6144
            and 0 <= args.seed <= 2 ** 32 - 1):
        parser.error("Demo limits exceed the bounded training contract")
    global ROOT
    ROOT = ordinary_d_path(ROOT)
    sys.path.insert(0, str(ROOT))
    output = ordinary_d_path(args.output, exists=False)
    source = ordinary_d_path(args.dataset)
    try:
        output.relative_to(source)
    except ValueError:
        pass
    else:
        raise ValueError("Demo output must be outside the source dataset")
    visible = os.environ.get("CUDA_VISIBLE_DEVICES")
    if visible not in (None, "", "0"):
        raise ValueError("Demo GPU0 headroom checks require no conflicting CUDA device mask")
    if output.exists():
        raise FileExistsError("Refusing to overwrite prior demo data: " + str(output))
    output.mkdir(parents=True)
    (output / "tmp").mkdir()
    dataset, data, source_hashes = clone_dataset(args.dataset, output)
    source_commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
        capture_output=True, text=True, timeout=10).stdout.strip()
    source_paths = ("gaussian-splatting/train.py", "native/worker/two_dgs_train.py",
        "native/worker/canonical_training.py", "native/worker/training_checkpoint.py",
        "native/worker/training_runtime.py", "native/worker/training_density_control.py",
        "native/worker/camera_rasterizer_runtime.py", "native/backend/camera_dataset.py",
        "native/backend/initialization.py", "scripts/readme_demo_training.py")
    evidence = {"schema": "gsw.readme_native_training_demo/1", "source_repository": str(ROOT),
        "source_commit_at_start": source_commit,
        "selected_source_files_sha256": {name: digest(ROOT / name) for name in source_paths},
        "selected_source_files_not_a_complete_source_closure": True,
        "input_source": str(ordinary_d_path(args.dataset)), "input_copy": str(dataset),
        "input_data_identity": data["data_identity"], "quality_or_accuracy_accepted": False,
        "physical_scale_accuracy_accepted": False, "initialization_geometry_prior_used": False,
        "test_image_feedback_into_optimization": False, "cases": [], "status": "running"}
    write_json(output / "RESULTS.json", evidence)
    try:
        for backend in (("3dgs", "2dgs") if args.backend == "both" else (args.backend,)):
            case = run_backend(args, backend, dataset, data, output)
            evidence["cases"].append(case)
            write_json(output / "RESULTS.json", evidence)
            if not case["passed"]:
                raise RuntimeError("Owned native demo failed: " + str(output / backend / "RESULTS.json"))
        source = ordinary_d_path(args.dataset)
        evidence["original_inputs_unchanged"] = all(digest(source / name) == expected
                                                    for name, expected in source_hashes.items())
        evidence["status"] = "passed" if evidence["original_inputs_unchanged"] else "failed"
    except BaseException as error:
        evidence.update(status="failed", error=str(error))
        write_json(output / "RESULTS.json", evidence)
        raise
    write_json(output / "RESULTS.json", evidence)
    print(json.dumps({"status": evidence["status"], "results": str(output / "RESULTS.json")}, ensure_ascii=False))
    return 0 if evidence["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
