"""Explicit training RNG policy, separate from frozen initialization seeds."""

import os
import random

import numpy as np


def validate_training_seed(value):
    if type(value) is not int or not 0 <= value <= 2**32 - 1:
        raise ValueError("training_seed_invalid: expected uint32 integer")
    return value


def seed_from_environment(value=None, environment=None):
    if value is not None:
        return validate_training_seed(value)
    environment = os.environ if environment is None else environment
    raw = environment.get("GSW_TRAINING_SEED", "0")
    if not isinstance(raw, str) or not raw.isascii() or not raw.isdigit() or len(raw) > 10:
        raise ValueError("training_seed_invalid: expected decimal uint32")
    return validate_training_seed(int(raw))


def apply_training_seed(seed, torch_module=None):
    """Call after upstream safe_state and before any training/model randomness.

    torch.manual_seed seeds CPU and CUDA generators; no separate CUDA call or
    image/split/initialization mutation is needed. Default zero matches the
    upstream safe_state policy. Full-state resume later restores saved RNGs.
    """
    seed = validate_training_seed(seed)
    if torch_module is None:
        import torch as torch_module
    random.seed(seed)
    np.random.seed(seed)
    torch_module.manual_seed(seed)
    return {"schema": "gsw.training_seed/1", "training_seed": seed,
            "initialization_seed_policy": "dataset_declared_unchanged",
            "resume_rng_policy": "complete_checkpoint_rng_restores_after_seed_setup",
            "bitwise_gpu_reproducibility": "not_guaranteed"}
