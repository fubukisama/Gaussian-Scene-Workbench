# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Bounded native camera-image staging shared by the 3DGS/2DGS adapters.

Independent implementation. Image sampling order is owned by the trainer;
this module never draws random numbers or changes dataset/optimizer arguments.
"""
from __future__ import annotations

from dataclasses import dataclass
from collections import OrderedDict
import os
from typing import Any, Optional


@dataclass(frozen=True)
class ImagePipelineConfig:
    enabled: bool = True
    slots: int = 2
    pinned_bytes: int = 256 * 1024 * 1024
    device_bytes: int = 512 * 1024 * 1024

    def __post_init__(self):
        if type(self.enabled) is not bool or any(type(value) is not int
                for value in (self.slots, self.pinned_bytes, self.device_bytes)):
            raise ValueError("image pipeline requires a boolean mode and integral finite byte budgets")
        if self.slots not in (1, 2):
            raise ValueError("image GPU slots must be one or two")
        if not 0 <= self.pinned_bytes <= 4096 * 1024 * 1024:
            raise ValueError("pinned image budget must be between zero and 4096 MiB")
        if not 0 < self.device_bytes <= 8192 * 1024 * 1024:
            raise ValueError("image GPU budget must be positive and at most 8192 MiB")

    @classmethod
    def from_environment(cls, environment=None):
        environment = os.environ if environment is None else environment
        return cls(enabled=environment.get("GSW_TRAINING_IMAGE_PIPELINE") == "bounded",
                   slots=int(environment.get("GSW_TRAINING_IMAGE_GPU_SLOTS", "2")),
                   pinned_bytes=int(environment.get("GSW_TRAINING_IMAGE_PINNED_MIB", "256")) * 1024 * 1024,
                   device_bytes=int(environment.get("GSW_TRAINING_IMAGE_GPU_MIB", "512")) * 1024 * 1024)


@dataclass(frozen=True)
class HostCameraImage:
    rgb: Any
    alpha_mask: Optional[Any] = None
    invdepthmap: Optional[Any] = None
    depth_mask: Optional[Any] = None
    bit_depth: Optional[int] = None

    @classmethod
    def from_tensors(cls, rgb, alpha_mask=None, invdepthmap=None, depth_mask=None,
                     bit_depth=None, torch_module=None):
        if torch_module is None:
            import torch as torch_module
        def host(tensor, compact=False):
            if tensor is None:
                return None
            tensor = tensor.detach().cpu().contiguous()
            if compact and bit_depth == 8 and tensor.dtype == torch_module.float32:
                candidate = tensor.mul(255.0).round().clamp(0, 255).to(torch_module.uint8)
                if torch_module.equal(candidate.float().div(255.0), tensor):
                    return candidate
            return tensor
        return cls(host(rgb, True), host(alpha_mask, True), host(invdepthmap),
                   host(depth_mask), bit_depth)

    def rgb_float(self, torch_module=None):
        if torch_module is None:
            import torch as torch_module
        value = self.rgb.to(torch_module.float32)
        return value.div(255.0) if self.rgb.dtype == torch_module.uint8 else value

    def alpha_float(self, torch_module=None):
        if self.alpha_mask is None:
            return None
        if torch_module is None:
            import torch as torch_module
        value = self.alpha_mask.to(torch_module.float32)
        return value.div(255.0) if self.alpha_mask.dtype == torch_module.uint8 else value

    @classmethod
    def from_pil(cls, image, resolution, torch_module=None, masked_half=None):
        if torch_module is None:
            import torch as torch_module
        import numpy as np
        pixels_array = np.array(image.resize(resolution), copy=True)
        # The camera adapter must retain its legacy float/HDR path. Never
        # squeeze a higher-bit-depth source into an eight-bit host cache.
        if pixels_array.dtype != np.uint8:
            return None
        pixels = torch_module.from_numpy(pixels_array)
        if pixels.ndim == 2:
            pixels = pixels.unsqueeze(-1)
        pixels = pixels.permute(2, 0, 1).contiguous()
        alpha = pixels[3:4].clone() if pixels.shape[0] == 4 else None
        if masked_half is not None:
            if masked_half not in ("left", "right"):
                raise ValueError("masked_half must be left, right or None")
            if alpha is None:
                alpha = torch_module.full((1, pixels.shape[1], pixels.shape[2]), 255, dtype=torch_module.uint8)
            middle = alpha.shape[-1] // 2
            if masked_half == "left":
                alpha[..., :middle] = 0
            else:
                alpha[..., middle:] = 0
        rgb = pixels[:3].clone() if pixels.shape[0] == 4 else pixels[:3]
        return cls(rgb, alpha, bit_depth=8)


def install_two_dgs_camera_adapter(torch_module=None, camera_module=None, camera_utils_module=None):
    """Install the compact PIL decode seam in this native worker process only.

    Call before constructing the external 2DGS Scene. The returned callable
    restores the original functions after those native cameras are discarded;
    external source files are untouched. Do not restore a live native Scene.
    Legacy workers are a no-op. Unsupported high-bit-depth PIL inputs retain
    the external decoder's float semantics rather than being down-quantized.
    """
    if os.environ.get("GSW_TRAINING_IMAGE_PIPELINE") != "bounded":
        return lambda: None
    ImagePipelineConfig.from_environment()
    if torch_module is None:
        import torch as torch_module
    if camera_module is None:
        from scene import cameras as camera_module
    if camera_utils_module is None:
        from utils import camera_utils as camera_utils_module
    import inspect
    camera_class = camera_module.Camera
    if getattr(camera_class, "_gsw_native_image_adapter", None) is not None:
        return lambda: None
    original_init = camera_class.__init__
    constructor = inspect.signature(original_init)
    if not {"image", "gt_alpha_mask"}.issubset(constructor.parameters):
        raise ValueError("unsupported external 2DGS Camera image constructor")
    original_decode = camera_utils_module.PILtoTorch
    missing = object()
    saved_properties = {name: vars(camera_class).get(name, missing)
                        for name in ("original_image", "gt_alpha_mask", "alpha_mask")}

    def compact_decode(image, resolution):
        if os.environ.get("GSW_TRAINING_IMAGE_PIPELINE") != "bounded":
            return original_decode(image, resolution)
        payload = HostCameraImage.from_pil(image, resolution, torch_module)
        if payload is None:
            return original_decode(image, resolution)
        if payload.alpha_mask is not None:
            return torch_module.cat((payload.rgb, payload.alpha_mask), dim=0)
        return payload.rgb

    def camera_init(camera, *args, **kwargs):
        if os.environ.get("GSW_TRAINING_IMAGE_PIPELINE") != "bounded":
            return original_init(camera, *args, **kwargs)
        bound = constructor.bind(camera, *args, **kwargs)
        image = bound.arguments["image"]
        mask = bound.arguments["gt_alpha_mask"]
        if image.dtype == torch_module.uint8:
            payload = HostCameraImage.from_tensors(image, mask, bit_depth=8, torch_module=torch_module)
        else:
            # Preserve the external Camera's existing clamp and float/HDR
            # semantics. Only explicitly eight-bit targets may be compacted.
            payload = HostCameraImage.from_tensors(image.clamp(0.0, 1.0), mask,
                                                  torch_module=torch_module)
        # Reuse the actual upstream metadata and CUDA transform constructor.
        # Its transforms depend on R/T/FoV, not image dimensions. A one-pixel
        # placeholder avoids allocating a full float RGB/alpha before packing.
        bound.arguments["image"] = torch_module.zeros((payload.rgb.shape[0], 1, 1), dtype=torch_module.float32)
        bound.arguments["gt_alpha_mask"] = None
        camera._gsw_image_payload = None
        original_init(*bound.args, **bound.kwargs)
        camera.image_width = int(payload.rgb.shape[2])
        camera.image_height = int(payload.rgb.shape[1])
        camera._gsw_image_payload = payload
        camera.__dict__.pop("original_image", None)
        camera.__dict__.pop("gt_alpha_mask", None)

    def rgb(camera):
        payload = getattr(camera, "_gsw_image_payload", None)
        return payload.rgb_float(torch_module) if payload is not None else camera.__dict__.get("original_image")

    def alpha(camera):
        payload = getattr(camera, "_gsw_image_payload", None)
        return payload.alpha_float(torch_module) if payload is not None else camera.__dict__.get("gt_alpha_mask")

    def gt_alpha(camera):
        # External evaluation/mesh callers expect the legacy target device.
        # Training uses the payload through prepare(), with bounded residency.
        value = alpha(camera)
        return value.to(camera.data_device) if value is not None else None

    camera_class.original_image = property(rgb, lambda camera, value: camera.__dict__.__setitem__("original_image", value))
    camera_class.gt_alpha_mask = property(gt_alpha, lambda camera, value: camera.__dict__.__setitem__("gt_alpha_mask", value))
    camera_class.alpha_mask = property(alpha)
    camera_class.__init__ = camera_init
    camera_class._gsw_native_image_adapter = camera_init
    camera_utils_module.PILtoTorch = compact_decode

    def restore():
        if camera_utils_module.PILtoTorch is compact_decode:
            camera_utils_module.PILtoTorch = original_decode
        if camera_class.__init__ is camera_init:
            camera_class.__init__ = original_init
            for name, value in saved_properties.items():
                if value is missing:
                    delattr(camera_class, name)
                else:
                    setattr(camera_class, name, value)
            delattr(camera_class, "_gsw_native_image_adapter")

    return restore


def total_host_bytes(cameras):
    """Actual CPU image-cache tensor storage, deduplicated across views/cameras.

    This is not process RSS, GPU memory or the pipeline's separate pinned pool.
    Native payloads are read directly, without materializing lazy float images.
    A legacy slice retaining a larger storage is charged for that whole storage.
    """
    storages = {}
    for camera in cameras:
        payload = camera if isinstance(camera, HostCameraImage) else getattr(camera, "_gsw_image_payload", None)
        if payload is not None:
            tensors = (payload.rgb, payload.alpha_mask, payload.invdepthmap, payload.depth_mask)
        else:
            alpha = getattr(camera, "alpha_mask", None)
            if alpha is None:
                alpha = getattr(camera, "gt_alpha_mask", None)
            tensors = (camera.original_image, alpha, getattr(camera, "invdepthmap", None),
                       getattr(camera, "depth_mask", None))
        for tensor in tensors:
            if tensor is not None and tensor.device.type == "cpu":
                storage = tensor.untyped_storage()
                if storage.nbytes():
                    storages[storage.data_ptr()] = storage.nbytes()
    return sum(storages.values())


@dataclass(frozen=True)
class ReadyImage:
    image: Any
    alpha_mask: Optional[Any] = None
    invdepthmap: Optional[Any] = None
    depth_mask: Optional[Any] = None


@dataclass
class _Transfer:
    source: Any
    payload: HostCameraImage
    ready: ReadyImage
    slot: int
    event: Optional[Any] = None
    start_event: Optional[Any] = None
    device_bytes: int = 0


class TrainingImagePipeline:
    """One/two owned target slots; callers release ReadyImage after each step.

    CUDA events protect pinned reuse and record_stream protects queued consumers.
    Returned tensors intentionally are not immutable copies: retaining them
    across steps keeps external allocations alive outside the owned-slot budget.
    stats() reports owned payload/output bytes, not CUDA allocator reservations.
    """
    def __init__(self, torch_module=None, device="cuda", config=None):
        if torch_module is None:
            import torch as torch_module
        self.torch = torch_module
        self.device = torch_module.device(device)
        self.config = config if config is not None else ImagePipelineConfig()
        self._transfers = OrderedDict()
        self._hits = 0
        self._loads = 0
        self._prefetch_skips = 0
        self._closed = False
        self._copy_stream = None
        self._pin_pool = None
        self._pin_failed = False
        self._normalizer = None
        self._last_start = None
        self._last_end = None
        self._h2d_bytes = 0
        self._unpinned_transfers = 0

    def _payload(self, camera):
        if isinstance(camera, HostCameraImage):
            return camera
        payload = getattr(camera, "_gsw_image_payload", None)
        if payload is not None:
            return payload
        def host(tensor):
            return tensor.detach().cpu() if tensor is not None else None
        return HostCameraImage(host(camera.original_image), host(getattr(camera, "alpha_mask", None)),
                               host(getattr(camera, "invdepthmap", None)),
                               host(getattr(camera, "depth_mask", None)))

    def _validate(self, payload):
        if payload.rgb.ndim != 3 or payload.rgb.shape[0] not in (1, 3):
            raise ValueError("camera RGB must have one or three CHW channels")
        size = tuple(payload.rgb.shape[1:])
        if min(size) <= 0:
            raise ValueError("camera image dimensions must be positive")
        for index, tensor in enumerate((payload.rgb, payload.alpha_mask, payload.invdepthmap, payload.depth_mask)):
            if tensor is None:
                continue
            if tensor.device.type != "cpu" or tensor.ndim != 3 or tuple(tensor.shape[1:]) != size:
                raise ValueError("camera image and auxiliary targets must share CPU CHW dimensions")
            if index and tensor.shape[0] != 1:
                raise ValueError("camera auxiliary targets must have one channel")
            if tensor.dtype not in (self.torch.uint8, self.torch.float32, self.torch.bool):
                raise ValueError("camera targets require float32, uint8 or boolean storage")
            if index >= 2 and tensor.dtype == self.torch.uint8:
                raise ValueError("depth values must not be reinterpreted as eight-bit image colors")
            if tensor.is_floating_point() and not bool(self.torch.isfinite(tensor).all()):
                raise ValueError("camera targets must be finite")

    def _evict(self, key):
        entry = self._transfers.pop(key)
        if entry.event is not None:
            entry.event.synchronize()

    def _stage(self, camera, keep=None):
        key = id(camera)
        if key in self._transfers:
            self._hits += 1
            self._transfers.move_to_end(key)
            return self._transfers[key]
        payload = self._payload(camera)
        self._validate(payload)
        tensors = (payload.rgb, payload.alpha_mask, payload.invdepthmap, payload.depth_mask)
        output_bytes = sum(tensor.numel() * 4 for tensor in tensors if tensor is not None)
        temporary_bytes = sum(tensor.numel() for tensor in tensors
                              if tensor is not None and tensor.dtype in (self.torch.uint8, self.torch.bool))
        if self.device.type == "cuda":
            peak = output_bytes + temporary_bytes + 4  # shared device normalization scalar
            if peak > self.config.device_bytes:
                if keep is not None:
                    self._prefetch_skips += 1
                    return None
                raise MemoryError("current training image exceeds the configured GPU staging budget")
            while sum(item.device_bytes for item in self._transfers.values()) + peak > self.config.device_bytes:
                victim = next((item for item in self._transfers if item != keep), None)
                if victim is None:
                    self._prefetch_skips += 1
                    return None
                self._evict(victim)
        if len(self._transfers) >= self.config.slots:
            victim = next((item for item in self._transfers if item != keep), None)
            if victim is None:
                return None
            self._evict(victim)
        used = {entry.slot for entry in self._transfers.values()}
        slot = next(index for index in range(self.config.slots) if index not in used)
        try:
            ready, event, start = self._convert(payload, slot)
        except self.torch.cuda.OutOfMemoryError:
            # Available headroom can change after the configured-budget check.
            # A partially copied optional image must not abort a valid step or
            # allow its pinned region to be reused before queued H2D completes.
            if self._copy_stream is not None:
                self._copy_stream.synchronize()
            if keep is None:
                raise
            self._prefetch_skips += 1
            return None
        entry = _Transfer(camera, payload, ready, slot, event, start,
                          output_bytes if self.device.type == "cuda" else 0)
        self._transfers[key] = entry
        self._loads += 1
        return entry

    def _convert(self, payload, slot):
        def normalized(tensor):
            if tensor is None:
                return None
            output = tensor.to(self.device).to(self.torch.float32)
            return output.div(255.0) if tensor.dtype == self.torch.uint8 else output
        if self.device.type != "cuda":
            return (ReadyImage(normalized(payload.rgb), normalized(payload.alpha_mask),
                               normalized(payload.invdepthmap), normalized(payload.depth_mask)), None, None)

        if self._copy_stream is None:
            self._copy_stream = self.torch.cuda.Stream(device=self.device)
        if self.config.pinned_bytes and self._pin_pool is None and not self._pin_failed:
            try:
                # One fixed allocation prevents a sequence of varying image
                # sizes from leaving an unbounded page-locked allocator cache.
                self._pin_pool = self.torch.empty(self.config.pinned_bytes,
                    dtype=self.torch.uint8, device="cpu", pin_memory=True)
            except RuntimeError:
                self._pin_failed = True

        tensors = (payload.rgb, payload.alpha_mask, payload.invdepthmap, payload.depth_mask)
        region = self.config.pinned_bytes // self.config.slots
        region -= region % 4
        offsets = []
        offset = 0
        for tensor in tensors:
            offset = (offset + 3) // 4 * 4
            offsets.append(offset)
            if tensor is not None:
                offset += tensor.numel() * tensor.element_size()
        pinned = self._pin_pool is not None and offset <= region
        if not pinned:
            self._unpinned_transfers += 1
        self._copy_stream.wait_stream(self.torch.cuda.current_stream(self.device))
        start = self.torch.cuda.Event(enable_timing=True)
        end = self.torch.cuda.Event(enable_timing=True)
        prepared = []
        with self.torch.cuda.stream(self._copy_stream):
            if self._normalizer is None:
                self._normalizer = self.torch.tensor(255.0, dtype=self.torch.float32, device=self.device)
            start.record(self._copy_stream)
            for index, tensor in enumerate(tensors):
                if tensor is None:
                    prepared.append(None)
                    continue
                source = tensor
                if pinned:
                    size = tensor.numel() * tensor.element_size()
                    source = self._pin_pool.narrow(0, slot * region + offsets[index], size)
                    source = source.view(tensor.dtype).view(tuple(tensor.shape))
                    source.copy_(tensor)
                raw = source.to(self.device, non_blocking=pinned)
                output = raw.to(self.torch.float32)
                if tensor.dtype == self.torch.uint8:
                    # A device scalar uses true division. PyTorch 2.0's CPU
                    # scalar CUDA fast path instead multiplies a reciprocal,
                    # which differs from legacy CPU /255 by one ULP at some
                    # byte values. Keep all 256 normalized targets bit-exact.
                    output.div_(self._normalizer)
                prepared.append(output)
                self._h2d_bytes += tensor.numel() * tensor.element_size()
            end.record(self._copy_stream)
        return ReadyImage(*prepared), end, start

    def prepare(self, camera, next_camera=None):
        if self._closed:
            raise RuntimeError("image pipeline is closed")
        if not self.config.enabled:
            payload = self._payload(camera)
            def legacy(tensor):
                if tensor is None:
                    return None
                # The old camera's float target contract and current-stream
                # transfer remain unchanged for full-state legacy resumes.
                source = tensor.float().div(255.0) if tensor.dtype == self.torch.uint8 else tensor
                return source.to(self.device)
            self._loads += 1
            return ReadyImage(legacy(payload.rgb), legacy(payload.alpha_mask),
                              legacy(payload.invdepthmap), legacy(payload.depth_mask))
        entry = self._stage(camera)
        if next_camera is not None and next_camera is not camera:
            if self.config.slots > 1:
                self._stage(next_camera, keep=id(camera))
            else:
                self._prefetch_skips += 1
        if entry.event is not None:
            consumer = self.torch.cuda.current_stream(self.device)
            consumer.wait_event(entry.event)
            for tensor in (entry.ready.image, entry.ready.alpha_mask,
                           entry.ready.invdepthmap, entry.ready.depth_mask):
                if tensor is not None:
                    tensor.record_stream(consumer)
            self._last_start, self._last_end = entry.start_event, entry.event
        return entry.ready

    def stats(self):
        elapsed = None
        if self._last_end is not None and self._last_end.query():
            elapsed = self._last_start.elapsed_time(self._last_end)
        return {"resident_slots": len(self._transfers),
                "active_host_bytes": sum(tensor.numel() * tensor.element_size()
                    for entry in self._transfers.values()
                    for tensor in (entry.payload.rgb, entry.payload.alpha_mask,
                                   entry.payload.invdepthmap, entry.payload.depth_mask)
                    if tensor is not None),
                "pinned_bytes": self._pin_pool.numel() if self._pin_pool is not None else 0,
                "device_bytes": sum(entry.device_bytes for entry in self._transfers.values()) +
                    (4 if self._normalizer is not None else 0),
                "cache_hits": self._hits, "transfers": self._loads,
                "prefetch_skips": self._prefetch_skips, "h2d_bytes": self._h2d_bytes,
                "unpinned_transfers": self._unpinned_transfers, "last_transfer_ms": elapsed}

    def close(self):
        for key in list(self._transfers):
            self._evict(key)
        if self._copy_stream is not None:
            self._copy_stream.synchronize()
        self._pin_pool = None
        self._normalizer = None
        self._last_start = self._last_end = None
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
