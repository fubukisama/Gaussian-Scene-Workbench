# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Public, CPU-tensor render/regularization compatibility checks."""
import builtins
import hashlib
import importlib.util
import math
import os
from pathlib import Path
import tempfile
import types
import unittest


# A controlled external-renderer dependency, not a replacement rasterizer. The
# literal image and maps make the observable objective/gradients independent of
# the adapter implementation. Production uses the pinned external 2DGS render.
_RENDERER_SOURCE = '''def render(camera, model, pipe, background):
    image = model[:, None, None].expand(3, 2, 2)
    alpha = model[0].expand(1, 2, 2)
    normal = model[:, None, None].expand(3, 2, 2)
    depth = model[1].expand(1, 2, 2)
    surface = depth_to_normal(camera, depth)
    surface = surface.permute(2, 0, 1)
    surface = surface * alpha.detach()
    return {"render": image, "rend_alpha": alpha, "rend_normal": normal,
            "rend_dist": model[2].expand(1, 2, 2), "surf_depth": depth,
            "surf_normal": surface, "radii": camera.radii,
            "visibility_filter": camera.visible, "viewspace_points": model}
'''


@unittest.skipUnless(importlib.util.find_spec("torch"), "Run the dedicated Torch CPU tensor lane")
class SurfelRenderAdapterTests(unittest.TestCase):
    def setUp(self):
        import torch
        self.torch = torch
        self.directory = tempfile.TemporaryDirectory(prefix="gsw-surfel-adapter-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "external_renderer.py"
        self.path.write_text(_RENDERER_SOURCE, encoding="utf-8")
        self.camera = types.SimpleNamespace(
            radii=torch.tensor([2, 0, 4]), visible=torch.tensor([True, False, True]))
        self.pipe = types.SimpleNamespace(depth_ratio=1.0)

    def renderer(self, normal_calculator):
        namespace = {"depth_to_normal": normal_calculator}
        exec(compile(self.path.read_bytes(), str(self.path), "exec"), namespace)
        return namespace["render"]

    def adapter(self, renderer):
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        return SurfelRenderAdapter(renderer, verified_source_hashes=(
            hashlib.sha256(_RENDERER_SOURCE.encode("utf-8")).hexdigest(),))

    def test_zero_normal_training_does_not_require_surface_normal_calculation(self):
        from native.worker.surfel_regularization import surfel_regularization
        torch = self.torch
        def unavailable_normal(camera, depth):
            raise AssertionError("An unrequested surface normal must not be computed")
        render = self.renderer(unavailable_normal)
        adapter = self.adapter(render)
        model = torch.tensor([.2, .3, .4], requires_grad=True)
        before_rng = torch.get_rng_state().clone()
        result = adapter(self.camera, model, self.pipe, None, surface_normal_required=False)
        normal, distortion = surfel_regularization(torch, result, 0.0, .3)
        total = result["render"].square().mean() + normal + distortion
        total.backward()
        self.assertNotIn("surf_normal", result)
        self.assertAlmostEqual(total.item(), .21666666666666667, places=6)
        torch.testing.assert_close(model.grad, torch.tensor([2 / 15, .2, 17 / 30]),
                                   rtol=1e-6, atol=1e-7)
        self.assertTrue(torch.equal(torch.get_rng_state(), before_rng))
        self.assertIs(result["radii"], self.camera.radii)
        self.assertIs(result["visibility_filter"], self.camera.visible)
        self.assertIs(render.__globals__["depth_to_normal"], unavailable_normal)

    def test_default_and_explicit_active_normal_outputs_and_gradients_are_stock(self):
        from native.worker.surfel_regularization import surfel_regularization
        torch = self.torch
        def normal_from_depth(camera, depth):
            return torch.stack((depth[0], depth[0] * 2, -depth[0]), dim=-1)
        render = self.renderer(normal_from_depth)
        adapter = self.adapter(render)
        self.assertTrue(adapter.capability["available"])
        for flags in ({}, {"surface_normal_required": True}):
            with self.subTest(flags=flags):
                candidate = torch.tensor([.2, .3, .4], requires_grad=True)
                reference = candidate.detach().clone().requires_grad_()
                result = adapter(self.camera, candidate, self.pipe, None, **flags)
                stock = render(self.camera, reference, self.pipe, None)
                self.assertEqual(set(result), set(stock))
                for key in result:
                    self.assertTrue(torch.equal(result[key], stock[key]), key)
                normal, distortion = surfel_regularization(torch, result, .2, .3)
                stock_normal, stock_distortion = surfel_regularization(torch, stock, .2, .3)
                total = result["render"].square().mean() + normal + distortion
                stock_total = stock["render"].square().mean() + stock_normal + stock_distortion
                self.assertAlmostEqual(total.item(), .4118666666666667, places=6)
                total.backward()
                stock_total.backward()
                torch.testing.assert_close(candidate.grad, reference.grad, rtol=0., atol=0.)
                self.assertIs(render.__globals__["depth_to_normal"], normal_from_depth)

    def test_omission_preserves_all_remaining_outputs_and_non_normal_gradients(self):
        from native.worker.surfel_regularization import surfel_regularization
        torch = self.torch
        def normal_from_depth(camera, depth):
            return torch.stack((depth[0], depth[0] * 2, -depth[0]), dim=-1)
        render = self.renderer(normal_from_depth)
        adapter = self.adapter(render)
        for distortion_weight in (0., .3):
            with self.subTest(distortion_weight=distortion_weight):
                candidate = torch.tensor([.2, .3, .4], requires_grad=True)
                reference = candidate.detach().clone().requires_grad_()
                before_rng = torch.get_rng_state().clone()
                result = adapter(self.camera, candidate, self.pipe, None, surface_normal_required=False)
                stock = render(self.camera, reference, self.pipe, None)
                self.assertEqual(set(result), set(stock) - {"surf_normal"})
                for key in result:
                    torch.testing.assert_close(result[key], stock[key], rtol=0., atol=0.)
                normal, distortion = surfel_regularization(torch, result, 0., distortion_weight)
                stock_normal, stock_distortion = surfel_regularization(torch, stock, 0., distortion_weight)
                (result["render"].square().mean() + normal + distortion).backward()
                (stock["render"].square().mean() + stock_normal + stock_distortion).backward()
                torch.testing.assert_close(candidate.grad, reference.grad, rtol=0., atol=0.)
                self.assertTrue(torch.equal(torch.get_rng_state(), before_rng))

    def test_installed_external_renderer_preserves_cpu_maps_and_gradients(self):
        """Original Python render, injected CUDA seam, real CPU Tensor/autograd.

        This optional lane does not invoke the CUDA rasterizer or claim a CUDA
        performance result. It exercises the original renderer's map processing
        and the same source/code pin that the actual runtime must satisfy.
        """
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        from native.worker.surfel_regularization import surfel_regularization
        torch = self.torch
        source = Path(os.environ.get("TWO_DGS_DIR", "D:/Apps/GSW/runtime/2dgs"))
        path = source / "gaussian_renderer/__init__.py"
        if not path.is_file():
            self.skipTest("The optional external 2DGS renderer source is not installed")
        original_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        compiled = compile(path.read_bytes(), str(path), "exec", dont_inherit=True)
        code = next(item for item in compiled.co_consts
                    if isinstance(item, types.CodeType) and item.co_name == "render")
        class CpuTorch:
            def __getattr__(self, name):
                return getattr(torch, name)
            def zeros_like(self, *args, **kwargs):
                kwargs["device"] = "cpu"
                return torch.zeros_like(*args, **kwargs)
        class Rasterizer:
            def __call__(self, **inputs):
                p = inputs["means3D"][0]
                grid = torch.arange(12, dtype=torch.float32).reshape(3, 4) / 50
                maps = torch.stack((p[0] + 1 + grid, p[0] + .4 + grid,
                                    p[0] + grid, p[1] - grid, p[2] + grid,
                                    p[1] + 2 + grid, p[2] + grid), dim=0)
                return p[:, None, None].expand(3, 3, 4), torch.tensor([2]), maps
        def normal_from_depth(camera, depth):
            return torch.stack((depth[0], depth[0] * 2, -depth[0]), dim=-1)
        namespace = {"__builtins__": builtins.__dict__, "torch": CpuTorch(), "math": math,
                     "GaussianRasterizationSettings": lambda **values: values,
                     "GaussianRasterizer": lambda raster_settings: Rasterizer(),
                     "depth_to_normal": normal_from_depth}
        render = types.FunctionType(code, namespace, "render", (1.0, None))
        adapter = SurfelRenderAdapter(render)
        self.assertTrue(adapter.capability["available"], adapter.capability)
        camera = types.SimpleNamespace(
            FoVx=.7, FoVy=.5, image_height=3, image_width=4,
            world_view_transform=torch.tensor([[0., -1., 0., 0.], [1., 0., 0., 0.],
                                              [0., 0., 1., 0.], [0., 0., 0., 1.]]),
            full_proj_transform=torch.eye(4), camera_center=torch.zeros(3))
        def model(parameters):
            return types.SimpleNamespace(
                get_xyz=parameters.reshape(1, 3), get_opacity=torch.ones(1, 1),
                get_scaling=torch.ones(1, 2), get_rotation=torch.tensor([[1., 0., 0., 0.]]),
                get_features=torch.ones(1, 16, 3), active_sh_degree=3)
        for depth_ratio in (0., .5, 1.):
            for normal_weight, distortion_weight in ((0., 0.), (0., .3), (.2, 0.), (.2, .3)):
                with self.subTest(depth_ratio=depth_ratio, normal_weight=normal_weight,
                                  distortion_weight=distortion_weight):
                    candidate = torch.tensor([.2, .3, .4], requires_grad=True)
                    reference = candidate.detach().clone().requires_grad_()
                    pipe = types.SimpleNamespace(compute_cov3D_python=False, depth_ratio=depth_ratio)
                    result = adapter(camera, model(candidate), pipe, torch.zeros(3),
                                     surface_normal_required=normal_weight != 0.)
                    stock = render(camera, model(reference), pipe, torch.zeros(3))
                    expected_keys = set(stock) - ({"surf_normal"} if normal_weight == 0. else set())
                    self.assertEqual(set(result), expected_keys)
                    for key in result:
                        torch.testing.assert_close(result[key], stock[key], rtol=0., atol=0.)
                    normal, distortion = surfel_regularization(torch, result, normal_weight, distortion_weight)
                    stock_normal, stock_distortion = surfel_regularization(torch, stock, normal_weight, distortion_weight)
                    total = result["render"].square().mean() + normal + distortion
                    stock_total = stock["render"].square().mean() + stock_normal + stock_distortion
                    torch.testing.assert_close(total, stock_total, rtol=0., atol=0.)
                    total.backward()
                    stock_total.backward()
                    torch.testing.assert_close(candidate.grad, reference.grad, rtol=0., atol=0.)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), original_hash)


class SurfelRenderVerificationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gsw-surfel-source-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "external_renderer.py"

    def load(self, source):
        self.path.write_text(source, encoding="utf-8")
        namespace = {"depth_to_normal": lambda camera, depth: depth}
        exec(compile(self.path.read_bytes(), str(self.path), "exec"), namespace)
        return namespace["render"]

    def test_unknown_renderer_version_falls_back_to_stock_for_an_omission_request(self):
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        render = self.load('def render(*args):\n    return {"surf_normal": "stock"}\n')
        adapter = SurfelRenderAdapter(render)
        self.assertFalse(adapter.capability["available"])
        self.assertEqual(adapter.capability["reason"], "surfel_renderer_source_unverified")
        self.assertEqual(adapter(surface_normal_required=False), {"surf_normal": "stock"})

    def test_callable_non_function_falls_back_to_stock(self):
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        class Renderer:
            def __call__(self):
                return {"surf_normal": "stock callable"}
        adapter = SurfelRenderAdapter(Renderer())
        self.assertFalse(adapter.capability["available"])
        self.assertEqual(adapter.capability["reason"], "surfel_renderer_not_python_function")
        self.assertEqual(adapter(surface_normal_required=False), {"surf_normal": "stock callable"})

    def test_pinned_file_does_not_authorize_a_different_live_code_object(self):
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        source = 'def render(*args):\n    return {"surf_normal": "stock"}\n'
        render = self.load(source)
        changed = compile('def render(*args):\n    return {"surf_normal": "changed"}\n',
                          str(self.path), "exec")
        render.__code__ = next(item for item in changed.co_consts if isinstance(item, types.CodeType))
        adapter = SurfelRenderAdapter(render, verified_source_hashes=(
            hashlib.sha256(source.encode("utf-8")).hexdigest(),))
        self.assertFalse(adapter.capability["available"])
        self.assertEqual(adapter.capability["reason"], "surfel_renderer_code_unverified")
        self.assertEqual(adapter(surface_normal_required=False), {"surf_normal": "changed"})

    def test_renderer_replaced_after_verification_falls_back_without_substitution(self):
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        render = self.load(_RENDERER_SOURCE)
        adapter = SurfelRenderAdapter(render, verified_source_hashes=(
            hashlib.sha256(_RENDERER_SOURCE.encode("utf-8")).hexdigest(),))
        self.assertTrue(adapter.capability["available"])
        changed = compile('def render(*args):\n    return {"surf_normal": "new version"}\n',
                          str(self.path), "exec")
        render.__code__ = next(item for item in changed.co_consts if isinstance(item, types.CodeType))
        self.assertEqual(adapter(surface_normal_required=False), {"surf_normal": "new version"})
        self.assertFalse(adapter.capability["available"])


if __name__ == "__main__":
    unittest.main()
