"""CPU-only initialization recipes and geometric visibility diagnostics.

Camera coverage is not object-depth evidence. Inspection reads no evaluation
geometry, performs no pose solving, and never starts a renderer or training.
"""
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import zipfile

import numpy as np

from native.backend.camera_dataset import CameraDatasetError


REQUEST_SCHEMA = "gsw.initialization_request/1"
REPORT_SCHEMA = "gsw.initialization_report/1"


class InitializationError(CameraDatasetError):
    def __init__(self, code, detail=""):
        super().__init__(code, str(detail))


def _cancel(cancelled):
    if cancelled and cancelled():
        raise InitializationError("camera_import_cancelled")


def _request(dataset, request):
    allowed = {"schema", "input_data_identity", "backend", "resolution", "initialization", "suggestion"}
    if not isinstance(request, dict) or set(request) - allowed or request.get("schema") != REQUEST_SCHEMA:
        raise InitializationError("initialization_invalid_request", "schema or unsupported option")
    if request.get("input_data_identity") != dataset["data_identity"]:
        raise InitializationError("initialization_input_identity_mismatch")
    if request.get("backend") not in ("3dgs", "2dgs"):
        raise InitializationError("initialization_backend_unsupported")
    resolution = request.get("resolution")
    if (isinstance(resolution, bool) or not isinstance(resolution, (int, float))
            or not math.isfinite(resolution) or (resolution != -1 and resolution <= 0)):
        raise InitializationError("initialization_invalid_request", "explicit resolution")
    declaration = copy.deepcopy(request.get("initialization"))
    fields = {"kind", "bounds", "count", "seed", "frame", "path", "format", "sha256", "priors", "provenance",
              "color_policy", "point_bounds", "reason", "unused_source", "input_to_canonical", "materialization"}
    if not isinstance(declaration, dict) or set(declaration) - fields:
        raise InitializationError("initialization_invalid_request", "initialization declaration")
    if declaration.get("kind", "random") not in ("random", "external_point_cloud", "sfm_sparse"):
        raise InitializationError("initialization_kind_unsupported")
    declaration.setdefault("kind", "random")
    declaration.setdefault("frame", "source")
    declaration.setdefault("priors", [])
    if declaration["frame"] not in ("source", "canonical"):
        raise InitializationError("initialization_invalid_request", "input frame")
    if (not isinstance(declaration["priors"], list)
            or any(not isinstance(value, str) for value in declaration["priors"])):
        raise InitializationError("initialization_invalid_request", "prior declaration")
    if any(value in ("ground_truth", "evaluation_geometry", "test_geometry") for value in declaration["priors"]):
        raise InitializationError("initialization_evaluation_prior_forbidden")
    if declaration["kind"] == "random":
        if declaration.get("path"):
            raise InitializationError("initialization_invalid_request", "random recipe cannot consume geometry")
        declaration["seed"] = 0 if declaration.get("seed") is None else declaration["seed"]
        declaration["count"] = 10000 if declaration.get("count") is None else declaration["count"]
        if (type(declaration["seed"]) is not int or not 0 <= declaration["seed"] <= 2**32 - 1
                or type(declaration["count"]) is not int or not 4 <= declaration["count"] <= 10_000_000):
            raise InitializationError("initialization_invalid_request", "point count or uint32 seed")
    if declaration.get("bounds") is not None:
        value = declaration["bounds"]
        if (not isinstance(value, (list, tuple)) or len(value) != 2
                or any(not isinstance(row, (list, tuple)) or len(row) != 3 for row in value)
                or any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x)
                       for row in value for x in row)):
            raise InitializationError("initialization_invalid_request", "finite three-dimensional bounds")
        bounds = np.asarray(value, dtype=np.float64)
        if np.any(bounds[1] <= bounds[0]):
            raise InitializationError("initialization_invalid_request", "strictly increasing bounds")
        with np.errstate(over="ignore", invalid="ignore"):
            if not np.isfinite(bounds[1] - bounds[0]).all():
                raise InitializationError("initialization_invalid_request", "sampling interval exceeds binary64 range")
        declaration["bounds"] = bounds.tolist()
    material = declaration.get("materialization")
    if material is not None:
        if not isinstance(material, dict) or not isinstance(material.get("recipe"), dict):
            raise InitializationError("initialization_materialization_invalid")
        ignored = {"materialization", "input_to_canonical"}
        current_recipe = {key: value for key, value in declaration.items() if key not in ignored}
        original_recipe = {key: value for key, value in material["recipe"].items() if key not in ignored}
        if current_recipe != original_recipe:
            declaration.pop("materialization", None)
            declaration.pop("input_to_canonical", None)
    if "input_to_canonical" in declaration:
        from native.backend import camera_dataset
        value = declaration["input_to_canonical"]
        if not isinstance(value, dict) or (declaration["frame"] != "canonical"
                                          and declaration.get("materialization") is None):
            raise InitializationError("initialization_invalid_request", "initialization input transform/frame")
        try:
            transform = camera_dataset._sim3(value, "initialization_input", "canonical")
        except camera_dataset.CameraDatasetError as error:
            raise InitializationError(error.code, error.detail)
        if (transform["input_frame"] != "initialization_input" or transform["output_frame"] != "canonical"
                or transform["applied"]):
            raise InitializationError("initialization_invalid_request", "initialization transform already applied or wrong frame")
        declaration["input_to_canonical"] = transform
    normalized = dict(request, initialization=declaration)
    try:
        if len(json.dumps(normalized, sort_keys=True, allow_nan=False).encode("utf-8")) > 16 * 1024 * 1024:
            raise InitializationError("initialization_resource_limit", "request exceeds 16 MiB")
    except (TypeError, ValueError) as error:
        if isinstance(error, InitializationError):
            raise
        raise InitializationError("initialization_invalid_request", str(error))
    return normalized


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode("utf-8")).hexdigest()


