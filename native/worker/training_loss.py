# SPDX-FileCopyrightText: 2026 fubukisama
# SPDX-License-Identifier: MIT
"""Shared optional photometric loss; surfel depth/normal terms stay outside it."""
import math
import importlib
import hashlib
from pathlib import Path

FUSED_SSIM_UPSTREAM_COMMIT = "1272e21a282342e89537159e4bad508b19b34157"


def validate_loss_weight(weight):
    if isinstance(weight, bool) or not isinstance(weight, (int, float)) or not math.isfinite(weight) or not 0 <= weight <= 1:
        raise ValueError("invalid_photometric_loss_weight")
    return float(weight)


class PhotometricLoss:
    def __init__(self, torch, l1=None, ssim=None, mode="stock"):
        if mode not in ("stock", "fused_l1_ssim"):
            raise ValueError("invalid_photometric_loss_mode")
        self.torch, self.l1, self.ssim = torch, l1, ssim
        self.mode = mode
        self._module = None
        self._buffers = []
        self._busy = False
        self._effective = "stock"
        self._reason = ""
        self._proof = {}
        if mode == "fused_l1_ssim":
            try:
                if not torch.cuda.is_available():
                    raise RuntimeError("fused_loss_cuda_unavailable")
                module = importlib.import_module("gsw_training_loss_cuda._C")
                self._proof = verify_loss_extension(torch, module)
                self._module = module
            except Exception as error:
                self._reason = "fused_loss_runtime_or_numerical_gate_failed"
                self._proof = {"ready": False, "detail": str(error)}
        self._function = self._autograd_function()

    def status(self):
        return {"requested": self.mode, "effective": self._effective,
                "reason": self._reason, "proof": dict(self._proof),
                "workspaceBytes": sum(buffer.numel() * buffer.element_size() for buffer in self._buffers)}

    def compute(self, image, target, weight):
        weight = validate_loss_weight(weight)
        if image.shape != target.shape or image.ndim not in (3, 4) or image.numel() == 0:
            raise ValueError("photometric_loss_image_contract")
        supported = (self._module is not None and image.is_cuda and image.dtype == self.torch.float32
                     and target.device == image.device and not target.requires_grad
                     and target.dtype in (self.torch.float32, self.torch.uint8) and not self._busy)
        if supported:
            prediction = image.unsqueeze(0) if image.ndim == 3 else image
            truth = target.unsqueeze(0) if target.ndim == 3 else target
            prediction, truth = prediction.contiguous(), truth.contiguous()
            required = workspace_sizes(prediction.shape)
            for index, count in enumerate(required):
                if index >= len(self._buffers):
                    self._buffers.append(self.torch.empty(count, device=image.device, dtype=self.torch.float32))
                elif self._buffers[index].numel() < count or self._buffers[index].device != image.device:
                    self._buffers[index] = self.torch.empty(count, device=image.device, dtype=self.torch.float32)
            self._busy = bool(image.requires_grad and self.torch.is_grad_enabled())
            self._effective, self._reason = "fused_l1_ssim", ""
            try:
                return self._function.apply(prediction, truth, weight, self)
            except Exception:
                self._busy = False
                raise
        self._effective = "stock"
        if self.mode == "fused_l1_ssim" and self._module is not None:
            self._reason = "fused_loss_input_not_supported"
        truth = target.to(dtype=image.dtype) / 255 if target.dtype == self.torch.uint8 else target
        value = self.l1(image, truth) if self.l1 else self.torch.abs(image - truth).mean()
        if weight == 0:
            return value, value.detach()
        similarity = self.ssim(image, truth) if self.ssim else reference_ssim(self.torch, image, truth)
        return (1-weight) * value + weight * (1-similarity), value.detach()

    def _autograd_function(self):
        torch = self.torch

        class CombinedLoss(torch.autograd.Function):
            @staticmethod
            def forward(ctx, image, target, weight, owner):
                ctx.owner, ctx.weight = owner, weight
                ctx.save_for_backward(image, target, *owner._buffers)
                scalar = owner._module.forward(image, target, weight, owner._buffers)
                loss, l1 = scalar[0], scalar[1]
                ctx.mark_non_differentiable(l1)
                return loss, l1

            @staticmethod
            def backward(ctx, grad_loss, _grad_l1):
                image, target, *buffers = ctx.saved_tensors
                try:
                    gradient = ctx.owner._module.backward(image, target, ctx.weight,
                        grad_loss.contiguous(), buffers)
                    return gradient, None, None, None
                finally:
                    ctx.owner._busy = False

        return CombinedLoss


