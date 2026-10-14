"""Public growth-admission regressions; no GPU or training is started."""
import random
import sys
import unittest
from types import SimpleNamespace

import numpy as np

from native.worker.training_resource_policy import GrowthAdmission


MIB = 1024 ** 2
GIB = 1024 ** 3


def model_fixture(backend="3dgs", count=10, dtype=np.float32,
                  moment_dtype=np.float32):
    shapes = {"xyz": (count, 3), "f_dc": (count, 1, 3),
              "f_rest": (count, 15, 3), "opacity": (count, 1),
              "scaling": (count, 3 if backend == "3dgs" else 2),
              "rotation": (count, 4)}
    parameters = {name: np.ones(shape, dtype=dtype)
                  for name, shape in shapes.items()}
    states = {name: {"exp_avg": np.zeros(shape, dtype=moment_dtype),
                     "exp_avg_sq": np.zeros(shape, dtype=moment_dtype)}
              for name, shape in shapes.items()}
    return SimpleNamespace(
        optimizer=SimpleNamespace(
            param_groups=[{"name": name, "params": [tensor]}
                          for name, tensor in parameters.items()],
            state=states),
        xyz_gradient_accum=np.ones((count, 1), dtype=np.float32),
        denom=np.ones((count, 1), dtype=np.float32),
        max_radii2D=np.ones(count, dtype=np.float32), tmp_radii=None)


