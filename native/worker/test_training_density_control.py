"""Density regressions: narrow camera baselines must not erase wide geometry."""
import unittest
from types import SimpleNamespace


class DensityControlTests(unittest.TestCase):
    def setUp(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run with a training Python for density tensor tests")
        self.torch = torch
        from native.worker.training_density_control import NativeDensityControl
        self.control_class = NativeDensityControl

    def model(self, scales=2, count=100):
        torch = self.torch
        x = torch.linspace(-100, 100, count)
        model = SimpleNamespace(
            get_xyz=torch.stack((x, x * .5, x * .25), dim=1),
            get_scaling=torch.full((count, scales), 2.),
            get_opacity=torch.full((count, 1), .8),
            xyz_gradient_accum=torch.zeros(count, 1), denom=torch.ones(count, 1),
            max_radii2D=torch.zeros(count), tmp_radii=None)
        model.densify_and_clone = lambda *args: None
        model.densify_and_split = lambda *args: None
        def prune(mask):
            model.removed = mask.clone()
        model.prune_points = prune
        return model

    def test_geometry_extent_does_not_change_clone_split_extent(self):
        for backend, scales in (("2dgs", 2), ("3dgs", 3)):
            with self.subTest(backend=backend):
                model = self.model(scales)
                seen = []
                model.densify_and_clone = lambda *args: seen.append(args[2])
                model.densify_and_split = lambda *args: seen.append(args[2])
                control = self.control_class(self.torch, model, 5.678, backend)
                control.densify_and_prune(model, .00025, .05, 20, 3100, 3000,
                                          radii=self.torch.ones(100))
                self.assertEqual(seen, [5.678, 5.678])
                self.assertEqual(int(model.removed.sum()), 0)
                self.assertGreater(control.world_size_limit, 9.)
                self.assertIsNone(model.tmp_radii)

    def test_over_pruning_is_limited_but_bad_opacity_is_still_removed(self):
        model = self.model()
        control = self.control_class(self.torch, model, 5., "2dgs")
        model.get_opacity[:60] = .0001
        events = []
        control.emit = lambda *args: events.append(args)
        control.densify_and_prune(model, .00025, .05, 20, 3500, 3000)
        self.assertEqual(int(model.removed.sum()), 20)
        self.assertTrue(model.removed[:20].all())
        self.assertFalse(model.removed[60:].any())
        self.assertEqual(events[0][1]["deferred"], 40)
        model.get_opacity[:60] = .8
        model.get_opacity[:5] = .0001
        control.densify_and_prune(model, .00025, .05, 20, 3600, 3000)
        self.assertEqual(int(model.removed.sum()), 5)

    def test_reset_recovery_is_finite_and_survives_checkpoint(self):
        model = self.model()
        control = self.control_class(self.torch, model, 5., "2dgs")
        model.get_opacity[:10] = .01
        control.densify_and_prune(model, .00025, .05, 20, 3100, 3000)
        self.assertEqual(int(model.removed.sum()), 0)
        control.densify_and_prune(model, .00025, .05, 20, 3400, 3000)
        self.assertEqual(int(model.removed.sum()), 10)
        restored = self.control_class(self.torch, self.model(), 1., "2dgs")
        restored.restore(control.capture())
        self.assertEqual(restored.capture(), control.capture())
        with self.assertRaises(ValueError):
            restored.restore(dict(control.capture(), version=-1))
        with self.assertRaises(ValueError):
            restored.restore(dict(control.capture(), world_size_limit=float("nan")))

    def test_invalid_geometry_is_not_preserved_by_the_budget(self):
        model = self.model()
        control = self.control_class(self.torch, model, 5., "2dgs")
        model.get_scaling[:60] = float("nan")
        control.densify_and_prune(model, .00025, .05, 20, 3500, 3000)
        self.assertEqual(int(model.removed.sum()), 60)

    def test_excessive_size_and_screen_pruning_are_not_disabled(self):
        model = self.model()
        control = self.control_class(self.torch, model, 5., "2dgs")
        model.get_scaling[:5] = 1000.
        model.max_radii2D[5:10] = 100.
        control.densify_and_prune(model, .00025, .05, 20, 3500, 3000)
        self.assertEqual(int(model.removed.sum()), 10)
        self.assertTrue(model.removed[:10].all())


if __name__ == "__main__":
    unittest.main()
