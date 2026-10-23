# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Opt-in, immutable training-view supervision and explicit normalized losses.

The public seams are ``load_supervision`` -> ``provider.for_view`` and
``supervision_losses``. No Torch import or GPU work occurs while loading input.
Assets and manifests are read only; nothing in this module changes cameras,
initialization, source images, training scale, optimizer or runtime policy.
"""
import copy
from collections import OrderedDict
import hashlib
import io
import json
from pathlib import Path
import re
import threading

import numpy as np


class SupervisionError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if detail else ""))


def _fail(code, detail=""):
    raise SupervisionError(code, detail)


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()


def _fields(value, required, optional=()):
    if (not isinstance(value, dict) or set(required) - set(value)
            or set(value) - set(required) - set(optional)):
        _fail("supervision_schema_invalid")


def _poll(cancelled):
    if cancelled and cancelled():
        _fail("supervision_cancelled")


def _read_bytes(path, limit, cancelled):
    _poll(cancelled)
    try:
        for entry in (path,) + tuple(path.parents):
            if entry.is_symlink() or getattr(entry.lstat(), "st_file_attributes", 0) & 0x400:
                _fail("supervision_asset_link_refused")
        if not path.is_file() or path.stat().st_size > limit:
            _fail("supervision_asset_size_or_type_invalid")
        parts, length = [], 0
        with path.open("rb") as stream:
            while True:
                _poll(cancelled)
                data = stream.read(4 * 1024 * 1024)
                if not data:
                    break
                length += len(data)
                if length > limit:
                    _fail("supervision_asset_size_or_type_invalid")
                parts.append(data)
        _poll(cancelled)
        return b"".join(parts)
    except OSError as error:
        _fail("supervision_asset_unreadable", error)


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _fail("supervision_duplicate_json_key", key)
        result[key] = value
    return result


def _camera_index(contract):
    if (not isinstance(contract, dict) or not isinstance(contract.get("input_data_identity"), str)
            or re.fullmatch(r"[0-9a-f]{64}", contract["input_data_identity"]) is None
            or not isinstance(contract.get("views"), list) or not isinstance(contract.get("splits"), dict)
            or set(contract["splits"]) != {"train", "val", "test"}):
        _fail("supervision_camera_contract_invalid")
    cameras = {}
    for view in contract["views"]:
        if (not isinstance(view, dict) or not isinstance(view.get("view_id"), str) or not view["view_id"]
                or view["view_id"] in cameras or view.get("split") not in ("train", "val", "test")
                or not isinstance(view.get("sha256"), str) or re.fullmatch(r"[0-9a-f]{64}", view["sha256"]) is None):
            _fail("supervision_camera_contract_invalid")
        cameras[view["view_id"]] = view
    for split in ("train", "val", "test"):
        ids = contract["splits"][split]
        if (not isinstance(ids, list) or any(not isinstance(view_id, str) for view_id in ids)
                or len(set(ids)) != len(ids) or set(ids) != {key for key, view in cameras.items() if view["split"] == split}):
            _fail("supervision_camera_contract_invalid")
    if not contract["splits"]["train"]:
        _fail("supervision_camera_contract_no_train_views")
    return cameras


def _readonly(array):
    with np.errstate(over="ignore", invalid="ignore"):
        result = np.ascontiguousarray(array, dtype=np.float32)
    if not np.isfinite(result).all():
        _fail("supervision_float32_conversion_invalid")
    # A bytes owner also prevents a caller from re-enabling the write flag.
    return np.frombuffer(result.tobytes(), dtype=np.float32).reshape(result.shape)


def _geometry(row, view):
    geometry = row.get("geometry")
    _fields(geometry, ("width", "height", "K", "pixel_coordinates", "frame", "distortion"))
    try:
        size = [geometry["width"], geometry["height"]]
        k = np.asarray(geometry["K"], dtype=np.float64).copy()
        expected_size = view.get("training_size", [view["width"], view["height"]])
        expected_k = np.asarray(view.get("training_K", view["K"]), dtype=np.float64)
        distortion = np.asarray(geometry["distortion"], dtype=np.float64)
    except (KeyError, TypeError, ValueError):
        _fail("supervision_geometry_invalid")
    pixels = geometry.get("pixel_coordinates")
    if pixels not in ("pixel_border", "opencv_integer") or geometry.get("frame") != "camera":
        _fail("supervision_geometry_convention_invalid")
    if (any(type(x) is not int or x <= 0 for x in size) or k.shape != (3, 3)
            or not np.isfinite(k).all() or k[0, 0] <= 0 or k[1, 1] <= 0
            or not np.allclose(k[2], [0., 0., 1.], rtol=0, atol=1e-12)):
        _fail("supervision_geometry_invalid")
    if pixels == "opencv_integer":
        k[0, 2] += .5
        k[1, 2] += .5
    if (distortion.ndim != 1 or not np.isfinite(distortion).all() or np.any(distortion != 0)
            or expected_k.shape != (3, 3) or not np.isfinite(expected_k).all()
            or np.linalg.det(k) <= 0 or np.linalg.det(expected_k) <= 0):
        _fail("supervision_reprojection_required", row["view_id"])
    if "registration" not in row and (size != expected_size
            or not np.allclose(k, expected_k, rtol=1e-12, atol=1e-12)):
        _fail("supervision_reprojection_required", row["view_id"])
    return (size[1], size[0]), k


def _registration(row, view, kind, unit_scale, scale_identity):
    if "registration" not in row:
        return None
    record = row["registration"]
    _fields(record, ("model", "source_frame", "target_frame", "target_view_id", "target_image_sha256",
        "R", "t", "translation_unit", "translation_to_training_scale", "scale_evidence_identity", "evidence_identity"))
    expected_model = "shared_camera_rays" if kind == "mask" else "rigid_depth"
    if (record["model"] != expected_model or record["source_frame"] != "camera" or record["target_frame"] != "camera"
            or record["target_view_id"] != view["view_id"] or record["target_image_sha256"] != view["sha256"]):
        _fail("supervision_registration_binding_invalid")
    if (not isinstance(scale_identity, str) or re.fullmatch(r"[0-9a-f]{64}", scale_identity) is None
            or record["scale_evidence_identity"] != scale_identity
            or not isinstance(record["evidence_identity"], str)
            or re.fullmatch(r"[0-9a-f]{64}", record["evidence_identity"]) is None
            or isinstance(unit_scale, bool) or not isinstance(unit_scale, (int, float))
            or not np.isfinite(unit_scale) or unit_scale <= 0):
        _fail("supervision_registration_scale_evidence_invalid")
    try:
        if (not isinstance(record["R"], list) or len(record["R"]) != 3
                or any(not isinstance(row, list) or len(row) != 3 for row in record["R"])
                or not isinstance(record["t"], list) or len(record["t"]) != 3
                or any(type(number) not in (int, float) for row in record["R"] for number in row)
                or any(type(number) not in (int, float) for number in record["t"])):
            _fail("supervision_registration_rigid_transform_invalid")
        rotation, translation = np.asarray(record["R"], dtype=np.float64), np.asarray(record["t"], dtype=np.float64)
    except (TypeError, ValueError):
        _fail("supervision_registration_rigid_transform_invalid")
    if (rotation.shape != (3, 3) or translation.shape != (3,) or not np.isfinite(rotation).all()
            or not np.isfinite(translation).all() or not np.allclose(rotation.T @ rotation, np.eye(3), rtol=0, atol=1e-8)
            or not np.isclose(np.linalg.det(rotation), 1., rtol=0, atol=1e-8)):
        _fail("supervision_registration_rigid_transform_invalid")
    unit = record["translation_unit"]
    physical = {"m": 1., "cm": .01, "mm": .001}
    if unit not in ("m", "cm", "mm", "scene"):
        _fail("supervision_registration_translation_unit_invalid")
    factor = record["translation_to_training_scale"]
    expected_factor = unit_scale
    if kind == "depth":
        source_unit = row["depth"].get("unit")
        if unit in physical and source_unit in physical:
            expected_factor *= physical[unit] / physical[source_unit]
        elif source_unit != unit:
            _fail("supervision_registration_translation_scale_unresolved")
    if (isinstance(factor, bool) or not isinstance(factor, (int, float)) or not np.isfinite(factor)
            or factor <= 0 or not np.isclose(factor, expected_factor, rtol=1e-12, atol=0)):
        _fail("supervision_registration_translation_scale_mismatch")
    if kind == "mask" and np.any(translation != 0):
        _fail("supervision_mask_registration_requires_shared_center")
    with np.errstate(over="ignore", invalid="ignore"):
        training_translation = translation * factor
    if not np.isfinite(training_translation).all():
        _fail("supervision_registration_rigid_transform_invalid")
    return {"R": rotation, "t_training": training_translation, "record": copy.deepcopy(record)}


def _registered_mask(payload, counts, source_shape, source_k, target_shape, target_k, registration, cancelled):
    """Inverse-map target pixel-center rays to source nearest half-open cells.

    Pixel-border coordinates select cell ``floor(u),floor(v)``. At an exact cell
    boundary the positive-side cell wins. Unknown/outside cells stay invalid;
    there is no bilinear averaging, image resizing or background filling.
    """
    _poll(cancelled)
    y, x = np.indices(target_shape, dtype=np.float64)
    pixels = np.stack((x + .5, y + .5, np.ones(target_shape)), axis=-1)
    source_rays = pixels @ np.linalg.inv(target_k).T @ registration["R"]
    projected = source_rays @ source_k.T
    with np.errstate(divide="ignore", invalid="ignore"):
        u, v = projected[..., 0] / projected[..., 2], projected[..., 1] / projected[..., 2]
    covered = (np.isfinite(u) & np.isfinite(v) & (source_rays[..., 2] > 0)
        & (u >= 0) & (u < source_shape[1]) & (v >= 0) & (v < source_shape[0]))
    source_x, source_y = np.floor(u[covered]).astype(np.int64), np.floor(v[covered]).astype(np.int64)
    result = {}
    for name in ("foreground_weights", "background_weights"):
        mapped = np.zeros(target_shape, dtype=np.float32)
        mapped[covered] = payload[name][source_y, source_x]
        result[name] = _readonly(mapped)
    foreground, background = result["foreground_weights"], result["background_weights"]
    active = (foreground > 0) | (background > 0)
    if not active.any():
        _fail("supervision_mask_all_invalid", "registered rays have no valid source regions")
    updated = {"source_" + name: count for name, count in counts.items()}
    updated.update(foreground_pixels=int((foreground > 0).sum()), background_pixels=int((background > 0).sum()),
        invalid_pixels=int((~active).sum()), registration_covered_pixels=int(covered.sum()),
        registration_uncovered_pixels=int((~covered).sum()))
    _poll(cancelled)
    return result, updated


def _immutable(array, dtype):
    result = np.ascontiguousarray(array, dtype=dtype)
    return np.frombuffer(result.tobytes(), dtype=result.dtype).reshape(result.shape)


def _registered_depth(values, definition, candidate, confidence, material, labels, source_k,
                      target_shape, target_k, unit_scale, registration, cancelled):
    """Forward-project reliable source samples, selecting closest target camera-Z.

    The depth winner is selected before material exclusion. A transparent or
    unknown nearest surface can therefore invalidate supervision but cannot be
    replaced by farther opaque geometry. Ties retain the first source pixel in
    row-major order; no depths, confidences or material labels are averaged.
    """
    _poll(cancelled)
    y, x = np.indices(values.shape, dtype=np.float64)
    pixels = np.stack((x + .5, y + .5, np.ones(values.shape)), axis=-1)
    rays = pixels @ np.linalg.inv(source_k).T
    with np.errstate(divide="ignore", over="ignore", invalid="ignore", under="ignore"):
        z = 1. / values if definition == "inverse_z" else values.copy()
        if definition == "ray_range":
            z /= np.linalg.norm(rays, axis=-1)
        source_z_training = z * unit_scale
        source_points = rays * source_z_training[..., None]
        source_geometry_valid = (np.isfinite(source_z_training) & (source_z_training > 0)
                                 & np.isfinite(source_points).all(axis=-1))
        points = source_points @ registration["R"].T + registration["t_training"]
        projected = points @ target_k.T
        u, v = projected[..., 0] / projected[..., 2], projected[..., 1] / projected[..., 2]
    target_z = points[..., 2]
    inside = (candidate & source_geometry_valid & np.isfinite(points).all(axis=-1) & (target_z > 0) & np.isfinite(u) & np.isfinite(v)
              & (u >= 0) & (u < target_shape[1]) & (v >= 0) & (v < target_shape[0]))
    source_indices = np.flatnonzero(inside)
    target_indices = (np.floor(v[inside]).astype(np.int64) * target_shape[1]
                      + np.floor(u[inside]).astype(np.int64))
    depths = target_z[inside]
    order = np.lexsort((source_indices, depths, target_indices))
    target_indices, source_indices = target_indices[order], source_indices[order]
    first = np.ones(len(order), dtype=bool)
    if len(order):
        first[1:] = target_indices[1:] != target_indices[:-1]
    chosen_target, chosen_source = target_indices[first], source_indices[first]
    winner = np.full(target_shape, -1, dtype=np.int64)
    winner.ravel()[chosen_target] = chosen_source
    reasons = {"valid": 0, "no_coverage": 1, "unknown": 2, "transparent": 3,
               "specular": 4, "occluded": 5, "conversion_invalid": 6}
    exclusion = np.full(target_shape, reasons["no_coverage"], dtype=np.uint8)
    exclusion.ravel()[chosen_target] = reasons["valid"]
    winning_material = material.ravel()[chosen_source]
    for name in ("unknown", "transparent", "specular", "occluded"):
        exclusion.ravel()[chosen_target[winning_material == labels[name]]] = reasons[name]
    inverse_z, weights = np.zeros(target_shape, dtype=np.float64), np.zeros(target_shape, dtype=np.float64)
    with np.errstate(divide="ignore", over="ignore", invalid="ignore", under="ignore"):
        inverse_z.ravel()[chosen_target] = 1. / target_z.ravel()[chosen_source]
        converted = inverse_z.astype(np.float32)
    bad_conversion = (winner >= 0) & (~np.isfinite(converted) | (converted <= 0))
    exclusion[bad_conversion] = reasons["conversion_invalid"]
    usable = exclusion == reasons["valid"]
    weights.ravel()[chosen_target] = confidence.ravel()[chosen_source]
    weights[~usable], inverse_z[~usable] = 0., 0.
    if not usable.any():
        _fail("supervision_depth_all_invalid", "registered depth has no valid target coverage")
    payload = {"inverse_z": _readonly(inverse_z), "depth_weights": _readonly(weights),
        "winner_source_index": _immutable(winner, np.int64), "exclusion_reason": _immutable(exclusion, np.uint8),
        "exclusion_reason_codes": reasons}
    counts = {"valid_depth_pixels": int(usable.sum()), "registration_covered_pixels": len(chosen_target),
        "registration_uncovered_pixels": int((winner < 0).sum()),
        "registration_occluded_source_pixels": len(order) - len(chosen_target),
        "registration_unprojected_source_pixels": int(candidate.sum()) - len(order),
        "excluded_conversion_pixels": int(bad_conversion.sum())}
    for name in ("unknown", "transparent", "specular", "occluded"):
        counts["excluded_" + name + "_pixels"] = int((exclusion == reasons[name]).sum())
    _poll(cancelled)
    return payload, counts


class SupervisionProvider:
    """Metadata plus a bounded decoded-view LRU; no per-camera tensor ownership.

    Callers must release returned payloads when no longer needed: references
    retained by callers cannot be bounded by the provider's own cache budget.
    Cached assets are hash-rechecked before reuse; evicted views are decoded
    and verified again. Oversized single views are returned but never cached.
    """
    def __init__(self, view_ids, decoder, verifier, report, identity, contract, max_views, max_bytes):
        self._view_ids, self._decoder, self._verifier = set(view_ids), decoder, verifier
        self._cache = OrderedDict()
        self._lock = threading.RLock()
        self._max_views, self._max_bytes, self._resident_bytes = max_views, max_bytes, 0
        self._hits, self._misses, self._evictions, self._oversize = 0, 0, 0, 0
        self._report = report
        self.identity = identity
        self._splits = {view["view_id"]: view["split"] for view in contract["views"]}

    @property
    def report(self):
        result = copy.deepcopy(self._report)
        result["resident_cache"] = self.cache_info
        return result

    @property
    def cache_info(self):
        with self._lock:
            return self._cache_info_locked()

    def _cache_info_locked(self):
        return {"max_cached_views": self._max_views, "max_cached_bytes": self._max_bytes,
            "resident_views": len(self._cache), "resident_bytes": self._resident_bytes,
            "hits": self._hits, "misses": self._misses, "evictions": self._evictions,
            "uncached_oversize_views": self._oversize}

    def for_view(self, view_id):
        # Serialize decode as well as insertion: simultaneous current/prefetch
        # requests must not each allocate a view and overcount one cache key.
        with self._lock:
            return self._for_view_locked(view_id)

    def _for_view_locked(self, view_id):
        if self._splits.get(view_id) != "train":
            _fail("supervision_view_not_train", view_id)
        if view_id not in self._view_ids:
            _fail("supervision_view_missing", view_id)
        if view_id in self._cache:
            self._verifier(view_id)
            self._hits += 1
            payload, size = self._cache.pop(view_id)
            self._cache[view_id] = (payload, size)
        else:
            self._misses += 1
            payload = self._decoder(view_id)
            size = sum(value.nbytes for value in payload.values() if isinstance(value, np.ndarray))
            if size <= self._max_bytes:
                while self._cache and (len(self._cache) >= self._max_views or self._resident_bytes + size > self._max_bytes):
                    removed_payload, removed_size = self._cache.popitem(last=False)[1]
                    self._resident_bytes -= removed_size
                    self._evictions += 1
                    del removed_payload
                self._cache[view_id] = (payload, size)
                self._resident_bytes += size
            else:
                self._oversize += 1
        return {name: value if isinstance(value, np.ndarray) else copy.deepcopy(value)
                for name, value in payload.items()}


def _asset(root, record, cancelled):
    if not isinstance(record, dict) or not isinstance(record.get("path"), str):
        _fail("supervision_asset_invalid")
    relative = Path(record["path"])
    if relative.is_absolute() or ".." in relative.parts:
        _fail("supervision_asset_path_invalid")
    path = root / relative
    data = _read_bytes(path, 512 * 1024 * 1024, cancelled)
    if hashlib.sha256(data).hexdigest() != record.get("sha256"):
        _fail("supervision_asset_hash_mismatch", relative)
    try:
        if path.suffix.lower() == ".npy":
            result = np.load(io.BytesIO(data), allow_pickle=False)
        elif path.suffix.lower() == ".png":
            from PIL import Image
            with Image.open(io.BytesIO(data)) as image:
                if (image.format != "PNG" or image.mode != "L" or image.getexif().get(274, 1) != 1
                        or "transparency" in image.info):
                    _fail("supervision_asset_image_contract")
                result = np.asarray(image, dtype=np.uint8).copy()
        else:
            _fail("supervision_asset_format_unsupported")
    except (ValueError, OSError) as error:
        _fail("supervision_asset_decode_failed", error)
    _poll(cancelled)
    if not isinstance(result, np.ndarray) or result.dtype.hasobject:
        _fail("supervision_asset_dtype_invalid")
    return result


def _mask(root, record, shape, cancelled):
    _fields(record, ("path", "sha256", "labels"), ("weights",))
    labels = _asset(root, record, cancelled)
    if labels.shape != shape:
        _fail("supervision_reprojection_required")
    values = record["labels"]
    _fields(values, ("invalid", "foreground", "background"))
    if (labels.dtype != np.uint8 or any(type(value) is not int or not 0 <= value <= 255
            for value in values.values()) or len(set(values.values())) != 3
            or not np.isin(labels, list(values.values())).all()):
        _fail("supervision_mask_labels_invalid")
    weights = np.ones(shape, dtype=np.float32)
    if "weights" in record:
        _fields(record["weights"], ("path", "sha256"))
        weights = _asset(root, record["weights"], cancelled)
        if weights.shape != shape:
            _fail("supervision_reprojection_required")
        if (weights.dtype.kind != "f" or not np.isfinite(weights).all()
                or np.any(weights < 0)):
            _fail("supervision_mask_weights_invalid")
    foreground = _readonly(np.where(labels == values["foreground"], weights, 0.))
    background = _readonly(np.where(labels == values["background"], weights, 0.))
    if float(foreground.sum()) + float(background.sum()) <= 0:
        _fail("supervision_mask_all_invalid")
    return {"foreground_weights": foreground, "background_weights": background}, {
        "foreground_pixels": int((foreground > 0).sum()), "background_pixels": int((background > 0).sum()),
        "invalid_pixels": int((labels == values["invalid"]).sum())}


def _depth(root, record, shape, k, unit_to_training_scale, allow_oracle, train_ids, cancelled,
           registration=None, target_shape=None, target_k=None):
    _fields(record, ("path", "sha256", "definition", "unit", "frame", "confidence", "validity", "materials", "provenance"))
    definition = record["definition"]
    if definition not in ("camera_z", "ray_range", "inverse_z"):
        _fail("supervision_depth_definition_invalid")
    if record["unit"] not in ("m", "cm", "mm", "scene") or record["frame"] != "camera":
        _fail("supervision_depth_unit_or_frame_invalid")
    _fields(record["confidence"], ("path", "sha256"))
    _fields(record["validity"], ("path", "sha256"))
    _fields(record["materials"], ("path", "sha256", "labels"))
    values = _asset(root, record, cancelled)
    confidence = _asset(root, record["confidence"], cancelled)
    validity = _asset(root, record["validity"], cancelled)
    material = _asset(root, record["materials"], cancelled)
    if any(array.shape != shape for array in (values, confidence, validity, material)):
        _fail("supervision_reprojection_required")
    if (values.dtype not in (np.dtype("float32"), np.dtype("float64"))
            or confidence.dtype.kind != "f" or validity.dtype.kind not in ("b", "u")
            or not np.isin(validity, [0, 1]).all() or material.dtype != np.uint8):
        _fail("supervision_depth_dtype_invalid")
    labels = record["materials"].get("labels")
    _fields(labels, ("unknown", "opaque", "transparent", "specular", "occluded"))
    if (any(type(value) is not int or not 0 <= value <= 255 for value in labels.values())
            or len(set(labels.values())) != 5 or not np.isin(material, list(labels.values())).all()):
        _fail("supervision_material_labels_invalid")
    provenance = record["provenance"]
    _fields(provenance, ("kind", "source", "role", "alignment"), ("oracle_training_authorized",))
    if (provenance["kind"] not in ("measured", "relative", "oracle") or provenance["role"] != "training"
            or not isinstance(provenance["source"], str) or not provenance["source"]):
        _fail("supervision_depth_provenance_invalid")
    if provenance["kind"] == "oracle" and (allow_oracle is not True
            or provenance.get("oracle_training_authorized") is not True):
        _fail("supervision_oracle_training_not_authorized")
    alignment = provenance["alignment"]
    scale, shift = 1., 0.
    if alignment == {"source": "none", "view_ids": []}:
        if provenance["kind"] == "relative":
            _fail("supervision_depth_alignment_required")
    else:
        _fields(alignment, ("source", "view_ids", "scale", "shift", "evidence_identity"))
        if (alignment["source"] not in ("train_sfm", "train_scale_evidence")
                or not isinstance(alignment["view_ids"], list) or not alignment["view_ids"]
                or any(not isinstance(view_id, str) or view_id not in train_ids for view_id in alignment["view_ids"])
                or len(set(alignment["view_ids"])) != len(alignment["view_ids"])
                or not isinstance(alignment["evidence_identity"], str)
                or re.fullmatch(r"[0-9a-f]{64}", alignment["evidence_identity"]) is None):
            _fail("supervision_depth_alignment_invalid")
        scale, shift = alignment["scale"], alignment["shift"]
        if (any(isinstance(number, bool) or not isinstance(number, (int, float))
                or not np.isfinite(number) for number in (scale, shift)) or scale <= 0):
            _fail("supervision_depth_alignment_invalid")
    values = values.astype(np.float64)
    raw_valid_values = np.isfinite(values) & (values > 0)
    with np.errstate(over="ignore", invalid="ignore"):
        values = values * scale + shift
    valid_values = raw_valid_values & np.isfinite(values) & (values > 0)
    with np.errstate(over="ignore", invalid="ignore", under="ignore"):
        converted_confidence = confidence.astype(np.float32)
    valid_confidence = (np.isfinite(confidence) & (confidence > 0) & (confidence <= 1)
                        & np.isfinite(converted_confidence) & (converted_confidence > 0))
    if registration is not None:
        candidate = valid_values & valid_confidence & validity.astype(bool)
        payload, counts = _registered_depth(values, definition, candidate, confidence, material, labels, k,
            target_shape, target_k, unit_to_training_scale, registration, cancelled)
        counts.update(source_invalid_depth_values=int((~valid_values).sum()),
            source_invalid_confidence_pixels=int((~valid_confidence).sum()),
            source_excluded_validity_pixels=int((~validity.astype(bool)).sum()))
        payload.update(depth_definition="inverse_z", depth_frame="training.camera",
            provenance=copy.deepcopy(provenance), source_unit=record["unit"])
        return payload, counts
    valid = valid_values & valid_confidence & validity.astype(bool) & (material == labels["opaque"])
    if not valid.any():
        _fail("supervision_depth_all_invalid")
    inverse_z = np.zeros(shape, dtype=np.float64)
    with np.errstate(over="ignore", invalid="ignore", divide="ignore", under="ignore"):
        if definition == "inverse_z":
            inverse_z[valid] = values[valid] / unit_to_training_scale
        else:
            z = values.copy()
            if definition == "ray_range":
                y, x = np.indices(shape, dtype=np.float64)
                pixels = np.stack((x + .5, y + .5, np.ones(shape)), axis=-1)
                rays = pixels @ np.linalg.inv(k).T
                z /= np.linalg.norm(rays, axis=-1)
            inverse_z[valid] = 1. / (z[valid] * unit_to_training_scale)
        converted = inverse_z.astype(np.float32)
    converted_valid = np.isfinite(converted) & (converted > 0)
    excluded_conversion = int((valid & ~converted_valid).sum())
    valid &= converted_valid
    if not valid.any():
        _fail("supervision_depth_all_invalid", "unit conversion is not representable")
    inverse_z[~valid] = 0.
    weights = np.where(valid, confidence, 0.)
    counts = {"valid_depth_pixels": int(valid.sum()), "invalid_depth_values": int((~valid_values).sum()),
              "invalid_confidence_pixels": int((~valid_confidence).sum()),
              "excluded_validity_pixels": int((~validity.astype(bool)).sum()),
              "excluded_conversion_pixels": excluded_conversion}
    for name in ("unknown", "transparent", "specular", "occluded"):
        counts["excluded_" + name + "_pixels"] = int((material == labels[name]).sum())
    return {"inverse_z": _readonly(inverse_z), "depth_weights": _readonly(weights),
            "depth_definition": "inverse_z", "depth_frame": "training.camera",
            "provenance": copy.deepcopy(provenance), "source_unit": record["unit"]}, counts


def _decode_view(root, row, view, kind, unit_scale, scale_identity, allow_oracle, train_ids, cancelled):
    shape, k = _geometry(row, view)
    registration = _registration(row, view, kind, unit_scale, scale_identity)
    target_size = view.get("training_size", [view["width"], view["height"]])
    target_shape = (target_size[1], target_size[0])
    target_k = np.asarray(view.get("training_K", view["K"]), dtype=np.float64)
    payload, counts = (_mask(root, row[kind], shape, cancelled) if kind == "mask"
        else _depth(root, row[kind], shape, k, unit_scale, allow_oracle, train_ids, cancelled,
                    registration, target_shape, target_k))
    adapter = "exact_camera_geometry"
    if registration is not None:
        if kind == "mask":
            payload, counts = _registered_mask(payload, counts, shape, k, target_shape, target_k, registration, cancelled)
        adapter = row["registration"]["model"]
    return dict(payload, view_id=row["view_id"], kind=kind), counts, adapter


def _verify_assets(root, section, cancelled):
    """Recheck exact asset bytes on cache hits, without keeping decoded arrays."""
    if not isinstance(section, dict):
        return
    if "path" in section and "sha256" in section:
        relative = Path(section["path"])
        if relative.is_absolute() or ".." in relative.parts:
            _fail("supervision_asset_path_invalid")
        if hashlib.sha256(_read_bytes(root / relative, 512 * 1024 * 1024, cancelled)).hexdigest() != section["sha256"]:
            _fail("supervision_asset_hash_mismatch", relative)
    for value in section.values():
        if isinstance(value, dict):
            _verify_assets(root, value, cancelled)


def load_supervision(manifest_path, camera_contract, *, kind, allow_oracle=False,
                     unit_to_training_scale=None, scale_evidence_identity=None, cancelled=None,
                     max_cached_views=2, max_cached_bytes=256 * 1024 * 1024):
    """Decode a hash-bound manifest without changing any source or camera data.

    Preflight decodes and reports one view at a time, then drops its arrays.
    ``for_view`` performs on-demand decode with a bounded view/byte LRU.
    ``kind`` selects the mask or depth section of each view. Depth conversion
    requires a caller-resolved unit-to-training scale and its evidence identity;
    this module never guesses metric scale from scene geometry or depth values.
    """
    _poll(cancelled)
    if (type(max_cached_views) is not int or not 1 <= max_cached_views <= 64
            or type(max_cached_bytes) is not int or max_cached_bytes < 1):
        _fail("supervision_cache_budget_invalid")
    camera_contract = copy.deepcopy(camera_contract)
    cameras = _camera_index(camera_contract)
    path = Path(manifest_path).absolute()
    try:
        data = _read_bytes(path, 16 * 1024 * 1024, cancelled)
        manifest = json.loads(data.decode("utf-8"), object_pairs_hook=_json_object,
            parse_constant=lambda value: _fail("supervision_nonfinite_json", value))
    except SupervisionError:
        raise
    except (OSError, ValueError) as error:
        _fail("supervision_manifest_invalid", error)
    _fields(manifest, ("schema", "dataset_identity", "views"))
    if (manifest.get("schema") != "gsw.quality_supervision/1"
            or manifest.get("dataset_identity") != camera_contract.get("input_data_identity")):
        _fail("supervision_dataset_identity_mismatch")
    if kind not in ("mask", "depth"):
        _fail("supervision_kind_unsupported")
    if kind == "depth" and (isinstance(unit_to_training_scale, bool)
            or not isinstance(unit_to_training_scale, (int, float))
            or not np.isfinite(unit_to_training_scale) or unit_to_training_scale <= 0
            or not isinstance(scale_evidence_identity, str)
            or re.fullmatch(r"[0-9a-f]{64}", scale_evidence_identity) is None):
        _fail("supervision_scale_evidence_required")
    records, counts, per_view, source_units = {}, {}, [], set()
    if not isinstance(manifest["views"], list):
        _fail("supervision_schema_invalid")
    for row in manifest["views"]:
        _poll(cancelled)
        _fields(row, ("view_id", "split", "image_sha256", "geometry"), ("mask", "depth", "registration"))
        view_id = row["view_id"]
        if not isinstance(view_id, str) or view_id in records:
            _fail("supervision_view_duplicate_or_invalid")
        view = cameras.get(view_id)
        if view is None or row.get("split") != "train" or view["split"] != "train":
            _fail("supervision_view_not_train", view_id)
        if row.get("image_sha256") != view["sha256"]:
            _fail("supervision_image_hash_mismatch", view_id)
        if kind not in row:
            _fail("supervision_view_payload_missing", view_id)
        payload, view_counts, adapter = _decode_view(path.parent, row, view, kind, unit_to_training_scale,
            scale_evidence_identity, allow_oracle, set(camera_contract["splits"]["train"]), cancelled)
        records[view_id] = row
        if kind == "depth":
            source_units.add(payload["source_unit"])
            if len(source_units) != 1:
                _fail("supervision_mixed_depth_units", "one explicit conversion scale requires one declared unit")
        registration_result = None
        if "winner_source_index" in payload:
            registration_result = {name: {"shape": list(payload[name].shape), "dtype": payload[name].dtype.str,
                "sha256": hashlib.sha256(payload[name].tobytes()).hexdigest(), "storage": "decoded_payload_array"}
                for name in ("winner_source_index", "exclusion_reason")}
            registration_result["exclusion_reason_codes"] = copy.deepcopy(payload["exclusion_reason_codes"])
        per_view.append({"view_id": view_id, "image_sha256": row["image_sha256"],
                         "counts": copy.deepcopy(view_counts),
                         "provenance": copy.deepcopy(payload.get("provenance")),
                         "geometry_adapter": adapter,
                         "registration": copy.deepcopy(row.get("registration")),
                         "registration_result": registration_result})
        for name, count in view_counts.items():
            counts[name] = counts.get(name, 0) + count
        # Do not keep this view's arrays alive while decoding the next view.
        del payload
    expected = set(camera_contract["splits"]["train"])
    if set(records) != expected or expected != {view_id for view_id, view in cameras.items() if view["split"] == "train"}:
        _fail("supervision_view_missing")
    identity = _digest({"manifest": manifest, "kind": kind, "camera_contract": camera_contract,
        "unit_to_training_scale": unit_to_training_scale, "scale_evidence_identity": scale_evidence_identity})
    report = {"schema": "gsw.supervision_report/1", "kind": kind, "identity": identity,
              "dataset_identity": manifest["dataset_identity"], "counts": counts, "views": per_view,
              "geometry_adapters": sorted({row["geometry_adapter"] for row in per_view}),
              "reprojection_supported": any(row["registration"] is not None for row in per_view),
              "preflight_decoding": "one_view_at_a_time_metadata_only_retained"}
    if kind == "depth":
        report.update(unit_to_training_scale=unit_to_training_scale, scale_evidence_identity=scale_evidence_identity,
                      source_unit=next(iter(source_units)))
    def decode(view_id):
        return _decode_view(path.parent, records[view_id], cameras[view_id], kind, unit_to_training_scale,
            scale_evidence_identity, allow_oracle, expected, cancelled)[0]

    def verify(view_id):
        _verify_assets(path.parent, records[view_id][kind], cancelled)

    return SupervisionProvider(records, decode, verify, report, identity, camera_contract,
        max_cached_views, max_cached_bytes)


def supervision_losses(rendered_rgb, target_rgb, payload, *, predicted_depth=None,
                       rendered_alpha=None, mask_loss_weight=0., background_alpha_weight=0.,
                       depth_loss_weight=0.):
    """Return independent normalized foreground, alpha-negative and depth losses.

    This is opt-in supervision, not a change to the legacy L1/SSIM objective.
    Callers must choose the requested objective explicitly; adding a foreground
    term to an unmasked global photo loss does not exclude invalid/background
    pixels from that global loss. ``predicted_depth`` must state its convention.
    """
    import torch
    for weight in (mask_loss_weight, background_alpha_weight, depth_loss_weight):
        if (isinstance(weight, bool) or not isinstance(weight, (int, float))
                or not np.isfinite(weight) or weight < 0):
            _fail("supervision_loss_weight_invalid")
    if (rendered_rgb.ndim != 3 or rendered_rgb.shape[0] != 3
            or rendered_rgb.shape != target_rgb.shape or target_rgb.requires_grad
            or rendered_rgb.device != target_rgb.device or rendered_rgb.dtype not in (torch.float32, torch.float64)
            or target_rgb.dtype not in (torch.uint8, rendered_rgb.dtype)):
        _fail("supervision_loss_image_contract")
    target_rgb = (target_rgb.to(dtype=rendered_rgb.dtype) / 255.
                  if target_rgb.dtype == torch.uint8 else target_rgb)
    zero = rendered_rgb.new_zeros(())
    result = {"foreground_rgb": zero, "background_alpha": zero, "depth_inverse_z": zero,
              "counts": {}}
    shape = tuple(rendered_rgb.shape[-2:])

    def weighted(value, name, prediction_valid=None):
        weights = torch.tensor(payload[name], dtype=rendered_rgb.dtype, device=rendered_rgb.device)
        if tuple(weights.shape) != shape or tuple(value.shape) != shape:
            _fail("supervision_loss_shape_mismatch")
        if prediction_valid is not None:
            result["counts"]["depth_render_excluded_pixels"] = int(((weights > 0) & ~prediction_valid).sum())
            weights = torch.where(prediction_valid, weights, torch.zeros_like(weights))
        valid = weights > 0
        denominator = weights.sum()
        if (not bool(torch.isfinite(weights).all()) or bool((weights < 0).any())
                or not bool(torch.isfinite(denominator)) or float(denominator) <= 0):
            _fail("supervision_loss_all_invalid", name)
        if not bool(torch.isfinite(value[valid]).all()):
            _fail("supervision_prediction_nonfinite", name)
        result["counts"][name] = {"pixels": int(valid.sum()), "weight_sum": float(denominator)}
        return (value[valid] * weights[valid]).sum() / denominator

    if mask_loss_weight:
        if payload is None or "foreground_weights" not in payload:
            _fail("supervision_mask_payload_required")
        result["foreground_rgb"] = weighted(torch.abs(rendered_rgb - target_rgb).mean(dim=0), "foreground_weights")
    if background_alpha_weight:
        if payload is None or "background_weights" not in payload or rendered_alpha is None:
            _fail("supervision_alpha_payload_required")
        alpha = rendered_alpha.squeeze(0) if rendered_alpha.ndim == 3 and rendered_alpha.shape[0] == 1 else rendered_alpha
        result["background_alpha"] = weighted(torch.abs(alpha), "background_weights")
    if depth_loss_weight:
        if payload is None or "inverse_z" not in payload or "depth_weights" not in payload:
            _fail("supervision_depth_payload_required")
        if not isinstance(predicted_depth, dict) or predicted_depth.get("definition") not in ("inverse_z", "inverse_z_moment"):
            _fail("supervision_prediction_depth_definition_required")
        definition = predicted_depth["definition"]
        _fields(predicted_depth, ("definition", "values", "alpha") if definition == "inverse_z_moment" else ("definition", "values"))
        prediction = predicted_depth["values"]
        if prediction.ndim == 3 and prediction.shape[0] == 1:
            prediction = prediction.squeeze(0)
        if (tuple(prediction.shape) != shape or prediction.device != rendered_rgb.device
                or prediction.dtype != rendered_rgb.dtype):
            _fail("supervision_loss_shape_mismatch")
        prediction_valid = torch.isfinite(prediction) & (prediction > 0)
        if definition == "inverse_z_moment":
            alpha = predicted_depth["alpha"]
            if alpha.ndim == 3 and alpha.shape[0] == 1:
                alpha = alpha.squeeze(0)
            if (tuple(alpha.shape) != shape or alpha.device != prediction.device or alpha.dtype != prediction.dtype):
                _fail("supervision_loss_shape_mismatch")
            alpha_valid = torch.isfinite(alpha) & (alpha > 0) & (alpha <= 1)
            prediction_valid = prediction_valid & alpha_valid
            # The safe branch prevents zero/NaN alpha outside valid support
            # from creating NaN forward values or autograd derivatives.
            prediction = torch.where(prediction_valid, prediction, torch.zeros_like(prediction)) / torch.where(
                alpha_valid, alpha, torch.ones_like(alpha))
        target = torch.tensor(payload["inverse_z"], dtype=prediction.dtype, device=prediction.device)
        if tuple(target.shape) != shape:
            _fail("supervision_loss_shape_mismatch")
        prediction_valid = prediction_valid & torch.isfinite(prediction)
        safe_prediction = torch.where(prediction_valid, prediction, torch.zeros_like(prediction))
        result["depth_inverse_z"] = weighted(torch.abs(safe_prediction - target), "depth_weights", prediction_valid)
    result["total"] = (mask_loss_weight * result["foreground_rgb"]
        + background_alpha_weight * result["background_alpha"] + depth_loss_weight * result["depth_inverse_z"])
    return result
