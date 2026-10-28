"""Public archive CLI contracts; execute on remote CI only."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location("gsw_archive_verification", Path(__file__).with_name("package_native_update.py"))
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


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

    def test_existing_output_is_not_overwritten_even_for_an_invalid_package(self):
        with tempfile.TemporaryDirectory(prefix="gsw-update-archive-") as temporary:
            root = Path(temporary) / "payload"
            root.mkdir()
            archive = Path(temporary) / "candidate.zip"
            archive.write_bytes(b"previous immutable artifact - must survive")
            result = subprocess.run([sys.executable, "-I", "-B", str(SCRIPT),
                "--package-root", str(root), "--output-archive", str(archive),
                "--product", PRODUCT, "--source-commit", "a" * 40,
                "--source-tree", "b" * 40, "--build-id", "fixture-build-2"],
                capture_output=True, text=True, encoding="utf-8", timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stdout)["code"], "update_archive_destination_not_fresh")
            self.assertEqual(archive.read_bytes(), b"previous immutable artifact - must survive")
            self.assertEqual(list(root.iterdir()), [])


class ReleaseArchiveReadbackTests(unittest.TestCase):
    """Real archive bytes and independently supplied Git identities, not a gate mock."""
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-release-readback-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sequence = 0

    def candidate(self, *, changed_source=False, generated=False, crate_proof=False):
        self.sequence += 1
        source = {"native/CMakeLists.txt": b"# independently tracked source\n",
                  ".github/workflows/check.yml": b"name: isolated source fixture\n"}
        generated_data = {}
        if crate_proof:
            crate_files = {"Cargo.toml": b'[package]\nname="owned-fixture"\nversion="1.0.0"\nlicense="MIT"\n',
                "src/lib.rs": b"// owned test source; never compiled or executed\n",
                "LICENSE": b"owned test license bytes\n",
                ".cargo_vcs_info.json": b'{"git":{"sha1":"0000000000000000000000000000000000000000"}}'}
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode="w:gz", format=tarfile.USTAR_FORMAT) as crate:
                for name, data in sorted(crate_files.items()):
                    item = tarfile.TarInfo("owned-fixture-1.0.0/" + name)
                    item.size, item.mtime = len(data), 0
                    crate.addfile(item, io.BytesIO(data))
            raw_crate = buffer.getvalue()
            checksum = hashlib.sha256(raw_crate).hexdigest()
            source["native/updater-trust/Cargo.lock"] = (
                'version = 4\n\n[[package]]\nname = "owned-fixture"\nversion = "1.0.0"\n'
                'source = "registry+https://github.com/rust-lang/crates.io-index"\n'
                f'checksum = "{checksum}"\n').encode()
            source["native/updater-trust/Cargo.toml"] = b'[package]\nname="wrapper-fixture"\nversion="0.1.0"\n'
            prefix = "native/updater-trust/vendor/owned-fixture/"
            generated_data.update({prefix + name: (data, "cargo-vendor") for name, data in crate_files.items()})
            generated_data[prefix + ".cargo-checksum.json"] = (json.dumps({"package": checksum,
                "files": {name: hashlib.sha256(data).hexdigest() for name, data in crate_files.items()}}, sort_keys=True).encode(), "cargo-vendor")
            generated_data["native/updater-trust/.cargo/config.toml"] = (
                b'[source.crates-io]\nreplace-with="vendored-sources"\n[source.vendored-sources]\ndirectory="vendor"\n', "cargo-vendor")
            generated_data["native/updater-trust/vendor-archives/owned-fixture-1.0.0.crate"] = (raw_crate, "cargo-crate-archive")
        expected = [{"path": name, "size": len(data), "sha": subprocess.check_output(
            ["git", "hash-object", "--stdin"], input=data, timeout=10).decode("ascii").strip(),
            "mode": "100644", "type": "blob"} for name, data in sorted(source.items())]
        tracked = [{"path": row["path"], "size": row["size"], "blobSha": row["sha"]} for row in expected]
        if changed_source:
            # Re-hash both local manifests coherently; only the independent Git
            # expectation can expose that this is not the accepted source tree.
            source["native/CMakeLists.txt"] = b"# substituted source, self-consistent local hashes\n"
        generated_rows = []
        for name, (data, kind) in generated_data.items():
            source[name] = data
            generated_rows.append({"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "kind": kind})
        if generated:
            name, data = "native/updater-trust/vendor/crate/src/lib.rs", b"// self-reported dependency source\n"
            source[name] = data
            generated_rows.append({"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "kind": "cargo-vendor"})
        files = {"bin/Gaussian Scene Workbench.exe": b"owned nonexecuted readback fixture",
            "COPYING": b"owned GPL notice", "LICENSE": b"owned GPL notice",
            "THIRD_PARTY_LICENSES.md": b"owned notices", "native/LICENSE.md": b"GPL-3.0-or-later",
            "build_manifest.json": b'{"product":"Gaussian Scene Workbench","legacyWebRuntimeIncluded":false}',
            "backend_manifest.json": b'{"schema":"owned.readback.fixture/1"}',
            **{"source/" + name: data for name, data in source.items()}}
        files["source-manifest.json"] = json.dumps({"schema": "gsw-native-corresponding-source-v1",
            "executableSha256": hashlib.sha256(files["bin/Gaussian Scene Workbench.exe"]).hexdigest(),
            "license": "GPL-3.0-or-later", "sourceFileCount": len(source), "files": [
                {"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                for name, data in sorted(source.items())]}, sort_keys=True).encode()
        files["source-provenance.json"] = json.dumps({"schema": "gsw.native_update_source_provenance/1",
            "sourceCommit": "a" * 40, "sourceTree": "b" * 40, "trackedFiles": tracked,
            "generatedDependencies": generated_rows}, sort_keys=True).encode()
        package = self.root / f"payload-{self.sequence}"
        for name, data in files.items():
            path = package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        archive = self.root / f"candidate-{self.sequence}.zip"
        report = ARCHIVE.create_archive(package, archive, product=PRODUCT,
            source_commit="a" * 40, source_tree="b" * 40, build_id="readback-fixture")
        return archive, report, expected

    def test_complete_readback_binds_hidden_source_to_real_git_blob_ids(self):
        archive, report, expected = self.candidate()
        before = archive.read_bytes()
        result = ARCHIVE.verify_release_archive(archive, report, expected)
        self.assertEqual(result["sourceCommit"], "a" * 40)
        self.assertEqual(result["sourceTree"], "b" * 40)
        self.assertEqual(result["trackedFileCount"], len(expected))
        self.assertEqual(result["generatedFileCount"], 0)
        self.assertEqual(archive.read_bytes(), before)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), ["candidate-1.zip", "payload-1"])

    def test_self_consistent_local_source_hashes_do_not_override_git_tree(self):
        archive, report, expected = self.candidate(changed_source=True)
        with self.assertRaises(ARCHIVE.ArchiveError) as caught:
            ARCHIVE.verify_release_archive(archive, report, expected)
        self.assertEqual(caught.exception.code, "update_source_git_blob_differs")

    def test_source_tree_must_include_every_archived_tracked_blob(self):
        archive, report, expected = self.candidate()
        with self.assertRaises(ARCHIVE.ArchiveError):
            ARCHIVE.verify_release_archive(archive, report, expected[:-1])

    def test_git_links_are_not_materialized_as_ordinary_source(self):
        archive, report, expected = self.candidate()
        expected[0] = dict(expected[0], mode="120000")
        with self.assertRaises(ARCHIVE.ArchiveError) as caught:
            ARCHIVE.verify_release_archive(archive, report, expected)
        self.assertEqual(caught.exception.code, "update_source_tree_unsupported_entry")

    def test_self_reported_vendor_hashes_are_not_independent_dependency_evidence(self):
        archive, report, expected = self.candidate(generated=True)
        with self.assertRaises(ARCHIVE.ArchiveError) as caught:
            ARCHIVE.verify_release_archive(archive, report, expected)
        self.assertEqual(caught.exception.code, "update_source_dependency_evidence_unavailable")

    def test_raw_crate_binds_all_generated_vendor_files_to_exact_git_lock(self):
        archive, report, expected = self.candidate(crate_proof=True)
        before = archive.read_bytes()
        result = ARCHIVE.verify_release_archive(archive, report, expected)
        self.assertEqual(result["registryPackageCount"], 1)
        self.assertEqual(result["dependencyArchiveCount"], 1)
        self.assertEqual(result["generatedFileCount"], 7)
        self.assertEqual(archive.read_bytes(), before)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), ["candidate-1.zip", "payload-1"])

    def test_report_identity_and_both_digest_layers_are_bound(self):
        archive, report, expected = self.candidate()
        for field, changed in (("buildId", "different-build"), ("archiveSha256", "0" * 64),
                               ("payloadManifestSha256", "0" * 64), ("correspondingSourceSha256", "0" * 64)):
            with self.subTest(field=field), self.assertRaises(ARCHIVE.ArchiveError):
                ARCHIVE.verify_release_archive(archive, dict(report, **{field: changed}), expected)

    def test_zip_link_type_is_rejected_even_if_manifest_and_all_bytes_match(self):
        archive, report, expected = self.candidate()
        altered = self.root / "link-type.zip"
        with zipfile.ZipFile(archive) as source, zipfile.ZipFile(altered, "w") as target:
            for item in source.infolist():
                data = source.read(item)
                if item.filename == "bin/Gaussian Scene Workbench.exe":
                    item.external_attr = (0o120777 << 16)
                target.writestr(item, data)
        report = dict(report, archiveLength=altered.stat().st_size,
                      archiveSha256=hashlib.sha256(altered.read_bytes()).hexdigest())
        with self.assertRaises(ARCHIVE.ArchiveError) as caught:
            ARCHIVE.verify_release_archive(altered, report, expected)
        self.assertEqual(caught.exception.code, "update_archive_type_or_file_set_invalid")


if __name__ == "__main__":
    unittest.main()
