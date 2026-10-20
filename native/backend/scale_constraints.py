"""Deterministic CPU scale/reference fitting, never model-accuracy estimation.

All geometry is read in original source coordinates. A scale-only result does
not estimate position or orientation; check rows do not participate in fitting.
No images, model files, SfM processes or training runtimes are opened here.
"""
import copy
import hashlib
import json
import math

import numpy as np

from native.backend import camera_dataset


REQUEST_SCHEMA = "gsw.scale_constraints/1"
SOLUTION_SCHEMA = "gsw.scale_solution/1"
SOLVER = "gsw.weighted_scale_constraints"
SOLVER_VERSION = 1
SCOPE = "scale_and_reference_constraint_agreement"
MAX_REQUEST_BYTES = 16 * 1024 * 1024
MAX_DERIVED_PAIRS = 100000
_REPLAY_POLICY = {"ulp_multiplier": 64, "relative_tolerance": 64. * float(np.finfo(np.float64).eps),
                  "residual_floor": "recomputed_numerical_allowance_m",
                  "input_request_roles_thresholds_weights_and_verdicts": "exact",
                  "stored_solution_digest": "retained_without_rewriting"}
_UNITS = {"m": 1., "cm": .01, "mm": .001}


class ScaleConstraintError(ValueError):
    """Machine-facing admission error with preserved numerical diagnostics."""
    def __init__(self, code, detail="", diagnostics=None):
        self.code, self.detail = code, str(detail)
        self.diagnostics = copy.deepcopy(diagnostics or {})
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _fail(code, detail="", diagnostics=None):
    raise ScaleConstraintError(code, detail, diagnostics)


def _json_bytes(value):
    try:
        raw = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False, allow_nan=False).encode("utf-8")
    except (TypeError, ValueError, UnicodeError) as error:
        _fail("scale_constraints_invalid_json", error)
    return raw


def _digest(value):
    return hashlib.sha256(_json_bytes(value)).hexdigest()


def request_sha256(request):
    """Digest every supplied request field, including provenance and checks."""
    return _digest(request)


def solution_sha256(solution):
    """Digest the complete solution except its own digest field."""
    value = copy.deepcopy(solution)
    value.pop("solution_sha256", None)
    return _digest(value)


def _canonical_request(request, cancelled=None):
    """Validate JSON request fields and retain original plus SI reference data."""
    if len(_json_bytes(request)) > MAX_REQUEST_BYTES:
        _fail("scale_constraints_resource_limit", "raw request exceeds 16 MiB", {"maximum_request_bytes": MAX_REQUEST_BYTES})
    if not isinstance(request, dict) or request.get("schema") != REQUEST_SCHEMA:
        _fail("scale_constraints_invalid_schema")
    result = copy.deepcopy(request)
    if result.get("input_frame") != "source" or result.get("mode") not in ("scale_only", "sim3"):
        _fail("scale_constraints_invalid_mode")
    _identifier(result.get("input_data_identity"), "input_data_identity")
    result["fit_tolerance_m"] = _number(result.get("fit_tolerance_m"), "fit_tolerance_m", minimum=0.)
    rows = result.get("constraints")
    if not isinstance(rows, list) or not rows or any(not isinstance(row, dict) for row in rows):
        _fail("scale_constraints_invalid_constraints")
    identifiers = set()
    for row in rows:
        _check_cancel(cancelled)
        identifier = _identifier(row.get("id"), "id")
        if identifier in identifiers:
            _fail("scale_constraints_duplicate_id", identifier)
        identifiers.add(identifier)
        _identifier(row.get("reference_id"), "reference_id")
        if row.get("kind") not in ("distance", "baseline", "control") or row.get("role") not in ("fit", "check"):
            _fail("scale_constraints_invalid_constraint", identifier)
        unit = row.get("reference_unit")
        if not isinstance(unit, str) or unit not in _UNITS:
            _fail("scale_constraints_invalid_unit", identifier)
        weight = _number(row.get("weight", 1.), "weight", positive=True)
        row["weight"] = weight
        sigma = row.get("reference_sigma")
        if "reference_sigma" in row and sigma is None:
            _fail("scale_constraints_invalid_number", "reference_sigma: omit unknown uncertainty instead of declaring null")
        sigma_m = None if sigma is None else _number(sigma, "reference_sigma", positive=True) * _UNITS[unit]
        if sigma_m is not None and (not math.isfinite(sigma_m) or sigma_m <= 0):
            _fail("scale_constraints_numeric_range", "reference uncertainty conversion")
        effective = weight if sigma_m is None else weight / sigma_m / sigma_m
        if not math.isfinite(effective) or effective <= 0:
            _fail("scale_constraints_numeric_range", "effective weight")
        row["reference_sigma_m"], row["effective_weight"] = sigma_m, effective
        provenance = row.get("provenance", {})
        if not isinstance(provenance, dict):
            _fail("scale_constraints_invalid_provenance", identifier)
        row["provenance"] = provenance
        if "tolerance_m" in row:
            row["tolerance_m"] = _number(row["tolerance_m"], "tolerance_m", minimum=0.)
        if "independent_of_fit" in row and type(row["independent_of_fit"]) is not bool:
            _fail("scale_constraints_invalid_provenance", identifier)
        if row["kind"] in ("distance", "baseline"):
            if row["kind"] == "distance":
                row["source_a"] = _vector(row.get("source_a"), "source_a")
                row["source_b"] = _vector(row.get("source_b"), "source_b")
            else:
                _identifier(row.get("view_a"), "view_a")
                _identifier(row.get("view_b"), "view_b")
                for name in ("source_a", "source_b"):
                    if name in row:
                        row[name] = _vector(row[name], name)
            row["reference_distance_m"] = _number(row.get("reference_distance"), "reference_distance", positive=True) * _UNITS[unit]
        else:
            row["source_xyz"] = _vector(row.get("source_xyz"), "source_xyz")
            row["reference_xyz"] = _vector(row.get("reference_xyz"), "reference_xyz")
            row["reference_xyz_m"] = [number * _UNITS[unit] for number in row["reference_xyz"]]
    return result


