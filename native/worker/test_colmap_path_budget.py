"""Public COLMAP admission rejects invalid paths before destructive work."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from PIL import Image

from native.backend import compute
from native.backend.camera_dataset import CameraDatasetError


class ColmapPathBudgetTests(unittest.TestCase):
    def setUp(self):
        base = Path("D:/Apps/GSW/validation/acceptance-followup-oct22/paths/colmap-unit")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    @unittest.skipUnless(os.name == "nt", "legacy Windows path budget")
    def test_invalid_generated_paths_fail_before_reset_or_external_process(self):
        dataset = self.root / ("p" * (197 - len(str(self.root))))
        (dataset / "input").mkdir(parents=True)
        Image.new("RGB", (11, 9), (20, 40, 60)).save(dataset / "input/v00000000.png")
        database = dataset / "database.db"
        database.write_bytes(b"existing solver cache")
        cache = dataset / "distorted/keep.bin"
        cache.parent.mkdir()
        cache.write_bytes(b"existing disposable cache not yet admitted for reset")
        published = dataset / "sparse/0/keep.bin"
        published.parent.mkdir(parents=True)
        published.write_bytes(b"previous reconstruction")
        failure = None
        with mock.patch.object(subprocess, "Popen", side_effect=AssertionError("external process must not start")) as launch:
            try:
                compute.run_colmap_convert({"id": "path-budget", "log": []}, dataset,
                                          compute.colmap_options_from_payload({"reset": True}))
            except Exception as error:
                failure = error
        self.assertTrue(cache.exists(), "path rejection must precede destructive reset")
        self.assertEqual(cache.read_bytes(), b"existing disposable cache not yet admitted for reset")
        self.assertEqual(database.read_bytes(), b"existing solver cache")
        self.assertEqual(published.read_bytes(), b"previous reconstruction")
        self.assertIsInstance(failure, CameraDatasetError)
        self.assertEqual(failure.code, "camera_solve_path_budget_exceeded")
        launch.assert_not_called()

    def test_read_only_layout_uses_short_owned_names_and_retains_full_job_identity(self):
        dataset = self.root / ("p" * (123 - len(str(self.root)))) / ("result" + "r" * 26)
        (dataset / "input").mkdir(parents=True)
        Image.new("RGB", (11, 9), (20, 40, 60)).save(dataset / "input/v00000000.png")
        job_id = "利用者の任意ID-" + "experiment-" * 30
        before = sorted(path.relative_to(dataset).as_posix() for path in dataset.rglob("*"))
        plan = compute.plan_colmap_path_layout({"id": job_id}, dataset, nonce="a1b2c3d4e5f6")
        self.assertEqual(plan["jobId"], job_id)
        self.assertEqual(plan["nonce"], "a1b2c3d4e5f6")
        for path in plan["files"]:
            self.assertLessEqual(len(path.encode("utf-16-le")) // 2, 259)
            self.assertNotIn(job_id, path)
        for path in plan["directories"]:
            self.assertLessEqual(len(path.encode("utf-16-le")) // 2, 247)
        self.assertEqual(plan["snapshotChildBound"]["digits"], 20)
        self.assertIn("bd1fcf6", plan["snapshotChildBound"]["source"])
        self.assertEqual(before, sorted(path.relative_to(dataset).as_posix() for path in dataset.rglob("*")))
        other = compute.plan_colmap_path_layout({"id": job_id}, dataset, nonce="f6e5d4c3b2a1")
        self.assertTrue(set(plan["paths"].values()).isdisjoint(other["paths"].values()))

    def test_layout_collision_never_owns_or_removes_an_existing_run_directory(self):
        dataset = self.root / "dataset"
        (dataset / "input").mkdir(parents=True)
        initial = compute.plan_colmap_path_layout({"id": "job"}, dataset, nonce="planned")
        marker = Path(initial["paths"]["undistort"]) / "foreign.bin"
        marker.parent.mkdir()
        marker.write_bytes(b"another run's directory")
        with self.assertRaises(CameraDatasetError) as raised:
            compute.plan_colmap_path_layout({"id": "job"}, dataset, nonce="planned")
        self.assertEqual(raised.exception.code, "camera_solve_destination_exists")
        self.assertEqual(marker.read_bytes(), b"another run's directory")

    @unittest.skipUnless(os.name == "nt", "historical Windows root path boundary")
    def test_historical_normal_root_fits_without_relaxing_legacy_limits(self):
        dataset = self.root / ("p" * (158 - len(str(self.root))))
        (dataset / "input").mkdir(parents=True)
        Image.new("RGB", (11, 9), (20, 40, 60)).save(dataset / "input/v00000000.png")
        self.assertEqual(len(str(dataset)), 159)
        plan = compute.plan_colmap_path_layout({"id": "full-task-identity"}, dataset)
        self.assertTrue(all(len(path.encode("utf-16-le")) // 2 <= 259 for path in plan["files"]))
        self.assertTrue(all(len(path.encode("utf-16-le")) // 2 <= 247 for path in plan["directories"]))

    def test_retry_uses_fresh_layout_and_keeps_prior_preview_and_identity_record(self):
        dataset = self.root / "retry-dataset"
        (dataset / "input").mkdir(parents=True)
        Image.new("RGB", (11, 9), (20, 40, 60)).save(dataset / "input/v00000000.png")
        job = {"id": "retry-task-full-identity", "log": []}
        options = compute.colmap_options_from_payload({"reset": False})
        with mock.patch.dict(os.environ, {"COLMAP_EXE": sys.executable}), \
             mock.patch.object(subprocess, "Popen", side_effect=OSError("controlled external process boundary")):
            with self.assertRaisesRegex(OSError, "controlled external process boundary"):
                compute.run_colmap_convert(job, dataset, options)
            previous = job["colmapPathLayout"]
            old_preview = Path(previous["paths"]["preview"]) / "sparse_000001.ply"
            old_preview.parent.mkdir()
            old_preview.write_bytes(b"previous attempt preview artifact")
            job["partial_point_cloud_path"] = str(old_preview)
            with self.assertRaisesRegex(OSError, "controlled external process boundary"):
                compute.run_colmap_convert(job, dataset, options)
        current = job["colmapPathLayout"]
        self.assertNotEqual(current["nonce"], previous["nonce"])
        self.assertEqual(current["jobId"], previous["jobId"])
        self.assertTrue(set(current["paths"].values()).isdisjoint(previous["paths"].values()))
        self.assertEqual(job["colmapPathHistory"][-1]["paths"]["preview"], previous["paths"]["preview"])
        self.assertEqual(job["partial_point_cloud_path"], str(old_preview))
        self.assertEqual(old_preview.read_bytes(), b"previous attempt preview artifact")


if __name__ == "__main__":
    unittest.main()
