"""Independent, reproducible geometry measurements; never imports a trainer.

Known transforms are applied before measurement. No automatic registration,
scale fit, hole filling or reference feedback is performed by the main result.
"""
import copy
import hashlib
import json
import math
import os
from pathlib import Path

import numpy as np

from native.backend.atomic_file import atomic_sibling_directory


SCHEMA = "gsw.geometry_evaluation_request/1"
_UNITS = {"m": 1., "cm": .01, "mm": .001}


class GeometryEvaluationError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _fail(code, detail=""):
    raise GeometryEvaluationError(code, detail)


def _poll(cancelled):
    if cancelled and cancelled():
        _fail("geometry_evaluation_cancelled")


def _sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _number(value, name, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or (positive and value <= 0):
        _fail("geometry_invalid_number", name)
    return float(value)


def _array(value, shape, name):
    try:
        if np.asarray(value).dtype.kind not in ("i", "u", "f"):
            _fail("geometry_invalid_coordinates", name)
        result = np.asarray(value, dtype=np.float64)
    except GeometryEvaluationError:
        raise
    except (ValueError, TypeError):
        _fail("geometry_invalid_coordinates", name)
    if result.shape != shape or not np.isfinite(result).all():
        _fail("geometry_invalid_coordinates", name)
    return result


def _unit_ratio(unit, evaluation_unit, declared_meters_per_unit=None):
    if declared_meters_per_unit is not None:
        declared = _number(declared_meters_per_unit, "declared metres per coordinate", positive=True)
        if unit in _UNITS and not math.isclose(declared, _UNITS[unit], rel_tol=1e-12, abs_tol=0.):
            _fail("geometry_inconsistent_unit_declaration")
        if unit == "unknown":
            _fail("geometry_inconsistent_unknown_unit_declaration")
        if evaluation_unit in _UNITS:
            return declared / _UNITS[evaluation_unit]
    if unit in _UNITS and evaluation_unit in _UNITS:
        return _UNITS[unit] / _UNITS[evaluation_unit]
    if unit == evaluation_unit == "unknown":
        return 1.
    _fail("geometry_unknown_or_incompatible_units", "{} -> {}".format(unit, evaluation_unit))


def _transform(record, evaluation):
    frame = record.get("frame")
    if not isinstance(frame, str) or not frame:
        _fail("geometry_frame_required")
    unit_ratio = _unit_ratio(record.get("unit"), evaluation["unit"], record.get("meters_per_unit"))
    value = record.get("to_evaluation")
    if not isinstance(value, dict) or value.get("input_frame") != frame or value.get("output_frame") != evaluation["id"]:
        _fail("geometry_explicit_known_transform_required")
    if set(value) - {"s", "R", "t", "input_frame", "output_frame", "applied", "provenance"}:
        _fail("geometry_unsupported_transform_options")
    if "applied" in value and type(value["applied"]) is not bool:
        _fail("geometry_invalid_transform_application_state")
    if value.get("applied", False):
        _fail("geometry_transform_already_applied")
    rotation = _array(value.get("R"), (3, 3), "rotation")
    if not np.allclose(rotation.T @ rotation, np.eye(3), rtol=0., atol=1e-10) or abs(np.linalg.det(rotation) - 1.) > 1e-10:
        _fail("geometry_invalid_proper_rotation")
    scale = _number(value.get("s"), "transform scale", positive=True)
    translation = _array(value.get("t"), (3,), "translation")
    return scale, rotation, translation, {"declared": copy.deepcopy(value),
        "applied_once": True, "declared_unit_conversion": unit_ratio,
        "scale_relative_to_unit_conversion": scale / unit_ratio,
        "authority": "explicit_user_recorded_known_transform_not_automatically_fitted"}


def _load(record, evaluation, base, cancelled):
    _poll(cancelled)
    if not isinstance(record, dict):
        _fail("geometry_invalid_input_descriptor")
    common = {"frame", "unit", "meters_per_unit", "to_evaluation", "provenance"}
    if "manifest_path" in record:
        allowed = common | {"manifest_path"}
    elif record.get("kind") == "plane_patch":
        allowed = common | {"kind", "origin", "axis_u", "axis_v", "range_u", "range_v"}
    else:
        allowed = common | {"path", "sha256", "kind"}
        if record.get("kind") not in (None, "points", "mesh"):
            _fail("geometry_unsupported_input_kind")
    if set(record) - allowed:
        _fail("geometry_unsupported_or_conflicting_input_options")
    if not isinstance(record.get("provenance", {}), dict):
        _fail("geometry_invalid_input_provenance")
    record = copy.deepcopy(record)
    manifest, manifest_input = None, None
    if record.get("manifest_path") is not None:
        if "path" in record:
            _fail("geometry_ambiguous_manifest_and_ply_input")
        path = Path(record["manifest_path"])
        path = (base / path).resolve() if not path.is_absolute() else path.resolve()
        if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
            _fail("geometry_manifest_missing_or_too_large")
        raw = path.read_bytes()
        def pairs(items):
            value = {}
            for key, item in items:
                if key in value:
                    _fail("geometry_duplicate_manifest_key", key)
                value[key] = item
            return value
        def constant(value):
            _fail("geometry_nonfinite_manifest", value)
        data = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=pairs, parse_constant=constant)
        geometry = data.get("geometry", {})
        if (data.get("schema") != "gsw.gs_geometry/1" or geometry.get("kind") != "depth_point_cloud" or
                geometry.get("surface_semantics") != "appearance_proxy_not_material_boundary"):
            _fail("geometry_unsupported_or_ambiguous_product_manifest")
        relative = Path(geometry.get("path", ""))
        if relative.is_absolute() or ".." in relative.parts or not relative.name:
            _fail("geometry_unsafe_product_path")
        cloud = (path.parent / relative).resolve()
        try:
            cloud.relative_to(path.parent)
        except ValueError:
            _fail("geometry_unsafe_product_path")
        frame = geometry.get("frame_id") or geometry.get("frame")
        for key, declared in (("frame", frame), ("unit", geometry.get("unit")),
                              ("meters_per_unit", geometry.get("meters_per_unit"))):
            if key in record and record[key] != declared:
                _fail("geometry_product_frame_or_unit_mismatch", key)
            record[key] = declared
        record.update(path=str(cloud), sha256=geometry.get("sha256"))
        manifest_input = (path, hashlib.sha256(raw).hexdigest())
        manifest = {"path": str(path), "sha256": manifest_input[1], "geometry": geometry,
                    "source": data.get("source", {}), "frames": data.get("frames", {}),
                    "view_count": len(data.get("views", [])),
                    "verification": "product_geometry_hash_count_and_declared_frame_not_source_authenticity_attestation"}
    scale, rotation, translation, transform_record = _transform(record, evaluation)
    source = {"frame": record["frame"], "unit": record["unit"],
              "transform": transform_record, "provenance": copy.deepcopy(record.get("provenance", {}))}
    if record.get("kind") == "plane_patch":
        origin = _array(record.get("origin"), (3,), "plane origin")
        u = _array(record.get("axis_u"), (3,), "plane u")
        v = _array(record.get("axis_v"), (3,), "plane v")
        if (abs(np.linalg.norm(u) - 1.) > 1e-10 or abs(np.linalg.norm(v) - 1.) > 1e-10 or abs(u @ v) > 1e-10):
            _fail("geometry_plane_basis_not_orthonormal")
        ru = _array(record.get("range_u"), (2,), "plane range u")
        rv = _array(record.get("range_v"), (2,), "plane range v")
        if ru[1] <= ru[0] or rv[1] <= rv[0]:
            _fail("geometry_degenerate_plane_patch")
        origin, u, v, ru, rv = scale * rotation @ origin + translation, rotation @ u, rotation @ v, ru * scale, rv * scale
        points = np.asarray([origin + a * u + b * v for a, b in ((ru[0], rv[0]), (ru[1], rv[0]), (ru[1], rv[1]), (ru[0], rv[1]))])
        source.update(kind="bounded_analytic_plane", definition=copy.deepcopy(record))
        return {"kind": "plane", "points": points, "origin": origin, "u": u, "v": v,
                "range_u": ru, "range_v": rv, "source": source, "input": None}
    path = Path(record.get("path", ""))
    path = (base / path).resolve() if not path.is_absolute() else path.resolve()
    if not path.is_file() or path.suffix.lower() != ".ply":
        _fail("geometry_ply_input_required", path)
    digest = _sha(path)
    if record.get("sha256", digest) != digest:
        _fail("geometry_input_hash_mismatch", path)
    from plyfile import PlyData
    ply = PlyData.read(str(path), mmap=False)
    try:
        vertex = ply["vertex"].data
        names = set(vertex.dtype.names or ())
        if "opacity" in names and ("f_dc_0" in names or "scale_0" in names or "rot_0" in names):
            _fail("geometry_gaussian_centres_not_surface", path)
        points = np.column_stack([vertex[name] for name in ("x", "y", "z")]).astype(np.float64)
    except GeometryEvaluationError:
        raise
    except (KeyError, ValueError, TypeError):
        _fail("geometry_invalid_ply_coordinates", path)
    if not np.isfinite(points).all():
        _fail("geometry_nonfinite_input", path)
    if len(points) > 10_000_000:
        _fail("geometry_resource_limit", "maximum input vertices exceeded")
    points = scale * points @ rotation.T + translation
    if not np.isfinite(points).all():
        _fail("geometry_transform_numeric_range")
    faces = np.empty((0, 3), dtype=np.int64)
    if "face" in ply and len(ply["face"].data):
        fields = ply["face"].data.dtype.names
        key = "vertex_indices" if "vertex_indices" in fields else "vertex_index"
        try:
            rows = ply["face"].data[key]
            if any(len(row) != 3 for row in rows):
                _fail("geometry_triangle_mesh_required")
            raw_faces = np.asarray([list(row) for row in rows])
            # A PLY float list is not an index list. In particular, converting
            # 1.9 to int64 must never silently select vertex 1.
            if raw_faces.dtype.kind not in ("i", "u"):
                _fail("geometry_invalid_ply_indices", path)
            faces = np.asarray(raw_faces, dtype=np.int64)
        except GeometryEvaluationError:
            raise
        except (KeyError, ValueError, TypeError):
            _fail("geometry_invalid_ply_faces", path)
        if np.any(faces < 0) or np.any(faces >= len(points)):
            _fail("geometry_invalid_ply_indices", path)
        triangles = points[faces]
        areas = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]), axis=1)
        if np.any(~np.isfinite(areas)) or np.any(areas <= 0):
            _fail("geometry_degenerate_triangle", path)
    kind = "mesh" if len(faces) else "points"
    if record.get("kind") is not None and record["kind"] != kind:
        _fail("geometry_declared_input_kind_mismatch")
    if manifest is not None:
        if kind != "points" or type(manifest["geometry"].get("point_count")) is not int or manifest["geometry"]["point_count"] != len(points):
            _fail("geometry_product_point_count_or_type_mismatch")
        source["geometry_manifest"] = manifest
    source.update(kind=kind, path=str(path), sha256=digest, vertices=len(points), triangles=len(faces))
    return {"kind": kind, "points": points, "faces": faces, "source": source, "input": (path, digest),
            "manifest_input": manifest_input}