def _point_digest(points, colors, declaration):
    metadata = {"schema": "gsw.canonical_initial_points/1", "generator":
                "mt19937-uniform-explicit-f64-sim3/1" if declaration["kind"] == "random" else "declared-point-cloud/1",
                "kind": declaration["kind"], "count": len(points), "seed": declaration.get("seed"),
                "priors": declaration.get("priors", [])}
    digest = hashlib.sha256(json.dumps(metadata, sort_keys=True, separators=(",", ":"),
                                      allow_nan=False).encode("utf-8") + b"\0")
    for array in (points, colors):
        digest.update(np.ascontiguousarray(array, dtype="<f8").tobytes())
    return digest.hexdigest()


def _corners(bounds):
    return np.asarray([[bounds[x][0], bounds[y][1], bounds[z][2]]
                       for x in (0, 1) for y in (0, 1) for z in (0, 1)], dtype=np.float64)


def _preview_views(dataset):
    # Only drawing is bounded. All training cameras still participate in the
    # actual visibility report; omitted drawings never imply omitted training.
    views = dataset["views"]
    step = max(1, (len(views) + 255) // 256)
    selected = views[::step]
    return {"views": [{key: copy.deepcopy(view[key]) for key in
        ("view_id", "split", "K", "width", "height", "c2w_opencv")} for view in selected],
        "total_views": len(views), "sampled_views": len(selected)}


def _transform_recipe_points(transform, points):
    # This persisted recipe is deliberately independent of BLAS/FMA reduction
    # policies. Actual NumPy 1.24/1.26 vs 2.5 probes differed by one ULP with @.
    # Keep explicit binary64 ufunc operations and retain strict point hashes.
    points = np.asarray(points, dtype=np.float64)
    rotation = np.asarray(transform["R"], dtype=np.float64)
    result = np.empty_like(points)
    with np.errstate(over="ignore", invalid="ignore"):
        for i in range(3):
            row = ((points[:, 0] * rotation[i, 0]) + (points[:, 1] * rotation[i, 1]))
            row = row + points[:, 2] * rotation[i, 2]
            result[:, i] = float(transform["s"]) * row + float(transform["t"][i])
    if not np.isfinite(result).all():
        raise InitializationError("initialization_invalid_request", "point transform exceeds binary64 range")
    return result


def _kernel_contract(backend):
    result = {"mode": "stock_fixed_positive_depth_clip", "frame": "training",
        "minimum_positive_view_z": .2, "float32_equivalent_check": True,
        "boundary_policy": "conservative_float32_roundoff_margin",
        "runtime_gpu_verified": False, "center_clip": "view_z_must_exceed_0.2"}
    if backend == "2dgs":
        result.update(per_pixel_minimum_depth=.2, distortion_near=.2, distortion_far=100.,
                      surface_intersection_visibility_not_predicted=True)
    return result


def _coverage(views, points, normalization, resolution, cancelled=None):
    from native.worker.canonical_training import _training_size, resized_intrinsics, transform_points
    localized = transform_points(normalization, points)
    if not np.isfinite(localized).all() or np.any(np.abs(localized) > np.finfo(np.float32).max):
        raise InitializationError("initialization_invalid_request", "localized points exceed renderer float32 range")
    rotation = np.asarray(normalization["R"], dtype=np.float64)
    rows = []
    union = np.zeros(len(points), dtype=bool)
    for view in views:
        _cancel(cancelled)
        if view["split"] != "train":
            continue
        pose = np.asarray(view["c2w_opencv"], dtype=np.float64).copy()
        pose[:3, 3] = transform_points(normalization, [pose[:3, 3]])[0]
        pose[:3, :3] = rotation @ pose[:3, :3]
        w2c = np.linalg.inv(pose).astype(np.float32)
        xyz = localized.astype(np.float32)
        products = xyz[:, None, :] * w2c[None, :3, :3]
        local = ((products[:, :, 0] + products[:, :, 1]) + products[:, :, 2]) + w2c[:3, 3]
        margin = (8. * np.finfo(np.float32).eps) * (
            np.abs(products[:, 2, :]).sum(axis=1, dtype=np.float64) + abs(float(w2c[2, 3])))
        boundary = np.abs(local[:, 2].astype(np.float64) - float(np.float32(.2))) <= margin
        positive = local[:, 2] > np.float32(.2)
        safe = positive & ~boundary
        size = _training_size(view["width"], view["height"], resolution)
        k = np.asarray(resized_intrinsics(view["K"], (view["width"], view["height"]), size))
        image_xy = np.full((len(points), 2), np.nan, dtype=np.float64)
        image_xy[safe] = local[safe, :2] / local[safe, 2:3]
        image_xy[safe] *= [k[0, 0], k[1, 1]]
        image_xy[safe] += [k[0, 2], k[1, 2]]
        inside = safe & (image_xy[:, 0] >= 0) & (image_xy[:, 0] < size[0]) & (
            image_xy[:, 1] >= 0) & (image_xy[:, 1] < size[1])
        union |= inside
        rows.append({"view_id": view["view_id"], "split": "train", "initial_points": len(points),
            "positive_depth": int((local[:, 2] > 0).sum()),
            "forward_past_backend_near": int(safe.sum()), "inside_image": int(inside.sum()),
            "near_boundary_uncertain": int(boundary.sum()),
            "ratios": {"forward": float(safe.mean()), "inside_image": float(inside.mean())},
            "minimum_view_z": float(local[:, 2].min()), "maximum_view_z": float(local[:, 2].max())})
    return {"per_view": rows, "any_training_view": int(union.sum()),
            "not_surface_evidence": True, "geometry_only": True,
            "visibility_definition": "conservative_point_centers_not_splat_footprints"}


def check_initialization_visibility(dataset, canonical_points, normalization, resolution, cancelled=None):
    """Shared float32/conservative point-center check used by both trainers."""
    return _coverage(dataset["views"], canonical_points, normalization, resolution, cancelled)


def resolve_initialization_points(dataset, root, cancelled=None):
    """Return (actual canonical points, colors, recipe identity), without Torch."""
    _cancel(cancelled)
    request = _request(dataset, {"schema": REQUEST_SCHEMA, "input_data_identity": dataset["data_identity"],
        "backend": "3dgs", "resolution": 1, "initialization": dataset["initialization"]})
    declaration = request["initialization"]
    if declaration.get("materialization") is not None:
        points, colors = read_materialized_initialization(declaration, root, cancelled)
        corners = np.asarray(declaration["materialization"]["bounds_corners"], dtype=np.float64)
        if declaration.get("input_to_canonical") is not None:
            points = _transform_recipe_points(declaration["input_to_canonical"], points)
            corners = _transform_recipe_points(declaration["input_to_canonical"], corners)
        return points, colors, {"initialization": declaration,
            "point_sha256": _point_digest(points, colors, declaration), "bounds_corners": corners.tolist()}
    if declaration["kind"] != "random":
        from native.backend import camera_dataset
        try:
            declaration = camera_dataset._initialization(root, declaration, cancelled)
            path, _ = camera_dataset._relative(Path(root).resolve(), declaration["path"])
            records = list(camera_dataset._point_records(path, declaration["format"], cancelled))
            points = [xyz for xyz, _ in records]
            colors = [rgb for _, rgb in records]
        except camera_dataset.CameraDatasetError as error:
            raise InitializationError(error.code, error.detail)
        points, colors = np.asarray(points, dtype=np.float64), np.asarray(colors, dtype=np.float64)
        transform = (dataset["transforms"]["source_to_canonical"] if declaration["frame"] == "source"
                     else declaration.get("input_to_canonical"))
        if transform is not None:
            points = _transform_recipe_points(transform, points)
        corners = _corners([points.min(axis=0), points.max(axis=0)])
        return points, colors, {"initialization": declaration,
            "point_sha256": _point_digest(points, colors, declaration), "bounds_corners": corners.tolist()}
    if declaration.get("bounds") is None:
        raise InitializationError("initialization_bounds_required")
    bounds = np.asarray(declaration["bounds"], dtype=np.float64)
    corners = _corners(bounds)
    random = np.random.RandomState(declaration["seed"])
    points = np.empty((declaration["count"], 3), dtype=np.float64)
    for start in range(0, len(points), 65536):
        _cancel(cancelled)
        points[start:start + 65536] = random.uniform(bounds[0], bounds[1], size=(min(65536, len(points) - start), 3))
    colors = np.full(points.shape, .5, dtype=np.float64)
    transform = (dataset["transforms"]["source_to_canonical"] if declaration["frame"] == "source"
                 else declaration.get("input_to_canonical"))
    if transform is not None:
        points, corners = _transform_recipe_points(transform, points), _transform_recipe_points(transform, corners)
    return points, colors, {"initialization": declaration, "point_sha256": _point_digest(points, colors, declaration),
                           "bounds_corners": corners.tolist()}


def read_materialized_initialization(declaration, root, cancelled=None):
    """Verify a saved owned float64 point artifact; no random prior relabelling."""
    from native.backend import camera_dataset
    _cancel(cancelled)
    material = declaration.get("materialization")
    if (not isinstance(material, dict) or material.get("schema") != "gsw.initialization_materialization/1"
            or material.get("frame") != "canonical" or type(material.get("count")) is not int
            or not 1 <= material["count"] <= 10_000_000):
        raise InitializationError("initialization_materialization_invalid")
    relative = material.get("path", "")
    if not relative.startswith("source/initialization/points/") or not relative.endswith(".npz"):
        raise InitializationError("initialization_materialization_invalid", "artifact path")
    path, _ = camera_dataset._relative(Path(root), relative)
    for entry in (path, *path.parents):
        metadata = entry.lstat()
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
            raise InitializationError("initialization_materialization_invalid", "reparse artifact")
        if entry == Path(root):
            break
    if camera_dataset._hash(path, cancelled) != material.get("sha256"):
        raise InitializationError("initialization_materialization_hash_mismatch")
    maximum = material["count"] * 3 * 8 + 16384
    try:
        with zipfile.ZipFile(path) as archive:
            entries = archive.infolist()
            if (len(entries) != 2 or {entry.filename for entry in entries} != {"points.npy", "colors.npy"}
                    or any(entry.compress_type != zipfile.ZIP_STORED or entry.file_size > maximum for entry in entries)):
                raise InitializationError("initialization_materialization_invalid", "bounded NPZ layout")
            for entry in entries:
                with archive.open(entry) as stream:
                    version = np.lib.format.read_magic(stream)
                    if version == (1, 0):
                        shape, fortran, dtype = np.lib.format.read_array_header_1_0(stream)
                    elif version == (2, 0):
                        shape, fortran, dtype = np.lib.format.read_array_header_2_0(stream)
                    else:
                        raise InitializationError("initialization_materialization_invalid", "NPY version")
                    if shape != (material["count"], 3) or fortran or dtype != np.dtype("<f8"):
                        raise InitializationError("initialization_materialization_invalid", "NPY shape/dtype")
        with np.load(path, allow_pickle=False) as values:
            points, colors = values["points"].copy(), values["colors"].copy()
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        if isinstance(error, InitializationError):
            raise
        raise InitializationError("initialization_materialization_invalid", str(error))
    if (not np.isfinite(points).all() or not np.isfinite(colors).all()
            or np.any(colors < 0) or np.any(colors > 1)
            or _point_digest(points, colors, material["recipe"]) != material.get("point_sha256")):
        raise InitializationError("initialization_materialization_hash_mismatch")
    return points, colors


def initialization_version_id(input_identity, request_digest, point_digest, npz_digest):
    return "initialization-" + _digest({"input": input_identity, "request": request_digest,
                                      "points": point_digest, "file": npz_digest})


def initialization_reference_records(dataset, root, cancelled=None):
    """Hash-bound auxiliary references from all already admitted ancestors."""
    from native.backend import camera_dataset
    snapshots = [dataset]
    for record in camera_dataset.scale_derivation_archive_records(dataset):
        _cancel(cancelled)
        prefix = ("source/initialization/inputs/" if record["path"].startswith("source/initialization/inputs/")
                  else "source/scale_constraints/inputs/")
        path = camera_dataset._scale_archive_path(root, record["path"], prefix=prefix)
        if camera_dataset._hash(path, cancelled) != record["sha256"]:
            raise InitializationError("initialization_input_manifest_hash_mismatch")
        snapshots.append(camera_dataset._read_json(path))
    records = {}
    for snapshot in snapshots:
        declaration = snapshot.get("initialization", {})
        values = []
        if declaration.get("kind") != "random" and declaration.get("path"):
            values.append({"path": declaration["path"], "sha256": declaration["sha256"]})
        if declaration.get("materialization"):
            values.append({key: declaration["materialization"][key] for key in ("path", "sha256")})
        if snapshot.get("initialization_derivation"):
            values.append(snapshot["initialization_derivation"]["report_manifest"])
        for record in values:
            if record["path"] in records and records[record["path"]] != record["sha256"]:
                raise InitializationError("initialization_conflicting_reference")
            records[record["path"]] = record["sha256"]
    return [{"path": path, "sha256": digest} for path, digest in records.items()]


def suggest_initialization(dataset, request, cancelled=None):
    """Offer an explicit hypothesis; never infer a physical object extent."""
    _cancel(cancelled)
    raw = copy.deepcopy(request)
    request = _request(dataset, request)
    suggestion = request.get("suggestion")
    if isinstance(suggestion, dict) and suggestion.get("method") == "user_depth_interval":
        return _suggest_depth_interval(dataset, raw, request)
    if not isinstance(suggestion, dict) or suggestion.get("method") != "optical_axis_convergence":
        raise InitializationError("initialization_suggestion_unsupported")
    if set(suggestion) - {"method", "view_ids", "half_extents"}:
        raise InitializationError("initialization_invalid_request", "unsupported suggestion option")
    views = [view for view in dataset["views"] if view["split"] == "train"]
    if "view_ids" in suggestion:
        ids = suggestion["view_ids"]
        if (not isinstance(ids, list) or not ids or any(not isinstance(value, str) for value in ids)
                or len(set(ids)) != len(ids)
                or any(value not in {view["view_id"] for view in views} for value in ids)):
            raise InitializationError("initialization_suggestion_requires_training_views")
        views = [view for view in views if view["view_id"] in ids]
    frame = request["initialization"]["frame"]
    poses = [np.asarray(view["source_c2w_opencv" if frame == "source" else "c2w_opencv"],
                        dtype=np.float64) for view in views]
    centers = np.asarray([pose[:3, 3] for pose in poses])
    directions = np.asarray([pose[:3, 2] for pose in poses])
    directions /= np.linalg.norm(directions, axis=1)[:, None]
    projectors = np.eye(3)[None, :, :] - directions[:, :, None] * directions[:, None, :]
    matrix = projectors.sum(axis=0)
    rhs = np.einsum("nij,nj->i", projectors, centers)
    singular = np.linalg.svd(matrix, compute_uv=False)
    rank = int((singular > max(singular[0], np.finfo(np.float64).tiny) * 1e-10).sum())
    diagnostics = {"rank": rank, "view_ids": [view["view_id"] for view in views], "frame": frame,
                   "conditioning": float(singular[0] / singular[-1]) if rank == 3 else None}
    errors = []
    candidate = None
    if rank < 3:
        errors.append("initialization_parallel_or_rank_deficient_axes")
    else:
        center = np.linalg.solve(matrix, rhs)
        depths = np.einsum("ni,ni->n", center - centers, directions)
        residuals = np.linalg.norm(np.einsum("nij,nj->ni", projectors, center - centers), axis=1)
        diagnostics.update(center=center.tolist(), positive_ray_count=int((depths > 0).sum()),
                           ray_depths=depths.tolist(), perpendicular_residuals=residuals.tolist())
        if not np.all(depths > 0):
            errors.append("initialization_axis_target_behind_camera")
        elif "half_extents" not in suggestion:
            errors.append("initialization_half_extents_required")
        else:
            half = suggestion["half_extents"]
            if (not isinstance(half, (list, tuple)) or len(half) != 3
                    or any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) or x <= 0
                           for x in half)):
                raise InitializationError("initialization_invalid_request", "positive half extents")
            candidate = copy.deepcopy(request["initialization"])
            candidate["bounds"] = [(center - half).tolist(), (center + half).tolist()]
            provenance = copy.deepcopy(candidate.get("provenance", {}))
            if not isinstance(provenance, dict):
                raise InitializationError("initialization_invalid_request", "provenance object")
            provenance["derived_suggestion"] = {"method": suggestion["method"], "parameters": suggestion,
                "input_data_identity": dataset["data_identity"], "not_surface_evidence": True}
            candidate["provenance"] = provenance
    return _suggestion_output(dataset, raw, request, candidate, diagnostics, errors,
        ["optical_axes_are_only_a_target_center_hypothesis", "object_extent_is_user_supplied_not_observed"])


