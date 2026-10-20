"""Explicit versioned trajectory/image association for fixed-camera admission.

This adapter never solves, interpolates, smooths or corrects camera poses. It
does not infer calibration, units, rolling shutter, rig relationships or CRS.
"""
import copy
import bisect
import csv
from decimal import Decimal, InvalidOperation, localcontext
import hashlib
import math
import os
from pathlib import Path, PurePosixPath

from native.backend import camera_dataset


SCHEMA = "gsw.trajectory_dataset/1"
MANIFEST_NAME = "trajectory_dataset.json"
_UNITS = {"m": 1., "cm": .01, "mm": .001}


def _fail(code, detail=""):
    raise camera_dataset.CameraDatasetError(code, str(detail))


def _cancel(cancelled):
    if cancelled and cancelled():
        _fail("camera_import_cancelled")


def _file(root, relative):
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative or "\x00" in relative:
        _fail("trajectory_invalid_path", relative)
    name = PurePosixPath(relative)
    if name.is_absolute() or ".." in name.parts:
        _fail("trajectory_path_outside_dataset", relative)
    path = root
    for part in name.parts:
        path = path / part
        info = path.lstat()
        if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            _fail("trajectory_reparse_path_unsupported", relative)
    if not path.is_file():
        _fail("trajectory_missing_file", relative)
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        _fail("trajectory_path_outside_dataset", relative)
    return path


def _time(value, name):
    if isinstance(value, bool) or not isinstance(value, (str, int, Decimal)):
        _fail("trajectory_timestamp_requires_decimal", name)
    lexical = str(value)
    if len(lexical) > 256 or not lexical.strip() or lexical != lexical.strip():
        _fail("trajectory_invalid_timestamp", name)
    try:
        number = Decimal(lexical)
    except InvalidOperation:
        _fail("trajectory_invalid_timestamp", name)
    if not number.is_finite() or abs(number.adjusted()) > 308:
        _fail("trajectory_invalid_timestamp", name)
    return number, lexical


def _number(value, name):
    if isinstance(value, bool):
        _fail("trajectory_invalid_number", name)
    try:
        result = float(value)
    except (ValueError, TypeError, OverflowError):
        _fail("trajectory_invalid_number", name)
    if not math.isfinite(result):
        _fail("trajectory_invalid_number", name)
    return result


def _hash(path, cancelled):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            _cancel(cancelled)
            digest.update(block)
    return digest.hexdigest()


def _quaternion_pose(values, tolerance):
    translation = [_number(value, "translation") for value in values[:3]]
    qx, qy, qz, qw = [_number(value, "quaternion") for value in values[3:]]
    norm = math.hypot(qx, qy, qz, qw)
    if norm <= 0 or abs(norm - 1.) > tolerance:
        _fail("trajectory_nonunit_quaternion", "norm=" + str(norm))
    qx, qy, qz, qw = [value / norm for value in (qx, qy, qz, qw)]
    matrix = [[1. - 2. * (qy * qy + qz * qz), 2. * (qx * qy - qz * qw), 2. * (qx * qz + qy * qw)],
              [2. * (qx * qy + qz * qw), 1. - 2. * (qx * qx + qz * qz), 2. * (qy * qz - qx * qw)],
              [2. * (qx * qz - qy * qw), 2. * (qy * qz + qx * qw), 1. - 2. * (qx * qx + qy * qy)]]
    return [matrix[index] + [translation[index]] for index in range(3)] + [[0., 0., 0., 1.]], norm


def _read_tum(path, trajectory, cancelled):
    if trajectory.get("pose_convention") != "c2w" or trajectory.get("camera_axes") != "opencv":
        _fail("trajectory_conflicting_pose_convention", "TUM requires c2w/OpenCV optical camera")
    tolerance = _number(trajectory.get("quaternion_norm_tolerance"), "quaternion_norm_tolerance")
    if tolerance < 0 or tolerance > 1e-3:
        _fail("trajectory_invalid_quaternion_tolerance")
    output, timestamps = [], set()
    with path.open(encoding="utf-8-sig") as stream:
        for line_number, line in enumerate(stream, 1):
            _cancel(cancelled)
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            parts = stripped.split()
            if len(parts) != 8:
                _fail("trajectory_invalid_pose_row", line_number)
            timestamp, lexical = _time(parts[0], "pose " + str(line_number))
            if timestamp in timestamps:
                _fail("trajectory_duplicate_timestamp", lexical)
            timestamps.add(timestamp)
            pose, norm = _quaternion_pose(parts[1:], tolerance)
            output.append({"timestamp": timestamp, "timestamp_s": lexical, "pose": pose,
                           "pose_key": "c2w_opencv", "source_row": line_number,
                           "quaternion_norm": norm, "quaternion_normalized_rounding_only": norm != 1.})
    if not output:
        _fail("trajectory_no_poses")
    return output


