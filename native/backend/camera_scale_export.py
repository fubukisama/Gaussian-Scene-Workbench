"""Gaussian-aware, non-destructive export of recorded training coordinates.

P0 supports positive similarities. Directional SH rotation is deliberately
rejected until a verified SH basis conversion exists. No renderer or training
runtime is imported by this CPU module.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import uuid

import numpy as np
from plyfile import PlyData, PlyElement

from native.backend import camera_dataset


class CameraScaleExportError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _fail(code, detail=""):
    raise CameraScaleExportError(code, detail)


def _hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _check_cancel(cancelled):
    if cancelled and cancelled():
        _fail("camera_scale_export_cancelled")


def _same_file(first, second):
    if first.resolve() == second.resolve():
        return True
    return first.exists() and second.exists() and os.path.samefile(first, second)


def _similarity(value):
    # invert_sim3 validates s>0, proper R, finiteness and any supplied inverse.
    inverse = camera_dataset.invert_sim3(value)
    return camera_dataset.invert_sim3(inverse)


def _compose(first, second):
    """first(second(p)), for positive uniform similarities."""
    a, b = _similarity(first), _similarity(second)
    ra, rb = np.asarray(a["R"], dtype=np.float64), np.asarray(b["R"], dtype=np.float64)
    return {"s": a["s"] * b["s"], "R": (ra @ rb).tolist(),
            "t": (a["s"] * ra @ np.asarray(b["t"], dtype=np.float64) + np.asarray(a["t"])).tolist(),
            "input_frame": second.get("input_frame", "training"),
            "output_frame": first.get("output_frame", "source")}


def _quaternion_from_rotation(matrix):
    # wxyz Hamilton quaternion, matching the two Graphdeco model parameters.
    r = np.asarray(matrix, dtype=np.float64)
    trace = float(np.trace(r))
    if trace > 0:
        s = math.sqrt(trace + 1.) * 2.
        q = np.asarray([.25 * s, (r[2, 1] - r[1, 2]) / s,
                        (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s])
    else:
        index = int(np.diag(r).argmax())
        other, last = (index + 1) % 3, (index + 2) % 3
        s = math.sqrt(1. + r[index, index] - r[other, other] - r[last, last]) * 2.
        q = np.empty(4, dtype=np.float64)
        q[0] = (r[last, other] - r[other, last]) / s
        q[index + 1] = .25 * s
        q[other + 1] = (r[other, index] + r[index, other]) / s
        q[last + 1] = (r[last, index] + r[index, last]) / s
    return q / np.linalg.norm(q)


def _rotate_quaternions(values, rotation):
    q = _quaternion_from_rotation(rotation)
    norms = np.linalg.norm(values, axis=1, keepdims=True)
    if not np.isfinite(values).all() or np.any(norms <= 1e-20):
        _fail("camera_scale_export_invalid_quaternion")
    p = values / norms
    w = q[0] * p[:, :1] - p[:, 1:] @ q[1:, None]
    xyz = q[0] * p[:, 1:] + p[:, :1] * q[1:] + np.cross(q[1:], p[:, 1:])
    return np.concatenate((w, xyz), axis=1)


def _covariance_fields(names):
    found = []
    for prefix in ("cov_", "covariance_"):
        for suffixes in ([str(index) for index in range(6)], ["xx", "xy", "xz", "yy", "yz", "zz"]):
            fields = [prefix + suffix for suffix in suffixes]
            present = [field in names for field in fields]
            if any(present) and not all(present):
                _fail("camera_scale_export_incomplete_covariance", prefix)
            if all(present):
                found.append(fields)
    if len(found) > 1:
        _fail("camera_scale_export_ambiguous_covariance")
    return found[0] if found else None


def _output_transform(contract, mode):
    identity = {"s": 1., "R": np.eye(3).tolist(), "t": [0., 0., 0.]}
    if contract.get("schema") == "gsw.coordinate_export/1":
        if mode != "preserve":
            _fail("camera_scale_export_already_restored")
        frame, unit = contract.get("output_frame"), contract.get("output_unit")
        if frame not in ("source", "canonical") or not isinstance(unit, str):
            _fail("camera_scale_export_invalid_metadata")
        return dict(identity, input_frame=frame, output_frame=frame), unit, frame
    if contract.get("schema") != "gsw.training_camera_contract/1":
        _fail("camera_scale_export_invalid_metadata")
    scale = contract.get("scale", {})
    chain = contract.get("transforms", {})
    if chain.get("canonical_to_training", {}).get("applied") is not True or chain.get("source_to_canonical", {}).get("applied") is not True:
        _fail("camera_scale_export_unapplied_transform")
    source_transform = _similarity(chain["source_to_canonical"])
    meters = scale.get("meters_per_unit")
    status = scale.get("status")
    if status not in ("arbitrary", "source_declared", "constraint_derived", "independently_checked"):
        _fail("camera_scale_export_invalid_scale_status")
    if meters is not None and (isinstance(meters, bool) or not isinstance(meters, (int, float))
            or not math.isfinite(meters) or meters <= 0 or status == "arbitrary"
            or not scale.get("evidence") or not math.isclose(source_transform["s"], meters, rel_tol=1e-12)):
        _fail("camera_scale_export_conflicting_scale_provenance")
    if mode in ("training", "preserve"):
        return dict(identity, input_frame="training", output_frame="training"), "training", "training"
    training_to_canonical = camera_dataset.invert_sim3(chain["canonical_to_training"])
    if mode == "metric":
        if (scale.get("status") == "arbitrary" or meters is None or not isinstance(meters, (int, float))
                or not math.isfinite(meters) or meters <= 0 or scale.get("canonical_unit") != "m"):
            _fail("camera_scale_export_unknown_metric_scale")
        # Canonical coordinates are already metres. The source-unit conversion
        # was applied before numerical normalization; applying it again is wrong.
        return training_to_canonical, "m", "canonical"
    if mode != "source":
        _fail("camera_scale_export_invalid_coordinate_mode", mode)
    canonical_to_source = camera_dataset.invert_sim3(chain["source_to_canonical"])
    return _compose(canonical_to_source, training_to_canonical), scale.get("source_unit", "unknown"), "source"


def export_camera_scale_ply(input_ply, output_ply, mode="source", sidecar=None, cancelled=None,
                            deleted_indices=None, progress=None, deleted_vertex_count=None):
    """Write a new Gaussian PLY and traceable sidecar, retaining the input.

    XYZ is promoted to float64 so restoration of large source origins does not
    lose centimetre-scale geometry to a float32 PLY field. Other attributes and
    elements are retained, except explicitly transformed Gaussian quantities.
    """
    source, output = Path(input_ply), Path(output_ply)
    metadata = Path(sidecar) if sidecar else source.with_suffix(".camera-scale.json")
    output_metadata = output.with_suffix(".camera-scale.json")
    if _same_file(source, output) or _same_file(metadata, output_metadata):
        _fail("camera_scale_export_cannot_overwrite_source")
    if output.exists() or output_metadata.exists():
        _fail("camera_scale_export_requires_new_output")
    if not output.parent.is_dir() or output.parent.is_symlink() or output.is_symlink():
        _fail("camera_scale_export_invalid_output_directory")
    _check_cancel(cancelled)
    source_hash, metadata_hash = _hash(source), _hash(metadata)
    if metadata.stat().st_size > 128 * 1024 * 1024:
        _fail("camera_scale_export_metadata_too_large")
    with metadata.open(encoding="utf-8") as stream:
        contract = json.load(stream)
    if contract.get("ply_sha256") != source_hash:
        _fail("camera_scale_export_model_hash_mismatch")
    transform, unit, frame = _output_transform(contract, mode)
    s, rotation, translation = transform["s"], np.asarray(transform["R"]), np.asarray(transform["t"])
    rotated = not np.allclose(rotation, np.eye(3), rtol=0., atol=1e-12)
    ply = PlyData.read(source)
    if "vertex" not in ply or not len(ply["vertex"].data):
        _fail("camera_scale_export_empty_gaussian")
    original = ply["vertex"].data
    if deleted_vertex_count is not None and (type(deleted_vertex_count) is not int or deleted_vertex_count != len(original)):
        _fail("camera_scale_export_edit_state_mismatch")
    names = original.dtype.names
    scale_fields = sorted((name for name in names if name.startswith("scale_") and name[6:].isdigit()),
                          key=lambda name: int(name[6:]))
    if scale_fields not in (["scale_0", "scale_1"], ["scale_0", "scale_1", "scale_2"]) or not all(name in names for name in ("x", "y", "z", "opacity")):
        _fail("camera_scale_export_requires_supported_gaussian_ply")
    if any(original.dtype.fields[name][0].kind != "f" for name in scale_fields):
        _fail("camera_scale_export_requires_float_log_scales")
    if rotated and any(name.startswith("f_rest_") for name in names):
        _fail("camera_scale_export_rotation_SH_unsupported")
    rotations = ["rot_{}".format(index) for index in range(4)]
    if rotated and not all(name in names for name in rotations):
        _fail("camera_scale_export_missing_gaussian_rotation")
    covariance = _covariance_fields(names)
    dtype = [(name, "f8" if name in ("x", "y", "z") else original.dtype.fields[name][0]) for name in names]
    retained = np.ones(len(original), dtype=bool)
    if deleted_indices is not None:
        indices = np.asarray(list(deleted_indices))
        if indices.size and (indices.dtype.kind not in "iu" or int(indices.min()) < 0 or int(indices.max()) >= len(original)):
            _fail("camera_scale_export_invalid_deleted_vertices")
        retained[indices.astype(np.int64)] = False
    if not retained.any():
        _fail("camera_scale_export_empty_gaussian")
    vertices = np.empty(int(retained.sum()), dtype=dtype)
    for name in names:
        vertices[name] = original[name][retained]
    count = len(vertices)
    for start in range(0, count, 65536):
        _check_cancel(cancelled)
        if progress:
            progress(int(start * 85 / count))
        block = vertices[start:start + 65536]
        xyz = np.stack([block[name] for name in ("x", "y", "z")], axis=1)
        if not np.isfinite(xyz).all() or any(not np.isfinite(block[name]).all() for name in scale_fields):
            _fail("camera_scale_export_nonfinite_gaussian")
        if s != 1. or rotated or bool(np.any(translation != 0.)):
            xyz = s * (xyz @ rotation.T) + translation
        if not np.isfinite(xyz).all():
            _fail("camera_scale_export_nonfinite_output")
        for index, name in enumerate(("x", "y", "z")):
            block[name] = xyz[:, index]
        if s != 1.:
            for name in scale_fields:
                block[name] += math.log(s)
        if rotated:
            q = _rotate_quaternions(np.stack([block[name] for name in rotations], axis=1), rotation)
            for index, name in enumerate(rotations):
                block[name] = q[:, index]
            if all(name in names for name in ("nx", "ny", "nz")):
                normal = np.stack([block[name] for name in ("nx", "ny", "nz")], axis=1) @ rotation.T
                for index, name in enumerate(("nx", "ny", "nz")):
                    block[name] = normal[:, index]
        if covariance:
            values = np.stack([block[name] for name in covariance], axis=1)
            if not np.isfinite(values).all():
                _fail("camera_scale_export_invalid_covariance")
            matrix = np.zeros((len(block), 3, 3), dtype=np.float64)
            matrix[:, 0, 0], matrix[:, 0, 1], matrix[:, 0, 2] = values[:, 0], values[:, 1], values[:, 2]
            matrix[:, 1, 0], matrix[:, 1, 1], matrix[:, 1, 2] = values[:, 1], values[:, 3], values[:, 4]
            matrix[:, 2, 0], matrix[:, 2, 1], matrix[:, 2, 2] = values[:, 2], values[:, 4], values[:, 5]
            changed = s * s * (rotation @ matrix @ rotation.T)
            if not np.isfinite(changed).all():
                _fail("camera_scale_export_invalid_covariance")
            for name, index in zip(covariance, ((0, 0), (0, 1), (0, 2), (1, 1), (1, 2), (2, 2))):
                block[name] = changed[:, index[0], index[1]]
    temporary = output.with_name("." + output.name + "." + uuid.uuid4().hex + ".tmp")
    temporary_metadata = output_metadata.with_name("." + output_metadata.name + "." + uuid.uuid4().hex + ".tmp")
    published = []
    try:
        comments = [value for value in ply.comments if not value.lower().startswith(("units ", "unit ", "coordinate_unit "))]
        if unit in ("m", "cm", "mm"):
            comments.append("units " + unit)
        scale_status = contract.get("scale", {}).get("status", contract.get("scale_status", "arbitrary"))
        evidence = contract.get("scale", {}).get("evidence", contract.get("scale_evidence", []))
        comments += ["gsw_coordinate_frame " + frame, "gsw_scale_status " + scale_status]
        elements = [PlyElement.describe(vertices, "vertex") if element.name == "vertex" else element for element in ply.elements]
        PlyData(elements, text=False, byte_order="<", comments=comments, obj_info=ply.obj_info).write(temporary)
        _check_cancel(cancelled)
        if progress:
            progress(95)
        if _hash(source) != source_hash or _hash(metadata) != metadata_hash:
            _fail("camera_scale_export_source_changed")
        report = {"schema": "gsw.coordinate_export/1", "success": True, "coordinate_mode": mode,
                  "input_frame": transform.get("input_frame", "training"), "output_frame": frame, "output_unit": unit,
                  "scale_status": scale_status, "scale_evidence": copy.deepcopy(evidence),
                  "applied_transform": transform, "input_sha256": source_hash, "input_metadata_sha256": metadata_hash,
                  "output_sha256": _hash(temporary), "vertex_count": count, "deleted_vertices": int((~retained).sum()),
                  "gaussian_dimensions": len(scale_fields), "appearance_rotation": "DC_only" if rotated else "unchanged",
                  "covariance_fields": covariance or [],
                  "original_training_model_retained": True, "input_model_retained": True,
                  "input_data_identity": contract.get("input_data_identity"),
                  "training_contract": copy.deepcopy(contract)}
        report["ply_sha256"] = report["output_sha256"]
        sidecar_record = report
        if mode in ("training", "preserve"):
            sidecar_record = copy.deepcopy(contract)
            sidecar_record["ply_sha256"] = report["output_sha256"]
            if sidecar_record.get("schema") == "gsw.coordinate_export/1":
                sidecar_record["output_sha256"] = report["output_sha256"]
            operations = sidecar_record.setdefault("export_operations", [])
            if not isinstance(operations, list):
                _fail("camera_scale_export_invalid_metadata")
            operations.append({"operation": "source_subset" if int((~retained).sum()) else "coordinate_preserving_copy",
                               "input_ply_sha256": source_hash, "input_metadata_sha256": metadata_hash,
                               "coordinate_frame": frame, "retained_vertices": count,
                               "deleted_vertices": int((~retained).sum()), "transform_applied": False})
        with temporary_metadata.open("w", encoding="utf-8") as stream:
            json.dump(sidecar_record, stream, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        _check_cancel(cancelled)
        # Atomic exclusive publication: no existing output can be overwritten,
        # including a file created by another writer after the initial check.
        os.link(temporary, output)
        published.append((output, temporary))
        os.link(temporary_metadata, output_metadata)
        published.append((output_metadata, temporary_metadata))
        if progress:
            progress(100)
        return report
    except Exception:
        for path, owned_temporary in reversed(published):
            if path.exists() and owned_temporary.exists() and os.path.samefile(path, owned_temporary):
                path.unlink()
        raise
    finally:
        for path in (temporary, temporary_metadata):
            if path.exists():
                path.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Restore recorded native Gaussian coordinates without changing the training PLY")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--coordinates", choices=("source", "metric", "training", "preserve"), required=True)
    parser.add_argument("--metadata")
    parser.add_argument("--deleted-indices")
    parser.add_argument("--deleted-vertex-count", type=int)
    parser.add_argument("--cancel-file")
    parser.add_argument("--progress-json", action="store_true")
    args = parser.parse_args(argv)
    try:
        deleted = None
        if args.deleted_indices:
            with Path(args.deleted_indices).open(encoding="utf-8") as stream:
                deleted = json.load(stream)
        report = export_camera_scale_ply(args.input, args.output, args.coordinates, args.metadata,
            cancelled=(lambda: Path(args.cancel_file).is_file()) if args.cancel_file else None,
            deleted_indices=deleted, deleted_vertex_count=args.deleted_vertex_count,
            progress=(lambda value: print("[gsw-coordinate-export-progress] " +
                json.dumps({"progress": value}, separators=(",", ":")), flush=True)) if args.progress_json else None)
        print(json.dumps(report, ensure_ascii=False, allow_nan=False, separators=(",", ":")))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({"success": False, "code": getattr(error, "code", "camera_scale_export_failed"),
                          "detail": str(error)}, ensure_ascii=False, separators=(",", ":")))
        return 3 if getattr(error, "code", "") == "camera_scale_export_cancelled" else 2


if __name__ == "__main__":
    sys.exit(main())