def _suggestion_output(dataset, raw, request, candidate, diagnostics, errors, assumptions):
    frame = request["initialization"]["frame"]
    report = {"schema": REPORT_SCHEMA, "status": "suggestion_requires_confirmation" if candidate else "needs_input",
        "request": raw, "normalized_request": request, "raw_request_sha256": _digest(raw),
        "request_sha256": _digest(request), "input_data_identity": dataset["data_identity"],
        "backend": request["backend"], "resolution": request["resolution"],
        "initialization": request["initialization"], "point_count": 0, "point_sha256": None,
        "suggestion_diagnostics": diagnostics, "errors": errors, "warnings": [],
        "assumptions": assumptions,
        "frames": {"source_unit": dataset["scale"]["source_unit"], "canonical_unit": dataset["scale"]["canonical_unit"],
                   "scale_status": dataset["scale"]["status"]},
        "preview": {"frame": "canonical", "display_origin": [0., 0., 0.], "points": [],
            "bounds_corners": [], **_preview_views(dataset)}}
    if candidate is not None:
        report["suggested_initialization"] = candidate
        corners = _corners(candidate["bounds"])
        if frame == "source":
            corners = _transform_recipe_points(dataset["transforms"]["source_to_canonical"], corners)
        report["preview"]["bounds_corners"] = corners.tolist()
        report["preview"]["display_origin"] = corners.mean(axis=0).tolist()
    return report


