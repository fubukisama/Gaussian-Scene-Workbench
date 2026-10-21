"""Exercise the shipped media-import command without a web application."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


class NativeComputePackageTests(unittest.TestCase):
    def test_runtime_modules_and_read_only_connection_cli_are_shipped_and_isolated(self):
        repository = Path(os.environ.get(
            "GSW_PACKAGE_TEST_ROOT", Path(__file__).resolve().parents[2])).resolve()
        with tempfile.TemporaryDirectory(prefix="native-runtime-package-") as temporary:
            root = Path(temporary)
            store = root / "owned-job-store"
            store.mkdir()
            job_id = "e" * 32
            record = store / (job_id + ".json")
            record.write_text(json.dumps({"id": job_id, "backend": "3dgs", "status": "running",
                                          "stage": "train", "iteration": 6, "total_iterations": 99,
                                          "output_scene": "owned-observation-only"}), encoding="utf-8")
            before = record.read_bytes()
            environment = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8", PYTHONPATH="",
                               GS_EDITOR_WORKSPACE_ROOT=str(root / "workspace"))
            # An isolated interpreter and foreign working directory prevent a
            # staged package from accidentally importing this source checkout.
            probe = r'''
import json, sys
sys.path.insert(0, sys.argv[1])
from native.worker import (training_configuration, training_failure,
                           training_job_connection, training_batch_connection)
report = training_configuration.normalize_runtime_options(
    {"version": 2, "resourceGuard": {"enabled": False},
     "execution": {"image_staging": False, "camera_prefetch": True}})
failure = training_failure.training_failure_record(RuntimeError("owned normal failure"), "3dgs")
print(json.dumps({"runtime": report, "failure": failure, "modules": {
    module.__name__: module.__file__ for module in
    (training_configuration, training_failure, training_job_connection, training_batch_connection)},
    "forbidden": [name for name in sys.modules if name == "torch" or name.startswith("torch.")
                  or name == "native.backend.compute" or name in ("http.server", "webbrowser")]},
    allow_nan=False))
'''
            completed = subprocess.run([sys.executable, "-I", "-B", "-c", probe, str(repository)],
                                       cwd=str(root), env=environment, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       encoding="utf-8", errors="replace", timeout=30, check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout)
            result = json.loads(completed.stdout)
            self.assertFalse(result["runtime"]["requested"]["resourceGuard"]["enabled"])
            self.assertTrue(result["runtime"]["requested"]["execution"]["camera_prefetch"])
            self.assertFalse(result["runtime"]["effective"]["execution"]["camera_prefetch"])
            self.assertEqual(result["runtime"]["reasons"], [
                {"feature": "execution.camera_prefetch", "code": "requires_image_staging"}])
            self.assertEqual(result["failure"]["kind"], "training_exception")
            self.assertEqual(result["forbidden"], [])
            for name, path in result["modules"].items():
                self.assertEqual(Path(path).resolve(), repository / (name.replace(".", "/") + ".py"))
            # Match the real native observer command: a read-only module CLI,
            # never compute's recovery loader or a training launcher.
            inspected = subprocess.run([sys.executable, "-B", "-m", "native.worker.training_job_connection",
                                        "--job-store", str(store)], cwd=str(repository), env=environment,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       encoding="utf-8", errors="replace", timeout=30, check=False)
            self.assertEqual(inspected.returncode, 0, inspected.stdout)
            jobs = json.loads(inspected.stdout)["jobs"]
            self.assertEqual(len(jobs), 1)
            self.assertEqual(jobs[0]["jobId"], job_id)
            self.assertEqual(jobs[0]["liveness"], "unverified")
            self.assertFalse(jobs[0]["controlAvailable"])
            self.assertEqual(record.read_bytes(), before)
            self.assertEqual(list(store.iterdir()), [record])
            manifest_path = repository / "backend_manifest.json"
            if manifest_path.is_file():
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                for module in ("training_configuration", "training_failure", "training_job_connection",
                               "training_batch_connection"):
                    self.assertIn("native/worker/" + module + ".py", manifest["components"])

    def test_import_command_publishes_photos_and_preserves_existing_dataset(self):
        repository = Path(os.environ.get(
            "GSW_PACKAGE_TEST_ROOT", Path(__file__).resolve().parents[2])).resolve()
        with tempfile.TemporaryDirectory(prefix="native-compute-package-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            originals = []
            for index, color in enumerate(((20, 90, 170), (140, 60, 30))):
                photo = source / ("frame-{}.png".format(index))
                Image.new("RGB", (8, 6), color).save(str(photo))
                originals.append(photo)
            hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in originals}
            project = root / "project"
            config = root / "import.json"
            config.write_text(json.dumps({
                "task": "import", "repositoryRoot": str(repository),
                "projectRoot": str(project), "datasetRoot": str(project / "datasets"),
                "scene": "capture", "files": [{"path": str(path)} for path in originals],
            }), encoding="utf-8")
            environment = dict(os.environ, PYTHONUTF8="1", PYTHONPATH="",
                               GS_EDITOR_WORKSPACE_ROOT=str(root / "workspace"))
            command = [sys.executable, "-B", str(repository / "native/worker/gsw_worker.py"),
                       "--config", str(config), "--task", "import"]
            def invoke():
                return subprocess.run(command, cwd=str(root), env=environment,
                                      stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, encoding="utf-8",
                                      errors="replace", timeout=45, check=False)
            first = invoke()
            self.assertEqual(first.returncode, 0, first.stdout)
            destination = project / "datasets/capture/images"
            self.assertEqual(sorted(path.name for path in destination.iterdir()), sorted(hashes))
            for name, digest in hashes.items():
                self.assertEqual(hashlib.sha256((destination / name).read_bytes()).hexdigest(), digest)
                self.assertEqual(hashlib.sha256((source / name).read_bytes()).hexdigest(), digest)
            # Repeating an import without overwrite must fail through the public
            # command while leaving the published dataset and originals intact.
            repeated = invoke()
            self.assertNotEqual(repeated.returncode, 0, repeated.stdout)
            self.assertIn("Dataset already exists", repeated.stdout)
            for name, digest in hashes.items():
                self.assertEqual(hashlib.sha256((destination / name).read_bytes()).hexdigest(), digest)
                self.assertEqual(hashlib.sha256((source / name).read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
