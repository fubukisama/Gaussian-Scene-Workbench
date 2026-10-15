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
    def __init__(self, mode='pause', state=None):
        if mode not in ('pause', 'defer'):
            raise ValueError('Invalid growth quality mode')
        self.mode = mode
        self._observed_this_session = False
        self._state = {'admitted_events': 0, 'deferred_events': 0,
                       'last_growth_deferred': False, 'consecutive_deferred': 0}
        if isinstance(state, dict):
            for key in ('admitted_events', 'deferred_events', 'consecutive_deferred'):
                if type(state.get(key)) is int and state[key] >= 0:
                    self._state[key] = state[key]
            if type(state.get('last_growth_deferred')) is bool:
                self._state['last_growth_deferred'] = state['last_growth_deferred']

    def observe(self, allowed):
        self._observed_this_session = True
        self._state['admitted_events' if allowed else 'deferred_events'] += 1
        self._state['last_growth_deferred'] = not allowed
        self._state['consecutive_deferred'] = 0 if allowed else self._state['consecutive_deferred'] + 1

    def should_pause(self):
        return (self.mode == 'pause' and self._observed_this_session
                and self._state['consecutive_deferred'] >= 3)

    def snapshot(self):
        return dict(self._state)


def observed_memory(torch):
    from native.worker.training_resource_policy import MemoryObservation
    free, total = torch.cuda.mem_get_info()
    reusable = 0
    try:
        stats = torch.cuda.memory_stats()
        values = [stats[key] for key in ('reserved_bytes.all.current',
                  'active_bytes.all.current', 'inactive_split_bytes.all.current')]
        if all(type(value) is int and value >= 0 for value in values):
            reserved, active, fragments = values
            # Active includes pending stream use. Inactive split fragments are
            # deliberately excluded. The admission policy credits only HALF
            # of this fully inactive storage, never all reserved CUDA memory.
            reusable = min(max(0, reserved - active - fragments), total - free)
    except (KeyError, RuntimeError, AttributeError):
        pass
    return MemoryObservation(int(free), int(total), int(reusable))


def validate_resume_renderer(recorded, effective):
    if recorded is not None and recorded != effective:
        raise ValueError('resume_renderer_environment_changed')


def performance_configuration(control, checkpoint_performance=None):
    supplied = control.get('performance', checkpoint_performance)
    defaults = {'version': 1, 'enabled': not bool(control.get('resume')),
                'usage_fraction': .9, 'minimum_spare_mib': 512,
                'gpu_slots': 2, 'pinned_mib': 256, 'gpu_mib': 512,
                'profile_interval': 30, 'engine': 'stock', 'growth_mode': 'pause', 'loss_mode': 'auto',
                'camera_prefetch': True, 'capacity_storage': True, 'surfel_auxiliary': True}
    if supplied is not None:
        if not isinstance(supplied, dict):
            raise ValueError('Invalid native performance policy')
        defaults.update({key: supplied[key] for key in defaults if key in supplied})
    if (type(defaults['version']) is not int or defaults['version'] != 1 or
            type(defaults['enabled']) is not bool or defaults['engine'] not in ('stock', 'fastergs') or
            defaults['growth_mode'] not in ('pause', 'defer')):
        raise ValueError('Invalid native performance policy')
    if defaults['loss_mode'] not in ('auto', 'stock'):
        raise ValueError('Invalid photometric runtime policy')
    for key in ('camera_prefetch', 'capacity_storage', 'surfel_auxiliary'):
        if type(defaults[key]) is not bool:
            raise ValueError('Invalid exact runtime feature: ' + key)
    fraction = defaults['usage_fraction']
    if (isinstance(fraction, bool) or not isinstance(fraction, (int, float))
            or not math.isfinite(fraction) or not .5 <= fraction <= .95):
        raise ValueError('Invalid native memory fraction')
    limits = {'minimum_spare_mib': (256, 8192), 'gpu_slots': (1, 2),
              'pinned_mib': (16, 1024), 'gpu_mib': (16, 2048), 'profile_interval': (1, 1000)}
    for key, (low, high) in limits.items():
        if type(defaults[key]) is not int or not low <= defaults[key] <= high:
            raise ValueError('Invalid native performance limit: ' + key)
    return defaults


def configure_image_environment(configuration, environment=None):
    environment = os.environ if environment is None else environment
    if configuration['enabled']:
        environment['GSW_TRAINING_IMAGE_PIPELINE'] = 'bounded'
        for suffix, key in (('GPU_SLOTS', 'gpu_slots'), ('PINNED_MIB', 'pinned_mib'), ('GPU_MIB', 'gpu_mib')):
            environment['GSW_TRAINING_IMAGE_' + suffix] = str(configuration[key])
    else:
        environment.pop('GSW_TRAINING_IMAGE_PIPELINE', None)


