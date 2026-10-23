"""CPU tests at the real COLMAP inspection/preparation seam; no CUDA or mocks."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import prepare_quality_real_fixture as fixture


class RealFixturePreparationTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get("GSW_QUALITY_TEST_TMP", "D:/Codex/validation/gsw-quality-upgrade-20261023/cpu-tests"))
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=str(parent))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        (self.source / "images").mkdir(parents=True)
        model = self.source / "sparse" / "0"
        model.mkdir(parents=True)
        (model / "cameras.txt").write_text("1 PINHOLE 40 30 30 30 20 15\n", encoding="utf-8")
        names = ["view-z.png", "003.png", "camera-a.png", "next 9.png", "last.png"]
        records = []
        for i, name in enumerate(names):
            Image.new("RGB", (40, 30), (10 + i, 70 + i, 130 + i)).save(self.source / "images" / name)
            records.append("%d 1 0 0 0 %.4f 0 0 1 %s\n\n" % (i + 1, .01 * i, name))
        (model / "images.txt").write_text("".join(records), encoding="utf-8")
        (model / "points3D.txt").write_text("1 -0.2 0 3 255 0 0 0\n2 0.2 0 3 0 255 0 0\n3 0 0.2 3 0 0 255 0\n", encoding="utf-8")
        self.binding = {"dataset_root": str(self.source), "metadata_image_count": 5,
                        "authorization": "explicit_test_fixture_authorization"}
        self.output = self.root / "owned-new-version"

    def test_inspection_is_read_only_deterministic_and_discloses_geometry_leakage(self):
        before = {p.relative_to(self.source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in self.source.rglob("*") if p.is_file()}
        result = fixture.inspect_fixture(self.binding, binding_id="local:test", val_count=1, test_count=1)
        self.assertEqual("ready", result["status"])
        self.assertEqual([3, 1, 1], result["split_counts"])
        self.assertEqual(5, result["image_count"])
        self.assertEqual(3, result["initialization"]["point_count"])
        self.assertEqual("arbitrary", result["scale"]["status"])
        self.assertIsNone(result["scale"]["meters_per_unit"])
        self.assertFalse(result["evaluation_provenance"]["independent_pose_or_geometry"])
        self.assertFalse(result["evaluation_provenance"]["pristine_final_test"])
        self.assertEqual("heldout_from_GS_loss_only", result["evaluation_provenance"]["heldout_role"])
        again = fixture.inspect_fixture(self.binding, binding_id="local:test", val_count=1, test_count=1)
        self.assertEqual(result["split_by_path"], again["split_by_path"])
        self.assertEqual(before, {p.relative_to(self.source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in self.source.rglob("*") if p.is_file()})
        self.assertFalse(self.output.exists())

    def test_prepare_copies_bytes_and_freezes_real_prepared_contract_without_gpu(self):
        from native.worker.canonical_training import load_prepared_training_dataset
        result = fixture.prepare_fixture(self.binding, self.output, binding_id="local:test", val_count=1, test_count=1)
        self.assertEqual("prepared_not_trained", result["status"])
        self.assertTrue(result["source_unchanged"])
        self.assertEqual([3, 1, 1], result["split_counts"])
        self.assertEqual([17, 29], result["training_seeds"])
        self.assertEqual("not_run", result["acceptance"]["GPU"])
        self.assertEqual("unbound", result["run_parameters"]["status"])
        self.assertIsNone(result["ROI_reference"])
        self.assertIsNone(result["quality_thresholds"])
        prepared = load_prepared_training_dataset(self.output / result["prepared_manifest"], self.output)
        self.assertEqual(3, len(prepared["initial_points"]))
        with np.load(self.output / result["prepared_initialization"], allow_pickle=False) as actual:
            np.testing.assert_array_equal(actual["colors"], [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
        for row in result["source_inventory"]:
            copied = self.output / row["path"]
            self.assertEqual(row["sha256"], hashlib.sha256(copied.read_bytes()).hexdigest())
            self.assertNotEqual(copied.stat().st_ino, (self.source / row["path"]).stat().st_ino)
        self.assertNotIn("torch", sys.modules)
        with self.assertRaisesRegex(ValueError, "fresh_output_required"):
            fixture.prepare_fixture(self.binding, self.output, binding_id="local:test", val_count=1, test_count=1)

    def test_invalid_contract_and_budget_fail_before_any_output(self):
        with self.assertRaisesRegex(ValueError, "authorization_required"):
            fixture.prepare_fixture(dict(self.binding, authorization=None), self.output)
        with self.assertRaisesRegex(ValueError, "binding_image_count_mismatch"):
            fixture.prepare_fixture(dict(self.binding, metadata_image_count=59), self.output)
        with self.assertRaisesRegex(ValueError, "fixture_budget_blocked:copy_budget_exceeded"):
            fixture.prepare_fixture(self.binding, self.output, max_copy_bytes=1)
        self.assertFalse(self.output.exists())
        (self.source / "sparse/0/cameras.txt").write_text("1 OPENCV 40 30 30 30 20 15 0 0 0 0\n", encoding="utf-8")
        with self.assertRaisesRegex(Exception, "camera_unsupported_projection"):
            fixture.prepare_fixture(self.binding, self.output)
        self.assertFalse(self.output.exists())

    def test_exact_generic_manifest_preserves_existing_split_and_rejects_wrong_version(self):
        from native.backend import camera_dataset
        split = {"images/003.png": "train", "images/view-z.png": "train", "images/camera-a.png": "train",
                 "images/next 9.png": "val", "images/last.png": "test"}
        dataset = camera_dataset.load_camera_dataset(self.source, adapter="colmap", options={"split_by_path": split})
        dataset["source"]["kind"] = "synthetic"
        dataset = camera_dataset.refresh_dataset_identity(dataset)
        camera_dataset.write_camera_dataset(dataset, self.source / "camera_dataset.json")
        binding = dict(self.binding, adapter="generic_json", camera_manifest="camera_dataset.json",
                       manifest_sha256=hashlib.sha256((self.source / "camera_dataset.json").read_bytes()).hexdigest(),
                       data_identity=dataset["data_identity"])
        with self.assertRaisesRegex(ValueError, "approved_manifest_identity_mismatch"):
            fixture.inspect_fixture(dict(binding, manifest_sha256="0" * 64))
        with self.assertRaisesRegex(ValueError, "generic_existing_split_must_be_preserved"):
            fixture.inspect_fixture(binding, val_count=1, test_count=1)
        inspected = fixture.inspect_fixture(binding)
        self.assertEqual(split, inspected["split_by_path"])
        self.assertIsNone(inspected["split_seed"])
        self.assertEqual("synthetic", inspected["source_kind"])
        result = fixture.prepare_fixture(binding, self.output)
        self.assertEqual([3, 1, 1], result["split_counts"])
        self.assertTrue(result["source_unchanged"])
        self.assertEqual(binding["manifest_sha256"], hashlib.sha256(
            (self.output / "frozen-source/camera_dataset.json").read_bytes()).hexdigest())
        loaded = camera_dataset.load_camera_dataset(self.output / "camera_dataset.json")
        self.assertEqual(dataset["splits"], loaded["splits"])

    def test_overlap_missing_points_and_duplicate_content_are_not_silently_admitted(self):
        with self.assertRaisesRegex(ValueError, "source_output_overlap"):
            fixture.prepare_fixture(self.binding, self.source / "nested")
        points = self.source / "sparse/0/points3D.txt"
        points.unlink()
        with self.assertRaisesRegex(ValueError, "actual_SfM_initialization_required"):
            fixture.prepare_fixture(self.binding, self.output)
        self.assertFalse(self.output.exists())
        points.write_text("1 0 0 3 0 255 0 0\n", encoding="utf-8")
        payload = (self.source / "images/003.png").read_bytes()
        for path in (self.source / "images").iterdir():
            path.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, "cross_split_duplicate_RGB_content"):
            fixture.prepare_fixture(self.binding, self.output)
        self.assertFalse(self.output.exists())


class AcceptanceTraceTests(unittest.TestCase):
    def test_all_42_cases_and_18_requirements_are_traced_without_partial_promotion(self):
        report = json.loads((ROOT / "docs/quality_upgrade/ACCEPTANCE_REPORT.json").read_text(encoding="utf-8"))
        rows = report["test_results"]
        self.assertEqual(["QT%03d" % i for i in range(1, 43)], [row["id"] for row in rows])
        requirements = report["requirement_results"]
        self.assertEqual(["Q%02d" % i for i in range(1, 19)], [row["id"] for row in requirements])
        evidence = set(report["evidence_registry"])
        for row in rows:
            self.assertTrue(row["reason"])
            self.assertTrue(set(row["evidence_ids"]) <= evidence)
            if row["remaining_gates"]:
                self.assertNotEqual("pass", row["status"])
            for requirement in row["requirement_ids"]:
                self.assertIn(row["id"], next(r for r in requirements if r["id"] == requirement)["test_ids"])
        self.assertFalse(report["default_promotion"]["allowed"])
        self.assertNotEqual("pass", report["appearance_acceptance"]["status"])
        self.assertEqual("unmeasured", report["geometry_acceptance"]["status"])
        self.assertIsNone(report["installed_build"])
        serialized = json.dumps(report, ensure_ascii=False)
        self.assertNotIn("建築モデル-20261008.gswjson.files", serialized)
        self.assertNotIn("D:/張", serialized)


if __name__ == "__main__":
    unittest.main()
