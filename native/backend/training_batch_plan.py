"""Pinned, serial Gaussian batch descriptions; expansion never starts training."""
import copy
import hashlib
import itertools
import json
import math
from pathlib import Path

from native.backend import camera_dataset

_COMMON = {"iterations", "resolution", "densify_grad_threshold", "densification_interval", "densify_until_iter"}
_TWO_D = {"depth_ratio"}
_THREE_D = {"optimizer_type", "antialiasing", "exposure_compensation"}
_PERFORMANCE = {"version", "enabled", "usage_fraction", "minimum_spare_mib", "gpu_slots",
    "pinned_mib", "gpu_mib", "profile_interval", "engine", "growth_mode", "loss_mode",
    "camera_prefetch", "capacity_storage", "surfel_auxiliary"}
_SOURCE_FILES = ("native/backend/atomic_file.py", "native/backend/training_batch_plan.py", "native/backend/camera_dataset.py",
    "native/worker/canonical_training.py", "native/backend/compute.py", "native/worker/gsw_worker.py",
    "native/worker/training_runtime.py", "native/worker/training_checkpoint.py",
    "native/worker/two_dgs_train.py", "gaussian-splatting/train.py",
    "native/worker/training_seed.py", "native/worker/training_batch.py",
    "native/backend/camera_admission.py", "native/backend/scale_constraints.py", "native/backend/initialization.py")


class TrainingBatchError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, detail
        super().__init__(code + (": " + str(detail) if detail else ""))


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _validate_options(backend, options):
    if not isinstance(options, dict) or set(options) - (_COMMON | _TWO_D | _THREE_D):
        raise TrainingBatchError("batch_unsupported_parameter", "trainOptions")
    forbidden = _THREE_D if backend == "2dgs" else _TWO_D
    if set(options) & forbidden:
        raise TrainingBatchError("batch_parameter_backend_mismatch", ",".join(sorted(set(options) & forbidden)))
    if (type(options.get("iterations")) is not int or not 1000 <= options["iterations"] <= 200000 or
            type(options.get("resolution")) is not int or options["resolution"] not in (1, 2, 4, 8)):
        raise TrainingBatchError("batch_explicit_training_limits_required")
    ranges = {"densify_grad_threshold": (1e-5, .01, False),
        "densification_interval": (10, 1000, True),
        "densify_until_iter": (0, options["iterations"], True), "depth_ratio": (0., 1., False)}
    for key, (low, high, integral) in ranges.items():
        if key not in options:
            continue
        value = options[key]
        if ((integral and type(value) is not int) or isinstance(value, bool) or
                not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high):
            raise TrainingBatchError("batch_invalid_parameter_value", key)
    if "optimizer_type" in options and options["optimizer_type"] not in ("default", "sparse_adam"):
        raise TrainingBatchError("batch_invalid_parameter_value", "optimizer_type")
    for key in ("antialiasing", "exposure_compensation"):
        if key in options and type(options[key]) is not bool:
            raise TrainingBatchError("batch_invalid_parameter_value", key)


def _poll(cancelled):
    if cancelled and cancelled():
        raise TrainingBatchError("batch_cancelled")


def _source_fingerprints(root, cancelled):
    records = {}
    for relative in _SOURCE_FILES:
        _poll(cancelled)
        path = Path(root) / relative
        if not path.is_file():
            raise TrainingBatchError("batch_source_missing", relative)
        # Match complete-checkpoint source identity: Git archive LF and Windows
        # checkout CRLF are the same declared text-lf-v1 source. All other byte
        # changes remain identity changes; no whitespace/code is ignored.
        data = path.read_bytes()
        _poll(cancelled)
        records[relative] = hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()
    return records


