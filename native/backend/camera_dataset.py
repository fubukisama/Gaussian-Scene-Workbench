"""Versioned, CPU-only camera datasets shared by import and training.

The protocol uses float64 Python numbers, row-major camera-to-world OpenCV
poses (+X right, +Y down, +Z forward), pixel-border intrinsics and explicit
positive similarities. Input data is never edited, evaluated or re-solved.
"""

import copy
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import struct
import uuid


SCHEMA = "gsw.camera_dataset/1"
ADAPTER_VERSION = 1
MANIFEST_NAME = "camera_dataset.json"
_ROTATION = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
_UNITS = {"m": 1., "meter": 1., "meters": 1., "cm": .01,
          "centimeter": .01, "centimeters": .01, "mm": .001,
          "millimeter": .001, "millimeters": .001}


class CameraDatasetError(ValueError):
    """A typed admission error; code is suitable for localized native UI."""

    def __init__(self, code, detail=""):
        self.code = code
        self.detail = detail
        super().__init__(code + (": " + detail if detail else ""))


def _error(code, detail=""):
    raise CameraDatasetError(code, str(detail))


def _number(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        _error("camera_nonfinite_number", name)
    return float(value)


def _float64_ulp(value):
    # The configured research runtimes may use Python 3.8 (no math.ulp).
    # IEEE-754 spacing for a finite binary64 value, including subnormals.
    return math.ldexp(1., max(-1074, math.frexp(abs(value))[1] - 53)) if value else math.ldexp(1., -1074)


def _matrix(value, size, name):
    if not isinstance(value, (list, tuple)) or len(value) != size or any(
            not isinstance(row, (list, tuple)) or len(row) != size for row in value):
        _error("camera_invalid_matrix", name)
    return [[_number(x, name) for x in row] for row in value]


def _rotation(value, name):
    r = _matrix(value, 3, name)
    for i in range(3):
        for j in range(3):
            if abs(sum(r[k][i] * r[k][j] for k in range(3)) - (i == j)) > 2e-5:
                _error("camera_invalid_rotation", name)
    det = (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1])
           - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0])
           + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))
    if abs(det - 1.) > 2e-5:
        _error("camera_invalid_rotation", name)
    return r


def _pose(value, name="pose"):
    pose = _matrix(value, 4, name)
    if any(abs(x - expected) > 1e-10 for x, expected in zip(pose[3], (0., 0., 0., 1.))):
        _error("camera_invalid_pose", name)
    _rotation([row[:3] for row in pose[:3]], name)
    return pose


def _invert_pose(pose):
    r = [[pose[j][i] for j in range(3)] for i in range(3)]
    return [r[i] + [-sum(r[i][j] * pose[j][3] for j in range(3))]
            for i in range(3)] + [[0., 0., 0., 1.]]


def _read_json(path):
    path = Path(path)
    if path.stat().st_size > 128 * 1024 * 1024:
        _error("camera_manifest_too_large", path.name)
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                _error("camera_duplicate_json_key", key)
            result[key] = value
        return result

    def constant(value):
        _error("camera_nonfinite_number", value)

    try:
        with path.open(encoding="utf-8-sig") as stream:
            data = json.load(stream, object_pairs_hook=object_pairs, parse_constant=constant)
    except CameraDatasetError:
        raise
    except (ValueError, UnicodeError) as error:
        _error("camera_invalid_json", error)
    if not isinstance(data, dict):
        _error("camera_invalid_manifest", path.name)
    return data


def _hash(path, cancelled=None):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            if cancelled and cancelled():
                _error("camera_import_cancelled")
            result.update(block)
    return result.hexdigest()


def _relative(root, name, extension=False):
    if not isinstance(name, str) or not name or "\x00" in name or "\\" in name:
        _error("camera_invalid_image_path", name)
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or ":" in name:
        _error("camera_path_outside_dataset", name)
    path = root.joinpath(*relative.parts)
    if extension and not path.is_file() and not path.suffix:
        matches = [path.with_suffix(suffix) for suffix in (".png", ".jpg", ".jpeg", ".tif", ".tiff", ".exr")
                   if path.with_suffix(suffix).is_file()]
        if len(matches) == 1:
            path = matches[0]
        elif len(matches) > 1:
            _error("camera_ambiguous_image_path", name)
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        _error("camera_path_outside_dataset", name)
    if not path.is_file():
        _error("camera_missing_image", name)
    return path, path.relative_to(root).as_posix()