def canonical_request(request, cancelled=None):
    """Validate and normalize a request, raising only typed input errors."""
    try:
        return _canonical_request(request, cancelled)
    except ScaleConstraintError:
        raise
    except (OverflowError, ZeroDivisionError, FloatingPointError) as error:
        _fail("scale_constraints_numeric_range", error)
    except (TypeError, KeyError, IndexError, ValueError) as error:
        _fail("scale_constraints_invalid_constraints", error)


def _number(value, name, positive=False, minimum=None):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        _fail("scale_constraints_invalid_number", name)
    value = float(value)
    if (positive and value <= 0) or (minimum is not None and value < minimum):
        _fail("scale_constraints_invalid_number", name)
    return value


def _identifier(value, name):
    if not isinstance(value, str) or not value.strip() or len(value) > 1024 or any(ord(char) < 32 for char in value):
        _fail("scale_constraints_invalid_identifier", name)
    return value


def _vector(value, name):
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        _fail("scale_constraints_invalid_point", name)
    return [_number(number, name) for number in value]


def _check_cancel(cancelled):
    if cancelled and cancelled():
        _fail("scale_constraints_cancelled")


def _resolve_baselines(dataset, rows, cancelled=None):
    views = {view["view_id"]: view for view in dataset.get("views", [])}
    result = copy.deepcopy(rows)
    for row in result:
        _check_cancel(cancelled)
        if row["kind"] != "baseline":
            continue
        for suffix in ("a", "b"):
            view_id = row["view_" + suffix]
            if view_id not in views:
                _fail("scale_constraints_unknown_view", view_id)
            pose = views[view_id].get("source_c2w_opencv")
            if not isinstance(pose, (list, tuple)) or len(pose) != 4 or any(len(part) != 4 for part in pose):
                _fail("scale_constraints_missing_source_pose", view_id)
            center = _vector([pose[index][3] for index in range(3)], view_id)
            key = "source_" + suffix
            if key in row and any(a != b for a, b in zip(center, row[key])):
                _fail("scale_constraints_baseline_endpoint_conflict", row["id"])
            row[key] = center
    return result


def _distances(rows, cancelled=None):
    output = []
    for row in rows:
        _check_cancel(cancelled)
        if row["kind"] == "control":
            continue
        delta = [b - a for a, b in zip(row["source_a"], row["source_b"])]
        length = math.hypot(*delta)
        if not math.isfinite(length) or length <= 0:
            _fail("scale_constraints_degenerate_distance", row["id"])
        output.append(dict(row, source_distance=length))
    return output


