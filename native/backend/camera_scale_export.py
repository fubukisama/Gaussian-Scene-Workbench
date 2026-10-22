"""Versioned, non-destructive Gaussian/frame/unit restoration on the CPU."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import re
from pathlib import Path
import sys
import uuid

import numpy as np
from plyfile import PlyData, PlyElement, PlyListProperty

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


def _real_sh_basis(directions, degree):
    """Orthonormal real Condon-Shortley SH, l*l+l+m ordering (degrees0-4).

    Independently implemented associated-Legendre recurrence; convention is
    checked against the pinned Apache-2.0 gsplat/native evaluator and the BSD
    PlenOctree upstream polynomial evaluator, not copied training code.
    See docs/RUNTIME_DECOUPLING.md for the fixed primary-source references.
    """
    directions = np.asarray(directions, dtype=np.float64)
    directions = directions / np.linalg.norm(directions, axis=1, keepdims=True)
    z, phi = directions[:, 2], np.arctan2(directions[:, 1], directions[:, 0])
    result = np.empty((len(directions), (degree + 1) ** 2), dtype=np.float64)
    for m in range(degree + 1):
        pmm = np.ones(len(directions))
        for k in range(1, m + 1):
            pmm *= -(2 * k - 1) * np.sqrt(np.maximum(0., 1. - z * z))
        previous, current = None, pmm
        for l in range(m, degree + 1):
            if l == m:
                polynomial = current
            elif l == m + 1:
                previous, current = current, (2 * m + 1) * z * current
                polynomial = current
            else:
                previous, current = current, ((2 * l - 1) * z * current - (l + m - 1) * previous) / (l - m)
                polynomial = current
            normalization = math.sqrt((2 * l + 1) / (4. * math.pi) * math.factorial(l - m) / math.factorial(l + m))
            if m == 0:
                result[:, l * l + l] = normalization * polynomial
            else:
                result[:, l * l + l - m] = math.sqrt(2.) * normalization * polynomial * np.sin(m * phi)
                result[:, l * l + l + m] = math.sqrt(2.) * normalization * polynomial * np.cos(m * phi)
    return result


def _sh_rotation(names, rotation):
    fields = sorted((name for name in names if name.startswith("f_rest_")),
                    key=lambda name: int(name[7:]) if name[7:].isdigit() else -1)
    if not fields:
        return None, [], {"degree": 0, "method": "DC_invariant", "basis": "real_condon_shortley_l_l_plus_l_plus_m"}
    if fields != ["f_rest_" + str(index) for index in range(len(fields))] or len(fields) % 3:
        _fail("camera_scale_export_unsupported_SH_layout")
    count = len(fields) // 3 + 1
    degree = math.isqrt(count) - 1
    if (degree + 1) ** 2 != count or not 1 <= degree <= 4:
        _fail("camera_scale_export_unsupported_SH_degree")
    # Real SH of one degree form an invariant finite-dimensional subspace under
    # SO(3). Overdetermined collocation derives its change of basis once, not per
    # Gaussian. Independent directions and orthogonality verify the matrix.
    index = np.arange(96, dtype=np.float64)
    z = 1. - 2. * (index + .5) / len(index)
    phi = index * math.pi * (3. - math.sqrt(5.))
    points = np.column_stack((np.sqrt(1. - z * z) * np.cos(phi), np.sqrt(1. - z * z) * np.sin(phi), z))
    target, original = _real_sh_basis(points, degree), _real_sh_basis(points @ rotation, degree)
    matrix = np.eye(count)
    for l in range(1, degree + 1):
        band = slice(l * l, (l + 1) ** 2)
        band_matrix = np.linalg.lstsq(target[:, band], original[:, band], rcond=None)[0]
        if not np.allclose(band_matrix.T @ band_matrix, np.eye(2 * l + 1), atol=2e-12, rtol=0):
            _fail("camera_scale_export_SH_rotation_unstable")
        matrix[band, band] = band_matrix
    validation = np.random.default_rng(641315).normal(size=(137, 3))
    residual = float(np.max(np.abs(_real_sh_basis(validation, degree) @ matrix - _real_sh_basis(validation @ rotation, degree))))
    if not math.isfinite(residual) or residual > 2e-12:
        _fail("camera_scale_export_SH_rotation_unstable")
    return matrix[1:, 1:], fields, {"degree": degree, "method": "band_preserving_real_SH_change_of_basis",
        "basis": "real_condon_shortley_l_l_plus_l_plus_m", "validation_max_abs": residual,
        "validation_scope": "CPU_basis_before_coefficient_storage_rounding"}


def _identity(frame=None):
    return {"s": 1., "R": np.eye(3).tolist(), "t": [0., 0., 0.],
            "input_frame": frame, "output_frame": frame}


_METERS = {"m": 1., "cm": .01, "mm": .001}
_GEOMETRY_DECLARATIONS = ("geometry_kind", "depth_definition", "fusion", "surface_semantics", "model_geometry_accuracy")


def _validated_geometry_provenance(value, root):
    keys = {"schema", "source_model_sha256", "source_metadata_sha256", "source_frame", "source_unit",
            "source_applied_transform", "depth_definition_scope"} | set(_GEOMETRY_DECLARATIONS)
    if (type(value) is not dict or set(value) != keys or value.get("schema") != "gsw.geometry_source_provenance/1" or
            value.get("depth_definition_scope") != "extraction_camera_depth_not_current_file_axes_or_units" or
            any(not isinstance(value.get(name), str) or not value[name] or len(value[name]) > 4096 or "\x00" in value[name]
                for name in _GEOMETRY_DECLARATIONS) or
            any(not isinstance(value.get(name), str) or not re.fullmatch(r"[0-9a-f]{64}", value[name])
                for name in ("source_model_sha256", "source_metadata_sha256"))):
        _fail("camera_scale_export_invalid_geometry_provenance")
    try:
        source_frame, source_unit = value["source_frame"], value["source_unit"]
        original_to_canonical = _frame_to_canonical(root, source_frame, source_unit)
        original = value["source_applied_transform"]
        if (type(original) is not dict or original.get("applied") is not True or
                original.get("input_frame") != "training" or original.get("output_frame") != source_frame):
            _fail("camera_scale_export_invalid_geometry_provenance")
        expected = _compose(camera_dataset.invert_sim3(original_to_canonical),
                            _native_frame(root, "training")[0])
        if not _similarities_close(_similarity(original), expected):
            _fail("camera_scale_export_invalid_geometry_provenance")
    except (ValueError, KeyError, TypeError, OverflowError) as error:
        _fail("camera_scale_export_invalid_geometry_provenance", error)
    return copy.deepcopy(value)


def _geometry_provenance(record, root, source_hash, metadata_hash, frame, unit):
    if "source_geometry_provenance" in record:
        return _validated_geometry_provenance(record["source_geometry_provenance"], root)
    if (record.get("schema") == "gsw.coordinate_export/1" and record.get("export_version", 1) == 1 and
            all(name in record for name in _GEOMETRY_DECLARATIONS)):
        value = {"schema": "gsw.geometry_source_provenance/1", "source_model_sha256": source_hash,
            "source_metadata_sha256": metadata_hash, "source_frame": frame, "source_unit": unit,
            "source_applied_transform": copy.deepcopy(record.get("applied_transform")),
            "depth_definition_scope": "extraction_camera_depth_not_current_file_axes_or_units"}
        value.update({name: copy.deepcopy(record[name]) for name in _GEOMETRY_DECLARATIONS})
        return _validated_geometry_provenance(value, root)
    return None


def _root_contract(record):
    """Unwrap historical export layers; never mistake one for training input."""
    root, layers = record, []
    for _ in range(32):
        if type(root) is not dict:
            _fail("camera_scale_export_invalid_metadata")
        if root.get("schema") == "gsw.training_camera_contract/1":
            break
        if root.get("schema") != "gsw.coordinate_export/1":
            _fail("camera_scale_export_invalid_metadata")
        if type(root.get("export_version", 1)) is not int or root.get("export_version", 1) not in (1, 2, 3):
            _fail("camera_scale_export_unsupported_version")
        nested = root.get("training_contract")
        if type(nested) is not dict:
            _fail("camera_scale_export_missing_training_contract")
        if (nested.get("ply_sha256") and root.get("input_sha256") and
                nested["ply_sha256"] != root["input_sha256"]):
            # v2 always stores the immutable root contract rather than a chain
            # of intermediate models; its lineage is separately recorded.
            if root.get("export_version", 1) < 2 or nested.get("schema") != "gsw.training_camera_contract/1":
                _fail("camera_scale_export_nested_hash_mismatch")
        layers.append(root)
        root = nested
    else:
        _fail("camera_scale_export_metadata_depth_limit")
    identity = root.get("input_data_identity")
    if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{64}", identity):
        _fail("camera_scale_export_missing_data_identity")
    if any(layer.get("input_data_identity", identity) != identity for layer in layers):
        _fail("camera_scale_export_data_identity_mismatch")
    chain, scale = root.get("transforms", {}), root.get("scale", {})
    for name, input_frame, output_frame in (("source_to_canonical", "source", "canonical"),
                                            ("canonical_to_training", "canonical", "training")):
        if chain.get(name, {}).get("applied") is not True:
            _fail("camera_scale_export_unapplied_transform")
        _similarity(chain[name])
        if chain[name].get("input_frame", input_frame) != input_frame or chain[name].get("output_frame", output_frame) != output_frame:
            _fail("camera_scale_export_conflicting_transform_frame")
    status, meters = scale.get("status"), scale.get("meters_per_unit")
    if status not in ("arbitrary", "source_declared", "constraint_derived", "independently_checked"):
        _fail("camera_scale_export_invalid_scale_status")
    if meters is not None and (type(meters) not in (int, float) or not math.isfinite(meters) or meters <= 0
            or status == "arbitrary" or not scale.get("evidence") or scale.get("canonical_unit") != "m"
            or not math.isclose(chain["source_to_canonical"]["s"], meters, rel_tol=1e-12)):
        _fail("camera_scale_export_conflicting_scale_provenance")
    return root


def _native_frame(root, frame):
    """Return native-frame -> canonical, native unit and physical metres/unit."""
    scale, chain = root["scale"], root["transforms"]
    known = scale.get("status") != "arbitrary" and scale.get("meters_per_unit") is not None
    if frame == "source":
        return _similarity(chain["source_to_canonical"]), scale.get("source_unit", "unknown"), scale.get("meters_per_unit") if known else None
    if frame == "canonical":
        return _identity(frame), scale.get("canonical_unit", "arbitrary"), 1. if known else None
    if frame == "training":
        transform = camera_dataset.invert_sim3(chain["canonical_to_training"])
        return transform, "training", transform["s"] if known else None
    _fail("camera_scale_export_invalid_frame", frame)


def _frame_to_canonical(root, frame, unit):
    transform, native_unit, meters = _native_frame(root, frame)
    if unit == native_unit:
        return transform
    if unit not in _METERS or meters is None:
        _fail("camera_scale_export_conflicting_frame_unit", str((frame, unit)))
    return _compose(transform, dict(_identity(frame), s=_METERS[unit] / meters))


def _context(record):
    root = _root_contract(record)
    if record.get("schema") == "gsw.training_camera_contract/1":
        frame, unit = "training", "training"
    else:
        frame, unit = record.get("output_frame"), record.get("output_unit")
        if record.get("current_frame", frame) != frame or record.get("current_unit", unit) != unit:
            _fail("camera_scale_export_conflicting_current_frame")
        if record.get("export_version", 1) >= 2 and (record.get("transform_applied") is not True or
                record.get("applied_transform", {}).get("applied") is not True):
            _fail("camera_scale_export_unapplied_output")
        if record.get("export_version", 1) >= 2:
            applied = _similarity(record["applied_transform"])
            if (record["applied_transform"].get("input_frame") != record.get("input_frame") or
                    record["applied_transform"].get("output_frame") != frame):
                _fail("camera_scale_export_conflicting_applied_frame")
            before = _frame_to_canonical(root, record.get("input_frame"), record.get("input_unit"))
            after = _frame_to_canonical(root, frame, unit)
            if record.get("export_version", 1) >= 3:
                old_scene = _validated_scene_state(record.get("input_canonical_scene_transform"))
                new_scene = _validated_scene_state(record.get("canonical_scene_transform"))
                if "scene_transform" in record:
                    edit = _scene_request(record["scene_transform"], record.get("input_frame"), record.get("input_unit"))["transform"]
                    expected_scene = _compose(before, _compose(edit,
                        _compose(camera_dataset.invert_sim3(before), old_scene)))
                else:
                    expected_scene = old_scene
                if not _similarities_close(new_scene, expected_scene):
                    _fail("camera_scale_export_invalid_scene_history")
                expected = _compose(camera_dataset.invert_sim3(after), _compose(new_scene,
                    _compose(camera_dataset.invert_sim3(old_scene), before)))
                if (type(record.get("scene_scale_multiplier")) not in (int, float) or
                        not math.isclose(record["scene_scale_multiplier"], new_scene["s"], rel_tol=1e-12) or
                        record.get("physical_scale_modified") is not (abs(new_scene["s"] - 1.) > 1e-12) or
                        record.get("scale_evidence_scope") != "immutable_training_contract" or
                        record.get("scene_edit_independent_validation") is not False or
                        record.get("full_optimizer_state_transformed") is not False):
                    _fail("camera_scale_export_invalid_scene_history")
            else:
                expected = _compose(camera_dataset.invert_sim3(after), before)
            if (not math.isclose(applied["s"], expected["s"], rel_tol=1e-12) or
                    not np.allclose(applied["R"], expected["R"], atol=1e-11, rtol=0) or
                    not np.allclose(applied["t"], expected["t"], atol=2e-7, rtol=1e-12)):
                _fail("camera_scale_export_applied_chain_mismatch")
    return root, frame, unit, _frame_to_canonical(root, frame, unit)


def _validated_scene_state(value):
    if (type(value) is not dict or value.get("input_frame") != "canonical" or
            value.get("output_frame") != "canonical" or value.get("applied") is not True):
        _fail("camera_scale_export_invalid_scene_history")
    _similarity(value)
    # A no-edit reexport retains the recorded active edit exactly. Validating
    # through an inverse must not rewrite a previously applied ledger value.
    return {"s": value["s"], "R": copy.deepcopy(value["R"]), "t": copy.deepcopy(value["t"]),
            "input_frame": "canonical", "output_frame": "canonical", "applied": True}


def _similarities_close(first, second):
    return (math.isclose(first["s"], second["s"], rel_tol=1e-12) and
            np.allclose(first["R"], second["R"], atol=1e-11, rtol=0) and
            np.allclose(first["t"], second["t"], atol=2e-7, rtol=1e-12))


def _scene_request(value, frame, unit):
    if (type(value) is not dict or set(value) != {"schema", "frame", "unit", "transform"} or
            value.get("schema") != "gsw.scene_transform/1" or value.get("frame") != frame or
            value.get("unit") != unit or type(value.get("transform")) is not dict or
            set(value["transform"]) != {"s", "R", "t"}):
        _fail("camera_scale_export_invalid_scene_transform")
    try:
        transform = _similarity(value["transform"])
    except (ValueError, TypeError, KeyError) as error:
        _fail("camera_scale_export_invalid_scene_transform", error)
    return {"schema": "gsw.scene_transform/1", "frame": frame, "unit": unit,
            "transform": {key: transform[key] for key in ("s", "R", "t")}}


def _current_scene_state(record):
    if record.get("schema") == "gsw.coordinate_export/1" and record.get("export_version", 1) >= 3:
        return _validated_scene_state(record.get("canonical_scene_transform"))
    return dict(_identity("canonical"), applied=True)


def _no_links(path):
    for part in (Path(path).absolute(),) + tuple(Path(path).absolute().parents):
        if part.is_symlink() or part.exists() and getattr(part.stat(), "st_file_attributes", 0) & 0x400:
            _fail("camera_scale_export_link_refused", str(part))


def _json_read(path):
    _no_links(path)
    if Path(path).stat().st_size > 128 * 1024 * 1024:
        _fail("camera_scale_export_metadata_too_large")
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                _fail("camera_scale_export_duplicate_metadata_key", key)
            result[key] = value
        return result
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=object_pairs,
                         parse_constant=lambda value: _fail("camera_scale_export_nonfinite_metadata", value))


def _assets_hash(assets):
    return hashlib.sha256(json.dumps(assets, sort_keys=True, ensure_ascii=False,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def _relative_asset(root, relative):
    if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative or
            "\x00" in relative or Path(relative).is_absolute() or ".." in Path(relative).parts):
        _fail("camera_scale_export_unsafe_asset_reference")
    if any(part.endswith((".", " ")) or re.fullmatch(r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", part, re.IGNORECASE)
           for part in Path(relative).parts):
        _fail("camera_scale_export_unsafe_asset_reference")
    path = root / relative
    _no_links(path)
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        _fail("camera_scale_export_unsafe_asset_reference")
    return path


def _validate_assets(source, record):
    assets = record.get("assets")
    if type(assets) is not list or record.get("assets_sha256") != _assets_hash(assets):
        _fail("camera_scale_export_invalid_assets_manifest")
    for asset in assets:
        if (type(asset) is not dict or set(asset) != {"kind", "path", "sha256", "bytes", "source_reference"} or
                asset.get("kind") != "texture_file" or type(asset.get("bytes")) is not int or asset["bytes"] < 0 or
                not isinstance(asset.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", asset["sha256"])):
            _fail("camera_scale_export_invalid_assets_manifest")
        _relative_asset(source.parent, asset["source_reference"])
        path = _relative_asset(source.parent, asset["path"])
        if not path.is_file() or path.stat().st_size != asset["bytes"] or _hash(path) != asset["sha256"]:
            _fail("camera_scale_export_asset_hash_mismatch")
    comments = []
    header_bytes = 0
    with source.open("rb") as stream:
        while True:
            line = stream.readline(1024 * 1024)
            header_bytes += len(line)
            if not line or header_bytes > 16 * 1024 * 1024:
                _fail("camera_scale_export_invalid_asset_header")
            try:
                line = line.decode("ascii").rstrip("\r\n")
            except UnicodeError:
                _fail("camera_scale_export_invalid_asset_header")
            if line == "end_header":
                break
            if line.startswith("comment "):
                comments.append(line[8:])
    references = _texture_references(comments)
    if references != [asset["path"] for asset in assets]:
        _fail("camera_scale_export_asset_reference_mismatch")
    return assets


def _texture_references(comments):
    references = []
    for comment in comments:
        match = re.fullmatch(r"TextureFile\s+(.+)", comment, re.IGNORECASE)
        if match:
            references.append(match[1])
        elif re.match(r"(?:TextureFile|Texture|MaterialFile|MaterialLibrary|mtllib|map_Kd)\b", comment, re.IGNORECASE):
            _fail("camera_scale_export_unsupported_material_reference", comment)
    return references


def _texture_inputs(source, ply, contract):
    references = _texture_references(ply.comments)
    for element in ply.elements:
        # PLY permits comments anywhere in the header. plyfile retains their
        # sequence within each element rather than all at document level.
        references.extend(_texture_references(element.comments))
    prior = {asset["path"]: asset for asset in contract.get("assets", [])}
    inputs = []
    for reference in references:
        path = _relative_asset(source.parent, reference)
        if not path.is_file():
            _fail("camera_scale_export_asset_missing", reference)
        inputs.append({"path": path, "sha256": _hash(path), "bytes": path.stat().st_size,
            "source_reference": prior.get(reference, {}).get("source_reference", reference)})
    if inputs and "face" in ply and "texture_index" in ply["face"].data.dtype.names:
        indices = ply["face"]["texture_index"]
        if indices.dtype.kind not in "iu" or np.any(indices < 0) or np.any(indices >= len(inputs)):
            _fail("camera_scale_export_invalid_material_index")
    return inputs


def _asset_directory(output):
    stem = output.stem
    reserved = re.fullmatch(r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", stem, re.IGNORECASE)
    if not re.fullmatch(r"[A-Za-z0-9._-]+", stem) or stem in (".", "..") or stem.endswith(".") or reserved:
        stem = "model-" + hashlib.sha256(stem.encode("utf-8")).hexdigest()[:16]
    return output.parent / (stem + ".assets")


def _source_assets_unchanged(inputs):
    for asset in inputs:
        _no_links(asset["path"])
        if (not asset["path"].is_file() or asset["path"].stat().st_size != asset["bytes"] or
                _hash(asset["path"]) != asset["sha256"]):
            _fail("camera_scale_export_source_changed")


def _bundle_hash(model_hash, camera_hash, identity, frame, unit, assets_hash=None):
    payload = {"model_sha256": model_hash, "camera_sha256": camera_hash,
               "input_data_identity": identity, "coordinate_frame": frame, "coordinate_unit": unit}
    if assets_hash is not None:
        payload["assets_sha256"] = assets_hash
    return hashlib.sha256(json.dumps(payload, sort_keys=True, ensure_ascii=True,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def _validate_camera_reference(source, record, model_hash):
    reference = record.get("camera_reference")
    if reference is None:
        if record.get("export_version", 1) >= 2:
            _fail("camera_scale_export_camera_reference_missing")
        return None
    if type(reference) is not dict or reference.get("schema") != "gsw.model_cameras/1":
        _fail("camera_scale_export_camera_reference_invalid")
    relative = reference.get("path")
    if not isinstance(relative, str) or Path(relative).name != relative or relative in (".", "..") or "\\" in relative:
        _fail("camera_scale_export_camera_reference_invalid")
    path = source.parent / relative
    _no_links(path)
    if _hash(path) != reference.get("sha256"):
        _fail("camera_scale_export_camera_hash_mismatch")
    cameras = _json_read(path)
    root, frame, unit, _ = _context(record)
    expected = {"schema": "gsw.model_cameras/1", "model_sha256": model_hash,
                "input_data_identity": root["input_data_identity"], "coordinate_frame": frame,
                "coordinate_unit": unit, "transform_applied": True}
    if type(cameras) is not dict or any(cameras.get(key) != value for key, value in expected.items()):
        _fail("camera_scale_export_camera_binding_mismatch")
    assets_hash = record.get("assets_sha256") if record.get("export_version", 1) >= 3 else None
    if record.get("bundle_sha256") != _bundle_hash(model_hash, reference["sha256"], root["input_data_identity"], frame, unit, assets_hash):
        _fail("camera_scale_export_bundle_hash_mismatch")
    if record.get("export_version", 1) >= 3:
        expected_cameras = _camera_bundle(root, model_hash, frame, unit, _current_scene_state(record))
        if (cameras.get("camera_count") != expected_cameras["camera_count"] or
                type(cameras.get("views")) is not list or len(cameras["views"]) != len(expected_cameras["views"]) or
                type(cameras.get("clipping")) is not dict or set(cameras["clipping"]) != set(expected_cameras["clipping"])):
            _fail("camera_scale_export_camera_binding_mismatch")
        for actual, wanted in zip(cameras["views"], expected_cameras["views"]):
            if type(actual) is not dict or any(actual.get(key) != wanted[key] for key in (
                    "view_id", "split", "image_path", "width", "height", "projection_model", "pixel_coordinates")):
                _fail("camera_scale_export_camera_binding_mismatch")
            try:
                k, pose = np.asarray(actual.get("K")), np.asarray(actual.get("c2w_opencv"))
                if (k.shape != (3, 3) or pose.shape != (4, 4) or
                        not np.isfinite(k).all() or not np.isfinite(pose).all() or
                        not np.allclose(k, wanted["K"], atol=1e-10, rtol=1e-12) or
                        not np.allclose(pose, wanted["c2w_opencv"], atol=2e-7, rtol=1e-12)):
                    _fail("camera_scale_export_camera_binding_mismatch")
            except (ValueError, TypeError):
                _fail("camera_scale_export_camera_binding_mismatch")
        if any(type(cameras["clipping"][key]) not in (int, float) or
               not math.isclose(cameras["clipping"][key], value, rel_tol=1e-12, abs_tol=2e-7)
               for key, value in expected_cameras["clipping"].items()):
            _fail("camera_scale_export_camera_binding_mismatch")
    return path


def _read_context(source, metadata):
    _no_links(source)
    if not source.is_file():
        _fail("camera_scale_export_model_missing")
    if not metadata.is_file():
        _fail("camera_scale_export_metadata_missing")
    record = _json_read(metadata)
    if type(record) is not dict:
        _fail("camera_scale_export_invalid_metadata")
    digest = _hash(source)
    if record.get("ply_sha256") != digest or record.get("output_sha256", digest) != digest:
        _fail("camera_scale_export_model_hash_mismatch")
    root, frame, unit, _ = _context(record)
    if "source_geometry_provenance" in record:
        _validated_geometry_provenance(record["source_geometry_provenance"], root)
    if record.get("export_version", 1) >= 3:
        _validate_assets(source, record)
    camera_path = _validate_camera_reference(source, record, digest)
    return record, digest, _hash(metadata), camera_path


def inspect_coordinate_metadata(input_ply, sidecar=None):
    """Validate the actual model/metadata/bundle and expose its current frame."""
    source = Path(input_ply)
    metadata = Path(sidecar) if sidecar else source.with_suffix(".camera-scale.json")
    record, digest, metadata_digest, camera_path = _read_context(source, metadata)
    root, frame, unit, transform = _context(record)
    known = root["scale"].get("status") != "arbitrary" and root["scale"].get("meters_per_unit") is not None
    return {"schema": "gsw.coordinate_metadata/2", "current_frame": frame, "current_unit": unit,
            "scale_status": root["scale"]["status"], "scale_evidence": copy.deepcopy(root["scale"].get("evidence", [])),
            "input_data_identity": root["input_data_identity"], "metric_available": known,
            "metric_reason": "" if known else "camera_scale_export_unknown_metric_scale",
            "training_contract": copy.deepcopy(root), "frame_to_canonical": dict(transform, applied=False),
            "model_sha256": digest, "metadata_sha256": metadata_digest,
            "camera_path": str(camera_path) if camera_path else None}


def _camera_bundle(root, model_hash, frame, unit, canonical_scene_transform=None):
    native_to_canonical, _, _ = _native_frame(root, "training")
    transform = _compose(camera_dataset.invert_sim3(_frame_to_canonical(root, frame, unit)),
        _compose(canonical_scene_transform or _identity("canonical"), native_to_canonical))
    rotation, translation = np.asarray(transform["R"]), np.asarray(transform["t"])
    views, seen = [], set()
    for view in root.get("views", []):
        identity = view.get("view_id")
        if not isinstance(identity, str) or not identity or identity in seen or view.get("split") not in ("train", "val", "test"):
            _fail("camera_scale_export_camera_identity_invalid")
        seen.add(identity)
        size = view.get("training_size", [view.get("width"), view.get("height")])
        if type(size) is not list or len(size) != 2 or any(type(value) is not int or value <= 0 for value in size):
            _fail("camera_scale_export_camera_size_invalid")
        k = np.asarray(view.get("training_K", view.get("K")), dtype=np.float64)
        if k.shape != (3, 3) or not np.isfinite(k).all() or k[0, 0] <= 0 or k[1, 1] <= 0 or not np.allclose(k[2], [0., 0., 1.], atol=1e-12, rtol=0):
            _fail("camera_scale_export_camera_intrinsics_invalid")
        pose = np.asarray(view.get("training_c2w_opencv", view.get("c2w_opencv")), dtype=np.float64)
        if pose.shape != (4, 4) or not np.isfinite(pose).all() or not np.allclose(pose[3], [0., 0., 0., 1.], atol=1e-12, rtol=0):
            _fail("camera_scale_export_camera_pose_invalid")
        if "training_c2w_opencv" not in view:
            conversion = _similarity(root["transforms"]["canonical_to_training"])
            r, t = np.asarray(conversion["R"]), np.asarray(conversion["t"])
            pose[:3, 3] = conversion["s"] * r @ pose[:3, 3] + t
            pose[:3, :3] = r @ pose[:3, :3]
        _similarity({"s": 1., "R": pose[:3, :3].tolist(), "t": pose[:3, 3].tolist()})
        changed = np.eye(4)
        changed[:3, :3] = rotation @ pose[:3, :3]
        changed[:3, 3] = transform["s"] * rotation @ pose[:3, 3] + translation
        projection, pixel = view.get("projection_model", "PINHOLE"), view.get("pixel_coordinates", "pixel_border")
        if projection != "PINHOLE" or pixel != "pixel_border":
            _fail("camera_scale_export_camera_projection_unsupported")
        views.append({"view_id": identity, "split": view["split"], "image_path": view.get("image_path", ""),
                      "width": size[0], "height": size[1], "K": k.tolist(), "c2w_opencv": changed.tolist(),
                      "projection_model": projection, "pixel_coordinates": pixel})
    clipping = {}
    for name in ("znear", "zfar", "backend_near"):
        value = root.get("clipping", {}).get(name)
        if value is not None:
            if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
                _fail("camera_scale_export_invalid_clipping")
            clipping[name] = value * transform["s"]
    return {"schema": "gsw.model_cameras/1", "version": 1, "model_sha256": model_hash,
            "input_data_identity": root["input_data_identity"], "coordinate_frame": frame,
            "coordinate_unit": unit, "transform_applied": True, "camera_count": len(views),
            "views": views, "clipping": clipping, "image_files_required_for_pose_display": False}


def _output_transform(record, mode, output_unit=None):
    root, current_frame, current_unit, current_to_canonical = _context(record)
    if mode not in ("source", "canonical", "metric", "training", "preserve"):
        _fail("camera_scale_export_invalid_coordinate_mode", mode)
    frame = current_frame if mode == "preserve" else "canonical" if mode == "metric" else mode
    _, native_unit, meters = _native_frame(root, frame)
    unit = output_unit or (current_unit if mode == "preserve" else "m" if mode == "metric" else native_unit)
    if output_unit is not None and output_unit not in _METERS:
        _fail("camera_scale_export_invalid_output_unit", output_unit)
    if (mode == "metric" or unit in _METERS and unit != native_unit) and meters is None:
        _fail("camera_scale_export_unknown_metric_scale")
    if frame == current_frame and unit == current_unit:
        transform = _identity(frame)
    else:
        canonical_to_target = camera_dataset.invert_sim3(_frame_to_canonical(root, frame, unit))
        transform = _compose(canonical_to_target, current_to_canonical)
    return dict(transform, input_frame=current_frame, output_frame=frame, applied=True), unit, frame


def export_camera_scale_ply(input_ply, output_ply, mode="source", sidecar=None, cancelled=None,
                            deleted_indices=None, progress=None, deleted_vertex_count=None, output_unit=None,
                            scene_transform=None):
    """Compatible strict-Gaussian entry; never relabel ordinary geometry as GS."""
    return _export_recorded_ply(input_ply, output_ply, mode, sidecar, cancelled, deleted_indices,
        progress, deleted_vertex_count, output_unit, "gaussian", scene_transform)


def export_recorded_ply(input_ply, output_ply, mode="source", sidecar=None, cancelled=None,
                        deleted_indices=None, progress=None, deleted_vertex_count=None, output_unit=None,
                        representation="auto", scene_transform=None):
    """Export typed PLY geometry with the recorded camera/frame contract."""
    return _export_recorded_ply(input_ply, output_ply, mode, sidecar, cancelled, deleted_indices,
        progress, deleted_vertex_count, output_unit, representation, scene_transform)


def _describe_like(data, element, comments=None):
    # Object dtype alone does not retain a PLY list's value/length types.
    lists = [prop for prop in element.properties if isinstance(prop, PlyListProperty)]
    return PlyElement.describe(data, element.name,
        len_types={prop.name: prop.len_dtype for prop in lists},
        val_types={prop.name: prop.val_dtype for prop in lists}, comments=element.comments if comments is None else comments)


def _mesh_faces(element, vertex_count, rotation, retained):
    names = element.data.dtype.names
    indices = [name for name in ("vertex_indices", "vertex_index") if name in names]
    if len(indices) != 1:
        _fail("camera_scale_export_invalid_mesh_indices")
    property_map = {prop.name: prop for prop in element.properties}
    prop = property_map[indices[0]]
    if not isinstance(prop, PlyListProperty) or np.dtype(prop.val_dtype).kind not in "iu":
        _fail("camera_scale_export_invalid_mesh_indices")
    faces = element.data.copy()
    for polygon in faces[indices[0]]:
        if len(polygon) < 3 or np.any(polygon < 0) or np.any(polygon >= vertex_count):
            _fail("camera_scale_export_invalid_mesh_indices")
    if not retained.all():
        keep = np.array([bool(retained[polygon].all()) for polygon in faces[indices[0]]], dtype=bool)
        faces = faces[keep].copy()
        remap = np.full(vertex_count, -1, dtype=np.int64)
        remap[retained] = np.arange(int(retained.sum()))
        for row, polygon in enumerate(faces[indices[0]]):
            faces[indices[0]][row] = remap[polygon].astype(np.dtype(prop.val_dtype), copy=False)
    for fields in (("nx", "ny", "nz"), ("tangent_x", "tangent_y", "tangent_z"),
                   ("bitangent_x", "bitangent_y", "bitangent_z")):
        if any(name in names for name in fields):
            if not all(name in names and faces.dtype[name].kind == "f" for name in fields):
                _fail("camera_scale_export_incomplete_direction_fields")
            directions = np.column_stack([faces[name] for name in fields]).astype(np.float64)
            if not np.isfinite(directions).all():
                _fail("camera_scale_export_nonfinite_direction")
            directions = directions @ rotation.T
            for axis, name in enumerate(fields):
                faces[name] = directions[:, axis]
    return _describe_like(faces, element)


def _check_crop_relations(ply):
    for element in ply.elements:
        for prop in element.properties:
            if element.name == "face" and prop.name in ("vertex_indices", "vertex_index", "texture_index", "material_index"):
                continue
            if (re.search(r"(?:^|_)(?:index|indices)(?:_|$)", prop.name, re.IGNORECASE) or
                    re.search(r"(?:vertex|vertices|face|faces|edge|edges|point|points)(?:_|[0-9]|$)", prop.name, re.IGNORECASE)):
                _fail("camera_scale_export_unsupported_index_relation", element.name + "." + prop.name)


def _export_recorded_ply(input_ply, output_ply, mode, sidecar, cancelled, deleted_indices,
                         progress, deleted_vertex_count, output_unit, representation, scene_transform):
    """Write a new typed PLY and traceable sidecar, retaining the input.

    XYZ is promoted to float64 so restoration of large source origins does not
    lose centimetre-scale geometry to a float32 PLY field. Other attributes and
    elements are retained, except explicitly transformed Gaussian quantities.
    """
    source, output = Path(input_ply), Path(output_ply)
    metadata = Path(sidecar) if sidecar else source.with_suffix(".camera-scale.json")
    output_metadata = output.with_suffix(".camera-scale.json")
    output_cameras = output.with_suffix(".cameras.json")
    if _same_file(source, output) or _same_file(metadata, output_metadata):
        _fail("camera_scale_export_cannot_overwrite_source")
    if output.exists() or output_metadata.exists() or output_cameras.exists():
        _fail("camera_scale_export_requires_new_output")
    if not output.parent.is_dir() or output.parent.is_symlink() or output.is_symlink():
        _fail("camera_scale_export_invalid_output_directory")
    _no_links(output.parent)
    _check_cancel(cancelled)
    contract, source_hash, metadata_hash, input_cameras = _read_context(source, metadata)
    input_camera_hash = _hash(input_cameras) if input_cameras else None
    root_contract, current_frame, current_unit, current_to_canonical = _context(contract)
    geometry_provenance = _geometry_provenance(contract, root_contract, source_hash, metadata_hash, current_frame, current_unit)
    old_scene = _current_scene_state(contract)
    scene_request = _scene_request(scene_transform, current_frame, current_unit) if scene_transform is not None else None
    new_scene = old_scene
    transform, unit, frame = _output_transform(contract, mode, output_unit)
    if scene_request is not None:
        edit = scene_request["transform"]
        new_scene = dict(_compose(current_to_canonical, _compose(edit,
            _compose(camera_dataset.invert_sim3(current_to_canonical), old_scene))),
            input_frame="canonical", output_frame="canonical", applied=True)
        transform = dict(_compose(transform, edit), input_frame=current_frame, output_frame=frame, applied=True)
    s, rotation, translation = transform["s"], np.asarray(transform["R"]), np.asarray(transform["t"])
    rotated = not np.allclose(rotation, np.eye(3), rtol=0., atol=1e-12)
    ply = PlyData.read(source, mmap=False)
    source_assets = _texture_inputs(source, ply, contract)
    asset_directory = _asset_directory(output) if source_assets else None
    if asset_directory is not None and asset_directory.exists():
        _fail("camera_scale_export_requires_new_output")
    if "vertex" not in ply or not len(ply["vertex"].data):
        _fail("camera_scale_export_empty_gaussian")
    if representation not in ("auto", "gaussian", "point_cloud", "mesh"):
        _fail("camera_scale_export_invalid_representation")
    has_faces = "face" in ply and len(ply["face"].data) > 0
    if has_faces and representation in ("gaussian", "point_cloud"):
        _fail("camera_scale_export_mesh_not_supported")
    original = ply["vertex"].data
    if deleted_vertex_count is not None and (type(deleted_vertex_count) is not int or deleted_vertex_count != len(original)):
        _fail("camera_scale_export_edit_state_mismatch")
    names = original.dtype.names
    if not all(name in names and original.dtype.fields[name][0].kind in "fiu" for name in ("x", "y", "z")):
        _fail("camera_scale_export_invalid_vertex_positions")
    if representation == "auto":
        gaussian_markers = any(re.fullmatch(r"(?:scale|rot|f_dc|f_rest)_\d+", name) for name in names)
        if gaussian_markers and has_faces:
            _fail("camera_scale_export_mesh_not_supported")
        representation = "gaussian" if gaussian_markers else "mesh" if has_faces else "point_cloud"
    if representation == "mesh" and not has_faces:
        _fail("camera_scale_export_invalid_mesh_indices")
    gaussian = representation == "gaussian"
    scale_fields = sorted((name for name in names if name.startswith("scale_") and name[6:].isdigit()),
                          key=lambda name: int(name[6:])) if gaussian else []
    if gaussian and (scale_fields not in (["scale_0", "scale_1"], ["scale_0", "scale_1", "scale_2"]) or not all(name in names for name in ("x", "y", "z", "opacity"))):
        _fail("camera_scale_export_requires_supported_gaussian_ply")
    if gaussian and root_contract.get("backend") in ("3dgs", "2dgs") and len(scale_fields) != (2 if root_contract["backend"] == "2dgs" else 3):
        _fail("camera_scale_export_backend_dimensions_mismatch")
    if any(original.dtype.fields[name][0].kind != "f" for name in scale_fields):
        _fail("camera_scale_export_requires_float_log_scales")
    sh_matrix, sh_fields, sh_report = _sh_rotation(names, rotation) if gaussian and rotated else (None, [], {"degree": None, "method": "unchanged" if gaussian else "not_gaussian"})
    rotations = ["rot_{}".format(index) for index in range(4)] if gaussian else []
    if not all(name in names and original.dtype.fields[name][0].kind == "f" for name in rotations):
        _fail("camera_scale_export_missing_gaussian_rotation")
    if gaussian and not all(name in names and original.dtype.fields[name][0].kind == "f" for name in ("f_dc_0", "f_dc_1", "f_dc_2")):
        _fail("camera_scale_export_missing_gaussian_color")
    direction_fields = []
    for fields in (("nx", "ny", "nz"), ("tangent_x", "tangent_y", "tangent_z"),
                   ("bitangent_x", "bitangent_y", "bitangent_z")):
        if any(name in names for name in fields):
            if not all(name in names and original.dtype.fields[name][0].kind == "f" for name in fields):
                _fail("camera_scale_export_incomplete_direction_fields")
            direction_fields.append(fields)
    covariance = _covariance_fields(names)
    if covariance and any(original.dtype.fields[name][0].kind != "f" for name in covariance):
        _fail("camera_scale_export_requires_float_covariance")
    if any(original.dtype.fields[name][0].kind != "f" for name in sh_fields):
        _fail("camera_scale_export_requires_float_SH")
    dtype = [(name, "f8" if name in ("x", "y", "z") else original.dtype.fields[name][0]) for name in names]
    retained = np.ones(len(original), dtype=bool)
    if deleted_indices is not None:
        indices = np.asarray(list(deleted_indices))
        if indices.size and (indices.dtype.kind not in "iu" or int(indices.min()) < 0 or int(indices.max()) >= len(original)):
            _fail("camera_scale_export_invalid_deleted_vertices")
        retained[indices.astype(np.int64)] = False
    if not retained.any():
        _fail("camera_scale_export_empty_gaussian")
    if not retained.all():
        _check_crop_relations(ply)
    mesh_element = _mesh_faces(ply["face"], len(original), rotation, retained) if representation == "mesh" else None
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
        if gaussian:
            quaternions = np.stack([block[name] for name in rotations], axis=1).astype(np.float64)
            if not np.isfinite(quaternions).all() or np.any(np.linalg.norm(quaternions, axis=1) <= 1e-20):
                _fail("camera_scale_export_invalid_quaternion")
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
            if gaussian:
                q = _rotate_quaternions(np.stack([block[name] for name in rotations], axis=1), rotation)
                for index, name in enumerate(rotations):
                    block[name] = q[:, index]
            for fields in direction_fields:
                normal = np.stack([block[name] for name in fields], axis=1).astype(np.float64)
                if not np.isfinite(normal).all():
                    _fail("camera_scale_export_nonfinite_direction")
                normal = normal @ rotation.T
                for index, name in enumerate(fields):
                    block[name] = normal[:, index]
            if sh_matrix is not None:
                values = np.stack([block[name] for name in sh_fields], axis=1).astype(np.float64)
                if not np.isfinite(values).all():
                    _fail("camera_scale_export_nonfinite_SH")
                changed_sh = values.reshape(len(block), 3, -1) @ sh_matrix.T
                for index, name in enumerate(sh_fields):
                    block[name] = changed_sh.reshape(len(block), -1)[:, index]
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
        checked = list(scale_fields) + rotations + (covariance or [])
        if gaussian:
            checked += [name for name in names if name.startswith(("f_dc_", "f_rest_"))] + ["opacity"]
        if any(not np.isfinite(block[name]).all() for name in checked):
            _fail("camera_scale_export_output_attribute_overflow")
    temporary = output.with_name("." + output.name + "." + uuid.uuid4().hex + ".tmp")
    temporary_metadata = output_metadata.with_name("." + output_metadata.name + "." + uuid.uuid4().hex + ".tmp")
    temporary_cameras = output_cameras.with_name("." + output_cameras.name + "." + uuid.uuid4().hex + ".tmp")
    published = []
    temporary_assets, assets = [], []
    owned_asset_directory = False
    try:
        for index, asset in enumerate(source_assets):
            _check_cancel(cancelled)
            suffix = asset["path"].suffix.lower()
            suffix = suffix if re.fullmatch(r"\.[a-z0-9]{1,16}", suffix) else ".bin"
            final = asset_directory / ("texture-{:04d}".format(index) + suffix)
            temporary_asset = output.parent / (".gsw-asset-" + uuid.uuid4().hex + ".tmp")
            temporary_assets.append((temporary_asset, final))
            with asset["path"].open("rb") as incoming, temporary_asset.open("xb") as outgoing:
                while True:
                    _check_cancel(cancelled)
                    block = incoming.read(1024 * 1024)
                    if not block:
                        break
                    outgoing.write(block)
                outgoing.flush()
                os.fsync(outgoing.fileno())
            if _hash(temporary_asset) != asset["sha256"]:
                _fail("camera_scale_export_source_changed")
            assets.append({"kind": "texture_file", "path": final.relative_to(output.parent).as_posix(),
                "sha256": asset["sha256"], "bytes": asset["bytes"], "source_reference": asset["source_reference"]})
        comments = [value for value in ply.comments if not value.lower().startswith(("units ", "unit ", "coordinate_unit "))]
        texture_index = 0
        def rewrite_texture_comments(values):
            nonlocal texture_index
            changed = list(values)
            for index, comment in enumerate(changed):
                if re.fullmatch(r"TextureFile\s+(.+)", comment, re.IGNORECASE):
                    changed[index] = "TextureFile " + assets[texture_index]["path"]
                    texture_index += 1
            return changed
        comments = rewrite_texture_comments(comments)
        if unit in ("m", "cm", "mm"):
            comments.append("units " + unit)
        scale_status = root_contract["scale"]["status"]
        evidence = root_contract["scale"].get("evidence", [])
        comments += ["gsw_coordinate_frame " + frame, "gsw_scale_status " + scale_status]
        elements = []
        for element in ply.elements:
            element_comments = rewrite_texture_comments(element.comments)
            data = vertices if element.name == "vertex" else mesh_element.data if element.name == "face" and mesh_element is not None else element.data
            elements.append(_describe_like(data, element, element_comments))
        PlyData(elements, text=False, byte_order="<", comments=comments, obj_info=ply.obj_info).write(temporary)
        _check_cancel(cancelled)
        if progress:
            progress(95)
        if _hash(source) != source_hash or _hash(metadata) != metadata_hash:
            _fail("camera_scale_export_source_changed")
        version = 3 if not gaussian or scene_request is not None or assets or contract.get("export_version", 1) >= 3 else 2
        report = {"schema": "gsw.coordinate_export/1", "export_version": version, "success": True, "coordinate_mode": mode,
                  "input_frame": transform.get("input_frame", "training"), "output_frame": frame, "output_unit": unit,
                  "input_unit": current_unit, "current_frame": frame, "current_unit": unit, "transform_applied": True,
                  "scale_status": scale_status, "scale_evidence": copy.deepcopy(evidence),
                  "applied_transform": transform, "input_sha256": source_hash, "input_metadata_sha256": metadata_hash,
                  "output_sha256": _hash(temporary), "vertex_count": count, "deleted_vertices": int((~retained).sum()),
                  "gaussian_dimensions": len(scale_fields) if gaussian else None, "appearance_rotation": "not_applicable" if not gaussian else "real_sh_basis_rotation" if sh_matrix is not None else "DC_only" if rotated else "unchanged",
                  "sh_rotation": sh_report,
                  "representation": ("2dgs_surfel_gaussian" if len(scale_fields) == 2 else "3dgs_gaussian") if gaussian else representation,
                  "position_storage": "float64", "surface_accuracy_certified": False,
                  "covariance_fields": covariance or [],
                  "original_training_model_retained": True, "input_model_retained": True,
                  "input_data_identity": root_contract["input_data_identity"],
                  "scale": copy.deepcopy(root_contract["scale"]), "transforms": copy.deepcopy(root_contract["transforms"]),
                  "training_contract": copy.deepcopy(root_contract)}
        if mesh_element is not None:
            report["face_count"] = len(mesh_element.data)
            report["deleted_faces"] = len(ply["face"].data) - len(mesh_element.data)
        report["ply_sha256"] = report["output_sha256"]
        if version >= 3:
            report.update(input_canonical_scene_transform=old_scene, canonical_scene_transform=new_scene,
                scene_scale_multiplier=new_scene["s"], physical_scale_modified=abs(new_scene["s"] - 1.) > 1e-12,
                scale_evidence_scope="immutable_training_contract", scene_edit_independent_validation=False,
                full_optimizer_state_transformed=False, assets=assets, assets_sha256=_assets_hash(assets))
            if scene_request is not None:
                report["scene_transform"] = scene_request
            if geometry_provenance is not None:
                report["source_geometry_provenance"] = geometry_provenance
        cameras = _camera_bundle(root_contract, report["output_sha256"], frame, unit, new_scene)
        with temporary_cameras.open("x", encoding="utf-8") as stream:
            json.dump(cameras, stream, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        camera_hash = _hash(temporary_cameras)
        report["camera_reference"] = {"path": output_cameras.name, "sha256": camera_hash, "schema": "gsw.model_cameras/1"}
        report["bundle_sha256"] = _bundle_hash(report["output_sha256"], camera_hash, root_contract["input_data_identity"], frame, unit,
            report.get("assets_sha256") if version >= 3 else None)
        report["camera_count"] = len(cameras["views"])
        sidecar_record = report
        operations = copy.deepcopy(contract.get("export_operations", []))
        if not isinstance(operations, list) or len(operations) >= 1024 or any(type(item) is not dict for item in operations):
            _fail("camera_scale_export_invalid_operation_history")
        operations.append({"operation": "source_subset" if int((~retained).sum()) else "coordinate_export",
                           "input_ply_sha256": source_hash, "input_metadata_sha256": metadata_hash,
                           "input_frame": current_frame, "input_unit": current_unit,
                           "coordinate_frame": frame, "coordinate_unit": unit, "retained_vertices": count,
                           "deleted_vertices": int((~retained).sum()), "transform_applied": True,
                           "coordinate_values_changed": transform["s"] != 1. or rotated or bool(np.any(translation))})
        if version >= 3:
            operations[-1].update(input_canonical_scene_transform=old_scene, canonical_scene_transform=new_scene)
            if scene_request is not None:
                operations[-1]["scene_transform"] = scene_request
        if mesh_element is not None:
            operations[-1].update(retained_faces=report["face_count"], deleted_faces=report["deleted_faces"])
        sidecar_record["export_operations"] = operations
        with temporary_metadata.open("w", encoding="utf-8") as stream:
            json.dump(sidecar_record, stream, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        _check_cancel(cancelled)
        if _hash(source) != source_hash or _hash(metadata) != metadata_hash or input_cameras and _hash(input_cameras) != input_camera_hash:
            _fail("camera_scale_export_source_changed")
        _source_assets_unchanged(source_assets)
        # Atomic exclusive publication: no existing output can be overwritten,
        # including a file created by another writer after the initial check.
        if asset_directory is not None:
            asset_directory.mkdir()
            owned_asset_directory = True
            for temporary_asset, final in temporary_assets:
                os.link(temporary_asset, final)
                published.append((final, temporary_asset))
        os.link(temporary, output)
        published.append((output, temporary))
        os.link(temporary_cameras, output_cameras)
        published.append((output_cameras, temporary_cameras))
        os.link(temporary_metadata, output_metadata)
        published.append((output_metadata, temporary_metadata))
        if progress:
            progress(100)
        return report
    except Exception:
        for path, owned_temporary in reversed(published):
            if path.exists() and owned_temporary.exists() and os.path.samefile(path, owned_temporary):
                path.unlink()
        if owned_asset_directory:
            try:
                asset_directory.rmdir()
            except OSError:
                # A competing writer's file must not be recursively erased.
                pass
        raise
    finally:
        for path in (temporary, temporary_metadata, temporary_cameras) + tuple(item[0] for item in temporary_assets):
            if path.exists():
                path.unlink()


def deliver_training_result(input_ply, output_directory, sidecar=None, cancelled=None, progress=None):
    """Publish the metric default through the same validated manual exporter.

    Training completion is never relabelled failed by a blocked/failed export.
    Retry reuses only a completely validated bundle derived from this input.
    The raw model/checkpoint and source metadata are never modified.
    """
    source = Path(input_ply)
    result = {"schema": "gsw.model_delivery/1", "training_status": "done", "export_status": "blocked",
              "model_path": str(source), "sidecar_path": None, "camera_path": None,
              "reason": "", "report": None, "original_training_model_retained": True,
              "already_available": False}
    try:
        info = inspect_coordinate_metadata(source, sidecar)
        if not info["metric_available"]:
            result["reason"] = info["metric_reason"]
            return result
        destination = Path(output_directory)
        _no_links(destination)
        destination.mkdir(parents=True, exist_ok=True)
        stem = source.stem
        if len(stem) > 100:
            stem = stem[:64] + "-" + hashlib.sha256(stem.encode("utf-8")).hexdigest()[:16]
        output = destination / (stem + ".metric-m.ply")
        metadata = output.with_suffix(".camera-scale.json")
        if output.exists() or metadata.exists() or output.with_suffix(".cameras.json").exists():
            inspect_coordinate_metadata(output)
            report = _json_read(metadata)
            if (report.get("input_sha256") != info["model_sha256"] or
                    report.get("input_metadata_sha256") != info["metadata_sha256"] or
                    report.get("output_frame") != "canonical" or report.get("output_unit") != "m"):
                _fail("camera_scale_export_existing_delivery_mismatch")
            result["already_available"] = True
        else:
            report = export_camera_scale_ply(source, output, "metric", sidecar, cancelled,
                                             progress=progress, output_unit="m")
        result.update(export_status="ready", model_path=str(output), sidecar_path=str(metadata),
                      camera_path=str(output.parent / report["camera_reference"]["path"]), report=report)
    except (OSError, ValueError, KeyError, TypeError) as error:
        result.update(reason=getattr(error, "code", "camera_scale_export_failed"), detail=str(error))
        if result["reason"] == "camera_scale_export_cancelled":
            result["export_status"] = "cancelled"
        elif result["reason"] not in ("camera_scale_export_unknown_metric_scale",):
            result["export_status"] = "failed"
    return result


def main(argv=None):
    # Qt consumes UTF-8 protocol bytes, not the parent's Windows console code
    # page or PYTHONIOENCODING override, including error/argument diagnostics.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description="Restore recorded native Gaussian coordinates without changing the training PLY")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--coordinates", choices=("source", "canonical", "metric", "training", "preserve"), required=True)
    parser.add_argument("--unit", choices=("m", "cm", "mm"))
    parser.add_argument("--metadata")
    parser.add_argument("--representation", choices=("auto", "gaussian", "point_cloud", "mesh"), default="gaussian")
    parser.add_argument("--scene-transform")
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
        scene = _json_read(Path(args.scene_transform)) if args.scene_transform else None
        report = export_recorded_ply(args.input, args.output, args.coordinates, args.metadata,
            cancelled=(lambda: Path(args.cancel_file).is_file()) if args.cancel_file else None,
            deleted_indices=deleted, deleted_vertex_count=args.deleted_vertex_count, output_unit=args.unit,
            representation=args.representation, scene_transform=scene,
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
