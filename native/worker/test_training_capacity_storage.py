# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""The public optimizer-surgery seam, exercised with real FP32 Torch tensors."""
import ast
import io
from pathlib import Path
import unittest

try:
    import torch
except ImportError:
    torch = None

from native.worker.training_capacity_storage import install_capacity_storage


def make_model(backend="3dgs", rows=4):
    # Execute only the checked-in upstream public surgery methods: no CUDA
    # constructor, rasterizer import, copied algorithm or synthetic optimizer.
    source = Path(__file__).resolve().parents[2] / "gaussian-splatting/scene/gaussian_model.py"
    tree = ast.parse(source.read_text(encoding="utf-8"))
    methods = {"cat_tensors_to_optimizer", "_prune_optimizer",
               "replace_tensor_to_optimizer", "capture"}
    cls = next(node for node in tree.body if isinstance(node, ast.ClassDef)
               and node.name == "GaussianModel")
    cls.body = [node for node in cls.body if isinstance(node, ast.FunctionDef)
                and node.name in methods]
    module = ast.fix_missing_locations(ast.Module(body=[cls], type_ignores=[]))
    namespace = {"torch": torch, "nn": torch.nn}
    exec(compile(module, str(source), "exec"), namespace)
    model = namespace["GaussianModel"]()
    fields = (("xyz", "_xyz", (3,)), ("f_dc", "_features_dc", (1, 3)),
              ("f_rest", "_features_rest", (3, 3)), ("opacity", "_opacity", (1,)),
              ("scaling", "_scaling", (3,) if backend == "3dgs" else (2,)),
              ("rotation", "_rotation", (4,)))
    groups = []
    for name, attribute, tail in fields:
        width = 1
        for dimension in tail:
            width *= dimension
        value = torch.arange(rows * width, dtype=torch.float32).reshape((rows,) + tail) / 10
        parameter = torch.nn.Parameter(value)
        setattr(model, attribute, parameter)
        groups.append({"name": name, "params": [parameter], "lr": .01})
    model.optimizer = torch.optim.Adam(groups, eps=1e-15)
    for group in model.optimizer.param_groups:
        group["params"][0].grad = torch.ones_like(group["params"][0])
    model.optimizer.step()
    model.optimizer.zero_grad(set_to_none=True)
    model.active_sh_degree = 0
    model.spatial_lr_scale = 1.
    model.max_radii2D = torch.zeros(rows)
    model.xyz_gradient_accum = torch.zeros(rows, 1)
    model.denom = torch.zeros(rows, 1)
    return model


def extensions(model, rows=2):
    return {group["name"]: torch.full((rows,) + tuple(group["params"][0].shape[1:]), .75)
            for group in model.optimizer.param_groups}


def publish(model, parameters):
    names = {"xyz": "_xyz", "f_dc": "_features_dc", "f_rest": "_features_rest",
             "opacity": "_opacity", "scaling": "_scaling", "rotation": "_rotation"}
    for name, value in parameters.items():
        setattr(model, names[name], value)


class FailingAllocator:
    """Only the runtime allocation seam is replaced; tensors/Adam remain real."""
    def __init__(self, fail_after):
        self.remaining = fail_after

    def __getattr__(self, name):
        return getattr(torch, name)

    def zeros(self, *args, **kwargs):
        self.remaining -= 1
        if self.remaining == 0:
            raise RuntimeError("injected allocator exhaustion")
        return torch.zeros(*args, **kwargs)

    def empty_like(self, *args, **kwargs):
        self.remaining -= 1
        if self.remaining == 0:
            raise RuntimeError("injected allocator exhaustion")
        return torch.empty_like(*args, **kwargs)


def dense_f_xyz(model, initialized=True):
    group = next(group for group in model.optimizer.param_groups if group["name"] == "xyz")
    old = group["params"][0]
    parameter = torch.nn.Parameter(old.detach().t().contiguous().t())
    state = model.optimizer.state.pop(old, None)
    if initialized and state:
        state = dict(state)
        for key in ("exp_avg", "exp_avg_sq"):
            state[key] = state[key].t().contiguous().t()
        model.optimizer.state[parameter] = state
    elif not initialized:
        model.optimizer.state.clear()
    group["params"][0] = parameter
    model._xyz = parameter