def _control_pairs(rows, cancelled=None):
    controls = [row for row in rows if row["kind"] == "control"]
    if controls and len(controls) < 2:
        _fail("scale_constraints_insufficient_controls", controls[0]["role"])
    pair_count = len(controls) * (len(controls) - 1) // 2
    if pair_count > MAX_DERIVED_PAIRS:
        _fail("scale_constraints_resource_limit", "scale-only correlated control-pair expansion; inputs are not truncated",
              {"requested_derived_pairs": pair_count, "maximum_derived_pairs": MAX_DERIVED_PAIRS,
               "scope": "scale_only_control_pair_report_memory_not_model_budget_or_coordinate_range"})
    pairs = []
    for index, first in enumerate(controls):
        for second in controls[index + 1:]:
            _check_cancel(cancelled)
            source_length = math.hypot(*(b - a for a, b in zip(first["source_xyz"], second["source_xyz"])))
            reference_length = math.hypot(*(b - a for a, b in zip(first["reference_xyz_m"], second["reference_xyz_m"])))
            if not math.isfinite(source_length) or not math.isfinite(reference_length) or source_length <= 0 or reference_length <= 0:
                _fail("scale_constraints_degenerate_controls", first["id"] + "/" + second["id"])
            a, b = first["effective_weight"], second["effective_weight"]
            # Stable harmonic mean; each pair remains correlated with the
            # original control rows. Renormalization below prevents O(n^2)
            # fabricated information from overwhelming measured distances.
            harmonic = 2. * min(a, b) / (1. + min(a, b) / max(a, b))
            pair = {"id": "pair:" + first["id"] + ":" + second["id"], "kind": "control_pair",
                    "role": first["role"], "control_ids": [first["id"], second["id"]],
                    "source_a": first["source_xyz"], "source_b": second["source_xyz"],
                    "source_distance": source_length, "reference_distance_m": reference_length,
                    "effective_weight": harmonic}
            pairs.append(pair)
    if pairs:
        maximum = max(row["effective_weight"] for row in controls)
        target = math.fsum(row["effective_weight"] / maximum for row in controls)
        actual = math.fsum(pair["effective_weight"] / maximum for pair in pairs)
        for pair in pairs:
            pair["effective_weight"] *= target / actual
    return pairs


def _check_disjoint(rows):
    seen = {}
    for row in rows:
        keys = [("reference_id", row["reference_id"])]
        if row["kind"] in ("distance", "baseline"):
            keys.append(("distance_geometry", tuple(sorted((tuple(row["source_a"]), tuple(row["source_b"]))))))
        if row["kind"] == "baseline":
            keys.append(("camera_pair", tuple(sorted((row["view_a"], row["view_b"])))))
        if row["kind"] == "control":
            keys.append(("control_geometry", tuple(row["source_xyz"])))
        for name in ("observation_id", "source_point_id"):
            if row.get(name):
                keys.append((name, _identifier(row[name], name)))
        provenance = row["provenance"]
        if provenance.get("measurement_id"):
            keys.append(("measurement_id", _identifier(provenance["measurement_id"], "measurement_id")))
        for key in keys:
            if key in seen and seen[key] != row["role"]:
                _fail("scale_constraints_fit_check_overlap", row["id"])
            seen[key] = row["role"]
    # Control rows imply their pair geometry even when represented as point
    # observations. Re-labelling a fitted baseline/distance as two check
    # controls (or the reverse) does not create held-out information.
    control_points = {role: {tuple(row["source_xyz"]) for row in rows
                             if row["kind"] == "control" and row["role"] == role} for role in ("fit", "check")}
    for row in rows:
        if row["kind"] in ("distance", "baseline"):
            opposite = control_points["check" if row["role"] == "fit" else "fit"]
            if tuple(row["source_a"]) in opposite and tuple(row["source_b"]) in opposite:
                _fail("scale_constraints_fit_check_overlap", row["id"])


def _allowance(row, scale):
    magnitude = max(abs(row["reference_distance_m"]), abs(scale * row["source_distance"]),
                    *(abs(scale * number) for key in ("source_a", "source_b") for number in row[key]))
    return 64. * float(np.finfo(np.float64).eps) * magnitude