def _sim3(value=None, source_frame="source", target_frame="canonical"):
    value = value or {}
    s = _number(value.get("s", 1.), "similarity scale")
    if s <= 0:
        _error("camera_invalid_similarity", "scale must be positive")
    r = _rotation(value.get("R", _ROTATION), "similarity rotation")
    t = value.get("t", [0., 0., 0.])
    if not isinstance(t, (list, tuple)) or len(t) != 3:
        _error("camera_invalid_similarity", "translation")
    t = [_number(x, "similarity translation") for x in t]
    inverse_r = [[r[j][i] for j in range(3)] for i in range(3)]
    inverse = {"s": 1. / s, "R": inverse_r,
               "t": [-sum(inverse_r[i][j] * t[j] for j in range(3)) / s for i in range(3)]}
    if "inverse" in value:
        supplied = value["inverse"]
        if not isinstance(supplied, dict) or set(("s", "R", "t")) - set(supplied):
            _error("camera_conflicting_inverse")
        supplied_s = _number(supplied["s"], "inverse scale")
        supplied_r = _rotation(supplied["R"], "inverse rotation")
        supplied_t = supplied["t"]
        if (not isinstance(supplied_t, (list, tuple)) or len(supplied_t) != 3 or
                not math.isclose(supplied_s, inverse["s"], rel_tol=1e-10, abs_tol=1e-12) or
                any(not math.isclose(supplied_r[i][j], inverse["R"][i][j], rel_tol=1e-10, abs_tol=1e-12)
                    for i in range(3) for j in range(3)) or
                any(not math.isclose(_number(supplied_t[i], "inverse translation"), inverse["t"][i],
                                     rel_tol=1e-10, abs_tol=1e-10) for i in range(3))):
            _error("camera_conflicting_inverse")
    if "applied" in value and type(value["applied"]) is not bool:
        _error("camera_invalid_similarity", "applied must be boolean")
    return {"version": 1, "input_frame": value.get("input_frame", source_frame),
            "output_frame": value.get("output_frame", target_frame), "s": s, "R": r, "t": t,
            "inverse": inverse, "applied": bool(value.get("applied", False)),
            "mode": value.get("mode", "explicit" if value else "auto"),
            "evidence": copy.deepcopy(value.get("evidence", []))}


def apply_sim3(transform, points):
    """Apply one validated positive similarity to float64 points."""
    transform = _sim3(transform)
    output = []
    for point in points:
        if not isinstance(point, (list, tuple)) or len(point) != 3:
            _error("camera_invalid_point")
        point = [_number(x, "point") for x in point]
        transformed = [transform["s"] * sum(transform["R"][i][j] * point[j] for j in range(3))
                       + transform["t"][i] for i in range(3)]
        if not all(math.isfinite(x) for x in transformed):
            _error("camera_numeric_range_exceeded")
        output.append(transformed)
    return output


def invert_sim3(transform):
    """Return an explicit inverse, including provenance and frame direction."""
    value = _sim3(transform)
    return _sim3(dict(value["inverse"], evidence=value["evidence"]),
                 value["output_frame"], value["input_frame"])


def _transform_pose(pose, transform):
    center = apply_sim3(transform, [[pose[i][3] for i in range(3)]])[0]
    r = [[sum(transform["R"][i][k] * pose[k][j] for k in range(3)) for j in range(3)]
         for i in range(3)]
    return [r[i] + [center[i]] for i in range(3)] + [[0., 0., 0., 1.]]


def project_points(view, points, pixel_coordinates="pixel_border"):
    """Project canonical-frame positions with the actual full pinhole K.

    Returns [u, v, camera_z]. Points on/behind the camera are rejected rather
    than silently accepted as visible observations.
    """
    pose = _pose(view["c2w_opencv"])
    k = _intrinsics(view["K"])
    w2c = _invert_pose(pose)
    result = []
    for point in points:
        if not isinstance(point, (list, tuple)) or len(point) != 3:
            _error("camera_invalid_point")
        xyz = [sum(w2c[i][j] * _number(point[j], "point") for j in range(3)) + w2c[i][3]
               for i in range(3)]
        if xyz[2] <= 0:
            _error("camera_point_behind_camera")
        result.append([k[0][0] * xyz[0] / xyz[2] + k[0][2],
                       k[1][1] * xyz[1] / xyz[2] + k[1][2], xyz[2]])
    if pixel_coordinates == "opencv_integer":
        return [[u - .5, v - .5, z] for u, v, z in result]
    if pixel_coordinates != "pixel_border":
        _error("camera_unsupported_pixel_coordinates", pixel_coordinates)
    return result


def _intrinsics(value):
    k = _matrix(value, 3, "K")
    if k[0][0] <= 0 or k[1][1] <= 0 or any(abs(x) > 1e-12 for x in (k[1][0], k[2][0], k[2][1])) or abs(k[2][2] - 1.) > 1e-12:
        _error("camera_invalid_intrinsics")
    if abs(k[0][1]) > 1e-12:
        _error("camera_unsupported_skew")
    return k


def _scale(value):
    value = copy.deepcopy(value or {})
    status = value.get("status", "arbitrary")
    if status not in ("arbitrary", "source_declared", "constraint_derived", "independently_checked"):
        _error("camera_invalid_scale_status", status)
    unit = value.get("source_unit", "unknown")
    if not isinstance(unit, str):
        _error("camera_invalid_unit")
    meters = value.get("meters_per_unit", _UNITS.get(unit.lower()))
    if meters is not None:
        meters = _number(meters, "meters_per_unit")
        if meters <= 0 or (unit.lower() in _UNITS and not math.isclose(meters, _UNITS[unit.lower()], rel_tol=1e-12)):
            _error("camera_conflicting_units", unit)
    if status == "arbitrary" and (meters is not None or unit.lower() in _UNITS):
        _error("camera_conflicting_units", "arbitrary scale cannot claim meters")
    evidence = value.get("evidence", [])
    if not isinstance(evidence, list) or any(not isinstance(item, dict) or not isinstance(item.get("kind"), str)
                                             or not item["kind"] for item in evidence):
        _error("camera_missing_scale_evidence", status)
    if status != "arbitrary" and (meters is None or not evidence):
        _error("camera_missing_scale_evidence", status)
    value.update(status=status, source_unit=unit, meters_per_unit=meters,
                 evidence=evidence, canonical_unit="m" if meters is not None else "arbitrary")
    if status in ("constraint_derived", "independently_checked"):
        value["verification_origin"] = "external_claim"
    return value


