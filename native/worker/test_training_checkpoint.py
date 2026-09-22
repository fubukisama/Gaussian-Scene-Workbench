import io
import json
import pickle
from pathlib import Path
import random
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

from native.worker import training_checkpoint as checkpoint
from native.worker import gsw_worker


class PickleStore:
    save = staticmethod(pickle.dump)


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.control = {"output": str(self.root), "request": str(self.root / "pause.json"), "session": "test"}
        self.store = checkpoint.TrainingCheckpoint(self.root, self.control)
        self.state = {"iteration": 3, "total": 10, "identity": "fixture"}

    def test_pause_requires_matching_session(self):
        self.assertFalse(self.store.pause_requested())
        checkpoint.atomic_json(self.control["request"], {"session": "stale"})
        self.assertFalse(self.store.pause_requested())
        checkpoint.atomic_json(self.control["request"], {"session": "test"})
        self.assertTrue(self.store.pause_requested())

    def test_save_validates_hash_and_rejects_corruption(self):
        self.store.save(PickleStore, self.state)
        manifest, path = checkpoint.read_manifest(self.root)
        self.assertEqual(manifest["iteration"], 3)
        path.write_bytes(b"corrupted")
        with self.assertRaisesRegex(ValueError, "checksum"):
            checkpoint.read_manifest(self.root)

    def test_failed_replacement_keeps_previous_checkpoint(self):
        self.store.save(PickleStore, self.state)
        previous, path = checkpoint.read_manifest(self.root)
        with mock.patch.object(checkpoint, "atomic_json", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.store.save(PickleStore, dict(self.state, iteration=4))
        self.assertEqual(checkpoint.read_manifest(self.root)[0], previous)
        self.assertTrue(path.exists())

    def test_manifest_cannot_escape_root_or_advertise_completed_run(self):
        self.store.save(PickleStore, self.state)
        manifest, _ = checkpoint.read_manifest(self.root)
        for update in ({"file": "../outside.pth"}, {"iteration": 10}, {"iteration": True}):
            checkpoint.atomic_json(self.root / ".gsw-resume/ready.json", dict(manifest, **update))
            with self.assertRaises(ValueError):
                checkpoint.read_manifest(self.root)

    def test_worker_pause_does_not_cancel_and_cancel_remains_available(self):
        events = []
        with mock.patch("sys.stdin", io.StringIO("pause\ncancel\n")), context_stdout():
            gsw_worker.watch_cancel_input(lambda job: events.append(job), "job", lambda: events.append("pause"))
        self.assertEqual(events, ["pause", "job"])
        self.assertIn("paused", gsw_worker.FINAL_STATES)

    def test_identity_detects_changed_contents_even_at_same_size(self):
        (self.root / "images").mkdir()
        image = self.root / "images/a.png"
        image.write_bytes(b"abcd")
        dataset = SimpleNamespace(source_path=str(self.root), model_path="output", resolution=1)
        opt, pipe = SimpleNamespace(iterations=10), SimpleNamespace(debug=False)
        before = checkpoint.training_identity(dataset, opt, pipe)
        dataset.model_path = "moved-output"
        self.assertEqual(before, checkpoint.training_identity(dataset, opt, pipe))
        image.write_bytes(b"abce")
        self.assertNotEqual(before, checkpoint.training_identity(dataset, opt, pipe))

    def test_resume_worker_never_overwrites_output_or_reruns_colmap(self):
        output = self.root / "output" / "scene"
        store = checkpoint.TrainingCheckpoint(output, {"output": str(output), "session": "old"})
        store.save(PickleStore, self.state)
        backend = SimpleNamespace(TRAIN_LOCK=threading.Lock(), TRAIN_JOBS={})
        calls = []
        def start(*args, **kwargs):
            calls.append((args, kwargs))
            backend.TRAIN_JOBS["job"] = {"status": "paused", "stage": "paused", "log": []}
            return {"id": "job"}
        backend.start_training = start
        backend.cancel_training = lambda job: None
        config = {"repositoryRoot": str(Path(__file__).resolve().parents[2]), "outputRoot": str(output.parent),
                  "outputScene": "scene", "jobStore": str(self.root / "jobs"), "datasetPath": str(self.root / "dataset"),
                  "backend": "3dgs", "nativeCheckpoint": True, "overwrite": True, "runColmap": True}
        with mock.patch.object(gsw_worker, "import_backend", return_value=backend), context_stdout():
            with mock.patch.object(gsw_worker.threading, "Thread"):
                self.assertEqual(gsw_worker.run_training(config, resume=True), 75)
        args, kwargs = calls[0]
        self.assertFalse(args[3])  # runColmap
        self.assertFalse(args[4])  # overwrite
        self.assertTrue(args[8])   # allow existing output
        self.assertEqual(kwargs["resume_checkpoint_iteration"], 3)
        self.assertTrue(kwargs["native_control"]["resume"])
        self.assertNotIn("resume_checkpoint", kwargs)  # not a legacy PLY/tuple warm start

    def test_changed_identity_rejected_before_pickle_load(self):
        self.store.save(PickleStore, self.state)
        loader = mock.Mock()
        with self.assertRaisesRegex(ValueError, "changed"):
            self.store.load(loader, "different-settings")
        loader.load.assert_not_called()


def context_stdout():
    return mock.patch("sys.stdout", io.StringIO())


class OptimizerResumeTests(unittest.TestCase):
    def test_uninterrupted_and_resumed_adam_are_identical(self):
        try:
            import torch
            import numpy as np
        except ImportError:
            self.skipTest("Run with the configured training Python for tensor-state equivalence")

        class Model:
            def __init__(self):
                self.value = torch.nn.Parameter(torch.tensor([0.3, 0.8]))
                self._exposure = torch.nn.Parameter(torch.tensor([0.5]))
                self.exposure_mapping = {"camera": 0}
                self.pretrained_exposures = None
                self.setup()

            def setup(self):
                self.optimizer = torch.optim.Adam([self.value], lr=0.01)
                self.exposure_optimizer = torch.optim.Adam([self._exposure], lr=0.02)

            def capture(self):
                return self.value, self.optimizer.state_dict()

            def restore(self, state, _opt):
                self.value = state[0]
                self.setup()
                self.optimizer.load_state_dict(state[1])

        def seed(value):
            random.seed(value)
            np.random.seed(value)
            torch.manual_seed(value)

        def advance(model, pending, count):
            for _ in range(count):
                if not pending:
                    pending.extend(range(4))
                camera = pending.pop(random.randrange(len(pending)))
                target = torch.rand(2) + np.random.random() + camera
                loss = ((model.value * model._exposure - target) ** 2).sum()
                loss.backward()
                model.optimizer.step()
                model.exposure_optimizer.step()
                model.optimizer.zero_grad(set_to_none=True)
                model.exposure_optimizer.zero_grad(set_to_none=True)

        seed(42)
        reference = Model()
        advance(reference, [], 10)
        seed(42)
        interrupted, pending = Model(), []
        advance(interrupted, pending, 3)
        state = checkpoint.capture_state(torch, np, interrupted, 3, 10, "test", list(range(4)), pending, 1.2, 0.2, 0.0)
        with tempfile.TemporaryDirectory() as temporary:
            store = checkpoint.TrainingCheckpoint(temporary, {"output": temporary, "session": "test"})
            store.save(torch, state)
            seed(999)  # Fresh process initialization must not change continuation.
            resumed = Model()
            state = store.load(torch, "test")
            checkpoint.restore_state(torch, np, resumed, SimpleNamespace(iterations=10), state, "test", list(range(4)))
            advance(resumed, state["pending_indices"], 7)
        self.assertTrue(torch.equal(reference.value, resumed.value))
        self.assertTrue(torch.equal(reference._exposure, resumed._exposure))
        for optimizer in ("optimizer", "exposure_optimizer"):
            expected = getattr(reference, optimizer).state_dict()["state"][0]
            actual = getattr(resumed, optimizer).state_dict()["state"][0]
            for key in expected:
                self.assertTrue(torch.equal(expected[key], actual[key]))


if __name__ == "__main__":
    unittest.main()
