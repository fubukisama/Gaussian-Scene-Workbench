# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Explicit GaussianModel protocol bridge for independent MCMC mathematics.

This original adapter does not import/copy the restricted GaussianModel or
change its group names, LR schedule, standard trainer or resource policy.
CPU protocol tests do not certify the actual Graphdeco CUDA model/renderer.
"""
import math
import copy
import hashlib

import torch

from native.worker.quality_mcmc import MCMCAdapter


def _equal_state(left, right):
    if isinstance(left, torch.Tensor) and isinstance(right, torch.Tensor):
        return left.dtype == right.dtype and left.device == right.device and torch.equal(left, right)
    if isinstance(left, dict) and isinstance(right, dict):
        return set(left) == set(right) and all(_equal_state(left[key], right[key]) for key in left)
    if isinstance(left, (list, tuple)) and isinstance(right, (list, tuple)):
        return len(left) == len(right) and all(_equal_state(a, b) for a, b in zip(left, right))
    return type(left) is type(right) and left == right


class GSWMCMCBridge:
    STATE_SCHEMA = 'gsw.mcmc_model_bridge/1'
    ATTRIBUTES = {'means': '_xyz', 'features_dc': '_features_dc', 'features_rest': '_features_rest',
                  'opacity_logits': '_opacity', 'log_scales': '_scaling', 'quaternions': '_rotation'}
    GROUP_NAMES = {'means': 'xyz', 'features_dc': 'f_dc', 'features_rest': 'f_rest',
                   'opacity_logits': 'opacity', 'log_scales': 'scaling', 'quaternions': 'rotation'}
    STATISTICS = {'max_radii2D': (), 'xyz_gradient_accum': (1,), 'denom': (1,)}
    REQUIRES_STANDARD_DENSITY_DISABLED = True

    def __init__(self, model, *, seed, cap, noise_lr=5e5, **options):
        if 'optimizer_group_names' in options:
            raise ValueError('quality_mcmc_model_mapping_is_fixed')
        self.model = model
        self._check_resource_adapter()
        if type(getattr(model, 'optimizer', None)) is not torch.optim.Adam:
            raise ValueError('quality_mcmc_model_default_adam_required')
        parameters = {name: getattr(model, attr, None) for name, attr in self.ATTRIBUTES.items()}
        self.adapter = MCMCAdapter(parameters, model.optimizer, seed=seed, cap=cap, noise_lr=noise_lr,
                                   optimizer_group_names=self.GROUP_NAMES, **options)
        self._check_statistics()
        self._last_optimizer_steps = self._optimizer_steps()

    def _check_resource_adapter(self):
        if getattr(self.model, '_gsw_capacity_storage', None) is not None:
            raise ValueError('quality_mcmc_capacity_storage_adapter_required')

    def _check_statistics(self):
        count = len(self.adapter.parameters['means'])
        for name, trailing in self.STATISTICS.items():
            value = getattr(self.model, name, None)
            if (not isinstance(value, torch.Tensor) or tuple(value.shape) != (count,)+trailing or
                    value.device != self.adapter.device or not bool(torch.isfinite(value).all())):
                raise ValueError('quality_mcmc_model_statistics_invalid: ' + name)
        temporary = getattr(self.model, 'tmp_radii', None)
        if temporary is not None and (not isinstance(temporary, torch.Tensor) or
                tuple(temporary.shape) != (count,) or temporary.device != self.adapter.device):
            raise ValueError('quality_mcmc_model_statistics_invalid: tmp_radii')

    def _require_bound(self):
        self._check_resource_adapter()
        if (self.model.optimizer is not self.adapter.optimizer or
                any(getattr(self.model, attr) is not self.adapter.parameters[name]
                    for name, attr in self.ATTRIBUTES.items())):
            raise ValueError('quality_mcmc_model_rebind_required')
        self._check_statistics()

    def _optimizer_steps(self):
        result = {}
        for group in self.model.optimizer.param_groups:
            step = self.model.optimizer.state.get(group['params'][0], {}).get('step', 0.)
            value = float(step)
            if not math.isfinite(value) or value < 0 or value != int(value):
                raise ValueError('quality_mcmc_model_optimizer_counter_invalid')
            result[group['name']] = value
        return result

    def _require_completed_step(self):
        if self._optimizer_steps() != self._last_optimizer_steps:
            raise ValueError('quality_mcmc_model_incomplete_optimizer_step')

    def _synchronize(self, reset_statistics=False):
        for name, attr in self.ATTRIBUTES.items():
            setattr(self.model, attr, self.adapter.parameters[name])
        count = len(self.model._xyz)
        for name, trailing in self.STATISTICS.items():
            old = getattr(self.model, name)
            if reset_statistics or tuple(old.shape) != (count,)+trailing:
                setattr(self.model, name, old.new_zeros((count,)+trailing))
        temporary = getattr(self.model, 'tmp_radii', None)
        if temporary is not None and (reset_statistics or len(temporary) != count):
            self.model.tmp_radii = temporary.new_zeros((count,))

    def regularization_loss(self):
        self._require_bound()
        self._require_completed_step()
        return self.adapter.regularization_loss()

    def after_optimizer_step(self, *, position_lr, refine=False, grow_count=None):
        """Finish exactly one caller-owned Adam step, never perform a second one.

        Clears already-consumed gradients. Core preflight rejects multiplicity
        for the entire relocation+growth phase before its first mutation. A
        rejected post-phase is incomplete; restore the previous full checkpoint
        if caller-owned Adam must also roll back. No memory-policy override or
        standard density/reset call is made here.
        """
        self._require_bound()
        now = self._optimizer_steps()
        differences = [now[name]-old for name, old in self._last_optimizer_steps.items()]
        if not any(value == 1 for value in differences) or any(value not in (0, 1) for value in differences):
            raise ValueError('quality_mcmc_model_requires_one_external_adam_step')
        self.model.optimizer.zero_grad(set_to_none=True)
        count, event_index = len(self.model._xyz), self.adapter.event_index
        try:
            event = self.adapter.after_optimizer_step(position_lr=position_lr, refine=refine, grow_count=grow_count)
        finally:
            # Even a later failure cannot leave model attrs pointing at discarded Parameters.
            reset = len(self.adapter.parameters['means']) != count or (refine and self.adapter.event_index != event_index)
            self._synchronize(reset_statistics=reset)
        self._last_optimizer_steps = self._optimizer_steps()
        return event

    def state_dict(self):
        """Full bridge/adapter sidecar, only at a completed external-step boundary."""
        self._require_bound()
        self._require_completed_step()
        statistics = {name: getattr(self.model, name).detach().clone() for name in self.STATISTICS}
        temporary = getattr(self.model, 'tmp_radii', None)
        statistics['tmp_radii'] = None if temporary is None else temporary.detach().clone()
        return {'schema': self.STATE_SCHEMA, 'adapter': self.adapter.state_dict(),
                'statistics': statistics, 'optimizer_steps': dict(self._last_optimizer_steps)}

    def _native_binding(self):
        """Bounded host chunks; never clone the complete model/Adam on GPU."""
        digest = hashlib.sha256()

        def visit(value):
            if isinstance(value, torch.Tensor):
                if not value.is_contiguous():
                    raise ValueError('quality_mcmc_native_tensor_layout_unsupported')
                digest.update(repr((str(value.dtype), str(value.device), tuple(value.shape))).encode('ascii'))
                flat = value.detach().view(-1)
                for offset in range(0, flat.numel(), 262144):
                    digest.update(flat[offset:offset + 262144].cpu().numpy().tobytes())
            elif isinstance(value, dict):
                digest.update(b'{')
                for key in sorted(value, key=lambda item: (type(item).__name__, str(item))):
                    visit(key)
                    visit(value[key])
                digest.update(b'}')
            elif isinstance(value, (list, tuple)):
                digest.update(b'[')
                for item in value:
                    visit(item)
                digest.update(b']')
            else:
                digest.update(repr((type(value).__name__, value)).encode('utf-8') + b'\x00')
        visit({name: getattr(self.model, attr) for name, attr in self.ATTRIBUTES.items()})
        visit(self.model.optimizer.state_dict())
        visit({name: getattr(self.model, name) for name in self.STATISTICS})
        return digest.hexdigest()

    def native_checkpoint_state(self):
        self._require_bound()
        self._require_completed_step()
        return {'schema': 'gsw.mcmc_native_strategy/1', 'native_binding_sha256': self._native_binding(),
                'strategy': self.adapter.strategy_state_dict(), 'optimizer_steps': dict(self._last_optimizer_steps)}

    def restore_native_checkpoint(self, state):
        self._check_resource_adapter()
        if (type(state) is not dict or set(state) != {'schema', 'native_binding_sha256', 'strategy', 'optimizer_steps'}
                or state.get('schema') != 'gsw.mcmc_native_strategy/1'
                or state.get('native_binding_sha256') != self._native_binding()
                or state.get('optimizer_steps') != self._optimizer_steps()):
            raise ValueError('quality_mcmc_native_binding_mismatch')
        parameters = {name: getattr(self.model, attr) for name, attr in self.ATTRIBUTES.items()}
        candidate = MCMCAdapter(parameters, self.model.optimizer, **self.adapter.config)
        candidate.load_strategy_state_dict(state['strategy'])
        self.adapter = candidate
        self._last_optimizer_steps = self._optimizer_steps()
        self._require_bound()

    def capture(self):
        """Pair the model's opaque native capture with the full MCMC sidecar.

        Normal resume order: model.restore(saved['model'], training_args), then
        bridge.rebind_after_model_restore(saved['bridge']). This includes the
        native capture's metadata, not additional worker/exposure/sampler state
        that the native model itself does not capture. Checkpoint copies are
        explicit and have not been performance/memory-accepted on CUDA.
        """
        sidecar = self.state_dict()
        if not callable(getattr(self.model, 'capture', None)):
            raise ValueError('quality_mcmc_model_capture_required')
        return {'schema': 'gsw.mcmc_model_capture/1', 'model': copy.deepcopy(self.model.capture()), 'bridge': sidecar}

    def rebind_after_model_restore(self, state):
        """Attach to restored native attrs/optimizer; never rename their groups.

        Native parameters/optimizer must already equal the sidecar's values;
        attaching the wrong native capture is rejected, not silently overwritten.
        A complete candidate and all statistics validate before any commit.
        """
        self._check_resource_adapter()
        if type(getattr(self.model, 'optimizer', None)) is not torch.optim.Adam:
            raise ValueError('quality_mcmc_model_default_adam_required')
        if (type(state) is not dict or set(state) != {'schema', 'adapter', 'statistics', 'optimizer_steps'} or
                state.get('schema') != self.STATE_SCHEMA):
            raise ValueError('quality_mcmc_model_checkpoint_invalid')
        parameters = {name: getattr(self.model, attr, None) for name, attr in self.ATTRIBUTES.items()}
        candidate = MCMCAdapter(parameters, self.model.optimizer, **self.adapter.config)
        try:
            saved = state['adapter']
            if (not _equal_state(parameters, saved['parameters']) or
                    not _equal_state(self.model.optimizer.state_dict(), saved['optimizer'])):
                raise ValueError('native capture does not match adapter sidecar')
            count = len(saved['parameters']['means'])
            statistics = state['statistics']
            if type(statistics) is not dict or set(statistics) != set(self.STATISTICS) | {'tmp_radii'}:
                raise ValueError('statistics layout differs')
            replacement = {}
            for name, trailing in self.STATISTICS.items():
                value, current = statistics[name], getattr(self.model, name)
                if (not isinstance(value, torch.Tensor) or tuple(value.shape) != (count,)+trailing or
                        value.dtype != current.dtype or value.device != candidate.device or
                        not bool(torch.isfinite(value).all()) or not _equal_state(value, current)):
                    raise ValueError('restored native statistics differ: ' + name)
                replacement[name] = value.detach().clone()
            temporary = statistics['tmp_radii']
            if temporary is not None and (not isinstance(temporary, torch.Tensor) or
                    tuple(temporary.shape) != (count,) or temporary.device != candidate.device or
                    not bool(torch.isfinite(temporary).all())):
                raise ValueError('temporary radius layout differs')
            replacement['tmp_radii'] = None if temporary is None else temporary.detach().clone()
            if not _equal_state(state['optimizer_steps'], self._optimizer_steps()):
                raise ValueError('optimizer completion marker differs')
            # Core loader is itself transactional; all bridge checks precede its commit.
            candidate.load_state_dict(saved)
        except (TypeError, ValueError, KeyError, AttributeError, RuntimeError) as error:
            raise ValueError('quality_mcmc_model_checkpoint_invalid: ' + str(error)) from error
        self.adapter = candidate
        self._synchronize()
        for name, value in replacement.items():
            setattr(self.model, name, value)
        self._last_optimizer_steps = self._optimizer_steps()

    def load_state_dict(self, state):
        return self.rebind_after_model_restore(state)
