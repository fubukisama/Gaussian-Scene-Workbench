"""Reusable named conditions at the public export/replay boundary."""
import copy
import json
import shutil
import os
import subprocess
import sys
from pathlib import Path
import unittest

from native.backend.tests import test_training_batch_plan as batch_fixtures
from native.backend.reconstruction_comparison import ComparisonError, export_comparison, verify_comparison
from native.backend import reconstruction_comparison, training_batch_plan


class ReconstructionComparisonTests(unittest.TestCase):
    def setUp(self):
        fixture = batch_fixtures.TrainingBatchPlanTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.root, self.source, self.data = fixture.root, fixture.source, fixture.data
        self.request = {"schema": "gsw.reconstruction_comparison_request/1",
            "datasets": [{"id": "indoor or object", "path": str(self.source),
                "data_identity": self.data["data_identity"]}],
            "conditions": [{"id": name, "label": name, "dataset_id": "indoor or object",
                "training": {"backend": backend, "quality": "quick", "trainingSeed": 42,
                    "trainOptions": {"iterations": 1000, "resolution": 1}},
                "extraction": {"method": "gs_depth", "coordinate_mode": "canonical",
                    "alpha_threshold": threshold, "pixel_stride": 1, "max_points": 1000000,
                    "memory_budget_mb": 256}}
                for name, backend, threshold in (("任意比较 A", "3dgs", .4), ("別の条件", "2dgs", .7))]}

    def test_export_reopen_and_relocation_keep_actual_initial_points_and_all_conditions(self):
        before = (self.source / "camera_dataset.json").read_bytes()
        result = export_comparison(self.request, self.root / "export")
        report = json.loads(Path(result["profilePath"]).read_text(encoding="utf-8"))
        self.assertEqual([c["id"] for c in report["conditions"]], ["任意比较 A", "別の条件"])
        self.assertEqual(len({c["initialization"]["point_sha256"] for c in report["conditions"]}), 1)
        self.assertEqual([c["extraction"]["alpha_threshold"] for c in report["conditions"]], [.4, .7])
        self.assertTrue(all(c["training_configuration"]["posePolicy"] == "locked" for c in report["conditions"]))
        self.assertTrue(all(not c["training_configuration"]["runColmap"] for c in report["conditions"]))
        self.assertEqual(report["conditions"][0]["splits"], self.data["splits"])
        self.assertEqual((self.source / "camera_dataset.json").read_bytes(), before)
        self.assertFalse(any(self.root.glob("**/point_cloud.ply")))
        moved = self.root / "relocated-dataset"
        shutil.copytree(str(self.source), str(moved))
        verified = verify_comparison(result["profilePath"], {"indoor or object": str(moved)})
        self.assertEqual(verified["comparison_sha256"], report["comparison_sha256"])
        self.assertEqual(verified["status"], "inputs_and_configurations_verified_no_jobs_executed")

    def test_mismatched_effective_configs_and_unknown_controls_are_not_silently_reused(self):
        result = export_comparison(self.request, self.root / "export")
        profile = Path(result["profilePath"])
        document = json.loads(profile.read_text(encoding="utf-8"))
        document["conditions"][0]["training_configuration"]["trainingSeed"] = 999
        profile.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(ComparisonError, "comparison_profile_or_inputs_changed"):
            verify_comparison(profile)
        request = copy.deepcopy(self.request)
        request["conditions"][0]["extraction"]["include_test_images"] = True
        with self.assertRaisesRegex(ComparisonError, "comparison_unsupported_extraction_recipe"):
            export_comparison(request, self.root / "invalid")
        self.assertFalse((self.root / "invalid").exists())
        with self.assertRaisesRegex(ComparisonError, "comparison_cancelled"):
            export_comparison(self.request, self.root / "cancelled", cancelled=lambda: True)
        self.assertFalse((self.root / "cancelled").exists())

    def test_public_cli_exports_without_training_and_completes_with_control_pipe_open(self):
        request = self.root / "comparison-request.json"
        request.write_text(json.dumps(self.request), encoding="utf-8")
        repository = Path(__file__).resolve().parents[3]
        command = [sys.executable, "-B", str(repository / "native/worker/reconstruction_comparison.py"),
            "--operation", "export", "--request", str(request), "--output-directory", str(self.root / "cli")]
        child = subprocess.Popen(command, cwd=str(repository), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=dict(os.environ, PYTHONUTF8="1"))
        try:
            self.assertEqual(child.wait(timeout=20), 0, child.stderr.read().decode("utf-8", "replace"))
            result = json.loads(child.stdout.read().decode("utf-8"))
            self.assertEqual(result["conditionCount"], 2)
            self.assertTrue(Path(result["profilePath"]).is_file())
            self.assertFalse(any(self.root.glob("**/point_cloud.ply")))
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()

    def test_actual_repository_algorithm_changes_invalidate_recipe_replay(self):
        repository = Path(__file__).resolve().parents[3]
        copied = self.root / "owned-source-copy"
        for relative in set(training_batch_plan._SOURCE_FILES) | set(reconstruction_comparison.REPRODUCTION_SOURCE_FILES):
            target = copied / relative; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(str(repository / relative), str(target))
        for relative in reconstruction_comparison.REPRODUCTION_SOURCE_DIRECTORIES:
            shutil.copytree(str(repository / relative), str(copied / relative),
                ignore=shutil.ignore_patterns('build', 'dist', '__pycache__', '.git', '*.pyd', '*.pyc'))
        result = export_comparison(self.request, self.root / "pinned", repository_root=copied)
        for relative in ("gaussian-splatting/scene/gaussian_model.py", "gaussian-splatting/gaussian_renderer/__init__.py",
                         "native/worker/camera_rasterizer_runtime.py"):
            source = copied / relative; original = source.read_bytes()
            source.write_bytes(original + b"\n# acceptance-owned algorithm mutation\n")
            with self.assertRaisesRegex(ComparisonError, "comparison_profile_or_inputs_changed"):
                verify_comparison(result["profilePath"], repository_root=copied)
            source.write_bytes(original)
        self.assertTrue(verify_comparison(result["profilePath"], repository_root=copied)["success"])


if __name__ == "__main__":
    unittest.main()
