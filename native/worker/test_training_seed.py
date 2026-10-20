"""Public stochastic training seed and complete-state identity acceptance."""

import os
import json
import io
from pathlib import Path
import random
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import numpy as np


class TrainingSeedTests(unittest.TestCase):
    def test_seed_controls_real_python_numpy_sequences_without_changing_dataset_initialization(self):
        from native.worker.training_seed import apply_training_seed
        torch_boundary = SimpleNamespace(manual_seed=lambda value: None)
        apply_training_seed(17, torch_module=torch_boundary)
        self.assertAlmostEqual(random.random(), .5219839097124932)
        self.assertAlmostEqual(float(np.random.random()), .2946650026871097)
        apply_training_seed(23, torch_module=torch_boundary)
        different = (random.random(), float(np.random.random()))
        self.assertNotEqual(different, (.5219839097124932, .2946650026871097))
        apply_training_seed(17, torch_module=torch_boundary)
        self.assertEqual((random.random(), float(np.random.random())),
                         (.5219839097124932, .2946650026871097))

    def test_invalid_seed_is_rejected_before_any_native_training_process(self):
        from native.worker.gsw_worker import load_configuration
        base = Path(os.environ.get("GSW_CAMERA_P1_TEST_ROOT", tempfile.gettempdir()))
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            path = Path(temporary) / "job.json"
            for seed in (-1, 2**32, True, 1.5, "17"):
                path.write_text(json.dumps({"repositoryRoot": "fixture", "datasetPath": "fixture",
                    "outputRoot": "fixture", "outputScene": "fixture", "backend": "3dgs", "quality": "quick",
                    "trainingSeed": seed}), encoding="utf-8")
                with self.subTest(seed=seed), self.assertRaisesRegex(ValueError, "training_seed_invalid"):
                    load_configuration(path)

    def test_complete_checkpoint_records_seed_and_refuses_changed_seed_before_optimizer_restore(self):
        from native.worker import training_checkpoint
        model = SimpleNamespace(_gsw_training_seed=17, capture=lambda: ("actual-state-boundary-fixture",))
        torch_boundary = SimpleNamespace(get_rng_state=lambda: "cpu-rng",
                                        cuda=SimpleNamespace(is_available=lambda: False))
        state = training_checkpoint.capture_state(torch_boundary, np, model, 3, 10, "identity", ["a"], [],
                                                   1., .1, 0., backend="2dgs")
        self.assertEqual(state["training_seed"], 17)
        model._gsw_training_seed = 23
        with self.assertRaisesRegex(ValueError, "seed"):
            training_checkpoint.restore_state(torch_boundary, np, model, SimpleNamespace(iterations=10),
                                              state, "identity", ["a"], backend="2dgs")

    def test_subprocess_control_preserves_legacy_zero_shape_and_forwards_nonzero_seed(self):
        from native.backend import compute
        for backend in ("3dgs", "2dgs"):
            for seed in (None, 0, 17):
                with self.subTest(backend=backend, seed=seed):
                    control = {"output": "owned-output", "session": "same", "backend": backend}
                    job = {"log": [], "backend": backend, "native_control": control}
                    if seed is not None:
                        job["training_seed"] = seed
                    process = mock.Mock(stdout=io.StringIO(), wait=mock.Mock(return_value=0), poll=mock.Mock(return_value=0))
                    with mock.patch.object(compute.subprocess, "Popen", return_value=process) as boundary:
                        compute.run_logged(job, ["python", "train.py"], compute.GAUSSIAN_DIR, backend)
                    environment = boundary.call_args.kwargs["env"]
                    passed = json.loads(environment["GSW_NATIVE_TRAINING_CONTROL"])
                    expected = dict(control)
                    if seed:
                        expected["training_seed"] = seed
                    self.assertEqual(passed, expected)
                    self.assertEqual(environment["GSW_TRAINING_SEED"], str(seed or 0))
                    self.assertEqual(control, {"output": "owned-output", "session": "same", "backend": backend})


if __name__ == "__main__":
    unittest.main()
