# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Independent implementation of published 3DGS-MCMC mathematics, not author code.

No Graphdeco implementation is copied or relicensed. Equations: arXiv
2404.09591v3 (7-10, 18); behavioral reference: author repository commit
7b4fc9f76a1c7b775f69603cb96e70f80c7e6d13. CPU numerical validation is not
renderer, CUDA, training-quality or complete-worker-checkpoint acceptance.
"""
import math
import copy
import hashlib
import json

import torch


def relocation_parameters(opacity, scales, multiplicity, max_multiplicity=50):
    """Eq.9/18 using a collapsed binomial series, evaluated in float64.

    Multiplicity includes the live parent. The finite numerical domain is
    explicit; unlike the author's CUDA helper, excess multiplicity is rejected,
    never silently clamped. Returned scales are standard deviations, not
    covariances. Peak opacity and the 1D slice integral are preserved, not the
    complete image or posterior probability exactly.
    """
    if (not isinstance(opacity, torch.Tensor) or not isinstance(scales, torch.Tensor) or
            not isinstance(multiplicity, torch.Tensor) or opacity.ndim != 1 or
            tuple(scales.shape) != (len(opacity), 3) or multiplicity.shape != opacity.shape or
            not opacity.is_floating_point() or scales.dtype != opacity.dtype or
            scales.device != opacity.device or multiplicity.device != opacity.device or
            multiplicity.dtype not in (torch.int32, torch.int64)):
        raise ValueError('quality_mcmc_relocation_shape_invalid')
    if type(max_multiplicity) is not int or not 1 <= max_multiplicity <= 256:
        raise ValueError('quality_mcmc_multiplicity_domain_invalid')
    if (not bool(torch.isfinite(opacity).all()) or not bool(torch.isfinite(scales).all()) or
            bool(((opacity <= 0) | (opacity >= 1)).any()) or bool((scales <= 0).any()) or
            bool(((multiplicity < 1) | (multiplicity > max_multiplicity)).any())):
        raise ValueError('quality_mcmc_relocation_domain_invalid')
    if opacity.numel() == 0:
        return opacity.clone(), scales.clone()
    alpha, copies = opacity.to(torch.float64), multiplicity.to(torch.float64)
    divided = -torch.expm1(torch.log1p(-alpha) / copies)
    # sum_{i=j..N} C(i-1,j-1) = C(N,j); recurrence avoids factorial tables.
    term, integral = torch.ones_like(divided), torch.zeros_like(divided)
    for order in range(1, int(multiplicity.max()) + 1):
        term = term * (copies-order+1).clamp_min(0) * divided / order
        integral = integral + (1. if order % 2 else -1.) * term / math.sqrt(order)
    ratio = alpha / integral
    adjusted = scales.to(torch.float64) * ratio[:, None]
    divided = torch.where(multiplicity == 1, alpha, divided)
    adjusted = torch.where((multiplicity == 1)[:, None], scales.to(torch.float64), adjusted)
    if (not bool(torch.isfinite(adjusted).all()) or bool((adjusted <= 0).any()) or
            not bool(torch.isfinite(divided).all()) or bool(((divided <= 0) | (divided >= 1)).any())):
        raise ValueError('quality_mcmc_relocation_numerically_invalid')
    return divided.to(opacity.dtype), adjusted.to(scales.dtype)


def covariance_noise(log_scales, quaternions, opacity_logits, *, position_lr, noise_lr,
                     generator, opacity_threshold=.005, sharpness=100.):
    """Author-intended opacity gate times Sigma @ N(0,I), wxyz quaternions.

    This returns only the stochastic term. MCMCAdapter.step separately performs
    the actual Adam drift; calling this function alone is not a full MCMC step.
    """
    if (not isinstance(log_scales, torch.Tensor) or log_scales.ndim != 2 or log_scales.shape[1] != 3 or
            not log_scales.is_floating_point() or quaternions.shape != (len(log_scales), 4) or
            opacity_logits.shape != (len(log_scales), 1) or
            any(t.dtype != log_scales.dtype or t.device != log_scales.device for t in (quaternions, opacity_logits))):
        raise ValueError('quality_mcmc_noise_shape_invalid')
    if (any(not math.isfinite(float(x)) or x < 0 for x in (position_lr, noise_lr)) or
            not math.isfinite(float(sharpness)) or sharpness <= 0 or not 0 < opacity_threshold < 1):
        raise ValueError('quality_mcmc_noise_config_invalid')
    if (not isinstance(generator, torch.Generator) or generator.device != log_scales.device or
            any(not bool(torch.isfinite(t).all()) for t in (log_scales, quaternions, opacity_logits)) or
            bool((quaternions.norm(dim=1) <= 0).any())):
        raise ValueError('quality_mcmc_noise_domain_invalid')
    if not position_lr or not noise_lr or not len(log_scales):
        return torch.zeros_like(log_scales)
    scales_squared = (2*log_scales).exp()
    if not bool(torch.isfinite(scales_squared).all()) or bool((scales_squared <= 0).any()):
        raise ValueError('quality_mcmc_covariance_invalid')
    w, x, y, z = (quaternions / quaternions.norm(dim=1, keepdim=True)).unbind(1)
    rotation = torch.stack((1-2*(y*y+z*z), 2*(x*y-w*z), 2*(x*z+w*y),
                            2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x),
                            2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y)), dim=1).reshape(-1, 3, 3)
    covariance = rotation @ torch.diag_embed(scales_squared) @ rotation.transpose(1, 2)
    normal = torch.randn(log_scales.shape, dtype=log_scales.dtype, device=log_scales.device, generator=generator)
    gate = torch.sigmoid(sharpness*(opacity_threshold-opacity_logits.sigmoid()))
    return (covariance @ normal[:, :, None]).squeeze(2) * gate * (position_lr*noise_lr)


class MCMCAdapter:
    """Explicit Gaussian tensor pool and real named-group Adam adapter.

    Required keys: means[N,3], log_scales[N,3], quaternions[N,4,wxyz],
    opacity_logits[N,1]. Additional floating per-Gaussian attributes are copied
    during relocation/growth. Explicit canonical-key -> optimizer group names
    are supported without renaming the optimizer's existing groups.
    Callers must use this mapping after growth; old Parameter references expire.
    CUDA/renderer equivalence is unverified; no production loop selects this.
    """
    VERSION = 2
    METHOD = 'gsw-independent-paper-mcmc/1'
    REQUIRED = {'means': 3, 'log_scales': 3, 'quaternions': 4, 'opacity_logits': 1}
    MOMENTS = ('exp_avg', 'exp_avg_sq', 'max_exp_avg_sq')

    def __init__(self, parameters, optimizer, *, seed, cap, noise_lr=5e5,
                 dead_threshold=.005, noise_threshold=.005, noise_sharpness=100.,
                 growth_rate=.05, opacity_reg=.01, scale_reg=.01,
                 regularization_reduction='mean', max_multiplicity=50, optimizer_group_names=None):
        if (type(seed) is not int or not 0 <= seed < 2**32 or type(cap) is not int or cap <= 0 or
                any(not math.isfinite(float(x)) or x < 0 for x in (noise_lr, opacity_reg, scale_reg)) or
                not 0 < dead_threshold < 1 or not 0 < noise_threshold < 1 or
                not math.isfinite(float(noise_sharpness)) or noise_sharpness <= 0 or
                not 0 < growth_rate <= 1 or regularization_reduction not in ('mean', 'sum') or
                type(max_multiplicity) is not int or not 1 <= max_multiplicity <= 256):
            raise ValueError('quality_mcmc_config_invalid')
        self.parameters, self.optimizer = parameters, optimizer
        if type(parameters) is not dict:
            raise ValueError('quality_mcmc_parameters_or_optimizer_invalid')
        mapping = {name: name for name in parameters} if optimizer_group_names is None else copy.deepcopy(optimizer_group_names)
        if (type(mapping) is not dict or set(mapping) != set(parameters) or
                any(type(name) is not str or not name for name in mapping.values()) or
                len(set(mapping.values())) != len(mapping)):
            raise ValueError('quality_mcmc_optimizer_mapping_invalid')
        self._optimizer_group_names = mapping
        self._group_name_to_parameter = {group: parameter for parameter, group in mapping.items()}
        self.config = dict(seed=seed, cap=cap, noise_lr=noise_lr, dead_threshold=dead_threshold,
            noise_threshold=noise_threshold, noise_sharpness=noise_sharpness, growth_rate=growth_rate,
            opacity_reg=opacity_reg, scale_reg=scale_reg, regularization_reduction=regularization_reduction,
            max_multiplicity=max_multiplicity, optimizer_group_names=copy.deepcopy(mapping))
        self._validate()
        self.noise_generator = self._generator('position_noise')
        self.relocation_generator = self._generator('relocation_sampling')
        self.gaussian_ids = torch.arange(len(parameters['means']), device=self.device, dtype=torch.int64)
        self.next_gaussian_id = len(self.gaussian_ids)
        self.steps, self.event_index, self.last_event, self.last_position_lr = 0, 0, None, None

    @property
    def device(self):
        return self.parameters['means'].device

    @property
    def optimizer_group_names(self):
        return dict(self._optimizer_group_names)

    def _parameter_name(self, group):
        return self._group_name_to_parameter.get(group.get('name'))

    def _generator(self, domain):
        identity = '{}:{}:{}'.format(self.METHOD, self.config['seed'], domain).encode('ascii')
        seed = int.from_bytes(hashlib.sha256(identity).digest()[:8], 'little') % (2**63)
        return torch.Generator(device=self.device).manual_seed(seed)

    def _validate(self):
        parameters, optimizer = self.parameters, self.optimizer
        if (type(parameters) is not dict or not self.REQUIRED.keys() <= parameters.keys() or
                not isinstance(optimizer, torch.optim.Adam)):
            raise ValueError('quality_mcmc_parameters_or_optimizer_invalid')
        means = parameters['means']
        if not isinstance(means, torch.nn.Parameter) or means.ndim != 2 or means.shape[1] != 3:
            raise ValueError('quality_mcmc_parameter_shape_invalid')
        count = len(means)
        if count > self.config['cap']:
            raise ValueError('quality_mcmc_cap_exceeded')
        for name, value in parameters.items():
            if (not isinstance(value, torch.nn.Parameter) or not value.is_leaf or not value.requires_grad or
                    value.ndim < 1 or len(value) != count or value.device != means.device or
                    value.dtype != means.dtype or value.dtype not in (torch.float32, torch.float64) or
                    not bool(torch.isfinite(value).all()) or
                    (name in self.REQUIRED and tuple(value.shape) != (count, self.REQUIRED[name]))):
                raise ValueError('quality_mcmc_parameter_shape_invalid: ' + name)
        scale = parameters['log_scales'].detach().exp()
        norm = parameters['quaternions'].detach().norm(dim=1)
        if (not bool(torch.isfinite(scale).all()) or bool((scale <= 0).any()) or
                not bool(torch.isfinite(norm).all()) or bool((norm <= 0).any()) or
                bool((parameters['opacity_logits'].detach().sigmoid() >= 1).any())):
            raise ValueError('quality_mcmc_parameter_domain_invalid')
        names = []
        for group in optimizer.param_groups:
            name = self._parameter_name(group)
            if (name not in parameters or len(group['params']) != 1 or
                    group['params'][0] is not parameters[name] or group.get('weight_decay', 0) != 0):
                raise ValueError('quality_mcmc_optimizer_groups_invalid')
            if (not math.isfinite(float(group['lr'])) or group['lr'] < 0 or
                    not math.isfinite(float(group['eps'])) or group['eps'] <= 0 or
                    len(group['betas']) != 2 or any(not 0 <= beta < 1 for beta in group['betas']) or
                    group.get('maximize', False)):
                raise ValueError('quality_mcmc_optimizer_groups_invalid')
            names.append(name)
            state = optimizer.state.get(parameters[name], {})
            if state:
                if (not {'step', 'exp_avg', 'exp_avg_sq'} <= state.keys() or
                        set(state) - {'step', 'exp_avg', 'exp_avg_sq', 'max_exp_avg_sq'}):
                    raise ValueError('quality_mcmc_optimizer_state_invalid')
                step = state['step']
                if ((isinstance(step, torch.Tensor) and step.numel() != 1) or
                        not math.isfinite(float(step)) or float(step) < 0 or float(step) != int(float(step))):
                    raise ValueError('quality_mcmc_optimizer_state_invalid')
                for key in self.MOMENTS:
                    if key in state and (not isinstance(state[key], torch.Tensor) or
                            state[key].shape != parameters[name].shape or state[key].dtype != parameters[name].dtype or
                            state[key].device != parameters[name].device or not bool(torch.isfinite(state[key]).all()) or
                            (key != 'exp_avg' and bool((state[key] < 0).any()))):
                        raise ValueError('quality_mcmc_optimizer_state_invalid')
        if len(names) != len(parameters) or set(names) != set(parameters):
            raise ValueError('quality_mcmc_optimizer_groups_invalid')

    def _clean_topology_seam(self):
        self._validate()
        if any(value.grad is not None for value in self.parameters.values()):
            raise ValueError('quality_mcmc_topology_requires_cleared_gradients')

    def _record(self, event):
        self.event_index += 1
        event = dict(event, event_index=self.event_index)
        self.last_event = copy.deepcopy(event)
        return event

    def regularization_loss(self):
        """Add to the photometric loss BEFORE backward; mean matches author code."""
        self._validate()
        opacity = self.parameters['opacity_logits'].sigmoid()
        scales = self.parameters['log_scales'].exp()
        if not len(opacity):
            return opacity.sum()*0 + scales.sum()*0
        reduce = (lambda x: x.mean()) if self.config['regularization_reduction'] == 'mean' else (lambda x: x.sum())
        return (self.config['opacity_reg']*reduce(opacity.abs()) +
                self.config['scale_reg']*reduce(scales.abs()))

    def _family(self, sampled):
        parents, inverse, numbers = torch.unique(sampled, sorted=True, return_inverse=True, return_counts=True)
        opacity = self.parameters['opacity_logits'].detach()[parents, 0].sigmoid()
        scale = self.parameters['log_scales'].detach()[parents].exp()
        opacity, scale = relocation_parameters(opacity, scale, numbers+1, self.config['max_multiplicity'])
        return parents, inverse, torch.logit(opacity)[:, None], scale.log()

    def _reset_parent_moments(self, parents):
        for parameter in self.parameters.values():
            state = self.optimizer.state.get(parameter, {})
            for key in self.MOMENTS:
                if key in state:
                    state[key][parents] = 0

    @torch.no_grad()
    def relocate(self):
        self._clean_topology_seam()
        opacity = self.parameters['opacity_logits'][:, 0].sigmoid()
        dead = torch.where(opacity <= self.config['dead_threshold'])[0]
        live = torch.where(opacity > self.config['dead_threshold'])[0]
        if not len(dead) or not len(live):
            return self._record({'kind': 'relocation', 'relocated': 0,
                                 'reason': 'no_dead' if not len(dead) else 'no_live'})
        rng_before = self.relocation_generator.get_state().clone()
        try:
            sampled = live[torch.multinomial(opacity[live], len(dead), replacement=True,
                                             generator=self.relocation_generator)]
            parents, inverse, logits, log_scales = self._family(sampled)
            replacement = {name: value.detach()[sampled].clone() for name, value in self.parameters.items()}
            replacement['opacity_logits'] = logits[inverse]
            replacement['log_scales'] = log_scales[inverse]
        except Exception:
            self.relocation_generator.set_state(rng_before)
            raise
        for name, value in self.parameters.items():
            value[dead] = replacement[name]
        self.parameters['opacity_logits'][parents] = logits
        self.parameters['log_scales'][parents] = log_scales
        # Paper policy: live targets reset; reused dead slots keep their moments.
        self._reset_parent_moments(parents)
        return self._record({'kind': 'relocation', 'relocated': len(dead),
                             'dead_ids': self.gaussian_ids[dead].cpu().tolist(),
                             'target_ids': self.gaussian_ids[sampled].cpu().tolist()})

    @torch.no_grad()
    def grow(self, count=None):
        """Budgeted family-corrected additions; new slots get zero Adam moments.

        Unlike the author's general-pool growth sampler, this independent
        adapter samples live parents only and skips an all-dead pool explicitly.
        """
        self._clean_topology_seam()
        current = len(self.parameters['means'])
        requested = int(current*self.config['growth_rate']) if count is None else count
        if type(requested) is not int or requested < 0:
            raise ValueError('quality_mcmc_growth_count_invalid')
        added = min(requested, self.config['cap']-current)
        opacity = self.parameters['opacity_logits'][:, 0].sigmoid()
        live = torch.where(opacity > self.config['dead_threshold'])[0]
        if not added or not len(live):
            return self._record({'kind': 'growth', 'requested': requested, 'added': 0,
                                 'reason': 'budget_or_schedule' if not added else 'no_live'})
        rng_before = self.relocation_generator.get_state().clone()
        try:
            sampled = live[torch.multinomial(opacity[live], added, replacement=True,
                                             generator=self.relocation_generator)]
            parents, inverse, logits, log_scales = self._family(sampled)
            new_parameters, new_states = {}, {}
            for name, old in self.parameters.items():
                kept, appended = old.detach().clone(), old.detach()[sampled].clone()
                if name in ('opacity_logits', 'log_scales'):
                    adjusted = logits if name == 'opacity_logits' else log_scales
                    kept[parents], appended = adjusted, adjusted[inverse]
                new_parameters[name] = torch.nn.Parameter(torch.cat((kept, appended), dim=0))
                state = copy.deepcopy(self.optimizer.state.get(old, {}))
                for key in self.MOMENTS:
                    if key in state:
                        state[key][parents] = 0
                        state[key] = torch.cat((state[key], torch.zeros_like(appended)), dim=0)
                new_states[name] = state
            new_ids = torch.arange(self.next_gaussian_id, self.next_gaussian_id+added,
                                   dtype=torch.int64, device=self.device)
            ids = torch.cat((self.gaussian_ids, new_ids))
        except Exception:
            self.relocation_generator.set_state(rng_before)
            raise
        target_ids = self.gaussian_ids[sampled].cpu().tolist()
        for group in self.optimizer.param_groups:
            name = self._parameter_name(group)
            self.optimizer.state.pop(self.parameters[name], None)
            group['params'][0] = new_parameters[name]
            self.optimizer.state[new_parameters[name]] = new_states[name]
        self.parameters.update(new_parameters)
        self.gaussian_ids, self.next_gaussian_id = ids, self.next_gaussian_id+added
        return self._record({'kind': 'growth', 'requested': requested, 'added': added,
                             'new_ids': new_ids.cpu().tolist(), 'target_ids': target_ids})

    @torch.no_grad()
    def inject_noise(self, position_lr):
        self._clean_topology_seam()
        state = self.noise_generator.get_state().clone()
        try:
            delta = covariance_noise(self.parameters['log_scales'], self.parameters['quaternions'],
                self.parameters['opacity_logits'], position_lr=position_lr, noise_lr=self.config['noise_lr'],
                generator=self.noise_generator, opacity_threshold=self.config['noise_threshold'],
                sharpness=self.config['noise_sharpness'])
            updated = self.parameters['means'] + delta
            if not bool(torch.isfinite(updated).all()):
                raise ValueError('quality_mcmc_noise_nonfinite_update')
        except Exception:
            self.noise_generator.set_state(state)
            raise
        self.parameters['means'].copy_(updated)
        maximum = float(delta.norm(dim=1).max()) if len(delta) else 0.
        return self._record({'kind': 'noise', 'position_lr': float(position_lr), 'maximum_norm': maximum})

    @torch.no_grad()
    def preflight_topology(self, *, refine, grow_count=None):
        """Preview both topology stages on opacity/scale clones and a cloned RNG.

        No pool, optimizer, RNG or event state changes. The preview must use
        the current POST-Adam state, immediately before topology; a pre-Adam
        preview cannot guarantee thresholds/weights after an optimizer update.
        This prevents a valid relocation from committing before a later growth
        multiplicity refusal. It is not a rollback of caller-owned Adam work.
        """
        self._clean_topology_seam()
        if type(refine) is not bool or (grow_count is not None and (type(grow_count) is not int or grow_count < 0)):
            raise ValueError('quality_mcmc_step_config_invalid')
        if grow_count is not None and not refine:
            raise ValueError('quality_mcmc_growth_requires_refine')
        summary = {'relocated': 0, 'added': 0, 'relocation_max_multiplicity': 0, 'growth_max_multiplicity': 0}
        if not refine:
            return summary
        logits = self.parameters['opacity_logits'].detach().clone()
        log_scales = self.parameters['log_scales'].detach().clone()
        generator = torch.Generator(device=self.device)
        generator.set_state(self.relocation_generator.get_state().clone())
        def family(sampled, stage):
            parents, inverse, numbers = torch.unique(sampled, sorted=True, return_inverse=True, return_counts=True)
            try:
                alpha, scales = relocation_parameters(logits[parents, 0].sigmoid(), log_scales[parents].exp(),
                    numbers+1, self.config['max_multiplicity'])
            except ValueError as error:
                raise ValueError('quality_mcmc_topology_preflight_failed: ' + stage + ': ' + str(error)) from error
            summary[stage+'_max_multiplicity'] = int(numbers.max())+1
            return parents, inverse, torch.logit(alpha)[:, None], scales.log()
        opacity = logits[:, 0].sigmoid()
        dead = torch.where(opacity <= self.config['dead_threshold'])[0]
        live = torch.where(opacity > self.config['dead_threshold'])[0]
        if len(dead) and len(live):
            sampled = live[torch.multinomial(opacity[live], len(dead), replacement=True, generator=generator)]
            parents, inverse, alpha, scales = family(sampled, 'relocation')
            logits[dead], log_scales[dead] = alpha[inverse], scales[inverse]
            logits[parents], log_scales[parents] = alpha, scales
            summary['relocated'] = len(dead)
        current = len(logits)
        requested = int(current*self.config['growth_rate']) if grow_count is None else grow_count
        added = min(requested, self.config['cap']-current)
        opacity = logits[:, 0].sigmoid()
        live = torch.where(opacity > self.config['dead_threshold'])[0]
        if added and len(live):
            sampled = live[torch.multinomial(opacity[live], added, replacement=True, generator=generator)]
            family(sampled, 'growth')
            summary['added'] = added
        return summary

    def after_optimizer_step(self, *, position_lr, refine=False, grow_count=None):
        """Post-Adam phase only: NEVER calls optimizer.step or consumes gradients.

        Caller must have performed Adam and cleared its consumed gradients.
        GSWMCMCBridge additionally verifies real optimizer step counters advanced.
        """
        return self._finish_after_optimizer_step(position_lr=position_lr, refine=refine,
                                                 grow_count=grow_count, optimizer_step=False)

    def _finish_after_optimizer_step(self, *, position_lr, refine, grow_count, optimizer_step):
        self._clean_topology_seam()
        mean_group = next(group for group in self.optimizer.param_groups
                          if group['name'] == self._optimizer_group_names['means'])
        if (not math.isfinite(float(position_lr)) or position_lr < 0 or
                float(position_lr) != float(mean_group['lr'])):
            raise ValueError('quality_mcmc_position_lr_mismatch')
        preflight = self.preflight_topology(refine=refine, grow_count=grow_count)
        relocation = self.relocate() if refine else None
        growth = self.grow(grow_count) if refine else None
        noise = self.inject_noise(position_lr)
        self.steps += 1
        self.last_position_lr = float(position_lr)
        return self._record({'kind': 'step', 'step': self.steps, 'optimizer_step': optimizer_step,
                             'external_optimizer_step': not optimizer_step, 'preflight': preflight,
                             'relocation': relocation, 'growth': growth, 'noise': noise})

    def step(self, *, position_lr, refine=False, grow_count=None):
        """Adam drift -> cleared grads -> optional topology -> covariance noise.

        The caller adds regularization_loss before backward. This explicit
        post-Adam topology order differs from the author trainer and avoids
        losing pending gradients when Parameters are rebound. A step failure
        after Adam is fatal to that attempt, not a promise of whole-step rollback;
        resume from a prior adapter checkpoint. Topology operations individually
        validate before mutation and restore their RNG on rejected plans.
        """
        self._validate()
        mean_group = next(group for group in self.optimizer.param_groups
                          if group['name'] == self._optimizer_group_names['means'])
        if (not math.isfinite(float(position_lr)) or position_lr < 0 or
                float(position_lr) != float(mean_group['lr'])):
            raise ValueError('quality_mcmc_position_lr_mismatch')
        if type(refine) is not bool or (grow_count is not None and (type(grow_count) is not int or grow_count < 0)):
            raise ValueError('quality_mcmc_step_config_invalid')
        if grow_count is not None and not refine:
            raise ValueError('quality_mcmc_growth_requires_refine')
        gradients = [value.grad for value in self.parameters.values() if value.grad is not None]
        if not gradients or any(not bool(torch.isfinite(gradient).all()) for gradient in gradients):
            raise ValueError('quality_mcmc_step_requires_finite_gradients')
        self.optimizer.step()
        self.optimizer.zero_grad(set_to_none=True)
        return self._finish_after_optimizer_step(position_lr=position_lr, refine=refine,
                                                 grow_count=grow_count, optimizer_step=True)

    def strategy_state_dict(self):
        """Lean sidecar when a native checkpoint already owns model and Adam.

        No duplicate parameter/moment tensors are allocated on the GPU. The
        caller must bind these fields to its exact native capture separately.
        """
        self._clean_topology_seam()
        return {'version': self.VERSION, 'method': self.METHOD, 'config': copy.deepcopy(self.config),
                'torch': torch.__version__, 'device': str(self.device),
                'rng': {'noise': self.noise_generator.get_state().clone(),
                        'relocation': self.relocation_generator.get_state().clone()},
                'gaussian_ids': self.gaussian_ids.clone(), 'next_gaussian_id': self.next_gaussian_id,
                'steps': self.steps, 'event_index': self.event_index,
                'last_event': copy.deepcopy(self.last_event), 'last_position_lr': self.last_position_lr}

    def load_strategy_state_dict(self, state):
        self._clean_topology_seam()
        fields = {'version', 'method', 'config', 'torch', 'device', 'rng', 'gaussian_ids',
                  'next_gaussian_id', 'steps', 'event_index', 'last_event', 'last_position_lr'}
        if (type(state) is not dict or set(state) != fields or state.get('version') != self.VERSION
                or state.get('method') != self.METHOD or state.get('config') != self.config
                or state.get('torch') != torch.__version__ or state.get('device') != str(self.device)):
            raise ValueError('quality_mcmc_strategy_identity_mismatch')
        try:
            ids, next_id = state['gaussian_ids'], state['next_gaussian_id']
            if (not isinstance(ids, torch.Tensor) or ids.dtype != torch.int64 or ids.device != self.device
                    or ids.shape != (len(self.parameters['means']),) or type(next_id) is not int or next_id < 0
                    or bool((ids < 0).any() or (ids >= next_id).any()) or len(torch.unique(ids)) != len(ids)
                    or any(type(state[key]) is not int or state[key] < 0 for key in ('steps', 'event_index'))):
                raise ValueError('invalid identities or counters')
            lr = state['last_position_lr']
            if lr is not None and (type(lr) not in (int, float) or not math.isfinite(lr) or lr < 0):
                raise ValueError('invalid learning rate')
            if state['last_event'] is not None and type(state['last_event']) is not dict:
                raise ValueError('invalid event')
            json.dumps(state['last_event'], allow_nan=False)
            if type(state['rng']) is not dict or set(state['rng']) != {'noise', 'relocation'}:
                raise ValueError('invalid RNG states')
            noise, relocation = self._generator('position_noise'), self._generator('relocation_sampling')
            noise.set_state(state['rng']['noise'].clone())
            relocation.set_state(state['rng']['relocation'].clone())
        except (ValueError, TypeError, KeyError, RuntimeError, AttributeError) as error:
            raise ValueError('quality_mcmc_strategy_state_invalid: ' + str(error)) from error
        self.noise_generator, self.relocation_generator = noise, relocation
        self.gaussian_ids, self.next_gaussian_id = ids.clone(), next_id
        self.steps, self.event_index = state['steps'], state['event_index']
        self.last_event, self.last_position_lr = copy.deepcopy(state['last_event']), lr

    def state_dict(self):
        """Independent full adapter snapshot at a cleared-gradient step boundary.

        Contains parameters, named Adam groups/moments, both strategy RNG states,
        Gaussian identities and counters. Dataset, sampler, renderer and schedule
        identities are caller-owned and must join a complete worker checkpoint.
        No file is loaded or written by this interface.
        """
        self._clean_topology_seam()
        return {'version': self.VERSION, 'method': self.METHOD, 'config': copy.deepcopy(self.config),
                'torch': torch.__version__, 'device': str(self.device),
                'parameters': {name: value.detach().clone() for name, value in self.parameters.items()},
                'optimizer': copy.deepcopy(self.optimizer.state_dict()),
                'optimizer_defaults': copy.deepcopy(self.optimizer.defaults),
                'rng': {'noise': self.noise_generator.get_state().clone(),
                        'relocation': self.relocation_generator.get_state().clone()},
                'gaussian_ids': self.gaussian_ids.clone(), 'next_gaussian_id': self.next_gaussian_id,
                'steps': self.steps, 'event_index': self.event_index, 'last_event': copy.deepcopy(self.last_event),
                'last_position_lr': self.last_position_lr}

    def load_state_dict(self, state):
        """Validate a complete candidate first; failed restore leaves live state intact."""
        self._clean_topology_seam()
        fields = {'version', 'method', 'config', 'torch', 'device', 'parameters', 'optimizer',
                  'optimizer_defaults', 'rng', 'gaussian_ids', 'next_gaussian_id', 'steps',
                  'event_index', 'last_event', 'last_position_lr'}
        if (type(state) is not dict or set(state) != fields or type(state.get('version')) is not int or
                state.get('version') != self.VERSION or
                state.get('method') != self.METHOD or state.get('config') != self.config):
            raise ValueError('quality_mcmc_checkpoint_identity_mismatch')
        if state['torch'] != torch.__version__ or state['device'] != str(self.device):
            raise ValueError('quality_mcmc_checkpoint_runtime_mismatch')
        try:
            saved = state['parameters']
            if type(saved) is not dict or set(saved) != set(self.parameters):
                raise ValueError('parameter names differ')
            new_parameters = {}
            for name, old in self.parameters.items():
                value = saved[name]
                if (not isinstance(value, torch.Tensor) or value.dtype != old.dtype or value.device != old.device or
                        value.ndim != old.ndim or value.shape[1:] != old.shape[1:]):
                    raise ValueError('parameter dtype/device/trailing shape differs')
                new_parameters[name] = torch.nn.Parameter(value.detach().clone())
            group_names = [group['name'] for group in self.optimizer.param_groups]
            if ([group.get('name') for group in state['optimizer']['param_groups']] != group_names or
                    any(len(group['params']) != 1 for group in state['optimizer']['param_groups'])):
                raise ValueError('optimizer group layout differs')
            new_groups = [{'params': [new_parameters[self._group_name_to_parameter[name]]], 'name': name}
                          for name in group_names]
            new_optimizer = torch.optim.Adam(new_groups, **copy.deepcopy(state['optimizer_defaults']))
            new_optimizer.load_state_dict(copy.deepcopy(state['optimizer']))
            candidate = MCMCAdapter(new_parameters, new_optimizer, **self.config)
            ids = state['gaussian_ids']
            next_id = state['next_gaussian_id']
            if (not isinstance(ids, torch.Tensor) or ids.dtype != torch.int64 or ids.device != self.device or
                    tuple(ids.shape) != (len(new_parameters['means']),) or
                    type(next_id) is not int or next_id < 0 or bool((ids < 0).any()) or
                    bool((ids >= next_id).any()) or len(torch.unique(ids)) != len(ids)):
                raise ValueError('invalid Gaussian identities')
            for counter in ('steps', 'event_index'):
                if type(state[counter]) is not int or state[counter] < 0:
                    raise ValueError('invalid counter')
            lr = state['last_position_lr']
            if lr is not None and (type(lr) not in (int, float) or not math.isfinite(lr) or lr < 0):
                raise ValueError('invalid position learning rate')
            if state['last_event'] is not None and type(state['last_event']) is not dict:
                raise ValueError('invalid event')
            json.dumps(state['last_event'], allow_nan=False)
            if type(state['rng']) is not dict or set(state['rng']) != {'noise', 'relocation'}:
                raise ValueError('invalid random-state collection')
            candidate.noise_generator.set_state(state['rng']['noise'].clone())
            candidate.relocation_generator.set_state(state['rng']['relocation'].clone())
        except (TypeError, ValueError, KeyError, RuntimeError, AttributeError) as error:
            raise ValueError('quality_mcmc_checkpoint_invalid: ' + str(error)) from error
        # Commit only after every tensor, optimizer state and RNG has validated.
        self.parameters.clear()
        self.parameters.update(new_parameters)
        self.optimizer.param_groups = new_optimizer.param_groups
        self.optimizer.state = new_optimizer.state
        self.optimizer.defaults = new_optimizer.defaults
        self.noise_generator, self.relocation_generator = candidate.noise_generator, candidate.relocation_generator
        self.gaussian_ids, self.next_gaussian_id = ids.clone(), next_id
        self.steps, self.event_index = state['steps'], state['event_index']
        self.last_event, self.last_position_lr = copy.deepcopy(state['last_event']), lr
