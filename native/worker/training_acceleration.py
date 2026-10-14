# SPDX-FileCopyrightText: 2026 fubukisama
# SPDX-License-Identifier: MIT
"""Explicit optional-renderer admission without importing Torch in UI workers."""

FASTGS_UPSTREAM_COMMIT = "c60ea269b7cf3ebda527d640b6fdd31462f91b41"
FASTGS_ADAPTER_VERSION = 1


def _runtime_verified(report):
    if not isinstance(report, dict) or report.get("ready") is not True:
        return False
    checks = report.get("checks")
    return (report.get("engine") == "fastergs"
            and report.get("upstreamCommit") == FASTGS_UPSTREAM_COMMIT
            and report.get("adapterVersion") == FASTGS_ADAPTER_VERSION
            and isinstance(checks, dict)
            and all(checks.get(name) is True for name in
                    ("render", "backward", "train", "sh0to3", "antialiasing", "exposureBackward")))


def select_acceleration(backend, requested, options, runtime_report=None,
                        checkpoint_engine=None):
    """Select without mutating options or pretending unsupported outputs exist.

    Caller passes effective options, including actual depth/radius requirements.
    A resume must additionally carry the checkpoint's recorded renderer engine;
    old/unspecified engines remain on stock. The runtime report comes from the
    bounded real CUDA probe in the selected experimental Python, not imports.
    """
    if backend not in ("3dgs", "2dgs") or requested not in ("stock", "fastergs") or not isinstance(options, dict):
        raise ValueError("invalid_acceleration_selection")
    if requested == "stock":
        return {"requested": requested, "effective": "stock", "reasons": []}
    if backend == "2dgs":
        return {"requested": requested, "effective": "stock",
                "reasons": ["fastergs_surfel_not_supported"]}
    if not _runtime_verified(runtime_report):
        return {"requested": requested, "effective": "stock",
                "reasons": ["fastergs_runtime_not_verified"]}
    reasons = []
    if options.get("resume", False) or checkpoint_engine is not None:
        if checkpoint_engine != "fastergs":
            reasons.append("fastergs_resume_engine_not_compatible")
        elif runtime_report.get("sameEngineAdamStateRoundtrip") is not True:
            reasons.append("fastergs_optimizer_resume_not_verified")
    if options.get("densify_until_iter", 15000) != 0:
        reasons.append("fastergs_requires_fixed_density")
    if options.get("optimizer_type", "default") != "default":
        reasons.append("fastergs_sparse_adam_not_supported")
    unsupported_flags = (
        ("requires_depth", "fastergs_depth_not_supported"),
        ("requires_normals", "fastergs_normals_not_supported"),
        ("requires_screen_radii", "fastergs_screen_radii_not_supported"),
        ("compute_cov3D_python", "fastergs_precomputed_covariance_not_supported"),
        ("convert_SHs_python", "fastergs_precomputed_color_not_supported"),
        ("debug", "fastergs_debug_not_supported"),
        ("custom_activations", "fastergs_custom_activations_not_supported"),
    )
    for flag, reason in unsupported_flags:
        if options.get(flag, False):
            reasons.append(reason)
    if options.get("override_color") is not None:
        reasons.append("fastergs_precomputed_color_not_supported")
    degree = options.get("sh_degree", 3)
    if isinstance(degree, bool) or not isinstance(degree, int) or not 0 <= degree <= 3:
        reasons.append("fastergs_sh_degree_not_supported")
    return {"requested": requested, "effective": "stock" if reasons else "fastergs",
            "reasons": list(dict.fromkeys(reasons))}
