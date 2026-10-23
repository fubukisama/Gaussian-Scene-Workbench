# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Conservative mask-derived subsets, never reconstruction or Clean-GS reproduction.

The sole public interface is ``postprocess(request, output_directory,
cancelled=None)``. It reads hash-bound model/camera/mask evidence and publishes
a fresh atomic directory. No RGB photos, Torch, renderer or optimizer are used.
"""
from collections import OrderedDict
import copy
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil

import numpy as np
from PIL import Image
from plyfile import PlyData, PlyListProperty

from native.backend import camera_dataset, camera_scale_export
from native.backend.atomic_file import atomic_sibling_directory


class QualityPostprocessError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _fail(code, detail=""):
    raise QualityPostprocessError(code, detail)


def _poll(cancelled):
    if cancelled and cancelled():
        _fail("cleanup_cancelled")


def _sha(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True, allow_nan=False).encode("utf-8")).hexdigest()


def _json_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _fail("cleanup_duplicate_json_key", key)
        result[key] = value
    return result


def _no_links(path):
    for part in (Path(path).absolute(), *Path(path).absolute().parents):
        if part.is_symlink() or part.exists() and getattr(part.stat(), "st_file_attributes", 0) & 0x400:
            _fail("cleanup_link_refused")


def _read_json(path, limit=16 * 1024 * 1024):
    _no_links(path)
    if not Path(path).is_file() or Path(path).stat().st_size > limit:
        _fail("cleanup_metadata_size_or_type")
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=_json_pairs,
            parse_constant=lambda value: _fail("cleanup_nonfinite_metadata", value))


def _write_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2)
        stream.write("\n")
        stream.flush(); os.fsync(stream.fileno())


def _fields(value, required, optional=()):
    if (not isinstance(value, dict) or set(required) - set(value)
            or set(value) - set(required) - set(optional)):
        _fail("cleanup_invalid_schema")


def _asset(root, relative):
    if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative
            or PurePosixPath(relative).is_absolute() or ".." in PurePosixPath(relative).parts):
        _fail("cleanup_unsafe_asset")
    path = root.joinpath(*PurePosixPath(relative).parts)
    _no_links(path)
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        _fail("cleanup_unsafe_asset")
    return path


def _bind(path, expected, inputs, limit):
    path = Path(path)
    if not path.is_absolute() or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
        _fail("cleanup_invalid_input_binding")
    _no_links(path)
    if not path.is_file() or path.stat().st_size > limit or _sha(path) != expected:
        _fail("cleanup_input_hash_or_size_mismatch", path.name)
    key = str(path.resolve())
    if key in inputs and inputs[key] != expected:
        _fail("cleanup_conflicting_input_binding")
    inputs[key] = expected
    return path.resolve()


def _request(request):
    _fields(request, ("schema", "input_data_identity", "source", "masks", "visibility", "oracle_cleanup", "policy", "budget"))
    if request["schema"] != "gsw.quality_mask_cleanup/1" or type(request["oracle_cleanup"]) is not bool:
        _fail("cleanup_invalid_schema")
    policy, budget = request["policy"], request["budget"]
    _fields(policy, ("min_views", "alpha_support_cutoff", "pixel_guard", "depth_tolerance"))
    _fields(policy["depth_tolerance"], ("value", "unit"))
    if (type(policy["min_views"]) is not int or policy["min_views"] < 2 or type(policy["pixel_guard"]) is not int
            or not 1 <= policy["pixel_guard"] <= 64 or type(policy["alpha_support_cutoff"]) not in (int, float)
            or not math.isfinite(policy["alpha_support_cutoff"]) or not 0 < policy["alpha_support_cutoff"] < 1
            or type(policy["depth_tolerance"]["value"]) not in (int, float)
            or not math.isfinite(policy["depth_tolerance"]["value"]) or policy["depth_tolerance"]["value"] < 0):
        _fail("cleanup_invalid_policy")
    names = ("max_vertices", "max_input_bytes", "max_cached_views", "max_cache_bytes", "chunk_vertices",
             "max_support_pixels", "max_report_bytes")
    _fields(budget, names)
    if any(type(budget[name]) is not int or budget[name] <= 0 for name in names):
        _fail("cleanup_invalid_budget")
    if budget["max_cached_views"] > 2 or budget["max_cache_bytes"] > 256 * 1024 * 1024 or budget["chunk_vertices"] > 4096:
        _fail("cleanup_cache_or_chunk_budget_exceeded")
    return copy.deepcopy(request)


def _cameras(info, record):
    if info["camera_path"]:
        return _read_json(info["camera_path"])
    root = info["training_contract"]
    to_current = camera_dataset.invert_sim3(info["frame_to_canonical"])
    from_training = camera_dataset.invert_sim3(root["transforms"]["canonical_to_training"])
    views = []
    for view in root.get("views", []):
        pose = np.asarray(view.get("training_c2w_opencv", view.get("c2w_opencv")), dtype=np.float64).copy()
        if pose.shape != (4, 4) or not np.isfinite(pose).all() or not np.array_equal(pose[3], [0., 0., 0., 1.]):
            _fail("cleanup_camera_pose_invalid")
        if "training_c2w_opencv" in view:
            r = np.asarray(from_training["R"])
            pose[:3, 3] = from_training["s"] * r @ pose[:3, 3] + from_training["t"]
            pose[:3, :3] = r @ pose[:3, :3]
        r = np.asarray(to_current["R"])
        pose[:3, 3] = to_current["s"] * r @ pose[:3, 3] + to_current["t"]
        pose[:3, :3] = r @ pose[:3, :3]
        width, height = view.get("training_size", [view.get("width"), view.get("height")])
        views.append({"view_id": view.get("view_id"), "split": view.get("split"), "image_path": view.get("image_path", ""),
            "width": width, "height": height, "K": view.get("training_K", view.get("K")), "c2w_opencv": pose.tolist(),
            "projection_model": view.get("projection_model", "PINHOLE"), "pixel_coordinates": view.get("pixel_coordinates", "pixel_border")})
    factor = to_current["s"] * from_training["s"]
    clipping = {key: value * factor for key, value in root.get("clipping", {}).items() if key in ("znear", "zfar", "backend_near")}
    return {"schema": "gsw.model_cameras/1", "version": 1, "input_data_identity": info["input_data_identity"],
        "coordinate_frame": info["current_frame"], "coordinate_unit": info["current_unit"], "transform_applied": True,
        "camera_count": len(views), "views": views, "clipping": clipping, "image_files_required_for_pose_display": False}


def _camera_index(bundle):
    result = {}
    for view in bundle["views"]:
        if view.get("view_id") in result or not isinstance(view.get("view_id"), str) or not view["view_id"]:
            _fail("cleanup_camera_identity_invalid")
        k, pose = np.asarray(view.get("K"), dtype=np.float64), np.asarray(view.get("c2w_opencv"), dtype=np.float64)
        if (k.shape != (3, 3) or not np.isfinite(k).all() or k[0, 0] <= 0 or k[1, 1] <= 0
                or not np.array_equal(k[2], [0., 0., 1.]) or pose.shape != (4, 4) or not np.isfinite(pose).all()
                or not np.array_equal(pose[3], [0., 0., 0., 1.]) or view.get("split") not in ("train", "val", "test")
                or any(type(view.get(name)) is not int or view[name] <= 0 for name in ("width", "height"))
                or view.get("projection_model") != "PINHOLE" or view.get("pixel_coordinates") != "pixel_border"):
            _fail("cleanup_camera_invalid")
        camera_dataset.invert_sim3({"s": 1., "R": pose[:3, :3].tolist(), "t": pose[:3, 3].tolist()})
        result[view["view_id"]] = copy.deepcopy(view)
    return result


def _mask_manifest(binding, identity, cameras, inputs, budget):
    _fields(binding, ("manifest", "sha256"))
    path = _bind(binding["manifest"], binding["sha256"], inputs, budget["max_input_bytes"])
    manifest = _read_json(path)
    _fields(manifest, ("schema", "input_data_identity", "labels", "views"))
    if (manifest["schema"] != "gsw.quality_cleanup_masks/1" or manifest["input_data_identity"] != identity
            or manifest["labels"] != {"unknown": 0, "foreground": 1, "background": 2}
            or not isinstance(manifest["views"], list) or not manifest["views"]):
        _fail("cleanup_mask_manifest_invalid")
    rows = {}
    for row in manifest["views"]:
        _fields(row, ("view_id", "path", "sha256"), ("camera_sha256",))
        view = cameras.get(row["view_id"])
        if view is None or view["split"] != "train":
            _fail("cleanup_heldout_or_unknown_mask_view")
        if row["view_id"] in rows:
            _fail("cleanup_duplicate_mask_view")
        if row.get("camera_sha256", _digest(view)) != _digest(view):
            _fail("cleanup_mask_camera_binding_mismatch")
        asset = _bind(_asset(path.parent, row["path"]), row["sha256"], inputs, budget["max_input_bytes"])
        if view["width"] * view["height"] > budget["max_cache_bytes"]:
            _fail("cleanup_single_view_exceeds_cache_budget")
        with Image.open(str(asset)) as image:
            if image.mode != "L" or image.size != (view["width"], view["height"]):
                _fail("cleanup_mask_shape_or_dtype")
            values = np.asarray(image)
            if np.any((values != 0) & (values != 1) & (values != 2)):
                _fail("cleanup_mask_labels_invalid")
        rows[row["view_id"]] = dict(row, absolute_path=asset)
    return rows


def _typed_model(source, info, budget, owned_maps):
    ply = PlyData.read(str(source), mmap="r")
    for element in ply.elements:
        if isinstance(element.data, np.memmap):
            owned_maps.append(element.data._mmap)
    if len(ply.elements) != 1 or ply.elements[0].name != "vertex" or any(isinstance(prop, PlyListProperty) for prop in ply["vertex"].properties):
        _fail("cleanup_unsupported_ply_element_or_relation")
    data = ply["vertex"].data
    if not 0 < len(data) <= budget["max_vertices"]:
        _fail("cleanup_vertex_budget_or_empty_model")
    names = data.dtype.names
    scales = [name for name in names if re.fullmatch(r"scale_[0-9]+", name)]
    dimensions = 2 if info["training_contract"].get("backend") == "2dgs" else 3
    if set(scales) != {"scale_%d" % i for i in range(dimensions)}:
        _fail("cleanup_gaussian_layout_invalid")
    required = ["x", "y", "z", "opacity", "f_dc_0", "f_dc_1", "f_dc_2"] + scales + ["rot_%d" % i for i in range(4)]
    covariance = _covariance_fields(names)
    required += covariance or []
    required += [name for name in names if re.fullmatch(r"f_rest_[0-9]+", name)]
    if any(name not in names or data.dtype[name].kind != "f" for name in required):
        _fail("cleanup_gaussian_layout_invalid")
    if any(re.search(r"(?:^|_)(?:index|indices)(?:_|$)", name, re.IGNORECASE) for name in names):
        _fail("cleanup_unsupported_ply_index_relation")
    comments = list(ply.comments) + [comment for element in ply.elements for comment in element.comments]
    if any(re.match(r"(?:TextureFile|Texture|MaterialFile|MaterialLibrary|mtllib|map_Kd)\b", comment, re.IGNORECASE) for comment in comments):
        _fail("cleanup_unsupported_material_assets")
    for start in range(0, len(data), budget["chunk_vertices"]):
        block = data[start:start + budget["chunk_vertices"]]
        if any(not np.isfinite(block[name]).all() for name in required):
            _fail("cleanup_nonfinite_gaussian")
        quaternions = np.column_stack([block["rot_%d" % i] for i in range(4)]).astype("float64")
        if np.any(np.linalg.norm(quaternions, axis=1) <= 1e-20):
            _fail("cleanup_invalid_quaternion")
        if covariance:
            for row in block:
                _covariance_sigma(row, covariance)
    return ply, data


def _covariance_fields(names):
    found = []
    for prefix in ("cov_", "covariance_"):
        for suffixes in ([str(i) for i in range(6)], ["xx", "xy", "xz", "yy", "yz", "zz"]):
            fields = [prefix + suffix for suffix in suffixes]
            if any(name in names for name in fields):
                if not all(name in names for name in fields):
                    _fail("cleanup_incomplete_covariance")
                found.append(fields)
    if len(found) > 1:
        _fail("cleanup_ambiguous_covariance")
    return found[0] if found else None


def _covariance_sigma(row, fields):
    xx, xy, xz, yy, yz, zz = [float(row[name]) for name in fields]
    matrix = np.array([[xx, xy, xz], [xy, yy, yz], [xz, yz, zz]], dtype="float64")
    if not np.isfinite(matrix).all():
        _fail("cleanup_invalid_covariance")
    values = np.linalg.eigvalsh(matrix)
    if values[0] < -64. * np.finfo(np.float64).eps * max(float(np.max(np.abs(matrix))), np.finfo(np.float64).tiny):
        _fail("cleanup_invalid_covariance")
    return math.sqrt(max(0., float(values[-1])))


def _unit_to_model(unit, info):
    if unit in ("model", info["current_unit"]):
        return 1.
    physical = {"m": 1., "cm": .01, "mm": .001}
    if unit not in physical or not info["metric_available"]:
        _fail("cleanup_depth_unit_unresolved")
    return physical[unit] / info["frame_to_canonical"]["s"]


def _visibility_manifest(binding, request, info, cameras, inputs):
    if binding is None:
        return {"rows": {}, "role": "missing", "provenance": None}
    _fields(binding, ("manifest", "sha256"))
    budget = request["budget"]
    path = _bind(binding["manifest"], binding["sha256"], inputs, budget["max_input_bytes"])
    manifest = _read_json(path)
    _fields(manifest, ("schema", "input_data_identity", "role", "provenance", "opaque_validity_labels", "views"))
    if (manifest["schema"] != "gsw.quality_cleanup_visibility/1" or manifest["input_data_identity"] != info["input_data_identity"]
            or manifest["opaque_validity_labels"] != {"unknown": 0, "reliable_opaque": 1}):
        _fail("cleanup_visibility_manifest_invalid")
    if manifest["role"] != "cleanup_visibility" and not (manifest["role"] == "evaluation_only" and request["oracle_cleanup"]):
        _fail("cleanup_visibility_role_requires_explicit_oracle_cleanup")
    provenance = manifest["provenance"]
    _fields(provenance, ("kind", "independent_of_source_model", "source_id", "source_sha256"))
    if (provenance["kind"] not in ("independent_observed_depth", "independent_external_geometry")
            or provenance["independent_of_source_model"] is not True
            or not isinstance(provenance["source_id"], str) or not 0 < len(provenance["source_id"]) <= 4096
            or not isinstance(provenance["source_sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", provenance["source_sha256"])
            or provenance["source_sha256"] == info["model_sha256"]):
        _fail("cleanup_independent_visibility_provenance_required")
    if not isinstance(manifest["views"], list):
        _fail("cleanup_visibility_manifest_invalid")
    rows = {}
    for row in manifest["views"]:
        _fields(row, ("view_id", "camera_sha256", "definition", "unit", "coordinate_frame", "depth", "opaque_validity"))
        view = cameras.get(row["view_id"])
        if view is None or view["split"] != "train":
            _fail("cleanup_heldout_or_unknown_visibility_view")
        if row["view_id"] in rows or row["camera_sha256"] != _digest(view):
            _fail("cleanup_visibility_camera_binding_mismatch")
        if row["definition"] != "camera_z" or row["coordinate_frame"] != info["current_frame"]:
            _fail("cleanup_visibility_depth_convention_invalid")
        factor = _unit_to_model(row["unit"], info)
        paths = {}
        for name in ("depth", "opaque_validity"):
            _fields(row[name], ("path", "sha256"))
            paths[name] = _bind(_asset(path.parent, row[name]["path"]), row[name]["sha256"], inputs, budget["max_input_bytes"])
        if view["width"] * view["height"] * 10 > budget["max_cache_bytes"]:
            _fail("cleanup_single_view_exceeds_cache_budget")
        depth = np.load(str(paths["depth"]), allow_pickle=False, mmap_mode="r")
        try:
            if (not isinstance(depth, np.ndarray) or depth.shape != (view["height"], view["width"])
                    or depth.dtype.kind != "f" or depth.dtype.itemsize not in (4, 8)):
                _fail("cleanup_visibility_depth_shape_or_dtype")
        finally:
            if isinstance(depth, np.memmap):
                depth._mmap.close()
            elif hasattr(depth, "close"):
                depth.close()
        with Image.open(str(paths["opaque_validity"])) as image:
            if image.mode != "L" or image.size != (view["width"], view["height"]):
                _fail("cleanup_visibility_validity_shape_or_dtype")
        rows[row["view_id"]] = dict(row, depth_path=paths["depth"], validity_path=paths["opaque_validity"], unit_to_model=factor)
    return {"rows": rows, "role": manifest["role"], "provenance": copy.deepcopy(provenance)}


class _ViewCache:
    def __init__(self, masks, visibility, budget, cancelled):
        self.masks, self.visibility, self.budget, self.cancelled = masks, visibility, budget, cancelled
        self.cache, self.bytes = OrderedDict(), 0
        self.stats = {"loads": 0, "evictions": 0, "peak_views": 0, "peak_bytes": 0}

    def get(self, identity):
        _poll(self.cancelled)
        if identity in self.cache:
            entry = self.cache.pop(identity); self.cache[identity] = entry
            return entry
        row = self.visibility["rows"].get(identity)
        with Image.open(str(self.masks[identity]["absolute_path"])) as image:
            predicted = image.width * image.height * (10 if row is not None else 1)
        if predicted > self.budget["max_cache_bytes"]:
            _fail("cleanup_single_view_exceeds_cache_budget")
        # Evict before decoding a new view, not after a third full depth array
        # has already been allocated. These counters describe cached arrays,
        # not total process RSS or the typed PLY reader's memory.
        while self.cache and (len(self.cache) >= self.budget["max_cached_views"] or self.bytes + predicted > self.budget["max_cache_bytes"]):
            unused, old = self.cache.popitem(last=False)
            self.bytes -= old["bytes"]; self.stats["evictions"] += 1
            del old
        with Image.open(str(self.masks[identity]["absolute_path"])) as image:
            mask = np.array(image, dtype="uint8", copy=True)
        depth, validity = None, None
        if row is not None:
            mapped = np.load(str(row["depth_path"]), allow_pickle=False, mmap_mode="r")
            try:
                with np.errstate(over="ignore", invalid="ignore"):
                    depth = np.array(mapped, dtype="float64", copy=True)
                    depth *= row["unit_to_model"]
            finally:
                if isinstance(mapped, np.memmap):
                    mapped._mmap.close()
            with Image.open(str(row["validity_path"])) as image:
                validity = np.array(image, dtype="uint8", copy=True)
            if np.any((validity != 0) & (validity != 1)):
                _fail("cleanup_visibility_validity_labels_invalid")
        size = mask.nbytes + (0 if depth is None else depth.nbytes + validity.nbytes)
        if size > self.budget["max_cache_bytes"]:
            _fail("cleanup_single_view_exceeds_cache_budget")
        entry = {"mask": mask, "depth": depth, "validity": validity, "bytes": size}
        self.cache[identity] = entry; self.bytes += size
        self.stats["loads"] += 1
        self.stats["peak_views"] = max(self.stats["peak_views"], len(self.cache))
        self.stats["peak_bytes"] = max(self.stats["peak_bytes"], self.bytes)
        return entry


def _footprint(row, view, policy, near, max_pixels):
    opacity = float(row["opacity"])
    alpha = 1. / (1. + math.exp(-opacity)) if opacity >= 0 else math.exp(opacity) / (1. + math.exp(opacity))
    if alpha <= policy["alpha_support_cutoff"]:
        return {"state": "uncertain", "reason": "below_declared_alpha_support_keep"}
    logs = [float(row[name]) for name in row.dtype.names if re.fullmatch(r"scale_[0-9]+", name)]
    try:
        sigma = math.exp(max(logs))
        covariance = _covariance_fields(row.dtype.names)
        if covariance:
            sigma = max(sigma, _covariance_sigma(row, covariance))
        radius = sigma * math.sqrt(2. * math.log(alpha / policy["alpha_support_cutoff"]))
    except OverflowError:
        return {"state": "uncertain", "reason": "unbounded_support"}
    if not math.isfinite(radius) or radius <= 0:
        return {"state": "uncertain", "reason": "degenerate_support"}
    # The sphere contains every orientation of both the 3D ellipsoid and the
    # 2D surfel's finite alpha support. It is deliberately looser than an ellipse.
    radius *= 1. + 16. * np.finfo(np.float32).eps
    pose, k = np.asarray(view["c2w_opencv"]), np.asarray(view["K"])
    center = (np.array([row["x"], row["y"], row["z"]], dtype="float64") - pose[:3, 3]) @ pose[:3, :3]
    low_z, high_z = np.nextafter(center[2] - radius, -np.inf), np.nextafter(center[2] + radius, np.inf)
    if high_z <= near:
        return {"state": "outside", "reason": "behind_near_plane"}
    if low_z <= near:
        return {"state": "uncertain", "reason": "support_crosses_near_plane"}
    intervals = []
    for axis in (0, 1):
        values = [(center[axis] + sign * radius) / z for sign in (-1., 1.) for z in (low_z, high_z)]
        intervals.append((min(values), max(values)))
    corners = np.array([[x, y, 1.] for x in intervals[0] for y in intervals[1]]) @ k.T
    low, high = corners[:, :2].min(axis=0), corners[:, :2].max(axis=0)
    integer_limit = np.iinfo(np.int64).max / 4.
    if (not np.isfinite(low).all() or not np.isfinite(high).all()
            or np.any(np.abs(low) > integer_limit) or np.any(np.abs(high) > integer_limit)):
        return {"state": "uncertain", "reason": "projection_numeric_range"}
    guard = policy["pixel_guard"]
    x0, y0 = np.floor(np.nextafter(low, -np.inf)).astype("int64") - guard
    x1, y1 = np.ceil(np.nextafter(high, np.inf)).astype("int64") + guard + 1
    width, height = view["width"], view["height"]
    if x1 <= 0 or y1 <= 0 or x0 >= width or y0 >= height:
        return {"state": "outside", "reason": "finite_support_outside_image"}
    if x0 < 0 or y0 < 0 or x1 > width or y1 > height:
        return {"state": "uncertain", "reason": "partial_footprint_unknown"}
    if (x1 - x0) * (y1 - y0) > max_pixels:
        return {"state": "uncertain", "reason": "footprint_pixel_budget_keep"}
    return {"state": "covered", "bounds": [int(x0), int(y0), int(x1), int(y1)],
        "finite_support_radius_model_units": radius, "finite_support_z_bounds": [float(low_z), float(high_z)],
        "alpha": alpha, "alpha_support_cutoff": policy["alpha_support_cutoff"]}


def _view_evidence(row, view, entry, policy, near, budget, tolerance):
    evidence = _footprint(row, view, policy, near, budget["max_support_pixels"])
    evidence.update(view_id=view["view_id"], camera_sha256=_digest(view), verified_background=False)
    if evidence["state"] != "covered":
        return evidence
    x0, y0, x1, y1 = evidence["bounds"]
    mask = entry["mask"][y0:y1, x0:x1]
    if np.any(mask == 1):
        evidence.update(state="veto", reason="foreground_overlap")
    elif np.any(mask == 0):
        evidence.update(state="veto", reason="unknown_mask_overlap")
    elif entry["depth"] is None:
        evidence.update(state="veto", reason="visibility_missing")
    else:
        depth, validity = entry["depth"][y0:y1, x0:x1], entry["validity"][y0:y1, x0:x1]
        if np.any(validity != 1) or not np.isfinite(depth).all() or np.any(depth <= 0):
            evidence.update(state="veto", reason="visibility_unknown")
        else:
            margin = float(np.min(depth - evidence["finite_support_z_bounds"][1]))
            evidence["minimum_free_space_margin_model_units"] = margin
            if margin < tolerance:
                evidence.update(state="veto", reason="occluded_or_depth_overlap")
            else:
                evidence.update(state="verified", reason="whole_finite_support_background_free_space",
                    verified_background=True, background_pixels=int(mask.size))
    return evidence


def _decide(stage, data, request, info, cameras, masks, visibility, bundle, cancelled):
    budget, policy = request["budget"], request["policy"]
    tolerance = policy["depth_tolerance"]["value"] * _unit_to_model(policy["depth_tolerance"]["unit"], info)
    cache = _ViewCache(masks, visibility, budget, cancelled)
    retained = np.ones(len(data), dtype=bool)
    priorities = ("foreground_overlap", "unknown_mask_overlap", "occluded_or_depth_overlap", "visibility_unknown", "visibility_missing")
    total_bytes, deleted = 0, 0
    with (stage / "decisions.jsonl").open("x", encoding="utf-8", newline="\n") as stream:
        for start in range(0, len(data), budget["chunk_vertices"]):
            _poll(cancelled)
            block = data[start:start + budget["chunk_vertices"]]
            decisions = [{"source_row": start + offset, "source_id": request["source"]["sha256"] + ":" + str(start + offset),
                "views": [], "verified_background_views": 0} for offset in range(len(block))]
            if request["visibility"] is not None:
                for identity in sorted(masks):
                    entry = None  # Release the prior view before a one-view cache evicts it.
                    entry, view = cache.get(identity), cameras[identity]
                    near = max(0., bundle["clipping"].get("znear", 0.), bundle["clipping"].get("backend_near", 0.))
                    for row, decision in zip(block, decisions):
                        evidence = _view_evidence(row, view, entry, policy, near, budget, tolerance)
                        decision["views"].append(evidence)
                        decision["verified_background_views"] += int(evidence["verified_background"])
            for offset, decision in enumerate(decisions):
                _poll(cancelled)
                veto = [evidence.get("reason", "unknown") for evidence in decision["views"] if evidence["state"] in ("uncertain", "veto")]
                if request["visibility"] is None:
                    reason = "independent_visibility_missing"
                elif veto:
                    reason = next((name for name in priorities if name in veto), veto[0])
                elif decision["verified_background_views"] < policy["min_views"]:
                    reason = "insufficient_verified_views"
                else:
                    reason = "whole_finite_support_background_free_space"
                    retained[start + offset] = False; deleted += 1
                decision.update(action="keep" if retained[start + offset] else "delete", reason=reason)
                encoded = json.dumps(decision, separators=(",", ":"), allow_nan=False) + "\n"
                total_bytes += len(encoded.encode("utf-8"))
                if total_bytes > budget["max_report_bytes"]:
                    _fail("cleanup_decision_report_budget_exceeded")
                stream.write(encoded)
        stream.flush(); os.fsync(stream.fileno())
    if not retained.any():
        _fail("cleanup_would_remove_all_gaussians")
    return retained, deleted, cache.stats


def _write_subset(path, ply, data, retained, chunk, cancelled):
    byte_order = ply.byte_order if ply.byte_order in ("<", ">") else "<"
    header = re.sub(r"(?m)^format .+$", "format binary_%s_endian 1.0" % ("little" if byte_order == "<" else "big"), ply.header)
    header = re.sub(r"(?m)^element vertex [0-9]+$", "element vertex %d" % int(retained.sum()), header)
    with Path(path).open("xb") as stream:
        stream.write((header.rstrip("\r\n") + "\n").encode("ascii"))
        for start in range(0, len(data), chunk):
            _poll(cancelled)
            selected = data[start:start + chunk][retained[start:start + chunk]]
            stream.write(selected.astype(data.dtype.newbyteorder(byte_order), copy=False).tobytes())
        stream.flush(); os.fsync(stream.fileno())
    actual = PlyData.read(str(path), mmap="r")["vertex"].data
    try:
        cursor = 0
        for start in range(0, len(data), chunk):
            _poll(cancelled)
            wanted = data[start:start + chunk][retained[start:start + chunk]]
            block = actual[cursor:cursor + len(wanted)]
            for name in data.dtype.names:
                if wanted.dtype[name] != block.dtype[name] or wanted[name].tobytes() != block[name].tobytes():
                    _fail("cleanup_retained_attribute_changed", name)
            cursor += len(wanted)
        if cursor != len(actual):
            _fail("cleanup_retained_count_mismatch")
    finally:
        if isinstance(actual, np.memmap):
            actual._mmap.close()


def _coordinate_output(stage, info, record, bundle, source_hash, metadata_hash, count):
    model = stage / "derived.ply"
    model_hash = _sha(model)
    cameras = copy.deepcopy(bundle); cameras["model_sha256"] = model_hash
    _write_json(stage / "derived.cameras.json", cameras)
    camera_hash = _sha(stage / "derived.cameras.json")
    frame, unit, root = info["current_frame"], info["current_unit"], info["training_contract"]
    identity = {"s": 1., "R": np.eye(3).tolist(), "t": [0., 0., 0.], "input_frame": frame, "output_frame": frame, "applied": True}
    scene = copy.deepcopy(record.get("canonical_scene_transform", dict(identity, input_frame="canonical", output_frame="canonical")))
    metadata = {"schema": "gsw.coordinate_export/1", "export_version": 3, "success": True, "coordinate_mode": "preserve",
        "input_frame": frame, "output_frame": frame, "input_unit": unit, "output_unit": unit, "current_frame": frame, "current_unit": unit,
        "transform_applied": True, "applied_transform": identity, "input_sha256": source_hash, "input_metadata_sha256": metadata_hash,
        "output_sha256": model_hash, "ply_sha256": model_hash, "vertex_count": count, "input_data_identity": info["input_data_identity"],
        "scale_status": root["scale"]["status"], "scale_evidence": root["scale"].get("evidence", []),
        "scale": copy.deepcopy(root["scale"]), "transforms": copy.deepcopy(root["transforms"]), "training_contract": copy.deepcopy(root),
        "input_canonical_scene_transform": scene, "canonical_scene_transform": scene, "scene_scale_multiplier": scene["s"],
        "physical_scale_modified": abs(scene["s"] - 1.) > 1e-12, "scale_evidence_scope": "immutable_training_contract",
        "scene_edit_independent_validation": False, "full_optimizer_state_transformed": False, "assets": [], "assets_sha256": _digest([]),
        "camera_reference": {"path": "derived.cameras.json", "sha256": camera_hash, "schema": "gsw.model_cameras/1"},
        "camera_count": len(cameras["views"]), "surface_accuracy_certified": False, "appearance_rotation": "unchanged",
        "position_storage": "source_property_types_unchanged", "original_training_model_retained": True, "input_model_retained": True,
        "export_operations": copy.deepcopy(record.get("export_operations", [])) + [{"operation": "quality_mask_subset",
            "input_ply_sha256": source_hash, "coordinate_values_changed": False, "retained_vertices": count}]}
    metadata["bundle_sha256"] = _digest({"model_sha256": model_hash, "camera_sha256": camera_hash,
        "input_data_identity": info["input_data_identity"], "coordinate_frame": frame, "coordinate_unit": unit, "assets_sha256": metadata["assets_sha256"]})
    _write_json(stage / "derived.camera-scale.json", metadata)
    camera_scale_export.inspect_coordinate_metadata(model)


def _postprocess(request, output_directory, cancelled, owned_maps):
    """Publish a traceable conservative subset; no visibility means keep all."""
    _poll(cancelled)
    request = _request(request)
    budget, inputs = request["budget"], {}
    _fields(request["source"], ("ply", "sha256", "sidecar", "sidecar_sha256"))
    source = _bind(request["source"]["ply"], request["source"]["sha256"], inputs, budget["max_input_bytes"])
    sidecar = _bind(request["source"]["sidecar"], request["source"]["sidecar_sha256"], inputs, budget["max_input_bytes"])
    info = camera_scale_export.inspect_coordinate_metadata(source, sidecar)
    if info["input_data_identity"] != request["input_data_identity"]:
        _fail("cleanup_data_identity_mismatch")
    record = _read_json(sidecar)
    if record.get("assets"):
        _fail("cleanup_unsupported_material_assets")
    if info["camera_path"]:
        _bind(info["camera_path"], _sha(info["camera_path"]), inputs, budget["max_input_bytes"])
    bundle = _cameras(info, record)
    cameras = _camera_index(bundle)
    masks = _mask_manifest(request["masks"], info["input_data_identity"], cameras, inputs, budget)
    visibility = _visibility_manifest(request["visibility"], request, info, cameras, inputs)
    ply, data = _typed_model(source, info, budget, owned_maps)
    output = Path(output_directory)
    if not output.is_absolute() or output.drive.upper() != "D:":
        _fail("cleanup_D_output_required")
    _no_links(output)
    if output.exists() or not output.parent.is_dir():
        _fail("cleanup_fresh_output_required")
    for path in inputs:
        if Path(path).parent == output or Path(path).parent in output.parents or output in Path(path).parents:
            _fail("cleanup_output_overlaps_input")
    _poll(cancelled)
    with atomic_sibling_directory(output) as stage:
        retained, deleted, cache_stats = _decide(stage, data, request, info, cameras, masks, visibility, bundle, cancelled)
        _write_subset(stage / "derived.ply", ply, data, retained, budget["chunk_vertices"], cancelled)
        _coordinate_output(stage, info, record, bundle, request["source"]["sha256"], request["source"]["sidecar_sha256"], int(retained.sum()))
        shutil.copyfile(str(sidecar), str(stage / "raw-source-metadata.json"))
        manifest = {"schema": "gsw.quality_mask_cleanup_manifest/1", "status": "complete", "cleanup_status": "applied_unvalidated" if deleted else "unverified",
            "implementation": "conservative_background_free_space_v1_clean_inspired_independent_engineering_not_paper_reproduction",
            "source": copy.deepcopy(request["source"]), "input_data_identity": info["input_data_identity"],
            "coordinate_frame": info["current_frame"], "coordinate_unit": info["current_unit"], "projection_transform": "identity_model_and_bound_camera_frame",
            "policy": copy.deepcopy(request["policy"]), "budget": copy.deepcopy(budget), "oracle_cleanup": request["oracle_cleanup"],
            "independent_geometry_evaluation_eligible": not request["oracle_cleanup"], "retained_count": int(retained.sum()), "deleted_count": deleted,
            "visibility_role": visibility["role"], "visibility_provenance": visibility["provenance"], "cache_stats": cache_stats,
            "support_definition": "opacity_weighted_gaussian_support_above_explicit_alpha_cutoff_conservative_sphere_projection",
            "parameterization": "log_scale_logit_opacity_wxyz_quaternion_optional_explicit_covariance_union",
            "full_infinite_gaussian_support_evaluated": False, "tail_below_cutoff_is_chosen_policy_not_surface_error": True,
            "resource_scope": "binary_PLY_memory_mapped_rows_chunked_views_LRU_bounded_ASCII_reader_within_input_byte_budget_cache_stats_not_total_RSS",
            "quality_accepted": False, "raw_quality_evaluation": "not_run", "postprocess_quality_evaluation": "not_run",
            "geometry_accuracy": "not_measured", "finite_support_is_surface_error": False,
            "source_attributes_unchanged": True, "source_model_retained": True, "input_hashes": inputs,
            "outputs": {name: _sha(stage / name) for name in ("derived.ply", "derived.camera-scale.json", "derived.cameras.json", "raw-source-metadata.json", "decisions.jsonl")}}
        _poll(cancelled)
        _write_json(stage / "manifest.json", manifest)
        _poll(cancelled)
        # The last caller callback may request cancellation or expose concurrent
        # input changes. Recheck the whole input set after it, before publication.
        for path, expected in inputs.items():
            _no_links(path)
            if _sha(path) != expected:
                _fail("cleanup_input_changed_before_publication")
        if output.exists():
            _fail("cleanup_fresh_output_required")
        os.rename(str(stage), str(output))
    return dict(manifest, model_path=str(output / "derived.ply"), manifest_path=str(output / "manifest.json"))


def postprocess(request, output_directory, cancelled=None):
    """Publish a traceable conservative subset, always releasing input maps."""
    owned_maps = []
    try:
        return _postprocess(request, output_directory, cancelled, owned_maps)
    finally:
        for mapping in reversed(owned_maps):
            mapping.close()