def _source_pose(value, relative):
    poses = []
    if "source_c2w_opencv" in value:
        poses.append(_pose(value["source_c2w_opencv"], relative))
    for key in ("c2w_opencv", "w2c_opencv", "c2w_blender", "w2c_blender", "c2w", "w2c"):
        if key not in value:
            continue
        if key == "c2w_opencv" and "source_c2w_opencv" in value:
            continue  # This one is canonical; _view checks it against the chain.
        pose = _pose(value[key], relative)
        if key.startswith("w2c"):
            pose = _invert_pose(pose)
        axes = "blender" if key.endswith("blender") else value.get("camera_axes", "opencv")
        if key.endswith("opencv"):
            axes = "opencv"
        if axes not in ("opencv", "blender"):
            _error("camera_unsupported_axes", axes)
        if axes == "blender":
            for row in pose[:3]:
                row[1], row[2] = -row[1], -row[2]
        poses.append(pose)
    if not poses:
        _error("camera_missing_pose", relative)
    reference = poses[0]
    for candidate in poses[1:]:
        for i in range(4):
            for j in range(4):
                a, b = reference[i][j], candidate[i][j]
                if j == 3 and i < 3:
                    # A million-unit origin must not turn a relative tolerance
                    # into permission to move a camera by whole source units.
                    equal = abs(a - b) <= max(2e-7, 8 * _float64_ulp(a), 8 * _float64_ulp(b))
                else:
                    equal = math.isclose(a, b, rel_tol=2e-6, abs_tol=2e-7)
                if not equal:
                    _error("camera_conflicting_pose", relative)
    return reference


def _view(root, value, transform, cancelled=None):
    if cancelled and cancelled():
        _error("camera_import_cancelled")
    path, relative = _relative(root, value["image_path"], value.get("infer_extension", False))
    from PIL import Image
    try:
        with Image.open(path) as image:
            width, height = image.size
    except (OSError, ValueError) as error:
        _error("camera_invalid_image", relative + ": " + str(error))
    if any(name in value and value[name] != actual for name, actual in (("width", width), ("height", height))):
        _error("camera_image_dimensions_mismatch", relative)
    digest = _hash(path, cancelled)
    if value.get("sha256", digest) != digest:
        _error("camera_image_hash_mismatch", relative)
    model = value.get("projection_model", "PINHOLE")
    if model not in ("PINHOLE", "SIMPLE_PINHOLE"):
        _error("camera_unsupported_projection", model)
    distortion = value.get("distortion", [])
    if (value.get("undistorted") is False or not isinstance(distortion, (list, tuple))
            or any(abs(_number(x, "distortion")) > 1e-12 for x in distortion)):
        _error("camera_unsupported_distortion", relative)
    # Serialized normalized manifests carry both poses. Reopening must not
    # apply unit conversion/localization to the canonical pose a second time.
    pose = _source_pose(value, relative)
    source_k = _intrinsics(value.get("source_K", value["K"]))
    source_pixels = value.get("source_pixel_coordinates", value.get("pixel_coordinates", "opencv_integer"))
    if source_pixels not in ("opencv_integer", "pixel_border"):
        _error("camera_unsupported_pixel_coordinates", source_pixels)
    k = copy.deepcopy(source_k)
    if source_pixels == "opencv_integer":
        k[0][2] += .5
        k[1][2] += .5
    if "source_K" in value:
        actual_k = _intrinsics(value["K"])
        if any(not math.isclose(k[i][j], actual_k[i][j], rel_tol=1e-12, abs_tol=1e-12)
               for i in range(3) for j in range(3)):
            _error("camera_conflicting_intrinsics", relative)
    canonical_pose = _transform_pose(pose, transform)
    if "source_c2w_opencv" in value:
        actual_pose = _pose(value["c2w_opencv"], relative)
        if any(not math.isclose(actual_pose[i][j], canonical_pose[i][j], rel_tol=1e-12, abs_tol=1e-10)
               for i in range(4) for j in range(4)):
            _error("camera_conflicting_pose", relative)
    view_id = value.get("view_id") or "v-" + hashlib.sha256(relative.encode("utf-8")).hexdigest()[:24]
    if not isinstance(view_id, str) or not view_id or len(view_id) > 256 or any(ord(char) < 32 for char in view_id):
        _error("camera_invalid_view_id", relative)
    split = value.get("split", "train")
    if split not in ("train", "val", "test"):
        _error("camera_invalid_split", split)
    result = dict(value, view_id=view_id, image_path=relative, width=width, height=height,
                  sha256=digest, K=k, source_K=source_k,
                  source_pixel_coordinates=source_pixels, pixel_coordinates="pixel_border",
                  source_c2w_opencv=pose,
                  c2w_opencv=canonical_pose, split=split,
                  projection_model="PINHOLE", distortion=[], undistorted=True)
    result.pop("infer_extension", None)
    return result


