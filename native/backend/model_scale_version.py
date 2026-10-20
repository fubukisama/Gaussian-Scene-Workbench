"""Content-bound scale constraints for an existing PLY, without fake cameras.

Inspection streams source/asset hashes and parses only a bounded PLY header.
Version creation validates and transforms real model records on CPU, retains
independent source bytes and never claims constraint agreement is accuracy.
"""

import copy
import hashlib
import io
import json
import math
import os
import re
from pathlib import Path, PurePosixPath
import shutil
import stat
import uuid

import numpy as np
from plyfile import PlyData, PlyElement

from native.backend.camera_scale_export import _covariance_fields, _rotate_quaternions, _similarity
from native.backend.scale_constraints import solve_scale_constraints, validate_scale_solution


MAXIMUM_JSON_BYTES = 16 * 1024 * 1024
MAXIMUM_HEADER_BYTES = 8 * 1024 * 1024
_TYPES = {"char", "uchar", "short", "ushort", "int", "uint", "float", "double",
          "int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"}


class ModelScaleVersionError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _fail(code, detail=""):
    raise ModelScaleVersionError(code, detail)


def _cancel(cancelled):
    if cancelled and cancelled():
        _fail("model_scale_cancelled")


def _emit(progress, stage, completed, total):
    if progress:
        progress({"stage": stage, "completed": completed, "total": total})


def _json_bytes(value):
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2).encode("utf-8")
    if len(encoded) > MAXIMUM_JSON_BYTES:
        _fail("model_scale_metadata_too_large")
    return encoded


