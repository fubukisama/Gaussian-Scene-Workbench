# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Resolve quality mechanisms without changing camera, resolution or runtime policy.

This interface has no Torch/GUI imports or training side effects. Algorithm
availability is distinct from a requested configuration and quality acceptance.
"""
import copy
import hashlib
import json
import math
import re


class QualityPlanError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, detail
        super().__init__(code + (": " + detail if detail else ""))


def reject_legacy_quality_checkpoint(requested, checkpoint):
    """A tuple checkpoint has no complete quality sampler/strategy state."""
    if requested is not None and checkpoint:
        raise QualityPlanError('quality_legacy_checkpoint_unsupported')


def _finite_number(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def _parameters(value, defaults, section):
    if type(value) is not dict or set(value) - set(defaults):
        raise QualityPlanError('quality_parameters_invalid', section)
    result = dict(defaults, **value)
    for key, item in result.items():
        if not _finite_number(item) or item <= 0:
            raise QualityPlanError('quality_parameters_invalid', section + ':' + key)
    return result


def _supervision(value):
    if type(value) is not dict or set(value) - {'mask', 'depth'}:
        raise QualityPlanError('quality_supervision_invalid')
    result = {}
    for kind, row in value.items():
        if row is None:
            continue
        fields = ({'manifest', 'foreground_weight', 'background_alpha_weight',
                   'unit_to_training_scale', 'scale_evidence_identity'} if kind == 'mask' else
                  {'manifest', 'weight', 'unit_to_training_scale', 'scale_evidence_identity', 'allow_oracle'})
        if type(row) is not dict or set(row) - fields or not isinstance(row.get('manifest'), str) or not row['manifest'].strip():
            raise QualityPlanError('quality_supervision_invalid', kind)
        defaults = ({'foreground_weight': 1., 'background_alpha_weight': 0.} if kind == 'mask' else
                    {'weight': 1., 'allow_oracle': False})
        row = dict(defaults, **copy.deepcopy(row))
        for name in ('foreground_weight', 'background_alpha_weight', 'weight'):
            if name in row and (not _finite_number(row[name]) or row[name] < 0):
                raise QualityPlanError('quality_supervision_invalid', name)
        if kind == 'mask' and row['foreground_weight'] <= 0:
            raise QualityPlanError('quality_supervision_invalid', 'foreground_weight')
        if kind == 'mask' and ('unit_to_training_scale' in row or 'scale_evidence_identity' in row):
            if (not _finite_number(row.get('unit_to_training_scale')) or row['unit_to_training_scale'] <= 0
                    or not isinstance(row.get('scale_evidence_identity'), str)
                    or re.fullmatch(r'[0-9a-f]{64}', row['scale_evidence_identity']) is None):
                raise QualityPlanError('quality_supervision_invalid', 'mask_registration_scale_evidence')
        if kind == 'depth' and (row['weight'] <= 0 or type(row['allow_oracle']) is not bool
                or not _finite_number(row.get('unit_to_training_scale')) or row['unit_to_training_scale'] <= 0
                or not isinstance(row.get('scale_evidence_identity'), str)
                or re.fullmatch(r'[0-9a-f]{64}', row['scale_evidence_identity']) is None):
            raise QualityPlanError('quality_supervision_invalid', 'depth_scale_evidence')
        result[kind] = row
    return result


def resolve_quality_plan(requested=None, *, backend="3dgs"):
    if backend not in ("3dgs", "2dgs"):
        raise QualityPlanError("quality_backend_unsupported", str(backend))
    baseline_filter = "legacy_ewa" if backend == "3dgs" else "surfel_default"
    normalized = {"version": 1,
                  "densification": {"strategy": "standard", "parameters": {}},
                  "filter": {"mode": baseline_filter},
                  "geometry_lr_multiplier": 1.0}
    if requested is not None:
        if type(requested) is not dict or set(requested) - set(normalized) - {'supervision'}:
            raise QualityPlanError("quality_plan_invalid")
        if type(requested.get("version")) is not int or requested["version"] != 1:
            raise QualityPlanError("quality_plan_version_unsupported")
        multiplier = requested.get("geometry_lr_multiplier", 1.0)
        if not _finite_number(multiplier) or multiplier <= 0:
            raise QualityPlanError("quality_geometry_multiplier_invalid")
        normalized["geometry_lr_multiplier"] = float(multiplier)
        for section in ("densification", "filter"):
            if section in requested:
                value = requested[section]
                allowed = set(normalized[section]) | ({'pixel_variance', 'margin', 'update_interval'} if section == 'filter' else set())
                if type(value) is not dict or set(value) - allowed:
                    raise QualityPlanError("quality_plan_invalid", section)
                normalized[section].update(copy.deepcopy(value))
        strategy = normalized['densification']['strategy']
        defaults = {'standard': {}, 'abs_gs': {'split_threshold': .0008},
                    'pixel_gs': {'depth_factor': .37}, 'mcmc': {}}
        if not isinstance(strategy, str) or strategy not in defaults:
            raise QualityPlanError('quality_strategy_unavailable', str(strategy))
        if backend != '3dgs' and strategy != 'standard':
            raise QualityPlanError('quality_strategy_unsupported_backend', backend + ':' + strategy)
        if strategy == 'mcmc':
            given = normalized['densification']['parameters']
            settings = dict(cap=None, noise_lr=5e5, dead_threshold=.005, noise_threshold=.005,
                noise_sharpness=100., growth_rate=.05, opacity_reg=.01, scale_reg=.01, max_multiplicity=50)
            if type(given) is not dict or set(given) - set(settings):
                raise QualityPlanError('quality_parameters_invalid', 'mcmc')
            settings.update(given)
            if type(settings['cap']) is not int or not 1 <= settings['cap'] <= 2 ** 31 - 1:
                raise QualityPlanError('quality_mcmc_budget_required')
            for key, value in settings.items():
                if not _finite_number(value) or value < 0:
                    raise QualityPlanError('quality_parameters_invalid', 'mcmc:' + key)
            if (not 0 < settings['dead_threshold'] < 1 or not 0 < settings['noise_threshold'] < 1
                    or settings['noise_sharpness'] <= 0 or not 0 < settings['growth_rate'] <= 1
                    or type(settings['max_multiplicity']) is not int or not 1 <= settings['max_multiplicity'] <= 50):
                raise QualityPlanError('quality_parameters_invalid', 'mcmc')
            normalized['densification']['parameters'] = settings
        else:
            normalized['densification']['parameters'] = _parameters(
                normalized['densification']['parameters'], defaults[strategy], strategy)
        if normalized["filter"]["mode"] == "backend_default":
            normalized["filter"]["mode"] = baseline_filter
        if normalized["filter"]["mode"] in ("legacy_ewa", "surfel_default") and normalized["filter"]["mode"] != baseline_filter:
            raise QualityPlanError("quality_filter_unsupported_backend", backend + ":" + str(normalized["filter"]["mode"]))
        if normalized['filter']['mode'] == 'full_mip':
            if backend != '3dgs':
                raise QualityPlanError('quality_filter_unsupported_backend', backend + ':full_mip')
            values = dict(normalized['filter'])
            values.pop('mode')
            mip = _parameters(values, {'pixel_variance': .2, 'margin': .15, 'update_interval': 100}, 'full_mip')
            if mip['margin'] > 1 or type(mip['update_interval']) is not int or mip['update_interval'] > 1000000:
                raise QualityPlanError('quality_parameters_invalid', 'full_mip')
            normalized['filter'] = dict(mode='full_mip', **mip)
        elif normalized["filter"]["mode"] != baseline_filter:
            raise QualityPlanError("quality_filter_unavailable", str(normalized["filter"]["mode"]))
        elif set(normalized['filter']) != {'mode'}:
            raise QualityPlanError('quality_parameters_invalid', 'baseline_filter')
        supervision = _supervision(requested.get('supervision', {}))
        if supervision:
            normalized['supervision'] = supervision
        if (int(strategy != 'standard') + int(normalized['filter']['mode'] == 'full_mip')
                + len(supervision) > 1):
            raise QualityPlanError('quality_combination_unvalidated')
    effective = copy.deepcopy(normalized)
    identity = {"backend": backend, "requested": normalized, "effective": effective}
    digest = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":"),
                                       ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()
    return {"schema": "gsw.quality_plan_resolution/1", **identity, "plan_hash": digest,
            "training_overrides": ({} if normalized["geometry_lr_multiplier"] == 1.0 else
                                   {"geometry_lr_multiplier": normalized["geometry_lr_multiplier"]}),
            "reasons": [], "quality_accepted": False}


def geometry_rate_overrides(plan, optimization):
    """Return only explicitly selected position/scale changes; never mutate opt.

    Camera extent, normalisation, delays, feature/opacity rates, optimizer,
    resolution, schedule, density protection and runtime settings are untouched.
    """
    multiplier = plan["effective"]["geometry_lr_multiplier"]
    if multiplier == 1.0:
        return {}
    result = {}
    for name in ("position_lr_init", "position_lr_final", "scaling_lr"):
        value = optimization.get(name)
        if not _finite_number(value) or value < 0:
            raise QualityPlanError("quality_geometry_rate_invalid", name)
        value = value * multiplier
        if not math.isfinite(value):
            raise QualityPlanError("quality_geometry_rate_invalid", name)
        result[name] = value
    return result
