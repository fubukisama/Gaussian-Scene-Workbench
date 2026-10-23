# SPDX-License-Identifier: MIT
import unittest
from unittest import mock
from pathlib import Path
import numpy as np
try:
    import torch
except ImportError:
    torch = None
from native.worker.quality_training_runtime import masked_photometric, quality_kernel_root


class QualityKernelSelectionTests(unittest.TestCase):
    def test_explicit_environment_and_separate_default_component(self):
        with mock.patch.dict('os.environ', {'GSW_QUALITY_KERNEL_ROOT': 'D:/owned/quality-runtime'}):
            self.assertEqual(quality_kernel_root('D:/owned/explicit'), str(Path('D:/owned/explicit').resolve()))
            self.assertEqual(quality_kernel_root(), str(Path('D:/owned/quality-runtime').resolve()))
        with mock.patch.dict('os.environ', {}, clear=True), mock.patch(
                'native.worker.quality_training_runtime.sys.executable',
                'D:/owned/runtime/envs/training/python.exe'):
            self.assertEqual(Path(quality_kernel_root()), Path('D:/owned/runtime/kernels/quality3d-v1'))


@unittest.skipIf(torch is None, 'real Torch required')
class MaskedPhotometricTests(unittest.TestCase):
    def test_unobserved_pixels_cannot_change_objective_or_receive_gradient(self):
        weights = np.zeros((25, 27), np.float32)
        weights[3:22, 4:23] = 1
        target = torch.zeros(3, 25, 27)
        image = torch.full_like(target, .2, requires_grad=True)
        altered = image.detach().clone()
        altered[:, weights == 0] = float('nan')
        altered.requires_grad_(True)
        good = masked_photometric(torch, image, target, weights, .2)
        result = masked_photometric(torch, altered, target, weights, .2)
        torch.testing.assert_close(result[0], good[0])
        result[0].backward()
        self.assertTrue(bool(torch.isfinite(altered.grad).all()))
        self.assertFalse(bool(altered.grad[:, weights == 0].any()))
        self.assertGreater(result[2]['ssim_windows'], 0)

    def test_no_complete_window_is_explicit_l1_not_global_ssim(self):
        weights = np.zeros((5, 7), np.float32)
        weights[2, 3] = 1
        image = torch.zeros(3, 5, 7, requires_grad=True)
        target = torch.ones_like(image, requires_grad=False)
        loss, l1, report = masked_photometric(torch, image, target, weights, .2)
        self.assertEqual(float(loss), 1.)
        self.assertEqual(float(l1), 1.)
        self.assertEqual(report['reason'], 'no_complete_foreground_windows_l1_only')


if __name__ == '__main__':
    unittest.main()
