"""Public loss contract and real tensor/gradient tests in selected runtimes."""
import unittest
import importlib.util
import math
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest import mock

from native.worker.training_loss import validate_loss_weight, PhotometricLoss


class LossPolicyTests(unittest.TestCase):
    def test_unknown_loss_mode_is_not_silently_enabled(self):
        with self.assertRaises(ValueError):
            PhotometricLoss(None, mode="typo")

    def test_invalid_loss_weight_is_rejected_without_clamping(self):
        for value in (-.1, 1.1, float("nan"), float("inf"), True, "0.2"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    validate_loss_weight(value)
        self.assertEqual(validate_loss_weight(0), 0.)
        self.assertEqual(validate_loss_weight(.2), .2)
        self.assertEqual(validate_loss_weight(1), 1.)


@unittest.skipUnless(importlib.util.find_spec("torch"), "selected QA Python has no Torch; run the dedicated tensor lane")
class TensorLossTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import torch
        cls.torch = torch
        root = Path(__file__).resolve().parents[2] / "gaussian-splatting"
        sys.path.insert(0, str(root))
        from utils.loss_utils import l1_loss, ssim
        cls.l1 = staticmethod(l1_loss)
        cls.ssim = staticmethod(ssim)

    def test_fused_public_loss_alternates_dimensions_without_capacity_becoming_gradient_shape(self):
        """CPU external-device/kernel boundary reproduces the actual ABI view.

        This does not certify CUDA arithmetic; real-kernel acceptance runs in
        the dedicated training environment. The mock retains the native ABI's
        buffers[6].view_as(image), which exposed the production failure.
        """
        from native.worker.training_loss import reference_ssim, FUSED_SSIM_UPSTREAM_COMMIT
        torch = self.torch

        class CudaBoundaryTensor(torch.Tensor):
            @property
            def is_cuda(self):
                return True

        class CpuCudaBoundary:
            cuda = SimpleNamespace(is_available=lambda: True, synchronize=lambda: None)

            def __getattr__(self, name):
                return getattr(torch, name)

            def _create(self, name, *args, **kwargs):
                if str(kwargs.get("device", "")).startswith("cuda"):
                    kwargs["device"] = "cpu"
                return getattr(torch, name)(*args, **kwargs).as_subclass(CudaBoundaryTensor)

            def linspace(self, *args, **kwargs):
                return self._create("linspace", *args, **kwargs)

            def arange(self, *args, **kwargs):
                return self._create("arange", *args, **kwargs)

            def empty(self, *args, **kwargs):
                return self._create("empty", *args, **kwargs)

        class ExternalKernelBoundary:
            SOURCE_COMMIT = FUSED_SSIM_UPSTREAM_COMMIT
            LICENSE_ID, KERNEL_ABI, KERNEL_VERSION = "MIT", 2, "cpu-abi-contract-only"
            __file__ = __file__

            @staticmethod
            def forward(image, target, weight, buffers):
                # External CUDA execution is represented by the independently
                # defined CPU objective; workspace sizing/view ABI is exact.
                with torch.enable_grad():
                    reference = image.detach().clone().requires_grad_()
                    truth = target.float() / 255 if target.dtype == torch.uint8 else target
                    l1 = (reference - truth).abs().mean()
                    loss = (1 - weight) * l1 + weight * (1 - reference_ssim(torch, reference, truth))
                    gradient = torch.autograd.grad(loss, reference)[0]
                with torch.no_grad():
                    buffers[6][:image.numel()].copy_(gradient.flatten())
                    buffers[5][:2].copy_(torch.stack((loss.detach(), l1.detach())))
                return buffers[5]

            @staticmethod
            def backward(image, target, weight, grad_loss, buffers):
                return buffers[6].view_as(image) * grad_loss

        threads = torch.get_num_threads()
        torch.set_num_threads(1)
        self.addCleanup(torch.set_num_threads, threads)
        with mock.patch("native.worker.training_loss.importlib.import_module", return_value=ExternalKernelBoundary):
            implementation = PhotometricLoss(CpuCudaBoundary(), self.l1, self.ssim, mode="fused_l1_ssim")
        self.assertTrue(implementation.status()["proof"]["ready"])
        for shape in ((3, 4, 5), (3, 6, 7), (3, 5, 4), (3, 4, 5),
                      (2, 3, 3, 4), (3, 3, 4), (3, 6, 7)):
            for target_bytes in (False, True):
                for weight in (0., .2, 1.):
                    with self.subTest(shape=shape, bytes=target_bytes, weight=weight):
                        image = torch.linspace(.12, .88, math.prod(shape)).reshape(shape).as_subclass(CudaBoundaryTensor).detach().requires_grad_()
                        target = image.detach().flip(-1).contiguous()
                        if target_bytes:
                            target = (target * 255).round().to(torch.uint8)
                        truth = target.float() / 255 if target_bytes else target
                        reference = image.detach().clone().requires_grad_()
                        expected = (1-weight)*self.l1(reference, truth) + weight*(1-reference_ssim(torch, reference, truth))
                        (2.7 * expected).backward()
                        loss, _ = implementation.compute(image, target, weight)
                        (2.7 * loss).backward()
                        self.assertEqual(implementation.status()["effective"], "fused_l1_ssim")
                        self.assertLess(float((image.grad-reference.grad).abs().max()), 2e-6)
                        self.assertLess(abs(float(loss.detach()) - float(expected.detach())), 1e-6)

    def test_cuda_combined_loss_and_gradient_match_current_same_padding_objective(self):
        torch = self.torch
        if not torch.cuda.is_available() or importlib.util.find_spec("gsw_training_loss_cuda") is None:
            self.skipTest("dedicated optional CUDA loss runtime is not available")
        image = torch.linspace(.15, .85, 3 * 16 * 17, device="cuda").reshape(3, 16, 17).requires_grad_()
        target = image.detach().flip(-1).contiguous()
        reference = image.detach().clone().requires_grad_()
        baseline_l1 = self.l1(reference, target)
        baseline = .8 * baseline_l1 + .2 * (1 - self.ssim(reference, target))
        baseline.backward()
        implementation = PhotometricLoss(torch, self.l1, self.ssim, mode="fused_l1_ssim")
        loss, l1 = implementation.compute(image, target, .2)
        loss.backward()
        torch.cuda.synchronize()
        self.assertEqual(implementation.status()["effective"], "fused_l1_ssim")
        self.assertLess(abs(float(loss.detach()) - float(baseline.detach())), 1e-5)
        self.assertLess(abs(float(l1.detach()) - float(baseline_l1.detach())), 1e-6)
        self.assertLess(float((image.grad-reference.grad).abs().max()), 2e-6)

    def test_cuda_uint8_small_batched_and_zero_difference_pixels_preserve_values(self):
        torch = self.torch
        if not torch.cuda.is_available() or importlib.util.find_spec("gsw_training_loss_cuda") is None:
            self.skipTest("dedicated optional CUDA loss runtime is not available")
        implementation = PhotometricLoss(torch, self.l1, self.ssim, mode="fused_l1_ssim")
        for shape in ((2, 3, 5, 7), (1, 3, 16, 17)):
            for weight in (0., .2, 1.):
                with self.subTest(shape=shape, weight=weight):
                    count = math.prod(shape)
                    target = torch.arange(count, device="cuda").reshape(shape).remainder(251).to(torch.uint8)
                    original_target = target.clone()
                    image = (target.float()/255).detach()
                    image[..., 1:, 1:] *= .8
                    image.requires_grad_()
                    reference = image.detach().clone().requires_grad_()
                    truth = target.float()/255
                    baseline_l1 = self.l1(reference, truth)
                    baseline = (1-weight)*baseline_l1 + weight*(1-self.ssim(reference, truth))
                    baseline.backward()
                    loss, l1 = implementation.compute(image, target, weight)
                    loss.backward()
                    torch.cuda.synchronize()
                    self.assertEqual(implementation.status()["effective"], "fused_l1_ssim")
                    self.assertLess(abs(float(loss.detach())-float(baseline.detach())), 1e-5)
                    self.assertLess(abs(float(l1.detach())-float(baseline_l1.detach())), 1e-6)
                    self.assertLess(float((image.grad-reference.grad).abs().max()), 3e-6)
                    self.assertTrue(torch.equal(target, original_target))

    def test_cuda_noncontiguous_prediction_and_auxiliary_terms_keep_the_gradient_graph(self):
        torch = self.torch
        if not torch.cuda.is_available() or importlib.util.find_spec("gsw_training_loss_cuda") is None:
            self.skipTest("dedicated optional CUDA loss runtime is not available")
        image = torch.linspace(.1, .9, 16*17*3, device="cuda").reshape(16, 17, 3).permute(2, 0, 1).requires_grad_()
        target = image.detach().flip(-1)
        reference = image.detach().clone().requires_grad_()
        baseline = .8*self.l1(reference, target) + .2*(1-self.ssim(reference, target))
        (2.7*baseline + .13*reference.square().mean()).backward()
        implementation = PhotometricLoss(torch, self.l1, self.ssim, mode="fused_l1_ssim")
        loss, _ = implementation.compute(image, target, .2)
        (2.7*loss + .13*image.square().mean()).backward()
        torch.cuda.synchronize()
        self.assertEqual(implementation.status()["effective"], "fused_l1_ssim")
        self.assertLess(float((image.grad-reference.grad).abs().max()), 4e-6)

    def test_numerical_gate_preserves_training_rng_and_stock_mode_is_exact(self):
        torch = self.torch
        cpu_rng = torch.get_rng_state().clone()
        cuda_rng = torch.cuda.get_rng_state_all() if torch.cuda.is_available() else []
        implementation = PhotometricLoss(torch, self.l1, self.ssim, mode="fused_l1_ssim")
        self.assertTrue(torch.equal(torch.get_rng_state(), cpu_rng))
        for before, after in zip(cuda_rng, torch.cuda.get_rng_state_all() if cuda_rng else []):
            self.assertTrue(torch.equal(before, after))
        stock = PhotometricLoss(torch, self.l1, self.ssim, mode="stock")
        image = torch.linspace(.1, .8, 3*12*13).reshape(3, 12, 13).requires_grad_()
        target = image.detach().flip(-1)
        loss, value = stock.compute(image, target, .2)
        expected = .8*self.l1(image, target)+.2*(1-self.ssim(image, target))
        self.assertTrue(torch.equal(loss, expected))
        self.assertEqual(stock.status()["effective"], "stock")
        self.assertTrue(torch.equal(value, self.l1(image, target)))


if __name__ == "__main__":
    unittest.main()