def expand_training_batch(request, repository_root=None, cancelled=None):
    """Inspect inputs and return an immutable serial plan, without side effects."""
    if not isinstance(request, dict) or request.get("schema") != "gsw.training_batch_plan/1":
        raise TrainingBatchError("batch_invalid_schema")
    _poll(cancelled)
    if set(request) - {"schema", "datasets", "base", "matrix", "initialization_policy",
            "evaluation_policy", "run_policy"}:
        raise TrainingBatchError("batch_unsupported_parameter")
    try:
        if len(json.dumps(request, allow_nan=False).encode("utf-8")) > 16 * 1024 * 1024:
            raise TrainingBatchError("batch_resource_limit", "request exceeds 16 MiB")
    except (TypeError, ValueError) as error:
        if isinstance(error, TrainingBatchError):
            raise
        raise TrainingBatchError("batch_invalid_parameter_value", str(error))
    request = copy.deepcopy(request)
    if (request.get("initialization_policy") != "dataset_declared" or
            request.get("evaluation_policy") != "declared_splits"):
        raise TrainingBatchError("batch_unsupported_data_policy")
    policy = request.get("run_policy", {"on_failure": "stop"})
    if (not isinstance(policy, dict) or set(policy) != {"on_failure"} or
            policy.get("on_failure") not in ("stop", "continue")):
        raise TrainingBatchError("batch_invalid_run_policy")
    matrix = request.get("matrix", {})
    base = request.get("base", {})
    supported = {"backend", "quality", "trainingSeed", "repeat", "trainOptions.iterations", "trainOptions.resolution"}
    supported.update("trainOptions." + key for key in _COMMON | _TWO_D | _THREE_D)
    if (not isinstance(matrix, dict) or not matrix or not isinstance(base, dict) or
            set(matrix) - supported or set(base) - {"backend", "quality", "trainingSeed", "trainOptions", "performanceOptions"}):
        raise TrainingBatchError("batch_unsupported_parameter")
    if not isinstance(base.get("trainOptions", {}), dict):
        raise TrainingBatchError("batch_unsupported_parameter", "trainOptions")
    performance = base.get("performanceOptions", {})
    if not isinstance(performance, dict) or set(performance) - _PERFORMANCE:
        raise TrainingBatchError("batch_invalid_performance_policy")
    try:
        from native.worker.training_runtime import performance_configuration
        performance = performance_configuration({"performance": performance})
    except (TypeError, ValueError) as error:
        raise TrainingBatchError("batch_invalid_performance_policy", str(error))
    for key, values in matrix.items():
        if not isinstance(values, list) or not values:
            raise TrainingBatchError("batch_invalid_axis", key)
        try:
            if len({_digest(value) for value in values}) != len(values):
                raise TrainingBatchError("batch_duplicate_axis_value", key)
        except (TypeError, ValueError) as error:
            if isinstance(error, TrainingBatchError):
                raise
            raise TrainingBatchError("batch_invalid_axis", key)
    datasets = request.get("datasets")
    if not isinstance(datasets, list) or not datasets:
        raise TrainingBatchError("batch_no_datasets")
    run_count = len(datasets)
    if run_count > 256:
        raise TrainingBatchError("batch_resource_limit", "maximum 256 explicit runs")
    for values in matrix.values():
        run_count *= len(values)
        if run_count > 256:
            raise TrainingBatchError("batch_resource_limit", "maximum 256 explicit runs")
    references = []
    identifiers = set()
    for record in datasets:
        _poll(cancelled)
        if not isinstance(record, dict) or not isinstance(record.get("id"), str) or not record["id"]:
            raise TrainingBatchError("batch_invalid_dataset")
        if (set(record) - {"id", "path", "data_identity", "display_name"} or
                not isinstance(record.get("path"), str) or not record["path"] or
                not isinstance(record.get("display_name", record["id"]), str)):
            raise TrainingBatchError("batch_invalid_dataset")
        if record["id"] in identifiers:
            raise TrainingBatchError("batch_duplicate_dataset_id", record["id"])
        identifiers.add(record["id"])
        source = Path(record.get("path", "")).resolve()
        manifest = source / camera_dataset.MANIFEST_NAME if source.is_dir() else source
        if not manifest.is_file():
            raise TrainingBatchError("batch_canonical_dataset_required", str(manifest))
        try:
            if manifest.stat().st_size > 16 * 1024 * 1024:
                raise TrainingBatchError("batch_resource_limit", "dataset manifest exceeds 16 MiB")
            document = json.loads(manifest.read_text(encoding="utf-8-sig"))
            if not isinstance(document, dict) or document.get("schema") != camera_dataset.SCHEMA:
                raise TrainingBatchError("batch_canonical_dataset_required", str(manifest))
            data = camera_dataset.load_camera_dataset(manifest, cancelled=cancelled)
        except camera_dataset.CameraDatasetError as error:
            if error.code in ("camera_import_cancelled", "scale_constraints_cancelled"):
                raise TrainingBatchError("batch_cancelled")
            raise TrainingBatchError("batch_dataset_invalid", str(error))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise TrainingBatchError("batch_dataset_invalid", str(error))
        if data.get("pose_policy") != "locked":
            raise TrainingBatchError("batch_locked_dataset_required", record["id"])
        if record.get("data_identity") and record["data_identity"] != data["data_identity"]:
            raise TrainingBatchError("batch_dataset_identity_changed", record["id"])
        references.append({"id": record["id"], "path": str(manifest.parent),
            "manifest_path": str(manifest), "data_identity": data["data_identity"],
            "manifest_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
            "display_name": record.get("display_name", record["id"]),
            "initialization": copy.deepcopy(data["initialization"]), "splits": copy.deepcopy(data["splits"])})
    runs = []
    names = sorted(matrix)
    root = str(Path(repository_root).resolve()) if repository_root else str(Path(__file__).resolve().parents[2])
    fingerprints = _source_fingerprints(root, cancelled)
    for reference in references:
        for values in itertools.product(*(matrix[name] for name in names)):
            _poll(cancelled)
            parameters = dict(zip(names, values))
            backend = parameters.get("backend", base.get("backend", "3dgs"))
            quality = parameters.get("quality", base.get("quality", "quick"))
            seed = parameters.get("trainingSeed", base.get("trainingSeed", 0))
            repeat = parameters.get("repeat", 1)
            if type(repeat) is not int or not 1 <= repeat <= 256:
                raise TrainingBatchError("batch_invalid_repeat")
            options = copy.deepcopy(base.get("trainOptions", {}))
            for key, value in parameters.items():
                if key.startswith("trainOptions."):
                    options[key.split(".", 1)[1]] = value
            if (backend not in ("3dgs", "2dgs") or quality not in
                    ("quick", "full", "quality", "max_quality", "original_quality")):
                raise TrainingBatchError("batch_invalid_backend_or_quality")
            if type(seed) is not int or not 0 <= seed <= 2**32 - 1:
                raise TrainingBatchError("batch_invalid_training_seed")
            _validate_options(backend, options)
            semantic = {"dataset_id": reference["id"], "data_identity": reference["data_identity"],
                "backend": backend, "quality": quality, "trainingSeed": seed, "repeat": repeat, "trainOptions": options,
                "performanceOptions": performance,
                "initialization_policy": request["initialization_policy"],
                "evaluation_policy": request["evaluation_policy"]}
            run_id = "run-" + _digest(semantic)[:24]
            config = {"task": "training", "repositoryRoot": root,
                "datasetPath": reference["path"], "cameraDatasetPath": reference["manifest_path"],
                "cameraDatasetIdentity": reference["data_identity"], "posePolicy": "locked",
                "resolutionPolicy": "preserve" if options["resolution"] == 1 else "manual",
                "runColmap": False, "overwrite": False, "nativeCheckpoint": True,
                "backend": backend, "quality": quality, "trainingSeed": seed, "trainOptions": options}
            config["performanceOptions"] = copy.deepcopy(performance)
            runs.append({"id": run_id, "dataset_id": reference["id"], "parameters": semantic,
                "configuration": config, "initialization": reference["initialization"],
                "splits": reference["splits"]})
    identity = {"request": {key: value for key, value in request.items() if key != "datasets"},
        "datasets": [{key: record[key] for key in ("id", "data_identity", "manifest_sha256")}
                     for record in references], "runs": [run["parameters"] for run in runs],
        "source_hash_mode": "text-lf-v1", "source_fingerprints": fingerprints}
    return {"schema": "gsw.training_batch/1", "plan_sha256": _digest(identity), "request": request,
        "datasets": references, "runs": runs,
        "initialization_policy": request["initialization_policy"],
        "evaluation_policy": request["evaluation_policy"],
        "run_policy": copy.deepcopy(policy), "source_hash_mode": "text-lf-v1", "source_fingerprints": fingerprints,
        "summary": {"runCount": len(runs), "datasetCount": len(references)}}