def _read_csv(path, trajectory, cancelled):
    convention, axes = trajectory.get("pose_convention"), trajectory.get("camera_axes")
    if convention not in ("c2w", "w2c") or axes not in ("opencv", "blender"):
        _fail("trajectory_conflicting_pose_convention")
    tolerance = _number(trajectory.get("quaternion_norm_tolerance"), "quaternion_norm_tolerance")
    if tolerance < 0 or tolerance > 1e-3:
        _fail("trajectory_invalid_quaternion_tolerance")
    output, timestamps = [], set()
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream, strict=True)
        if next(reader, None) != ["timestamp_s", "tx", "ty", "tz", "qx", "qy", "qz", "qw"]:
            _fail("trajectory_csv_header_mismatch")
        for row in reader:
            _cancel(cancelled)
            if len(row) != 8:
                _fail("trajectory_invalid_pose_row", reader.line_num)
            timestamp, lexical = _time(row[0], "pose " + str(reader.line_num))
            if timestamp in timestamps:
                _fail("trajectory_duplicate_timestamp", lexical)
            timestamps.add(timestamp)
            pose, norm = _quaternion_pose(row[1:], tolerance)
            output.append({"timestamp": timestamp, "timestamp_s": lexical, "pose": pose,
                           "pose_key": convention + "_" + axes, "source_row": reader.line_num,
                           "quaternion_norm": norm, "quaternion_normalized_rounding_only": norm != 1.})
    if not output:
        _fail("trajectory_no_poses")
    return output


def _read_pose_json(path, trajectory, cancelled):
    convention, axes = trajectory.get("pose_convention"), trajectory.get("camera_axes")
    if convention not in ("c2w", "w2c") or axes not in ("opencv", "blender"):
        _fail("trajectory_conflicting_pose_convention")
    expected = convention + "_" + axes
    document = camera_dataset._read_json(path)
    if document.get("schema") != "gsw.pose_trajectory/1" or set(document) != {"schema", "poses"}:
        _fail("trajectory_unsupported_pose_schema")
    rows = document.get("poses")
    if not isinstance(rows, list) or not rows:
        _fail("trajectory_no_poses")
    output, timestamps = [], set()
    for index, row in enumerate(rows, 1):
        _cancel(cancelled)
        if not isinstance(row, dict) or set(row) != {"timestamp_s", expected}:
            _fail("trajectory_conflicting_pose_convention", index)
        timestamp, lexical = _time(row["timestamp_s"], "pose " + str(index))
        if timestamp in timestamps:
            _fail("trajectory_duplicate_timestamp", lexical)
        timestamps.add(timestamp)
        pose = camera_dataset._pose(row[expected], "trajectory pose " + str(index))
        output.append({"timestamp": timestamp, "timestamp_s": lexical, "pose": pose,
                       "pose_key": expected, "source_row": index, "quaternion_norm": None,
                       "quaternion_normalized_rounding_only": False})
    return output


def _calibrations(manifest):
    if "calibration" in manifest and "calibrations" in manifest:
        _fail("trajectory_ambiguous_calibration")
    if "calibration" in manifest:
        values = [copy.deepcopy(manifest["calibration"])]
        if isinstance(values[0], dict):
            values[0].setdefault("id", "default")
    else:
        values = manifest.get("calibrations")
    if not isinstance(values, list) or not values or any(not isinstance(value, dict) for value in values):
        _fail("trajectory_calibration_required")
    result = {}
    for value in values:
        identifier = value.get("id")
        if not isinstance(identifier, str) or not identifier or identifier in result:
            _fail("trajectory_ambiguous_calibration", identifier)
        if (value.get("undistorted") is not True or value.get("projection_model") not in ("PINHOLE", "SIMPLE_PINHOLE")
                or value.get("pixel_coordinates") not in ("opencv_integer", "pixel_border")
                or not isinstance(value.get("distortion"), list)
                or any(abs(_number(number, "distortion")) > 1e-12 for number in value["distortion"])):
            _fail("trajectory_invalid_calibration", identifier)
        if any(type(value.get(name)) is not int or value[name] <= 0 for name in ("width", "height")):
            _fail("trajectory_invalid_calibration", identifier)
        camera_dataset._intrinsics(value.get("K"))
        result[identifier] = copy.deepcopy(value)
    return result


def _image_calibration(image, calibrations):
    identifier = image.get("calibration_id")
    if identifier is None and len(calibrations) == 1:
        identifier = next(iter(calibrations))
    if identifier not in calibrations:
        _fail("trajectory_unknown_calibration", image.get("image_path"))
    value = copy.deepcopy(calibrations[identifier])
    value.pop("id", None)
    return identifier, value


