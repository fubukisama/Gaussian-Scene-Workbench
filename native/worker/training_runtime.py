# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Native performance policy separate from optimizer/dataset identity.

Old checkpoints without this policy retain their original runtime mode. New
checkpoints retain the selected policy, without adding attributes to upstream
OptimizationParams or relabelling a renderer/optimizer as compatible.
"""
import math
import os
import json
import sys

_FRESH_LOSS_HISTORY = object()


class LossExecutionRecord:
    """Bounded, observed engine history, not a pre-compute availability guess."""
    def __init__(self, requested, state=_FRESH_LOSS_HISTORY):
        if requested not in ('stock', 'fused_l1_ssim'):
            raise ValueError('Invalid requested loss engine')
        self.requested = requested
        self._requested_modes = [requested]
        self._last = 'unobserved'
        self._counts, self._transitions = {}, []
        self._reason = ''
        self._truncated = False
        self._prior_unavailable = state is None
        self._history_invalid = False
        self._session_observations = 0
        self._session_last = 'unobserved'
        if state is not _FRESH_LOSS_HISTORY and state is not None:
            if not self._restore_history(state):
                self._prior_unavailable = self._history_invalid = True

    def _restore_history(self, state):
        """Reject reporting metadata without blocking actual model restore."""
        engines = ('stock', 'fused_l1_ssim')
        try:
            if type(state) is not dict or state['requested'] not in engines:
                return False
            counts, transitions = state['counts'], state['transitions']
            last = state['last_effective']
            if (type(counts) is not dict or type(transitions) is not list or
                    last not in engines + ('unobserved',) or any(
                        key not in engines or type(value) is not int or not 0 <= value <= 2 ** 53 - 1
                        for key, value in counts.items())):
                return False
            modes = state.get('requested_modes', [state['requested']])
            if (type(modes) is not list or not modes or any(type(mode) is not str or mode not in engines for mode in modes)
                    or len(set(modes)) != len(modes) or state['requested'] not in modes):
                return False
            flags = [state['history_truncated'], state.get('prior_history_unavailable', False),
                     state.get('history_invalid', False)]
            if any(type(value) is not bool for value in flags):
                return False
            prior_iteration, switches = 0, {}
            for item in transitions:
                if (type(item) is not dict or type(item['iteration']) is not int or
                        not prior_iteration < item['iteration'] <= 2 ** 53 - 1 or item['effective'] not in engines or
                        type(item['reason']) is not str):
                    return False
                prior_iteration = item['iteration']
                switches[item['effective']] = switches.get(item['effective'], 0) + 1
            if (any(counts.get(engine, 0) < count for engine, count in switches.items()) or
                    (transitions and transitions[-1]['effective'] != last) or
                    (not transitions and (last != 'unobserved' or any(counts.values()))) or
                    (transitions and counts.get(last, 0) < 1)):
                return False
            # Commit only after every field validates, preserving a safe empty
            # record if any nonessential metadata was malformed.
            self._counts = dict(counts)
            self._transitions = [{key: item[key] for key in ('iteration', 'effective', 'reason')}
                                 for item in transitions[-256:]]
            self._last = last
            self._reason = self._transitions[-1]['reason'] if self._transitions else ''
            self._truncated = flags[0] or len(transitions) > 256
            self._prior_unavailable = flags[1] or flags[2]
            self._history_invalid = flags[2]
            self._requested_modes = list(modes)
            if self.requested not in self._requested_modes:
                self._requested_modes.append(self.requested)
            return True
        except (KeyError, TypeError, ValueError):
            return False

    def observe(self, iteration, status):
        engine, reason = status['effective'], status.get('reason', '')
        if (type(iteration) is not int or iteration < 1 or engine not in ('stock', 'fused_l1_ssim')
                or type(reason) is not str):
            raise ValueError('Invalid observed loss engine')
        changed = self._session_observations == 0 or (engine, reason) != (self._last, self._reason)
        self._counts[engine] = self._counts.get(engine, 0) + 1
        self._session_observations += 1
        self._session_last = engine
        if changed:
            if len(self._transitions) >= 256:
                self._transitions.pop(0)
                self._truncated = True
            self._transitions.append({'iteration': iteration, 'effective': engine, 'reason': reason})
        self._last, self._reason = engine, reason
        return changed

    def snapshot(self):
        return {'requested': self.requested, 'last_effective': self._last,
                'requested_modes': list(self._requested_modes),
                'counts': dict(self._counts), 'transitions': [dict(item) for item in self._transitions],
                'history_truncated': self._truncated,
                'prior_history_unavailable': self._prior_unavailable, 'history_invalid': self._history_invalid,
                'session_observations': self._session_observations, 'session_last_effective': self._session_last}


def execution_quality_profile(backend, dataset, optimization, pipeline, performance,
                              images, renderer, loss):
    """Record effective controls without asserting unmeasured output quality."""
    if backend not in ('3dgs', '2dgs'):
        raise ValueError('Invalid quality profile backend')
    value = {'version': 1, 'backend': backend,
             'settings': {'dataset': dict(dataset), 'optimization': dict(optimization),
                          'pipeline': dict(pipeline), 'performance': dict(performance)},
             'images': dict(images), 'execution': {'renderer': renderer, 'loss': loss},
             'validation': {'status': 'unverified', 'reason': 'no_independent_measurements',
                            'training_metric_scope': 'training_fit'}}
    # A strict JSON copy prevents live option mutation and non-finite metrics
    # from turning an execution record into an ambiguous quality certificate.
    return json.loads(json.dumps(value, allow_nan=False))


class GrowthQuality:
    def __init__(self, mode='pause', state=None, max_consecutive_denials=3):
        if (mode not in ('pause', 'defer') or type(max_consecutive_denials) is not int
                or not 1 <= max_consecutive_denials <= 1000000):
            raise ValueError('Invalid growth quality mode')
        self.mode = mode
        self.max_consecutive_denials = max_consecutive_denials
        self._observed_this_session = False
        self._state = {'admitted_events': 0, 'deferred_events': 0,
                       'last_growth_deferred': False, 'consecutive_deferred': 0,
                       'last_reason': 'unobserved'}
        if isinstance(state, dict):
            for key in ('admitted_events', 'deferred_events', 'consecutive_deferred'):
                if type(state.get(key)) is int and state[key] >= 0:
                    self._state[key] = state[key]
            if type(state.get('last_growth_deferred')) is bool:
                self._state['last_growth_deferred'] = state['last_growth_deferred']
            if state.get('last_reason') in ('admitted', 'vram_pressure', 'probe_unavailable',
                                            'invalid_observation', 'estimate_unavailable'):
                self._state['last_reason'] = state['last_reason']

    def observe(self, allowed, reason=None):
        self._observed_this_session = True
        self._state['admitted_events' if allowed else 'deferred_events'] += 1
        self._state['last_growth_deferred'] = not allowed
        self._state['consecutive_deferred'] = 0 if allowed else self._state['consecutive_deferred'] + 1
        self._state['last_reason'] = 'admitted' if allowed else (reason or 'vram_pressure')

    def should_pause(self):
        return (self.mode == 'pause' and self._observed_this_session
                and self._state['consecutive_deferred'] >= self.max_consecutive_denials)

    def snapshot(self):
        return dict(self._state)


def observed_memory(torch):
    from native.worker.training_resource_policy import MemoryObservation, is_cuda_out_of_memory
    free, total = torch.cuda.mem_get_info()
    reusable = 0
    metadata = {'allocator_status': 'unavailable'}
    if type(free) is not int or type(total) is not int or not 0 <= free <= total or total <= 0:
        return MemoryObservation(free, total, allocator_status='invalid')
    try:
        stats = torch.cuda.memory_stats()
        values = [stats[key] for key in ('reserved_bytes.all.current',
                  'active_bytes.all.current', 'inactive_split_bytes.all.current')]
        if (all(type(value) is int and value >= 0 for value in values)
                and values[1] <= values[0] and values[2] <= values[0] - values[1]):
            reserved, active, fragments = values
            # Active includes pending stream use. Inactive split fragments are
            # deliberately excluded. The admission policy credits only HALF
            # of this fully inactive storage, never all reserved CUDA memory.
            reusable = min(max(0, reserved - active - fragments), total - free)
            allocated = stats.get('allocated_bytes.all.current')
            if type(allocated) is not int or not 0 <= allocated <= reserved:
                allocated = None
            metadata = {'allocated_bytes': allocated, 'reserved_bytes': reserved,
                        'active_bytes': active, 'inactive_split_bytes': fragments,
                        'allocator_status': 'known' if allocated is not None else 'partial'}
        else:
            metadata = {'allocator_status': 'invalid'}
    except (KeyError, RuntimeError, AttributeError) as error:
        if is_cuda_out_of_memory(error):
            raise
    return MemoryObservation(free, total, int(reusable), **metadata)


def validate_resume_renderer(recorded, effective):
    if recorded is not None and recorded != effective:
        raise ValueError('resume_renderer_environment_changed')


def runtime_configuration(control, checkpoint_runtime=None, checkpoint_performance=None):
    from native.worker.training_configuration import normalize_runtime_options, runtime_options_from_flat
    supplied = control.get('runtimeOptions')
    legacy = control.get('performance')
    resume = bool(control.get('resume'))
    if checkpoint_runtime is None and checkpoint_performance is not None:
        if isinstance(checkpoint_performance, dict) and checkpoint_performance.get('version') == 2:
            checkpoint_runtime = runtime_options_from_flat(checkpoint_performance)
        else:
            checkpoint_runtime = normalize_runtime_options(
                legacy_performance=checkpoint_performance, resume=resume)['requested']
    if isinstance(legacy, dict) and legacy.get('version') == 2:
        adapted = runtime_options_from_flat(legacy)
        if supplied is None:
            supplied, legacy = adapted, None
        elif normalize_runtime_options(supplied, resume=resume,
                                     checkpoint_runtime=checkpoint_runtime)['requested'] != adapted:
            raise ValueError('runtime_configuration_conflict: native control')
        else:
            legacy = None
    report = normalize_runtime_options(supplied, legacy, resume=resume,
                                       checkpoint_runtime=checkpoint_runtime)
    if control.get('backend') == '3dgs' and report['effective']['execution']['surfel_auxiliary']:
        report['effective']['execution']['surfel_auxiliary'] = False
        report['reasons'].append({'feature': 'execution.surfel_auxiliary', 'code': 'not_applicable_to_3dgs'})
    return report


def performance_configuration(control, checkpoint_performance=None):
    from native.worker.training_configuration import flat_runtime_options
    return flat_runtime_options(runtime_configuration(control, checkpoint_performance=checkpoint_performance))


def configure_image_environment(configuration, environment=None):
    environment = os.environ if environment is None else environment
    if configuration['image_staging']:
        environment['GSW_TRAINING_IMAGE_PIPELINE'] = 'bounded'
        for suffix, key in (('GPU_SLOTS', 'gpu_slots'), ('PINNED_MIB', 'pinned_mib'), ('GPU_MIB', 'gpu_mib')):
            environment['GSW_TRAINING_IMAGE_' + suffix] = str(configuration[key])
    else:
        environment.pop('GSW_TRAINING_IMAGE_PIPELINE', None)


def configure_native_images(control, return_report=False):
    checkpoint_policy = None
    checkpoint_runtime = None
    if control.get('resume'):
        from native.worker.training_checkpoint import read_manifest
        manifest, _ = read_manifest(control['output'], verify=False)
        checkpoint_policy = manifest.get('performance')
        checkpoint_runtime = manifest.get('runtime_options')
    from native.worker.training_configuration import flat_runtime_options
    report = runtime_configuration(control, checkpoint_runtime, checkpoint_policy)
    configuration = flat_runtime_options(report)
    configure_image_environment(configuration)
    return report if return_report else configuration


def _entry_report(control, current):
    entry = control.get('runtimeConfigurationReport')
    if entry is None:
        return None
    if (type(entry) is not dict or set(entry) != {'requested', 'effective', 'reasons', 'migration'}
            or any(entry[key] != current[key] for key in ('requested', 'effective', 'reasons'))):
        raise ValueError('runtime_entry_configuration_report_invalid')
    migration = entry['migration']
    if (type(migration) is not dict or set(migration) != {'source', 'from_version', 'to_version', 'notes'}
            or migration['source'] not in ('runtimeOptions', 'performanceOptions', 'checkpoint_runtime',
                                            'legacy_resume_without_policy', 'defaults')
            or migration['from_version'] is not None and type(migration['from_version']) is not int
            or migration['from_version'] not in (None, 1, 2) or migration['to_version'] != 2
            or type(migration['notes']) is not list
            or any(type(note) is not str or not 0 < len(note) <= 4096 for note in migration['notes'])):
        raise ValueError('runtime_entry_configuration_report_invalid')
    return json.loads(json.dumps(entry, allow_nan=False))


class NativeTrainingRuntime:
    def __init__(self, control, torch, emit, backend):
        from native.worker.training_step_profile import IterationProfiler
        from native.worker.training_resource_policy import GrowthAdmission
        from native.worker.training_image_pipeline import TrainingImagePipeline, ImagePipelineConfig
        from native.worker.training_loss import PhotometricLoss
        from native.worker.training_configuration import flat_runtime_options
        self.runtime_configuration = configure_native_images(dict(control, backend=backend), return_report=True)
        entry = _entry_report(control, self.runtime_configuration)
        if entry is not None:
            self.runtime_configuration['entry_migration'] = entry
        self.configuration = flat_runtime_options(self.runtime_configuration)
        self.output = control.get('output')
        self.quality_profile = None
        self.image_summary = {}
        self.storage = None
        self.storage_status = {'enabled': False, 'reason': 'not_bound'}
        self.surfel_status = {'available': False, 'reason': 'not_applicable' if backend == '3dgs' else 'not_registered',
                              'omitted_calls': 0, 'required_calls': 0}
        self.lookahead_opportunities = 0
        self.backend, self.torch, self.emit = backend, torch, emit
        self.engine = 'stock'
        self.growth_quality = GrowthQuality(self.configuration['growth_mode'],
            max_consecutive_denials=self.configuration['max_consecutive_denials'])
        self.last_growth_decision = None
        self.runtime_policy_history = []
        self.profiler = IterationProfiler(backend, self.configuration['profile_interval'],
            torch_module=torch, emit=emit, metadata=lambda: {
                'renderer_engine': self.engine,
                'loss_engine': self.loss.status()['effective'] if self.loss else 'stock'})
        self.admission = (GrowthAdmission(lambda: observed_memory(torch),
            self.configuration['usage_fraction'], self.configuration['minimum_spare_mib'] * 1024 ** 2,
            cache_reclaimer=getattr(torch.cuda, 'empty_cache', None))
            if self.configuration['resource_guard_enabled'] else None)
        self.images = (TrainingImagePipeline(torch, config=ImagePipelineConfig.from_environment())
                       if self.configuration['image_staging'] else None)
        module_path = os.environ.get('GSW_TRAINING_LOSS_MODULE_PATH')
        if module_path and module_path not in sys.path:
            sys.path.append(module_path)  # Never shadow the configured Torch runtime.
        self.loss = None
        self.loss_execution = None
        self._loss_history_state = _FRESH_LOSS_HISTORY
        self._loss_class = PhotometricLoss
        emit('[gsw-training-runtime]', dict(self.configuration, backend=backend))
        emit('[gsw-training-runtime-configuration]', dict(self.runtime_configuration, backend=backend))

    def photometric_loss(self, l1, ssim):
        self.loss = self._loss_class(self.torch, l1, ssim,
            mode='fused_l1_ssim' if self.configuration['loss_mode'] == 'auto' else 'stock')
        self.emit('[gsw-training-loss-engine]', self.loss.status())
        self.loss_execution = LossExecutionRecord(self.loss.mode, self._loss_history_state)
        return self.loss

    def observe_loss(self, iteration):
        status = self.loss.status()
        changed = self.loss_execution.observe(iteration, status)
        if changed:
            self._refresh_profile()
            self._save_profile()
            history = self.loss_execution.snapshot()
            self.emit('[gsw-training-loss-transition]', {
                'version': 1, 'backend': self.backend, 'iteration': iteration,
                'requested': history['requested'], 'requested_modes': history['requested_modes'],
                'effective': status['effective'], 'reason': status.get('reason', ''),
                'counts': history['counts'], 'history_invalid': history['history_invalid'],
                'prior_history_unavailable': history['prior_history_unavailable']})
        return changed

    def report_images(self, cameras):
        from native.worker.training_image_pipeline import total_host_bytes
        self.image_summary = {'cameras': len(cameras), 'dimensions': sorted({
            (int(camera.image_width), int(camera.image_height)) for camera in cameras})}
        self.emit('[gsw-training-image-cache]', {
            'version': 1, 'backend': self.backend, 'cameras': len(cameras),
            'host_cache_bytes': total_host_bytes(cameras),
            'bounded': self.configuration['image_staging']})

    def report_profile(self, dataset, optimization, pipeline):
        self.quality_profile = execution_quality_profile(self.backend, vars(dataset),
            vars(optimization), vars(pipeline), self.configuration, self.image_summary,
            self.engine, self.loss.status()['effective'] if self.loss else 'stock')
        self._refresh_profile()
        self._save_profile()
        self.emit('[gsw-training-quality-profile]', self.quality_profile)

    def _save_profile(self):
        if self.output and self.quality_profile:
            from pathlib import Path
            from native.worker.training_checkpoint import atomic_json
            atomic_json(Path(self.output) / 'quality-profile.json', self.quality_profile)

    def _refresh_profile(self):
        if not self.quality_profile:
            return
        if self.storage:
            self.storage_status.update(self.storage.snapshot())
        execution = self.quality_profile['execution']
        loss = self.loss_execution.snapshot() if self.loss_execution else {'last_effective': 'unobserved'}
        execution['loss'] = loss['last_effective']
        execution['loss_history'] = loss
        execution['runtime_configuration'] = json.loads(json.dumps(self.runtime_configuration))
        execution['runtime_policy_history'] = json.loads(json.dumps(self.runtime_policy_history))
        requested = self.runtime_configuration['requested']['execution']
        execution['features'] = {
            'capacity_storage': dict(self.storage_status,
                requested=requested['capacity_storage']),
            'surfel_auxiliary': dict(self.surfel_status,
                requested=requested['surfel_auxiliary']),
            'camera_prefetch': {'requested': requested['camera_prefetch'],
                                'effective': self.configuration['camera_prefetch'],
                                'reason': '' if self.configuration['camera_prefetch'] or not requested['camera_prefetch'] else 'requires_image_staging',
                                'opportunities': self.lookahead_opportunities},
            'image_staging': {'requested': requested['image_staging'],
                              'effective': self.images is not None},
            'photometric_loss': self.loss.status() if self.loss else {'effective': 'unobserved'}}
        if self.images:
            execution['image_staging'] = self.images.stats()

    def register_surfel_adapter(self, capability):
        self.surfel_status.update(capability)

    def observe_surfel_output(self, required, result):
        if required:
            self.surfel_status['required_calls'] += 1
        if 'surf_normal' not in result:
            self.surfel_status['omitted_calls'] += 1

    def bind_model(self, model):
        if self.configuration['capacity_storage']:
            from native.worker.training_capacity_storage import install_capacity_storage
            self.storage = install_capacity_storage(self.torch, model, self.backend,
                memory_probe=lambda: observed_memory(self.torch))
            status = dict(getattr(model, '_gsw_capacity_storage_status', {}))
            if self.storage:
                status.update(self.storage.snapshot())
            self.storage_status = status
            self.emit('[gsw-training-capacity-storage]', status)
        else:
            self.storage_status = {'enabled': False, 'reason': 'disabled_by_policy'}

    def image(self, camera, cameras=None, pending_indices=None, python_rng_state=None):
        from native.worker.training_image_pipeline import ReadyImage
        if self.images:
            next_camera = None
            if self.configuration['camera_prefetch'] and cameras is not None and pending_indices is not None:
                from native.worker.training_camera_lookahead import predict_next_camera
                next_camera = (predict_next_camera(cameras, pending_indices) if python_rng_state is None
                               else predict_next_camera(cameras, pending_indices, python_rng_state))
                if next_camera is not None:
                    self.lookahead_opportunities += 1
            return self.images.prepare(camera, next_camera=next_camera)
        def cuda(tensor):
            return tensor.cuda() if tensor is not None else None
        return ReadyImage(cuda(camera.original_image), cuda(getattr(camera, 'alpha_mask', None)),
                          cuda(getattr(camera, 'invdepthmap', None)), cuda(getattr(camera, 'depth_mask', None)))

    def admit(self, model, threshold, extent, iteration, **quality_density_arguments):
        from native.worker.training_resource_policy import admit_densification
        def observed(kind, value):
            if kind == '[gsw-training-growth]':
                self.last_growth_decision = dict(value)
            self.emit(kind, value)
        allowed = admit_densification(self.torch, model, self.backend, threshold, extent,
                                      self.admission, observed, iteration, **quality_density_arguments)
        if self.admission is not None:
            self.growth_quality.observe(allowed, (self.last_growth_decision or {}).get('reason'))
        return allowed

    def pause_for_resources(self):
        return self.admission is not None and self.growth_quality.should_pause()

    def admit_mcmc_growth(self, model, growth, iteration):
        """Same resource policy, explicit MCMC additions instead of split stats."""
        if self.admission is None:
            return True
        from dataclasses import asdict
        base = self.admission.evaluate(model, self.backend, clone_count=0, split_count=0)
        if not base.allowed:
            raise ValueError('quality_mcmc_iteration_headroom_unavailable')
        decision = self.admission.evaluate(model, self.backend, clone_count=growth, split_count=0)
        event = asdict(decision)
        event.update(version=1, backend=self.backend, iteration=iteration,
            state='admitted' if decision.allowed else 'deferred', clone_count=growth, split_count=0,
            candidate_counts_known=True, guard_enabled=True)
        self.last_growth_decision = event
        self.emit('[gsw-training-growth]', event)
        self.growth_quality.observe(decision.allowed, decision.reason)
        return decision.allowed

    def resource_pause_reason(self):
        reason = self.growth_quality.snapshot().get('last_reason')
        if reason in ('probe_unavailable', 'invalid_observation'):
            return 'resource_probe_unavailable'
        if reason == 'estimate_unavailable':
            return 'resource_estimate_unavailable'
        return 'vram_pressure'

    def capture(self, state):
        state['performance'] = dict(self.configuration)
        state['runtime_options'] = json.loads(json.dumps(self.runtime_configuration['requested']))
        state['runtime_configuration'] = json.loads(json.dumps(self.runtime_configuration))
        state['runtime_policy_history'] = json.loads(json.dumps(self.runtime_policy_history))
        state['renderer_engine'] = self.engine
        state['growth_quality'] = self.growth_quality.snapshot()
        if self.quality_profile:
            self._refresh_profile()
            self._save_profile()
            state['quality_profile'] = json.loads(json.dumps(self.quality_profile, allow_nan=False))

    def restore_quality(self, state):
        from native.worker.training_configuration import normalize_runtime_options
        history = state.get('runtime_policy_history', [])
        if type(history) is not list:
            raise ValueError('runtime_policy_history_invalid')
        restored_history = []
        for entry in history:
            if (type(entry) is not dict or set(entry) != {'iteration', 'before', 'after', 'reason'}
                    or type(entry['iteration']) is not int or not 0 <= entry['iteration'] <= 2 ** 53 - 1
                    or type(entry['reason']) is not str or not 0 < len(entry['reason']) <= 4096):
                raise ValueError('runtime_policy_history_invalid')
            for key in ('before', 'after'):
                normalize_runtime_options(entry[key])
            restored_history.append(json.loads(json.dumps(entry, allow_nan=False)))
        before = runtime_configuration({'resume': True},
            checkpoint_runtime=state.get('runtime_options'),
            checkpoint_performance=state.get('performance'))['requested']
        after = self.runtime_configuration['requested']
        if before != after:
            iteration = state.get('iteration')
            if type(iteration) is not int or not 0 <= iteration <= 2 ** 53 - 1:
                raise ValueError('runtime_policy_history_iteration_invalid')
            change = {'iteration': iteration, 'before': before, 'after': after,
                      'reason': 'explicit_resume_runtime_override'}
            restored_history.append(json.loads(json.dumps(change, allow_nan=False)))
            self.emit('[gsw-training-runtime-policy-change]', change)
        self.runtime_policy_history = restored_history
        self.growth_quality = GrowthQuality(self.configuration['growth_mode'], state.get('growth_quality'),
            max_consecutive_denials=self.configuration['max_consecutive_denials'])
        profile = state.get('quality_profile')
        if profile is None and 'quality_profile' not in state:
            self._loss_history_state = None  # Legacy: prior execution is unknown.
        elif type(profile) is dict and type(profile.get('execution')) is dict:
            self._loss_history_state = profile['execution'].get('loss_history')
        else:
            self._loss_history_state = {}  # Present but invalid, never a model-resume blocker.

    def close(self):
        self.emit('[gsw-training-growth-quality]', dict(version=1, backend=self.backend, **self.growth_quality.snapshot()))
        if self.quality_profile:
            self._refresh_profile()
            self.quality_profile['growth'] = self.growth_quality.snapshot()
            if self.storage:
                self.quality_profile['storage'] = self.storage.snapshot()
            self._save_profile()
        if self.images:
            self.emit('[gsw-training-image-staging]', self.images.stats())
            self.images.close()
        if self.loss:
            self.emit('[gsw-training-loss-engine]', self.loss.status())

    def select_renderer(self, opt, dataset, pipe, stock, checkpoint_engine=None):
        if self.configuration['engine'] == 'stock':
            if checkpoint_engine == 'fastergs':
                self.emit('[gsw-training-acceleration]', {
                    'requested': 'stock', 'effective': 'stock', 'resumeBlocked': True,
                    'reasons': ['fastergs_resume_engine_not_compatible']})
            validate_resume_renderer(checkpoint_engine, 'stock')
            return stock
        from native.worker.training_acceleration import select_acceleration
        report = None
        proof = os.environ.get('GSW_FASTGS_RUNTIME_PROOF')
        if proof:
            try:
                with open(proof, encoding='utf-8') as stream:
                    report = json.load(stream)
            except (OSError, ValueError):
                pass
        options = dict(vars(opt))
        options.update(sh_degree=dataset.sh_degree,
                       requires_depth=bool(getattr(dataset, 'depths', '') and
                           (getattr(opt, 'depth_l1_weight_init', 0) > 0 or getattr(opt, 'depth_l1_weight_final', 0) > 0)),
                       requires_normals=self.backend == '2dgs',
                       requires_screen_radii=opt.densify_until_iter > 0,
                       compute_cov3D_python=pipe.compute_cov3D_python,
                       convert_SHs_python=pipe.convert_SHs_python, debug=pipe.debug,
                       resume=checkpoint_engine is not None)
        decision = select_acceleration(self.backend, self.configuration['engine'], options, report, checkpoint_engine)
        if checkpoint_engine is not None and decision['effective'] != checkpoint_engine:
            self.emit('[gsw-training-acceleration]', dict(decision, resumeBlocked=True))
            validate_resume_renderer(checkpoint_engine, decision['effective'])
        self.emit('[gsw-training-acceleration]', decision)
        if decision['effective'] == 'fastergs':
            try:
                import importlib
                import hashlib
                extension = importlib.import_module('FasterGSCudaBackend._C')
                with open(extension.__file__, 'rb') as stream:
                    actual_hash = hashlib.sha256(stream.read()).hexdigest()
                if (report.get('extensionSha256') != actual_hash or
                        report.get('pythonMajorMinor') != '{}.{}'.format(*sys.version_info[:2]) or
                        report.get('torch') != self.torch.__version__ or
                        report.get('cuda') != self.torch.version.cuda or
                        report.get('device') != self.torch.cuda.get_device_name()):
                    raise RuntimeError('fastergs_runtime_fingerprint_changed')
                from gsw_fastgs_adapter import render
            except (ImportError, OSError, RuntimeError) as error:
                self.emit('[gsw-training-acceleration]', {
                    'requested': 'fastergs', 'effective': 'stock',
                    'reasons': ['fastergs_runtime_not_verified'], 'detail': str(error),
                    'resumeBlocked': checkpoint_engine == 'fastergs'})
                validate_resume_renderer(checkpoint_engine, 'stock')
                return stock
            self.engine = 'fastergs'
            return render
        return stock
