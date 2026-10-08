"""Observation-only resource scheduling; never changes optimizer state or inputs.

The memory probe is an external CUDA query, not a tensor operation. Publishers
must ask permission before reading or packing the model's training tensors.
"""
import math
import time

MIB = 1024**2
GIB = 1024**3


def cuda_memory():
    import torch
    if not torch.cuda.is_available():
        return None
    return torch.cuda.mem_get_info()


def close_preview_safely(publisher, emit, **kwargs):
    """Observer teardown failures must not turn optimization into a failed job."""
    try:
        publisher.close(**kwargs)
        return True
    except Exception as error:
        emit("[gsw-preview-warning]", str(error))
        return False


class TrainingPreviewPolicy:
    def __init__(self, memory_probe=None, clock=None, emit=None):
        self.memory_probe = memory_probe or cuda_memory
        self.clock = clock or time.monotonic
        self.emit = emit
        self.enabled = True
        self.pressure = False
        self.pressure_reason = "vram_pressure"
        self.last_probe = float("-inf")
        self.last_status = None
        self.recovery_since = None

    def _allowed(self):
        if not self.enabled:
            return False
        now = self.clock()
        if now - self.last_probe >= .5:
            self.last_probe = now
            try:
                memory = self.memory_probe()
                if memory is not None:
                    free, total = memory
                    if (isinstance(free, bool) or isinstance(total, bool)
                            or not isinstance(free, (int, float)) or not isinstance(total, (int, float))
                            or not math.isfinite(free) or not math.isfinite(total)
                            or not 0 <= free <= total or total <= 0):
                        raise ValueError("Invalid CUDA memory observation")
                    if free < max(512 * MIB, total * .05):
                        self.pressure = True
                        self.pressure_reason = "vram_pressure"
                        self.recovery_since = None
                    elif self.pressure:
                        if free >= max(GIB, total * .10):
                            if self.recovery_since is None:
                                self.recovery_since = now
                            elif now - self.recovery_since >= 10.:
                                self.pressure = False
                                self.recovery_since = None
                        else:
                            self.recovery_since = None
            except Exception:
                # An unavailable observation query must not abort optimization.
                self.pressure = True
                self.pressure_reason = "probe_unavailable"
                self.recovery_since = None
            status = self.pressure_reason if self.pressure else "active"
            if status != self.last_status:
                self.last_status = status
                if self.emit:
                    self.emit("[gsw-preview-status]", {"state": status})
        return not self.pressure

    def allows_snapshot(self, shared_gpu_healthy=False):
        return self._allowed()

    def allows_gpu(self):
        return self._allowed()

    def set_enabled(self, enabled):
        self.enabled = bool(enabled)
