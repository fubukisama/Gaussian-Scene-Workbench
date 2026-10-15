"""Native process contracts using bounded, test-owned Python children."""

import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

from native.backend import compute


RECEIVER = """
import json, pathlib, sys, time
pathlib.Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:], ensure_ascii=False), encoding='utf-8')
print('owned receiver ready', flush=True)
if '--fail' in sys.argv:
    print('owned failure', flush=True)
    sys.exit(23)
if '--wait' in sys.argv:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        print('owned receiver heartbeat', flush=True)
        time.sleep(.02)
"""


class CommandSecurityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-native-command-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "模型 space & 100% project"
        self.root.mkdir()
        self.receiver = self.root / "receiver.py"
        self.receiver.write_text(RECEIVER, encoding="utf-8")
        self.result = self.root / "argv.json"
        self.job = {
            "id": "owned-command", "kind": "mesh", "scene": "fixture",
            "iteration": 1, "mode": "bounded", "status": "running",
            "stage": "mesh", "created_at": time.time(), "updated_at": time.time(),
            "cancel_requested": False, "process": None, "log": [],
        }
        self.environment = mock.patch.object(compute, "training_env", return_value=os.environ.copy())
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.addCleanup(compute.MESH_JOBS.pop, self.job["id"], None)

    def command(self, *arguments):
        return [sys.executable, "-u", self.receiver, self.result, *arguments]

    def test_native_command_preserves_literal_argv_for_both_backends(self):
        operands = ["--leading-dash", str(self.root / "入力 & output 100%.ply"),
                    "quoted \"name\"", "x; echo not-a-shell-command", "日本語 中文"]
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                self.assertEqual(compute.run_logged(self.job, self.command(*operands), self.root, backend), 0)
                self.assertEqual(json.loads(self.result.read_text(encoding="utf-8")), operands)
                self.assertIsNone(self.job["process"])

    def test_native_command_failure_is_reported_and_preserves_existing_artifacts(self):
        artifact = self.root / "previous-model.ply"
        artifact.write_bytes(b"existing model")
        with self.assertRaisesRegex(RuntimeError, "exit code 23"):
            compute.run_logged(self.job, self.command("--fail"), self.root, "3dgs")
        self.assertIn("owned failure", self.job["log"])
        self.assertEqual(artifact.read_bytes(), b"existing model")
        self.assertIsNone(self.job["process"])

    def test_cancel_stops_only_the_owned_child_and_clears_the_handle(self):
        compute.MESH_JOBS[self.job["id"]] = self.job
        observed, errors = [], []

        def cancel_owned():
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                process = self.job.get("process")
                if process is not None:
                    observed.append(process)
                    try:
                        compute.cancel_mesh_job(self.job["id"])
                    except Exception as exc:
                        errors.append(exc)
                    return
                time.sleep(.01)

        thread = threading.Thread(target=cancel_owned)
        thread.start()
        try:
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                compute.run_logged(self.job, self.command("--wait"), self.root, "2dgs")
        finally:
            thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(len(observed), 1)
        self.assertIsNotNone(observed[0].poll())
        self.assertIsNone(self.job["process"])
        self.assertTrue(self.job["cancel_requested"])


if __name__ == "__main__":
    unittest.main()
