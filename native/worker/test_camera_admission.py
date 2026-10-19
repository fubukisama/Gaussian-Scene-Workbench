"""Locked/solve policy at public dataset admission and probe CLI seams."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from PIL import Image

from native.backend import camera_admission
from native.backend.camera_dataset import CameraDatasetError
from native.worker import training_preflight, training_reconstruction_summary


class CameraAdmissionTests(unittest.TestCase):
    def setUp(self):
        base = Path("D:/Codex/qa-camera-admission-p0")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repository = Path(__file__).resolve().parents[2]
        self.dataset = self.root / "dataset"
        self.dataset.mkdir()
        views = []
        for index, split in enumerate(("train", "train", "val", "test")):
            path = self.dataset / split / (str(index) + ".png")
            path.parent.mkdir(exist_ok=True)
            Image.new("RGB", (16, 12), (10 + index, 20, 30)).save(path)
            views.append({"image_path": path.relative_to(self.dataset).as_posix(),
                "K": [[20., 0., 8.], [0., 21., 6.], [0., 0., 1.]],
                "pixel_coordinates": "pixel_border", "split": split,
                "c2w_opencv": [[1., 0., 0., index * .1], [0., 1., 0., 0.],
                               [0., 0., 1., -.5], [0., 0., 0., 1.]]})
        self.manifest = self.dataset / "camera_dataset.json"
        self.manifest.write_text(json.dumps({"schema": "gsw.camera_dataset/1", "pose_policy": "locked",
            "source": {"kind": "synthetic"}, "scale": {"status": "arbitrary"}, "views": views}), encoding="utf-8")

    def test_locked_admission_needs_no_flat_images_sparse_or_colmap(self):
        admitted = camera_admission.admit_locked_dataset(self.dataset)
        self.assertEqual(admitted["root"], str(self.dataset))
        self.assertEqual(admitted["manifestPath"], str(self.manifest))
        self.assertEqual(admitted["cameraDataset"]["counts"], {"train": 2, "val": 1, "test": 1})
        self.assertEqual(admitted["cameraDataset"]["posePolicy"], "locked")
        with self.assertRaises(CameraDatasetError) as raised:
            camera_admission.admit_locked_dataset(self.dataset, run_colmap=True)
        self.assertEqual(raised.exception.code, "camera_locked_colmap_conflict")
        with self.assertRaises(CameraDatasetError) as raised:
            camera_admission.admit_locked_dataset(self.dataset, expected_identity="wrong-version")
        self.assertEqual(raised.exception.code, "camera_data_identity_mismatch")

    def test_summary_cli_locked_mode_reports_all_splits_and_never_falls_back(self):
        completed = subprocess.run([sys.executable, "-B", str(self.repository / "native/worker/training_reconstruction_summary.py"),
            "--backend-root", str(self.repository), "--dataset", str(self.dataset), "--pose-policy", "locked"],
            cwd=str(self.root), capture_output=True, text=True, encoding="utf-8", timeout=20,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual(report["version"], 1)
        self.assertFalse(report["effectiveRunColmap"])
        self.assertEqual(report["sourceKind"], "canonical")
        self.assertEqual(report["cameraDataset"]["counts"]["test"], 1)
        (self.dataset / "test/3.png").unlink()
        blocked = training_reconstruction_summary.summarize_reconstruction(self.dataset, pose_policy="locked")
        self.assertTrue(blocked["blocked"])
        self.assertFalse(blocked["effectiveRunColmap"])
        self.assertEqual(blocked["cameraDataset"]["errors"][0]["code"], "camera_missing_image")

    def test_explicit_solve_plans_a_new_train_only_version_and_does_not_claim_sfm_already_passed(self):
        report = training_reconstruction_summary.summarize_reconstruction(self.dataset, pose_policy="solve")
        self.assertEqual(report["sourceKind"], "canonical_solve")
        self.assertEqual(report["decision"], "new_solve_version")
        self.assertFalse(report["blocked"])
        self.assertTrue(report["effectiveRunColmap"])
        self.assertFalse(report["minimumViabilityPassed"])
        self.assertEqual(report["inputImages"], 2)
        self.assertIsNone(report["registeredImages"])
        self.assertEqual(report["cameraDataset"]["posePolicy"], "solve")
        self.assertEqual(report["cameraDataset"]["scale"]["status"], "arbitrary")
        self.assertEqual(report["cameraDataset"]["sourceCounts"], {"train": 2, "val": 1, "test": 1})
        self.assertFalse((self.dataset / "input").exists())
        self.assertFalse((self.dataset / "sparse").exists())

    def test_locked_runtime_probe_still_runs_cuda_probe_but_no_colmap_command(self):
        runtime = self.root / "runtime"
        runtime.mkdir()
        python = runtime / "python.exe"
        python.write_bytes(b"external-process-boundary-fixture")
        environment = {"GAUSSIAN_SPLATTING_CONDA_PREFIX": str(runtime), "GS_CONDA_PREFIX": str(runtime),
                       "COLMAP_EXE": str(self.root / "missing-colmap.exe"),
                       "COLMAP_PATH": str(self.root / "missing-colmap.exe")}
        two_dgs = self.root / "two-dgs-runtime-source"
        two_dgs.mkdir()
        (two_dgs / "train.py").write_text("# runtime source presence fixture", encoding="utf-8")
        environment.update(TWO_DGS_DIR=str(two_dgs), TWO_DGS_PYTHON=str(python))
        commands = []

        def external_process(command, **options):
            commands.append(command)
            self.assertIn("-c", command)
            self.assertIn("torch.cuda.is_available", command[-1])
            return subprocess.CompletedProcess(command, 0, json.dumps({"torch": "boundary-fixture",
                "cuda": "boundary-fixture", "device": "boundary-fixture"}), "")

        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend), mock.patch.dict(os.environ, environment), mock.patch.object(
                    subprocess, "run", side_effect=external_process):
                report = training_preflight.probe_training_environment(self.repository, self.dataset, backend,
                                                                       pose_policy="locked")
                self.assertTrue(report["ready"], report)
                self.assertEqual(report["imageCount"], 4)
                self.assertFalse(report["colmapRequired"])
        self.assertEqual(len(commands), 2)

    def test_explicit_new_solve_version_copies_only_train_images_and_records_split_mapping(self):
        destination = self.root / "new-solve-version"
        admitted = camera_admission.admit_locked_dataset(self.dataset)
        result = camera_admission.prepare_solve_dataset_version(self.dataset, destination,
            expected_identity=admitted["dataset"]["data_identity"])
        self.assertEqual(result["datasetPath"], str(destination))
        self.assertEqual(len(list((destination / "input").iterdir())), 2)
        provenance = json.loads((destination / "camera-solve-provenance.json").read_text(encoding="utf-8"))
        self.assertEqual(provenance["posePolicy"], "solve")
        self.assertEqual(provenance["sourceDataIdentity"], admitted["dataset"]["data_identity"])
        self.assertEqual([entry["viewId"] for entry in provenance["files"]],
                         admitted["dataset"]["splits"]["train"])
        for entry in provenance["files"]:
            self.assertEqual(entry["sourceSplit"], "train")
            self.assertEqual((destination / entry["inputImagePath"]).read_bytes(),
                             (self.dataset / entry["sourceImagePath"]).read_bytes())
            self.assertFalse(os.path.samefile(destination / entry["inputImagePath"],
                                             self.dataset / entry["sourceImagePath"]))
        self.assertEqual(provenance["excludedSplits"], {"val": admitted["dataset"]["splits"]["val"],
                                                       "test": admitted["dataset"]["splits"]["test"]})
        self.assertFalse((destination / "camera_dataset.json").exists())
        self.assertEqual(camera_admission.admit_locked_dataset(self.dataset)["dataset"]["data_identity"],
                         admitted["dataset"]["data_identity"])

    def test_new_solve_version_refuses_existing_destination_or_cancellation(self):
        destination = self.root / "new-solve-version"
        with self.assertRaises(CameraDatasetError) as raised:
            camera_admission.prepare_solve_dataset_version(self.dataset, destination, cancelled=lambda: True)
        self.assertEqual(raised.exception.code, "camera_import_cancelled")
        self.assertFalse(destination.exists())
        destination.mkdir()
        marker = destination / "existing.bin"
        marker.write_bytes(b"owned previous data")
        with self.assertRaises(CameraDatasetError):
            camera_admission.prepare_solve_dataset_version(self.dataset, destination)
        self.assertEqual(marker.read_bytes(), b"owned previous data")

    def test_preflight_cli_records_manual_resolution_and_conflict_stops_before_runtime(self):
        completed = subprocess.run([sys.executable, "-B", str(self.repository / "native/worker/training_preflight.py"),
            "--backend-root", str(self.repository), "--dataset", str(self.dataset), "--pose-policy", "locked",
            "--run-colmap", "--resolution-policy", "manual", "--resolution", "2"],
            cwd=str(self.root), capture_output=True, text=True, encoding="utf-8", timeout=20,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        report = json.loads(completed.stdout)
        self.assertFalse(report["ready"])
        self.assertEqual(report["errorCode"], "camera_locked_colmap_conflict")
        self.assertEqual(report["resolutionPolicy"], "manual")
        self.assertEqual(report["resolution"], 2)


if __name__ == "__main__":
    unittest.main()