def _sample(geometry, count, rng, grid_shape=None):
    points = geometry["points"]
    if geometry["kind"] == "plane":
        if grid_shape is not None:
            if (not isinstance(grid_shape, list) or len(grid_shape) != 2 or
                    any(type(n) is not int or n < 1 for n in grid_shape) or grid_shape[0] * grid_shape[1] != count):
                _fail("geometry_invalid_reference_grid")
            u = (np.arange(grid_shape[0], dtype=np.float64) + .5) / grid_shape[0]
            v = (np.arange(grid_shape[1], dtype=np.float64) + .5) / grid_shape[1]
            a, b = np.meshgrid(u, v, indexing="xy")
            coords = np.column_stack((a.ravel(), b.ravel()))
            coords *= [np.ptp(geometry["range_u"]), np.ptp(geometry["range_v"])]
            coords += [geometry["range_u"][0], geometry["range_v"][0]]
        else:
            coords = rng.uniform([geometry["range_u"][0], geometry["range_v"][0]],
                                 [geometry["range_u"][1], geometry["range_v"][1]], size=(count, 2))
        return geometry["origin"] + coords[:, :1] * geometry["u"] + coords[:, 1:] * geometry["v"]
    if grid_shape is not None:
        _fail("geometry_reference_grid_requires_analytic_plane")
    if geometry["kind"] == "mesh":
        triangles = points[geometry["faces"]]
        area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]), axis=1)
        selected = triangles[rng.choice(len(triangles), count, p=area / area.sum())]
        u, v = np.sqrt(rng.uniform(size=(count, 1))), rng.uniform(size=(count, 1))
        return (1. - u) * selected[:, 0] + u * (1. - v) * selected[:, 1] + u * v * selected[:, 2]
    if len(points) <= count:
        return points.copy()
    return points[rng.choice(len(points), count, replace=False)]


