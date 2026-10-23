"""Actual GaussianModel/Adam CPU opacity reset; no CUDA training claim."""
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

try:
    import torch
except ImportError:
    torch = None


@unittest.skipIf(torch is None, 'real Torch and model runtime required')
class MipOpacityResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2] / 'gaussian-splatting'
        sys.path.insert(0, str(root))
        spec = importlib.util.spec_from_file_location('gsw_owned_cpu_opacity_model', root / 'scene/gaussian_model.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        cls.model_type = module.GaussianModel

    def fixture(self, log_scale, width, logits=(0.,)):
        from native.worker.quality_mip import MipState
        model = self.model_type(0)
        model._scaling = torch.nn.Parameter(torch.full((len(logits), 3), float(log_scale)))
        model._opacity = torch.nn.Parameter(torch.tensor(logits, dtype=torch.float32)[:, None])
        model.optimizer = torch.optim.Adam([{'params': [model._opacity], 'name': 'opacity'}], lr=0.)
        model._opacity.sum().backward()
        model.optimizer.step()
        model.optimizer.zero_grad(set_to_none=True)
        mip = MipState(torch)
        mip.widths = torch.full((len(logits), 1), float(width))
        return SimpleNamespace(torch=torch, model=model, mip=mip, is_mcmc=False)

    def test_underflow_keeps_finite_raw_logits_and_real_optimizer_binding(self):
        from native.worker.quality_training_runtime import QualityTrainingController
        controller = self.fixture(-50., .25, (-1000., 0., 1000.))
        model = controller.model
        before = model._opacity.detach().clone()
        old = model._opacity
        step = model.optimizer.state[old]['step'].clone()
        QualityTrainingController.reset_opacity(controller)
        self.assertTrue(bool(torch.isfinite(model._opacity).all()))
        torch.testing.assert_close(model._opacity, before, rtol=0, atol=0)
        self.assertIs(model.optimizer.param_groups[0]['params'][0], model._opacity)
        self.assertNotIn(old, model.optimizer.state)
        state = model.optimizer.state[model._opacity]
        torch.testing.assert_close(state['step'], step, rtol=0, atol=0)
        self.assertFalse(bool(state['exp_avg'].any() or state['exp_avg_sq'].any()))

    def test_normal_reset_caps_effective_alpha_without_reducing_already_small_alpha(self):
        from native.worker.quality_training_runtime import QualityTrainingController
        controller = self.fixture(-2., .001)
        QualityTrainingController.reset_opacity(controller)
        unused, active = controller.mip.active(controller.model.get_scaling, controller.model.get_opacity)
        self.assertLessEqual(active.item(), .01000001)
        self.assertAlmostEqual(active.item(), .01, places=7)
        low = self.fixture(-5., .1)
        before = low.model._opacity.detach().clone()
        QualityTrainingController.reset_opacity(low)
        torch.testing.assert_close(low.model._opacity, before, rtol=0, atol=0)

    def test_invalid_filter_does_not_replace_parameter_or_optimizer_state(self):
        from native.worker.quality_training_runtime import QualityTrainingController
        controller = self.fixture(-2., float('nan'))
        old = controller.model._opacity
        state = {key: value.clone() for key, value in controller.model.optimizer.state[old].items()}
        with self.assertRaisesRegex(ValueError, 'quality_mip_attributes_invalid'):
            QualityTrainingController.reset_opacity(controller)
        self.assertIs(controller.model._opacity, old)
        for key, value in state.items():
            torch.testing.assert_close(controller.model.optimizer.state[old][key], value, rtol=0, atol=0)


if __name__ == '__main__':
    unittest.main()
