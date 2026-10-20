"""Pinned, arbitrary named reconstruction conditions. Export/replay never runs jobs.

References and evaluation geometry are deliberately not inputs to this recipe.
Initial point identity comes from the same resolver as actual canonical training.
"""
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re

from native.backend import camera_dataset
from native.backend.atomic_file import atomic_sibling_directory
from native.backend.initialization import resolve_initialization_points
from native.backend.training_batch_plan import expand_training_batch

REPRODUCTION_SOURCE_DIRECTORIES = ("gaussian-splatting/scene", "gaussian-splatting/gaussian_renderer",
    "gaussian-splatting/arguments", "gaussian-splatting/utils",
    "gaussian-splatting/submodules/diff-gaussian-rasterization")
REPRODUCTION_SOURCE_FILES = ("native/backend/initialization.py", "native/backend/gs_geometry.py",
    "native/worker/gs_geometry.py", "native/backend/reconstruction_comparison.py",
    "native/worker/reconstruction_comparison.py", "native/worker/camera_rasterizer_runtime.py",
    "native/worker/gs_geometry_runtime.py")


class ComparisonError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _poll(cancelled):
    if cancelled and cancelled():
        raise ComparisonError("comparison_cancelled")


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
        allow_nan=False, separators=(",", ":")).encode("utf-8")).hexdigest()


def _extraction(value):
    allowed = {"method", "coordinate_mode", "alpha_threshold", "pixel_stride",
               "max_points", "memory_budget_mb", "depth_range"}
    if (not isinstance(value, dict) or set(value) - allowed or
            value.get("method") != "gs_depth" or value.get("coordinate_mode") not in
            ("canonical", "source", "training")):
        raise ComparisonError("comparison_unsupported_extraction_recipe")
    alpha = value.get("alpha_threshold")
    if (isinstance(alpha, bool) or not isinstance(alpha, (int, float)) or
            not math.isfinite(alpha) or not 0 <= alpha <= 1):
        raise ComparisonError("comparison_invalid_extraction_parameter", "alpha_threshold")
    for key, low, high in (("pixel_stride", 1, 9007199254740991), ("max_points", 1, 9007199254740991),
                           ("memory_budget_mb", 1, 9007199254740991)):
        if type(value.get(key)) is not int or not low <= value[key] <= high:
            raise ComparisonError("comparison_invalid_extraction_parameter", key)
    depth = value.get("depth_range")
    if depth is not None:
        if (not isinstance(depth, dict) or set(depth) != {"min", "max", "frame"} or
                depth["frame"] not in ("canonical", "source", "training") or
                any(isinstance(depth[k], bool) or not isinstance(depth[k], (float, int)) or
                    not math.isfinite(depth[k]) for k in ("min", "max")) or
                not 0 < depth["min"] < depth["max"]):
            raise ComparisonError("comparison_invalid_extraction_parameter", "depth_range")
    return copy.deepcopy(value)


def _portable(document):
    """Only declared locator fields are excluded, not calibration or parameters."""
    value = copy.deepcopy(document)
    for dataset in value["request"]["datasets"]:
        dataset.pop("path")
    for dataset in value["datasets"]:
        dataset.pop("path")
        dataset.pop("manifest_path")
    for condition in value["conditions"]:
        for key in ("repositoryRoot", "datasetPath", "cameraDatasetPath"):
            condition["training_configuration"].pop(key)
    return value