def _distances(query, geometry, cancelled):
    if not len(query):
        return np.empty(0), "no_query_samples"
    if not len(geometry["points"]):
        return np.full(len(query), np.inf), "empty_target"
    if geometry["kind"] == "plane":
        offset = query - geometry["origin"]
        u = np.clip(offset @ geometry["u"], *geometry["range_u"])
        v = np.clip(offset @ geometry["v"], *geometry["range_v"])
        nearest = geometry["origin"] + u[:, None] * geometry["u"] + v[:, None] * geometry["v"]
        return np.linalg.norm(query - nearest, axis=1), "point_to_bounded_analytic_plane"
    if geometry["kind"] == "points":
        try:
            from scipy.spatial import cKDTree
        except ImportError:
            cKDTree = None
        if cKDTree is not None:
            _poll(cancelled)
            return cKDTree(geometry["points"]).query(query, k=1, eps=0., p=2.)[0], "nearest_point_euclidean_scipy"
    target = geometry["points"] if geometry["kind"] == "points" else geometry["points"][geometry["faces"]]
    if len(query) * len(target) > 200_000_000:
        _fail("geometry_distance_resource_limit", "explicitly reduce sampling or use an accelerated evaluator")
    result = np.full(len(query), np.inf)
    for first in range(0, len(query), 128):
        _poll(cancelled)
        p = query[first:first + 128, None, :]
        best = np.full(len(p), np.inf)
        for start in range(0, len(target), 256):
            _poll(cancelled)
            block = target[start:start + 256]
            if geometry["kind"] == "points":
                square = ((p - block[None, :, :]) ** 2).sum(axis=2)
            else:
                a, b, c = block[:, 0], block[:, 1], block[:, 2]
                ab, ac = b - a, c - a
                normal = np.cross(ab, ac)
                normal_length = np.linalg.norm(normal, axis=1)
                unit_normal = normal / normal_length[:, None]
                pa = p - a
                height = (pa * unit_normal).sum(axis=2)
                projected = pa - height[:, :, None] * unit_normal
                # Oriented edge half-spaces avoid catastrophic cancellation in
                # (ab.ab)*(ac.ac)-(ab.ac)^2 on legitimate skinny triangles.
                inside = np.ones(height.shape, dtype=bool)
                for left, right in ((a, b), (b, c), (c, a)):
                    oriented = np.cross(right - left, projected - (left - a))
                    inside &= (oriented * unit_normal).sum(axis=2) >= 0.
                square = np.where(inside, height ** 2, np.inf)
                for left, right in ((a, b), (b, c), (c, a)):
                    edge = right - left
                    t = np.clip(((p - left) * edge).sum(axis=2) / (edge * edge).sum(axis=1), 0., 1.)
                    edge_square = ((p - left - t[:, :, None] * edge) ** 2).sum(axis=2)
                    square = np.minimum(square, edge_square)
            best = np.minimum(best, square.min(axis=1))
        result[first:first + len(p)] = np.sqrt(best)
    return result, "nearest_point_euclidean_numpy" if geometry["kind"] == "points" else "point_to_triangle_surface_unsigned_float64"


