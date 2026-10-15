# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Opt-in, test-owned CUDA optimizer-surgery and checkpoint equivalence gate.

Set GSW_CAPACITY_CUDA_TEST=1 and use the configured Graphdeco training Python.
The allocator is capped at 64 MiB; only 32 synthetic Gaussian rows are used.
No rasterizer training, input photos, original models or user pickles are read.
"""
import io
import os
import unittest


@unittest.skipUnless(os.environ.get("GSW_CAPACITY_CUDA_TEST") == "1",
                     "Opt-in tiny CUDA capacity/Adam acceptance")
class CapacityCudaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import torch
        import diff_gaussian_rasterization
        from native.worker import test_training_capacity_storage as fixtures
        cls.torch, cls.module, cls.fixtures = torch, diff_gaussian_rasterization, fixtures
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is required for the explicitly enabled capacity gate")
        total = torch.cuda.get_device_properties(0).total_memory
        torch.cuda.set_per_process_memory_fraction((64 * 1024 ** 2) / total, 0)
        torch.cuda.reset_peak_memory_stats()

    def model(self, kind):
        torch, fixture = self.torch, self.fixtures
        model = fixture.make_model(rows=32)
        groups, values = [], {}
        for old in model.optimizer.param_groups:
            value = torch.nn.Parameter(old["params"][0].detach().cuda())
            values[old["name"]] = value
            groups.append({"name": old["name"], "params": [value], "lr": old["lr"]})
        cls = torch.optim.Adam if kind == "standard" else self.module.SparseGaussianAdam
        model.optimizer = cls(groups, lr=.01, eps=1e-15)
        fixture.publish(model, values)
        model.max_radii2D = model.max_radii2D.cuda()
        model.xyz_gradient_accum = model.xyz_gradient_accum.cuda()
        model.denom = model.denom.cuda()
        self.step(model, kind)
        return model

    def step(self, model, kind, gradients=True):
        torch = self.torch
        rows = model.optimizer.param_groups[0]["params"][0].shape[0]
        for group in model.optimizer.param_groups:
            value = group["params"][0]
            value.grad = (torch.arange(value.numel(), dtype=torch.float32, device="cuda")
                          .reshape_as(value).remainder(17).div(32).add(.125)) if gradients else None
        if kind == "standard":
            model.optimizer.step()
        else:
            visible = (torch.arange(rows, device="cuda").remainder(3) != 1).contiguous()
            model.optimizer.step(visible, rows)
        model.optimizer.zero_grad(set_to_none=True)

    def extensions(self, model, rows):
        torch = self.torch
        return {group["name"]: torch.full((rows,) + tuple(group["params"][0].shape[1:]),
                                         .375, dtype=torch.float32, device="cuda")
                for group in model.optimizer.param_groups}

    def run_route(self, kind, pooled):
        from native.worker.training_capacity_storage import install_capacity_storage
        torch, fixture = self.torch, self.fixtures
        model = self.model(kind)
        manager = (install_capacity_storage(torch, model, "3dgs", max_reserve_bytes=16384,
                    growth_fraction=.5, chunk_bytes=256) if pooled else None)
        if pooled:
            self.assertIsNotNone(manager, getattr(model, "_gsw_capacity_storage_status", None))
        before_rng = torch.cuda.get_rng_state().clone()
        fixture.publish(model, model.cat_tensors_to_optimizer(self.extensions(model, 5)))
        for group in model.optimizer.param_groups:
            self.assertIsNone(group["params"][0].grad)
        self.step(model, kind, gradients=False)
        self.step(model, kind)
        mask = torch.arange(37, device="cuda").remainder(4) != 1
        fixture.publish(model, model._prune_optimizer(mask))
        self.step(model, kind, gradients=False)
        self.step(model, kind)
        rows = model._xyz.shape[0]
        reset = torch.full((rows, 1), -.75, dtype=torch.float32, device="cuda")
        fixture.publish(model, model.replace_tensor_to_optimizer(reset, "opacity"))
        self.step(model, kind, gradients=False)
        self.step(model, kind)
        fixture.publish(model, model.cat_tensors_to_optimizer(self.extensions(model, 0)))
        self.assertTrue(torch.equal(before_rng, torch.cuda.get_rng_state()))
        checkpoint = io.BytesIO()
        capture = model.capture()
        self.assertIs(capture[1], model._xyz)
        torch.save(capture, checkpoint)
        checkpoint.seek(0)
        state = torch.load(checkpoint)
        restored = self.model(kind)
        values = dict(zip(("xyz", "f_dc", "f_rest", "scaling", "rotation", "opacity"), state[1:7]))
        fixture.publish(restored, values)
        for group in restored.optimizer.param_groups:
            group["params"][0] = values[group["name"]]
        restored.optimizer.state.clear()
        restored.optimizer.load_state_dict(state[10])
        if pooled:
            self.assertIsNotNone(install_capacity_storage(torch, restored, "3dgs", max_reserve_bytes=16384,
                                  growth_fraction=.5, chunk_bytes=256))
        for group in restored.optimizer.param_groups:
            value = group["params"][0]
            self.assertIsInstance(value, torch.nn.Parameter)
            self.assertIsNone(value.grad)
            self.assertTrue(value.is_leaf and value.is_contiguous())
            self.assertEqual("cuda", value.device.type)
        self.step(restored, kind)
        torch.cuda.synchronize()
        result = {}
        for group in restored.optimizer.param_groups:
            value = group["params"][0]
            result[group["name"]] = value.detach().cpu().clone()
            for key, tensor in restored.optimizer.state[value].items():
                result[group["name"] + "/" + key] = tensor.detach().cpu().clone()
        if pooled:
            self.assertLessEqual(manager.snapshot()["spare_bytes"], 16384)
        self.assertLess(self.torch.cuda.max_memory_allocated(), 64 * 1024 ** 2)
        return result

    def check_kind(self, kind):
        torch = self.torch
        first = self.run_route(kind, False)
        repeat = self.run_route(kind, False)
        pooled = self.run_route(kind, True)
        baseline = max(float((first[key] - repeat[key]).abs().max()) for key in first)
        delta = max(float((first[key] - pooled[key]).abs().max()) for key in first)
        self.assertLessEqual(delta, max(1e-7, baseline * 2), (kind, delta, baseline))
        print("capacity_cuda {} delta={} stock_repeat={} torch_peak_bytes={}".format(
            kind, delta, baseline, torch.cuda.max_memory_allocated()))

    def test_standard_adam_real_cuda_surgery_and_checkpoint(self):
        self.check_kind("standard")

    def test_canonical_sparse_adam_real_cuda_surgery_and_checkpoint(self):
        self.check_kind("sparse")

    def test_subclass_of_canonical_sparse_is_not_implicitly_supported(self):
        from native.worker.training_capacity_storage import install_capacity_storage
        model = self.model("sparse")
        class UnknownAdam(self.module.SparseGaussianAdam):
            pass
        model.optimizer = UnknownAdam(model.optimizer.param_groups, lr=.01, eps=1e-15)
        self.assertIsNone(install_capacity_storage(self.torch, model, "3dgs"))

    def test_known_dense_f_standard_adam_one_time_normalization_keeps_next_cuda_steps(self):
        from native.worker.training_capacity_storage import install_capacity_storage
        torch, fixture = self.torch, self.fixtures
        for initialized in (False, True):
            results, normalization = [], None
            for pooled in (False, False, True):
                model = self.model("standard")
                fixture.dense_f_xyz(model, initialized)
                self.assertEqual((1, 32), model._xyz.stride())
                if pooled:
                    manager = install_capacity_storage(torch, model, "3dgs",
                        max_reserve_bytes=16384, growth_fraction=.5, chunk_bytes=256)
                    self.assertIsNotNone(manager, model._gsw_capacity_storage_status)
                    normalization = manager.snapshot()["layout_normalization"]
                    self.assertEqual(1, normalization["events"])
                    self.assertEqual(3 if initialized else 1, normalization["copies"])
                    self.assertTrue(model._xyz.is_contiguous())
                self.step(model, "standard")
                self.step(model, "standard")
                result = {}
                for group in model.optimizer.param_groups:
                    parameter = group["params"][0]
                    result[group["name"]] = parameter.detach().cpu().clone()
                    for key, value in model.optimizer.state[parameter].items():
                        result[group["name"] + "/" + key] = value.detach().cpu().clone()
                results.append(result)
            first, repeat, pooled = results
            baseline = max(float((first[key] - repeat[key]).abs().max()) for key in first)
            delta = max(float((first[key] - pooled[key]).abs().max()) for key in first)
            self.assertLessEqual(delta, max(1e-7, baseline * 2), (initialized, baseline, delta))
            print("capacity_cuda dense_f initialized={} delta={} stock_repeat={} normalization={}".format(
                initialized, delta, baseline, normalization))

    def test_dense_f_sparse_layout_keeps_stock_until_visibility_equivalence_is_verified(self):
        from native.worker.training_capacity_storage import install_capacity_storage
        model = self.model("sparse")
        self.fixtures.dense_f_xyz(model)
        original, parameter = model.cat_tensors_to_optimizer, model._xyz
        self.assertIsNone(install_capacity_storage(self.torch, model, "3dgs"))
        self.assertEqual("sparse_adam_layout_not_verified", model._gsw_capacity_storage_status["reason"])
        self.assertEqual(original, model.cat_tensors_to_optimizer)
        self.assertIs(parameter, model._xyz)


if __name__ == "__main__":
    unittest.main()
