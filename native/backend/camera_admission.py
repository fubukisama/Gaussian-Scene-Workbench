"""Read-only fixed-camera admission shared by compute, probes and import.

This seam never discovers, invokes or repairs COLMAP. Known calibration is an
explicit policy, not a heuristic inferred from the presence of sparse points.
"""

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import uuid

from native.backend.camera_dataset import CameraDatasetError, MANIFEST_NAME, load_camera_dataset
from native.backend.camera_dataset import write_camera_dataset


def camera_dataset_summary(dataset, metadata_path=MANIFEST_NAME):
    """Presentation metadata only, not a model-quality/metric-accuracy proof."""
    return {"version": 1, "schema": dataset["schema"], "blocked": False,
            "dataIdentity": dataset["data_identity"], "dataVersion": dataset.get("data_version", dataset["data_identity"]),
            "adapter": dataset["source"]["adapter"], "sourceKind": dataset["source"].get("kind", "manual"),
            "posePolicy": dataset["pose_policy"], "scale": copy.deepcopy(dataset["scale"]),
            "counts": {key: len(value) for key, value in dataset["splits"].items()},
            "splits": copy.deepcopy(dataset["splits"]), "initialization": copy.deepcopy(dataset["initialization"]),
            "transforms": copy.deepcopy(dataset["transforms"]), "metadataPath": str(metadata_path),
            "independentTest": dataset["independent_test"],
            "warnings": [item["code"] for item in dataset["warnings"]], "errors": []}


def admit_locked_dataset(source, expected_identity=None, run_colmap=False):
    """Validate a canonical managed root/JSON with byte identity checks.

    Returns {dataset, root, manifestPath, cameraDataset}. Paths are absolute
    strings outside the hashed dataset. Raises CameraDatasetError and never
    silently falls back to a solver, raw photos, sparse or cached SfM data.
    """
    if run_colmap:
        raise CameraDatasetError("camera_locked_colmap_conflict")
    path = Path(source)
    if not path.is_absolute():
        raise CameraDatasetError("camera_absolute_manifest_required", str(path))
    path = path.resolve()
    manifest = path / MANIFEST_NAME if path.is_dir() else path
    dataset = load_camera_dataset(manifest, adapter="generic_json")
    if dataset["pose_policy"] != "locked":
        raise CameraDatasetError("camera_pose_policy_mismatch", dataset["pose_policy"])
    if expected_identity is not None and expected_identity != dataset["data_identity"]:
        raise CameraDatasetError("camera_data_identity_mismatch")
    return {"dataset": dataset, "root": str(manifest.parent), "manifestPath": str(manifest),
            "cameraDataset": camera_dataset_summary(dataset, manifest)}


