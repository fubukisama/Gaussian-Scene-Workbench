# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Opt-in independent random streams; importing creates no Torch/CUDA state.

Frozen dataset initialization remains owned by the existing initializer.
Version-one seeds are SHA-256(domain + seed), not the process-global RNG.
"""
import hashlib
import random
import re


def _stream_seed(seed, domain):
    text = "gsw-quality-rng-v1:{}:{}".format(domain, seed)
    return int.from_bytes(hashlib.sha256(text.encode("ascii")).digest()[:8], "little")


class QualityRandomStreams:
    VERSION = 1

    def __init__(self, seed):
        if type(seed) is not int or not 0 <= seed < 2 ** 32:
            raise ValueError("quality_rng_seed_invalid")
        self.seed = seed
        self._image = random.Random(_stream_seed(seed, "image_sampler"))
        self._strategy = random.Random(_stream_seed(seed, "strategy_noise"))
        self._torch = {}

    def choose_index(self, count):
        if type(count) is not int or count <= 0:
            raise ValueError("quality_rng_camera_count_invalid")
        return self._image.randrange(count)

    def image_sampler_state(self):
        """Immutable Python RNG snapshot for read-only camera lookahead."""
        return self._image.getstate()

    def strategy_random(self):
        return self._strategy

    def torch_generator(self, device="cpu"):
        if type(device) is not str or not re.fullmatch(r"cpu|cuda:[0-9]+", device):
            raise ValueError("quality_rng_device_invalid")
        if device not in self._torch:
            import torch
            generator = torch.Generator(device=device)
            generator.manual_seed(_stream_seed(self.seed, "strategy_noise:" + device) % (2 ** 63))
            self._torch[device] = generator
        return self._torch[device]

    def state_dict(self):
        return {"version": self.VERSION, "seed": self.seed,
                "image_sampler": self._image.getstate(),
                "strategy_python": self._strategy.getstate(),
                "strategy_torch": {device: generator.get_state().clone()
                                   for device, generator in self._torch.items()}}

    def load_state_dict(self, state):
        if (type(state) is not dict or type(state.get("version")) is not int
                or state["version"] != self.VERSION or type(state.get("seed")) is not int
                or state["seed"] != self.seed):
            raise ValueError("quality_rng_identity_mismatch")
        try:
            if set(state) != {"version", "seed", "image_sampler", "strategy_python", "strategy_torch"}:
                raise ValueError("unexpected RNG state fields")
            image, strategy = random.Random(), random.Random()
            image.setstate(state["image_sampler"])
            strategy.setstate(state["strategy_python"])
            saved_generators = state["strategy_torch"]
            if type(saved_generators) is not dict or len(saved_generators) > 16:
                raise ValueError("invalid generator collection")
            replacement = QualityRandomStreams(self.seed)
            for device, saved in saved_generators.items():
                replacement.torch_generator(device).set_state(saved.cpu())
        except (TypeError, ValueError, KeyError, RuntimeError, AttributeError) as error:
            raise ValueError("quality_rng_state_invalid: " + str(error)) from error
        # Failed restores never partially advance or replace any live stream.
        self._image, self._strategy, self._torch = image, strategy, replacement._torch
