"""Owned, cancellable scale estimation and immutable dataset-version CLI.

No subprocess, SfM, pose optimization, pixel changes or training occurs here.
The pure solver and replaying dataset admission are shared with both trainers.
"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import uuid

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.backend import camera_dataset
from native.backend.camera_admission import admit_locked_dataset, camera_dataset_summary
from native.backend.scale_constraints import solve_scale_constraints, validate_scale_solution
from native.worker.camera_import import _InputCancellation, _copy_verified, _output, _reference, _reparse


MAXIMUM_REPORT_BYTES = 16 * 1024 * 1024


def _cancelled(cancelled):
    if cancelled and cancelled():
        raise camera_dataset.CameraDatasetError("camera_scale_cancelled")


def _json_bytes(value):
    data = json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2).encode("utf-8")
    if len(data) > MAXIMUM_REPORT_BYTES:
        raise camera_dataset.CameraDatasetError("camera_scale_derivation_too_large")
    return data


def _safe_new_path(path):
    path = Path(path)
    if not path.is_absolute():
        raise camera_dataset.CameraDatasetError("camera_scale_absolute_path_required")
    for item in (path, *path.parents):
        if _reparse(item):
            raise camera_dataset.CameraDatasetError("camera_scale_unsafe_destination", str(path))
    if path.exists():
        raise camera_dataset.CameraDatasetError("camera_scale_output_exists", str(path))
    return path.resolve()


def _outside_source(path, source_root):
    try:
        Path(path).resolve().relative_to(source_root)
    except ValueError:
        return
    raise camera_dataset.CameraDatasetError("camera_scale_unsafe_destination", "output must remain outside the source dataset")


def _write_exclusive_json(value, path, cancelled=None):
    path = _safe_new_path(path)
    if not path.parent.is_dir():
        raise camera_dataset.CameraDatasetError("camera_scale_output_parent_missing", str(path.parent))
    encoded = _json_bytes(value)
    # Content-addressed report names already carry a 64-character digest.
    # Repeating that name in the sibling temporary path exceeds MAX_PATH in
    # otherwise ordinary Windows workspace roots.
    temporary = path.with_name(".scale-" + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        _cancelled(cancelled)
        os.link(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()


def _admitted(source, expected_identity=None):
    return admit_locked_dataset(source, expected_identity=expected_identity)


def estimate_scale_constraints(dataset, request, expected_identity=None, cancelled=None):
    """Estimate against admitted immutable source cameras, returning a report."""
    _cancelled(cancelled)
    _json_bytes(request)
    admission = _admitted(dataset, expected_identity)
    solution = solve_scale_constraints(admission["dataset"], request, cancelled=cancelled)
    validate_scale_solution(admission["dataset"], request, solution, cancelled=cancelled)
    _json_bytes(solution)
    return solution


def _copy_records(root, dataset):
    records = {view["image_path"]: view["sha256"] for view in dataset["views"]}
    initialization = dataset["initialization"]
    if initialization["kind"] != "random":
        records[initialization["path"]] = initialization["sha256"]
    for record in camera_dataset.scale_derivation_archive_records(dataset):
        if record["path"] in records and records[record["path"]] != record["sha256"]:
            raise camera_dataset.CameraDatasetError("camera_scale_conflicting_archive")
        records[record["path"]] = record["sha256"]
    metadata = root / "source"
    if metadata.exists():
        if _reparse(metadata):
            raise camera_dataset.CameraDatasetError("camera_scale_archive_reparse")
        for directory, subdirectories, filenames in os.walk(str(metadata), followlinks=False):
            parent = Path(directory)
            for name in subdirectories + filenames:
                if _reparse(parent / name):
                    raise camera_dataset.CameraDatasetError("camera_scale_archive_reparse", name)
            for name in filenames:
                path = parent / name
                relative = path.relative_to(root).as_posix()
                digest = camera_dataset._hash(path)
                if relative in records and records[relative] != digest:
                    raise camera_dataset.CameraDatasetError("camera_scale_archive_hash_mismatch", relative)
                records[relative] = digest
    return records


def create_scaled_dataset_version(dataset, request, output_directory, expected_identity=None, cancelled=None):
    """Create one fresh version transactionally; source poses/bytes stay fixed."""
    _cancelled(cancelled)
    _json_bytes(request)
    admission = _admitted(dataset, expected_identity)
    original, root = admission["dataset"], Path(admission["root"])
    destination = _safe_new_path(output_directory)
    _outside_source(destination, root)
    if not destination.parent.is_dir():
        raise camera_dataset.CameraDatasetError("camera_scale_output_parent_missing", str(destination.parent))
    solution = solve_scale_constraints(original, request, cancelled=cancelled)
    validate_scale_solution(original, request, solution, cancelled=cancelled)
    if solution["independent_checks"]["status"] == "fail":
        raise camera_dataset.CameraDatasetError("camera_scale_independent_checks_failed")
    original_bytes = _json_bytes(original)
    archive = {"path": "source/scale_constraints/inputs/" + original["data_identity"] + ".json",
               "sha256": hashlib.sha256(original_bytes).hexdigest(), "data_identity": original["data_identity"]}
    previous = original["scale"].get("derivation", {}).get("lineage", [])
    lineage = copy.deepcopy(previous) + [{"data_identity": original["data_identity"],
        "manifest_sha256": archive["sha256"], "scale_status": original["scale"]["status"],
        "source_unit": original["scale"]["source_unit"],
        "source_to_canonical": copy.deepcopy(original["transforms"]["source_to_canonical"])}]
    if len(lineage) > 8:
        raise camera_dataset.CameraDatasetError("camera_scale_derivation_depth_exceeded")
    derivation = {"schema": "gsw.scale_derivation/1", "input_manifest": archive,
                  "request": copy.deepcopy(request), "solution": copy.deepcopy(solution),
                  "lineage": lineage, "archives": camera_dataset.scale_derivation_archive_records(original)}
    candidate = camera_dataset._scaled_dataset_from_solution(original, derivation)
    _json_bytes(candidate)
    temporary = destination.with_name("." + destination.name + ".preparing-scale-" + uuid.uuid4().hex)
    temporary.mkdir()
    try:
        for relative, digest in _copy_records(root, original).items():
            _cancelled(cancelled)
            _copy_verified(_reference(root, relative), _output(temporary, relative), digest,
                           lambda: bool(cancelled and cancelled()))
        _write_exclusive_json(original, _output(temporary, archive["path"]), cancelled)
        request_name = "source/scale_constraints/requests/" + solution["solution_sha256"] + ".json"
        report_name = "source/scale_constraints/reports/" + solution["solution_sha256"] + ".json"
        _write_exclusive_json(request, _output(temporary, request_name), cancelled)
        _write_exclusive_json(solution, _output(temporary, report_name), cancelled)
        verified = camera_dataset.validate_camera_dataset(candidate, temporary, cancelled=cancelled)
        summary = camera_dataset_summary(verified)
        camera_dataset.write_camera_dataset(verified, temporary / camera_dataset.MANIFEST_NAME)
        camera_dataset.write_camera_dataset(summary, temporary / "camera_dataset_info.json")
        # Re-read original metadata/images immediately before publication, not
        # only the bytes observed when the transaction began.
        _admitted(dataset, original["data_identity"])
        _cancelled(cancelled)
        _safe_new_path(destination)
        os.rename(str(temporary), str(destination))
        return {"datasetPath": str(destination), "summary": summary, "solution": solution}
    finally:
        if temporary.exists() and not _reparse(temporary) and temporary.parent == destination.parent:
            shutil.rmtree(str(temporary))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", required=True, choices=("inspect", "solve", "create-version"))
    source_choice = parser.add_mutually_exclusive_group(required=True)
    source_choice.add_argument("--dataset")
    source_choice.add_argument("--model")
    parser.add_argument("--request")
    parser.add_argument("--report", required=True)
    parser.add_argument("--output-directory")
    parser.add_argument("--expected-identity")
    arguments = parser.parse_args(argv)
    cancelled = _InputCancellation()
    try:
        source = Path(arguments.dataset or arguments.model)
        source_root = source.resolve() if source.is_dir() else source.resolve().parent
        report = _safe_new_path(arguments.report)
        if arguments.dataset:
            _outside_source(report, source_root)
        if not report.parent.is_dir():
            raise camera_dataset.CameraDatasetError("camera_scale_output_parent_missing", str(report.parent))
        if arguments.operation != "inspect" and not arguments.request:
            raise camera_dataset.CameraDatasetError("camera_scale_request_required")
        request = camera_dataset._read_json(arguments.request) if arguments.request else None
        if arguments.request and Path(arguments.request).resolve() == report:
            raise camera_dataset.CameraDatasetError("camera_scale_cannot_overwrite_input")
        if arguments.model:
            from native.backend import model_scale_version
            def progress(value):
                print(json.dumps({"progress": value}, ensure_ascii=False, allow_nan=False), flush=True)
            if arguments.operation == "inspect":
                envelope = model_scale_version.inspect_model_scale_input(source, arguments.expected_identity,
                                                                         cancelled, progress)
                _write_exclusive_json(envelope, report, cancelled)
                print(json.dumps({"success": True, "reportPath": str(report),
                    "inputDataIdentity": envelope["data_identity"],
                    "summary": model_scale_version.model_scale_input_summary(envelope)},
                    ensure_ascii=False, allow_nan=False), flush=True)
                return 0
            if arguments.operation == "create-version":
                if not arguments.output_directory:
                    raise camera_dataset.CameraDatasetError("camera_scale_output_directory_required")
                output = model_scale_version.create_scaled_model_version(source, request, arguments.output_directory,
                    arguments.expected_identity, cancelled, progress)
                solution = output.pop("solution")
            else:
                estimated = model_scale_version.estimate_model_scale_constraints(source, request,
                    arguments.expected_identity, cancelled, progress)
                solution = estimated["solution"]
                from native.backend.camera_admission import scale_solution_summary
                output = {"versionPath": "", "modelPath": "", "summary": dict(
                    model_scale_version.model_scale_input_summary(estimated["envelope"]),
                    scaleDerivation=scale_solution_summary(solution))}
        elif arguments.operation == "inspect":
            admission = _admitted(source, arguments.expected_identity)
            _write_exclusive_json(admission["dataset"], report, cancelled)
            print(json.dumps({"success": True, "reportPath": str(report),
                "inputDataIdentity": admission["dataset"]["data_identity"], "summary": admission["cameraDataset"]},
                ensure_ascii=False, allow_nan=False), flush=True)
            return 0
        elif arguments.operation == "create-version":
            if not arguments.output_directory:
                raise camera_dataset.CameraDatasetError("camera_scale_output_directory_required")
            output = create_scaled_dataset_version(source, request, arguments.output_directory,
                                                     arguments.expected_identity, cancelled)
            solution = output.pop("solution")
        else:
            solution = estimate_scale_constraints(source, request, arguments.expected_identity, cancelled)
            from native.backend.camera_admission import scale_solution_summary
            output = {"datasetPath": "", "summary": {"scaleDerivation": scale_solution_summary(solution)}}
        # Once a new dataset directory has committed, cancellation cannot
        # truthfully roll that public result back. Finish its immutable report
        # before acknowledging success; cooperative cancel remains effective
        # throughout solving, copying and pre-publication validation.
        _write_exclusive_json(solution, report, None if arguments.operation == "create-version" else cancelled)
        print(json.dumps(dict(output, success=True, reportPath=str(report)), ensure_ascii=False, allow_nan=False), flush=True)
        return 0
    except Exception as error:
        code = getattr(error, "code", "camera_scale_operation_failed")
        was_cancelled = "cancel" in code
        print(json.dumps({"success": False, "code": code, "detail": getattr(error, "detail", str(error)),
                          "diagnostics": getattr(error, "diagnostics", {})}, ensure_ascii=False, allow_nan=False), flush=True)
        return 130 if was_cancelled else 2


if __name__ == "__main__":
    sys.exit(main())