def _finish(data, root, adapter, options=None, cancelled=None):
    options = options or {}
    source = data.get("source", {})
    if (not isinstance(source, dict) or source.get("handedness", "right") not in ("right", "right-handed")
            or "left-handed" in str(source.get("world_axes", "")).lower()):
        _error("camera_unsupported_axes", "source world handedness")
    scale = _scale(options.get("scale", data.get("scale")))
    if options.get("scale") and data.get("scale"):
        original_scale = _scale(data["scale"])
        if (original_scale["meters_per_unit"] is not None and
                original_scale["meters_per_unit"] != scale["meters_per_unit"]):
            _error("camera_conflicting_units", "source and explicit unit declarations disagree")
    transforms = data.get("transforms", {})
    source_to = _sim3(transforms.get("source_to_canonical", {"s": scale["meters_per_unit"] or 1.}),
                       "source", "canonical")
    if source_to["input_frame"] != "source" or source_to["output_frame"] != "canonical":
        _error("camera_invalid_transform_chain")
    if scale["meters_per_unit"] is not None and not math.isclose(source_to["s"], scale["meters_per_unit"], rel_tol=1e-12):
        _error("camera_conflicting_units", "source_to_canonical scale disagrees with meters_per_unit")
    source_to["applied"] = True
    training = _sim3(transforms.get("canonical_to_training"), "canonical", "training")
    if training["input_frame"] != "canonical" or training["output_frame"] != "training":
        _error("camera_invalid_transform_chain")
    normalization = options.get("normalization", data.get("normalization", {"mode": "auto"}))
    if normalization.get("mode") not in ("auto", "none", "explicit"):
        _error("camera_invalid_normalization")
    if normalization["mode"] == "none":
        training = _sim3({"mode": "explicit"}, "canonical", "training")
    policy = options.get("pose_policy", data.get("pose_policy", "locked"))
    if policy not in ("solve", "locked"):
        _error("camera_unsupported_pose_policy", policy)
    raw_views = data.get("views", [])
    if not isinstance(raw_views, list) or any(not isinstance(value, dict) for value in raw_views):
        _error("camera_invalid_manifest", "views")
    if transforms.get("source_to_canonical", {}).get("applied") and any(
            "source_c2w_opencv" not in view for view in raw_views):
        _error("camera_ambiguous_applied_transform")
    views = [_view(root, value, source_to, cancelled) for value in raw_views]
    if not views or not any(view["split"] == "train" for view in views):
        _error("camera_no_training_views")
    if len({view["view_id"] for view in views}) != len(views) or len({view["image_path"] for view in views}) != len(views):
        _error("camera_duplicate_view")
    splits = {name: [view["view_id"] for view in views if view["split"] == name] for name in ("train", "val", "test")}
    if data.get("splits") is not None and data["splits"] != splits:
        _error("camera_split_membership_mismatch")
    initialization = _initialization(root, options.get("initialization", data.get("initialization")), cancelled)
    result = dict(data, schema=SCHEMA, adapter_version=ADAPTER_VERSION, pose_policy=policy,
                  scale=scale, normalization=normalization,
                  transforms={"source_to_canonical": source_to, "canonical_to_training": training},
                  views=views, splits=splits,
                  initialization=initialization,
                  source=dict(data.get("source", {})))
    result["source"].setdefault("adapter", adapter)
    result["source"].setdefault("adapter_version", ADAPTER_VERSION)
    result["frames"] = dict(data.get("frames", {}), camera_axes="opencv", handedness="right")
    result["warnings"] = []
    by_hash = {}
    for view in views:
        by_hash.setdefault(view["sha256"], []).append(view)
    for digest, group in by_hash.items():
        if len({view["split"] for view in group}) > 1:
            result["warnings"].append({"code": "camera_cross_split_duplicate_content", "sha256": digest,
                                       "view_ids": [view["view_id"] for view in group]})
    result["independent_test"] = bool(splits["test"]) and not result["warnings"]
    return refresh_dataset_identity(result)