def _statistics(values, quantiles):
    finite = values[np.isfinite(values)]
    return {"sample_count": len(values), "finite_count": len(finite),
        "missing_or_nonfinite_count": len(values) - len(finite),
        "mean": float(finite.mean()) if len(finite) else None,
        "median": float(np.median(finite)) if len(finite) else None,
        "rmse": float(np.sqrt(np.mean(finite ** 2))) if len(finite) else None,
        "quantiles": [{"probability": q, "value": float(np.quantile(finite, q)) if len(finite) else None} for q in quantiles],
        "aggregation": "finite_distances_only_missing_count_and_completeness_reported_separately"}


def _roi(record, evaluation):
    if record is None:
        return None
    if (not isinstance(record, dict) or set(record) != {"min", "max", "frame", "unit"} or
            record.get("frame") != evaluation["id"] or record.get("unit") != evaluation["unit"]):
        _fail("geometry_roi_frame_or_unit_mismatch")
    low = _array(record.get("min"), (3,), "ROI minimum")
    high = _array(record.get("max"), (3,), "ROI maximum")
    if np.any(high < low) or np.all(high == low):
        _fail("geometry_degenerate_roi")
    return low, high


def _in_roi(points, roi):
    if roi is None:
        return np.ones(len(points), dtype=bool)
    return np.all((points >= roi[0]) & (points <= roi[1]), axis=1)