def _fit_report(rows, scale, tolerance, original_rows=None):
    residuals = []
    for row in rows:
        signed = scale * row["source_distance"] - row["reference_distance_m"]
        allowance = _allowance(row, scale)
        residuals.append({"id": row["id"], "kind": row["kind"], "role": row["role"],
                          "residual_m": abs(signed), "signed_residual_m": signed,
                          "predicted_distance_m": scale * row["source_distance"],
                          "reference_distance_m": row["reference_distance_m"],
                          "tolerance_m": tolerance, "numerical_allowance_m": allowance,
                          "accepted": abs(signed) <= tolerance + allowance,
                          "effective_weight": row["effective_weight"],
                          **({"control_ids": row["control_ids"]} if "control_ids" in row else {})})
    maximum_weight = max(row["effective_weight"] for row in rows)
    total_weight = math.fsum(row["effective_weight"] / maximum_weight for row in rows)
    source_rows = original_rows if original_rows is not None else rows
    pairs = [item for item in residuals if item["kind"] == "control_pair"]
    per_constraint = []
    for row in source_rows:
        if row["kind"] != "control":
            per_constraint.append(next(item for item in residuals if item["id"] == row["id"]))
            continue
        related = [item for item in pairs if row["id"] in item["control_ids"]]
        per_constraint.append({"id": row["id"], "kind": "control", "role": row["role"],
                               "residual_m": max(item["residual_m"] for item in related),
                               "accepted": all(item["accepted"] for item in related),
                               "tolerance_m": tolerance,
                               "numerical_allowance_m": max(item["numerical_allowance_m"] for item in related),
                               "residual_semantics": "maximum_incident_correlated_pair_distance_residual"})
    report = {"constraint_ids": [row["id"] for row in source_rows], "source_constraint_count": len(source_rows),
              "scalar_observation_count": len(rows) - len(pairs), "derived_pair_count": len(pairs),
              "derived_pairs": pairs, "derived_pairs_correlated": bool(pairs), "independent_observation_count": None,
              "weighted_rmse_m": math.sqrt(math.fsum(item["effective_weight"] / maximum_weight / total_weight * item["residual_m"] ** 2
                                                     for item in residuals)),
              "max_residual_m": max(item["residual_m"] for item in residuals),
              "per_constraint": per_constraint, "fit_tolerance_m": tolerance,
              "numerical_allowance_m": max(item["numerical_allowance_m"] for item in residuals),
              "accepted": all(item["accepted"] for item in residuals),
              "residual_semantics": "scalar_reference_distance_agreement_not_model_accuracy"}
    return report


def _checks_report(rows, scale, mode, rotation, geometry, cancelled=None):
    residuals, warnings = [], []
    distances = _distances(rows, cancelled)
    controls = [row for row in rows if row["kind"] == "control"]
    pairs = _control_pairs(rows, cancelled) if mode == "scale_only" and len(controls) >= 2 else []
    pair_results = (_fit_report(pairs, scale, 0.)["per_constraint"] if pairs else [])
    for row in rows:
        _check_cancel(cancelled)
        provenance = row["provenance"]
        declared = (row.get("independent_of_fit") is True
                    and isinstance(provenance.get("source"), str) and bool(provenance["source"].strip())
                    and isinstance(provenance.get("measurement_id"), str) and bool(provenance["measurement_id"].strip()))
        judged = declared and "tolerance_m" in row
        tolerance = row.get("tolerance_m", 0.)
        if row["kind"] != "control":
            observation = next(item for item in distances if item["id"] == row["id"])
            item = _fit_report([observation], scale, tolerance)["per_constraint"][0]
        elif mode == "sim3":
            vector, allowance = _control_residual(row, scale, rotation, geometry)
            norm = math.hypot(*vector.tolist())
            item = {"id": row["id"], "kind": "control", "role": "check", "residual_m": norm,
                    "vector_residual_m": vector.tolist(), "numerical_allowance_m": allowance,
                    "accepted": norm <= tolerance + allowance}
        elif pairs:
            related = [item for item in pair_results if row["id"] in item["control_ids"]]
            item = {"id": row["id"], "kind": "control", "role": "check",
                    "residual_m": max(item["residual_m"] for item in related),
                    "numerical_allowance_m": max(item["numerical_allowance_m"] for item in related),
                    "accepted": all(item["residual_m"] <= tolerance + item["numerical_allowance_m"] for item in related),
                    "residual_semantics": "maximum_incident_correlated_pair_distance_residual"}
            partners = [other for other in controls if other["id"] != row["id"]]
            judged = judged and all(other.get("independent_of_fit") is True
                                   and bool(other["provenance"].get("source"))
                                   and bool(other["provenance"].get("measurement_id"))
                                   and "tolerance_m" in other for other in partners)
        else:
            judged = False
            item = {"id": row["id"], "kind": "control", "role": "check", "residual_m": None,
                    "numerical_allowance_m": None, "accepted": None,
                    "residual_semantics": "scale_only_requires_two_heldout_control_points"}
        item.update(declared_independent=declared, provenance=copy.deepcopy(provenance),
                    tolerance_m=row.get("tolerance_m"),
                    accepted=item["accepted"] if judged else None,
                    status=("pass" if item["accepted"] else "fail") if judged else "unverified")
        if not judged:
            warnings.append({"code": "scale_constraints_check_unverified", "constraint_id": row["id"]})
        residuals.append(item)
    status = ("not_provided" if not rows else
              "fail" if any(item["status"] == "fail" for item in residuals) else
              "unverified" if any(item["status"] == "unverified" for item in residuals) else "pass")
    return {"status": status, "scope": SCOPE, "model_geometry_accuracy": "not_measured",
            "declared_independent": bool(rows) and all(item["declared_independent"] for item in residuals),
            "independence_basis": "user_declared_disjoint_reference_observations_not_external_authentication",
            "disjoint": True, "constraint_ids": [row["id"] for row in rows],
            "per_constraint": residuals, "warnings": warnings,
            "derived_pair_count": len(pairs), "derived_pairs_correlated": bool(pairs),
            "derived_pairs": pair_results, "independent_observation_count": None}