def refresh_dataset_identity(data):
    """Hash full semantic/data-byte identity after a controlled derived change.

    The caller must subsequently use validate_camera_dataset before training;
    this helper does not turn unchecked dictionaries into admitted datasets.
    """
    result = copy.deepcopy(data)
    result.pop("data_identity", None)
    identity = json.dumps(result, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    result["data_identity"] = hashlib.sha256(identity).hexdigest()
    return result


def _point_records(path, format_name, cancelled=None):
    if format_name == "colmap_bin":
        with path.open("rb") as stream:
            count, = _unpack(stream, "<Q")
            end = path.stat().st_size
            if count > end // 51:
                _error("camera_invalid_colmap_count", path.name)
            for _ in range(count):
                if cancelled and cancelled():
                    _error("camera_import_cancelled")
                record = _unpack(stream, "<QdddBBBd")
                track, = _unpack(stream, "<Q")
                if stream.tell() + track * 8 > end:
                    _error("camera_truncated_colmap", path.name)
                stream.seek(track * 8, 1)
                yield list(record[1:4]), [value / 255. for value in record[4:7]]
            if stream.read(1):
                _error("camera_invalid_colmap_payload", path.name)
    elif format_name == "colmap_txt":
        with path.open(encoding="utf-8-sig") as stream:
            for line in stream:
                if cancelled and cancelled():
                    _error("camera_import_cancelled")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                fields = line.split()
                try:
                    if len(fields) < 8 or (len(fields) - 8) % 2:
                        raise ValueError()
                    xyz = [float(x) for x in fields[1:4]]
                    rgb = [int(x) / 255. for x in fields[4:7]]
                    if any(x < 0 or x > 1 for x in rgb):
                        raise ValueError()
                except ValueError:
                    _error("camera_invalid_colmap_payload", path.name)
                yield xyz, rgb
    elif format_name == "ply":
        from plyfile import PlyData
        try:
            vertices = PlyData.read(str(path))["vertex"]
            names = vertices.data.dtype.names
            if not all(name in names for name in ("x", "y", "z")):
                _error("camera_invalid_initialization", "PLY requires x/y/z")
            rgb_present = all(name in names for name in ("red", "green", "blue"))
            for vertex in vertices.data:
                if cancelled and cancelled():
                    _error("camera_import_cancelled")
                xyz = [float(vertex[name]) for name in ("x", "y", "z")]
                rgb = [float(vertex[name]) / 255. for name in ("red", "green", "blue")] if rgb_present else [.5] * 3
                yield xyz, rgb
        except (KeyError, ValueError, OSError) as error:
            if isinstance(error, CameraDatasetError):
                raise
            _error("camera_invalid_initialization", str(error))
    else:
        _error("camera_unsupported_initialization", format_name)


def _initialization(root, value, cancelled=None):
    value = copy.deepcopy(value or {"kind": "random"})
    kind = value.get("kind", "random")
    if kind not in ("random", "external_point_cloud", "sfm_sparse"):
        _error("camera_unsupported_initialization", kind)
    frame = value.get("frame", "source")
    if frame not in ("source", "canonical"):
        _error("camera_invalid_initialization", "frame")
    priors = value.get("priors", [])
    if not isinstance(priors, list) or any(not isinstance(x, str) for x in priors):
        _error("camera_invalid_initialization", "priors")
    if any(name in ("evaluation_geometry", "ground_truth", "test_geometry") for name in priors):
        _error("camera_evaluation_prior_forbidden")
    if kind == "external_point_cloud" and "external_geometry" not in priors:
        priors.append("external_geometry")
    bounds = value.get("bounds")
    if bounds is not None:
        if not isinstance(bounds, (list, tuple)) or len(bounds) != 2 or any(
                not isinstance(row, (list, tuple)) or len(row) != 3 for row in bounds):
            _error("camera_invalid_initialization", "bounds")
        bounds = [[_number(x, "initialization bounds") for x in row] for row in bounds]
        if any(bounds[0][i] >= bounds[1][i] for i in range(3)):
            _error("camera_invalid_initialization", "degenerate bounds")
    for name in ("count", "seed"):
        if value.get(name) is not None and (type(value[name]) is not int or value[name] < (1 if name == "count" else 0)):
            _error("camera_invalid_initialization", name)
    value.update(kind=kind, frame=frame, priors=priors, bounds=bounds,
                 count=value.get("count"), seed=value.get("seed"))
    if kind == "random":
        if value.get("path"):
            _error("camera_invalid_initialization", "random initialization cannot consume a point cloud")
        return value
    path, relative = _relative(root, value.get("path"))
    digest = _hash(path, cancelled)
    if value.get("sha256", digest) != digest:
        _error("camera_initialization_hash_mismatch", relative)
    format_name = value.get("format", "ply" if path.suffix.lower() == ".ply" else "")
    count = 0
    low, high = [math.inf] * 3, [-math.inf] * 3
    for xyz, rgb in _point_records(path, format_name, cancelled):
        xyz = [_number(x, "initialization point") for x in xyz]
        if any(not math.isfinite(x) or x < 0 or x > 1 for x in rgb):
            _error("camera_invalid_initialization", "color")
        for i in range(3):
            low[i], high[i] = min(low[i], xyz[i]), max(high[i], xyz[i])
        count += 1
    if count == 0:
        if kind == "external_point_cloud":
            _error("camera_empty_initialization")
        return {"kind": "random", "frame": "source", "priors": [], "bounds": None,
                "count": None, "seed": None, "reason": "empty_sfm_sparse",
                "unused_source": {"path": relative, "sha256": digest}}
    if value.get("count") is not None and value["count"] != count:
        _error("camera_initialization_count_mismatch")
    value.update(path=relative, sha256=digest, format=format_name, count=count,
                 point_bounds=[low, high], color_policy="source_rgb_or_neutral_gray")
    return value


def read_initialization_points(dataset, root, cancelled=None):
    """Return (xyz, rgb) in canonical coordinates or None for random input.

    Points are float64 lists and colors are in [0, 1]. The training caller must
    apply canonical_to_training exactly once. No evaluation geometry is read.
    """
    initialization = dataset["initialization"]
    if initialization["kind"] == "random":
        return None
    path, _ = _relative(Path(root).resolve(), initialization["path"])
    if _hash(path, cancelled) != initialization["sha256"]:
        _error("camera_initialization_hash_mismatch")
    points, colors = [], []
    for xyz, rgb in _point_records(path, initialization["format"], cancelled):
        xyz = [_number(x, "initialization point") for x in xyz]
        points.append(xyz)
        colors.append(rgb)
    if initialization["frame"] == "source":
        points = apply_sim3(dataset["transforms"]["source_to_canonical"], points)
    return points, colors


def _blender(root):
    data = {"source": {"kind": "synthetic", "world_axes": "right-handed z-up",
                       "camera_axes": "blender", "files": []}, "views": []}
    for split in ("train", "val", "test"):
        path = root / ("transforms_" + split + ".json")
        if not path.is_file():
            if split == "train":
                _error("camera_missing_manifest", path.name)
            continue
        part = _read_json(path)
        data["source"]["files"].append({"path": path.name, "sha256": _hash(path)})
        if "scale" in part:
            if "scale" in data and data["scale"] != part["scale"]:
                _error("camera_conflicting_units", path.name)
            data["scale"] = part["scale"]
        frames = part.get("frames")
        if not isinstance(frames, list):
            _error("camera_invalid_manifest", path.name)
        for frame in frames:
            pose = _pose(frame.get("transform_matrix"), path.name)
            # Blender camera local Y/Z to OpenCV local Y/Z. This proper
            # rotation changes camera convention only, never the world axes.
            for row in pose[:3]:
                row[1] = -row[1]
                row[2] = -row[2]
            image_path, relative = _relative(root, frame.get("file_path"), True)
            from PIL import Image
            with Image.open(image_path) as image:
                width, height = image.size
            values = dict(part, **frame)
            if "K" in values:
                k = values["K"]
            else:
                fx = values.get("fl_x")
                fy = values.get("fl_y")
                if fx is None:
                    angle = _number(values.get("camera_angle_x"), "camera_angle_x")
                    if not 0 < angle < math.pi:
                        _error("camera_invalid_intrinsics", path.name)
                    fx = width / (2. * math.tan(angle / 2.))
                if fy is None:
                    if "camera_angle_y" in values:
                        angle = _number(values["camera_angle_y"], "camera_angle_y")
                        if not 0 < angle < math.pi:
                            _error("camera_invalid_intrinsics", path.name)
                        fy = height / (2. * math.tan(angle / 2.))
                    else:
                        fy = fx
                k = [[fx, values.get("skew", 0.), values.get("cx", width / 2.)],
                     [0., fy, values.get("cy", height / 2.)], [0., 0., 1.]]
            view = {"image_path": relative, "width": values.get("w", width),
                    "height": values.get("h", height), "K": k, "c2w_opencv": pose,
                    "split": split, "distortion": [values.get(name, 0.) for name in
                                                        ("k1", "k2", "k3", "k4", "p1", "p2")],
                    "projection_model": values.get("camera_model", "PINHOLE")}
            view["pixel_coordinates"] = values.get("pixel_coordinates", "opencv_integer")
            for key in ("view_id", "timestamp", "sensor_id"):
                if key in frame:
                    view[key] = frame[key]
            data["views"].append(view)
    return data


def _unpack(stream, layout):
    size = struct.calcsize(layout)
    payload = stream.read(size)
    if len(payload) != size:
        _error("camera_truncated_colmap")
    return struct.unpack(layout, payload)


def _quaternion_pose(values, translation):
    q = [_number(x, "COLMAP quaternion") for x in values]
    norm = sum(x * x for x in q)
    if abs(norm - 1.) > 2e-5:
        _error("camera_invalid_rotation", "COLMAP quaternion")
    q = [value / math.sqrt(norm) for value in q]
    w, x, y, z = q
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
         [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
         [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    return _invert_pose(_pose([r[i] + [_number(translation[i], "COLMAP translation")]
                              for i in range(3)] + [[0., 0., 0., 1.]]))


def _colmap_calibration(model, width, height, parameters):
    if model == "SIMPLE_PINHOLE":
        if len(parameters) != 3:
            _error("camera_invalid_intrinsics", model)
        fx, cx, cy = parameters
        fy = fx
    elif model == "PINHOLE":
        if len(parameters) != 4:
            _error("camera_invalid_intrinsics", model)
        fx, fy, cx, cy = parameters
    else:
        _error("camera_unsupported_projection", model)
    if type(width) is not int or type(height) is not int or width <= 0 or height <= 0:
        _error("camera_invalid_image_dimensions")
    return width, height, _intrinsics([[fx, 0., cx], [0., fy, cy], [0., 0., 1.]])


def _colmap(source, options):
    """Independent reader of COLMAP's documented text/little-endian format.

    See https://colmap.github.io/format.html. Only undistorted SIMPLE_PINHOLE
    and PINHOLE models enter P0. No reconstruction or model conversion runs.
    """
    source = source if source.is_dir() else source.parent
    candidates = [source, source / "sparse" / "0", source / "sparse"]
    model = next((path for path in candidates if any(
        all((path / (name + "." + extension)).is_file() for name in ("cameras", "images"))
        for extension in ("bin", "txt"))), None)
    if model is None:
        _error("camera_missing_colmap_model")
    root = next((path for path in [source, model, model.parent, model.parent.parent]
                 if (path / "images").is_dir()), None)
    if root is None:
        _error("camera_missing_images_directory")
    extension = "bin" if all((model / (name + ".bin")).is_file() for name in ("cameras", "images")) else "txt"
    cameras, records = {}, []
    camera_file, image_file = [model / (name + "." + extension) for name in ("cameras", "images")]
    if extension == "bin":
        with camera_file.open("rb") as stream:
            count, = _unpack(stream, "<Q")
            if count > camera_file.stat().st_size // 24:
                _error("camera_invalid_colmap_count")
            for _ in range(count):
                identifier, model_id, width, height = _unpack(stream, "<iiQQ")
                if model_id not in (0, 1):
                    _error("camera_unsupported_projection", "COLMAP model ID " + str(model_id))
                parameters = _unpack(stream, "<" + "d" * (3 if model_id == 0 else 4))
                if identifier in cameras:
                    _error("camera_duplicate_calibration", identifier)
                cameras[identifier] = _colmap_calibration("SIMPLE_PINHOLE" if model_id == 0 else "PINHOLE",
                                                        width, height, parameters)
            if stream.read(1):
                _error("camera_invalid_colmap_payload", camera_file.name)
        with image_file.open("rb") as stream:
            count, = _unpack(stream, "<Q")
            end = image_file.stat().st_size
            if count > end // 73:
                _error("camera_invalid_colmap_count")
            for _ in range(count):
                record = _unpack(stream, "<idddddddi")
                name = bytearray()
                while True:
                    char = stream.read(1)
                    if not char or len(name) > 65536:
                        _error("camera_invalid_image_path", image_file.name)
                    if char == b"\0":
                        break
                    name.extend(char)
                try:
                    name = name.decode("utf-8")
                except UnicodeError:
                    _error("camera_invalid_image_path", image_file.name)
                observations, = _unpack(stream, "<Q")
                if stream.tell() + observations * 24 > end:
                    _error("camera_truncated_colmap", image_file.name)
                stream.seek(observations * 24, 1)
                records.append((record[0], record[1:5], record[5:8], record[8], name))
            if stream.read(1):
                _error("camera_invalid_colmap_payload", image_file.name)
    else:
        with camera_file.open(encoding="utf-8-sig") as stream:
            for line in stream:
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                fields = line.split()
                try:
                    identifier, width, height = int(fields[0]), int(fields[2]), int(fields[3])
                    parameters = [float(x) for x in fields[4:]]
                    model_name = fields[1]
                except (ValueError, IndexError):
                    _error("camera_invalid_colmap_payload", camera_file.name)
                if identifier in cameras:
                    _error("camera_duplicate_calibration", identifier)
                cameras[identifier] = _colmap_calibration(model_name, width, height, parameters)
        with image_file.open(encoding="utf-8-sig") as stream:
            while True:
                line = stream.readline()
                if not line:
                    break
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                fields = line.strip().split(maxsplit=9)
                try:
                    if len(fields) != 10:
                        raise ValueError()
                    records.append((int(fields[0]), [float(x) for x in fields[1:5]],
                                    [float(x) for x in fields[5:8]], int(fields[8]), fields[9]))
                except ValueError:
                    _error("camera_invalid_colmap_payload", image_file.name)
                observations = stream.readline()
                if not observations:
                    _error("camera_truncated_colmap", image_file.name)
                if len(observations.split()) % 3:
                    _error("camera_invalid_colmap_payload", image_file.name)
    data = {"source": {"kind": "external_sfm", "camera_axes": "opencv",
                       "world_axes": options.get("world_axes", "source-defined right-handed"),
                       "format": "colmap_" + extension,
                       "files": [{"path": path.relative_to(root).as_posix(), "sha256": _hash(path)}
                                 for path in (camera_file, image_file)]}, "views": []}
    identifiers = set()
    split_by_path = options.get("split_by_path", {})
    for identifier, quaternion, translation, camera_id, filename in records:
        if identifier in identifiers:
            _error("camera_duplicate_view", identifier)
        identifiers.add(identifier)
        if camera_id not in cameras:
            _error("camera_missing_calibration", camera_id)
        width, height, k = cameras[camera_id]
        relative = "images/" + filename
        data["views"].append({"image_path": relative, "width": width, "height": height,
                              "K": k, "c2w_opencv": _quaternion_pose(quaternion, translation),
                              "pixel_coordinates": "pixel_border",
                              "source_camera_id": str(camera_id), "source_image_id": str(identifier),
                              "split": split_by_path.get(relative, "train")})
    points = model / ("points3D." + extension)
    if points.is_file():
        data["initialization"] = {"kind": "sfm_sparse", "path": points.relative_to(root).as_posix(),
                                  "frame": "source", "format": "colmap_" + extension,
                                  "sha256": _hash(points), "priors": ["sfm_sparse"]}
    return data, root


def _research(path):
    """Adapter for the documented research-export camera record, not an
    experiment-specific runner. Nominal object dimensions, material metadata,
    evaluation geometry and formula strings are deliberately not consulted.
    """
    original = _read_json(path)
    conventions = original.get("conventions", {})
    unit = conventions.get("units")
    if not isinstance(unit, str) or unit.lower() not in _UNITS:
        _error("camera_missing_scale_evidence", "research conventions.units")
    if not math.isclose(_UNITS[unit.lower()], 1., rel_tol=1e-12):
        _error("camera_conflicting_units", "research *_m pose requires meter declaration")
    pixels = conventions.get("pixels", "")
    if "top-left image border" in pixels or pixels == "pixel_border":
        pixel_coordinates = "pixel_border"
    elif pixels in ("opencv_integer", "integer pixel centers"):
        pixel_coordinates = "opencv_integer"
    else:
        _error("camera_unsupported_pixel_coordinates", "research conventions.pixels")
    cameras = original.get("cameras")
    if not isinstance(cameras, list):
        _error("camera_invalid_manifest", path.name)
    data = {"source": {"kind": "synthetic", "world_axes": conventions.get("world", "source-defined right-handed"),
                       "adapter": "research_json", "files": [{"path": path.name, "sha256": _hash(path)}],
                       "conventions": conventions},
            "scale": {"status": "source_declared", "source_unit": unit, "meters_per_unit": 1.,
                      "evidence": [{"kind": "source_declaration", "field": "conventions.units", "value": unit}]},
            "views": []}
    for camera in cameras:
        view = {"image_path": camera.get("file_name", camera.get("file_path")),
                "infer_extension": True, "K": camera.get("K"), "split": camera.get("split", "train"),
                "pixel_coordinates": pixel_coordinates}
        if "camera_to_world_blender_m" in camera:
            view["c2w_blender"] = camera["camera_to_world_blender_m"]
        if "world_to_camera_opencv_m" in camera:
            view["w2c_opencv"] = camera["world_to_camera_opencv_m"]
        for name in ("view_id", "width", "height", "timestamp", "sensor_id"):
            if name in camera:
                view[name] = camera[name]
        data["views"].append(view)
    return data


def validate_camera_dataset(data, root, verify_images=True, cancelled=None):
    """Revalidate a normalized manifest without repeating its transforms.

    P0 deliberately requires byte/hash verification at this seam. Passing
    verify_images=False is unsupported, rather than creating a weaker training
    admission route with unchecked paths, dimensions or changed images.
    """
    if not verify_images:
        _error("camera_image_verification_required")
    if data.get("schema") != SCHEMA:
        _error("camera_unsupported_schema", data.get("schema"))
    result = _finish(copy.deepcopy(data), Path(root).resolve(),
                     data.get("source", {}).get("adapter", "generic_json"), cancelled=cancelled)
    if data.get("data_identity") and result["data_identity"] != data["data_identity"]:
        _error("camera_data_identity_mismatch")
    return result


def camera_dataset_root(source, adapter="auto"):
    """Return the image/reference root used by the selected adapter.

    It is intentionally returned separately from the content-addressed JSON,
    so copying a managed dataset does not change its identity.
    """
    source = Path(source).resolve()
    directory = source if source.is_dir() else source.parent
    if adapter == "auto" and source.is_file() and source.suffix.lower() == ".json":
        return directory
    if adapter in ("colmap", "colmap_text", "colmap_binary") or (adapter == "auto" and any(
            all((directory / (name + "." + extension)).is_file() for name in ("cameras", "images"))
            for extension in ("bin", "txt"))):
        root = next((path for path in (directory, directory.parent, directory.parent.parent)
                     if (path / "images").is_dir()), None)
        if root is None:
            _error("camera_missing_images_directory")
        return root
    return directory


def write_camera_dataset(data, path):
    """Atomically publish JSON metadata only; source image bytes are read-only."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(data, stream, ensure_ascii=False, allow_nan=False, sort_keys=True, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()


def _load_camera_dataset(source, adapter="auto", options=None, cancelled=None):
    """Read image bytes and calibration through an explicit source adapter.

    The returned dictionary is JSON-safe and contains no absolute image paths.
    Missing/unsupported input fails closed; this function never launches SfM.
    """
    source = Path(source).resolve()
    options = options or {}
    if not isinstance(options, dict):
        _error("camera_invalid_manifest", "options")
    if adapter == "auto":
        if source.is_dir() and (source / MANIFEST_NAME).is_file():
            adapter = "generic_json"
        elif (source.is_dir() and (source / "transforms_train.json").is_file()) or source.name.startswith("transforms_"):
            adapter = "blender_transforms"
        elif source.is_file() and source.suffix.lower() == ".json":
            selected = _read_json(source)
            adapter = "research_json" if "conventions" in selected and isinstance(selected.get("cameras"), list) else "generic_json"
        elif any((path / ("cameras." + extension)).is_file()
                                     for path in (source if source.is_dir() else source.parent,
                                                  source / "sparse" / "0", source / "sparse")
                                     for extension in ("bin", "txt")):
            adapter = "colmap"
        else:
            adapter = "generic_json"
    if adapter == "blender_transforms":
        root = source if source.is_dir() else source.parent
        return _finish(_blender(root), root, adapter, options, cancelled)
    if adapter == "research_json":
        if source.is_dir():
            source = source / "cameras_metric.json"
        return _finish(_research(source), source.parent, adapter, options, cancelled)
    if adapter in ("colmap", "colmap_text", "colmap_binary"):
        data, root = _colmap(source, options)
        return _finish(data, root, "colmap", options, cancelled)
    if adapter != "generic_json":
        _error("camera_unsupported_adapter", adapter)
    if source.is_dir():
        source = source / MANIFEST_NAME
    data = _read_json(source)
    if data.get("schema") != SCHEMA:
        _error("camera_unsupported_schema", data.get("schema"))
    if data.get("data_identity"):
        verified = validate_camera_dataset(data, source.parent, cancelled=cancelled)
        if not options:
            return verified
        data = verified
    return _finish(data, source.parent, adapter, options, cancelled)


def load_camera_dataset(source, adapter="auto", options=None, cancelled=None):
    """Validate/normalize a source without modifying bytes or solving poses.

    Throws CameraDatasetError with a machine-facing code plus source detail.
    No parse/model failure changes the selected policy or launches COLMAP.
    """
    try:
        return _load_camera_dataset(source, adapter, options, cancelled)
    except CameraDatasetError:
        raise
    except FileNotFoundError as error:
        _error("camera_missing_file", str(error))
    except (OSError, TypeError, KeyError, IndexError, OverflowError, ValueError, struct.error) as error:
        _error("camera_invalid_manifest", str(error))