def _dimensions(candidate, reference, definitions, unit):
    if not isinstance(definitions, list) or len(definitions) > 4096:
        _fail("geometry_invalid_dimension_definitions")
    results, ids = [], set()
    for definition in definitions:
        if not isinstance(definition, dict) or definition.get("kind") != "axis_extent":
            _fail("geometry_unsupported_dimension_feature")
        if set(definition) - {"id", "kind", "axis", "quantile_bounds"}:
            _fail("geometry_unsupported_dimension_options")
        feature_id = definition.get("id")
        if not isinstance(feature_id, str) or not feature_id or feature_id in ids:
            _fail("geometry_dimension_id_required_or_duplicate")
        ids.add(feature_id)
        axis = _array(definition.get("axis"), (3,), "dimension axis")
        length = float(np.linalg.norm(axis))
        if not math.isfinite(length) or length <= 0:
            _fail("geometry_invalid_dimension_axis")
        axis /= length
        interval = _array(definition.get("quantile_bounds", [0., 1.]), (2,), "dimension quantile bounds")
        if not 0 <= interval[0] < interval[1] <= 1:
            _fail("geometry_invalid_dimension_quantiles")
        values = []
        for geometry in (candidate, reference):
            points = geometry["points"]
            if not len(points):
                values.append(None)
                continue
            projection = (points - points[0]) @ axis
            limits = np.quantile(projection, interval)
            values.append(float(limits[1] - limits[0]))
        measured, truth = values
        bias = measured - truth if measured is not None and truth is not None else None
        results.append({"id": feature_id, "kind": "axis_extent", "unit": unit,
            "axis": axis.tolist(), "quantile_bounds": interval.tolist(),
            "selection": "all_supplied_points_or_mesh_vertices_not_random_surface_samples",
            "candidate_selected_count": len(candidate["points"]), "reference_selected_count": len(reference["points"]),
            "candidate_value": measured, "reference_value": truth, "signed_bias": bias,
            "absolute_bias": abs(bias) if bias is not None else None,
            "relative_bias": bias / truth if bias is not None and truth > 0 else None,
            "status": "measured_feature_extent" if bias is not None else "unavailable_missing_geometry",
            "scope": "user_defined_feature_not_automatic_physical_object_dimension"})
    return results


