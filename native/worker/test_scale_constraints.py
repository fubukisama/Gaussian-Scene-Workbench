"""Scale-version acceptance through the real owned CLI and dataset admission."""

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

from PIL import Image

from native.backend.camera_dataset import (CameraDatasetError, load_camera_dataset, read_initialization_points,
                                           refresh_dataset_identity, write_camera_dataset)


class ScaleVersionTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_CAMERA_P1_TEST_ROOT", tempfile.gettempdir())) / "gsw-camera-scale-p1-worker"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repository = Path(__file__).resolve().parents[2]
        self.source = self.root / "source"
        self.source.mkdir()
        views = []
        for index, (split, x, y) in enumerate((("train", -2., 0.), ("train", 2., 0.),
                                               ("val", 0., 2.), ("test", 0., -2.))):
            image = self.source / split / (str(index) + ".png")
            image.parent.mkdir(exist_ok=True)
            Image.new("RGB", (20, 14), (20 + index, 30, 40)).save(image)
            views.append({"view_id": "view-" + str(index), "image_path": image.relative_to(self.source).as_posix(),
                "K": [[12., 0., 10.], [0., 13., 7.], [0., 0., 1.]],
                "pixel_coordinates": "pixel_border", "split": split,
                "c2w_opencv": [[1., 0., 0., x], [0., 1., 0., y], [0., 0., 1., -4.], [0., 0., 0., 1.]]})
        raw = {"schema": "gsw.camera_dataset/1", "pose_policy": "locked",
               "source": {"kind": "manual"}, "scale": {"status": "arbitrary"}, "views": views,
               "initialization": {"kind": "random", "seed": 3, "count": 16,
                                  "bounds": [[-.5, -.5, -.5], [.5, .5, .5]], "frame": "source"}}
        write_camera_dataset(raw, self.source / "camera_dataset.json")
        self.dataset = load_camera_dataset(self.source)
        write_camera_dataset(self.dataset, self.source / "camera_dataset.json")
        self.request = {"schema": "gsw.scale_constraints/1", "input_data_identity": self.dataset["data_identity"],
                        "input_frame": "source", "mode": "scale_only", "fit_tolerance_m": 1e-8,
                        "constraints": [{"id": "fit-length", "kind": "distance", "role": "fit",
                            "reference_id": "ruler-A", "reference_unit": "m", "source_a": [0., 0., 0.],
                            "source_b": [2., 0., 0.], "reference_distance": 6.,
                            "provenance": {"source": "generated_fixture", "measurement_id": "ruler-A"}},
                            {"id": "check-length", "kind": "distance", "role": "check",
                            "reference_id": "ruler-B", "reference_unit": "m", "source_a": [0., 0., 0.],
                            "source_b": [0., 1., 0.], "reference_distance": 3., "independent_of_fit": True,
                            "tolerance_m": 1e-8,
                            "provenance": {"source": "separate_generated_check", "measurement_id": "ruler-B"}}]}
        self.request_path = self.root / "request.json"
        self.request_path.write_text(json.dumps(self.request), encoding="utf-8")

    def run_worker(self, operation, destination=None, report=None, cancel=False):
        report = report or self.root / (operation + "-report.json")
        command = [sys.executable, "-B", str(self.repository / "native/worker/scale_constraints.py"),
                   "--operation", operation, "--dataset", str(self.source), "--request", str(self.request_path),
                   "--report", str(report), "--expected-identity", self.dataset["data_identity"]]
        if destination is not None:
            command.extend(["--output-directory", str(destination)])
        process = subprocess.Popen(command, cwd=str(self.root), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        def stop_owned_process():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
        self.addCleanup(stop_owned_process)
        captured = {"stdout": "", "stderr": ""}
        def drain(name, stream):
            captured[name] = stream.read()
        readers = [threading.Thread(target=drain, args=("stdout", process.stdout)),
                   threading.Thread(target=drain, args=("stderr", process.stderr))]
        for reader in readers:
            reader.start()
        if cancel:
            process.stdin.write("cancel\n")
            process.stdin.flush()
        process.wait(timeout=30)
        process.stdin.close()
        for reader in readers:
            reader.join(timeout=5)
        output, error = captured["stdout"], captured["stderr"]
        process.stdout.close()
        process.stderr.close()
        lines = [line for line in output.splitlines() if line.strip()]
        result = json.loads(lines[-1]) if lines else {}
        return process.returncode, result, output + error

    def source_hashes(self):
        return {path.relative_to(self.source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.source.rglob("*") if path.is_file()}

    def test_create_version_preserves_source_holdouts_and_replays_internal_evidence(self):
        before = self.source_hashes()
        destination = self.root / "scaled-version"
        code, result, log = self.run_worker("create-version", destination)
        self.assertEqual(code, 0, log)
        self.assertTrue(result["success"])
        scaled = load_camera_dataset(destination)
        self.assertEqual(scaled["scale"]["verification_origin"], "gsw_replay_verified")
        self.assertEqual(scaled["scale"]["status"], "independently_checked")
        self.assertAlmostEqual(scaled["transforms"]["source_to_canonical"]["s"], 3.)
        self.assertEqual(scaled["splits"], self.dataset["splits"])
        self.assertNotEqual(scaled["data_identity"], self.dataset["data_identity"])
        for original, changed in zip(self.dataset["views"], scaled["views"]):
            self.assertEqual(changed["source_c2w_opencv"], original["source_c2w_opencv"])
            self.assertEqual(changed["K"], original["K"])
            self.assertEqual((destination / original["image_path"]).read_bytes(),
                             (self.source / original["image_path"]).read_bytes())
            self.assertFalse(os.path.samefile(destination / original["image_path"],
                                             self.source / original["image_path"]))
        self.assertEqual(scaled["views"][0]["c2w_opencv"][0][3], -6.)
        self.assertEqual(scaled["views"][0]["c2w_opencv"][2][3], -12.)
        self.assertEqual(scaled["scale"]["derivation"]["solution"]["model_geometry_accuracy"], "not_measured")
        self.assertEqual(result["summary"]["scaleDerivation"]["independentChecks"]["status"], "pass")
        self.assertNotIn("derivation", result["summary"]["scale"])
        self.assertEqual(before, self.source_hashes())
        moved = self.root / "relocated"
        shutil.copytree(destination, moved)
        self.assertEqual(load_camera_dataset(moved)["data_identity"], scaled["data_identity"])

    def test_derived_report_tampering_is_replayed_not_trusted_by_status_or_hash(self):
        destination = self.root / "scaled-version"
        code, _, log = self.run_worker("create-version", destination)
        self.assertEqual(code, 0, log)
        manifest = destination / "camera_dataset.json"
        changed = json.loads(manifest.read_text(encoding="utf-8"))
        changed["scale"]["derivation"]["solution"]["fit"]["max_residual_m"] = 100.
        changed.pop("data_identity")
        write_camera_dataset(refresh_dataset_identity(changed), manifest)
        with self.assertRaises(CameraDatasetError) as raised:
            load_camera_dataset(destination)
        self.assertEqual(raised.exception.code, "camera_scale_derivation_replay_failed")

    def test_failed_independent_check_can_be_reported_but_cannot_publish_a_version(self):
        request = copy.deepcopy(self.request)
        request["constraints"][1]["reference_distance"] = 4.
        self.request_path.write_text(json.dumps(request), encoding="utf-8")
        code, result, log = self.run_worker("solve")
        self.assertEqual(code, 0, log)
        solution = json.loads(Path(result["reportPath"]).read_text(encoding="utf-8"))
        self.assertEqual(solution["independent_checks"]["status"], "fail")
        self.assertEqual(solution["model_geometry_accuracy"], "not_measured")
        destination = self.root / "failed-version"
        code, result, log = self.run_worker("create-version", destination)
        self.assertEqual(code, 2, log)
        self.assertEqual(result["code"], "camera_scale_independent_checks_failed")
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob(".*.preparing-scale-*")))

    def test_second_version_uses_fixed_source_poses_and_keeps_flat_replayable_lineage(self):
        first = self.root / "version-one"
        code, _, log = self.run_worker("create-version", first)
        self.assertEqual(code, 0, log)
        self.source = first
        self.dataset = load_camera_dataset(first)
        self.request["input_data_identity"] = self.dataset["data_identity"]
        self.request["constraints"][0]["reference_distance"] = 10.
        self.request["constraints"][1]["reference_distance"] = 5.
        self.request_path.write_text(json.dumps(self.request), encoding="utf-8")
        second = self.root / "version-two"
        code, _, log = self.run_worker("create-version", second, report=self.root / "second-report.json")
        self.assertEqual(code, 0, log)
        version = load_camera_dataset(second)
        self.assertEqual(version["transforms"]["source_to_canonical"]["s"], 5.)
        self.assertEqual(version["views"][0]["c2w_opencv"][0][3], -10.)
        self.assertEqual(version["views"][0]["source_c2w_opencv"][0][3], -2.)
        derivation = version["scale"]["derivation"]
        self.assertEqual(len(derivation["lineage"]), 2)
        self.assertEqual(len(derivation["archives"]), 1)
        self.assertNotIn("original_dataset", derivation)
        self.assertEqual(len(list((second / "source/scale_constraints/inputs").glob("*.json"))), 2)

    def test_cancel_and_existing_report_leave_source_and_existing_data_unchanged(self):
        before = self.source_hashes()
        report = self.root / "existing.json"
        report.write_bytes(b"preserve an older report")
        destination = self.root / "scaled-version"
        code, result, log = self.run_worker("create-version", destination, report=report)
        self.assertEqual(code, 2, log)
        self.assertEqual(result["code"], "camera_scale_output_exists")
        self.assertEqual(report.read_bytes(), b"preserve an older report")
        self.assertFalse(destination.exists())
        code, _, log = self.run_worker("create-version", destination, cancel=True)
        self.assertEqual(code, 130, log)
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob(".*.preparing-scale-*")))
        self.assertEqual(before, self.source_hashes())

    def test_external_independent_claim_cannot_impersonate_replayed_internal_check(self):
        for status in ("independently_checked", "source_declared", "arbitrary"):
            changed = copy.deepcopy(self.dataset)
            changed.pop("data_identity")
            changed["scale"] = {"status": status, "source_unit": "m" if status != "arbitrary" else "unknown",
                "meters_per_unit": 1. if status != "arbitrary" else None,
                "verification_origin": "gsw_replay_verified", "evidence": [{"kind": "external"}]}
            write_camera_dataset(changed, self.source / "camera_dataset.json")
            admitted = load_camera_dataset(self.source)
            with self.subTest(status=status):
                self.assertEqual(admitted["scale"]["verification_origin"], "external_claim")

    def test_canonical_initialization_bytes_follow_rotation_scale_and_second_version_once(self):
        raw = copy.deepcopy(self.dataset)
        raw.pop("data_identity")
        for view in raw["views"]:
            view["c2w_opencv"] = view.pop("source_c2w_opencv")
        raw["transforms"]["source_to_canonical"] = {
            "s": 2., "R": [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]], "t": [4., 5., 6.]}
        initial = self.source / "seed.ply"
        initial.write_text("ply\nformat ascii 1.0\nelement vertex 4\nproperty double x\nproperty double y\n"
                           "property double z\nend_header\n4 5 6\n4 6 6\n3 5 6\n4 5 7\n", encoding="ascii")
        raw["initialization"] = {"kind": "external_point_cloud", "path": "seed.ply", "frame": "canonical",
                                  "bounds": [[3., 5., 6.], [4., 6., 7.]]}
        write_camera_dataset(raw, self.source / "camera_dataset.json")
        self.dataset = load_camera_dataset(self.source)
        write_camera_dataset(self.dataset, self.source / "camera_dataset.json")
        self.request["input_data_identity"] = self.dataset["data_identity"]
        self.request_path.write_text(json.dumps(self.request), encoding="utf-8")
        first = self.root / "first-canonical-initialization"
        code, _, log = self.run_worker("create-version", first)
        self.assertEqual(code, 0, log)
        first_data = load_camera_dataset(first)
        points, _ = read_initialization_points(first_data, first)
        self.assertEqual(points, [[0., 0., 0.], [1.5, 0., 0.], [0., 1.5, 0.], [0., 0., 1.5]])
        self.assertEqual((first / "seed.ply").read_bytes(), initial.read_bytes())
        self.source, self.dataset = first, first_data
        self.request["input_data_identity"] = first_data["data_identity"]
        self.request["constraints"][0]["reference_distance"] = 10.
        self.request["constraints"][1]["reference_distance"] = 5.
        self.request_path.write_text(json.dumps(self.request), encoding="utf-8")
        second = self.root / "second-canonical-initialization"
        code, _, log = self.run_worker("create-version", second, self.root / "second-canonical-report.json")
        self.assertEqual(code, 0, log)
        second_data = load_camera_dataset(second)
        points, _ = read_initialization_points(second_data, second)
        for actual, expected in zip(points, [[0., 0., 0.], [2.5, 0., 0.], [0., 2.5, 0.], [0., 0., 2.5]]):
            for a, b in zip(actual, expected):
                self.assertAlmostEqual(a, b, places=13)
        from native.worker.canonical_training import prepare_training_dataset, load_prepared_training_dataset
        prepared = prepare_training_dataset(second, self.root / "prepared", resolution=1)
        restored = load_prepared_training_dataset(prepared["manifestPath"], second)
        self.assertEqual(restored["scale"], second_data["scale"])
        self.assertEqual(restored["initialization"]["input_to_canonical"],
                         second_data["initialization"]["input_to_canonical"])

    def test_derived_version_reimports_through_native_worker_and_preserves_verified_ancestry(self):
        destination = self.root / "scale-version"
        code, _, log = self.run_worker("create-version", destination)
        self.assertEqual(code, 0, log)
        scaled = load_camera_dataset(destination)
        project = self.root / "new-project"
        project.mkdir()
        configuration = {"task": "import", "repositoryRoot": str(self.repository), "projectRoot": str(project),
            "datasetRoot": str(project / "datasets"), "scene": "imported-version", "cameraInput": {
                "adapter": "generic_json", "sourcePath": str(destination / "camera_dataset.json"), "posePolicy": "locked"}}
        path = self.root / "import.json"
        path.write_text(json.dumps(configuration), encoding="utf-8")
        completed = subprocess.run([sys.executable, "-B", str(self.repository / "native/worker/gsw_worker.py"),
            "--task", "import", "--config", str(path)], cwd=str(self.root), input="", capture_output=True,
            text=True, encoding="utf-8", timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        imported = load_camera_dataset(project / "datasets/imported-version")
        self.assertEqual(imported["data_identity"], scaled["data_identity"])
        self.assertEqual(imported["scale"], scaled["scale"])

    def test_changed_uncertainty_changes_identity_and_cannot_reuse_a_prepared_training_task(self):
        from native.worker.canonical_training import prepare_training_dataset
        first = self.root / "first-version"
        code, _, log = self.run_worker("create-version", first)
        self.assertEqual(code, 0, log)
        prepared = prepare_training_dataset(first, self.root / "prepared", resolution=1)
        before = Path(prepared["manifestPath"]).read_bytes()
        request = copy.deepcopy(self.request)
        request["constraints"][0]["reference_sigma"] = .01
        self.request_path.write_text(json.dumps(request), encoding="utf-8")
        second = self.root / "different-uncertainty"
        code, _, log = self.run_worker("create-version", second, self.root / "different-uncertainty.json")
        self.assertEqual(code, 0, log)
        first_data, second_data = load_camera_dataset(first), load_camera_dataset(second)
        self.assertEqual(first_data["transforms"]["source_to_canonical"]["s"],
                         second_data["transforms"]["source_to_canonical"]["s"])
        self.assertNotEqual(first_data["data_identity"], second_data["data_identity"])
        with self.assertRaisesRegex(ValueError, "create a new dataset/task version"):
            prepare_training_dataset(second, self.root / "prepared", resolution=1)
        self.assertEqual(Path(prepared["manifestPath"]).read_bytes(), before)

    def test_archived_source_evidence_tampering_escape_and_size_are_refused_before_training(self):
        destination = self.root / "scale-version"
        code, _, log = self.run_worker("create-version", destination)
        self.assertEqual(code, 0, log)
        manifest = destination / "camera_dataset.json"
        admitted = load_camera_dataset(destination)
        archive = destination / admitted["scale"]["derivation"]["input_manifest"]["path"]
        original = archive.read_bytes()
        archive.write_bytes(original + b" ")
        with self.assertRaises(CameraDatasetError) as raised:
            load_camera_dataset(destination)
        self.assertEqual(raised.exception.code, "camera_scale_archive_hash_mismatch")
        with archive.open("wb") as stream:
            for _ in range(17):
                stream.write(b"x" * (1024 * 1024))
        with self.assertRaises(CameraDatasetError) as raised:
            load_camera_dataset(destination)
        self.assertEqual(raised.exception.code, "camera_scale_derivation_too_large")
        archive.write_bytes(original)
        changed = copy.deepcopy(admitted)
        changed.pop("data_identity")
        changed["scale"]["derivation"]["input_manifest"]["path"] = "../outside.json"
        write_camera_dataset(changed, manifest)
        with self.assertRaises(CameraDatasetError) as raised:
            load_camera_dataset(destination)
        self.assertEqual(raised.exception.code, "camera_path_outside_dataset")

    def test_eighth_derivation_can_reopen_but_ninth_refuses_without_erasing_versions(self):
        for number in range(1, 9):
            self.request["input_data_identity"] = self.dataset["data_identity"]
            self.request_path.write_text(json.dumps(self.request), encoding="utf-8")
            destination = self.root / ("scale-version-" + str(number))
            code, _, log = self.run_worker("create-version", destination,
                                          self.root / ("scale-report-" + str(number) + ".json"))
            self.assertEqual(code, 0, log)
            self.source, self.dataset = destination, load_camera_dataset(destination)
        self.assertEqual(len(self.dataset["scale"]["derivation"]["lineage"]), 8)
        before = self.source_hashes()
        self.request["input_data_identity"] = self.dataset["data_identity"]
        self.request_path.write_text(json.dumps(self.request), encoding="utf-8")
        destination = self.root / "scale-version-9"
        code, result, log = self.run_worker("create-version", destination, self.root / "scale-report-9.json")
        self.assertEqual(code, 2, log)
        self.assertEqual(result["code"], "camera_scale_derivation_depth_exceeded")
        self.assertFalse(destination.exists())
        self.assertEqual(self.source_hashes(), before)


if __name__ == "__main__":
    unittest.main()
