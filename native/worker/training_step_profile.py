# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Sample complete native training steps, without synchronizing every step.

CUDA stage timings are stream intervals, not claims about pure kernel time.
The wall sample ends after the sampled GPU work has completed, including the
optimizer, densification and observation/checkpoint work.
"""
import math
import time
from contextlib import contextmanager


STAGES = frozenset(('setup', 'transfer', 'render', 'loss', 'backward',
                    'densification', 'optimizer', 'preview', 'checkpoint', 'report'))
MEMORY_FIELDS = ('torch_allocated_bytes', 'torch_reserved_bytes',
                 'torch_peak_allocated_bytes', 'device_free_bytes', 'device_total_bytes')


def normalize_performance(payload):
    if not isinstance(payload, dict) or type(payload.get('version')) is not int or payload.get('version') != 1:
        return None
    if payload.get('backend') not in ('3dgs', '2dgs'):
        return None
    iteration = payload.get('iteration')
    if isinstance(iteration, bool) or not isinstance(iteration, int) or not 0 <= iteration <= 2 ** 53 - 1:
        return None
    def real(value):
        return (not isinstance(value, bool) and isinstance(value, (int, float))
                and math.isfinite(value) and value >= 0)
    if not real(payload.get('wall_step_ms')):
        return None
    result = {key: payload[key] for key in ('version', 'backend', 'iteration', 'wall_step_ms')}
    for field in ('stages_ms', 'gpu_stages_ms'):
        values = payload.get(field, {})
        if not isinstance(values, dict) or any(key not in STAGES or not real(value)
                                               for key, value in values.items()):
            return None
        result[field] = dict(values)
    for field in MEMORY_FIELDS:
        if field in payload:
            value = payload[field]
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 2 ** 53 - 1:
                return None
            result[field] = value
    for field, allowed in (('renderer_engine', ('stock', 'fastergs')),
                           ('loss_engine', ('stock', 'fused_l1_ssim'))):
        if field in payload:
            if payload[field] not in allowed:
                return None
            result[field] = payload[field]
    if result.get('device_free_bytes', 0) > result.get('device_total_bytes', float('inf')):
        return None
    return result


class IterationProfiler:
    def __init__(self, backend, interval=30, clock=time.perf_counter, torch_module=None, emit=None, metadata=None):
        if backend not in ('3dgs', '2dgs') or interval < 1:
            raise ValueError('Invalid training profiler configuration')
        self.backend, self.interval, self.clock = backend, int(interval), clock
        self.torch, self.emit = torch_module, emit
        self.metadata = metadata
        if torch_module is not None and torch_module.cuda.is_available():
            # A job-wide peak, including loading/density work between samples.
            # Resetting at every sample would lose unsampled growth peaks.
            torch_module.cuda.reset_peak_memory_stats()
        self.active = False
        self._stage = None

    def begin(self, iteration):
        self.iteration = int(iteration)
        self.active = iteration == 1 or iteration % self.interval == 0
        self._stage = None
        self._cpu = {}
        self._gpu = []
        self._cuda = self.active and self.torch is not None and self.torch.cuda.is_available()
        if self.active:
            # Only sampled iterations pay synchronization and reset-peak costs.
            if self._cuda:
                self.torch.cuda.synchronize()
            self._started = self.clock()
            self.mark('setup')

    def _end_stage(self):
        if self._stage is None:
            return
        name, started, event = self._stage
        self._cpu[name] = self._cpu.get(name, 0.) + (self.clock() - started) * 1000.
        if event is not None:
            end = self.torch.cuda.Event(enable_timing=True)
            end.record()
            self._gpu.append((name, event, end))
        self._stage = None

    def mark(self, name):
        if name not in STAGES:
            raise ValueError('Unknown training stage')
        if not self.active:
            return
        self._end_stage()
        event = None
        if self._cuda:
            event = self.torch.cuda.Event(enable_timing=True)
            event.record()
        self._stage = (name, self.clock(), event)

    @contextmanager
    def stage(self, name):
        self.mark(name)
        try:
            yield
        finally:
            if self.active:
                self._end_stage()

    def finish(self):
        if not self.active:
            return None
        self._end_stage()
        if self._cuda:
            self.torch.cuda.synchronize()
        payload = {'version': 1, 'backend': self.backend, 'iteration': self.iteration,
                   'wall_step_ms': (self.clock() - self._started) * 1000.,
                   'stages_ms': self._cpu, 'gpu_stages_ms': {}}
        if self._cuda:
            for name, start, end in self._gpu:
                payload['gpu_stages_ms'][name] = payload['gpu_stages_ms'].get(name, 0.) + start.elapsed_time(end)
            payload.update(torch_allocated_bytes=self.torch.cuda.memory_allocated(),
                           torch_reserved_bytes=self.torch.cuda.memory_reserved(),
                           torch_peak_allocated_bytes=self.torch.cuda.max_memory_allocated())
            try:
                free, total = self.torch.cuda.mem_get_info()
                payload.update(device_free_bytes=int(free), device_total_bytes=int(total))
            except (RuntimeError, AttributeError):
                pass
        self.active = False
        if self.metadata:
            payload.update(self.metadata())
        payload = normalize_performance(payload)
        if self.emit and payload:
            self.emit('[gsw-training-performance]', payload)
        return payload