@unittest.skipIf(torch is None, "Run with a configured Torch training Python")
class CapacityStorageTests(unittest.TestCase):
    def assert_optimizer_equal(self, stock, pooled):
        for a_group, b_group in zip(stock.optimizer.param_groups, pooled.optimizer.param_groups):
            a, b = a_group["params"][0], b_group["params"][0]
            self.assertTrue(torch.equal(a, b))
            self.assertIsNone(b.grad)
            for key, value in stock.optimizer.state[a].items():
                self.assertTrue(torch.equal(value, pooled.optimizer.state[b][key]))

    def test_append_preserves_upstream_parameters_moments_steps_and_rng(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                stock, pooled = make_model(backend), make_model(backend)
                manager = install_capacity_storage(torch, pooled, backend,
                    max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
                self.assertIsNotNone(manager)
                rng = torch.get_rng_state().clone()
                expected = stock.cat_tensors_to_optimizer(extensions(stock))
                actual = pooled.cat_tensors_to_optimizer(extensions(pooled))
                for name in expected:
                    self.assertTrue(torch.equal(expected[name], actual[name]))
                    self.assertIsNone(actual[name].grad)
                    self.assertTrue(actual[name].is_leaf)
                    self.assertTrue(actual[name].is_contiguous())
                for expected_group, actual_group in zip(stock.optimizer.param_groups, pooled.optimizer.param_groups):
                    a = stock.optimizer.state[expected_group["params"][0]]
                    b = pooled.optimizer.state[actual_group["params"][0]]
                    for key in ("exp_avg", "exp_avg_sq", "step"):
                        self.assertTrue(torch.equal(a[key], b[key]))
                self.assertTrue(torch.equal(rng, torch.get_rng_state()))
                report = manager.snapshot()
                self.assertEqual(6, report["logical_rows"])
                self.assertGreater(report["capacity_rows"], 6)
                self.assertLessEqual(report["reserved_bytes"] - report["logical_bytes"], 4096)

    def test_stable_prune_reuses_capacity_and_preserves_next_adam_update(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                stock, pooled = make_model(backend), make_model(backend)
                manager = install_capacity_storage(torch, pooled, backend,
                    max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=16)
                stock.cat_tensors_to_optimizer(extensions(stock, 3))
                pooled.cat_tensors_to_optimizer(extensions(pooled, 3))
                capacity = manager.snapshot()["capacity_rows"]
                mask = torch.tensor([True, False, True, False, True, False, True])
                stock._prune_optimizer(mask)
                pooled._prune_optimizer(mask)
                self.assert_optimizer_equal(stock, pooled)
                self.assertEqual(capacity, manager.snapshot()["capacity_rows"])
                for model in (stock, pooled):
                    for group in model.optimizer.param_groups:
                        group["params"][0].grad = torch.full_like(group["params"][0], .125)
                    model.optimizer.step()
                    model.optimizer.zero_grad(set_to_none=True)
                self.assert_optimizer_equal(stock, pooled)

    def test_opacity_reset_reuses_capacity_but_preserves_upstream_step_and_new_leaf(self):
        stock, pooled = make_model(), make_model()
        manager = install_capacity_storage(torch, pooled, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
        stock.cat_tensors_to_optimizer(extensions(stock))
        pooled.cat_tensors_to_optimizer(extensions(pooled))
        before = manager.snapshot()
        reset = torch.full((6, 1), -.75)
        expected = stock.replace_tensor_to_optimizer(reset.clone(), "opacity")
        actual = pooled.replace_tensor_to_optimizer(reset.clone(), "opacity")
        self.assertTrue(torch.equal(expected["opacity"], actual["opacity"]))
        self.assert_optimizer_equal(stock, pooled)
        self.assertEqual(before, manager.snapshot())

    def test_growth_estimate_reports_capacity_peak_separately_from_current_storage(self):
        model = make_model()
        manager = install_capacity_storage(torch, model, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
        report = manager.estimate_growth(2, 1)
        self.assertEqual(8256, report["capacity_additional_peak_bytes"])
        self.assertEqual(8, report["transient_point_count"])
        self.assertEqual(7, report["final_point_count"])
        self.assertEqual(manager.snapshot()["reserved_bytes"], report["reserved_bytes"])

    def test_unknown_optimizer_and_half_precision_leave_stock_methods_unpatched(self):
        model = make_model()
        original = model.cat_tensors_to_optimizer
        model.optimizer = torch.optim.SGD(model.optimizer.param_groups)
        self.assertIsNone(install_capacity_storage(torch, model, "3dgs"))
        self.assertEqual(original, model.cat_tensors_to_optimizer)
        self.assertEqual("unsupported_optimizer", model._gsw_capacity_storage_status["reason"])
        model = make_model()
        original = model.cat_tensors_to_optimizer
        model.optimizer.param_groups[0]["params"][0] = torch.nn.Parameter(model._xyz.half())
        self.assertIsNone(install_capacity_storage(torch, model, "3dgs"))
        self.assertEqual(original, model.cat_tensors_to_optimizer)
        self.assertEqual("unsupported_parameter_layout", model._gsw_capacity_storage_status["reason"])

    def test_restored_bounded_storage_is_rebounded_after_small_prune(self):
        model = make_model()
        first = install_capacity_storage(torch, model, "3dgs",
            max_reserve_bytes=600, growth_fraction=.5, chunk_bytes=64)
        publish(model, model.cat_tensors_to_optimizer(extensions(model)))
        payload = io.BytesIO()
        torch.save(model.capture(), payload)
        payload.seek(0)
        captured = torch.load(payload)
        restored = make_model(rows=6)
        values = dict(zip(("xyz", "f_dc", "f_rest", "scaling", "rotation", "opacity"), captured[1:7]))
        for group in restored.optimizer.param_groups:
            group["params"][0] = values[group["name"]]
        restored.optimizer.state.clear()
        restored.optimizer.load_state_dict(captured[10])
        manager = install_capacity_storage(torch, restored, "3dgs",
            max_reserve_bytes=600, growth_fraction=.5, chunk_bytes=64)
        restored._prune_optimizer(torch.tensor([True, True, True, True, True, False]))
        self.assertLessEqual(manager.snapshot()["spare_bytes"], 600)

    def test_public_growth_admission_charges_capacity_peak_on_top_of_stock_estimate(self):
        from native.worker.training_resource_policy import GrowthAdmission
        model = make_model()
        policy = GrowthAdmission(lambda: (1024 ** 3, 2 * 1024 ** 3))
        before = policy.evaluate(model, "3dgs", clone_count=1, split_count=1)
        install_capacity_storage(torch, model, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
        after = policy.evaluate(model, "3dgs", clone_count=1, split_count=1)
        self.assertEqual(before.additional_peak_bytes + 8256, after.additional_peak_bytes)

    def test_allocation_exhaustion_leaves_all_original_groups_and_moments_unchanged(self):
        model = make_model()
        parameters = [group["params"][0] for group in model.optimizer.param_groups]
        values = [parameter.detach().clone() for parameter in parameters]
        states = [{key: value.clone() for key, value in model.optimizer.state[parameter].items()}
                  for parameter in parameters]
        manager = install_capacity_storage(FailingAllocator(5), model, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
        with self.assertRaisesRegex(RuntimeError, "injected allocator exhaustion"):
            model.cat_tensors_to_optimizer(extensions(model))
        for group, old, value, state in zip(model.optimizer.param_groups, parameters, values, states):
            self.assertIs(old, group["params"][0])
            self.assertTrue(torch.equal(value, old))
            for key, expected in state.items():
                self.assertTrue(torch.equal(expected, model.optimizer.state[old][key]))
        self.assertEqual(4, manager.snapshot()["logical_rows"])

    def test_insufficient_or_unknown_headroom_disables_optional_capacity_not_training_values(self):
        def unavailable():
            raise RuntimeError("unavailable memory observer")
        for probe in (lambda: (100 * 1024 ** 2, 1024 ** 3), unavailable):
            stock, pooled = make_model(), make_model()
            manager = install_capacity_storage(torch, pooled, "3dgs", memory_probe=probe,
                max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
            stock.cat_tensors_to_optimizer(extensions(stock))
            pooled.cat_tensors_to_optimizer(extensions(pooled))
            self.assert_optimizer_equal(stock, pooled)
            self.assertEqual(0, manager.snapshot()["spare_bytes"])

    def test_zero_initialized_spare_survives_checkpoint_without_gpu_compaction(self):
        for backend in ("3dgs", "2dgs"):
            model = make_model(backend)
            manager = install_capacity_storage(torch, model, backend,
                max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
            publish(model, model.cat_tensors_to_optimizer(extensions(model)))
            publish(model, model._prune_optimizer(torch.tensor([True, False, True, True, False, True])))
            capture = model.capture()
            self.assertIs(capture[1], model._xyz)
            self.assertEqual(12, len(capture))
            payload = io.BytesIO()
            torch.save(capture, payload)
            payload.seek(0)
            saved = torch.load(payload)
            self.assertIsInstance(saved[1], torch.nn.Parameter)
            self.assertTrue(saved[1].requires_grad)
            tensors = list(saved[1:7])
            tensors += [value for state in saved[10]["state"].values()
                        for key, value in state.items() if key in ("exp_avg", "exp_avg_sq")]
            for tensor in tensors:
                rows, tail = tensor.shape[0], tuple(tensor.shape[1:])
                row_elements = 1
                for dimension in tail:
                    row_elements *= dimension
                capacity = tensor.untyped_storage().nbytes() // (row_elements * 4)
                whole = tensor.detach().as_strided((capacity,) + tail, tensor.stride())
                self.assertEqual(0, int(torch.count_nonzero(whole[rows:]).item()))
            reserved = sum(tensor.untyped_storage().nbytes() for tensor in tensors)
            logical = sum(tensor.numel() * tensor.element_size() for tensor in tensors)
            self.assertEqual(manager.snapshot()["reserved_bytes"], reserved)
            self.assertLessEqual(reserved - logical, 4096)

    def test_empty_prune_and_regrowth_preserve_adam_step_and_stable_rows(self):
        stock, pooled = make_model(), make_model()
        manager = install_capacity_storage(torch, pooled, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=16)
        stock._prune_optimizer(torch.zeros(4, dtype=torch.bool))
        pooled._prune_optimizer(torch.zeros(4, dtype=torch.bool))
        stock.cat_tensors_to_optimizer(extensions(stock, 3))
        pooled.cat_tensors_to_optimizer(extensions(pooled, 3))
        self.assert_optimizer_equal(stock, pooled)
        self.assertEqual(3, manager.snapshot()["logical_rows"])
        self.assertLessEqual(manager.snapshot()["spare_bytes"], 4096)

    def test_lazy_adam_state_is_created_by_the_same_first_optimizer_update(self):
        stock, pooled = make_model(), make_model()
        stock.optimizer.state.clear()
        pooled.optimizer.state.clear()
        manager = install_capacity_storage(torch, pooled, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
        for model in (stock, pooled):
            model.cat_tensors_to_optimizer(extensions(model))
            self.assertEqual(0, len(model.optimizer.state))
            for group in model.optimizer.param_groups:
                group["params"][0].grad = torch.full_like(group["params"][0], .375)
            model.optimizer.step()
            model.optimizer.zero_grad(set_to_none=True)
            model.cat_tensors_to_optimizer(extensions(model, 1))
            model._prune_optimizer(torch.tensor([True, False, True, True, False, True, True]))
        self.assert_optimizer_equal(stock, pooled)
        self.assertLessEqual(manager.snapshot()["spare_bytes"], 4096)

    def test_sh_zero_empty_feature_group_does_not_hide_other_capacity_rows(self):
        stock, pooled = make_model(), make_model()
        for model in (stock, pooled):
            group = next(group for group in model.optimizer.param_groups if group["name"] == "f_rest")
            old = group["params"][0]
            state = model.optimizer.state.pop(old)
            parameter = torch.nn.Parameter(torch.empty(4, 0, 3))
            group["params"][0] = parameter
            state["exp_avg"] = torch.empty_like(parameter)
            state["exp_avg_sq"] = torch.empty_like(parameter)
            model.optimizer.state[parameter] = state
            model._features_rest = parameter
        manager = install_capacity_storage(torch, pooled, "3dgs",
            max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
        stock.cat_tensors_to_optimizer(extensions(stock))
        pooled.cat_tensors_to_optimizer(extensions(pooled))
        self.assert_optimizer_equal(stock, pooled)
        self.assertGreaterEqual(manager.snapshot()["capacity_rows"], 6)
        stock._prune_optimizer(torch.tensor([True, False, True, True, False, True]))
        pooled._prune_optimizer(torch.tensor([True, False, True, True, False, True]))
        self.assert_optimizer_equal(stock, pooled)

    def test_offset_or_aliased_optimizer_storage_is_not_reinterpreted_as_owned_capacity(self):
        model = make_model()
        parameter = model.optimizer.param_groups[0]["params"][0]
        model.optimizer.state[parameter]["exp_avg"] = torch.zeros(5, 3)[1:]
        self.assertIsNone(install_capacity_storage(torch, model, "3dgs"))
        model = make_model()
        group = next(group for group in model.optimizer.param_groups if group["name"] == "scaling")
        group["params"][0] = torch.nn.Parameter(model._xyz.detach())
        self.assertIsNone(install_capacity_storage(torch, model, "3dgs"))

    def test_known_dense_f_xyz_normalizes_once_without_changing_values_state_or_next_adam_step(self):
        for backend in ("3dgs", "2dgs"):
            for initialized in (False, True):
                with self.subTest(backend=backend, initialized=initialized):
                    stock, pooled = make_model(backend), make_model(backend)
                    dense_f_xyz(stock, initialized)
                    dense_f_xyz(pooled, initialized)
                    self.assertEqual((1, 4), pooled._xyz.stride())
                    before = pooled._xyz.detach().clone()
                    rng = torch.get_rng_state().clone()
                    step = pooled.optimizer.state[pooled._xyz]["step"] if initialized else None
                    manager = install_capacity_storage(torch, pooled, backend,
                        max_reserve_bytes=4096, growth_fraction=.5, chunk_bytes=64)
                    self.assertIsNotNone(manager, pooled._gsw_capacity_storage_status)
                    self.assertTrue(pooled._xyz.is_contiguous())
                    self.assertTrue(torch.equal(before, pooled._xyz))
                    self.assertIsNone(pooled._xyz.grad)
                    if initialized:
                        self.assertIs(step, pooled.optimizer.state[pooled._xyz]["step"])
                    self.assertTrue(torch.equal(rng, torch.get_rng_state()))
                    self.assertIs(manager, install_capacity_storage(torch, pooled, backend))
                    normalization = manager.snapshot()["layout_normalization"]
                    self.assertEqual(1, normalization["events"])
                    self.assertEqual(3 if initialized else 1, normalization["copies"])
                    for model in (stock, pooled):
                        for group in model.optimizer.param_groups:
                            parameter = group["params"][0]
                            parameter.grad = torch.full_like(parameter, .375)
                        model.optimizer.step()
                        model.optimizer.zero_grad(set_to_none=True)
                    self.assert_optimizer_equal(stock, pooled)

    def test_normalization_allocation_failure_cannot_partially_replace_parameters_or_moments(self):
        model = make_model()
        dense_f_xyz(model)
        old = model._xyz
        state = model.optimizer.state[old]
        moments = {key: value.clone() for key, value in state.items()}
        with self.assertRaisesRegex(RuntimeError, "injected allocator exhaustion"):
            install_capacity_storage(FailingAllocator(2), model, "3dgs")
        self.assertIs(old, model._xyz)
        self.assertIs(old, model.optimizer.param_groups[0]["params"][0])
        self.assertIs(state, model.optimizer.state[old])
        for key, value in moments.items():
            self.assertTrue(torch.equal(value, state[key]))

    def test_dense_f_normalization_waits_for_all_layout_gradient_and_headroom_checks(self):
        for rejected in ("late_state", "gradient", "headroom"):
            with self.subTest(rejected=rejected):
                model = make_model()
                dense_f_xyz(model)
                parameter = model._xyz
                probe = None
                if rejected == "late_state":
                    last = model.optimizer.param_groups[-1]["params"][0]
                    model.optimizer.state[last]["exp_avg"] = model.optimizer.state[last]["exp_avg"].half()
                elif rejected == "gradient":
                    parameter.grad = torch.ones_like(parameter)
                else:
                    probe = lambda: (100 * 1024 ** 2, 1024 ** 3)
                self.assertIsNone(install_capacity_storage(FailingAllocator(1), model, "3dgs", memory_probe=probe))
                self.assertIs(parameter, model._xyz)
                self.assertEqual((1, 4), parameter.stride())


if __name__ == "__main__":
    unittest.main()