def _registration_diagnostic(definition, candidate, reference, a, b, evaluation, threshold, quantiles, cancelled):
    if (not isinstance(definition, dict) or definition.get("kind") not in ("rigid", "similarity") or
            set(definition) - {"kind", "candidate_indices", "reference_indices", "region"}):
        _fail("geometry_invalid_diagnostic_registration")
    region = _roi(definition.get("region"), evaluation)
    if region is None:
        _fail("geometry_registration_region_required")
    left, right = definition.get("candidate_indices"), definition.get("reference_indices")
    if (not isinstance(left, list) or not isinstance(right, list) or len(left) != len(right) or len(left) < 3 or
            len(left) > 4096 or any(type(i) is not int for i in left + right) or
            len(set(left)) != len(left) or len(set(right)) != len(right) or
            min(left + right) < 0 or max(left) >= len(candidate["points"]) or max(right) >= len(reference["points"])):
        _fail("geometry_invalid_registration_correspondences")
    x, y = candidate["points"][left], reference["points"][right]
    if not _in_roi(x, region).all() or not _in_roi(y, region).all():
        _fail("geometry_registration_anchor_outside_region")
    mx, my = x.mean(axis=0), y.mean(axis=0)
    xc, yc = x - mx, y - my
    if np.linalg.matrix_rank(xc) < 2 or np.linalg.matrix_rank(yc) < 2:
        _fail("geometry_degenerate_registration_anchors")
    u, singular, vh = np.linalg.svd(xc.T @ yc)
    diagonal = np.ones(3)
    diagonal[-1] = 1. if np.linalg.det(vh.T @ u.T) > 0 else -1.
    rotation = (vh.T * diagonal) @ u.T
    scale = float((singular * diagonal).sum() / (xc * xc).sum()) if definition["kind"] == "similarity" else 1.
    translation = my - scale * (rotation @ mx)
    if not math.isfinite(scale) or scale <= 0 or not np.isfinite(translation).all():
        _fail("geometry_registration_numeric_range")
    fitted = copy.deepcopy(candidate)
    fitted["points"] = scale * candidate["points"] @ rotation.T + translation
    if fitted["kind"] == "plane":
        fitted["origin"] = scale * rotation @ candidate["origin"] + translation
        fitted["u"], fitted["v"] = rotation @ candidate["u"], rotation @ candidate["v"]
        fitted["range_u"], fitted["range_v"] = candidate["range_u"] * scale, candidate["range_v"] * scale
    transformed_a = scale * a @ rotation.T + translation
    da, ad = _distances(transformed_a, reference, cancelled)
    db, bd = _distances(b, fitted, cancelled)
    covered = np.isfinite(db) & (db <= threshold)
    anchor_error = np.linalg.norm(scale * x @ rotation.T + translation - y, axis=1)
    record = {"kind": definition["kind"], "degrees_of_freedom": 7 if definition["kind"] == "similarity" else 6,
        "method": "proper_similarity_or_rigid_from_explicit_point_correspondences_no_ICP",
        "region": copy.deepcopy(definition["region"]), "candidate_indices": left, "reference_indices": right,
        "transform": {"s": scale, "R": rotation.tolist(), "t": translation.tolist(),
                      "input_frame": evaluation["id"], "output_frame": evaluation["id"], "applied_to_main_result": False},
        "main_result_unchanged": True, "fitted_scale_is_not_original_scale_accuracy": True,
        "anchor_fit": {"statistics": _statistics(anchor_error, quantiles), "not_model_accuracy": True},
        "sample_selection": "fixed_main_query_samples_ROI_selected_before_diagnostic_no_resampling",
        "after": {"candidate_to_reference": {"definition": ad, "statistics": _statistics(da, quantiles)},
            "reference_to_candidate": {"definition": bd, "statistics": _statistics(db, quantiles)},
            "completeness": {"threshold": threshold, "covered_count": int(covered.sum()),
                "missing_count": int((~covered).sum()), "reference_sample_count": len(b),
                "coverage_fraction": float(covered.mean()) if len(b) else None}}}
    return record, {"diagnostic_candidate_distances": da, "diagnostic_reference_distances": db,
                    "diagnostic_candidate_points": transformed_a, "diagnostic_reference_covered": covered}