def _suggest_depth_interval(dataset, raw, request):
    suggestion = request["suggestion"]
    if set(suggestion) - {"method", "view_id", "near", "far"}:
        raise InitializationError("initialization_invalid_request", "unsupported depth interval option")
    views = {view["view_id"]: view for view in dataset["views"] if view["split"] == "train"}
    selected = suggestion.get("view_id")
    if not isinstance(selected, str) or selected not in views:
        raise InitializationError("initialization_suggestion_requires_training_views")
    diagnostics = {"selected_view_id": selected, "depth_definition": "camera_z",
                   "frame": request["initialization"]["frame"]}
    if "near" not in suggestion or "far" not in suggestion:
        return _suggestion_output(dataset, raw, request, None, diagnostics,
            ["initialization_depth_interval_required"], ["selected_view_frustum_aabb_is_not_object_bounds"])
    near, far = suggestion["near"], suggestion["far"]
    if (any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in (near, far))
            or not 0 < near < far):
        raise InitializationError("initialization_invalid_request", "positive ordered camera-Z depths")
    view = views[selected]
    pose = np.asarray(view["source_c2w_opencv" if diagnostics["frame"] == "source" else "c2w_opencv"])
    inverse_k = np.linalg.inv(np.asarray(view["K"], dtype=np.float64))
    pixels = np.asarray([[0., 0., 1.], [view["width"], 0., 1.],
        [view["width"], view["height"], 1.], [0., view["height"], 1.]])
    rays = pixels @ inverse_k.T
    camera_corners = np.concatenate((rays * near, rays * far))
    corners = camera_corners @ pose[:3, :3].T + pose[:3, 3]
    candidate = copy.deepcopy(request["initialization"])
    candidate["bounds"] = [corners.min(axis=0).tolist(), corners.max(axis=0).tolist()]
    provenance = copy.deepcopy(candidate.get("provenance", {}))
    if not isinstance(provenance, dict):
        raise InitializationError("initialization_invalid_request", "provenance object")
    provenance["derived_suggestion"] = {"method": "user_depth_interval", "parameters": suggestion,
        "input_data_identity": dataset["data_identity"], "not_surface_evidence": True}
    candidate["provenance"] = provenance
    diagnostics.update(near=near, far=far, frustum_corners=corners.tolist())
    return _suggestion_output(dataset, raw, request, candidate, diagnostics, [],
        ["selected_view_frustum_aabb_is_not_object_bounds", "depth_interval_is_explicit_user_input_not_observed_depth"])