def workspace_sizes(shape):
    n, c, h, w = map(int, shape)
    pixels, values = n * h * w, n * c * h * w
    blocks = min((pixels + 255) // 256, 1024)
    return (values, values, values, values, blocks * 2, 2, values)


def reference_ssim(torch, image, target):
    """Independent Gaussian-window definition used for the numerical gate."""
    if image.ndim == 3:
        image, target = image.unsqueeze(0), target.unsqueeze(0)
    coefficients = [math.exp(-((x-5)**2)/(2*1.5**2)) for x in range(11)]
    window = image.new_tensor(coefficients)
    window = window / window.sum()
    kernel = (window[:, None] * window[None, :])[None, None]
    kernel = kernel.expand(image.shape[1], 1, 11, 11).contiguous()
    conv = lambda value: torch.nn.functional.conv2d(value, kernel, padding=5, groups=image.shape[1])
    mu_x, mu_y = conv(image), conv(target)
    var_x, var_y = conv(image*image)-mu_x.square(), conv(target*target)-mu_y.square()
    covariance = conv(image*target)-mu_x*mu_y
    return (((2*mu_x*mu_y+.0001)*(2*covariance+.0009)) /
            ((mu_x.square()+mu_y.square()+.0001)*(var_x+var_y+.0009))).mean()


def verify_loss_extension(torch, module):
    """Actual CUDA values/gradients; deterministic samples do not advance RNG."""
    if (getattr(module, "SOURCE_COMMIT", None) != FUSED_SSIM_UPSTREAM_COMMIT
            or getattr(module, "LICENSE_ID", None) != "MIT"
            or getattr(module, "KERNEL_ABI", None) != 2):
        raise RuntimeError("fused_loss_extension_provenance_not_compatible")
    max_loss_error, max_gradient_error = 0., 0.
    for target_bytes in (False, True):
        for weight in (0., .2, 1.):
            image = torch.linspace(.15, .85, 3*12*13, device="cuda").reshape(1, 3, 12, 13)
            target = image.flip(-1).contiguous()
            if target_bytes:
                target = (target * 255).round().to(torch.uint8)
            reference_target = target.float()/255 if target_bytes else target
            reference = image.detach().clone().requires_grad_()
            l1 = (reference-reference_target).abs().mean()
            loss = (1-weight)*l1 + weight*(1-reference_ssim(torch, reference, reference_target))
            loss.backward()
            buffers = [torch.empty(count, device="cuda", dtype=torch.float32)
                       for count in workspace_sizes(image.shape)]
            values = module.forward(image, target, weight, buffers)
            gradient = module.backward(image, target, weight, image.new_tensor(1.), buffers)
            torch.cuda.synchronize()
            loss_error = abs(float(values[0])-float(loss.detach()))
            gradient_error = float((gradient-reference.grad).abs().max())
            if not bool(torch.isfinite(gradient).all()) or loss_error > 1e-5 or gradient_error > 2e-6:
                raise RuntimeError("fused_loss_numerical_mismatch: loss={} gradient={}".format(loss_error, gradient_error))
            max_loss_error = max(max_loss_error, loss_error)
            max_gradient_error = max(max_gradient_error, gradient_error)
    # Exact uint8-normalized predictions must keep L1 sign(0)=0. A division
    # implementation with a slightly different FP32 rounding can otherwise
    # pass ordinary error tests but assign a full 1/N gradient to equal pixels.
    target = torch.arange(3*5*7, device="cuda").reshape(1, 3, 5, 7).to(torch.uint8)
    image = target.float()/255
    buffers = [torch.empty(count, device="cuda", dtype=torch.float32)
               for count in workspace_sizes(image.shape)]
    values = module.forward(image, target, 0., buffers)
    gradient = module.backward(image, target, 0., image.new_tensor(1.), buffers)
    torch.cuda.synchronize()
    if float(values[0]) != 0 or not bool((gradient == 0).all()):
        raise RuntimeError("fused_loss_uint8_zero_difference_gradient_mismatch")
    return {"ready": True, "upstreamCommit": FUSED_SSIM_UPSTREAM_COMMIT,
            "license": "MIT", "kernelAbi": 2, "samePadding": True,
            "kernelVersion": getattr(module, "KERNEL_VERSION", "unknown"),
            "extensionSha256": hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest(),
            "partials": "float32", "targetTypes": ["float32", "uint8"],
            "maxLossError": max_loss_error, "maxGradientError": max_gradient_error}