def _sim3_controls(rows, distances):
    controls = [row for row in rows if row["kind"] == "control"]
    if len(controls) < 3:
        _fail("scale_constraints_insufficient_controls", "Sim3 requires at least three non-collinear controls")
    source = np.asarray([row["source_xyz"] for row in controls], dtype=np.float64)
    reference = np.asarray([row["reference_xyz_m"] for row in controls], dtype=np.float64)
    maximum_weight = max(row["effective_weight"] for row in rows)
    weights = np.asarray([row["effective_weight"] / maximum_weight for row in controls])
    normalized_weights = weights / float(weights.sum())
    # Subtract an actual nearby point before weighted centering so million-unit
    # origins never dominate covariance or the scale objective.
    source_local, reference_local = source - source[0], reference - reference[0]
    source_mean_local = normalized_weights @ source_local
    reference_mean_local = normalized_weights @ reference_local
    xc, yc = source_local - source_mean_local, reference_local - reference_mean_local
    source_center, reference_center = source[0] + source_mean_local, reference[0] + reference_mean_local
    source_extent, reference_extent = float(np.max(np.abs(xc))), float(np.max(np.abs(yc)))
    if not all(math.isfinite(value) and value > 0 for value in (source_extent, reference_extent)):
        _fail("scale_constraints_degenerate_controls")
    xn, yn = xc / source_extent, yc / reference_extent
    try:
        source_singular = np.linalg.svd(xn * np.sqrt(normalized_weights[:, None]), compute_uv=False)
        reference_singular = np.linalg.svd(yn * np.sqrt(normalized_weights[:, None]), compute_uv=False)
        covariance = (yn * normalized_weights[:, None]).T @ xn
        u, singular, vt = np.linalg.svd(covariance)
    except np.linalg.LinAlgError as error:
        _fail("scale_constraints_degenerate_controls", error)
    source_rank = int(np.count_nonzero(source_singular > source_singular[0] * 1e-10))
    reference_rank = int(np.count_nonzero(reference_singular > reference_singular[0] * 1e-10))
    cross_rank = int(np.count_nonzero(singular > singular[0] * 1e-10)) if singular[0] > 0 else 0
    if min(source_rank, reference_rank, cross_rank) < 2:
        _fail("scale_constraints_degenerate_controls", "coincident, collinear or unstable correspondences")
    determinant = float(np.linalg.det(u @ vt))
    if source_rank == reference_rank == 3 and determinant < 0:
        _fail("scale_constraints_reflection_unsupported", "full-rank correspondences prefer a reflected frame")
    correction = np.eye(3)
    correction[2, 2] = 1. if determinant >= 0 else -1.
    rotation = u @ correction @ vt
    numerator = float(np.sum(weights * np.sum(yc * (xc @ rotation.T), axis=1)))
    denominator = float(np.sum(weights * np.sum(xc * xc, axis=1)))
    numerator += math.fsum(row["effective_weight"] / maximum_weight * row["source_distance"] * row["reference_distance_m"] for row in distances)
    denominator += math.fsum(row["effective_weight"] / maximum_weight * row["source_distance"] ** 2 for row in distances)
    if not math.isfinite(numerator) or not math.isfinite(denominator) or denominator <= 0:
        _fail("scale_constraints_numeric_range")
    scale = numerator / denominator
    if not math.isfinite(scale) or scale <= 0:
        _fail("scale_constraints_nonpositive_scale")
    translation = reference_center - scale * (rotation @ source_center)
    if not np.isfinite(translation).all():
        _fail("scale_constraints_numeric_range")
    diagnostics = {"source_rank": source_rank, "reference_rank": reference_rank,
                   "cross_rank": cross_rank, "rank_relative_threshold": 1e-10,
                   "source_singular_ratios": (source_singular / source_singular[0]).tolist(),
                   "reference_singular_ratios": (reference_singular / reference_singular[0]).tolist(),
                   "cross_singular_ratios": (singular / singular[0]).tolist(),
                   "rotation_determinant": float(np.linalg.det(rotation)),
                   "source_centroid": source_center.tolist(), "reference_centroid_m": reference_center.tolist(),
                   "planar_handedness_limitation": min(source_rank, reference_rank) == 2,
                   "warnings": (["scale_constraints_planar_offplane_check_required_for_handedness"]
                                if min(source_rank, reference_rank) == 2 else [])}
    return scale, rotation, translation, diagnostics


