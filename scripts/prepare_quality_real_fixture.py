"""Read-only COLMAP inspection and copy-only, CPU prepared-fixture creation.

No SfM, undistortion, image rewriting, training, GPU import, or upload occurs.
Use local asset binding IDs, not private photo paths, in public evidence.
Newly split COLMAP fixtures retain all-view SfM pose/point-color conditioning.
Exact generic manifests retain their existing split and declared provenance.
Neither route certifies independent geometry or a pristine final test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from native.backend import camera_dataset

MAX_COPY_BYTES = 2 * 1024 ** 3
RESERVE_BYTES = 10 * 1024 ** 3
DEFAULT_SPLIT_SEED = 20261023


def _digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 ** 2), b""):
            value.update(block)
    return value.hexdigest()


def _json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode("utf-8")).hexdigest()


def _array_identity(values):
    array = np.asarray(values, dtype="<f8", order="C")
    return {"shape": list(array.shape), "dtype": "little_endian_float64_C",
            "sha256": hashlib.sha256(array.tobytes(order="C")).hexdigest()}


def _d_path(value):
    path = Path(value).resolve()
    if path.drive.lower() != "d:":
        raise ValueError("D_drive_required")
    return path


def _source(binding):
    if not isinstance(binding, dict) or not binding.get("authorization"):
        raise ValueError("explicit_local_binding_authorization_required")
    source = _d_path(binding["dataset_root"])
    if not source.is_dir():
        raise ValueError("source_directory_required")
    return source


def _relative_file(root, relative):
    path = root / relative
    resolved = path.resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        raise ValueError("source_path_escape")
    if not path.is_file():
        raise ValueError("source_file_missing")
    return path


def _load_source(binding):
    source = _source(binding)
    adapter = binding.get("adapter", "colmap")
    if adapter == "generic_json":
        manifest = _relative_file(source, binding["camera_manifest"])
        if not binding.get("manifest_sha256") or _digest(manifest) != binding["manifest_sha256"]:
            raise ValueError("approved_manifest_identity_mismatch")
        dataset = camera_dataset.load_camera_dataset(manifest, adapter="generic_json")
        if not binding.get("data_identity") or dataset["data_identity"] != binding["data_identity"]:
            raise ValueError("approved_data_identity_mismatch")
        return dataset
    if adapter != "colmap":
        raise ValueError("explicit_supported_adapter_required")
    return camera_dataset.load_camera_dataset(source, adapter="colmap", options={
        "pose_policy": "locked", "normalization": {"mode": "auto"},
        "scale": {"status": "arbitrary", "source_unit": "unknown",
                  "meters_per_unit": None, "evidence": []}})


def inspect_fixture(binding, *, binding_id="local:unbound", split_seed=DEFAULT_SPLIT_SEED,
                    val_count=None, test_count=None, max_copy_bytes=MAX_COPY_BYTES,
                    reserve_bytes=RESERVE_BYTES):
    """Inspect real files through the production camera adapter; never write.

    Return relative image/model inventory, frozen split and numeric identities.
    Only explicit, undistorted PINHOLE/SIMPLE_PINHOLE COLMAP or an exact
    approved generic manifest is admitted. Existing generic splits are kept.
    Budget failures return `blocked` and reasons without creating directories.
    """
    source = _source(binding)
    dataset = _load_source(binding)
    generic = binding.get("adapter") == "generic_json"
    if generic and (val_count is not None or test_count is not None):
        raise ValueError("generic_existing_split_must_be_preserved")
    if dataset["pose_policy"] != "locked":
        raise ValueError("locked_source_pose_policy_required")
    views = dataset["views"]
    count = len(views)
    expected = binding.get("metadata_image_count")
    if expected is not None and count != expected:
        raise ValueError("binding_image_count_mismatch")
    image_paths = {view["image_path"] for view in views}
    image_files = {path.relative_to(source).as_posix() for path in (source / "images").rglob("*")
                   if path.is_file()}
    if image_paths != image_files:
        raise ValueError("COLMAP_images_inventory_mismatch")
    allowed_initializations = ("sfm_sparse", "external_point_cloud") if generic else ("sfm_sparse",)
    if dataset["initialization"]["kind"] not in allowed_initializations:
        raise ValueError("actual_SfM_initialization_required_no_random_fallback")
    points, colors = camera_dataset.read_initialization_points(dataset, source)
    if not points or len(points) != len(colors):
        raise ValueError("nonempty_SfM_points_and_colors_required")
    val_count = len(dataset["splits"]["val"]) if generic else max(1, round(count * .1)) if val_count is None else val_count
    test_count = len(dataset["splits"]["test"]) if generic else max(1, round(count * .1)) if test_count is None else test_count
    if (type(split_seed) is not int or type(val_count) is not int or type(test_count) is not int
            or min(val_count, test_count) < 1 or val_count + test_count >= count):
        raise ValueError("nonempty_train_val_test_required")
    ranked = sorted(views, key=lambda view: hashlib.sha256(
        (str(split_seed) + "|" + view["sha256"] + "|" + view["image_path"]).encode("utf-8")).digest())
    splits = {view["image_path"]: "test" if i < test_count else
              "val" if i < test_count + val_count else "train"
              for i, view in enumerate(ranked)}
    if generic:
        splits = {view["image_path"]: view["split"] for view in views}
    by_hash = {}
    for view in views:
        by_hash.setdefault(view["sha256"], set()).add(splits[view["image_path"]])
    if any(len(group) > 1 for group in by_hash.values()):
        raise ValueError("cross_split_duplicate_RGB_content")
    model_paths = {row["path"] for row in dataset["source"].get("files", [])}
    model_paths.add(dataset["initialization"]["path"])
    if generic:
        model_paths.add(binding["camera_manifest"])
    inventory = []
    for relative in sorted(image_paths | model_paths):
        path = _relative_file(source, relative)
        row = {"path": relative, "bytes": path.stat().st_size,
               "sha256": _digest(path), "role": "RGB" if relative in image_paths else "camera_contract" if generic and relative == binding["camera_manifest"] else "SfM"}
        if row["role"] == "camera_contract":
            row["copied_path"] = "frozen-source/camera_dataset.json"
        inventory.append(row)
    total = sum(row["bytes"] for row in inventory)
    # Bounded conservative allowance for points/colors, contracts and metadata.
    estimate = total + len(points) * 6 * 8 + 16 * 1024 ** 2
    free = shutil.disk_usage(source.anchor).free
    reasons = []
    if max_copy_bytes <= 0 or reserve_bytes < 0:
        raise ValueError("invalid_copy_budget")
    if estimate > min(max_copy_bytes, MAX_COPY_BYTES):
        reasons.append("copy_budget_exceeded")
    if free < estimate + max(reserve_bytes, RESERVE_BYTES):
        reasons.append("insufficient_D_free_space_with_reserve")
    provenance = {"heldout_role": "heldout_from_GS_loss_only",
        "independent_pose_or_geometry": False, "pristine_final_test": False,
        "prior_training_history": "previously_used_photos_not_pristine",
        "SfM_conditioning": "all_source_views_including_val_test",
        "initialization_color_conditioning": "all_view_SfM_track_RGB_may_include_heldout",
        "native_independent_test_flag_meaning": "RGB_hash_split_integrity_only_not_pose_geometry_or_history",
        "independent_geometry_reference": None, "metric_scale_evidence": None}
    if generic:
        provenance.update(SfM_conditioning="source_manifest_provenance_preserved_not_reconstructed",
            initialization_color_conditioning=dataset["initialization"].get("provenance", {"declaration": "source_colors_preserved"}),
            source_initialization_priors=dataset["initialization"]["priors"],
            evaluation_pose_origin="source_manifest_preserved_no_adjacent_metric_or_GT_pose_substitution",
            source_geometry_reference_not_implicitly_admitted=True)
    return {"schema": "gsw.quality_real_fixture_inspection/1", "status": "blocked" if reasons else "ready",
        "binding_id": binding_id, "source_adapter": binding.get("adapter", "colmap"),
        "source_kind": dataset["source"]["kind"], "image_count": count, "image_bytes": sum(row["bytes"] for row in inventory if row["role"] == "RGB"),
        "copy_bytes": total, "estimated_output_bytes": estimate, "free_D_bytes": free,
        "copy_budget_bytes": min(max_copy_bytes, MAX_COPY_BYTES), "reserve_bytes": max(reserve_bytes, RESERVE_BYTES),
        "blocked_reasons": reasons, "source_data_identity": dataset["data_identity"],
        "source_inventory": inventory, "source_inventory_sha256": _json_digest(inventory),
        "camera_contract": {"projection": "undistorted_PINHOLE_or_SIMPLE_PINHOLE_only",
            "canonical_projection": sorted({v["projection_model"] for v in views}),
            "sizes": [list(size) for size in sorted({(v["width"], v["height"]) for v in views})],
            "pose_policy": "locked", "pixel_coordinates": "pixel_border", "resolution_policy": "preserve"},
        "split_seed": None if generic else split_seed,
        "split_algorithm": "existing_approved_manifest_split_preserved_v1" if generic else "SHA256(seed|RGBsha256|relative_path)_ascending_test_then_val_then_train_v1",
        "split_by_path": dict(sorted(splits.items())), "split_counts": [count - val_count - test_count, val_count, test_count],
        "training_seeds": [17, 29], "scale": dataset["scale"],
        "initialization": {"kind": dataset["initialization"]["kind"], "point_count": len(points),
            "source_file_sha256": dataset["initialization"]["sha256"],
            "canonical_points": _array_identity(points), "source_colors": _array_identity(colors)},
        "evaluation_provenance": provenance}


def _fresh_output(output, source):
    unresolved = Path(os.path.abspath(str(output)))
    # Existing reparse parents must not silently redirect the approved output.
    for path in [unresolved] + list(unresolved.parents):
        if path.exists() and (path.is_symlink() or getattr(path.stat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("output_reparse_point_rejected")
    output = _d_path(unresolved)
    if output.exists():
        raise ValueError("fresh_output_required")
    if output == source or source in output.parents or output in source.parents:
        raise ValueError("source_output_overlap")
    return output


def prepare_fixture(binding, output, *, binding_id="local:unbound", run_parameters=None,
                    split_seed=DEFAULT_SPLIT_SEED, val_count=None, test_count=None,
                    max_copy_bytes=MAX_COPY_BYTES, reserve_bytes=RESERVE_BYTES):
    """Create a fresh D-only copy and actual production prepared contract.

    Success is `prepared_not_trained`, never GPU/quality/geometry acceptance.
    Failed partial copies remain explicitly incomplete; no source is altered.
    Hyperparameters/ROI/thresholds without supplied evidence remain unbound.
    """
    source = _source(binding)
    output = _fresh_output(output, source)
    inspected = inspect_fixture(binding, binding_id=binding_id, split_seed=split_seed,
        val_count=val_count, test_count=test_count, max_copy_bytes=max_copy_bytes, reserve_bytes=reserve_bytes)
    if inspected["status"] != "ready":
        raise ValueError("fixture_budget_blocked:" + ",".join(inspected["blocked_reasons"]))
    if run_parameters is not None:
        if not isinstance(run_parameters, dict) or not run_parameters:
            raise ValueError("run_parameters_must_be_explicit_nonempty_JSON_object")
        _json_digest(run_parameters)  # Reject NaN/non-JSON before output creation.
    output.mkdir(parents=True, exist_ok=False)
    try:
        for row in inspected["source_inventory"]:
            original = _relative_file(source, row["path"])
            destination = output / row.get("copied_path", row["path"])
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(str(original), str(destination))  # Never hardlink private originals.
            if _digest(destination) != row["sha256"]:
                raise ValueError("copied_file_identity_changed")
        if binding.get("adapter") == "generic_json":
            dataset = _load_source(binding)
        else:
            dataset = camera_dataset.load_camera_dataset(output, adapter="colmap", options={
                "split_by_path": inspected["split_by_path"], "pose_policy": "locked",
                "normalization": {"mode": "auto"}, "scale": inspected["scale"]})
        dataset["quality_evaluation_provenance"] = inspected["evaluation_provenance"]
        dataset = camera_dataset.refresh_dataset_identity(dataset)
        dataset = camera_dataset.validate_camera_dataset(dataset, output)
        camera_dataset.write_camera_dataset(dataset, output / "camera_dataset.json")
        from native.worker.canonical_training import prepare_training_dataset, load_prepared_training_dataset
        prepared = prepare_training_dataset(output / "camera_dataset.json", output / "prepared",
                                            config={"resolutionPolicy": "preserve"})
        reloaded = load_prepared_training_dataset(prepared["manifestPath"], output)
        initial = np.load(prepared["initializationPath"], allow_pickle=False)
        try:
            initial_points, initial_colors = initial["points"], initial["colors"]
            actual_initialization = {"points": _array_identity(initial_points), "colors": _array_identity(initial_colors)}
        finally:
            initial.close()
        after = []
        for row in inspected["source_inventory"]:
            original = _relative_file(source, row["path"])
            after.append(dict(row, bytes=original.stat().st_size, sha256=_digest(original)))
        if after != inspected["source_inventory"]:
            raise ValueError("original_source_changed_during_preparation")
        files = ["camera_dataset.json", "prepared/training-camera-dataset.json",
                 "prepared/training-camera-contract.json", "prepared/training-initialization.npz"]
        manifest = dict(inspected, schema="gsw.quality_real_fixture/1", status="prepared_not_trained",
            source_unchanged=True, source_unchanged_scope="all_copied_RGB_and_selected_source_camera_initialization_files",
            source_inventory_after_sha256=_json_digest(after), copy_mode="independent_byte_copy_not_hardlink",
            source_data_identity=inspected["source_data_identity"], data_identity=dataset["data_identity"],
            prepared_contract_sha256=_json_digest(prepared["contract"]),
            prepared_manifest="prepared/training-camera-dataset.json",
            prepared_initialization="prepared/training-initialization.npz",
            prepared_initialization_actual=actual_initialization,
            prepared_point_count=len(reloaded["initial_points"]),
            generated_files={name: _digest(output / name) for name in files},
            run_parameters={"status": "bound_snapshot_not_execution_admission" if run_parameters else "unbound",
                            "config": run_parameters, "sha256": _json_digest(run_parameters) if run_parameters else None},
            ROI_reference=None, quality_thresholds=None,
            acceptance={"CPU_preparation": "pass", "GPU": "not_run", "worker": "not_run",
                        "appearance_quality": "not_run", "geometry": "unmeasured", "native_viewer": "not_run"},
            limitations=["No independent geometry reference or metric scale; arbitrary SfM units.",
                "Heldout RGB is excluded only from subsequent GS loss; source pose/geometry/color provenance remains conditioned as disclosed.",
                "Source photos were previously used: this is not a pristine final test.",
                "ROI, quality thresholds and any unbound trainer parameters must be frozen before quality trials.",
                "Non-cube/material/occlusion suitability still requires explicit review; preparation does not prove it."])
        manifest["fixture_identity"] = _json_digest(manifest)
        camera_dataset.write_camera_dataset(manifest, output / "fixture_manifest.json")
        return manifest
    except Exception as error:
        camera_dataset.write_camera_dataset({"schema": "gsw.quality_real_fixture_failure/1",
            "status": "incomplete_not_admitted", "error_type": type(error).__name__, "reason": str(error),
            "source_write_attempted": False, "GPU_executed": False}, output / "preparation_failure.json")
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("inspect", "prepare"))
    parser.add_argument("--bindings", required=True, help="D-only local binding JSON; never publish its private paths")
    parser.add_argument("--binding-id", required=True)
    parser.add_argument("--output-root", help="Fresh D-only directory, required only for prepare")
    parser.add_argument("--split-seed", type=int, default=DEFAULT_SPLIT_SEED)
    parser.add_argument("--val-count", type=int)
    parser.add_argument("--test-count", type=int)
    parser.add_argument("--run-parameters", help="Optional frozen JSON snapshot; absence remains unbound")
    arguments = parser.parse_args(argv)
    binding_document = json.loads(_d_path(arguments.bindings).read_text(encoding="utf-8"))
    binding = binding_document["bindings"][arguments.binding_id]
    common = dict(binding_id=arguments.binding_id, split_seed=arguments.split_seed,
                  val_count=arguments.val_count, test_count=arguments.test_count)
    if arguments.mode == "inspect":
        if arguments.output_root or arguments.run_parameters:
            parser.error("inspect is read-only; do not provide output/run parameters")
        result = inspect_fixture(binding, **common)
    else:
        if not arguments.output_root:
            parser.error("prepare requires --output-root")
        parameters = json.loads(_d_path(arguments.run_parameters).read_text(encoding="utf-8")) if arguments.run_parameters else None
        result = prepare_fixture(binding, arguments.output_root, run_parameters=parameters, **common)
    # Privacy-minimal summary; full relative inventory lives only in local output.
    keys = ("schema", "status", "binding_id", "source_kind", "source_adapter", "image_count", "copy_bytes", "estimated_output_bytes",
            "free_D_bytes", "blocked_reasons", "split_counts", "training_seeds", "initialization",
            "split_seed", "split_algorithm", "scale", "fixture_identity", "source_unchanged", "evaluation_provenance", "acceptance")
    print(json.dumps({key: result[key] for key in keys if key in result}, ensure_ascii=True, allow_nan=False, indent=2))
    return 0 if result["status"] != "blocked" else 2


if __name__ == "__main__":
    raise SystemExit(main())
