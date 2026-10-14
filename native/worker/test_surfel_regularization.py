# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Public regularization values/gradients; all initial gates use CPU tensors."""
import importlib.util
import unittest

from native.worker.surfel_regularization import surfel_regularization


@unittest.skipUnless(importlib.util.find_spec("torch"), "Run the dedicated Torch CPU tensor lane")
class SurfelRegularizationTests(unittest.TestCase):
    def test_inactive_terms_do_not_require_maps_or_add_a_gradient_graph(self):
        import torch
        image = torch.full((3, 2, 2), .5, requires_grad=True)
        normal, distortion = surfel_regularization(torch, {"render": image}, 0., 0.)
        for term in (normal, distortion):
            self.assertEqual(term.shape, torch.Size([]))
            self.assertEqual(term.dtype, image.dtype)
            self.assertEqual(term.device, image.device)
            self.assertEqual(term.item(), 0.)
            self.assertIsNone(term.grad_fn)
            self.assertFalse(term.requires_grad)
        (image.square().mean() + normal + distortion).backward()
        self.assertTrue(torch.equal(image.grad, torch.full_like(image, 1. / 12)))

    def test_distortion_only_does_not_require_normals(self):
        import torch
        image = torch.full((3, 1, 2), .5, requires_grad=True)
        distance = torch.tensor([[[.2, .4]]], requires_grad=True)
        normal, distortion = surfel_regularization(torch, {"render": image, "rend_dist": distance}, 0., .3)
        self.assertEqual(normal.item(), 0.)
        self.assertIsNone(normal.grad_fn)
        self.assertAlmostEqual(distortion.item(), .09, places=7)
        (image.square().mean() + normal + distortion).backward()
        self.assertTrue(torch.equal(distance.grad, torch.full_like(distance, .15)))

    def test_normal_only_does_not_require_distortion(self):
        import torch
        image = torch.full((3, 1, 2), .5, requires_grad=True)
        rendered = torch.tensor([[[.2, .4]], [[.5, .5]], [[0., 0.]]], requires_grad=True)
        surface = torch.tensor([[[1., 1.]], [[0., 0.]], [[0., 0.]]], requires_grad=True)
        normal, distortion = surfel_regularization(torch,
            {"render": image, "rend_normal": rendered, "surf_normal": surface}, .2, 0.)
        self.assertAlmostEqual(normal.item(), .14, places=7)
        self.assertEqual(distortion.item(), 0.)
        self.assertIsNone(distortion.grad_fn)
        (image.square().mean() + normal + distortion).backward()
        torch.testing.assert_close(rendered.grad,
            torch.tensor([[[-.1, -.1]], [[0., 0.]], [[0., 0.]]]), rtol=0., atol=0.)
        torch.testing.assert_close(surface.grad,
            torch.tensor([[[-.02, -.04]], [[-.05, -.05]], [[0., 0.]]]), rtol=1e-6, atol=1e-8)

    def test_all_weight_combinations_preserve_stock_values_gradients_inputs_and_rng(self):
        import torch
        for normal_weight, distortion_weight in ((0., 0.), (0., .3), (.2, 0.), (.2, .3)):
            with self.subTest(normal=normal_weight, distortion=distortion_weight):
                # Both branches are driven by real model parameters. The photo
                # loss supplies a nonzero gradient to every parameter even when
                # both regularizers are absent. No mocked autograd collaborators.
                current = torch.tensor([.2, .3, .4], requires_grad=True)
                stock = current.detach().clone().requires_grad_()
                current.grad = torch.full_like(current, .07)
                stock.grad = torch.full_like(stock, .07)
                def maps(parameters):
                    return {
                        "render": parameters[:, None, None].expand(3, 2, 3),
                        "rend_normal": parameters[:, None, None].expand(3, 2, 3),
                        "surf_normal": parameters.flip(0)[:, None, None].expand(3, 2, 3),
                        "rend_dist": parameters.sum().expand(1, 2, 3) / 10,
                    }
                candidate = maps(current)
                reference = maps(stock)
                original_maps = {name: value.detach().clone() for name, value in candidate.items()}
                rng_before = torch.get_rng_state().clone()
                normal, distortion = surfel_regularization(torch, candidate, normal_weight, distortion_weight)
                self.assertTrue(torch.equal(current.grad, torch.full_like(current, .07)))
                photo_gradient = torch.autograd.grad(candidate["render"].square().mean(), current,
                                                     retain_graph=True)[0]
                self.assertTrue(bool((photo_gradient != 0).all()))
                # Frozen stock 2DGS objective is the independent compatibility
                # oracle. Preserve multiplication/reduction order, not merely an
                # algebraically equivalent float32 expression.
                stock_normal = normal_weight * (
                    1 - (reference["rend_normal"] * reference["surf_normal"]).sum(dim=0))[None].mean()
                stock_distortion = distortion_weight * reference["rend_dist"].mean()
                self.assertTrue(torch.equal(normal, stock_normal))
                self.assertTrue(torch.equal(distortion, stock_distortion))
                (candidate["render"].square().mean() + distortion + normal).backward()
                (reference["render"].square().mean() + stock_distortion + stock_normal).backward()
                torch.testing.assert_close(current.grad, stock.grad, rtol=0., atol=0.)
                self.assertTrue(torch.equal(torch.get_rng_state(), rng_before))
                for name, value in candidate.items():
                    self.assertTrue(torch.equal(value, original_maps[name]), name)


if __name__ == "__main__":
    unittest.main()
