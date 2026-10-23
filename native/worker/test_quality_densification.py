# SPDX-License-Identifier: MIT
import unittest
try:
    import torch
except ImportError:
    torch = None
from native.worker.quality_densification import DensificationStatistics


@unittest.skipIf(torch is None, 'real Torch required')
class DensificationTests(unittest.TestCase):
    def test_abs_split_retains_precancellation_signal_without_changing_signed_gradient(self):
        signed = torch.tensor([[0., 0., 0.], [2., 0., 0.]])
        original = signed.clone()
        stats = DensificationStatistics(torch, 'abs_gs', 2, signed.device, 1.)
        stats.accumulate(signed, torch.tensor([True, True]), absolute_screen_gradient=torch.tensor([[4., 3.], [2., 0.]]))
        clone, split = stats.gradients()
        torch.testing.assert_close(clone[:, 0], torch.tensor([0., 2.]))
        torch.testing.assert_close(split[:, 0], torch.tensor([5., 2.]))
        torch.testing.assert_close(signed, original)

    def test_pixel_weights_are_accepted_pixels_not_view_counts_and_depth_is_relative(self):
        grad = torch.tensor([[2., 0., 0.], [2., 0., 0.]])
        for units in (1., 1000.):
            stats = DensificationStatistics(torch, 'pixel_gs', 2, grad.device, 10. * units, .5)
            stats.accumulate(grad, torch.ones(2, dtype=torch.bool),
                             contributor_pixels=torch.tensor([1, 3]), camera_z=torch.tensor([2.5, 5.]) * units)
            stats.accumulate(torch.tensor([[4., 0., 0.], [4., 0., 0.]]), torch.ones(2, dtype=torch.bool),
                             contributor_pixels=torch.tensor([3, 1]), camera_z=torch.tensor([2.5, 5.]) * units)
            clone, split = stats.gradients()
            torch.testing.assert_close(clone[:, 0], torch.tensor([.875, 2.5]))
            torch.testing.assert_close(split, clone)
            self.assertEqual(stats.state_dict()['denominator'].tolist(), [[4], [4]])

    def test_state_restores_pending_statistics_and_topology_resets_explicitly(self):
        stats = DensificationStatistics(torch, 'abs_gs', 1, 'cpu', 2.)
        stats.accumulate(torch.tensor([[1., 0., 0.]]), torch.tensor([True]),
                         absolute_screen_gradient=torch.tensor([[2., 0.]]))
        restored = DensificationStatistics(torch, 'abs_gs', 1, 'cpu', 2.)
        restored.load_state_dict(stats.state_dict(), 1)
        for a, b in zip(stats.gradients(), restored.gradients()):
            torch.testing.assert_close(a, b)
        restored.reset(3)
        self.assertEqual(restored.gradients()[0].shape, (3, 1))
        self.assertFalse(bool(restored.gradients()[0].any()))
        with self.assertRaisesRegex(ValueError, 'checkpoint'):
            restored.load_state_dict(stats.state_dict(), 3)

    def test_fake_coverage_and_reduced_abs_are_not_admitted(self):
        stats = DensificationStatistics(torch, 'pixel_gs', 1, 'cpu', 1.)
        with self.assertRaisesRegex(ValueError, 'contributor'):
            stats.accumulate(torch.ones(1, 3), torch.tensor([True]),
                             contributor_pixels=torch.tensor([4.]), camera_z=torch.ones(1))
        abs_stats = DensificationStatistics(torch, 'abs_gs', 1, 'cpu', 1.)
        with self.assertRaisesRegex(ValueError, 'absolute'):
            abs_stats.accumulate(torch.ones(1, 3), torch.tensor([True]))


if __name__ == '__main__':
    unittest.main()