def verify_initialization_derivation(dataset, root, cancelled=None, context=None):
    """Replay an initializer-only edit; all other admitted data stay exact."""
    from native.backend import camera_dataset
    _cancel(cancelled)
    derivation = dataset.get("initialization_derivation")
    if not isinstance(derivation, dict) or derivation.get("schema") != "gsw.initialization_derivation/1":
        raise InitializationError("initialization_derivation_invalid")
    context = context or {"depth": 0, "bytes": 0, "seen": set()}
    context["bytes"] += len(json.dumps(derivation, sort_keys=True, allow_nan=False).encode("utf-8"))
    if context["depth"] >= camera_dataset._SCALE_DERIVATION_DEPTH:
        raise InitializationError("initialization_derivation_depth_exceeded")
    record = derivation.get("input_manifest", {})
    archive = camera_dataset._scale_archive_path(root, record.get("path", ""),
                                                prefix="source/initialization/inputs/")
    token = (record.get("path"), record.get("sha256"))
    if token in context["seen"]:
        raise InitializationError("initialization_derivation_cycle")
    context["bytes"] += archive.stat().st_size
    if context["bytes"] > camera_dataset._SCALE_DERIVATION_LIMIT:
        raise InitializationError("initialization_derivation_too_large")
    if camera_dataset._hash(archive, cancelled) != record.get("sha256"):
        raise InitializationError("initialization_input_manifest_hash_mismatch")
    context["seen"].add(token)
    context["depth"] += 1
    try:
        original = camera_dataset.validate_camera_dataset(camera_dataset._read_json(archive), root,
            cancelled=cancelled, _derivation_context=context)
    finally:
        context["depth"] -= 1
        context["seen"].remove(token)
    if original["data_identity"] != record.get("data_identity"):
        raise InitializationError("initialization_input_identity_mismatch")
    if derivation.get("archives") != camera_dataset.scale_derivation_archive_records(original):
        raise InitializationError("initialization_derivation_invalid", "archives")
    request = derivation.get("request")
    report = inspect_initialization(original, root, request, cancelled)
    if report["status"] != "ready" or report["request_sha256"] != derivation.get("request_sha256"):
        raise InitializationError("initialization_derivation_invalid", "request or readiness")
    report_record = derivation.get("report_manifest", {})
    report_path = camera_dataset._scale_archive_path(root, report_record.get("path", ""),
                                                    prefix="source/initialization/reports/")
    context["bytes"] += report_path.stat().st_size
    if context["bytes"] > camera_dataset._SCALE_DERIVATION_LIMIT:
        raise InitializationError("initialization_derivation_too_large")
    if camera_dataset._hash(report_path, cancelled) != report_record.get("sha256"):
        raise InitializationError("initialization_report_hash_mismatch")
    saved_report = camera_dataset._read_json(report_path)
    for key in ("schema", "status", "request", "normalized_request", "raw_request_sha256", "request_sha256",
                "input_data_identity", "backend", "resolution", "initialization", "point_count", "point_sha256",
                "frames", "clipping", "assumptions", "errors"):
        if saved_report.get(key) != report.get(key):
            raise InitializationError("initialization_report_replay_mismatch", key)
    material = dataset["initialization"].get("materialization")
    if not isinstance(material, dict) or material.get("frame_data_identity") != original["data_identity"]:
        raise InitializationError("initialization_derivation_invalid", "artifact frame identity")
    expected_declaration = copy.deepcopy(report["initialization"])
    expected_declaration["materialization"] = copy.deepcopy(material)
    if dataset["initialization"] != expected_declaration or material.get("recipe") != report["initialization"]:
        raise InitializationError("initialization_derivation_invalid", "recipe")
    points, colors = read_materialized_initialization(dataset["initialization"], root, cancelled)
    if (_point_digest(points, colors, report["initialization"]) != report["point_sha256"]
            or material.get("point_sha256") != report["point_sha256"]
            or material.get("bounds_corners") != report["preview"]["bounds_corners"]):
        raise InitializationError("initialization_derivation_invalid", "generated point replay")
    wanted_version = initialization_version_id(original["data_identity"], report["request_sha256"],
                                              report["point_sha256"], material["sha256"])
    if dataset.get("data_version") != wanted_version:
        raise InitializationError("initialization_derivation_invalid", "version")
    actual_other, original_other = copy.deepcopy(dataset), copy.deepcopy(original)
    for value in (actual_other, original_other):
        for key in ("initialization", "initialization_derivation", "data_version", "data_identity"):
            value.pop(key, None)
    if "verification_origin" in original_other["scale"]:
        actual_other["scale"]["verification_origin"] = original_other["scale"]["verification_origin"]
    if actual_other != original_other:
        raise InitializationError("initialization_derivation_invalid", "non-initialization data changed")
    if "verification_origin" in original["scale"]:
        dataset["scale"]["verification_origin"] = original["scale"]["verification_origin"]


