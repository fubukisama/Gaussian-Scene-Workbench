# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Read-only lookahead for the trainers' existing randint/pop camera sampler."""
import random


def predict_next_camera(cameras, pending_indices, python_rng_state=None):
    """Predict one camera after the current sample has already been popped.

    ``cameras`` is the stable full training-camera sequence. ``pending_indices``
    retains its current order; an exhausted epoch refills in full camera order,
    exactly as the legacy trainers do. No sampler state is advanced or retained:
    checkpoints still own the original Python RNG and pending-index list. A
    caller can pass a saved Python RNG state instead of reading the live one.
    An empty training set has no prediction and returns ``None``.
    """
    if not cameras:
        return None
    pending = pending_indices if pending_indices else range(len(cameras))
    clone = random.Random(0)
    clone.setstate(random.getstate() if python_rng_state is None else python_rng_state)
    return cameras[pending[clone.randint(0, len(pending) - 1)]]
