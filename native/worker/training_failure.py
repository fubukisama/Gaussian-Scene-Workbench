# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Structured training failures without changing exception or checkpoint state.

Torch is an injected system dependency, not imported here. This permits version-
specific typed CUDA allocator errors and CPU-only regression without allocation.
"""
from contextlib import contextmanager


def training_failure_record(error, backend, torch_module=None,
                            last_completed_iteration=None, checkpoint_preserved=None):
    """Describe observed evidence only; textual CUDA reports are not typed OOM."""
    if backend not in ('3dgs', '2dgs') or not isinstance(error, Exception):
        raise ValueError('Invalid training failure input')
    classes = []
    for namespace in (torch_module, getattr(torch_module, 'cuda', None)):
        candidate = getattr(namespace, 'OutOfMemoryError', None)
        if isinstance(candidate, type) and issubclass(candidate, Exception):
            classes.append(candidate)
    message = str(error)
    if classes and isinstance(error, tuple(classes)):
        kind = 'cuda_oom'
    elif isinstance(error, RuntimeError) and any(marker in message.lower() for marker in (
            'cuda out of memory', 'cuda error: out of memory',
            'cuda runtime error (2) : out of memory')):
        kind = 'cuda_oom_reported'
    else:
        kind = 'training_exception'
    exception_class = type(error)
    record = {'version': 1, 'backend': backend, 'kind': kind,
              'exception_type': (exception_class.__module__ + '.' + exception_class.__name__)[:512],
              'message': message[:8192], 'message_truncated': len(message) > 8192}
    if last_completed_iteration is not None:
        if type(last_completed_iteration) is not int or not 0 <= last_completed_iteration <= 2 ** 53 - 1:
            raise ValueError('Invalid last completed iteration')
        record['last_completed_iteration'] = last_completed_iteration
    if checkpoint_preserved is not None:
        if type(checkpoint_preserved) is not bool:
            raise ValueError('Invalid checkpoint preservation evidence')
        record['checkpoint_preserved'] = checkpoint_preserved
    return record


@contextmanager
def report_training_failure(backend, torch_module, emit):
    """Emit once then re-raise; no failed-state save or recovery-file writes.

This intentionally does not catch BaseException, so explicit process exit and
keyboard interruption keep their original semantics. Unknown completed-step or
checkpoint-preservation metadata is omitted, never asserted optimistically.
"""
    try:
        yield
    except Exception as error:
        try:
            emit('[gsw-training-error]', training_failure_record(error, backend, torch_module))
        except Exception:
            # A broken stdout pipe or error formatter must not replace the
            # actual allocator/training exception that made the process fail.
            pass
        raise