def _images(manifest, root, cancelled):
    rows = manifest.get("images")
    if not isinstance(rows, list) or not rows:
        _fail("trajectory_no_images")
    output, timestamps, identifiers, paths, sensors = [], set(), set(), set(), set()
    allowed = {"image_path", "timestamp_s", "view_id", "split", "calibration_id", "sensor_id"}
    for original in rows:
        _cancel(cancelled)
        if not isinstance(original, dict) or set(original) - allowed:
            _fail("trajectory_invalid_image_fields")
        row = copy.deepcopy(original)
        if row.get("split") not in ("train", "val", "test"):
            _fail("trajectory_explicit_split_required", row.get("image_path"))
        path = _file(root, row.get("image_path"))
        relative = path.relative_to(root).as_posix()
        key = os.path.normcase(str(path.resolve()))
        if key in paths:
            _fail("trajectory_duplicate_image", relative)
        paths.add(key)
        row["image_path"] = relative
        identifier = row.get("view_id") or "v-" + hashlib.sha256(relative.encode("utf-8")).hexdigest()[:24]
        if not isinstance(identifier, str) or not identifier or len(identifier) > 256 or any(ord(char) < 32 for char in identifier):
            _fail("trajectory_invalid_view_id", relative)
        if identifier in identifiers:
            _fail("trajectory_duplicate_view_id", identifier)
        identifiers.add(identifier)
        row["view_id"] = identifier
        timestamp, lexical = _time(row.get("timestamp_s"), identifier)
        if timestamp in timestamps:
            _fail("trajectory_duplicate_timestamp", lexical)
        timestamps.add(timestamp)
        row["timestamp_s"] = lexical
        sensor = row.get("sensor_id")
        if sensor is not None:
            if not isinstance(sensor, str) or not sensor.strip():
                _fail("trajectory_invalid_sensor_id", identifier)
            sensors.add(sensor)
        output.append(row)
    if len(sensors) > 1:
        _fail("trajectory_multiple_sensors_unsupported")
    return output


