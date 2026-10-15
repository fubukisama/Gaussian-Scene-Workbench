# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Compare supplied measurements against an explicit user quality contract.

This module does not measure or invent quality, and a passing contract is not a
guarantee about unmeasured views, final convergence, or every aspect of a model.
"""
from copy import deepcopy
from decimal import Decimal, localcontext
import math

_METRICS = {"psnr": ("image", "higher"), "ssim": ("image", "higher"),
            "lpips": ("image", "lower"), "depth_rmse": ("geometry", "lower"),
            "normal_mae": ("geometry", "lower"), "mesh_distance": ("geometry", "lower")}
_IDENTITY = ("dataset_id", "viewset_id", "resolution_id", "mask_id", "metric_protocol_id")


def _finite(value, location):
    try:
        valid = type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError(location + " requires a finite number, not a boolean")


def _object(value, location):
    if type(value) is not dict or any(type(key) is not str for key in value):
        raise ValueError(location + " requires an object with string keys")


def _version(value, location):
    if type(value) is not int or value != 1:
        raise ValueError(location + " requires integer version 1")


def _metric_range(name, value, location):
    if name == "ssim" and not -1 <= value <= 1:
        raise ValueError(location + " requires SSIM in [-1, 1]")
    if name in ("lpips", "depth_rmse", "normal_mae", "mesh_distance") and value < 0:
        raise ValueError(location + " requires a nonnegative metric value")


def _validate_settings(value, location, depth=0):
    if depth > 16:
        raise ValueError(location + " exceeds the settings nesting limit")
    if type(value) is dict:
        _object(value, location)
        for key, child in value.items():
            _validate_settings(child, location + "." + key, depth + 1)
    elif type(value) is list:
        for index, child in enumerate(value):
            _validate_settings(child, location + "." + str(index), depth + 1)
    elif type(value) in (int, float):
        _finite(value, location)
    elif value is not None and type(value) not in (str, bool):
        raise ValueError(location + " requires finite JSON settings")


def _same_json_value(before, after):
    if type(before) is not type(after):
        return False
    if type(before) is list:
        return len(before) == len(after) and all(_same_json_value(a, b) for a, b in zip(before, after))
    if type(before) is dict:
        return set(before) == set(after) and all(_same_json_value(before[key], after[key]) for key in before)
    return before == after


def _settings_changes(before, after, path=()):
    changes = []
    for key in sorted(set(before) | set(after)):
        old, new = before.get(key), after.get(key)
        old_present, new_present = key in before, key in after
        if old_present and new_present and type(old) is dict and type(new) is dict:
            changes.extend(_settings_changes(old, new, path + (key,)))
        elif old_present != new_present or not _same_json_value(old, new):
            changes.append({"path": list(path + (key,)), "before_present": old_present,
                            "after_present": new_present, "before": deepcopy(old), "after": deepcopy(new)})
    return changes


def _validate_report(report, location):
    _object(report, location)
    if (not {"version", "identity", "split", "metrics"}.issubset(report)
            or set(report) - {"version", "identity", "split", "metrics", "settings", "evidence"}):
        raise ValueError(location + " has missing or unknown fields")
    _version(report["version"], location)
    _object(report["identity"], location + ".identity")
    if set(report["identity"]) != set(_IDENTITY) or any(
            type(report["identity"][key]) is not str or not report["identity"][key].strip()
            for key in _IDENTITY):
        raise ValueError(location + " requires explicit dataset, views, resolution, mask and metric protocol identities")
    if report["split"] not in ("held_out", "training_fit", "unverified"):
        raise ValueError(location + ".split requires held_out, training_fit or unverified")
    if "settings" in report:
        _object(report["settings"], location + ".settings")
        _validate_settings(report["settings"], location + ".settings")
    if "evidence" in report:
        _object(report["evidence"], location + ".evidence")
        _validate_settings(report["evidence"], location + ".evidence")
        if "held_out_verified" in report["evidence"] and type(report["evidence"]["held_out_verified"]) is not bool:
            raise ValueError(location + ".evidence.held_out_verified requires a boolean")
    _object(report["metrics"], location + ".metrics")
    for name, value in report["metrics"].items():
        if name not in _METRICS:
            raise ValueError(location + ".metrics has an unsupported metric: " + name)
        if value is not None:
            if type(value) is dict:
                if name != "psnr" or value != {"value": None, "special": "positive_infinity", "reason": "zero_mse"}:
                    raise ValueError(location + ".metrics only permits explicit zero-MSE positive-infinite PSNR")
            else:
                _finite(value, location + ".metrics." + name)
                _metric_range(name, value, location + ".metrics." + name)


def _validate_contract(contract):
    _object(contract, "contract")
    if set(contract) != {"version", "required_domains", "metrics"}:
        raise ValueError("contract has missing or unknown fields")
    _version(contract["version"], "contract")
    domains = contract["required_domains"]
    if (type(domains) is not list or any(type(domain) is not str or domain not in ("image", "geometry")
                                        for domain in domains) or len(set(domains)) != len(domains)):
        raise ValueError("contract.required_domains requires unique image/geometry names")
    _object(contract["metrics"], "contract.metrics")
    for name, limits in contract["metrics"].items():
        if name not in _METRICS:
            raise ValueError("contract.metrics has an unsupported metric: " + name)
        _object(limits, "contract.metrics." + name)
        if set(limits) - {"absolute_min", "absolute_max", "max_regression"}:
            raise ValueError("contract metric has an unsupported threshold")
        for key, value in limits.items():
            _finite(value, "contract.metrics." + name + "." + key)
            if key != "max_regression":
                _metric_range(name, value, "contract.metrics." + name + "." + key)
        if limits.get("max_regression", 0) < 0:
            raise ValueError("max_regression cannot be negative")
        if ("absolute_min" in limits and "absolute_max" in limits
                and limits["absolute_min"] > limits["absolute_max"]):
            raise ValueError("absolute_min cannot exceed absolute_max")


def _status(values):
    if "regressed" in values:
        return "regressed"
    return "pass" if values and all(value == "pass" for value in values) else "unverified"


def compare_quality(baseline, candidate, contract):
    """Compare an explicit contract, never infer a universal quality guarantee.

    Every contract metric is required. ``required_domains`` additionally guards
    coverage: image measurements alone cannot establish geometry quality. Extra
    measurements still receive visible deltas, but no invented tolerance.

    Exact zero-MSE PSNR is explicitly represented by ``{value: null,
    special: 'positive_infinity', reason: 'zero_mse'}``. Two such results are
    equivalent (zero regression), not an undefined Infinity-minus-Infinity.
    Infinite deltas/regressions use a null value plus an explicit ``*_kind``;
    output never relies on nonstandard JSON NaN or Infinity tokens.
    """
    _validate_report(baseline, "baseline")
    _validate_report(candidate, "candidate")
    _validate_contract(contract)
    trace = {'baseline_identity': deepcopy(baseline['identity']),
             'candidate_identity': deepcopy(candidate['identity']),
             'baseline_split': baseline['split'], 'candidate_split': candidate['split'],
             'contract': deepcopy(contract)}
    settings_changes = _settings_changes(baseline.get("settings", {}), candidate.get("settings", {}))
    names = sorted(set(baseline["metrics"]) | set(candidate["metrics"]) | set(contract["metrics"]))
    required = sorted(set(contract["required_domains"]) | {_METRICS[name][0] for name in contract["metrics"]})
    mismatches = [key for key in _IDENTITY if baseline["identity"][key] != candidate["identity"][key]]
    if baseline["split"] != candidate["split"]:
        mismatches.append("split")
    if mismatches:
        results = {name: {"domain": _METRICS[name][0], "direction": _METRICS[name][1],
                   "baseline": deepcopy(baseline["metrics"].get(name)),
                   "candidate": deepcopy(candidate["metrics"].get(name)),
                   "delta": None, "regression": None, "status": "not_comparable",
                   "delta_kind": "unavailable", "regression_kind": "unavailable",
                   "threshold_status": "not_comparable", "thresholds": dict(contract["metrics"].get(name, {})),
                   "reasons": ["measurement_identity_mismatch"]} for name in names}
        return {**trace, "version": 1, "status": "not_comparable", "identity_mismatches": mismatches,
                "independent_validation": False, "metrics": results, "settings_changes": settings_changes,
                "required_domains": required,
                "domains": {domain: {"status": "not_comparable", "metrics": [name for name in results
                            if _METRICS[name][0] == domain], "required_metrics": [name for name in contract["metrics"]
                            if _METRICS[name][0] == domain]} for domain in ("image", "geometry")}}
    results = {}
    independent = baseline["split"] == "held_out" and all(
        "evidence" not in report or report["evidence"].get("held_out_verified") is True
        for report in (baseline, candidate)) and any(
        baseline["metrics"].get(name) is not None and candidate["metrics"].get(name) is not None for name in names)
    for name in names:
        limits = contract["metrics"].get(name, {})
        domain, direction = _METRICS[name]
        old, new = baseline["metrics"].get(name), candidate["metrics"].get(name)
        old_value = math.inf if type(old) is dict else old
        new_value = math.inf if type(new) is dict else new
        failures = []
        missing = old is None or new is None
        both_perfect = type(old) is dict and type(new) is dict
        delta = None if missing else 0.0 if both_perfect else new_value - old_value
        if not missing and type(old) is not dict and type(new) is not dict:
            # Compare the decimal values users actually supplied, without an
            # implicit epsilon or a binary-subtraction penalty at the boundary
            # (30.0 -> 29.9 is a 0.1 regression, not 0.10000000000000142).
            with localcontext() as context:
                context.prec = 768  # Enough for exact subtraction of finite float64 decimal forms.
                delta = float(Decimal(str(new)) - Decimal(str(old)))
            _finite(delta, name + " delta")
        regression = None if missing else max(0.0, -delta if direction == "higher" else delta)
        delta_kind = ("unavailable" if missing else "both_zero_mse" if both_perfect else
                      "finite" if math.isfinite(delta) else "positive_infinity" if delta > 0 else "negative_infinity")
        regression_kind = "unavailable" if missing else "finite" if math.isfinite(regression) else "positive_infinity"
        if not missing and "absolute_min" in limits and new_value < limits["absolute_min"]:
            failures.append("below_absolute_min")
        if not missing and "absolute_max" in limits and new_value > limits["absolute_max"]:
            failures.append("above_absolute_max")
        if not missing and "max_regression" in limits and regression > limits["max_regression"]:
            failures.append("exceeds_max_regression")
        reasons = list(failures)
        if missing:
            reasons.append("missing_measurement")
        if not limits:
            reasons.append("user_threshold_missing")
        if not independent:
            reasons.append("independent_validation_missing")
        threshold_status = "unverified" if missing or not limits else "regressed" if failures else "pass"
        results[name] = {"domain": domain, "direction": direction, "baseline": deepcopy(old),
                         "candidate": deepcopy(new), "delta": delta if missing or math.isfinite(delta) else None,
                         "delta_kind": delta_kind,
                         "regression": regression if missing or math.isfinite(regression) else None,
                         "regression_kind": regression_kind,
                         "status": threshold_status if independent else "unverified",
                         "threshold_status": threshold_status, "thresholds": dict(limits), "reasons": reasons}
    domains = {}
    for domain in ("image", "geometry"):
        selected = [name for name in contract["metrics"] if _METRICS[name][0] == domain]
        domains[domain] = {"status": _status([results[name]["status"] for name in selected]),
                           "metrics": [name for name in names if _METRICS[name][0] == domain],
                           "required_metrics": selected}
    status = _status([domains[domain]["status"] for domain in required])
    return {**trace, "version": 1, "status": status, "metrics": results, "identity_mismatches": [],
            "independent_validation": independent, "domains": domains, "required_domains": required,
            "settings_changes": settings_changes}