def _control_residual(row, scale, rotation, geometry):
    source = np.asarray(row["source_xyz"])
    reference = np.asarray(row["reference_xyz_m"])
    vector = scale * (rotation @ (source - np.asarray(geometry["source_centroid"]))) - (
        reference - np.asarray(geometry["reference_centroid_m"]))
    magnitude = max(*(abs(number) for number in reference),
                    *(abs(scale * number) for number in source),
                    *(abs(number) for number in geometry["reference_centroid_m"]))
    allowance = 64. * float(np.finfo(np.float64).eps) * magnitude
    return vector, float(allowance)


def _sim3_report(rows, distances, scale, rotation, geometry, tolerance):
    residuals = (_fit_report(distances, scale, tolerance)["per_constraint"] if distances else [])
    for row in rows:
        if row["kind"] != "control":
            continue
        vector, allowance = _control_residual(row, scale, rotation, geometry)
        norm = math.hypot(*vector.tolist())
        residuals.append({"id": row["id"], "kind": "control", "role": row["role"],
                          "residual_m": norm, "vector_residual_m": vector.tolist(),
                          "tolerance_m": tolerance, "numerical_allowance_m": allowance,
                          "accepted": norm <= tolerance + allowance,
                          "effective_weight": row["effective_weight"]})
    maximum = max(item["effective_weight"] for item in residuals)
    total = math.fsum(item["effective_weight"] / maximum for item in residuals)
    return {"constraint_ids": [row["id"] for row in rows], "source_constraint_count": len(rows),
            "scalar_observation_count": len(distances), "vector_observation_count": len(rows) - len(distances),
            "derived_pair_count": 0, "derived_pairs": [], "derived_pairs_correlated": False,
            "independent_observation_count": None,
            "weighted_rmse_m": math.sqrt(math.fsum(item["effective_weight"] / maximum / total * item["residual_m"] ** 2 for item in residuals)),
            "max_residual_m": max(item["residual_m"] for item in residuals),
            "per_constraint": residuals, "fit_tolerance_m": tolerance,
            "numerical_allowance_m": max(item["numerical_allowance_m"] for item in residuals),
            "accepted": all(item["accepted"] for item in residuals),
            "residual_semantics": "control_vector_and_reference_distance_agreement_not_model_accuracy"}


