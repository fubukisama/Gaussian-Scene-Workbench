# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Actual optional quality mechanisms behind the shared lifecycle interface.

No default instance is made for legacy jobs. Availability, engineering evidence
and quality acceptance are separate. Unsupported combinations fail in the shared
resolver; missing binary proof never falls back to Standard.
"""
import importlib
import hashlib
import copy
import os
from pathlib import Path
import sys


def quality_kernel_root(explicit=None):
    """Select a separate quality component without replacing the stock kernel."""
    if explicit:
        return str(Path(explicit).resolve())
    configured = os.environ.get('GSW_QUALITY_KERNEL_ROOT')
    if configured:
        return str(Path(configured).resolve())
    prefix = Path(sys.executable).resolve().parent
    if prefix.name.lower() == 'scripts':
        prefix = prefix.parent
    return str(prefix.parent.parent / 'kernels' / 'quality3d-v1')


def bind_quality_controller(torch, model, scene, pipeline, lifecycle, native_runtime, backend, emit=None):
    if lifecycle is None:
        return None
    value = lifecycle.plan['effective']
    advanced = (value['densification']['strategy'] != 'standard'
                or value['filter']['mode'] == 'full_mip' or bool(value.get('supervision')))
    if not advanced:
        return None
    if native_runtime.configuration['engine'] != 'stock':
        raise ValueError('quality_renderer_combination_unvalidated')
    if value['densification']['strategy'] == 'mcmc' and native_runtime.configuration['capacity_storage']:
        raise ValueError('quality_mcmc_capacity_storage_adapter_required')
    controller = QualityTrainingController(torch, model, scene, pipeline, lifecycle.plan, backend, emit)
    lifecycle.bind_controller(controller)
    return controller


def masked_photometric(torch, image, target, weights, lambda_dssim):
    """Foreground L1 plus complete-valid-window SSIM, never global masked-zero SSIM."""
    weights = torch.as_tensor(weights.copy(), dtype=image.dtype, device=image.device)
    target = target.to(image.dtype) / (255. if target.dtype == torch.uint8 else 1.)
    valid = weights > 0
    total = weights.sum()
    if weights.shape != image.shape[-2:] or not bool(torch.isfinite(total)) or total <= 0:
        raise ValueError('quality_mask_photometric_invalid')
    image = torch.where(valid[None], image, torch.zeros_like(image))
    target = torch.where(valid[None], target, torch.zeros_like(target))
    if not bool(torch.isfinite(image).all() and torch.isfinite(target).all()):
        raise ValueError('quality_mask_prediction_nonfinite')
    l1 = (torch.abs(image - target).mean(0)[valid] * weights[valid]).sum() / total
    size = min(11, image.shape[-2], image.shape[-1])
    size -= 1 - size % 2
    if size < 3 or lambda_dssim == 0:
        return l1, l1, {'ssim_windows': 0, 'reason': 'foreground_l1_only'}
    conv = torch.nn.functional.conv2d
    coordinate = torch.arange(size, dtype=image.dtype, device=image.device) - size // 2
    one = torch.exp(-coordinate.square() / (2 * 1.5 ** 2))
    one = one / one.sum()
    window = (one[:, None] * one[None, :])[None, None]
    complete = conv(valid[None, None].to(image.dtype), torch.ones_like(window)) >= size * size
    window_weight = conv(weights[None, None], window)
    window_weight = torch.where(complete, window_weight, torch.zeros_like(window_weight))
    denominator = window_weight.sum()
    if denominator <= 0:
        return l1, l1, {'ssim_windows': 0, 'reason': 'no_complete_foreground_windows_l1_only'}
    kernel = window.expand(3, 1, size, size).contiguous()
    x, y = image[None], target[None]
    mx, my = conv(x, kernel, groups=3), conv(y, kernel, groups=3)
    vx = conv(x * x, kernel, groups=3) - mx.square()
    vy = conv(y * y, kernel, groups=3) - my.square()
    vxy = conv(x * y, kernel, groups=3) - mx * my
    score = ((2 * mx * my + .01 ** 2) * (2 * vxy + .03 ** 2)
             / ((mx.square() + my.square() + .01 ** 2) * (vx + vy + .03 ** 2)))
    ssim = (score * window_weight).sum() / (3 * denominator)
    return (1 - lambda_dssim) * l1 + lambda_dssim * (1 - ssim), l1, {
        'ssim_windows': int(complete.sum().item()), 'reason': 'complete_foreground_windows'}


class QualityTrainingController:
    VERSION = 1

    def __init__(self, torch, model, scene, pipeline, plan, backend, emit=None, kernel_root=None):
        self.torch, self.model, self.scene, self.plan, self.backend = torch, model, scene, plan, backend
        self.emit, self.providers, self.kernel = emit, {}, None
        effective = plan['effective']
        self.strategy = effective['densification']['strategy']
        self.stats, self.mip, self.renderer, self.mcmc = None, None, None, None
        self.is_mcmc = self.strategy == 'mcmc'
        self.last_loss_report = None
        self.replaces_photometric = 'mask' in effective.get('supervision', {})
        self.source_identity = {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
            for name in ('quality_training_runtime.py', 'quality_render_adapter.py', 'quality_densification.py',
                'quality_mip.py', 'quality_supervision.py', 'surfel_depth_adapter.py', 'training_quality_plan.py',
                'quality_export.py', 'quality_mcmc.py', 'quality_mcmc_model.py')}
        supervision = effective.get('supervision', {})
        if supervision:
            from native.worker.quality_supervision import load_supervision
            contract = getattr(model, '_gsw_camera_contract', None)
            if contract is None:
                raise ValueError('quality_supervision_locked_camera_contract_required')
            for kind, settings in supervision.items():
                kwargs = {key: settings[key] for key in ('allow_oracle', 'unit_to_training_scale', 'scale_evidence_identity')
                          if key in settings}
                self.providers[kind] = load_supervision(settings['manifest'], contract, kind=kind, **kwargs)
            for camera in scene.getTrainCameras():
                # Hold providers and stable IDs, not resident maps for all views.
                camera.quality_supervision = {'view_id': camera.view_id, 'providers': self.providers}
        if effective['filter']['mode'] == 'full_mip':
            from native.worker.quality_mip import MipState
            settings = effective['filter']
            self.mip = MipState(torch, settings['pixel_variance'], settings['margin'])
            self._update_mip(0)
        if self.strategy in ('abs_gs', 'pixel_gs'):
            from native.worker.quality_densification import DensificationStatistics
            self.stats = DensificationStatistics(torch, self.strategy, len(model.get_xyz), model.get_xyz.device,
                scene.cameras_extent, effective['densification']['parameters'].get('depth_factor', .37))
        requires_kernel = backend == '3dgs' and (self.stats is not None or self.mip is not None or bool(supervision))
        if requires_kernel:
            from native.worker.quality_rasterizer_runtime import configure_quality_rasterizer
            repository = Path(__file__).resolve().parents[2]
            root = quality_kernel_root(kernel_root)
            self.kernel = configure_quality_rasterizer(repository, root)
            module = importlib.import_module('diff_gaussian_rasterization_quality')
            from native.worker.quality_render_adapter import QualityRenderAdapter
            self.renderer = QualityRenderAdapter(torch, module, absgrad=self.strategy == 'abs_gs',
                contributor_pixels=self.strategy == 'pixel_gs', alpha=bool(supervision), mip=self.mip)
        self._bind_buffers()
        if emit:
            emit('[gsw-quality-plan]', {'plan': plan, 'quality_accepted': False,
                'kernel_build_sha256': self.kernel['buildSha256'] if self.kernel else None,
                'supervision': {kind: provider.report for kind, provider in self.providers.items()}})

    def _bind_buffers(self):
        buffers = []
        if self.stats is not None:
            buffers.extend((self.stats.clone_sum, self.stats.split_sum, self.stats.denominator))
        if self.mip is not None:
            buffers.append(self.mip.widths)
        self.model._gsw_quality_buffers = buffers

    def initialize_optimizer(self):
        if self.is_mcmc:
            from native.worker.quality_mcmc_model import GSWMCMCBridge
            self.mcmc = GSWMCMCBridge(self.model, seed=getattr(self.model, '_gsw_training_seed', 0),
                                     **self.plan['effective']['densification']['parameters'])

    def _update_mip(self, iteration):
        cameras = []
        for camera in self.scene.getTrainCameras():
            if not hasattr(camera, 'K') or getattr(camera, 'split', None) != 'train':
                raise ValueError('quality_mip_locked_camera_contract_required')
            cameras.append(dict(K=camera.K, w2c=camera.world_view_transform.detach().T,
                width=int(camera.image_width), height=int(camera.image_height),
                znear=camera.znear, split='train'))
        return self.mip.update(self.model.get_xyz, cameras, iteration)

    def before_render(self, iteration):
        if self.mip is not None and iteration % self.plan['effective']['filter']['update_interval'] == 0:
            self._update_mip(iteration)
            self._bind_buffers()

    def loss(self, camera, render_pkg, target, base_loss, base_l1, lambda_dssim):
        from native.worker.quality_supervision import supervision_losses
        image, torch = render_pkg['render'], self.torch
        loss, l1, report = base_loss, base_l1, {}
        selected = self.plan['effective'].get('supervision', {})
        if 'mask' in self.providers:
            payload = camera.quality_supervision['providers']['mask'].for_view(camera.quality_supervision['view_id'])
            settings = selected['mask']
            loss, l1, report = masked_photometric(torch, image, target, payload['foreground_weights'], lambda_dssim)
            loss = loss * settings['foreground_weight']
            if settings['background_alpha_weight']:
                alpha = render_pkg.get('alpha') if self.backend == '3dgs' else render_pkg.get('rend_alpha')
                negative = supervision_losses(image, target, payload, rendered_alpha=alpha,
                    mask_loss_weight=0, background_alpha_weight=settings['background_alpha_weight'])
                loss = loss + negative['total']
                report['background_alpha'] = negative['counts']
        if 'depth' in self.providers:
            payload = camera.quality_supervision['providers']['depth'].for_view(camera.quality_supervision['view_id'])
            if self.backend == '3dgs':
                predicted = dict(definition='inverse_z_moment', values=render_pkg['depth'], alpha=render_pkg['alpha'])
            else:
                # The official surfel expected Z is accumulated Z / alpha;
                # request this explicitly, never treat its hybrid surface map
                # (median+expected) as a matching inverse-depth estimator.
                alpha, moment = render_pkg.get('rend_alpha'), render_pkg.get('rend_depth_moment')
                if alpha is None or moment is None:
                    raise ValueError('quality_surfel_depth_output_required')
                valid = self.torch.isfinite(moment) & (moment > 0)
                inverse = self.torch.where(valid, alpha / self.torch.where(valid, moment, self.torch.ones_like(moment)),
                                           self.torch.zeros_like(moment))
                predicted = dict(definition='inverse_z', values=inverse)
            depth = supervision_losses(image, target, payload, predicted_depth=predicted,
                mask_loss_weight=0, depth_loss_weight=selected['depth']['weight'])
            loss = loss + depth['total']
            report['depth'] = depth['counts']
        if self.mcmc is not None:
            loss = loss + self.mcmc.regularization_loss()
        self.last_loss_report = report
        return loss, l1

    def after_backward(self, render_pkg, camera):
        if self.stats is None:
            return
        kwargs = {}
        if self.strategy == 'abs_gs':
            kwargs['absolute_screen_gradient'] = render_pkg['absolute_screen_gradient']
        else:
            world_to_camera = camera.world_view_transform
            z = (self.model.get_xyz.detach() @ world_to_camera[:3, 2] + world_to_camera[3, 2])
            kwargs.update(contributor_pixels=render_pkg['contributor_pixels'], camera_z=z)
        self.stats.accumulate(render_pkg['viewspace_points'].grad, render_pkg['visibility_filter'], **kwargs)

    def density_arguments(self):
        if self.stats is None:
            return {}
        clone, split = self.stats.gradients()
        result = dict(clone_grads=clone, split_grads=split)
        if self.strategy == 'abs_gs':
            result['split_threshold'] = self.plan['effective']['densification']['parameters']['split_threshold']
        return result

    def topology_changed(self, iteration):
        if self.stats is not None:
            self.stats.reset(len(self.model.get_xyz))
        if self.mip is not None:
            self._update_mip(iteration)
        self._bind_buffers()

    def reset_opacity(self):
        if self.is_mcmc:
            raise ValueError('quality_mcmc_standard_reset_forbidden')
        if self.mip is None:
            self.model.reset_opacity()
            return
        torch = self.torch
        with torch.no_grad():
            active_scales, unused = self.mip.active(self.model.get_scaling, self.model.get_opacity)
            log_coefficient = (self.model.get_scaling.log() - active_scales.log()).sum(1, keepdim=True)
            # Cap raw alpha at .01 / coefficient in log space. The old
            # min(raw_alpha * coefficient, .01) / coefficient became 0/0
            # when the valid determinant compensation underflowed in float32.
            # Preserve finite saturated logits too; sigmoid->logit round trips
            # would otherwise turn a finite +/-1000 into an infinity.
            log_ceiling = torch.full_like(log_coefficient, .01).log() - log_coefficient
            bounded = log_ceiling.clamp_max(-torch.finfo(log_ceiling.dtype).eps)
            maximum_logits = bounded - (-torch.expm1(bounded)).log()
            logits = torch.where(log_ceiling >= 0, self.model._opacity,
                                 torch.minimum(self.model._opacity, maximum_logits))
            self.model._opacity = self.model.replace_tensor_to_optimizer(logits, 'opacity')['opacity']

    def preview_model(self):
        if self.mip is None:
            return self.model
        # A lightweight read-only display facade, not optimizer state or a new
        # model object. Conventional previews receive the 3D filter once.
        scales, opacity = self.mip.active(self.model.get_scaling, self.model.get_opacity)
        value = copy.copy(self.model)
        value._scaling = scales.log()
        log_coefficient = (self.model.get_scaling.log() - scales.log()).sum(1, keepdim=True)
        log_alpha = self.torch.nn.functional.logsigmoid(self.model._opacity.double()) + log_coefficient.double()
        logits = log_alpha - (-self.torch.expm1(log_alpha)).log()
        value._opacity = self.torch.where(log_coefficient == 0, self.model._opacity, logits.to(self.model._opacity.dtype))
        value._gsw_quality_display = {'schema': 'gsw.quality_model/1', 'filter': 'full_mip',
            'representation': 'fused', 'filter_3d_applied': True, 'plan_hash': self.plan['plan_hash'],
            'filter_2d': {'variance': .3, 'opacity_compensation': True},
            'quality_accepted': False, 'raw_source_kind': 'preview_snapshot_rows_float32'}
        value._gsw_raw_preview_model = self.model
        return value

    def export_saved_model(self, iteration):
        if self.mip is None:
            return None
        from native.worker.quality_export import export_mip_checkpoint
        return export_mip_checkpoint(self, iteration)

    def after_optimizer_step(self, iteration, optimization, native_runtime):
        if self.mcmc is None:
            return
        if iteration >= optimization.iterations:
            # Upstream intentionally does not optimize the last evaluation
            # iteration. These gradients are unused, not an unrecorded Adam step.
            self.model.optimizer.zero_grad(set_to_none=True)
            return
        refine = (optimization.densify_from_iter < iteration < optimization.densify_until_iter
                  and iteration % optimization.densification_interval == 0)
        count = len(self.model.get_xyz)
        parameters = self.plan['effective']['densification']['parameters']
        growth = min(int(count * parameters['growth_rate']), parameters['cap'] - count)
        if refine:
            allowed = native_runtime.admit_mcmc_growth(self.model, growth, iteration)
            if not allowed:
                # Continue a complete stochastic step without topology. The
                # existing quality-first policy observes these denied events
                # and pauses at its normal iteration boundary, never prunes.
                refine = False
        position_lr = next(group['lr'] for group in self.model.optimizer.param_groups if group['name'] == 'xyz')
        event = self.mcmc.after_optimizer_step(position_lr=position_lr, refine=refine, grow_count=growth if refine else None)
        if self.emit and (refine or iteration == 1):
            self.emit('[gsw-quality-mcmc]', dict(iteration=iteration, rows=len(self.model.get_xyz),
                step=event['step'], noise=event['noise'], growth=event['growth'],
                relocated=(event['relocation'] or {}).get('relocated', 0), quality_accepted=False))

    def capture(self):
        return {'version': self.VERSION, 'plan_hash': self.plan['plan_hash'],
                'source_identity': self.source_identity,
                'kernel_build_sha256': self.kernel['buildSha256'] if self.kernel else None,
                'supervision': {kind: provider.identity for kind, provider in self.providers.items()},
                'density': self.stats.state_dict() if self.stats else None,
                'mip': self.mip.state_dict() if self.mip else None,
                'mcmc': self.mcmc.native_checkpoint_state() if self.mcmc else None}

    def restore(self, state):
        if (state.get('version') != self.VERSION or state.get('plan_hash') != self.plan['plan_hash']
                or state.get('source_identity') != self.source_identity
                or state.get('kernel_build_sha256') != (self.kernel['buildSha256'] if self.kernel else None)
                or state.get('supervision') != {kind: provider.identity for kind, provider in self.providers.items()}):
            raise ValueError('quality_controller_checkpoint_mismatch')
        if self.stats:
            self.stats.load_state_dict(state['density'], len(self.model.get_xyz))
        if self.mip:
            self.mip.load_state_dict(state['mip'], self.model.get_xyz)
        if self.mcmc:
            self.mcmc.restore_native_checkpoint(state['mcmc'])
        self._bind_buffers()
