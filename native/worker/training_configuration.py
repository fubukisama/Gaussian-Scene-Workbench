# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""One configuration interface for independent native runtime controls.

Requested options are a stable version-two wire form. Effective options resolve
only real execution dependencies and never rewrite the requested form. This
module imports no trainer, Torch or GUI and starts no jobs.
"""
import copy
import math


class RuntimeConfigurationError(ValueError):
    def __init__(self, code, detail=''):
        self.code, self.detail = code, detail
        super().__init__(code + (': ' + detail if detail else ''))


def _keys(value, allowed, path):
    if type(value) is not dict:
        raise RuntimeConfigurationError('runtime_options_invalid', path)
    unknown = set(value) - set(allowed)
    if unknown:
        raise RuntimeConfigurationError('runtime_unknown_key', path + '.' + str(sorted(unknown, key=str)[0]))


def _integer(value, low, high, path):
    if type(value) is not int or not low <= value <= high:
        raise RuntimeConfigurationError('runtime_options_invalid', path)


def _fraction(value, high, path):
    if (type(value) not in (int, float) or not math.isfinite(value)
            or not .5 <= value <= high):
        raise RuntimeConfigurationError('runtime_options_invalid', path)


def _choice(value, allowed, path):
    if type(value) is not str or value not in allowed:
        raise RuntimeConfigurationError('runtime_options_invalid', path)


def _boolean(value, path):
    if type(value) is not bool:
        raise RuntimeConfigurationError('runtime_options_invalid', path)


def _defaults():
    return {'version': 2,
            'resourceGuard': {'enabled': True, 'mode': 'pause',
                              'usage_fraction': .9, 'minimum_spare_mib': 512,
                              'max_consecutive_denials': 3},
            'execution': {'image_staging': True, 'camera_prefetch': True,
                          'capacity_storage': True, 'surfel_auxiliary': True,
                          'loss_mode': 'auto', 'engine': 'stock',
                          'gpu_slots': 2, 'pinned_mib': 256, 'gpu_mib': 512},
            'monitoring': {'profile_interval': 30}}


def _legacy(value, resume=False):
    allowed = ('version', 'enabled', 'usage_fraction', 'minimum_spare_mib',
               'gpu_slots', 'pinned_mib', 'gpu_mib', 'profile_interval',
               'engine', 'growth_mode', 'loss_mode', 'camera_prefetch',
               'capacity_storage', 'surfel_auxiliary')
    _keys(value, allowed, 'performanceOptions')
    if type(value.get('version', 1)) is not int or value.get('version', 1) != 1:
        raise RuntimeConfigurationError('runtime_version_unsupported', 'performanceOptions.version')
    _boolean(value.get('enabled', not resume), 'performanceOptions.enabled')
    for key in ('camera_prefetch', 'capacity_storage', 'surfel_auxiliary'):
        _boolean(value.get(key, True), 'performanceOptions.' + key)
    _fraction(value.get('usage_fraction', .9), .95, 'performanceOptions.usage_fraction')
    for key, low, high, default in (
            ('minimum_spare_mib', 256, 8192, 512), ('gpu_slots', 1, 2, 2),
            ('pinned_mib', 16, 1024, 256), ('gpu_mib', 16, 2048, 512),
            ('profile_interval', 1, 1000, 30)):
        _integer(value.get(key, default), low, high, 'performanceOptions.' + key)
    for key, allowed_values, default in (
            ('engine', ('stock', 'fastergs'), 'stock'),
            ('loss_mode', ('auto', 'stock'), 'auto'),
            ('growth_mode', ('pause', 'defer'), 'pause')):
        _choice(value.get(key, default), allowed_values, 'performanceOptions.' + key)
    result = _defaults()
    enabled = value.get('enabled', not resume)
    result['resourceGuard'].update(
        enabled=enabled, mode=value.get('growth_mode', 'pause'),
        usage_fraction=value.get('usage_fraction', .9),
        minimum_spare_mib=value.get('minimum_spare_mib', 512))
    execution = result['execution']
    execution['image_staging'] = enabled
    for key in ('camera_prefetch', 'capacity_storage', 'surfel_auxiliary'):
        execution[key] = bool(enabled and value.get(key, True))
    execution['loss_mode'] = value.get('loss_mode', 'auto') if enabled else 'stock'
    for key in ('engine', 'gpu_slots', 'pinned_mib', 'gpu_mib'):
        if key in value:
            execution[key] = value[key]
    result['monitoring']['profile_interval'] = value.get('profile_interval', 30)
    return result


def legacy_performance_options(value=None, resume=False):
    """Historical v1 requested serialization for immutable legacy recipe IDs.

    This is never an execution gate. Execute its migrated v2 report instead.
    """
    value = {} if value is None else value
    _legacy(value, resume)  # Same strict legacy validation as every entry point.
    result = {'version': 1, 'enabled': not resume, 'usage_fraction': .9,
              'minimum_spare_mib': 512, 'gpu_slots': 2, 'pinned_mib': 256,
              'gpu_mib': 512, 'profile_interval': 30, 'engine': 'stock',
              'growth_mode': 'pause', 'loss_mode': 'auto', 'camera_prefetch': True,
              'capacity_storage': True, 'surfel_auxiliary': True}
    result.update(value)
    return result


def _merge(value, base):
    _keys(value, ('version', 'resourceGuard', 'execution', 'monitoring'), 'runtimeOptions')
    if type(value.get('version')) is not int or value['version'] != 2:
        raise RuntimeConfigurationError('runtime_version_unsupported', 'runtimeOptions.version')
    result = copy.deepcopy(base)
    for section in ('resourceGuard', 'execution', 'monitoring'):
        if section in value:
            _keys(value[section], base[section], 'runtimeOptions.' + section)
            result[section].update(value[section])
    guard, execution = result['resourceGuard'], result['execution']
    _boolean(guard['enabled'], 'resourceGuard.enabled')
    _choice(guard['mode'], ('pause', 'defer'), 'resourceGuard.mode')
    _fraction(guard['usage_fraction'], 1., 'resourceGuard.usage_fraction')
    _integer(guard['minimum_spare_mib'], 0, 1048576, 'resourceGuard.minimum_spare_mib')
    _integer(guard['max_consecutive_denials'], 1, 1000000, 'resourceGuard.max_consecutive_denials')
    for key in ('image_staging', 'camera_prefetch', 'capacity_storage', 'surfel_auxiliary'):
        _boolean(execution[key], 'execution.' + key)
    _choice(execution['loss_mode'], ('auto', 'stock'), 'execution.loss_mode')
    _choice(execution['engine'], ('stock', 'fastergs'), 'execution.engine')
    for key, low, high in (('gpu_slots', 1, 2), ('pinned_mib', 16, 1024), ('gpu_mib', 16, 2048)):
        _integer(execution[key], low, high, 'execution.' + key)
    _integer(result['monitoring']['profile_interval'], 1, 1000000, 'monitoring.profile_interval')
    return result


def normalize_runtime_options(runtime_options=None, legacy_performance=None,
                              resume=False, checkpoint_runtime=None):
    """Return requested/effective options plus migration and dependency reasons."""
    _boolean(resume, 'resume')
    base = (_merge(checkpoint_runtime, _defaults()) if checkpoint_runtime is not None
            else _legacy({'enabled': False}, resume=True) if resume else _defaults())
    if runtime_options is not None:
        requested = _merge(runtime_options, base)
        source, version, notes = 'runtimeOptions', 2, []
        if checkpoint_runtime is not None:
            notes.append('checkpoint_runtime_inherited')
        if legacy_performance is not None:
            migrated = _legacy(legacy_performance, resume)
            if migrated != requested:
                raise RuntimeConfigurationError('runtime_configuration_conflict', 'performanceOptions/runtimeOptions')
            notes.append('equivalent_legacy_input')
    elif legacy_performance is not None:
        requested = _legacy(legacy_performance, resume)
        source, version = 'performanceOptions', 1
        notes = ['legacy_parent_effective_features_preserved']
    else:
        requested = base
        source, version, notes = ('checkpoint_runtime', 2, []) if checkpoint_runtime is not None else (
            ('legacy_resume_without_policy', None, ['legacy_effective_features_preserved'])
            if resume else ('defaults', None, []))
    effective, reasons = copy.deepcopy(requested), []
    if (requested['execution']['camera_prefetch'] and
            not requested['execution']['image_staging']):
        effective['execution']['camera_prefetch'] = False
        reasons.append({'feature': 'execution.camera_prefetch', 'code': 'requires_image_staging'})
    return {'requested': requested, 'effective': effective,
            'reasons': reasons, 'migration': {'source': source, 'from_version': version,
                                            'to_version': 2, 'notes': notes}}


def flat_runtime_options(report):
    """Internal adapter for existing trainer helpers; no shared parent gate."""
    options = report['effective']
    guard = options['resourceGuard']
    result = dict(options['execution'], **options['monitoring'])
    result.update(version=2, enabled=guard['enabled'],
                  resource_guard_enabled=guard['enabled'], growth_mode=guard['mode'],
                  usage_fraction=guard['usage_fraction'],
                  minimum_spare_mib=guard['minimum_spare_mib'],
                  max_consecutive_denials=guard['max_consecutive_denials'])
    return result


def runtime_options_from_flat(value):
    """Read the version-two internal checkpoint adapter, not a legacy policy."""
    shape = flat_runtime_options({'effective': _defaults()})
    _keys(value, shape, 'performance')
    if type(value.get('version')) is not int or value['version'] != 2:
        raise RuntimeConfigurationError('runtime_version_unsupported', 'performance.version')
    if ('enabled' in value and 'resource_guard_enabled' in value and
            value['enabled'] != value['resource_guard_enabled']):
        raise RuntimeConfigurationError('runtime_configuration_conflict', 'performance.enabled')
    wire = {'version': 2, 'resourceGuard': {}, 'execution': {}, 'monitoring': {}}
    for source, target in (('resource_guard_enabled', 'enabled'), ('growth_mode', 'mode'),
                           ('usage_fraction', 'usage_fraction'),
                           ('minimum_spare_mib', 'minimum_spare_mib'),
                           ('max_consecutive_denials', 'max_consecutive_denials')):
        if source in value:
            wire['resourceGuard'][target] = value[source]
    if 'enabled' in value and 'resource_guard_enabled' not in value:
        wire['resourceGuard']['enabled'] = value['enabled']
    for key in _defaults()['execution']:
        if key in value:
            wire['execution'][key] = value[key]
    if 'profile_interval' in value:
        wire['monitoring']['profile_interval'] = value['profile_interval']
    return _merge(wire, _defaults())