def _solve_scale_constraints(dataset, request, cancelled=None):
    """Solve a source-frame constraint request without mutating its inputs."""
    _check_cancel(cancelled)
    canonical = canonical_request(request, cancelled)
    if not isinstance(dataset, dict) or dataset.get("data_identity") != canonical["input_data_identity"]:
        _fail("scale_constraints_dataset_identity_mismatch")
    if dataset.get("schema") not in (None, "gsw.scale_input/1", camera_dataset.SCHEMA):
        _fail("scale_constraints_invalid_input_schema")
    identity = dataset["data_identity"]
    if len(identity) != 64 or any(char not in "0123456789abcdef" for char in identity):
        _fail("scale_constraints_dataset_identity_mismatch")
    # A model input envelope is already bound to model bytes by the caller.
    # Camera manifests instead carry a reproducible full semantic digest.
    if dataset.get("schema") == camera_dataset.SCHEMA and camera_dataset.refresh_dataset_identity(dataset)["data_identity"] != identity:
        _fail("scale_constraints_dataset_identity_mismatch")
    resolved = _resolve_baselines(dataset, canonical["constraints"], cancelled)
    _check_disjoint(resolved)
    fit_source_rows = [row for row in resolved if row["role"] == "fit"]
    distances = _distances(fit_source_rows, cancelled)
    if not fit_source_rows:
        _fail("scale_constraints_no_fit_constraints")
    rotation, translation, geometry = np.eye(3), np.zeros(3), {}
    if canonical["mode"] == "sim3":
        _check_cancel(cancelled)
        scale, rotation, translation, geometry = _sim3_controls(fit_source_rows, distances)
        _check_cancel(cancelled)
        rows = fit_source_rows
        fit = _sim3_report(fit_source_rows, distances, scale, rotation, geometry, canonical["fit_tolerance_m"])
    else:
        rows = distances + _control_pairs(fit_source_rows, cancelled)
        maximum_weight = max(row["effective_weight"] for row in rows)
        numerator = math.fsum(row["effective_weight"] / maximum_weight * row["source_distance"] * row["reference_distance_m"] for row in rows)
        denominator = math.fsum(row["effective_weight"] / maximum_weight * row["source_distance"] ** 2 for row in rows)
        if not math.isfinite(numerator) or not math.isfinite(denominator) or denominator <= 0:
            _fail("scale_constraints_numeric_range")
        scale = numerator / denominator
        if not math.isfinite(scale) or scale <= 0:
            _fail("scale_constraints_nonpositive_scale")
        fit = _fit_report(rows, scale, canonical["fit_tolerance_m"], fit_source_rows)
    if not fit["accepted"]:
        _fail("scale_constraints_fit_conflict", "fit residual exceeds declared limit", fit)
    checks = _checks_report([row for row in resolved if row["role"] == "check"], scale,
                            canonical["mode"], rotation, geometry, cancelled)
    transform = camera_dataset.invert_sim3(camera_dataset.invert_sim3({
        "s": scale, "R": rotation.tolist(), "t": translation.tolist(),
        "input_frame": "source", "output_frame": "metric"}))
    result = {"schema": SOLUTION_SCHEMA, "solver": SOLVER, "solver_version": SOLVER_VERSION,
              "input_data_identity": dataset["data_identity"], "request": copy.deepcopy(request),
              "request_sha256": request_sha256(request), "canonical_request": canonical,
              "canonical_request_sha256": _digest(canonical), "source_to_metric": transform,
              "fit": fit,
              "independent_checks": checks,
              "geometry_diagnostics": dict(geometry, **{"fit_nontraining_camera_ids": sorted({row["view_" + suffix]
                  for row in rows if row["kind"] == "baseline" for suffix in ("a", "b")
                  if next(view for view in dataset["views"] if view["view_id"] == row["view_" + suffix]).get("split") != "train"}),
                  "images_added_to_training": False}),
              "reference_uncertainty": [{"id": row["id"], "reference_unit": row["reference_unit"],
                                         "reference_sigma": row.get("reference_sigma"), "sigma_m": row["reference_sigma_m"],
                                         "requested_weight": row["weight"], "effective_weight": row["effective_weight"],
                                         "status": "specified" if row["reference_sigma_m"] is not None else "unknown"}
                                        for row in canonical["constraints"]],
              "scope": SCOPE, "model_geometry_accuracy": "not_measured",
              "numeric_replay_policy": copy.deepcopy(_REPLAY_POLICY)}
    _check_cancel(cancelled)
    result["solution_sha256"] = solution_sha256(result)
    return result


