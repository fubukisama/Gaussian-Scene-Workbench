# SPDX-License-Identifier: MIT
import copy
import unittest
try:
    import torch
except ImportError:
    torch = None
from native.worker.quality_mip import training_filter, apply_filter, filter_2d, MipState, MipError


@unittest.skipIf(torch is None, 'real Torch required')
class MipTests(unittest.TestCase):
    def setUp(self):
        self.xyz = torch.tensor([[0., 0., 2.], [.1, .2, 3.]], dtype=torch.float64)
        self.camera = dict(K=[[80., 0., 50.], [0., 80., 30.], [0., 0., 1.]],
                           w2c=torch.eye(4).tolist(), width=100, height=60, znear=.01, split='train')

    def test_sampling_uses_actual_full_k_and_scales_with_scene_units(self):
        widths, evidence = training_filter(torch, self.xyz, [self.camera])
        self.assertAlmostEqual(widths[0].item(), .2 ** .5 * 2 / 80)
        skew = copy.deepcopy(self.camera)
        skew['K'][0][1] = 60.
        changed, _ = training_filter(torch, self.xyz, [skew])
        self.assertTrue(bool((changed < widths).all()))
        unit_camera = copy.deepcopy(self.camera)
        unit_camera['znear'] *= 1000
        scaled, _ = training_filter(torch, self.xyz * 1000, [unit_camera])
        torch.testing.assert_close(scaled, widths * 1000)
        self.assertEqual(evidence['visible'], 2)

    def test_training_only_and_no_visibility_are_not_silently_accepted(self):
        invalid = dict(self.camera, split='test')
        with self.assertRaisesRegex(MipError, 'training_views_only'):
            training_filter(torch, self.xyz, [invalid])
        with self.assertRaisesRegex(MipError, 'no_visible'):
            training_filter(torch, -self.xyz, [self.camera])

    def test_filter_preserves_integral_and_has_raw_parameter_gradients(self):
        scales = torch.tensor([[.02, .03, .04]], dtype=torch.float64, requires_grad=True)
        opacity = torch.tensor([[.6]], dtype=torch.float64, requires_grad=True)
        widths = torch.tensor([[.01]], dtype=torch.float64)
        active_s, active_o = apply_filter(torch, scales, opacity, widths)
        torch.testing.assert_close(active_o * active_s.prod(1, keepdim=True), opacity * scales.prod(1, keepdim=True))
        self.assertTrue(torch.autograd.gradcheck(lambda s, o: apply_filter(torch, s, o, widths), (scales, opacity)))
        covariance = torch.tensor([[[.4, .1], [.1, .7]]], dtype=torch.float64, requires_grad=True)
        o2 = opacity.reshape(-1)
        filtered, adjusted = filter_2d(torch, covariance, o2)
        torch.testing.assert_close(adjusted * torch.linalg.det(filtered).sqrt(), o2 * torch.linalg.det(covariance).sqrt())
        self.assertTrue(torch.autograd.gradcheck(lambda c, o: filter_2d(torch, c, o), (covariance, o2)))

    def test_checkpoint_does_not_recompute_filter_from_evaluation_view(self):
        state = MipState(torch)
        state.update(self.xyz, [self.camera], 3)
        captured = state.state_dict()
        restored = MipState(torch)
        restored.load_state_dict(captured, self.xyz)
        captured['widths'].zero_()
        self.assertTrue(bool((restored.widths > 0).all()))
        with self.assertRaisesRegex(MipError, 'checkpoint_invalid'):
            restored.load_state_dict(state.state_dict(), self.xyz[:1])


if __name__ == '__main__':
    unittest.main()
