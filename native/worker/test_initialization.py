"""Real initialization worker publication, cancellation and immutable versions."""
import copy
import hashlib
import json
import importlib.util
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest
from types import SimpleNamespace

import numpy as np

from PIL import Image

from native.backend import camera_dataset


class InitializationWorkerTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_INITIALIZATION_TEST_ROOT", tempfile.gettempdir()))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        Image.new("RGB", (16, 12), (31, 61, 91)).save(self.source / "image.png")
        data = {"schema": camera_dataset.SCHEMA, "source": {"kind": "synthetic"},
            "scale": {"status": "arbitrary"}, "views": [{"view_id": "train-a", "image_path": "image.png",
                "split": "train", "pixel_coordinates": "pixel_border",
                "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]],
                "c2w_opencv": [[1., 0., 0., .5], [0., 1., 0., .5], [0., 0., 1., 0.], [0., 0., 0., 1.]]}]}
        camera_dataset.write_camera_dataset(data, self.source / camera_dataset.MANIFEST_NAME)
        self.dataset = camera_dataset.load_camera_dataset(self.source)
        camera_dataset.write_camera_dataset(self.dataset, self.source / camera_dataset.MANIFEST_NAME)
        self.request = {"schema": "gsw.initialization_request/1", "input_data_identity": self.dataset["data_identity"],
            "backend": "3dgs", "resolution": 1, "initialization": {"kind": "random", "seed": 0,
                "count": 16, "frame": "source", "bounds": [[0., 0., 2.], [1., 1., 3.]], "priors": []}}
        self.request_path = self.root / "request.json"
        camera_dataset.write_camera_dataset(self.request, self.request_path)

    def worker(self, operation="inspect", output=None, report=None, env=None):
        report = report or self.root / "report.json"
        command = [sys.executable, "-B", str(Path(__file__).with_name("initialization.py")),
            "--operation", operation, "--dataset", str(self.source), "--request", str(self.request_path),
            "--report", str(report), "--expected-identity", self.dataset["data_identity"]]
        if output is not None:
            command += ["--output-directory", str(output)]
        options = {"stdin": subprocess.PIPE, "stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
                   "text": True, "encoding": "utf-8"}
        if env is not None:
            options["env"] = env
        if os.name == "nt":
            options["creationflags"] = subprocess.CREATE_NO_WINDOW
        return subprocess.Popen(command, **options)

    def test_real_inspection_finishes_while_parent_stdin_is_open_and_preserves_source(self):
        before = {path.name: path.read_bytes() for path in self.source.iterdir() if path.is_file()}
        process = self.worker()
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        process.wait(timeout=20)  # stdin deliberately remains open.
        stdout, stderr = process.stdout.read(), process.stderr.read()
        process.stdout.close()
        process.stderr.close()
        process.stdin.close()
        self.assertEqual(process.returncode, 0, stderr)
        result = json.loads(stdout.strip().splitlines()[-1])
        self.assertTrue(result["success"])
        report = json.loads((self.root / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "ready")
        self.assertEqual(report["point_count"], 16)
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.source.iterdir() if path.is_file()})

    def test_new_version_persists_actual_random_points_without_claiming_an_external_prior(self):
        from native.backend.initialization import resolve_initialization_points
        from native.worker.initialization import create_initialized_dataset_version
        before = {path.name: path.read_bytes() for path in self.source.iterdir() if path.is_file()}
        output = self.root / "initialized"
        result = create_initialized_dataset_version(self.source, self.request, output, self.dataset["data_identity"])
        reloaded = camera_dataset.load_camera_dataset(output)
        points, colors, record = resolve_initialization_points(reloaded, output)
        self.assertEqual(len(points), 16)
        self.assertEqual(record["point_sha256"], result["report"]["point_sha256"])
        self.assertNotEqual(reloaded["data_identity"], self.dataset["data_identity"])
        self.assertEqual(reloaded["initialization"]["kind"], "random")
        self.assertNotIn("external_geometry", reloaded["initialization"]["priors"])
        material = reloaded["initialization"]["materialization"]
        self.assertTrue((output / material["path"]).is_file())
        self.assertEqual(len(material["sha256"]), 64)
        self.assertEqual(reloaded["initialization_derivation"]["input_manifest"]["data_identity"], self.dataset["data_identity"])
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.source.iterdir() if path.is_file()})

    def test_real_create_version_returns_an_admittable_identity_and_an_immutable_actual_report(self):
        output = self.root / "published"
        process = self.worker("create-version", output)
        stdout, stderr = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 0, stderr)
        result = json.loads(stdout.strip().splitlines()[-1])
        loaded = camera_dataset.load_camera_dataset(result["datasetPath"])
        report = json.loads((self.root / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(report["created_data_identity"], loaded["data_identity"])
        self.assertEqual(result["summary"]["dataIdentity"], loaded["data_identity"])
        self.assertEqual(result["summary"]["manifestSha256"], camera_dataset._hash(output / camera_dataset.MANIFEST_NAME))
        self.assertEqual(result["summary"]["infoSha256"], camera_dataset._hash(output / "camera_dataset_info.json"))
        self.assertNotIn("manifestSha256", json.loads((output / "camera_dataset_info.json").read_text(encoding="utf-8")))
        self.assertEqual(report["materialization"]["sha256"], loaded["initialization"]["materialization"]["sha256"])

    def test_scaled_then_initialized_then_rescaled_data_keep_exact_actual_points_on_prepare_and_reopen(self):
        from native.backend.initialization import resolve_initialization_points
        from native.worker.initialization import create_initialized_dataset_version
        from native.worker.scale_constraints import create_scaled_dataset_version
        from native.worker.canonical_training import prepare_training_dataset, load_prepared_training_dataset, transform_points
        scale_request = {"schema": "gsw.scale_constraints/1", "input_data_identity": self.dataset["data_identity"],
            "input_frame": "source", "mode": "scale_only", "fit_tolerance_m": 1e-8, "constraints": [{
                "id": "distance", "reference_id": "independent-ruler", "role": "fit", "kind": "distance",
                "source_a": [0., 0., 0.], "source_b": [1., 0., 0.], "reference_distance": 2., "reference_unit": "m"}]}
        scaled = self.root / "scaled"
        create_scaled_dataset_version(self.source, scale_request, scaled)
        scaled_data = camera_dataset.load_camera_dataset(scaled)
        request = copy.deepcopy(self.request)
        request["input_data_identity"] = scaled_data["data_identity"]
        initialized = self.root / "initialized"
        create_initialized_dataset_version(scaled, request, initialized)
        initialized_data = camera_dataset.load_camera_dataset(initialized)
        artifact = initialized / initialized_data["initialization"]["materialization"]["path"]
        before = artifact.read_bytes()
        points = [[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        references = [[10., 20., 0.], [10., 23., 0.], [7., 20., 0.], [10., 20., 3.]]
        request2 = {"schema": "gsw.scale_constraints/1", "input_data_identity": initialized_data["data_identity"],
            "input_frame": "source", "mode": "sim3", "fit_tolerance_m": 1e-8, "constraints": [
                {"id": "fit-" + str(i), "reference_id": "control-" + str(i), "role": "fit", "kind": "control",
                 "reference_unit": "m", "source_xyz": point, "reference_xyz": references[i]}
                 for i, point in enumerate(points)]}
        rescaled = self.root / "rescaled"
        create_scaled_dataset_version(initialized, request2, rescaled)
        final_data = camera_dataset.load_camera_dataset(rescaled)
        expected_points, _, _ = resolve_initialization_points(final_data, rescaled)
        np.testing.assert_allclose(expected_points[0],
            [7.854431900882741, 21.646440511781974, 7.808290128214932], atol=1e-12, rtol=0.)
        prepared = prepare_training_dataset(rescaled, self.root / "prepared")
        restored = load_prepared_training_dataset(prepared["manifestPath"], rescaled)
        canonical_points = transform_points(restored["transforms"]["canonical_to_training"]["inverse"], restored["initial_points"])
        np.testing.assert_allclose(canonical_points, expected_points, atol=1e-12, rtol=0.)
        self.assertEqual(artifact.read_bytes(), before)
        self.assertEqual(restored["scale"]["verification_origin"], "gsw_replay_verified")

    def test_published_report_bytes_are_bound_in_the_replayable_version(self):
        from native.backend.initialization import InitializationError
        from native.worker.initialization import create_initialized_dataset_version
        output = self.root / "initialized"
        result = create_initialized_dataset_version(self.source, self.request, output)
        report_path = output / "source/initialization/reports" / (result["report"]["request_sha256"] + ".json")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["point_count"] = 0
        camera_dataset.write_camera_dataset(report, report_path)
        with self.assertRaises(InitializationError) as raised:
            camera_dataset.load_camera_dataset(output)
        self.assertEqual(raised.exception.code, "initialization_report_hash_mismatch")

    def test_real_suggestion_returns_an_explicit_candidate_without_creating_a_dataset(self):
        request = copy.deepcopy(self.request)
        request["suggestion"] = {"method": "user_depth_interval", "view_id": "train-a", "near": 2., "far": 4.}
        camera_dataset.write_camera_dataset(request, self.request_path)
        process = self.worker("suggest")
        stdout, stderr = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, stderr)
        result = json.loads(stdout.strip().splitlines()[-1])
        self.assertNotIn("datasetPath", result)
        report = json.loads((self.root / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(report["request"], request)
        self.assertEqual(report["status"], "suggestion_requires_confirmation")
        self.assertEqual(camera_dataset.load_camera_dataset(self.source)["data_identity"], self.dataset["data_identity"])

    def test_editing_a_saved_random_recipe_invalidates_its_old_point_artifact_explicitly(self):
        from native.backend.initialization import inspect_initialization
        from native.worker.initialization import create_initialized_dataset_version
        output = self.root / "initialized"
        create_initialized_dataset_version(self.source, self.request, output)
        loaded = camera_dataset.load_camera_dataset(output)
        request = copy.deepcopy(self.request)
        request["input_data_identity"] = loaded["data_identity"]
        request["initialization"] = copy.deepcopy(loaded["initialization"])
        request["initialization"]["seed"] = 1
        report = inspect_initialization(loaded, output, request)
        np.testing.assert_allclose(report["preview"]["points"][0],
            [.417022004702574, .7203244934421581, 2.000114374817345], atol=1e-12, rtol=0.)
        self.assertNotIn("materialization", report["initialization"])
        self.assertIn("initialization_materialization_invalidated_by_recipe_change",
                      [warning["code"] for warning in report["warnings"]])

    def test_a_second_immutable_version_reuses_identical_actual_point_bytes_without_overwriting(self):
        from native.worker.initialization import create_initialized_dataset_version
        first = self.root / "first"
        first_result = create_initialized_dataset_version(self.source, self.request, first)
        first_data = camera_dataset.load_camera_dataset(first)
        first_material = first_data["initialization"]["materialization"]
        original_bytes = (first / first_material["path"]).read_bytes()
        request = copy.deepcopy(self.request)
        request["input_data_identity"] = first_data["data_identity"]
        second = self.root / "second"
        second_result = create_initialized_dataset_version(first, request, second)
        second_data = camera_dataset.load_camera_dataset(second)
        self.assertEqual(second_result["report"]["point_sha256"], first_result["report"]["point_sha256"])
        self.assertEqual((second / second_data["initialization"]["materialization"]["path"]).read_bytes(), original_bytes)
        self.assertEqual((first / first_material["path"]).read_bytes(), original_bytes)

    def test_recomputed_dataset_sha_cannot_forge_ancestor_records(self):
        from native.backend.initialization import InitializationError
        from native.worker.initialization import create_initialized_dataset_version
        output = self.root / "initialized"
        create_initialized_dataset_version(self.source, self.request, output)
        data = camera_dataset.load_camera_dataset(output)
        data["initialization_derivation"]["archives"] = [{
            "path": "source/initialization/inputs/not-a-real-ancestor.json", "sha256": "0" * 64,
            "data_identity": "1" * 64}]
        data = camera_dataset.refresh_dataset_identity(data)
        camera_dataset.write_camera_dataset(data, output / camera_dataset.MANIFEST_NAME)
        with self.assertRaises(InitializationError) as raised:
            camera_dataset.load_camera_dataset(output)
        self.assertEqual(raised.exception.code, "initialization_derivation_invalid")

    def test_real_reimport_preserves_the_point_artifact_report_and_every_replayable_ancestor(self):
        from native.worker.initialization import create_initialized_dataset_version
        initialized = self.root / "initialized"
        create_initialized_dataset_version(self.source, self.request, initialized)
        original = camera_dataset.load_camera_dataset(initialized)
        project = self.root / "project"
        project.mkdir()
        config = {"task": "import", "repositoryRoot": str(Path(__file__).resolve().parents[2]),
            "projectRoot": str(project), "datasetRoot": str(project / "datasets"), "scene": "copy",
            "cameraInput": {"adapter": "generic_json", "sourcePath": str(initialized), "posePolicy": "locked"}}
        config_path = self.root / "import-config.json"
        camera_dataset.write_camera_dataset(config, config_path)
        process = subprocess.run([sys.executable, "-B", str(Path(__file__).with_name("gsw_worker.py")),
            "--task", "import", "--config", str(config_path)], capture_output=True, text=True,
            encoding="utf-8", timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        copied = camera_dataset.load_camera_dataset(project / "datasets/copy")
        self.assertEqual(copied["data_identity"], original["data_identity"])

    def test_full_resume_identity_changes_when_only_the_initializer_implementation_changes(self):
        repository = Path(__file__).resolve().parents[2]
        copied = self.root / "copied-source"
        for relative in ("native/worker/training_checkpoint.py", "native/worker/training_density_control.py",
                "native/worker/canonical_training.py", "native/backend/camera_dataset.py", "native/backend/initialization.py"):
            path = copied / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(repository / relative, path)
        spec = importlib.util.spec_from_file_location("owned_checkpoint_copy", copied / "native/worker/training_checkpoint.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        dataset = SimpleNamespace(source_path=str(self.source), model_path=str(self.root / "output"),
                                  _gsw_camera_contract={"schema": "gsw.training_camera_contract/1"})
        first = module.training_identity(dataset, SimpleNamespace(), SimpleNamespace())
        initializer = copied / "native/backend/initialization.py"
        initializer.write_bytes(initializer.read_bytes() + b"\n# owned source-change fixture\n")
        second = module.training_identity(dataset, SimpleNamespace(), SimpleNamespace())
        self.assertNotEqual(first, second)

    def test_actual_training_preparation_uses_the_same_float32_near_boundary_as_inspection(self):
        from native.worker.canonical_training import prepare_training_dataset
        data = copy.deepcopy(self.dataset)
        data["normalization"] = {"mode": "none"}
        data["initialization"] = copy.deepcopy(self.request["initialization"])
        data["initialization"]["bounds"] = [[.49, .49, .200000001], [.51, .51, .200000002]]
        data.pop("data_identity")
        camera_dataset.write_camera_dataset(data, self.source / camera_dataset.MANIFEST_NAME)
        with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "camera_no_forward_initialization"):
            prepare_training_dataset(self.source)

    def test_real_pipe_cancel_returns_130_without_publishing_or_changing_source(self):
        before = {p.name: p.read_bytes() for p in self.source.iterdir() if p.is_file()}
        output = self.root / "cancelled-version"
        process = self.worker("create-version", output)
        stdout, stderr = process.communicate("cancel\n", timeout=20)
        self.assertEqual(process.returncode, 130, stdout + stderr)
        self.assertEqual(json.loads(stderr.strip().splitlines()[-1])["code"], "camera_import_cancelled")
        self.assertFalse(output.exists())
        self.assertFalse((self.root / "report.json").exists())
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.source.iterdir() if p.is_file()})

    def test_machine_json_is_utf8_even_when_parent_requests_cp932(self):
        env = dict(os.environ, PYTHONIOENCODING="cp932", PYTHONUTF8="0")
        report_path = self.root / "初始化验收.json"
        process = self.worker(report=report_path, env=env)
        stdout, stderr = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        returned_path = Path(json.loads(stdout.strip().splitlines()[-1])["reportPath"])
        # Windows may return the canonical long parent for an input spelling
        # containing an existing 8.3 alias. Both must identify the same actual
        # report, while the Unicode filename/content must survive unchanged.
        self.assertEqual(returned_path.name, report_path.name)
        self.assertTrue(returned_path.is_file())
        self.assertTrue(returned_path.samefile(report_path))
        report = json.loads(returned_path.read_text(encoding="utf-8"))
        self.assertEqual(report["request"], self.request)
        self.assertEqual(report["status"], "ready")

    @unittest.skipUnless(os.name == "nt", "Windows real 8.3 alias behavior")
    def test_utf8_report_through_a_real_short_parent_alias_identifies_the_same_file(self):
        import ctypes
        parent = self.root / "owned initialization report alias directory"
        parent.mkdir()
        parent = parent.resolve(strict=True)
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.windll.kernel32.GetShortPathNameW(str(parent), buffer, len(buffer))
        self.assertGreater(size, 0)
        self.assertLess(size, len(buffer))
        alias = Path(buffer.value)
        if os.path.normcase(str(alias)) == os.path.normcase(str(parent)):
            self.skipTest("test volume has no distinct real 8.3 alias; no volume settings or system files are changed")
        self.assertTrue(alias.samefile(parent))
        report_path = alias / "初始化验收.json"
        process = self.worker(report=report_path, env=dict(os.environ, PYTHONIOENCODING="cp932", PYTHONUTF8="0"))
        stdout, stderr = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        returned_path = Path(json.loads(stdout.strip().splitlines()[-1])["reportPath"])
        self.assertEqual(returned_path.name, "初始化验收.json")
        self.assertTrue(returned_path.samefile(report_path))
        self.assertTrue(returned_path.samefile(parent / report_path.name))
        report = json.loads(returned_path.read_text(encoding="utf-8"))
        self.assertEqual(report["request"], self.request)
        self.assertEqual(report["status"], "ready")

    def test_initializer_only_replay_rejects_rehashed_non_initializer_data_changes(self):
        from native.worker.initialization import create_initialized_dataset_version
        output = self.root / "initialized"
        create_initialized_dataset_version(self.source, self.request, output)
        original = camera_dataset.load_camera_dataset(output)
        for field in ("intrinsics", "source_metadata"):
            data = copy.deepcopy(original)
            if field == "intrinsics":
                data["views"][0]["source_K"][0][0] += 1.
                data["views"][0]["K"][0][0] += 1.
            else:
                data["source"]["new_claim"] = "must not inherit admitted provenance"
            camera_dataset.write_camera_dataset(camera_dataset.refresh_dataset_identity(data),
                                                output / camera_dataset.MANIFEST_NAME)
            with self.subTest(field=field):
                with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "initialization_derivation_invalid"):
                    camera_dataset.load_camera_dataset(output)

    def test_mixed_initializer_and_scale_versions_share_one_eight_level_replay_limit(self):
        from native.worker.initialization import create_initialized_dataset_version
        from native.worker.scale_constraints import create_scaled_dataset_version
        path, dataset = self.source, self.dataset
        for depth in range(1, 9):
            output = self.root / ("mixed-" + str(depth))
            if depth % 2:
                request = copy.deepcopy(self.request)
                request["input_data_identity"] = dataset["data_identity"]
                create_initialized_dataset_version(path, request, output)
            else:
                request = {"schema": "gsw.scale_constraints/1", "input_data_identity": dataset["data_identity"],
                    "input_frame": "source", "mode": "scale_only", "fit_tolerance_m": 1e-8,
                    "constraints": [{"id": "distance", "reference_id": "ruler", "role": "fit", "kind": "distance",
                        "source_a": [0., 0., 0.], "source_b": [1., 0., 0.],
                        "reference_distance": 2., "reference_unit": "m"}]}
                create_scaled_dataset_version(path, request, output)
            path, dataset = output, camera_dataset.load_camera_dataset(output)
        before = (path / camera_dataset.MANIFEST_NAME).read_bytes()
        request = copy.deepcopy(self.request)
        request["input_data_identity"] = dataset["data_identity"]
        ninth = self.root / "mixed-9"
        with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "derivation_depth_exceeded"):
            create_initialized_dataset_version(path, request, ninth)
        self.assertFalse(ninth.exists())
        self.assertEqual((path / camera_dataset.MANIFEST_NAME).read_bytes(), before)
        self.assertEqual(camera_dataset.load_camera_dataset(path)["data_identity"], dataset["data_identity"])

    def test_rehashing_arbitrary_point_bytes_cannot_impersonate_the_declared_random_recipe(self):
        from native.backend.initialization import initialization_version_id
        from native.worker.initialization import create_initialized_dataset_version
        output = self.root / "initialized"
        create_initialized_dataset_version(self.source, self.request, output)
        data = camera_dataset.load_camera_dataset(output)
        material = data["initialization"]["materialization"]
        artifact = output / material["path"]
        with np.load(artifact, allow_pickle=False) as arrays:
            points, colors = arrays["points"].copy(), arrays["colors"].copy()
        points[0, 0] += .05
        # An attacker can rehash its own bytes, but cannot change what seed-0
        # and the archived explicit box actually generate.
        metadata = {"schema": "gsw.canonical_initial_points/1", "generator": "mt19937-uniform-explicit-f64-sim3/1",
                    "kind": "random", "count": len(points), "seed": 0, "priors": []}
        digest = hashlib.sha256(json.dumps(metadata, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\0")
        digest.update(np.asarray(points, dtype="<f8").tobytes())
        digest.update(np.asarray(colors, dtype="<f8").tobytes())
        new_point_sha = digest.hexdigest()
        with artifact.open("wb") as stream:
            np.savez(stream, points=points, colors=colors)
        material["sha256"], material["point_sha256"] = camera_dataset._hash(artifact), new_point_sha
        new_version = initialization_version_id(self.dataset["data_identity"],
            data["initialization_derivation"]["request_sha256"], new_point_sha, material["sha256"])
        data["data_version"] = new_version
        report_record = data["initialization_derivation"]["report_manifest"]
        report_file = output / report_record["path"]
        report = json.loads(report_file.read_text(encoding="utf-8"))
        report.update(point_sha256=new_point_sha, materialization=copy.deepcopy(material), created_data_version=new_version)
        camera_dataset.write_camera_dataset(report, report_file)
        report_record["sha256"] = camera_dataset._hash(report_file)
        camera_dataset.write_camera_dataset(camera_dataset.refresh_dataset_identity(data), output / camera_dataset.MANIFEST_NAME)
        with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "initialization_report_replay_mismatch"):
            camera_dataset.load_camera_dataset(output)

    def test_external_prior_outside_source_directory_survives_random_replacement_rescale_and_real_reimport(self):
        from native.worker.initialization import create_initialized_dataset_version
        from native.worker.scale_constraints import create_scaled_dataset_version
        prior = self.source / "original-prior.ply"
        prior.write_text("ply\nformat ascii 1.0\nelement vertex 4\nproperty double x\nproperty double y\n"
            "property double z\nend_header\n0.4 0.4 2\n0.6 0.4 2\n0.4 0.6 2\n0.6 0.6 2\n", encoding="ascii")
        before = prior.read_bytes()
        base = copy.deepcopy(self.dataset)
        base.pop("data_identity")
        base["initialization"] = {"kind": "external_point_cloud", "frame": "source", "path": prior.name,
                                  "provenance": {"source": "explicit input prior, not evaluation geometry"}}
        camera_dataset.write_camera_dataset(base, self.source / camera_dataset.MANIFEST_NAME)
        base = camera_dataset.load_camera_dataset(self.source)
        request = copy.deepcopy(self.request)
        request["input_data_identity"] = base["data_identity"]
        initialized = self.root / "initialized"
        create_initialized_dataset_version(self.source, request, initialized)
        current = camera_dataset.load_camera_dataset(initialized)
        scale_request = {"schema": "gsw.scale_constraints/1", "input_data_identity": current["data_identity"],
            "input_frame": "source", "mode": "scale_only", "fit_tolerance_m": 1e-8,
            "constraints": [{"id": "distance", "reference_id": "ruler", "role": "fit", "kind": "distance",
                "source_a": [0., 0., 0.], "source_b": [1., 0., 0.], "reference_distance": 2., "reference_unit": "m"}]}
        rescaled = self.root / "rescaled"
        create_scaled_dataset_version(initialized, scale_request, rescaled)
        self.assertEqual((rescaled / prior.name).read_bytes(), before)
        project = self.root / "project"
        project.mkdir()
        config = {"task": "import", "repositoryRoot": str(Path(__file__).resolve().parents[2]),
            "projectRoot": str(project), "datasetRoot": str(project / "datasets"), "scene": "copied",
            "cameraInput": {"adapter": "generic_json", "sourcePath": str(rescaled), "posePolicy": "locked"}}
        config_path = self.root / "import-config.json"
        camera_dataset.write_camera_dataset(config, config_path)
        process = subprocess.run([sys.executable, "-B", str(Path(__file__).with_name("gsw_worker.py")),
            "--task", "import", "--config", str(config_path)], capture_output=True, text=True,
            encoding="utf-8", timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        copied = project / "datasets/copied"
        self.assertEqual(camera_dataset.load_camera_dataset(copied)["data_identity"],
                         camera_dataset.load_camera_dataset(rescaled)["data_identity"])
        self.assertEqual((copied / prior.name).read_bytes(), before)
        self.assertEqual(prior.read_bytes(), before)

    @unittest.skipUnless(os.name == "nt", "Windows legacy final-path publication boundary")
    def test_long_final_artifact_paths_are_refused_before_short_staging_can_publish_an_unusable_version(self):
        from native.worker.initialization import create_initialized_dataset_version
        relative = "source/initialization/inputs/" + "0" * 64 + ".json"
        name = "version-12345678-1234-1234-1234-123456789abc"
        desired_parent = 260 - len(relative) - len(name) - 2
        parent = self.root
        while len(str(parent)) < desired_parent:
            missing = desired_parent - len(str(parent))
            if missing == 1:
                parent = parent.with_name(parent.name + "x")
            else:
                parent = parent / ("p" * min(48, missing - 1))
        parent.mkdir(parents=True, exist_ok=True)
        output = parent / name
        self.assertEqual(len(str(output / relative)), 260)
        before = (self.source / camera_dataset.MANIFEST_NAME).read_bytes()
        with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "initialization_unsafe_output"):
            create_initialized_dataset_version(self.source, self.request, output)
        self.assertFalse(output.exists())
        self.assertEqual(list(parent.iterdir()), [])
        self.assertEqual((self.source / camera_dataset.MANIFEST_NAME).read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
