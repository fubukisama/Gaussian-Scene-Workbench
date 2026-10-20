"""Cancellable CPU initialization inspection and fresh immutable versions."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import sys

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.backend import camera_dataset
from native.backend.atomic_file import atomic_sibling, atomic_sibling_directory
from native.backend.camera_admission import admit_locked_dataset, camera_dataset_summary
from native.backend.initialization import (InitializationError, inspect_initialization,
    resolve_initialization_points, initialization_version_id, suggest_initialization)
from native.worker.camera_import import _InputCancellation, _reparse, _copy_verified, _reference, _output


def _cancelled(cancelled):
    if cancelled and cancelled():
        raise InitializationError("camera_import_cancelled")


def _fresh_path(path, source_root):
    path = Path(path)
    if not path.is_absolute() or path.exists():
        raise InitializationError("initialization_fresh_absolute_output_required", path)
    for ancestor in (path, *path.parents):
        if _reparse(ancestor):
            raise InitializationError("initialization_unsafe_output", path)
    path = path.resolve()
    try:
        path.relative_to(source_root.resolve())
    except ValueError:
        pass
    else:
        raise InitializationError("initialization_unsafe_output", "output must remain outside the source dataset")
    if not path.parent.is_dir():
        raise InitializationError("initialization_output_parent_missing", path.parent)
    return path


def _publish_json(data, path, cancelled=None):
    encoded = json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False, allow_nan=False).encode("utf-8")
    if len(encoded) > 16 * 1024 * 1024:
        raise InitializationError("initialization_resource_limit", "report exceeds 16 MiB")
    with atomic_sibling(path, mode="wb") as (temporary, stream):
        stream.write(encoded)
        stream.flush()
        os.fsync(stream.fileno())
        stream.close()
        _cancelled(cancelled)
        os.link(str(temporary), str(path))


def _check_final_paths(destination, relatives):
    if os.name != "nt":
        return
    # A shorter sibling stage can validate and then rename into an unreadable
    # legacy Win32 final path. Never publish that apparent success. Do not
    # silently rename the user's destination or depend on system-wide policy.
    for relative in relatives:
        path = destination.joinpath(*relative.split("/"))
        if len(str(path).encode("utf-16-le")) // 2 >= 260:
            raise InitializationError("initialization_unsafe_output", "final artifact path reaches Win32 MAX_PATH: " + relative)


def create_initialized_dataset_version(dataset, request, output_directory, expected_identity=None, cancelled=None):
    """Publish a new random/declared-prior initializer without editing its source."""
    from native.worker.scale_constraints import _copy_records
    _cancelled(cancelled)
    admission = admit_locked_dataset(dataset, expected_identity)
    original, root = admission["dataset"], Path(admission["root"])
    destination = _fresh_path(output_directory, root)
    report = inspect_initialization(original, root, request, cancelled)
    if report["status"] != "ready":
        raise InitializationError("initialization_not_ready", report["status"])
    declaration = copy.deepcopy(report["initialization"])
    points, colors, resolved = resolve_initialization_points(dict(original, initialization=declaration), root, cancelled)
    if resolved["point_sha256"] != report["point_sha256"]:
        raise InitializationError("initialization_materialization_hash_mismatch")
    records = _copy_records(root, original)
    if declaration["kind"] != "random":
        records[declaration["path"]] = declaration["sha256"]
    archive_path = "source/initialization/inputs/" + original["data_identity"] + ".json"
    artifact_path = "source/initialization/points/" + report["point_sha256"] + ".npz"
    saved_report_path = "source/initialization/reports/" + report["request_sha256"] + ".json"
    _check_final_paths(destination, list(records) + [archive_path, artifact_path, saved_report_path,
                                                   camera_dataset.MANIFEST_NAME, "camera_dataset_info.json"])
    with atomic_sibling_directory(destination) as stage:
        for relative, digest in records.items():
            _cancelled(cancelled)
            _copy_verified(_reference(root, relative), _output(stage, relative), digest,
                           lambda: bool(cancelled and cancelled()))
        archive_file = _output(stage, archive_path)
        _publish_json(original, archive_file, cancelled)
        artifact_file = _output(stage, artifact_path)
        if not artifact_file.exists():
            with atomic_sibling(artifact_file, mode="wb") as (temporary, stream):
                np.savez(stream, points=np.asarray(points, dtype="<f8"), colors=np.asarray(colors, dtype="<f8"))
                stream.flush()
                os.fsync(stream.fileno())
                stream.close()
                _cancelled(cancelled)
                os.link(str(temporary), str(artifact_file))
        material = {"schema": "gsw.initialization_materialization/1", "path": artifact_path,
            "sha256": camera_dataset._hash(artifact_file), "point_sha256": report["point_sha256"],
            "frame": "canonical", "frame_data_identity": original["data_identity"], "count": len(points),
            "recipe": copy.deepcopy(declaration), "bounds_corners": report["preview"]["bounds_corners"]}
        # Content-addressed artifacts inherited from an earlier version remain
        # byte-identical. Admission below replays their actual array identity.
        version_id = initialization_version_id(original["data_identity"], report["request_sha256"],
                                              report["point_sha256"], material["sha256"])
        report["materialization"], report["created_data_version"] = material, version_id
        saved_report_file = _output(stage, saved_report_path)
        _publish_json(report, saved_report_file, cancelled)
        candidate = copy.deepcopy(original)
        candidate.pop("data_identity", None)
        candidate["initialization"] = dict(declaration, materialization=material)
        candidate["initialization_derivation"] = {"schema": "gsw.initialization_derivation/1",
            "input_manifest": {"path": archive_path, "sha256": camera_dataset._hash(archive_file),
                               "data_identity": original["data_identity"]},
            "request": copy.deepcopy(request), "request_sha256": report["request_sha256"],
            "report_manifest": {"path": saved_report_path, "sha256": camera_dataset._hash(saved_report_file)},
            "archives": camera_dataset.scale_derivation_archive_records(original)}
        candidate["data_version"] = version_id
        verified = camera_dataset.validate_camera_dataset(candidate, stage, cancelled=cancelled)
        camera_dataset.write_camera_dataset(verified, stage / camera_dataset.MANIFEST_NAME)
        camera_dataset.write_camera_dataset(camera_dataset_summary(verified), stage / "camera_dataset_info.json")
        report["created_data_identity"], report["created_data_version"] = verified["data_identity"], verified["data_version"]
        admit_locked_dataset(dataset, original["data_identity"])
        _cancelled(cancelled)
        _fresh_path(destination, root)
        stage_identity = stage.stat()
        os.rename(str(stage), str(destination))
        try:
            # Re-open the real final path, not merely the shorter stage.
            # This is still an admission check, never a training operation.
            admit_locked_dataset(destination, verified["data_identity"])
        except Exception:
            # Roll back only our same directory object. A foreign replacement
            # is preserved; the context manager will not recursively remove it.
            if (not os.path.lexists(stage) and not _reparse(destination)
                    and os.path.samestat(destination.lstat(), stage_identity)):
                os.rename(str(destination), str(stage))
            raise
        summary = camera_dataset_summary(verified)
        # Response-only adoption bindings: do not create a self-referential
        # cached-summary hash or place transport fields in canonical data.
        summary["manifestSha256"] = camera_dataset._hash(destination / camera_dataset.MANIFEST_NAME)
        summary["infoSha256"] = camera_dataset._hash(destination / "camera_dataset_info.json")
        return {"datasetPath": str(destination), "summary": summary, "report": report}


def main(argv=None):
    # Qt consumes this as a machine JSON protocol, independent of a Windows
    # parent's console code page or PYTHONIOENCODING setting.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("inspect", "suggest", "create-version"), required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--request", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--output-directory")
    parser.add_argument("--expected-identity")
    arguments = parser.parse_args(argv)
    cancelled = _InputCancellation()
    try:
        _cancelled(cancelled)
        admission = admit_locked_dataset(arguments.dataset, arguments.expected_identity)
        root = Path(admission["root"])
        report_path = _fresh_path(arguments.report, root)
        request_path = Path(arguments.request)
        if request_path.stat().st_size > 16 * 1024 * 1024:
            raise InitializationError("initialization_resource_limit", "request exceeds 16 MiB")
        request = camera_dataset._read_json(request_path)
        if request_path.resolve() == report_path:
            raise InitializationError("initialization_unsafe_output", "cannot overwrite request")
        _cancelled(cancelled)
        output = {}
        if arguments.operation == "create-version":
            if not arguments.output_directory:
                raise InitializationError("initialization_output_required")
            output = create_initialized_dataset_version(arguments.dataset, request, arguments.output_directory,
                admission["dataset"]["data_identity"], cancelled)
            report = output.pop("report")
        elif arguments.operation == "inspect":
            report = inspect_initialization(admission["dataset"], root, request, cancelled)
        elif arguments.operation == "suggest":
            report = suggest_initialization(admission["dataset"], request, cancelled)
        else:
            raise InitializationError("initialization_operation_unavailable", arguments.operation)
        # A version which has already committed is not removed by late cancel.
        _publish_json(report, report_path, None if arguments.operation == "create-version" else cancelled)
        print(json.dumps(dict(output, success=True, reportPath=str(report_path),
            inputDataIdentity=admission["dataset"]["data_identity"]), ensure_ascii=False), flush=True)
        return 0
    except (InitializationError, camera_dataset.CameraDatasetError, OSError, ValueError) as error:
        code = getattr(error, "code", "initialization_operation_failed")
        print(json.dumps({"success": False, "code": code, "error": str(error)}, ensure_ascii=False),
              file=sys.stderr, flush=True)
        return 130 if code == "camera_import_cancelled" else 2


if __name__ == "__main__":
    raise SystemExit(main())