def _digest_json(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()


def _no_links(path):
    for entry in (Path(path), *Path(path).parents):
        try:
            metadata = os.lstat(entry)
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
            _fail("model_scale_reparse_path", str(path))


def _source_file(path):
    path = Path(path)
    if not path.is_absolute():
        _fail("model_scale_absolute_path_required")
    _no_links(path)
    if not path.is_file():
        _fail("model_scale_missing_source", str(path))
    return path.resolve()


def _new_directory(path, source):
    path = Path(path)
    if not path.is_absolute():
        _fail("model_scale_absolute_path_required")
    _no_links(path)
    if path.exists():
        _fail("model_scale_output_exists", str(path))
    path = path.resolve()
    if path == source or not path.parent.is_dir():
        _fail("model_scale_unsafe_destination", str(path))
    # Do not create output inside an original model's owned texture directory.
    try:
        path.relative_to(source.parent)
    except ValueError:
        pass
    else:
        # Sibling version directories are intentionally permitted. Only the
        # selected file and its explicitly owned assets are immutable inputs.
        if path == source.parent:
            _fail("model_scale_unsafe_destination", str(path))
    return path


def _hash(path, cancelled=None, progress=None, stage="hash_model"):
    path = _source_file(path)
    digest = hashlib.sha256()
    before = path.stat()
    completed = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            _cancel(cancelled)
            digest.update(block)
            completed += len(block)
            _emit(progress, stage, completed, before.st_size)
    after = path.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        _fail("model_scale_source_changed", str(path))
    _cancel(cancelled)
    return digest.hexdigest()


def _header(path, cancelled=None):
    comments, elements, current, size = [], [], None, 0
    format_name = None
    try:
        with path.open("rb") as stream:
            if stream.readline(128).strip() != b"ply":
                _fail("model_scale_invalid_ply", "missing PLY magic")
            while True:
                _cancel(cancelled)
                raw = stream.readline(MAXIMUM_HEADER_BYTES + 1)
                size += len(raw)
                if not raw or size > MAXIMUM_HEADER_BYTES:
                    _fail("model_scale_invalid_ply", "missing or oversized header")
                line = raw.decode("ascii").strip()
                if line == "end_header":
                    break
                parts = line.split()
                if not parts:
                    continue
                if parts[0] == "format":
                    if format_name is not None or len(parts) != 3 or parts[2] != "1.0" or parts[1] not in (
                            "ascii", "binary_little_endian", "binary_big_endian"):
                        _fail("model_scale_invalid_ply", "unsupported format")
                    format_name = parts[1]
                elif parts[0] == "comment":
                    comments.append(line[len("comment"):].strip())
                elif parts[0] == "obj_info":
                    continue
                elif parts[0] == "element":
                    if len(parts) != 3 or not parts[2].isdigit() or any(item["name"] == parts[1] for item in elements):
                        _fail("model_scale_invalid_ply", "element")
                    current = {"name": parts[1], "count": int(parts[2]), "properties": []}
                    elements.append(current)
                elif parts[0] == "property":
                    if current is None:
                        _fail("model_scale_invalid_ply", "property without element")
                    if len(parts) == 3 and parts[1] in _TYPES:
                        item = {"name": parts[2], "type": parts[1], "kind": "scalar"}
                    elif len(parts) == 5 and parts[1] == "list" and all(value in _TYPES for value in parts[2:4]):
                        item = {"name": parts[4], "count_type": parts[2], "type": parts[3], "kind": "list"}
                    else:
                        _fail("model_scale_invalid_ply", "unsupported property")
                    if any(previous["name"] == item["name"] for previous in current["properties"]):
                        _fail("model_scale_invalid_ply", "duplicate property")
                    current["properties"].append(item)
                else:
                    _fail("model_scale_invalid_ply", "unknown header record")
    except (OSError, UnicodeError) as error:
        _fail("model_scale_invalid_ply", error)
    vertices = next((item for item in elements if item["name"] == "vertex"), None)
    if format_name is None or vertices is None or vertices["count"] <= 0:
        _fail("model_scale_empty_model")
    scalar_names = {item["name"] for item in vertices["properties"] if item["kind"] == "scalar"}
    if not {"x", "y", "z"}.issubset(scalar_names):
        _fail("model_scale_missing_xyz")
    scales = sorted(name for name in scalar_names if name.startswith("scale_") and name[6:].isdigit())
    gaussian = "opacity" in scalar_names and bool(scales)
    if gaussian and scales not in (["scale_0", "scale_1"], ["scale_0", "scale_1", "scale_2"]):
        _fail("model_scale_invalid_gaussian_scales")
    return {"format": format_name, "elements": elements, "comments": comments,
            "vertex_count": vertices["count"],
            "face_count": next((item["count"] for item in elements if item["name"] == "face"), 0),
            "gaussian_dimensions": len(scales) if gaussian else 0,
            "has_directional_sh": any(name.startswith("f_rest_") for name in scalar_names)}


def _textures(path, header, cancelled=None, progress=None):
    textures = []
    for index, comment in enumerate(header["comments"]):
        if not comment.lower().startswith("texturefile") or (len(comment) > 11 and not comment[11].isspace()):
            continue
        relative = comment[11:].strip()
        if len(relative) >= 2 and relative[0] == relative[-1] and relative[0] in ("'", '"'):
            relative = relative[1:-1]
        normalized = relative.replace("\\", "/")
        name = PurePosixPath(normalized)
        if (not normalized or name.is_absolute() or ".." in name.parts or ":" in normalized or "\x00" in normalized):
            _fail("model_scale_texture_outside_source", relative)
        texture = _source_file(path.parent.joinpath(*name.parts))
        if texture == path:
            _fail("model_scale_invalid_texture", "model cannot be its own texture")
        try:
            texture.relative_to(path.parent)
        except ValueError:
            _fail("model_scale_texture_outside_source", relative)
        digest = _hash(texture, cancelled, progress, "hash_texture")
        textures.append({"relative_path": name.as_posix(), "sha256": digest,
                         "byte_size": texture.stat().st_size, "comment_index": index})
    return textures


def _associated_sidecar(path, model_digest, cancelled=None, progress=None):
    sidecar = path.with_suffix(".camera-scale.json")
    if not sidecar.exists() and not sidecar.is_symlink():
        return None, None
    sidecar = _source_file(sidecar)
    if sidecar.stat().st_size > MAXIMUM_JSON_BYTES:
        _fail("model_scale_metadata_too_large")
    from native.backend.camera_dataset import _read_json
    metadata = _read_json(sidecar)
    if metadata.get("schema") not in ("gsw.training_camera_contract/1", "gsw.coordinate_export/1", "gsw.model_scale_export/1"):
        _fail("model_scale_unsupported_sidecar")
    if metadata.get("ply_sha256") != model_digest:
        _fail("model_scale_sidecar_hash_mismatch")
    return {"sha256": _hash(sidecar, cancelled, progress, "hash_sidecar"),
            "byte_size": sidecar.stat().st_size, "schema": metadata["schema"]}, metadata


def inspect_model_scale_input(model, expected_identity=None, cancelled=None, progress=None):
    """Return a portable semantic envelope; absolute display paths are not identity."""
    _cancel(cancelled)
    model = _source_file(model)
    if model.suffix.lower() != ".ply":
        _fail("model_scale_unsupported_format", model.suffix)
    header = _header(model, cancelled)
    digest = _hash(model, cancelled, progress)
    textures = _textures(model, header, cancelled, progress)
    sidecar, _ = _associated_sidecar(model, digest, cancelled, progress)
    semantic = {"schema": "gsw.scale_input/1", "kind": "model", "model": {
        "sha256": digest, "byte_size": model.stat().st_size, "header": header,
        "textures": textures, "sidecar": sidecar}}
    identity = _digest_json(semantic)
    if expected_identity is not None and expected_identity != identity:
        _fail("model_scale_input_identity_mismatch")
    # The source may have changed between parsing the header and hashing it.
    if _hash(model, cancelled, None) != digest or _header(model, cancelled) != header:
        _fail("model_scale_source_changed")
    result = dict(semantic, data_identity=identity, views=[])
    result["model"] = dict(semantic["model"], path=str(model), file_name=model.name,
                           vertex_count=header["vertex_count"], face_count=header["face_count"],
                           gaussian_dimensions=header["gaussian_dimensions"],
                           has_directional_sh=header["has_directional_sh"])
    return result


def model_scale_input_summary(envelope):
    model = envelope["model"]
    return {"sourceKind": "model", "inputDataIdentity": envelope["data_identity"],
            "modelSha256": model["sha256"], "modelFileName": model["file_name"],
            "vertexCount": model["vertex_count"], "faceCount": model["face_count"],
            "gaussianDimensions": model["gaussian_dimensions"], "hasDirectionalSH": model["has_directional_sh"],
            "textureCount": len(model["textures"]), "modelGeometryAccuracy": "not_measured"}


def estimate_model_scale_constraints(model, request, expected_identity=None, cancelled=None, progress=None):
    envelope = inspect_model_scale_input(model, expected_identity, cancelled, progress)
    _json_bytes(request)
    solution = solve_scale_constraints(envelope, request, cancelled)
    validate_scale_solution(envelope, request, solution, cancelled)
    _json_bytes(solution)
    return {"envelope": envelope, "solution": solution}


def _copy_file(source, destination, digest, cancelled=None, progress=None):
    source = _source_file(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        _fail("model_scale_output_exists", str(destination))
    copied = hashlib.sha256()
    total, completed = source.stat().st_size, 0
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        for block in iter(lambda: incoming.read(4 * 1024 * 1024), b""):
            _cancel(cancelled)
            copied.update(block)
            outgoing.write(block)
            completed += len(block)
            _emit(progress, "copy_model_asset", completed, total)
        outgoing.flush()
        os.fsync(outgoing.fileno())
    if copied.hexdigest() != digest or os.path.samefile(source, destination):
        _fail("model_scale_source_changed", str(source))


def _write_json(value, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as stream:
        stream.write(_json_bytes(value))
        stream.flush()
        os.fsync(stream.fileno())


class _CancellableIO(io.RawIOBase):
    """Bounded reads/writes also cover plyfile's text/list record loops."""
    def __init__(self, path, mode, cancelled):
        super().__init__()
        self._stream = Path(path).open(mode, buffering=0)
        self._writing = "w" in mode or "x" in mode
        self._cancelled = cancelled

    def readable(self):
        return not self._writing

    def writable(self):
        return self._writing

    def seekable(self):
        return True

    def fileno(self):
        return self._stream.fileno()

    def seek(self, offset, whence=0):
        _cancel(self._cancelled)
        return self._stream.seek(offset, whence)

    def tell(self):
        return self._stream.tell()

    def readinto(self, target):
        _cancel(self._cancelled)
        return self._stream.readinto(memoryview(target)[:4 * 1024 * 1024])

    def write(self, value):
        view = memoryview(value)
        completed = 0
        while completed < len(view):
            _cancel(self._cancelled)
            written = self._stream.write(view[completed:completed + 4 * 1024 * 1024])
            if not written:
                raise OSError("PLY output write made no progress")
            completed += written
        return completed

    def flush(self):
        if not self._stream.closed:
            self._stream.flush()
            if self._writing:
                os.fsync(self._stream.fileno())

    def close(self):
        if not self.closed:
            try:
                super().close()
            finally:
                self._stream.close()


_CRS_COMMENT = re.compile(r"(?:EPSG\s*[:=]?\s*\d{3,8}|(?:crs|coordinate[_ ]?system)\s*[:=])", re.IGNORECASE)
# Same unit declarations accepted by native SceneCoordinates, including ':'
# and '=' forms. Removing only a whitespace prefix leaves contradictory units
# on an otherwise correctly baked metric PLY.
_UNIT_COMMENT = re.compile(
    r"(?:^|\b)(?:coordinate[_ ]?unit|units?)\s*[:=]?\s*"
    r"(?:mm|millimet(?:er|re)s?|cm|centimet(?:er|re)s?|m|met(?:er|re)s?)\b", re.IGNORECASE)


def _transform_metadata_comment(value):
    return (bool(_CRS_COMMENT.search(value)) or bool(_UNIT_COMMENT.search(value))
            or value.lower().startswith(("gsw_coordinate_frame", "gsw_scale_status")))


def _input_declared_crs(envelope):
    """Retain input declarations only; a local Sim3 never establishes an output CRS."""
    declarations = []
    for comment in envelope["model"]["header"]["comments"]:
        if not _CRS_COMMENT.search(comment):
            continue
        epsg = re.search(r"EPSG\s*[:=]?\s*(\d{3,8})", comment, re.IGNORECASE)
        declarations.append("EPSG:" + epsg.group(1) if epsg else comment)
    return "; ".join(dict.fromkeys(declarations))


def _transformed_ply(source, destination, transform, envelope, texture_comments, cancelled=None, progress=None):
    _cancel(cancelled)
    try:
        with _CancellableIO(source, "rb", cancelled) as raw:
            with io.BufferedReader(raw, buffer_size=4 * 1024 * 1024) as stream:
                ply = PlyData.read(stream)
    except (OSError, ValueError, TypeError) as error:
        _cancel(cancelled)
        _fail("model_scale_invalid_ply", error)
    transform = _similarity(transform)
    scale, rotation, translation = transform["s"], np.asarray(transform["R"]), np.asarray(transform["t"])
    rotated = not np.allclose(rotation, np.eye(3), rtol=0., atol=1e-12)
    gaussian = bool(envelope["model"]["gaussian_dimensions"])
    # Header inspection records all comments in file order, whereas plyfile
    # stores global and element-local comments separately. Keep that order for
    # the already-hashed texture comment indexes; never guess a replacement.
    ordered_comments = list(ply.comments)
    for element in ply.elements:
        ordered_comments.extend(element.comments)
    if [value.strip() for value in ordered_comments] != envelope["model"]["header"]["comments"]:
        _fail("model_scale_header_comment_layout_changed")
    comment_index = 0

    def transformed_comments(values):
        nonlocal comment_index
        result = []
        for value in values:
            original_index = comment_index
            comment_index += 1
            if original_index in texture_comments:
                result.append("TextureFile " + texture_comments[original_index])
            elif not _transform_metadata_comment(value):
                result.append(value)
        return result

    comments = transformed_comments(ply.comments)
    elements = []
    for element in ply.elements:
        element.comments = transformed_comments(element.comments)
        data = element.data
        names = data.dtype.names
        is_vertex = element.name == "vertex"
        normals = all(name in names for name in ("nx", "ny", "nz"))
        if rotated and any(name in names for name in ("nx", "ny", "nz")) and not normals:
            _fail("model_scale_incomplete_normals", element.name)
        if not is_vertex and not (rotated and normals):
            elements.append(element)
            continue
        scales = ["scale_" + str(index) for index in range(envelope["model"]["gaussian_dimensions"])] if is_vertex else []
        covariance = _covariance_fields(names) if is_vertex else None
        rotations = ["rot_" + str(index) for index in range(4)]
        if is_vertex and gaussian:
            if any(data.dtype.fields[name][0].kind != "f" for name in scales):
                _fail("model_scale_invalid_gaussian_scales")
            if rotated and not all(name in names for name in rotations):
                _fail("model_scale_missing_gaussian_rotation")
        promoted = set(("x", "y", "z") if is_vertex else ())
        if rotated and normals:
            promoted.update(("nx", "ny", "nz"))
        if covariance:
            promoted.update(covariance)
        changed = np.empty(len(data), dtype=[(name, "f8" if name in promoted else data.dtype.fields[name][0]) for name in names])
        for name in names:
            changed[name] = data[name]
        for start in range(0, len(changed), 65536):
            _cancel(cancelled)
            block = changed[start:start + 65536]
            if is_vertex:
                xyz = np.stack([block[name] for name in ("x", "y", "z")], axis=1)
                xyz = scale * (xyz @ rotation.T) + translation
                if not np.isfinite(xyz).all():
                    _fail("model_scale_nonfinite_geometry")
                for index, name in enumerate(("x", "y", "z")):
                    block[name] = xyz[:, index]
                for name in scales:
                    if not np.isfinite(block[name]).all():
                        _fail("model_scale_nonfinite_gaussian")
                    block[name] += math.log(scale)
                if rotated and gaussian:
                    values = _rotate_quaternions(np.stack([block[name] for name in rotations], axis=1), rotation)
                    for index, name in enumerate(rotations):
                        block[name] = values[:, index]
                if covariance:
                    values = np.stack([block[name] for name in covariance], axis=1)
                    matrix = np.zeros((len(block), 3, 3), dtype=np.float64)
                    matrix[:, 0, 0], matrix[:, 0, 1], matrix[:, 0, 2] = values[:, 0], values[:, 1], values[:, 2]
                    matrix[:, 1, 0], matrix[:, 1, 1], matrix[:, 1, 2] = values[:, 1], values[:, 3], values[:, 4]
                    matrix[:, 2, 0], matrix[:, 2, 1], matrix[:, 2, 2] = values[:, 2], values[:, 4], values[:, 5]
                    matrix = scale * scale * (rotation @ matrix @ rotation.T)
                    if not np.isfinite(matrix).all():
                        _fail("model_scale_nonfinite_covariance")
                    for name, pair in zip(covariance, ((0, 0), (0, 1), (0, 2), (1, 1), (1, 2), (2, 2))):
                        block[name] = matrix[:, pair[0], pair[1]]
            if rotated and normals:
                values = np.stack([block[name] for name in ("nx", "ny", "nz")], axis=1) @ rotation.T
                if not np.isfinite(values).all():
                    _fail("model_scale_nonfinite_normals")
                for index, name in enumerate(("nx", "ny", "nz")):
                    block[name] = values[:, index]
            _emit(progress, "transform_model", min(start + len(block), len(changed)), len(changed))
        # Preserve list count/value types so face topology and UVs are not
        # silently rewritten into a different integer/float convention.
        list_types = {property.name: property.len_dtype for property in element.properties if hasattr(property, "len_dtype")}
        value_types = {property.name: property.val_dtype for property in element.properties if hasattr(property, "len_dtype")}
        elements.append(PlyElement.describe(changed, element.name, len_types=list_types, val_types=value_types,
                                            comments=element.comments))
    comments.extend(["units m", "gsw_coordinate_frame metric"])
    _cancel(cancelled)
    with _CancellableIO(destination, "xb", cancelled) as raw:
        with io.BufferedWriter(raw, buffer_size=4 * 1024 * 1024) as stream:
            PlyData(elements, text=False, byte_order="<", comments=comments,
                    obj_info=[value for value in ply.obj_info if not _transform_metadata_comment(value)]).write(stream)
    _cancel(cancelled)


def create_scaled_model_version(model, request, output_directory, expected_identity=None, cancelled=None, progress=None):
    """Publish a fresh physical PLY or an explicit unbaked transform manifest."""
    source = _source_file(model)
    destination = _new_directory(output_directory, source)
    estimated = estimate_model_scale_constraints(source, request, expected_identity, cancelled, progress)
    envelope, solution = estimated["envelope"], estimated["solution"]
    if solution["independent_checks"]["status"] == "fail":
        _fail("model_scale_independent_checks_failed")
    rotated = not np.allclose(solution["source_to_metric"]["R"], np.eye(3), rtol=0., atol=1e-12)
    representation = "transform_manifest" if rotated and envelope["model"]["has_directional_sh"] else "baked_model"
    temporary = destination.with_name("." + destination.name + ".preparing-model-" + uuid.uuid4().hex)
    temporary.mkdir()
    try:
        original = temporary / "source/original.ply"
        _copy_file(source, original, envelope["model"]["sha256"], cancelled, progress)
        texture_comments, owned_assets, copied_texture_paths = {}, [], set()
        for index, texture in enumerate(envelope["model"]["textures"]):
            path = source.parent / texture["relative_path"]
            # The exact independent source copy remains usable with its
            # original relative texture names; the output gets safe names.
            if texture["relative_path"] not in copied_texture_paths:
                _copy_file(path, temporary / "source" / texture["relative_path"], texture["sha256"], cancelled, progress)
                copied_texture_paths.add(texture["relative_path"])
            output_name = "assets/texture-" + str(index) + "-" + texture["sha256"][:12] + path.suffix.lower()
            _copy_file(path, temporary / output_name, texture["sha256"], cancelled, progress)
            texture_comments[texture["comment_index"]] = output_name
            owned_assets.append(dict(texture, output_path=output_name))
        source_sidecar = envelope["model"]["sidecar"]
        prior = None
        if source_sidecar:
            _copy_file(source.with_suffix(".camera-scale.json"), temporary / "source/original.camera-scale.json",
                       source_sidecar["sha256"], cancelled, progress)
            from native.backend.camera_dataset import _read_json
            prior = _read_json(source.with_suffix(".camera-scale.json"))
        lineage = copy.deepcopy(prior.get("lineage", [])) if prior and prior.get("schema") == "gsw.model_scale_export/1" else []
        if not isinstance(lineage, list) or len(lineage) >= 8:
            _fail("model_scale_lineage_depth_exceeded")
        lineage.append({"input_data_identity": envelope["data_identity"], "model_sha256": envelope["model"]["sha256"],
                        "sidecar_sha256": source_sidecar["sha256"] if source_sidecar else None})
        _write_json(envelope, temporary / "source/input_descriptor.json")
        _write_json(request, temporary / "constraints.json")
        _write_json(solution, temporary / "solution.json")
        descriptor_hash = _hash(temporary / "source/input_descriptor.json", cancelled)
        scale = {"status": "independently_checked" if solution["independent_checks"]["status"] == "pass" else "constraint_derived",
            "source_unit": "source_units", "meters_per_unit": solution["source_to_metric"]["s"], "canonical_unit": "m",
            "verification_origin": "gsw_replay_verified", "evidence": [{"kind": "scale_constraints",
                "input_data_identity": envelope["data_identity"], "solution_sha256": solution["solution_sha256"],
                "scope": solution["scope"], "model_geometry_accuracy": "not_measured"}],
            "derivation": {"schema": "gsw.scale_derivation/1", "input_kind": "model",
                "input_data_identity": envelope["data_identity"], "input_manifest": {
                    "path": "source/input_descriptor.json", "sha256": descriptor_hash},
                "request": copy.deepcopy(request), "solution": copy.deepcopy(solution), "lineage": lineage}}
        applied = dict(solution["source_to_metric"], applied=representation == "baked_model")
        transform_record = {"schema": "gsw.model_scale_transform/1", "representation": representation,
            "input_model": "source/original.ply", "transform": applied,
            "appearance_rotation": "not_baked" if representation == "transform_manifest" else "DC_only" if rotated else "unchanged",
            "reason": "directional_sh_rotation_unsupported" if representation == "transform_manifest" else "supported_positive_similarity",
            "solution_sha256": solution["solution_sha256"], "model_geometry_accuracy": "not_measured"}
        _write_json(transform_record, temporary / "transform.json")
        output = temporary / "scaled.ply"
        if representation == "baked_model":
            _transformed_ply(source, output, applied, envelope, texture_comments, cancelled, progress)
            sidecar = {"schema": "gsw.model_scale_export/1", "ply_sha256": _hash(output, cancelled),
                "input_declared_crs": _input_declared_crs(envelope),
                "coordinate_frame": "metric", "input_model": {"sha256": envelope["model"]["sha256"],
                    "path": "source/original.ply", "coordinate_frame": "source", "data_identity": envelope["data_identity"]},
                "scale": scale, "solution_sha256": solution["solution_sha256"], "applied_transform": applied,
                "model_geometry_accuracy": "not_measured", "lineage": lineage, "textures": owned_assets,
                "representation": representation, "original_model_retained": True}
            _write_json(sidecar, output.with_suffix(".camera-scale.json"))
        version = {"schema": "gsw.model_scale_version/1", "representation": representation,
                   "input_data_identity": envelope["data_identity"], "scale": scale,
                   "model_path": "scaled.ply" if representation == "baked_model" else "",
                   "transform_path": "transform.json", "solution_path": "solution.json",
                   "model_geometry_accuracy": "not_measured", "lineage": lineage}
        _write_json(version, temporary / "model-scale-version.json")
        inspect_model_scale_input(source, envelope["data_identity"], cancelled, progress)
        _cancel(cancelled)
        _new_directory(destination, source)
        os.rename(str(temporary), str(destination))
        from native.backend.camera_admission import scale_solution_summary
        summary = dict(model_scale_input_summary(envelope), representation=representation,
                       scale={key: copy.deepcopy(value) for key, value in scale.items() if key != "derivation"},
                       scaleDerivation=scale_solution_summary(solution),
                       modelPath=str(destination / "scaled.ply") if representation == "baked_model" else "")
        return {"versionPath": str(destination), "modelPath": summary["modelPath"], "summary": summary, "solution": solution}
    finally:
        if temporary.exists() and temporary.parent == destination.parent:
            _no_links(temporary)
            shutil.rmtree(str(temporary))
