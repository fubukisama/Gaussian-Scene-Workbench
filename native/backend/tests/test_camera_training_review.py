"""Public queue admission must reject destructive/incompatible camera resumes.

The OS thread-launch seam is blocked so RED cannot start real training. Source
data, jobs and outputs are test-owned; runtime/CUDA probes are never reached.
"""

from pathlib import Path
import tempfile
import threading
import unittest
import sys
from unittest import mock

from native.backend import compute


class CameraTrainingReviewTests(unittest.TestCase):
    def setUp(self):
        base = Path("D:/Codex/qa-camera-job-review-p0")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.previous_jobs = compute.TRAIN_JOBS_DIR
        self.previous_output = compute.OUTPUT_DIR
        compute.TRAIN_JOBS_DIR = self.root / "jobs"
        compute.TRAIN_JOBS_DIR.mkdir()
        self.addCleanup(setattr, compute, "TRAIN_JOBS_DIR", self.previous_jobs)
        compute.OUTPUT_DIR = self.root / "output"
        compute.OUTPUT_DIR.mkdir()
        self.addCleanup(setattr, compute, "OUTPUT_DIR", self.previous_output)

    def reject_before_background_launch(self, **options):
        configuration = dict(run_convert=False, pose_policy="locked",
                             camera_dataset_path=str(self.root / "camera_dataset.json"))
        configuration.update(options)
        # threading is an external execution seam, not a replacement for any
        # camera parser, policy, normalization or optimizer implementation.
        with mock.patch.object(threading, "Thread", side_effect=RuntimeError("unexpected_thread_creation")):
            with self.assertRaises(ValueError):
                compute.start_training("camera-source", "camera-output", **configuration)

    def test_native_resume_cannot_overwrite_the_output_that_contains_its_checkpoint(self):
        self.reject_before_background_launch(overwrite=True,
            native_control={"resume": True, "output": str(self.root / "output")})

    def test_native_resume_cannot_request_camera_resolving(self):
        self.reject_before_background_launch(run_convert=True,
            native_control={"resume": True, "output": str(self.root / "output")})

    def test_locked_dataset_cannot_accept_a_legacy_checkpoint_without_camera_scale_identity(self):
        self.reject_before_background_launch(resume_checkpoint=self.root / "legacy-optimizer.pth")

    def test_direct_job_revalidates_legacy_checkpoint_policy_before_reading_camera_inputs(self):
        job = {"id": "owned-direct-review", "backend": "3dgs", "scene": "camera-source", "output_scene": "camera-output",
               "pose_policy": "locked", "camera_dataset_path": str(self.root / "missing-camera.json"),
               "dataset_path": str(self.root / "dataset"), "resume_checkpoint": str(self.root / "legacy.pth"),
               "cancel_requested": False, "log": []}
        compute.run_training_job(job, False, "quick", False)
        self.assertEqual(job["status"], "failed")
        self.assertIn("camera_locked_legacy_checkpoint_forbidden", job["error"])

    def test_cancelled_logged_command_never_launches_even_a_harmless_owned_child(self):
        marker = self.root / "child-was-started.txt"
        job = {"id": "owned-cancel-review", "backend": "3dgs", "cancel_requested": True, "log": []}
        command = [sys.executable, "-c", "from pathlib import Path; Path(" + repr(str(marker)) + ").write_text('launched')"]
        with self.assertRaisesRegex(RuntimeError, "cancelled"):
            compute.run_logged(job, command, compute.ROOT)
        self.assertFalse(marker.exists())

    def test_canonical_input_missing_explicit_policy_cannot_implicitly_start_sfm(self):
        source = self.root / "canonical-source"
        source.mkdir()
        (source / "camera_dataset.json").write_text('{"schema":"gsw.camera_dataset/1"}', encoding="utf-8")
        self.reject_before_background_launch(pose_policy=None, dataset_path=str(source), camera_dataset_path=None)


if __name__ == "__main__":
    unittest.main()