def solve_scale_constraints(dataset, request, cancelled=None):
    """Solve without side effects; reject malformed or unrepresentable input."""
    try:
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            return _solve_scale_constraints(dataset, request, cancelled)
    except ScaleConstraintError:
        raise
    except camera_dataset.CameraDatasetError as error:
        _fail("scale_constraints_invalid_similarity", error)
    except (OverflowError, ZeroDivisionError, FloatingPointError, np.linalg.LinAlgError) as error:
        _fail("scale_constraints_numeric_range", error)
    except (TypeError, KeyError, IndexError, ValueError) as error:
        _fail("scale_constraints_invalid_constraints", error)


def _ulp(value):
    # math.ulp is unavailable in the configured Python 3.8 environments.
    return math.ldexp(1., max(-1074, math.frexp(abs(value))[1] - 53)) if value else math.ldexp(1., -1074)


def _numeric_replay_equal(provided, expected, replay, path=()):
    """Permit only derived floating-point arithmetic drift, never policy drift."""
    if isinstance(expected, dict):
        if not isinstance(provided, dict) or set(provided) != set(expected):
            return False
        return all(_numeric_replay_equal(provided[key], value, replay, path + (key,))
                   for key, value in expected.items() if path or key != "solution_sha256")
    if isinstance(expected, list):
        return (isinstance(provided, list) and len(provided) == len(expected)
                and all(_numeric_replay_equal(a, b, replay, path + (index,))
                        for index, (a, b) in enumerate(zip(provided, expected))))
    if isinstance(expected, bool) or expected is None or isinstance(expected, (str, int)):
        return type(provided) is type(expected) and provided == expected
    if not isinstance(expected, float) or isinstance(provided, bool) or not isinstance(provided, (float, int)):
        return provided == expected
    if not math.isfinite(provided) or not math.isfinite(expected):
        return False
    if provided == expected:
        return True
    root = path[0] if path else ""
    field = next((part for part in reversed(path) if isinstance(part, str)), "")
    magnitude, floor = abs(expected), 0.
    if root == "source_to_metric" and field in ("s", "R", "t"):
        if field == "R":
            magnitude = 1.
        elif field == "t":
            # Recomputed source and reference origins bound cancellation in
            # t=ybar-sR*xbar; no supplied allowance can enlarge this floor.
            geometry = replay["geometry_diagnostics"]
            magnitude = max(magnitude, *(abs(number) for name in ("source_centroid", "reference_centroid_m")
                                         for number in geometry.get(name, [])))
    elif root in ("fit", "independent_checks") and field in (
            "residual_m", "signed_residual_m", "vector_residual_m", "weighted_rmse_m",
            "max_residual_m", "numerical_allowance_m", "predicted_distance_m"):
        floor = replay["fit"]["numerical_allowance_m"]
        if root == "independent_checks":
            floor = max([floor] + [item.get("numerical_allowance_m") or 0.
                                  for item in replay["independent_checks"]["per_constraint"]])
    elif root == "geometry_diagnostics" and field in (
            "source_singular_ratios", "reference_singular_ratios", "cross_singular_ratios",
            "rotation_determinant", "source_centroid", "reference_centroid_m"):
        if field.endswith("ratios") or field == "rotation_determinant":
            magnitude = 1.
    else:
        return False
    allowance = floor + 64. * _ulp(magnitude) + _REPLAY_POLICY["relative_tolerance"] * abs(expected)
    return abs(provided - expected) <= allowance


def validate_scale_solution(dataset, request, solution, cancelled=None):
    """Replay exact inputs plus bounded numeric drift; retain the stored digest.

    Different supported NumPy/LAPACK versions can differ by a few ULPs in SVD.
    Policy, input, threshold, uncertainty and pass/fail fields remain exact;
    replay never converts an originally failed check into a passing one.
    """
    replay = solve_scale_constraints(dataset, request, cancelled)
    if not isinstance(solution, dict) or solution.get("solution_sha256") != solution_sha256(solution):
        _fail("scale_constraints_solution_mismatch")
    if solution["solution_sha256"] != replay["solution_sha256"] and not _numeric_replay_equal(solution, replay, replay):
        _fail("scale_constraints_solution_mismatch")
    return copy.deepcopy(solution)
