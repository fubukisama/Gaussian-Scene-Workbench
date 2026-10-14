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
                'profile_interval': 30, 'engine': 'stock', 'growth_mode': 'pause', 'loss_mode': 'auto'}
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
        self._loss_class = PhotometricLoss
        emit('[gsw-training-runtime]', dict(self.configuration, backend=backend))

    def photometric_loss(self, l1, ssim):
        self.loss = self._loss_class(self.torch, l1, ssim,
            mode='fused_l1_ssim' if self.configuration['enabled'] and self.configuration['loss_mode'] == 'auto' else 'stock')
        self.emit('[gsw-training-loss-engine]', self.loss.status())
        return self.loss

    def report_images(self, cameras):
        from native.worker.training_image_pipeline import total_host_bytes
        self.emit('[gsw-training-image-cache]', {
            'version': 1, 'backend': self.backend, 'cameras': len(cameras),
            'host_cache_bytes': total_host_bytes(cameras),
            'bounded': self.configuration['enabled']})

    def image(self, camera):
        from native.worker.training_image_pipeline import ReadyImage
        if self.images:
            return self.images.prepare(camera)
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

    def restore_quality(self, state):
        self.growth_quality = GrowthQuality(self.configuration['growth_mode'], state.get('growth_quality'))

    def close(self):
        self.emit('[gsw-training-growth-quality]', dict(version=1, backend=self.backend, **self.growth_quality.snapshot()))
        if self.images:
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
