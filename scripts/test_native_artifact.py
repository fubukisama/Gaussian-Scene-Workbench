"""Public CLI byte-integrity contracts; no packaged program is ever executed."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parent


class NativeArtifactContracts(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get("RUNNER_TEMP", os.environ.get("TEMP", tempfile.gettempdir())))
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-artifact-contract-", dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.package = self.work / "package"
        fixture = subprocess.run([sys.executable, "-I", "-B", str(SCRIPTS / "prepare_native_artifact_fixture.py"),
                                  "--root", str(self.package)], text=True, capture_output=True)
        self.assertEqual(0, fixture.returncode, fixture.stdout + fixture.stderr)

    def check_cli(self, expected, baseline=None):
        output = self.work / "receipt.json"
        command = [sys.executable, "-I", "-B", str(SCRIPTS / "verify_native_artifact.py"),
                   "--root", str(self.package), "--output", str(output)]
        if baseline is not None:
            command += ["--baseline", str(baseline)]
        process = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(expected, process.returncode, process.stdout + process.stderr)
        return json.loads(output.read_text(encoding="utf-8"))

    def test_metadata_dotfiles_are_valid_corresponding_source(self):
        result = self.check_cli(0)
        self.assertEqual("PASS", result["status"])
        self.assertEqual(5, result["hiddenFileCount"])
        self.assertEqual(4, result["backendFilesVerified"])
        self.assertEqual(3, result["sourceFilesVerified"])
        self.assertFalse(result["applicationExecuted"])

    def test_omitted_hidden_backend_file_is_a_failure(self):
        (self.package / "gaussian-splatting/.gitmodules").unlink()
        result = self.check_cli(2)
        self.assertIn("manifest file missing: gaussian-splatting/.gitmodules", result["error"])

    def test_hidden_source_file_bytes_are_not_ignored(self):
        path = self.package / "source/.github/workflows/fixture.yml"
        path.write_bytes(b"corrupted corresponding source\n")
        result = self.check_cli(2)
        self.assertIn("manifest file bytes mismatch: source/.github/workflows/fixture.yml", result["error"])
        self.assertEqual(b"corrupted corresponding source\n", path.read_bytes())

    def test_actual_git_metadata_is_rejected_even_when_manifests_pass(self):
        metadata = self.package / "source/.git"
        metadata.mkdir()
        (metadata / "config").write_text("fixture\n", encoding="utf-8")
        result = self.check_cli(2)
        self.assertIn("Git metadata must not enter an artifact", result["error"])

    def test_unmanifested_runtime_loss_is_caught_by_readback_baseline(self):
        import shutil
        baseline = self.work / "staged"
        shutil.copytree(self.package, baseline)
        (baseline / "bin/QtFixture.dll").write_bytes(b"runtime byte fixture\n")
        result = self.check_cli(2, baseline)
        self.assertIn("artifact round-trip mismatch", result["error"])
        self.assertIn("bin/QtFixture.dll", result["error"])


if __name__ == "__main__":
    unittest.main()
