"""Public archive CLI contracts; execute on remote CI only."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile


SCRIPT = Path(__file__).with_name("package_native_update.py")
PRODUCT = "Fubukisama.GaussianSceneWorkbench.Native"


class NativeUpdateArchiveTests(unittest.TestCase):
    def test_complete_archive_keeps_hidden_source_and_licenses(self):
        with tempfile.TemporaryDirectory(prefix="gsw-update-archive-") as temporary:
            root = Path(temporary) / "payload"
            files = {
                "bin/Gaussian Scene Workbench.exe": b"owned nonexecuted archive fixture",
                "COPYING": b"owned license fixture\n",
                "LICENSE": b"owned license notice\n",
                "THIRD_PARTY_LICENSES.md": b"owned dependency notices\n",
                "native/LICENSE.md": b"GPL-3.0-or-later\n",
                "source/native/CMakeLists.txt": b"# owned corresponding source\n",
                "source/.github/workflows/fixture.yml": b"# hidden source path must survive\n",
                "source/.editorconfig": b"root = true\n",
                ".package-note": b"legitimate hidden payload\n",
                "build_manifest.json": json.dumps({"product": "Gaussian Scene Workbench",
                    "version": "0.3.8-native-preview", "legacyWebRuntimeIncluded": False}).encode(),
                "backend_manifest.json": b'{"schema":"owned.fixture/1"}',
            }
            source_files = [{"path": name[7:], "size": len(data),
                             "sha256": hashlib.sha256(data).hexdigest()}
                            for name, data in files.items() if name.startswith("source/")]
            files["source-manifest.json"] = json.dumps({
                "schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
                "executableSha256": hashlib.sha256(files["bin/Gaussian Scene Workbench.exe"]).hexdigest(),
                "sourceFileCount": len(source_files), "files": source_files}).encode()
            for name, data in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            archive = Path(temporary) / "candidate.zip"
            result = subprocess.run([sys.executable, "-I", "-B", str(SCRIPT),
                "--package-root", str(root), "--output-archive", str(archive),
                "--product", PRODUCT, "--source-commit", "a" * 40,
                "--source-tree", "b" * 40, "--build-id", "fixture-build-1"],
                capture_output=True, text=True, encoding="utf-8", timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["fileCount"], len(files))
            with zipfile.ZipFile(archive) as payload:
                self.assertEqual(set(payload.namelist()), set(files) | {"release-files.json"})
                for name, data in files.items():
                    self.assertEqual(payload.read(name), data, name)
                manifest = json.loads(payload.read("release-files.json"))
                self.assertEqual(manifest["schema"], "gsw.native_update_files/1")
                self.assertEqual(manifest["product"], PRODUCT)
                self.assertEqual(manifest["platform"], "windows-x64")
                self.assertEqual(manifest["buildId"], "fixture-build-1")
                self.assertEqual(manifest["sourceCommit"], "a" * 40)
                self.assertEqual(manifest["sourceTree"], "b" * 40)
                self.assertEqual({entry["path"] for entry in manifest["files"]}, set(files))
            self.assertEqual(root.joinpath(".package-note").read_bytes(), files[".package-note"])


if __name__ == "__main__":
    unittest.main()