class GrowthAdmissionTests(unittest.TestCase):
    def test_growth_event_schema_rejects_nonfinite_boolean_or_unknown_values(self):
        from native.worker.training_resource_policy import normalize_growth
        valid = {"version": 1, "backend": "3dgs", "iteration": 800,
                 "state": "admitted", "reason": "admitted",
                 "point_count": 10, "transient_point_count": 18,
                 "final_point_count": 15, "parameter_bytes": 2360,
                 "optimizer_state_bytes": 4720, "additional_peak_bytes": 20088,
                 "free_bytes": 60000, "total_bytes": 100000,
                 "spare_bytes": 10000, "available_bytes": 50000,
                 "cache_credit_bytes": 0, "clone_count": 2, "split_count": 3,
                 "observation_valid": True, "candidate_counts_known": True,
                 "allowed": True}
        self.assertEqual(normalize_growth(valid), valid)
        for key, value in (("version", True), ("version", 2), ("backend", "fastgs"),
                           ("iteration", -1), ("iteration", 1.5),
                           ("state", "paused"), ("reason", "unknown"),
                           ("additional_peak_bytes", True),
                           ("additional_peak_bytes", float("nan")),
                           ("total_bytes", float("inf")),
                           ("point_count", 2 ** 53), ("allowed", 1),
                           ("allowed", False), ("observation_valid", False)):
            with self.subTest(key=key, value=value):
                self.assertIsNone(normalize_growth(dict(valid, **{key: value})))
        missing = dict(valid)
        missing.pop("optimizer_state_bytes")
        self.assertIsNone(normalize_growth(missing))
        deferred = dict(valid, state="deferred", reason="vram_pressure", allowed=False)
        self.assertEqual(normalize_growth(deferred), deferred)

    def test_adapter_defers_before_tensor_operations_and_emits_valid_reason(self):
        from native.worker.training_resource_policy import admit_densification
        events = []
        policy = GrowthAdmission(lambda: (128 * MIB, 12 * GIB))
        self.assertFalse(admit_densification(
            None, model_fixture(), "3dgs", .0002, 1., policy,
            lambda *event: events.append(event), 800))
        self.assertEqual(events[0][0], "[gsw-training-growth]")
        self.assertEqual(events[0][1]["reason"], "vram_pressure")
        self.assertEqual(events[0][1]["state"], "deferred")
        self.assertFalse(events[0][1]["candidate_counts_known"])

    def test_legacy_resume_without_policy_preserves_the_existing_route(self):
        from native.worker.training_resource_policy import admit_densification
        # The adapter must not inspect model/Torch or emit a new policy event
        # when a legacy full-state checkpoint explicitly keeps its old route.
        events = []
        self.assertTrue(admit_densification(None, None, "3dgs", .0002, 1.,
                                           None, lambda *event: events.append(event), 800))
        self.assertEqual(events, [])

    def test_invalid_policy_configuration_is_rejected_at_construction(self):
        for options in ({"usage_fraction": float("nan")},
                        {"usage_fraction": 0}, {"usage_fraction": 1.01},
                        {"usage_fraction": True}, {"minimum_spare_bytes": 0},
                        {"minimum_spare_bytes": -1}, {"minimum_spare_bytes": True}):
            with self.subTest(options=options):
                with self.assertRaises(ValueError):
                    GrowthAdmission(lambda: (GIB, 2 * GIB), **options)

    def test_backend_and_real_optimizer_dtype_determine_peak(self):
        cases = [("2dgs", np.float32, np.float32, 2320, 4640, 19780),
                 ("3dgs", np.float32, np.float16, 2360, 2360, 14660),
                 ("3dgs", np.float16, np.float32, 1180, 4720, 16430)]
        for backend, parameter_dtype, moment_dtype, weights, moments, peak in cases:
            with self.subTest(backend=backend, moments=moment_dtype):
                decision = GrowthAdmission(lambda: (11 * GIB, 12 * GIB)).evaluate(
                    model_fixture(backend, dtype=parameter_dtype, moment_dtype=moment_dtype),
                    backend, clone_count=2, split_count=3)
                self.assertTrue(decision.allowed)
                self.assertEqual(decision.parameter_bytes, weights)
                self.assertEqual(decision.optimizer_state_bytes, moments)
                self.assertEqual(decision.additional_peak_bytes, peak)

    def test_observed_peak_not_net_new_parameter_bytes_controls_admission(self):
        # Only twenty thousand bytes remain after the independent 512 MiB floor.
        # Net new parameters are tiny, but full optimizer concatenation is not.
        model = model_fixture()
        refused = GrowthAdmission(lambda: (512 * MIB + 20000, GIB)).evaluate(
            model, "3dgs", clone_count=2, split_count=3)
        exact = GrowthAdmission(lambda: (512 * MIB + 20088, GIB)).evaluate(
            model, "3dgs", clone_count=2, split_count=3)
        self.assertFalse(refused.allowed)
        self.assertEqual(refused.available_bytes, 20000)
        self.assertTrue(exact.allowed)

    def test_lazy_adam_and_default_unknown_candidates_are_conservative(self):
        model = model_fixture()
        model.optimizer.state.clear()
        decision = GrowthAdmission(lambda: (11 * GIB, 12 * GIB)).evaluate(
            model, "3dgs")
        self.assertTrue(decision.allowed)
        self.assertEqual(decision.optimizer_state_bytes, 4720)
        self.assertEqual(decision.transient_point_count, 30)
        self.assertEqual(decision.final_point_count, 20)
        # All ten parents split: 28,320 replacements + 4,720 candidate
        # parameters + 800 statistics + 2,600 scratch = 36,440 bytes.
        self.assertEqual(decision.additional_peak_bytes, 36440)

    def test_unsupported_layout_or_counts_defer_without_running_density(self):
        cases = [(model_fixture(), {"clone_count": 8, "split_count": 3}),
                 (model_fixture(), {"clone_count": -1, "split_count": 0}),
                 (model_fixture(), {"clone_count": True, "split_count": 0}),
                 (model_fixture(), {"clone_count": None, "split_count": 0})]
        missing = model_fixture()
        missing.optimizer.param_groups.pop()
        cases.append((missing, {"clone_count": 0, "split_count": 0}))
        duplicated = model_fixture()
        duplicated.optimizer.param_groups.append(duplicated.optimizer.param_groups[0])
        cases.append((duplicated, {"clone_count": 0, "split_count": 0}))
        for model, counts in cases:
            with self.subTest(counts=counts):
                decision = GrowthAdmission(lambda: (11 * GIB, 12 * GIB)).evaluate(
                    model, "3dgs", **counts)
                self.assertFalse(decision.allowed)
                self.assertEqual(decision.reason, "estimate_unavailable")
                self.assertTrue(decision.observation_valid)

    def test_only_explicit_reusable_cache_gets_partial_credit(self):
        from native.worker.training_resource_policy import MemoryObservation
        model = model_fixture()
        without_cache = GrowthAdmission(
            lambda: (512 * MIB + 10088, GIB)).evaluate(
                model, "3dgs", clone_count=2, split_count=3)
        with_cache = GrowthAdmission(lambda: MemoryObservation(
            512 * MIB + 10088, GIB, reusable_cache_bytes=20000)).evaluate(
                model, "3dgs", clone_count=2, split_count=3)
        self.assertFalse(without_cache.allowed)
        self.assertTrue(with_cache.allowed)
        self.assertEqual(with_cache.cache_credit_bytes, 10000)
        self.assertEqual(with_cache.available_bytes, 20088)
        # Cache cannot erase the actual device-free reserve for the driver.
        low_device_free = GrowthAdmission(lambda: MemoryObservation(
            128 * MIB, GIB, reusable_cache_bytes=512 * MIB)).evaluate(
                model, "3dgs", clone_count=2, split_count=3)
        self.assertFalse(low_device_free.allowed)
        invalid_cache = GrowthAdmission(lambda: MemoryObservation(
            800 * MIB, GIB, reusable_cache_bytes=512 * MIB)).evaluate(
                model, "3dgs", clone_count=2, split_count=3)
        self.assertFalse(invalid_cache.allowed)
        self.assertEqual(invalid_cache.reason, "invalid_observation")

    def test_unknown_or_invalid_memory_is_fail_closed_not_a_training_error(self):
        def unavailable():
            raise OSError("Memory observation unavailable")
        cases = [(unavailable, "probe_unavailable")]
        for snapshot in (None, (True, 12 * GIB), (-1, GIB),
                         (GIB + 1, GIB), (float("nan"), GIB),
                         (float("inf"), GIB), (0, 0)):
            cases.append((lambda snapshot=snapshot: snapshot, "invalid_observation"))
        for probe, reason in cases:
            with self.subTest(reason=reason, probe=probe):
                decision = GrowthAdmission(probe).evaluate(
                    model_fixture(), "3dgs", clone_count=2, split_count=3)
                self.assertFalse(decision.allowed)
                self.assertEqual(decision.reason, reason)
                self.assertFalse(decision.observation_valid)

    def test_exact_clone_and_split_counts_admit_a_small_3dgs_growth(self):
        model = model_fixture()
        decision = GrowthAdmission(lambda: (11 * GIB, 12 * GIB)).evaluate(
            model, "3dgs", clone_count=2, split_count=3)
        self.assertTrue(decision.allowed)
        # Worked ten-point SH3/F32 layout: 2,360 parameter bytes, 4,720
        # Adam bytes; cloning two and splitting three peaks at eighteen rows.
        # Including replacement/candidate/stat/scratch buffers requires 20,088
        # additional bytes, independently fixed here rather than recalculated.
        self.assertEqual(decision.parameter_bytes, 2360)
        self.assertEqual(decision.optimizer_state_bytes, 4720)
        self.assertEqual(decision.transient_point_count, 18)
        self.assertEqual(decision.final_point_count, 15)
        self.assertEqual(decision.additional_peak_bytes, 20088)

    def test_pressure_defers_growth_without_model_state_or_rng_changes(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                model = model_fixture(backend)
                tensors = [group["params"][0]
                           for group in model.optimizer.param_groups]
                tensors += [tensor for state in model.optimizer.state.values()
                            for tensor in state.values()]
                tensors += [model.xyz_gradient_accum, model.denom,
                            model.max_radii2D]
                before = [tensor.copy() for tensor in tensors]
                python_rng = random.getstate()
                numpy_rng = np.random.get_state()
                decision = GrowthAdmission(lambda: (256 * MIB, 12 * GIB)).evaluate(
                    model, backend, clone_count=2, split_count=3)
                self.assertFalse(decision.allowed)
                self.assertEqual(decision.reason, "vram_pressure")
                for old, tensor in zip(before, tensors):
                    np.testing.assert_array_equal(old, tensor)
                self.assertEqual(random.getstate(), python_rng)
                after_rng = np.random.get_state()
                self.assertEqual(after_rng[0], numpy_rng[0])
                np.testing.assert_array_equal(after_rng[1], numpy_rng[1])
                self.assertEqual(after_rng[2:], numpy_rng[2:])


if "--torch-cpu" in sys.argv:
    # Explicit runtime validation; default QA has no Torch dependency or skips.
    # This route uses only CPU tensors and never asks CUDA for a device/context.
    sys.argv.remove("--torch-cpu")
    import torch

    class TorchGrowthAdmissionTests(unittest.TestCase):
        def model(self, backend):
            shapes = {"xyz": (4, 3), "f_dc": (4, 1, 3),
                      "f_rest": (4, 15, 3), "opacity": (4, 1),
                      "scaling": (4, 3 if backend == "3dgs" else 2),
                      "rotation": (4, 4)}
            groups = [{"name": name, "params": [torch.nn.Parameter(torch.ones(shape))]}
                      for name, shape in shapes.items()]
            optimizer = torch.optim.Adam(groups, lr=.001)
            sum(group["params"][0].sum() for group in optimizer.param_groups).backward()
            optimizer.step()
            model = SimpleNamespace(
                optimizer=optimizer, percent_dense=.01,
                xyz_gradient_accum=torch.tensor([[1.], [-1.], [float("nan")], [2.]]),
                denom=torch.ones(4, 1), max_radii2D=torch.ones(4), tmp_radii=None,
                get_scaling=torch.tensor([[.5] * shapes["scaling"][1],
                                          [.5] * shapes["scaling"][1],
                                          [2.] * shapes["scaling"][1],
                                          [2.] * shapes["scaling"][1]]))
            return model

        def snapshot(self, model):
            tensors = [tensor for group in model.optimizer.param_groups
                       for tensor in (group["params"][0], group["params"][0].grad)]
            tensors += [value for state in model.optimizer.state.values()
                        for value in state.values() if torch.is_tensor(value)]
            tensors += [model.xyz_gradient_accum, model.denom,
                        model.max_radii2D, model.get_scaling]
            return [tensor.detach().numpy().tobytes() for tensor in tensors]

        def test_actual_tensor_candidate_rules_and_state_are_preserved(self):
            from native.worker.training_resource_policy import admit_densification
            for backend in ("3dgs", "2dgs"):
                with self.subTest(backend=backend):
                    model = self.model(backend)
                    state = self.snapshot(model)
                    rng = torch.get_rng_state().clone()
                    events = []
                    policy = GrowthAdmission(lambda: (11 * GIB, 12 * GIB))
                    self.assertTrue(admit_densification(
                        torch, model, backend, .5, 100., policy,
                        lambda *event: events.append(event), 800))
                    event = events[-1][1]
                    self.assertEqual((event["clone_count"], event["split_count"]), (2, 1))
                    self.assertTrue(event["candidate_counts_known"])
                    self.assertEqual(self.snapshot(model), state)
                    self.assertTrue(torch.equal(torch.get_rng_state(), rng))

        def test_actual_tensor_pressure_defers_without_changes(self):
            from native.worker.training_resource_policy import admit_densification
            for backend in ("3dgs", "2dgs"):
                with self.subTest(backend=backend):
                    model = self.model(backend)
                    state = self.snapshot(model)
                    rng = torch.get_rng_state().clone()
                    policy = GrowthAdmission(lambda: (128 * MIB, 12 * GIB))
                    self.assertFalse(admit_densification(
                        torch, model, backend, .5, 100., policy, iteration=800))
                    self.assertEqual(self.snapshot(model), state)
                    self.assertTrue(torch.equal(torch.get_rng_state(), rng))


if __name__ == "__main__":
    unittest.main()
