"""Opt-in quality control, real CPU optimizer/RNG recovery and durable schedules.

No CUDA trainer or GUI is executed. All fixtures are owned D-drive children.
"""
import copy
import json
import os
from pathlib import Path
import random
import tempfile
from types import SimpleNamespace
import unittest

import numpy as np


class QualityTrainingLifecycleTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_QUALITY_LIFECYCLE_TEST_ROOT", "D:/Codex/quality-lifecycle-tests"))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="owned-", dir=str(base))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_load_configuration_rejects_quality_without_native_full_checkpoint_backend(self):
        from native.worker.gsw_worker import load_configuration
        base = {"repositoryRoot": "owned", "datasetPath": "owned", "outputRoot": "owned",
                "outputScene": "owned", "quality": "quick", "qualityPlan": {"version": 1}}
        for backend in ("3dgs", "2dgs"):
            for native in (False, None):
                config = dict(base, backend=backend)
                if native is not None:
                    config["nativeCheckpoint"] = native
                path = self.root / "request.json"
                path.write_text(json.dumps(config), encoding="utf-8")
                with self.subTest(backend=backend, native=native), self.assertRaisesRegex(ValueError, "quality_requires_native_checkpoint"):
                    load_configuration(path)

    def test_public_control_resolution_inherits_complete_plan_and_rejects_mismatch_or_tampering(self):
        from native.worker.training_checkpoint import quality_plan_for_control
        from native.worker.training_quality_plan import resolve_quality_plan
        for backend in ("3dgs", "2dgs"):
            request = {"version": 1, "geometry_lr_multiplier": .5}
            plan = quality_plan_for_control(request, backend=backend)
            self.assertEqual(plan, resolve_quality_plan(request, backend=backend))
            self.assertEqual(quality_plan_for_control(None, backend=backend, checkpoint_plan=plan, resume=True), plan)
            self.assertIsNone(quality_plan_for_control(None, backend=backend, resume=True))
            with self.assertRaisesRegex(ValueError, "quality_resume_plan_mismatch"):
                quality_plan_for_control({"version": 1}, backend=backend, checkpoint_plan=plan, resume=True)
            with self.assertRaisesRegex(ValueError, "quality_resume_plan_mismatch"):
                quality_plan_for_control(request, backend=backend, resume=True)
            changed = copy.deepcopy(plan)
            changed["plan_hash"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "quality_plan_resolution_invalid"):
                quality_plan_for_control(None, backend=backend, checkpoint_plan=changed, resume=True)

    def test_explicit_preparation_changes_only_three_geometry_rates_and_uses_independent_sampler(self):
        from native.worker.training_checkpoint import prepare_quality_training
        from native.worker.quality_random import QualityRandomStreams
        for backend in ("3dgs", "2dgs"):
            opt = SimpleNamespace(position_lr_init=.4, position_lr_final=.04, scaling_lr=.2,
                feature_lr=.3, opacity_lr=.5, iterations=10, densification_interval=100)
            original = copy.deepcopy(vars(opt))
            dataset = SimpleNamespace(training_seed=17)
            before = random.getstate()
            self.assertIsNone(prepare_quality_training(None, dataset, opt, backend))
            self.assertEqual(vars(opt), original)
            self.assertEqual(vars(dataset), {"training_seed": 17})
            self.assertEqual(random.getstate(), before)
            quality = prepare_quality_training({"qualityPlan": {"version": 1, "geometry_lr_multiplier": .5}},
                dataset, opt, backend)
            expected = dict(original, position_lr_init=.2, position_lr_final=.02, scaling_lr=.1)
            self.assertEqual(vars(opt), expected)
            self.assertEqual(quality.plan["backend"], backend)
            self.assertEqual(random.getstate(), before)
            independent = QualityRandomStreams(17)
            for _ in range(10):
                quality.random_streams.strategy_random().random()
                self.assertEqual(quality.choose_index(7), independent.choose_index(7))

    def test_real_cpu_checkpoint_restores_quality_sampler_strategy_rng_and_adam_for_both_backends(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Existing Torch runtime required; run with CUDA_VISIBLE_DEVICES=-1")
        if torch.cuda.is_available():
            self.skipTest("CPU-only evidence requires CUDA_VISIBLE_DEVICES=-1 before process start")
        from native.worker import training_checkpoint as cp

        class Model:
            def __init__(self, opt):
                self._gsw_training_seed = 17
                self.value = torch.nn.Parameter(torch.tensor([.3, .8]))
                self._exposure = torch.nn.Parameter(torch.tensor([.5]))
                self.exposure_mapping, self.pretrained_exposures = {"camera": 0}, None
                self.setup(opt)

            def setup(self, opt):
                self.optimizer = torch.optim.Adam([self.value], lr=opt.position_lr_init)
                self.exposure_optimizer = torch.optim.Adam([self._exposure], lr=.02)

            def capture(self):
                return self.value, self.optimizer.state_dict(), self._exposure

            def restore(self, state, opt):
                self.value, optimizer, self._exposure = state
                self.setup(opt)
                self.optimizer.load_state_dict(optimizer)

        def create(backend, output):
            opt = SimpleNamespace(position_lr_init=.02, position_lr_final=.002, scaling_lr=.01, iterations=12)
            dataset = SimpleNamespace(training_seed=17)
            quality = cp.prepare_quality_training({"qualityPlan": {"version": 1, "geometry_lr_multiplier": .5}}, dataset, opt, backend)
            model = Model(opt)
            model.backend = backend
            quality.bind_model(model)
            control = {"output": str(output), "session": "owned", "backend": backend,
                "training_seed": 17, "qualityPlan": quality.plan}
            return model, quality, opt, control

        def seed():
            random.seed(9)
            np.random.seed(9)
            torch.manual_seed(9)

        def advance(model, quality, pending, count):
            sequence = []
            for _ in range(count):
                if not pending:
                    pending.extend(range(4))
                camera = pending.pop(quality.choose_index(len(pending)))
                sequence.append(camera)
                target = (torch.rand(2, generator=quality.random_streams.torch_generator("cpu"))
                    + quality.random_streams.strategy_random().random() + float(np.random.random()) + camera)
                prediction = model.value * model._exposure if model.backend == "3dgs" else model.value
                loss = ((prediction - target) ** 2).sum()
                loss.backward()
                model.optimizer.step()
                model.exposure_optimizer.step()
                model.optimizer.zero_grad(set_to_none=True)
                model.exposure_optimizer.zero_grad(set_to_none=True)
            return sequence

        for backend in ("3dgs", "2dgs"):
            seed()
            expected, expected_quality, opt, control = create(backend, self.root / backend)
            expected_sequence = advance(expected, expected_quality, [], 12)
            seed()
            interrupted, quality, opt, control = create(backend, self.root / backend)
            pending = []
            prefix = advance(interrupted, quality, pending, 5)
            state = cp.capture_state(torch, np, interrupted, 5, 12, "owned", list(range(4)), pending, 1., .2, 0., backend=backend)
            self.assertEqual(state["quality_plan"], quality.plan)
            store = cp.TrainingCheckpoint(control["output"], control)
            store.save(torch, state)
            resumed, restored_quality, opt, control = create(backend, self.root / backend)
            loaded = store.load(torch, "owned")
            cp.restore_state(torch, np, resumed, opt, loaded, "owned", list(range(4)), backend=backend)
            suffix = advance(resumed, restored_quality, loaded["pending_indices"], 7)
            with self.subTest(backend=backend):
                self.assertEqual(prefix + suffix, expected_sequence)
                self.assertTrue(torch.equal(resumed.value, expected.value))
                self.assertTrue(torch.equal(resumed._exposure, expected._exposure))
                for name in ("exp_avg", "exp_avg_sq", "step"):
                    self.assertTrue(torch.equal(resumed.optimizer.state_dict()["state"][0][name],
                                                expected.optimizer.state_dict()["state"][0][name]))

    def test_any_enabled_supervision_requires_a_bound_execution_controller_not_just_a_saved_plan(self):
        from native.worker.training_checkpoint import prepare_quality_training
        for backend in ("3dgs", "2dgs"):
            opt = SimpleNamespace(position_lr_init=.1, position_lr_final=.01, scaling_lr=.1)
            quality = prepare_quality_training({"qualityPlan": {"version": 1,
                "supervision": {"mask": {"manifest": "owned-mask.json"}}}}, SimpleNamespace(training_seed=17), opt, backend)
            with self.subTest(backend=backend), self.assertRaisesRegex(ValueError, "quality_controller_required"):
                quality.assert_ready()

    def test_real_cpu_scheduled_checkpoints_are_atomic_full_states_preserve_history_and_final_is_audit_only(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Existing Torch runtime required with CUDA_VISIBLE_DEVICES=-1")
        if torch.cuda.is_available():
            self.skipTest("Run CPU checkpoint evidence with CUDA_VISIBLE_DEVICES=-1")
        from native.worker import training_checkpoint as cp
        output = self.root / "scheduled"
        output.mkdir()
        legacy = output / "chkpnt3.pth"
        legacy.write_bytes(b"existing tuple history must remain unchanged")
        opt = SimpleNamespace(position_lr_init=.1, position_lr_final=.01, scaling_lr=.1, iterations=10)
        dataset = SimpleNamespace(training_seed=17)
        quality = cp.prepare_quality_training({"qualityPlan": {"version": 1}}, dataset, opt, "2dgs")
        value = torch.nn.Parameter(torch.tensor([.3]))
        optimizer = torch.optim.Adam([value], lr=.1)
        value.square().sum().backward()
        optimizer.step()
        model = SimpleNamespace(_gsw_training_seed=17, capture=lambda: (value, optimizer.state_dict()))
        quality.bind_model(model)
        state = cp.capture_state(torch, np, model, 3, 10, "owned", ["a"], [], 1., .2, 0., backend="2dgs")
        control = {"output": str(output), "session": "owned", "backend": "2dgs", "training_seed": 17,
                   "qualityPlan": quality.plan}
        store = cp.TrainingCheckpoint(output, control)
        manifest = store.save_scheduled(torch, state)
        self.assertFalse(manifest["completed"])
        self.assertEqual(manifest["state_kind"], "scheduled")
        observed, path = cp.read_scheduled_manifest(output, 3)
        self.assertEqual(observed, manifest)
        self.assertEqual(path.parent, output / ".gsw-resume/scheduled/iteration-3")
        loaded = store.load_scheduled(torch, "owned", 3)
        self.assertEqual(loaded["quality_plan"], quality.plan)
        self.assertTrue(torch.equal(loaded["model"][0], value))
        with self.assertRaisesRegex(ValueError, "scheduled_checkpoint_exists"):
            store.save_scheduled(torch, state)
        self.assertEqual(legacy.read_bytes(), b"existing tuple history must remain unchanged")
        self.assertFalse((output / ".gsw-resume/ready.json").exists())
        final = store.save_scheduled(torch, dict(state, iteration=10))
        self.assertTrue(final["completed"])
        with self.assertRaisesRegex(ValueError, "completed_not_resumable"):
            store.load_scheduled(torch, "owned", 10)
        self.assertEqual(cp.read_scheduled_manifest(output, 3)[0], manifest)


if __name__ == "__main__":
    unittest.main()