def evaluate_geometry(request, output_directory, base_directory=None, cancelled=None, progress=None):
    """Publish a fresh independent measurement with full distance samples."""
    _poll(cancelled)
    if not isinstance(request, dict) or request.get("schema") != SCHEMA:
        _fail("geometry_invalid_evaluation_request")
    extra = set(request) - {"schema", "evaluation_frame", "candidate", "reference", "sampling",
                            "coverage_threshold", "quantiles", "dimensions", "roi", "metadata", "diagnostic_registration"}
    if extra:
        _fail("geometry_unsupported_evaluation_options", ",".join(sorted(extra)))
    evaluation = request.get("evaluation_frame", {})
    if (not isinstance(evaluation, dict) or set(evaluation) != {"id", "unit"} or
            not isinstance(evaluation.get("id"), str) or not evaluation["id"] or evaluation.get("unit") not in (*_UNITS, "unknown")):
        _fail("geometry_explicit_evaluation_frame_and_unit_required")
    reference_record = request.get("reference", {})
    if (not isinstance(reference_record, dict) or not isinstance(reference_record.get("provenance"), dict) or
            reference_record["provenance"].get("role") != "independent_reference" or
            not isinstance(reference_record["provenance"].get("source"), str) or not reference_record["provenance"]["source"]):
        _fail("geometry_independent_reference_declaration_required")
    sampling = request.get("sampling", {})
    if not isinstance(sampling, dict) or set(sampling) - {"count", "seed", "reference_grid_shape"}:
        _fail("geometry_unsupported_sampling_options")
    count, seed = sampling.get("count"), sampling.get("seed")
    if type(count) is not int or not 1 <= count <= 1_000_000 or type(seed) is not int or not 0 <= seed <= 2**32 - 1:
        _fail("geometry_explicit_sampling_required")
    threshold = _number(request.get("coverage_threshold"), "coverage threshold", positive=True)
    quantiles = [_number(q, "quantile") for q in request.get("quantiles", [0., .5, .9, .95, .99, 1.])]
    if not quantiles or any(not 0 <= q <= 1 for q in quantiles) or len(quantiles) != len(set(quantiles)):
        _fail("geometry_invalid_quantiles")
    base = Path(base_directory or os.getcwd()).resolve()
    candidate = _load(request.get("candidate", {}), evaluation, base, cancelled)
    reference = _load(reference_record, evaluation, base, cancelled)
    if (candidate["input"] and reference["input"] and
            os.path.samefile(candidate["input"][0], reference["input"][0])):
        _fail("geometry_reference_is_candidate", "same file or filesystem alias is not an independent reference")
    if not len(reference["points"]):
        _fail("geometry_empty_reference")
    if progress:
        progress({"stage": "independent_geometry_loaded", "progressPercent": 10})
    a = _sample(candidate, count, np.random.RandomState(seed))
    b = _sample(reference, count, np.random.RandomState(seed ^ 0x6A09E667), sampling.get("reference_grid_shape"))
    roi = _roi(request.get("roi"), evaluation)
    a_mask, b_mask = _in_roi(a, roi), _in_roi(b, roi)
    a_outside, b_outside = int((~a_mask).sum()), int((~b_mask).sum())
    a, b = a[a_mask], b[b_mask]
    ab, ab_definition = _distances(a, reference, cancelled)
    ba, ba_definition = _distances(b, candidate, cancelled)
    covered = np.isfinite(ba) & (ba <= threshold)
    status = ("no_reconstruction" if not len(candidate["points"]) else
              "no_reference_samples_in_roi" if not len(b) else
              "no_reconstruction_samples_in_roi" if not len(a) else
              "measured_unknown_units" if evaluation["unit"] == "unknown" else "measured")
    report = {"schema": "gsw.geometry_evaluation/1", "status": status,
        "evaluation_frame": copy.deepcopy(evaluation), "request": copy.deepcopy(request),
        "candidate": candidate["source"], "reference": reference["source"],
        "sampling": {"requested_count": count, "seed": seed, "generator": "numpy.RandomState.MT19937",
            "numpy_version": np.__version__, "mesh_sampling": "triangle_area_weighted_uniform_barycentric",
            "reference_grid_shape": copy.deepcopy(sampling.get("reference_grid_shape")),
            "reference_grid_definition": "analytic_patch_cell_centres" if sampling.get("reference_grid_shape") else None,
            "candidate_outside_roi": a_outside, "reference_outside_roi": b_outside,
            "point_sampling": "all_points_or_seeded_without_replacement", "filters": "none"},
        "roi": {"bounds": copy.deepcopy(request.get("roi")),
            "definition": "query_samples_in_closed_aabb_targets_not_clipped",
            "count_semantics": "requested_count_before_ROI_finite_samples_after_ROI"},
        "alignment": {"main": "explicit_recorded_known_transforms", "automatic_registration": False},
        "reference_independence": {"basis": "user_declared_provenance", "same_file_or_alias_refused": True,
            "external_provenance_independently_audited": False},
        "dimensions": _dimensions(candidate, reference, request.get("dimensions", []), evaluation["unit"]),
        "candidate_to_reference": {"definition": ab_definition, "statistics": _statistics(ab, quantiles)},
        "reference_to_candidate": {"definition": ba_definition, "statistics": _statistics(ba, quantiles)},
        "completeness": {"threshold": threshold, "unit": evaluation["unit"], "reference_sample_count": len(b),
            "covered_count": int(covered.sum()), "missing_count": int((~covered).sum()),
            "coverage_fraction": float(covered.mean()) if len(b) else None,
            "point_cloud_limitation": "nearest_point_coverage_is_sampling_density_dependent_not_continuous_surface_certification"},
        "physical_model_accuracy_certified": False, "reference_feedback_to_training": False,
        "material_boundary_verified": False}
    diagnostic_arrays = {}
    if request.get("diagnostic_registration") is not None:
        diagnostic, diagnostic_arrays = _registration_diagnostic(request["diagnostic_registration"], candidate,
            reference, a, b, evaluation, threshold, quantiles, cancelled)
        diagnostic["before"] = {key: copy.deepcopy(report[key]) for key in
                                  ("candidate_to_reference", "reference_to_candidate", "completeness", "dimensions")}
        report["diagnostic_registration"] = diagnostic
    destination = Path(output_directory).absolute()
    if destination.exists() or destination.is_symlink() or not destination.parent.is_dir():
        _fail("geometry_fresh_output_directory_required", destination)
    with atomic_sibling_directory(destination) as stage:
        arrays_path = stage / "distances.npz"
        with arrays_path.open("xb") as stream:
            np.savez(stream, candidate_points=a, reference_points=b, candidate_distances=ab,
                     reference_distances=ba, reference_covered=covered, **diagnostic_arrays)
        report["artifacts"] = {"distances": {"path": "distances.npz", "sha256": _sha(arrays_path)}}
        for geometry in (candidate, reference):
            if geometry["input"] and _sha(geometry["input"][0]) != geometry["input"][1]:
                _fail("geometry_input_changed_during_evaluation")
            if geometry.get("manifest_input") and _sha(geometry["manifest_input"][0]) != geometry["manifest_input"][1]:
                _fail("geometry_manifest_changed_during_evaluation")
        _poll(cancelled)
        with (stage / "report.json").open("x", encoding="utf-8") as stream:
            json.dump(report, stream, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2)
        os.rename(str(stage), str(destination))
    return {"success": True, "reportPath": str(destination / "report.json"),
            "distancesPath": str(destination / "distances.npz"), "summary": report["status"]}
