import contextlib
import io
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from native.worker import gsw_worker


class FakeBackend:
    def __init__(self, state="done", error=None):
        self.OUTPUT_DIR = None
        self.TRAIN_JOBS_DIR = None
        self.TRAIN_LOCK = threading.Lock()
        self.TRAIN_JOBS = {}
        self.state = state
        self.error = error
        self.call = None

    def start_training(self, *args, **kwargs):
        self.call = (args, kwargs)
        self.TRAIN_JOBS["test-job"] = {
            "status": self.state,
            "stage": self.state,
            "error": self.error,
            "log": ["backend log"],
        }
        return {"id": "test-job"}


class WorkerTests(unittest.TestCase):
    def configuration(self, root):
        return {
            "repositoryRoot": str(root),
            "datasetPath": str(root / "dataset"),
            "outputRoot": str(root / "output"),
            "outputScene": "test-scene",
            "backend": "3dgs",
            "quality": "quick",
            "jobStore": str(root / "jobs"),
            "runColmap": True,
            "overwrite": False,
            "trainOptions": {"iterations": 7000, "resolution": 8},
        }

    def test_load_configuration_rejects_missing_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "job.json"
            path.write_text('{"backend": "3dgs"}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Missing worker configuration fields"):
                gsw_worker.load_configuration(path)

    def test_run_training_forwards_native_configuration_and_succeeds(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backend = FakeBackend()
            idle_thread = mock.Mock()
            idle_thread.start = mock.Mock()
            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                mock.patch.object(gsw_worker.threading, "Thread", return_value=idle_thread),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_training(self.configuration(root))

            self.assertEqual(exit_code, 0)
            self.assertEqual(backend.OUTPUT_DIR, (root / "output").resolve())
            self.assertEqual(backend.TRAIN_JOBS_DIR, (root / "jobs").resolve())
            self.assertEqual(backend.call[1]["dataset_path"], str(root / "dataset"))
            self.assertEqual(backend.call[0][1], "test-scene")

    def test_run_training_returns_failure_for_failed_backend_job(self):
        with tempfile.TemporaryDirectory() as temporary:
            backend = FakeBackend(state="failed", error="training failed")
            idle_thread = mock.Mock()
            idle_thread.start = mock.Mock()
            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                mock.patch.object(gsw_worker.threading, "Thread", return_value=idle_thread),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_training(self.configuration(Path(temporary)))

            self.assertEqual(exit_code, 1)


if __name__ == "__main__":
    unittest.main()