def _build(request, repository_root=None, cancelled=None):
    if (not isinstance(request, dict) or set(request) != {"schema", "datasets", "conditions"} or
            request.get("schema") != "gsw.reconstruction_comparison_request/1"):
        raise ComparisonError("comparison_invalid_request")
    request = copy.deepcopy(request)
    if len(json.dumps(request, allow_nan=False).encode("utf-8")) > 16 * 1024 * 1024:
        raise ComparisonError("comparison_resource_limit")
    datasets, conditions = request["datasets"], request["conditions"]
    if (not isinstance(datasets, list) or not 1 <= len(datasets) <= 256 or
            not isinstance(conditions, list) or not 1 <= len(conditions) <= 256):
        raise ComparisonError("comparison_resource_limit")
    by_id, dataset_records, condition_records = {}, [], []
    for record in datasets:
        _poll(cancelled)
        if (not isinstance(record, dict) or set(record) != {"id", "path", "data_identity"} or
                not isinstance(record["id"], str) or not record["id"] or record["id"] in by_id or
                not isinstance(record["path"], str) or not Path(record["path"]).is_absolute() or
                not re.fullmatch(r"[0-9a-f]{64}", str(record["data_identity"]))):
            raise ComparisonError("comparison_invalid_or_duplicate_dataset")
        by_id[record["id"]] = record
    seen, sources, cached_datasets = set(), None, {}
    for condition in conditions:
        _poll(cancelled)
        if (not isinstance(condition, dict) or set(condition) !=
                {"id", "label", "dataset_id", "training", "extraction"} or
                not isinstance(condition["id"], str) or not condition["id"] or condition["id"] in seen or
                not isinstance(condition["label"], str) or not condition["label"] or
                condition["dataset_id"] not in by_id or not isinstance(condition["training"], dict)):
            raise ComparisonError("comparison_invalid_or_duplicate_condition")
        seen.add(condition["id"])
        training = condition["training"]
        if type(training.get("trainingSeed")) is not int:
            raise ComparisonError("comparison_explicit_training_seed_required")
        source = by_id[condition["dataset_id"]]
        batch = expand_training_batch({"schema": "gsw.training_batch_plan/1", "datasets": [source],
            "base": training, "matrix": {"trainingSeed": [training["trainingSeed"]]},
            "initialization_policy": "dataset_declared", "evaluation_policy": "declared_splits"},
            repository_root, cancelled)
        sources = batch["source_fingerprints"]
        reference = batch["datasets"][0]
        if source["id"] not in cached_datasets:
            data = camera_dataset.load_camera_dataset(reference["manifest_path"], cancelled=cancelled)
            points, _, initial = resolve_initialization_points(data, reference["path"], cancelled)
            record = dict(reference, camera_views_sha256=_digest(data["views"]),
                transforms=copy.deepcopy(data["transforms"]), scale=copy.deepcopy(data["scale"]))
            dataset_records.append(record)
            cached_datasets[source["id"]] = {"point_sha256": initial["point_sha256"],
                "count": len(points), "declaration": copy.deepcopy(data["initialization"]),
                "identity_frame": "canonical_float64", "prior_declarations": data["initialization"].get("priors", [])}
        condition_records.append({"id": condition["id"], "label": condition["label"],
            "dataset_id": source["id"], "data_identity": reference["data_identity"],
            "training_configuration": batch["runs"][0]["configuration"],
            "initialization": copy.deepcopy(cached_datasets[source["id"]]),
            "splits": copy.deepcopy(reference["splits"]),
            "extraction": _extraction(condition["extraction"]),
            "geometry_policy": "train_views_only_appearance_proxy_not_material_boundary"})
    if set(by_id) != set(c["dataset_id"] for c in conditions):
        raise ComparisonError("comparison_unused_dataset")
    root = Path(repository_root or Path(__file__).resolve().parents[2]).resolve()
    required = set(REPRODUCTION_SOURCE_FILES)
    for relative in REPRODUCTION_SOURCE_DIRECTORIES:
        directory = root / relative
        if not directory.is_dir():
            raise ComparisonError("comparison_source_missing", relative)
        for path in directory.rglob("*"):
            if (path.is_file() and path.suffix in (".py", ".cu", ".cuh", ".cpp", ".h", ".hpp") and
                    not set(path.relative_to(directory).parts) & {"build", "dist", "__pycache__", ".git"}):
                required.add(path.relative_to(root).as_posix())
    required.update(("gaussian-splatting/scene/gaussian_model.py", "gaussian-splatting/gaussian_renderer/__init__.py"))
    for relative in sorted(required):
        path = root / relative
        if not path.is_file() or path.is_symlink():
            raise ComparisonError("comparison_source_missing", relative)
        sources[relative] = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    result = {"schema": "gsw.reconstruction_comparison/1", "request": request,
        "datasets": dataset_records, "conditions": condition_records,
        "source_hash_mode": "text-lf-v1", "source_fingerprints": sources,
        "execution": "none_export_or_verification_only", "reference_inputs": "excluded",
        "runtime_validation": {"state": "not_executed_by_recipe_export_or_replay",
            "external_2dgs_source_and_compiled_environment": "not_certified_by_repository_source_pins",
            "required_actual_result_evidence": "training_contract_backend_identity_with_python_torch_cuda_rasterizer_and_proof_hashes"},
        "scientific_equivalence_certified": False}
    result["comparison_sha256"] = _digest(_portable(result))
    _poll(cancelled)
    return result


def export_comparison(request, output_directory, repository_root=None, cancelled=None):
    result = _build(request, repository_root, cancelled)
    output = Path(output_directory).absolute()
    if output.exists() or not output.parent.is_dir():
        raise ComparisonError("comparison_fresh_output_directory_required")
    with atomic_sibling_directory(output) as stage:
        (stage / "profile.json").write_text(json.dumps(result, ensure_ascii=False, sort_keys=True,
            indent=2, allow_nan=False) + "\n", encoding="utf-8")
        _poll(cancelled)
        # Recheck current bytes immediately before publication; export is not a
        # promise that mutable inputs stayed unchanged while being inspected.
        if _build(request, repository_root, cancelled) != result:
            raise ComparisonError("comparison_inputs_changed_during_export")
        os.rename(str(stage), str(output))
    return {"success": True, "profilePath": str(output / "profile.json"),
            "comparison_sha256": result["comparison_sha256"], "conditionCount": len(result["conditions"])}


def verify_comparison(profile_path, dataset_paths=None, repository_root=None, cancelled=None):
    path = Path(profile_path)
    if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
        raise ComparisonError("comparison_profile_missing_or_too_large")
    saved = camera_dataset._read_json(path)
    request = copy.deepcopy(saved.get("request"))
    mapping = dataset_paths or {}
    if (not isinstance(request, dict) or not isinstance(mapping, dict) or
            set(mapping) - {d["id"] for d in request.get("datasets", [])}):
        raise ComparisonError("comparison_invalid_relocation")
    for dataset in request["datasets"]:
        if dataset["id"] in mapping:
            dataset["path"] = mapping[dataset["id"]]
    fresh = _build(request, repository_root, cancelled)
    # Stored digest alone is not authority for arbitrary configurations. Replay
    # all effective inputs and only then compare both complete portable records.
    if _portable(saved) != _portable(fresh):
        raise ComparisonError("comparison_profile_or_inputs_changed")
    return {"success": True, "status": "inputs_and_configurations_verified_no_jobs_executed",
        "comparison_sha256": fresh["comparison_sha256"], "conditions": fresh["conditions"]}