def _is_reparse(path):
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(metadata.st_mode) or bool(getattr(metadata, "st_file_attributes", 0)
                                               & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def prepare_solve_dataset_version(source, destination, expected_identity=None, cancelled=None):
    """Create a new train-only raw-photo input for an explicit fresh solve.

    No known pose/metric claim is carried into freely solved SfM coordinates.
    Holdout image bytes are never copied. Original calibration, split and scale
    remain traceable through the source identity and explicit provenance file.
    The destination must be a fresh, absolute path outside the source dataset.
    """
    source = Path(source)
    destination = Path(destination)
    if not source.is_absolute() or not destination.is_absolute():
        raise CameraDatasetError("camera_absolute_manifest_required")
    if cancelled and cancelled():
        raise CameraDatasetError("camera_import_cancelled")
    manifest = source / MANIFEST_NAME if source.is_dir() else source
    manifest = manifest.resolve()
    root = manifest.parent
    dataset = load_camera_dataset(manifest, adapter="generic_json", cancelled=cancelled)
    if expected_identity is not None and dataset["data_identity"] != expected_identity:
        raise CameraDatasetError("camera_data_identity_mismatch")
    for ancestor in (destination, *destination.parents):
        if _is_reparse(ancestor):
            raise CameraDatasetError("camera_unsafe_solve_destination")
    destination = destination.resolve()
    if destination.exists():
        raise CameraDatasetError("camera_solve_destination_exists", str(destination))
    try:
        destination.relative_to(root)
    except ValueError:
        pass
    else:
        raise CameraDatasetError("camera_unsafe_solve_destination", "must remain outside original dataset")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".preparing-" + uuid.uuid4().hex)
    temporary.mkdir()
    try:
        (temporary / "input").mkdir()
        records = []
        for view in dataset["views"]:
            if view["split"] != "train":
                continue
            if cancelled and cancelled():
                raise CameraDatasetError("camera_import_cancelled")
            original = root / view["image_path"]
            original.resolve().relative_to(root)
            # A stable encoded view ID is safe on Windows even when the source
            # identifier itself contains Unicode, separators or reserved words.
            name = "view-" + hashlib.sha256(view["view_id"].encode("utf-8")).hexdigest() + original.suffix.lower()
            relative = "input/" + name
            copied = temporary / relative
            digest = hashlib.sha256()
            with original.open("rb") as incoming, copied.open("xb") as outgoing:
                for block in iter(lambda: incoming.read(1024 * 1024), b""):
                    if cancelled and cancelled():
                        raise CameraDatasetError("camera_import_cancelled")
                    digest.update(block)
                    outgoing.write(block)
                outgoing.flush()
                os.fsync(outgoing.fileno())
            if digest.hexdigest() != view["sha256"] or os.path.samefile(original, copied):
                raise CameraDatasetError("camera_image_hash_mismatch", view["image_path"])
            records.append({"viewId": view["view_id"], "sourceImagePath": view["image_path"],
                            "sourceSplit": "train", "inputImagePath": relative, "sha256": view["sha256"],
                            "copyMode": "independent_bytes"})
        semantic = {"schema": "gsw.camera_solve_source/1", "sourceDataIdentity": dataset["data_identity"],
                    "posePolicy": "solve", "files": records,
                    "excludedSplits": {key: dataset["splits"][key] for key in ("val", "test")},
                    "scale": {"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None},
                    "sourceScale": copy.deepcopy(dataset["scale"]),
                    "scaleResetReason": "free_sfm_does_not_preserve_source_scale"}
        identity = hashlib.sha256(json.dumps(semantic, sort_keys=True, separators=(",", ":"),
                                            allow_nan=False).encode("utf-8")).hexdigest()
        provenance = dict(semantic, version=1, solveDataIdentity=identity, sourceManifestPath=str(manifest))
        write_camera_dataset(provenance, temporary / "camera-solve-provenance.json")
        if cancelled and cancelled():
            raise CameraDatasetError("camera_import_cancelled")
        if destination.exists() or _is_reparse(destination):
            raise CameraDatasetError("camera_solve_destination_exists", str(destination))
        os.rename(temporary, destination)
        return {"datasetPath": str(destination), "imageRoot": str(destination / "input"),
                "sourceDataIdentity": dataset["data_identity"], "solveDataIdentity": identity,
                "provenancePath": str(destination / "camera-solve-provenance.json"),
                "files": records, "posePolicy": "solve", "scale": semantic["scale"]}
    finally:
        # Only this newly-created UUID staging directory is removed. Source
        # folders and a pre-existing destination are never cleanup candidates.
        if temporary.exists() and temporary.parent == destination.parent and not _is_reparse(temporary):
            shutil.rmtree(temporary)


def validate_solve_dataset_version(dataset_path, output_root, expected_identity):
    """Admit a portable, owned solve version before complete-state resume."""
    output_root = Path(output_root).resolve()
    expected_root = output_root / '.gsw' / 'solve-dataset'
    candidate = Path(dataset_path)
    for path in (candidate, *candidate.parents):
        if _is_reparse(path):
            raise CameraDatasetError('camera_unsafe_solve_destination')
    if candidate.resolve() != expected_root.resolve() or not candidate.is_dir():
        raise CameraDatasetError('camera_solve_resume_version_missing')
    with (candidate / 'camera-solve-provenance.json').open(encoding='utf-8') as stream:
        provenance = json.load(stream)
    semantic = {key: value for key, value in provenance.items()
                if key not in ('version', 'solveDataIdentity', 'sourceManifestPath')}
    digest = hashlib.sha256(json.dumps(semantic, sort_keys=True, separators=(',', ':'),
                                      allow_nan=False).encode('utf-8')).hexdigest()
    if (provenance.get('schema') != 'gsw.camera_solve_source/1' or
            provenance.get('posePolicy') != 'solve' or
            provenance.get('sourceDataIdentity') != expected_identity or
            provenance.get('solveDataIdentity') != digest):
        raise CameraDatasetError('camera_solve_resume_source_identity_mismatch')
    files = provenance.get('files')
    if not isinstance(files, list) or not files:
        raise CameraDatasetError('camera_solve_resume_source_identity_mismatch')
    seen = set()
    for record in files:
        relative = Path(record['inputImagePath'])
        if (relative.is_absolute() or '..' in relative.parts or len(relative.parts) != 2 or
                relative.parts[0] != 'input' or record.get('sourceSplit') != 'train' or
                relative.as_posix() in seen):
            raise CameraDatasetError('camera_unsafe_solve_destination')
        seen.add(relative.as_posix())
        path = candidate / relative
        if _is_reparse(path) or not path.is_file():
            raise CameraDatasetError('camera_solve_resume_image_hash_mismatch')
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != record.get('sha256'):
            raise CameraDatasetError('camera_solve_resume_image_hash_mismatch')
    return provenance