def inspect_initialization(dataset, root, request, cancelled=None):
    """Inspect actual recipe points using only the declared training cameras."""
    from native.worker.canonical_training import training_normalization, transform_points
    _cancel(cancelled)
    raw_request = copy.deepcopy(request)
    request = _request(dataset, request)
    declaration = copy.deepcopy(request["initialization"])
    if declaration["kind"] == "random" and declaration.get("bounds") is None:
        return {"schema": REPORT_SCHEMA, "status": "needs_input",
            "request": raw_request, "normalized_request": request,
            "raw_request_sha256": _digest(raw_request),
            "input_data_identity": dataset["data_identity"], "request_sha256": _digest(request),
            "backend": request["backend"], "resolution": request["resolution"],
            "initialization": declaration, "point_count": 0, "point_sha256": None,
            "frames": {"canonical_unit": dataset["scale"]["canonical_unit"],
                "source_unit": dataset["scale"]["source_unit"], "scale_status": dataset["scale"]["status"]},
            "errors": ["initialization_bounds_required"],
            "assumptions": ["camera_trajectory_does_not_determine_object_depth_or_extent"], "warnings": [],
            "preview": {"frame": "canonical", "display_origin": [0., 0., 0.], "points": [],
                "bounds_corners": [], **_preview_views(dataset)}}
    configured_dataset = dict(dataset, initialization=declaration)
    points, colors, resolved = resolve_initialization_points(configured_dataset, root, cancelled)
    declaration = resolved["initialization"]
    bounds = declaration.get("bounds")
    canonical_corners = np.asarray(resolved["bounds_corners"], dtype=np.float64)
    source_to = dataset["transforms"]["source_to_canonical"]
    train = [view for view in dataset["views"] if view["split"] == "train"]
    configured = dataset["transforms"]["canonical_to_training"]
    try:
        normalization = training_normalization(train, points=np.concatenate((points, canonical_corners)),
            explicit=configured if configured.get("mode") == "explicit" else None)
    except ValueError as error:
        raise InitializationError("initialization_invalid_request", str(error))
    normalization["applied"] = True
    training_corners = transform_points(normalization, canonical_corners)
    coverage = check_initialization_visibility(dataset, points, normalization, request["resolution"], cancelled)
    ready = all(row["inside_image"] > 0 for row in coverage["per_view"])
    errors = []
    if any(row["forward_past_backend_near"] == 0 for row in coverage["per_view"]):
        errors.append("initialization_backend_near_excludes_all_points")
    if any(row["inside_image"] == 0 and row["forward_past_backend_near"] > 0
           for row in coverage["per_view"]):
        errors.append("initialization_no_points_inside_training_image")
    step = max(1, (len(points) + 2047) // 2048)
    return {"schema": REPORT_SCHEMA, "status": "ready" if ready else "invalid",
        "request": raw_request, "normalized_request": request, "raw_request_sha256": _digest(raw_request),
        "input_data_identity": dataset["data_identity"], "request_sha256": _digest(request),
        "backend": request["backend"], "resolution": request["resolution"],
        "initialization": declaration, "point_count": len(points),
        "point_sha256": _point_digest(points, colors, declaration),
        "frames": {"canonical_unit": dataset["scale"]["canonical_unit"],
            "source_unit": dataset["scale"]["source_unit"], "scale_status": dataset["scale"]["status"],
            "training_unit": "numerical_training_units"},
        "bounds": {"declared": bounds, "frame": declaration["frame"],
            "canonical": [canonical_corners.min(axis=0).tolist(), canonical_corners.max(axis=0).tolist()],
            "training": [training_corners.min(axis=0).tolist(), training_corners.max(axis=0).tolist()]},
        "transforms": {"source_to_canonical": copy.deepcopy(source_to), "canonical_to_training": normalization},
        "clipping": _kernel_contract(request["backend"]), "coverage": coverage,
        "assumptions": [], "warnings": ([{"code": "initialization_materialization_invalidated_by_recipe_change",
            "detail": "The edited recipe was regenerated; its old saved points were not reused."}]
            if raw_request.get("initialization", {}).get("materialization") is not None
               and declaration.get("materialization") is None else []), "errors": errors,
        "preview": {"frame": "canonical", "display_origin": points.mean(axis=0).tolist(),
            "points": points[::step].tolist(), "bounds_corners": canonical_corners.tolist(),
            **_preview_views(dataset)}}
