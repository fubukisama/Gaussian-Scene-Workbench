"""Environment admission at the external-process seam, without launching CUDA."""
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from native.worker import training_preflight
from native.worker.test_training_reconstruction_summary import binary_model


class TrainingPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.repository = Path(__file__).resolve().parents[2]
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.python = self.runtime / "python.exe"
        self.python.write_bytes(b"owned-executable-fixture")
        self.colmap = self.runtime / "colmap.exe"
        self.colmap.write_bytes(b"owned-executable-fixture")
        self.two_dgs = self.root / "2dgs"
        self.two_dgs.mkdir()
        (self.two_dgs / "train.py").write_text("# source-presence fixture\n")
        self.dataset = self.root / "dataset"
        (self.dataset / "images").mkdir(parents=True)
        for index in range(3):
            (self.dataset / "images" / ("frame%d.png" % index)).write_bytes(b"header-fixture")
        self.environment = {
            "GAUSSIAN_SPLATTING_CONDA_PREFIX": str(self.runtime),
            "GS_CONDA_PREFIX": str(self.runtime),
            "TWO_DGS_PYTHON": str(self.python),
            "TWO_DGS_DIR": str(self.two_dgs),
            "COLMAP_PATH": str(self.colmap), "COLMAP_EXE": str(self.colmap)}

    def tearDown(self):
        self.temporary.cleanup()

    def test_reusing_valid_cache_does_not_probe_blocked_colmap_or_restore_dataset(self):
        binary_model(self.dataset / ".alignment_cache" / "sparse" / "0")
        launches = []
        def external_process(command, **_options):
            launches.append(command)
            if command[-1] == "-h":
                return subprocess.CompletedProcess(command, 0xC0E90002, "", "application control blocked")
            return subprocess.CompletedProcess(command, 0, json.dumps({"torch": "fixture", "cuda": "fixture", "device": "fixture"}), "")
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend), mock.patch.dict(os.environ, self.environment), mock.patch.object(subprocess, "run", side_effect=external_process):
                report = training_preflight.probe_training_environment(self.repository, self.dataset, backend)
            self.assertTrue(report["ready"], report)
            self.assertFalse(report["colmapRequired"])
            self.assertFalse(report["hasReconstruction"])
            self.assertFalse((self.dataset / "sparse").exists())
        self.assertFalse(any(command[-1] == "-h" for command in launches))

    def test_effective_rebuild_conditions_probe_colmap_for_both_training_backends(self):
        for backend in ("3dgs", "2dgs"):
            for condition in ("missing", "raw_input", "repair", "user_rerun"):
                with self.subTest(backend=backend, condition=condition), tempfile.TemporaryDirectory() as temporary:
                    dataset = Path(temporary)
                    images = dataset / ("input" if condition == "raw_input" else "images")
                    images.mkdir()
                    for index in range(3):
                        (images / ("frame%d.png" % index)).write_bytes(b"header")
                    if condition != "missing":
                        binary_model(dataset / "sparse" / "0", views=2 if condition == "repair" else 3,
                                     points=31 if condition == "repair" else 100)
                    launched = []
                    def external_process(command, **_options):
                        launched.append("colmap" if command[-1] == "-h" else "runtime")
                        return subprocess.CompletedProcess(command, 0, "help" if command[-1] == "-h" else
                            json.dumps({"torch": "fixture", "cuda": "fixture", "device": "fixture"}), "")
                    with mock.patch.dict(os.environ, self.environment), mock.patch.object(subprocess, "run", side_effect=external_process):
                        report = training_preflight.probe_training_environment(self.repository, dataset, backend,
                            run_colmap=condition == "user_rerun")
                    self.assertTrue(report["ready"], report)
                    self.assertTrue(report["colmapRequired"])
                    self.assertEqual(launched, ["colmap", "runtime"])

    def test_unusable_cache_is_rejected_without_claiming_current_reconstruction(self):
        binary_model(self.dataset / ".alignment_cache" / "sparse" / "0", views=2, points=31)
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend), mock.patch.dict(os.environ, self.environment), mock.patch.object(subprocess, "run") as external:
                report = training_preflight.probe_training_environment(self.repository, self.dataset, backend)
            self.assertFalse(report["ready"])
            self.assertFalse(report["hasReconstruction"])
            self.assertFalse(report["colmapRequired"])
            self.assertTrue(report["sourceBlocked"])
            self.assertEqual(report["errorCode"], "reconstruction_source_unusable")
            external.assert_not_called()
            self.assertFalse((self.dataset / "sparse").exists())


if __name__ == "__main__":
    unittest.main()
