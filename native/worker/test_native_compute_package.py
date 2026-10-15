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
