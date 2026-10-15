# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Opt-in tiny real-CUDA surfel renderer compatibility gate.

GSW_SURFEL_RENDER_CUDA=1 enables this module in the configured 2DGS Python.
TWO_DGS_DIR selects its original source tree. No images/files from a project,
optimizer checkpoints, training process or GUI are opened. The scene contains
only 32 synthetic surfels and a 48x32 Camera. This is not a speed benchmark.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import unittest


@unittest.skipUnless(os.environ.get("GSW_SURFEL_RENDER_CUDA") == "1",
                     "Opt-in tiny real CUDA surfel-renderer gate")
class SurfelRenderCudaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import numpy as np
        import torch
        if not torch.cuda.is_available():
            raise AssertionError("Real CUDA is required for this explicitly enabled gate")
        source = Path(os.environ.get("TWO_DGS_DIR", "D:/Apps/GSW/runtime/2dgs")).resolve()
        if not (source / "gaussian_renderer/__init__.py").is_file():
            raise AssertionError("TWO_DGS_DIR must select the real 2DGS runtime source")
        os.environ.setdefault("MPLBACKEND", "Agg")
        cls.original_path = list(sys.path)
        sys.path.insert(0, str(source))
        from gaussian_renderer import render
        from scene.gaussian_model import GaussianModel
        from scene.cameras import Camera
        from utils.sh_utils import RGB2SH
        cls.torch, cls.np, cls.GaussianModel = torch, np, GaussianModel
        cls.stock_render = staticmethod(render)
        cls.source = source
        cls.source_path = source / "gaussian_renderer/__init__.py"
        cls.source_sha256 = hashlib.sha256(cls.source_path.read_bytes()).hexdigest()
        cls.rgb_to_sh = staticmethod(RGB2SH)
        cls.camera = Camera(0, np.eye(3, dtype=np.float32),
            np.array([0., 0., 3.], dtype=np.float32), .7, .5,
            torch.full((3, 32, 48), .45), None, "synthetic-view", 0, data_device="cpu")
        cls.background = torch.tensor([.1, .15, .2], dtype=torch.float32, device="cuda")
        cls.report = {"version": 1, "backend": "2dgs", "width": 48, "height": 32,
                      "points": 32, "source_sha256": cls.source_sha256, "cases": {}}

    @classmethod
    def tearDownClass(cls):
        cls.torch.cuda.synchronize()
        if hashlib.sha256(cls.source_path.read_bytes()).hexdigest() != cls.source_sha256:
            raise AssertionError("The external renderer source changed during the test")
        output = os.environ.get("GSW_SURFEL_RENDER_CUDA_REPORT")
        if output:
            Path(output).write_text(json.dumps(cls.report, indent=2), encoding="utf-8")
        sys.path[:] = cls.original_path

    def model(self):
        torch, np = self.torch, self.np
        rng = np.random.RandomState(71)
        positions = np.array([[x * .24 - .36, y * .24 - .36, z * .12 - .06]
                              for x in range(4) for y in range(4) for z in range(2)], dtype=np.float32)
        positions += rng.uniform(-.015, .015, positions.shape).astype(np.float32)
        colors = torch.tensor(rng.uniform(.2, .8, (32, 3)), dtype=torch.float32, device="cuda")
        model = self.GaussianModel(3)
        model.active_sh_degree = 3
        model._xyz = torch.nn.Parameter(torch.tensor(positions, device="cuda"))
        model._features_dc = torch.nn.Parameter(self.rgb_to_sh(colors)[:, None, :].contiguous())
        model._features_rest = torch.nn.Parameter(torch.tensor(
            rng.uniform(-.01, .01, (32, 15, 3)), dtype=torch.float32, device="cuda"))
        model._scaling = torch.nn.Parameter(torch.full((32, 2), math.log(.09), device="cuda"))
        rotation = torch.zeros((32, 4), device="cuda")
        rotation[:, 0] = 1.
        rotation[:, 1] = torch.linspace(-.1, .1, 32, device="cuda")
        model._rotation = torch.nn.Parameter(rotation)
        model._opacity = torch.nn.Parameter(torch.full((32, 1), .4, device="cuda"))
        return model

    def observation(self, render, normal_weight=.2, distortion_weight=.3, flags=None):
        from types import SimpleNamespace
        from native.worker.surfel_regularization import surfel_regularization
        torch = self.torch
        model = self.model()
        pipe = SimpleNamespace(compute_cov3D_python=False, convert_SHs_python=False,
                               depth_ratio=.5, debug=False)
        before_cpu_rng = torch.get_rng_state().clone()
        before_cuda_rng = torch.cuda.get_rng_state_all()
        result = render(self.camera, model, pipe, self.background, **(flags or {}))
        normal, distortion = surfel_regularization(torch, result, normal_weight, distortion_weight)
        total = (result["render"] - .45).square().mean() + normal + distortion
        self.assertTrue(bool(torch.isfinite(total)))
        self.assertGreater(int(result["visibility_filter"].sum()), 0)
        values = {key: value.detach().cpu().clone() for key, value in result.items()}
        total.backward()
        torch.cuda.synchronize()
        self.assertTrue(torch.equal(torch.get_rng_state(), before_cpu_rng))
        for previous, current in zip(before_cuda_rng, torch.cuda.get_rng_state_all()):
            self.assertTrue(torch.equal(previous, current))
        gradients = {}
        for name in ("_xyz", "_features_dc", "_features_rest", "_scaling", "_rotation", "_opacity"):
            grad = getattr(model, name).grad
            self.assertIsNotNone(grad, name)
            self.assertTrue(bool(torch.isfinite(grad).all()), name)
            gradients[name] = grad.detach().cpu().clone()
        self.assertIsNotNone(result["viewspace_points"].grad)
        gradients["viewspace_points"] = result["viewspace_points"].grad.detach().cpu().clone()
        return {"maps": values, "gradients": gradients, "total_loss": total.item()}

    def compare(self, reference, candidate, missing_surface_normal=False):
        torch = self.torch
        expected = set(reference["maps"]) - ({"surf_normal"} if missing_surface_normal else set())
        self.assertEqual(set(candidate["maps"]), expected)
        maximum_map_delta = 0.
        for name in candidate["maps"]:
            left, right = reference["maps"][name], candidate["maps"][name]
            if left.dtype == torch.bool:
                self.assertTrue(torch.equal(left, right), name)
            else:
                maximum_map_delta = max(maximum_map_delta, float((left - right).abs().max()))
                torch.testing.assert_close(left, right, rtol=0., atol=0., msg=name)
        self.assertEqual(reference["total_loss"], candidate["total_loss"])
        maximum_gradient_delta = 0.
        for name, expected_gradient in reference["gradients"].items():
            actual = candidate["gradients"][name]
            maximum_gradient_delta = max(maximum_gradient_delta,
                                         float((expected_gradient - actual).abs().max()))
            # CUDA atomic accumulation is not a bitwise trajectory guarantee.
            # The stock-repeat test measures the same tolerance first.
            torch.testing.assert_close(actual, expected_gradient, rtol=5e-5, atol=2e-6, msg=name)
        return {"max_map_abs_delta": maximum_map_delta,
                "max_gradient_abs_delta": maximum_gradient_delta,
                "total_loss": candidate["total_loss"]}

    def test_01_stock_repeat_public_fidelity_baseline(self):
        """Record actual rasterizer-repeat noise before testing the adapter."""
        self.torch.cuda.synchronize()
        starting_bytes = self.torch.cuda.memory_allocated()
        self.torch.cuda.reset_peak_memory_stats()
        reference = self.observation(self.stock_render)
        repeat = self.observation(self.stock_render)
        result = self.compare(reference, repeat)
        result["scene_peak_extra_bytes"] = self.torch.cuda.max_memory_allocated() - starting_bytes
        self.assertLess(result["scene_peak_extra_bytes"], 64 * 1024 * 1024)
        self.report["cases"]["stock_repeat_baseline"] = result
        print("[gsw-surfel-cuda-baseline] " + json.dumps(result, sort_keys=True), flush=True)

    def test_02_adapter_keeps_real_maps_objective_and_parameter_gradients(self):
        from native.worker.surfel_render_adapter import SurfelRenderAdapter
        adapter = SurfelRenderAdapter(self.stock_render)
        self.assertTrue(adapter.capability["available"], adapter.capability)
        self.report["adapter_capability"] = adapter.capability
        for name, normal_weight, distortion_weight, flag in (
                ("default_active_normal_distortion", .2, .3, None),
                ("explicit_active_normal_distortion", .2, .3, True),
                ("omitted_zero_normal_active_distortion", 0., .3, False),
                ("omitted_zero_normal_zero_distortion", 0., 0., False)):
            with self.subTest(case=name):
                # Omission is requested only while the actual normal weight is
                # zero. Active normal/distortion terms always retain stock data.
                self.assertTrue(flag is not False or normal_weight == 0.)
                flags = {} if flag is None else {"surface_normal_required": flag}
                reference = self.observation(self.stock_render, normal_weight, distortion_weight)
                candidate = self.observation(adapter, normal_weight, distortion_weight, flags)
                result = self.compare(reference, candidate, missing_surface_normal=flag is False)
                self.report["cases"][name] = result
                print("[gsw-surfel-cuda-adapter] " + json.dumps(dict(case=name, **result), sort_keys=True),
                      flush=True)


if __name__ == "__main__":
    unittest.main()