def configure_native_images(control):
    checkpoint_policy = None
    if control.get('resume'):
        from native.worker.training_checkpoint import read_manifest
        manifest, _ = read_manifest(control['output'], verify=False)
        checkpoint_policy = manifest.get('performance')
    configuration = performance_configuration(control, checkpoint_policy)
    configure_image_environment(configuration)
    return configuration


class NativeTrainingRuntime:
    def __init__(self, control, torch, emit, backend):
        from native.worker.training_step_profile import IterationProfiler
        from native.worker.training_resource_policy import GrowthAdmission
        from native.worker.training_image_pipeline import TrainingImagePipeline, ImagePipelineConfig
        from native.worker.training_loss import PhotometricLoss
        self.configuration = configure_native_images(control)
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
        self.growth_quality = GrowthQuality(self.configuration['growth_mode'])
        self.profiler = IterationProfiler(backend, self.configuration['profile_interval'],
            torch_module=torch, emit=emit, metadata=lambda: {
                'renderer_engine': self.engine,
                'loss_engine': self.loss.status()['effective'] if self.loss else 'stock'})
        self.admission = (GrowthAdmission(lambda: observed_memory(torch),
            self.configuration['usage_fraction'], self.configuration['minimum_spare_mib'] * 1024 ** 2)
            if self.configuration['enabled'] else None)
        self.images = (TrainingImagePipeline(torch, config=ImagePipelineConfig.from_environment())
                       if self.configuration['enabled'] else None)
        module_path = os.environ.get('GSW_TRAINING_LOSS_MODULE_PATH')
        if module_path and module_path not in sys.path:
            sys.path.append(module_path)  # Never shadow the configured Torch runtime.
        self.loss = None
        self.loss_execution = None
        self._loss_history_state = _FRESH_LOSS_HISTORY
        self._loss_class = PhotometricLoss
        emit('[gsw-training-runtime]', dict(self.configuration, backend=backend))

    def photometric_loss(self, l1, ssim):
        self.loss = self._loss_class(self.torch, l1, ssim,
            mode='fused_l1_ssim' if self.configuration['enabled'] and self.configuration['loss_mode'] == 'auto' else 'stock')
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
            'bounded': self.configuration['enabled']})

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
        execution['features'] = {
            'capacity_storage': dict(self.storage_status,
                requested=bool(self.configuration['enabled'] and self.configuration['capacity_storage'])),
            'surfel_auxiliary': dict(self.surfel_status,
                requested=bool(self.configuration['enabled'] and self.configuration['surfel_auxiliary'])),
            'camera_prefetch': {'requested': bool(self.configuration['enabled'] and self.configuration['camera_prefetch']),
                                'opportunities': self.lookahead_opportunities}}
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
        if self.configuration['enabled'] and self.configuration['capacity_storage']:
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

    def image(self, camera, cameras=None, pending_indices=None):
        from native.worker.training_image_pipeline import ReadyImage
        if self.images:
            next_camera = None
            if self.configuration['camera_prefetch'] and cameras is not None and pending_indices is not None:
                from native.worker.training_camera_lookahead import predict_next_camera
                next_camera = predict_next_camera(cameras, pending_indices)
                if next_camera is not None:
                    self.lookahead_opportunities += 1
            return self.images.prepare(camera, next_camera=next_camera)
        def cuda(tensor):
            return tensor.cuda() if tensor is not None else None
        return ReadyImage(cuda(camera.original_image), cuda(getattr(camera, 'alpha_mask', None)),
                          cuda(getattr(camera, 'invdepthmap', None)), cuda(getattr(camera, 'depth_mask', None)))

    def admit(self, model, threshold, extent, iteration):
        from native.worker.training_resource_policy import admit_densification
        allowed = admit_densification(self.torch, model, self.backend, threshold, extent,
                                      self.admission, self.emit, iteration)
        if self.admission is not None:
            self.growth_quality.observe(allowed)
        return allowed

    def pause_for_resources(self):
        return self.admission is not None and self.growth_quality.should_pause()

    def capture(self, state):
        state['performance'] = dict(self.configuration)
        state['renderer_engine'] = self.engine
        state['growth_quality'] = self.growth_quality.snapshot()
        if self.quality_profile:
            self._refresh_profile()
            self._save_profile()
            state['quality_profile'] = json.loads(json.dumps(self.quality_profile, allow_nan=False))

    def restore_quality(self, state):
        self.growth_quality = GrowthQuality(self.configuration['growth_mode'], state.get('growth_quality'))
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