def _read_trajectory_dataset(manifest_path, cancelled=None):
    original = Path(os.path.abspath(str(manifest_path)))
    if original.is_dir():
        original = original / MANIFEST_NAME
    root = original.parent
    if root.is_symlink() or getattr(root.lstat(), "st_file_attributes", 0) & 0x400:
        _fail("trajectory_reparse_path_unsupported", str(root))
    manifest_path = _file(root, original.name)
    manifest = camera_dataset._read_json(manifest_path)
    if manifest.get("schema") != SCHEMA:
        _fail("trajectory_unsupported_schema", manifest.get("schema"))
    unsupported = set(manifest) - {"schema", "trajectory", "images", "association", "calibration", "calibrations",
                                   "source", "notes", "initialization", "normalization"}
    if unsupported:
        _fail("trajectory_unsupported_fields", ",".join(sorted(unsupported)))
    trajectory = manifest.get("trajectory")
    if not isinstance(trajectory, dict) or trajectory.get("format") not in ("tum", "gsw_pose_csv_v1", "gsw_pose_json_v1"):
        _fail("trajectory_unsupported_format")
    unsupported = set(trajectory) - {"path", "format", "pose_convention", "camera_axes", "source_unit", "quaternion_norm_tolerance"}
    if unsupported:
        _fail("trajectory_unsupported_fields", ",".join(sorted(unsupported)))
    unit = trajectory.get("source_unit")
    if unit not in ("m", "cm", "mm", "unknown"):
        _fail("trajectory_unit_declaration_required")
    trajectory_path = _file(root, trajectory.get("path"))
    reader = {"tum": _read_tum, "gsw_pose_csv_v1": _read_csv, "gsw_pose_json_v1": _read_pose_json}[trajectory["format"]]
    poses = reader(trajectory_path, trajectory, cancelled)
    association = manifest.get("association")
    if (not isinstance(association, dict) or association.get("mode") not in ("exact", "nearest_one_to_one")
            or association.get("missing") not in ("reject", "drop")):
        _fail("trajectory_invalid_association")
    if set(association) - {"mode", "max_difference_s", "missing"}:
        _fail("trajectory_unsupported_fields", ",".join(sorted(set(association) - {"mode", "max_difference_s", "missing"})))
    difference, difference_lexical = _time(association.get("max_difference_s"), "max_difference_s")
    if difference < 0:
        _fail("trajectory_invalid_association")
    calibrations = _calibrations(manifest)
    images = _images(manifest, root, cancelled)
    by_time = {pose["timestamp"]: pose for pose in poses}
    ordered_times = sorted(by_time)
    used_poses = set()
    views, matches, dropped = [], [], []
    for image in images:
        _cancel(cancelled)
        timestamp, lexical = _time(image.get("timestamp_s"), "image timestamp_s")
        image_path = _file(root, image.get("image_path"))
        calibration_id, calibration = _image_calibration(image, calibrations)
        from PIL import Image
        try:
            with Image.open(image_path) as pixels:
                actual_size = pixels.size
        except (OSError, ValueError) as error:
            _fail("trajectory_invalid_image", str(image_path) + ": " + str(error))
        if actual_size != (calibration["width"], calibration["height"]):
            _fail("trajectory_image_dimensions_mismatch", image["image_path"])
        selected_time = timestamp if timestamp in by_time else None
        if association["mode"] == "nearest_one_to_one":
            position = bisect.bisect_left(ordered_times, timestamp)
            neighbors = ordered_times[max(0, position - 1):min(len(ordered_times), position + 1)]
            minimum = min(abs(item - timestamp) for item in neighbors)
            nearest = [item for item in neighbors if abs(item - timestamp) == minimum]
            if minimum > difference:
                selected_time = None
            elif len(nearest) != 1:
                _fail("trajectory_ambiguous_association", image.get("image_path"))
            else:
                selected_time = nearest[0]
        if selected_time is None:
            if association["missing"] == "reject":
                _fail("trajectory_unmatched_image", image.get("image_path"))
            relative = image["image_path"]
            dropped.append({"view_id": image.get("view_id") or "v-" + hashlib.sha256(relative.encode("utf-8")).hexdigest()[:24],
                            "image_path": relative, "timestamp_s": lexical, "split": image.get("split"),
                            "reason": "no_pose_within_declared_association"})
            continue
        if selected_time in used_poses:
            _fail("trajectory_pose_reused", image.get("image_path"))
        used_poses.add(selected_time)
        pose = by_time[selected_time]
        view = dict(copy.deepcopy(calibration), **copy.deepcopy(image))
        view["calibration_id"] = calibration_id
        view[pose["pose_key"]] = pose["pose"]
        view["timestamp_s"] = lexical
        views.append(view)
        matches.append({"image_path": image["image_path"], "view_id": image.get("view_id"),
                        "image_timestamp_s": lexical, "pose_timestamp_s": pose["timestamp_s"],
                        "difference_s": str(abs(timestamp - pose["timestamp"])),
                        "pose_source_row": pose["source_row"], "quaternion_norm": pose["quaternion_norm"],
                        "quaternion_normalized_rounding_only": pose["quaternion_normalized_rounding_only"]})
    source_files = [{"path": original.name, "sha256": _hash(manifest_path, cancelled)},
                    {"path": trajectory["path"], "sha256": _hash(trajectory_path, cancelled)}]
    scale = ({"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None, "evidence": []}
             if unit == "unknown" else
             {"status": "source_declared", "source_unit": unit, "meters_per_unit": _UNITS[unit],
              "evidence": [{"kind": "trajectory_unit_declaration", "source_unit": unit,
                            "manifest_sha256": source_files[0]["sha256"]}]})
    raw = {"schema": camera_dataset.SCHEMA, "pose_policy": "locked", "scale": scale, "views": views,
           "source": {"kind": "trajectory", "adapter": "trajectory_dataset", "adapter_version": 1,
                      "world_axes": "right-handed local Cartesian", "files": source_files,
                      "trajectory": dict(copy.deepcopy(trajectory), pose_count=len(poses)),
                      "calibrations": copy.deepcopy(list(calibrations.values())),
                      "association": {"mode": association["mode"], "max_difference_s": difference_lexical,
                                      "missing": association["missing"], "matches": matches, "dropped": dropped},
                      "limitations": {"pose_solving": False, "interpolation": False, "drift_correction": False,
                                      "rig": False, "crs": False}}}
    for name in ("initialization", "normalization"):
        if name in manifest:
            raw[name] = copy.deepcopy(manifest[name])
    if "source" in manifest:
        raw["source"]["user_metadata"] = copy.deepcopy(manifest["source"])
    if "notes" in manifest:
        raw["source"]["notes"] = copy.deepcopy(manifest["notes"])
    return raw, root


def read_trajectory_dataset(manifest_path, cancelled=None):
    """Read a named format and exact image association; return raw input/root."""
    try:
        with localcontext() as context:
            context.prec = 1024
            return _read_trajectory_dataset(manifest_path, cancelled)
    except camera_dataset.CameraDatasetError:
        raise
    except FileNotFoundError as error:
        _fail("trajectory_missing_file", error)
    except csv.Error as error:
        _fail("trajectory_invalid_pose_row", error)
    except (OSError, ValueError, TypeError, KeyError, IndexError, OverflowError) as error:
        _fail("trajectory_invalid_manifest", error)
