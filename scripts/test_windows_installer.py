"""Public payload/translation contract tests for the native Windows setup."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_windows_installer as installer
from verify_native_backend import BACKEND_FILES


ROOT = Path(__file__).resolve().parents[1]


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_fixture_source_manifest(package):
    """Record the exact synthetic source/executable identity, without executing it."""
    package = Path(package)
    source = package / "source"
    manifest = {
        "schema": "gsw-native-corresponding-source-v1",
        "executableSha256": file_hash(package / "bin/Gaussian Scene Workbench.exe"),
        "license": "GPL-3.0-or-later",
        "files": [
            {"path": path.relative_to(source).as_posix(), "size": path.stat().st_size,
             "sha256": file_hash(path)}
            for path in sorted(source.rglob("*")) if path.is_file()
        ],
    }
    manifest["sourceFileCount"] = len(manifest["files"])
    write_json(package / "source-manifest.json", manifest)
    return manifest


def create_fixture_payload(package, *, version="0.3.1-native-preview", installer_fixture=False):
    """Create controlled non-executable test bytes for the installer boundary.

    This helper does not create a desktop application or a training runtime.
    Remote fixture callers must mark the payload and use a test-only Setup.
    """
    package = Path(package)
    package.mkdir(parents=True, exist_ok=False)
    for relative in set(installer.REQUIRED) | set(BACKEND_FILES):
        path = package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"# GPL-3.0-or-later fixture, never execute\n" if path.suffix == ".py"
                         else b"GSW synthetic GPL installer fixture; not executable.\n")
    for relative in (
        "source/native/src/main.cpp", "source/native/src/shaders/SurfelProjection.glsl",
        "source/native/resources.qrc", "source/native/lucide_icons.qrc",
        "source/native/build_manifest.json", "source/COPYING", "source/LICENSE",
        "source/THIRD_PARTY_LICENSES.md", "source/native/LICENSE.md",
    ):
        path = package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"GPL-3.0-or-later synthetic fixture source; never compile or execute.\n")
    build = {"product": "Gaussian Scene Workbench", "version": version,
             "legacyWebRuntimeIncluded": False}
    if installer_fixture:
        build["installerFixture"] = True
        build["channel"] = "installer-fixture"
    write_json(package / "build_manifest.json", build)
    write_json(package / "source/native/build_manifest.json", build)
    write_json(package / "backend_manifest.json", {"runtime": "External Python/Conda environment required",
                                                    "purpose": "Synthetic installer boundary fixture only"})
    write_fixture_source_manifest(package)
    return build


class InstallerFixture(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_INSTALLER_UNIT_ROOT", ROOT / "native/dist/installer-unit-tests"))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="fixture-", dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "payload 空白 日本語"
        self.build = create_fixture_payload(self.package)
        self.refresh_manifest()
        self.sources = self.root / "source checkout"
        for relative in installer.SOURCE_MATERIALS:
            path = self.sources / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture installer material: " + relative + "\n", encoding="utf-8")
        self.catalog = {
            source: {"zh_CN": "安装内容说明\n第二行", "en_US": "Installation contents\nSecond line",
                     "ja_JP": "インストール内容\n二行目"}
            for source in installer.MESSAGES.values()
        }
        self.save_catalog()
        self.attempt = 0

    def refresh_manifest(self):
        self.manifest = write_fixture_source_manifest(self.package)

    def save_manifest(self):
        write_json(self.package / "source-manifest.json", self.manifest)

    def save_catalog(self):
        write_json(self.sources / "native/i18n/catalog.json", self.catalog)
        write_json(self.sources / "packaging/windows/installer-catalog.json", self.catalog)

    def prepare_cli(self, *, work=None, output=None, package=None, test_build=True):
        self.attempt += 1
        work = work or self.root / ("work " + str(self.attempt))
        output = output or self.root / "output"
        arguments = [sys.executable, "-B", str(ROOT / "scripts/prepare_windows_installer.py"),
                     "--package-root", str(package or self.package), "--work-root", str(work),
                     "--output-root", str(output), "--source-root", str(self.sources)]
        if test_build:
            arguments.append("--test-build")
        environment = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8",
                                cwd=self.root, env=environment, timeout=30, check=False)
        return result, Path(work)

    def assert_rejected(self, **kwargs):
        result, work = self.prepare_cli(**kwargs)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Installer input validation failed:", result.stderr)
        self.assertFalse((work / "installer-build.json").exists())
        return result

    def assert_prepared(self, **kwargs):
        result, work = self.prepare_cli(**kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        self.assertEqual(record, json.loads((work / "installer-build.json").read_text(encoding="utf-8")))
        return record, work


class PayloadValidationTests(InstallerFixture):
    def test_complete_native_payload_is_accepted_without_executing_it(self):
        original = file_hash(self.package / "source-manifest.json")
        record, work = self.assert_prepared()
        self.assertEqual(record["version"], "0.3.1-native-preview")
        self.assertTrue(record["testBuild"])
        self.assertRegex(record["appId"], r"^GSW\.Test\.[a-f0-9]{32}$")
        self.assertEqual(record["locales"], ["zh_CN", "en_US", "ja_JP"])
        self.assertIs(record["trainingRuntimesBundled"], False)
        self.assertIs(record["automaticAppLaunch"], False)
        self.assertEqual(file_hash(self.package / "source-manifest.json"), original)
        self.assertFalse((self.package / "gsw-installation.ini").exists())
        marker = (work / "payload/gsw-installation.ini").read_text(encoding="utf-8-sig")
        self.assertIn("AppId=" + record["appId"] + "\n", marker)

    def test_source_manifest_rejects_parent_traversal_even_when_it_resolves_inside_source(self):
        entry = next(item for item in self.manifest["files"] if item["path"] == "native/src/main.cpp")
        entry["path"] = "native/src/../src/main.cpp"
        self.save_manifest()
        self.assert_rejected()

    def test_rehashed_manifest_cannot_hide_missing_native_shader_source(self):
        (self.package / "source/native/src/shaders/SurfelProjection.glsl").unlink()
        self.refresh_manifest()
        self.assert_rejected()

    def test_source_materials_for_rebuilding_installer_accompany_payload(self):
        _, work = self.assert_prepared()
        for relative in installer.SOURCE_MATERIALS:
            with self.subTest(material=relative):
                self.assertEqual((work / "payload/source" / relative).read_bytes(),
                                 (self.sources / relative).read_bytes())

    def test_upgrade_marker_is_ascii_without_bom_and_has_exact_installation_identity(self):
        record, work = self.assert_prepared()
        marker = (work / "payload/gsw-installation.ini").read_bytes()
        self.assertTrue(marker.startswith(b"[Installation]"))
        self.assertEqual(marker.decode("ascii").splitlines(),
                         ["[Installation]", "Product=Gaussian Scene Workbench", "AppId=" + record["appId"],
                          "Version=0.3.1-native-preview"])

    def test_missing_installer_build_material_rejects_setup_preparation(self):
        (self.sources / "scripts/build_windows_installer.ps1").unlink()
        self.assert_rejected()

    def test_missing_required_qt_runtime_or_platform_plugin_is_rejected(self):
        for relative in ("bin/Qt6Core.dll", "bin/Qt6Gui.dll", "bin/Qt6Widgets.dll",
                         "bin/Qt6OpenGL.dll", "bin/Qt6OpenGLWidgets.dll", "bin/Qt6Svg.dll",
                         "bin/platforms/qwindows.dll"):
            with self.subTest(runtime=relative):
                path = self.package / relative
                content = path.read_bytes()
                path.unlink()
                self.assert_rejected()
                path.write_bytes(content)

    def test_rehashed_manifest_cannot_hide_missing_blender_geometry_source(self):
        (self.package / "source/native/third_party/blender-gizmo/BlenderGizmoGeometry.cpp").unlink()
        self.refresh_manifest()
        self.assert_rejected()

    def test_executable_tampering_is_rejected(self):
        (self.package / "bin/Gaussian Scene Workbench.exe").write_bytes(b"not the accepted executable")
        self.assert_rejected()

    def test_corresponding_source_tampering_is_rejected(self):
        (self.package / "source/native/src/main.cpp").write_bytes(b"changed source\n")
        self.assert_rejected()

    def test_corresponding_source_size_tampering_is_rejected(self):
        self.manifest["files"][0]["size"] += 1
        self.save_manifest()
        self.assert_rejected()

    def test_unrecorded_source_is_rejected(self):
        (self.package / "source/added.cpp").write_bytes(b"unrecorded source\n")
        self.assert_rejected()

    def test_duplicate_source_record_is_rejected(self):
        self.manifest["files"].append(copy.deepcopy(self.manifest["files"][0]))
        self.manifest["sourceFileCount"] += 1
        self.save_manifest()
        self.assert_rejected()

    def test_empty_or_incomplete_source_manifest_is_rejected(self):
        for entries, count in (([], 0), (self.manifest["files"], 9999)):
            with self.subTest(count=count):
                self.manifest["files"], self.manifest["sourceFileCount"] = entries, count
                self.save_manifest()
                self.assert_rejected()

    def test_manifest_cannot_claim_another_main_program_license(self):
        self.manifest["license"] = "MIT"
        self.save_manifest()
        self.assert_rejected()

    def test_source_manifest_rejects_absolute_escaping_and_noncanonical_paths(self):
        entry = next(item for item in self.manifest["files"] if item["path"] == "native/src/main.cpp")
        outside = self.package / "outside.cpp"
        outside.write_bytes((self.package / "source/native/src/main.cpp").read_bytes())
        paths = ("../outside.cpp", str(outside), "native//src/main.cpp", "native/./src/main.cpp",
                 "native\\src\\main.cpp", "C:/Windows/main.cpp", "native/src/main.cpp:stream")
        for path in paths:
            with self.subTest(path=path):
                entry["path"] = path
                self.save_manifest()
                self.assert_rejected()

    def test_private_workspaces_and_generated_settings_are_not_distributed(self):
        for name in (".git", "__pycache__", "workspaces", "test-settings"):
            with self.subTest(name=name):
                path = self.package / name
                path.mkdir()
                self.assert_rejected()
                path.rmdir()

    def test_retired_web_runtime_is_not_distributed(self):
        for name in ("old.html", "old.js", "electron.exe", "Qt6WebEngineCore.dll"):
            with self.subTest(name=name):
                path = self.package / name
                path.write_bytes(b"not part of native setup\n")
                self.assert_rejected()
                path.unlink()

    def test_reparse_or_symlink_payload_is_rejected(self):
        target = self.root / "external source"
        target.mkdir()
        sentinel = target / "keep.txt"
        sentinel.write_bytes(b"external source must not change")
        link = self.package / "linked-content"
        try:
            link.symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            if os.name != "nt":
                self.skipTest("Cannot create a symlink in this environment: " + str(error))
            # NTFS directory junctions exercise the same reparse boundary and
            # do not require the symlink privilege on ordinary Windows users.
            result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
                                    capture_output=True, timeout=10, check=False)
            if result.returncode:
                self.skipTest("Neither symlink nor junction creation is permitted: " + str(error))
        try:
            self.assert_rejected()
            self.assertEqual(sentinel.read_bytes(), b"external source must not change")
        finally:
            # rmdir removes this one owned directory link, never its target.
            if link.is_symlink():
                link.unlink()
            else:
                link.rmdir()

    def test_unknown_or_legacy_product_identity_is_rejected(self):
        for changes in ({"product": "Another application"}, {"legacyWebRuntimeIncluded": True}):
            with self.subTest(changes=changes):
                write_json(self.package / "build_manifest.json", {**self.build, **changes})
                self.assert_rejected()

    def test_untrusted_version_cannot_inject_inno_definitions(self):
        for version in ("0.3", "0.3.1\n#define Attack 1", '0.3.1"', "../../outside"):
            with self.subTest(version=version):
                write_json(self.package / "build_manifest.json", {**self.build, "version": version})
                self.assert_rejected()

    def test_input_payload_is_not_used_as_output_or_work_directory(self):
        self.assert_rejected(work=self.package / "work")
        self.assert_rejected(output=self.package / "output")
        existing = self.root / "existing-work"
        existing.mkdir()
        marker = existing / "keep.txt"
        marker.write_bytes(b"must remain unchanged")
        self.assert_rejected(work=existing)
        self.assertEqual(marker.read_bytes(), b"must remain unchanged")

    def test_space_unicode_paths_are_quoted_in_generated_preprocessor_input(self):
        record, work = self.assert_prepared()
        generated = (work / "generated.iss").read_text(encoding="utf-8-sig")
        expected = '#define GswPayload "' + str(work / "payload") + '"\n'
        self.assertIn(expected, generated)
        self.assertIn('#define GswAppId "' + record["appId"] + '"\n', generated)
        self.assertNotIn("\x00", generated)


class InstallerTranslationTests(InstallerFixture):
    def test_three_language_custom_messages_are_generated_without_raw_line_breaks(self):
        _, work = self.assert_prepared()
        lines = (work / "messages.iss").read_text(encoding="utf-8-sig").splitlines()
        self.assertEqual(len(lines), 3 * len(installer.MESSAGES))
        self.assertIn("zh_CN.GswRuntimeTitle=安装内容说明%n第二行", lines)
        self.assertIn("en_US.GswRuntimeTitle=Installation contents%nSecond line", lines)
        self.assertIn("ja_JP.GswRuntimeTitle=インストール内容%n二行目", lines)

    def test_missing_locale_is_not_silently_fallbacked_to_another_language(self):
        self.catalog[installer.MESSAGES["GswRuntimeTitle"]].pop("ja_JP")
        self.save_catalog()
        self.assert_rejected()

    def test_empty_or_unsafe_custom_messages_are_rejected(self):
        entry = self.catalog[installer.MESSAGES["GswRuntimeTitle"]]
        for text in ("   ", "bad\rtext", "bad\x00text", "bad%ntext"):
            with self.subTest(text=repr(text)):
                entry["en_US"] = text
                self.save_catalog()
                self.assert_rejected()


class RemoteFixtureCliTests(InstallerFixture):
    def fixture_cli(self, *, output=None, remote=True):
        output = output or self.root / "remote installer fixture"
        environment = dict(os.environ, GITHUB_ACTIONS="true" if remote else "",
                           PYTHONIOENCODING="utf-8", PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run(
            [sys.executable, "-B", str(ROOT / "scripts/create_windows_installer_fixture.py"),
             "--output-root", str(output)], capture_output=True, text=True, encoding="utf-8",
            env=environment, cwd=self.root, timeout=30, check=False)
        return result, Path(output)

    def create_remote_fixture(self):
        result, output = self.fixture_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        self.assertEqual(record, json.loads((output / "fixture.json").read_text(encoding="utf-8")))
        return record

    def test_fixture_creator_refuses_local_environment_before_creating_output(self):
        result, output = self.fixture_cli(remote=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("remote GitHub Actions", result.stderr)
        self.assertFalse(output.exists())

    def test_fixture_creator_never_overwrites_an_existing_output_directory(self):
        output = self.root / "existing fixture output"
        output.mkdir()
        sentinel = output / "user-file.txt"
        sentinel.write_bytes(b"preserve this original")
        result, _ = self.fixture_cli(output=output)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"preserve this original")
        self.assertEqual([path.name for path in output.iterdir()], ["user-file.txt"])

    def test_remote_fixture_is_clearly_nonexecutable_marked_and_uses_repository_icon(self):
        record = self.create_remote_fixture()
        package = Path(record["packageRoot"])
        self.assertEqual(record["schema"], "gsw-installer-fixture-v1")
        self.assertEqual(record["version"], "0.0.0-installer-fixture")
        self.assertIs(record["installerFixture"], True)
        self.assertIs(record["applicationExecutable"], False)
        self.assertIs(record["productionReleaseAllowed"], False)
        self.assertFalse((package / "bin/Gaussian Scene Workbench.exe").read_bytes().startswith(b"MZ"))
        self.assertEqual(file_hash(package / "source/native/assets/gsw-app-icon.ico"),
                         file_hash(ROOT / "native/assets/gsw-app-icon.ico"))
        self.assertEqual((package / "COPYING").read_bytes(), (ROOT / "COPYING").read_bytes())
        self.assertIn("INSTALLER ACCEPTANCE FIXTURE ONLY", (package / "FIXTURE-ONLY.md").read_text(encoding="utf-8"))
        manifest = json.loads((package / "source-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["executableSha256"], file_hash(package / "bin/Gaussian Scene Workbench.exe"))
        for entry in manifest["files"]:
            self.assertEqual(file_hash(package / "source" / entry["path"]), entry["sha256"])

    def test_marked_remote_fixture_is_rejected_by_production_installer_cli(self):
        record = self.create_remote_fixture()
        result = self.assert_rejected(package=Path(record["packageRoot"]), test_build=False)
        self.assertIn("fixture", result.stderr.lower())

    def test_marked_remote_fixture_can_only_prepare_a_separate_test_installer(self):
        fixture = self.create_remote_fixture()
        record, _ = self.assert_prepared(package=Path(fixture["packageRoot"]), test_build=True)
        self.assertIs(record["testBuild"], True)
        self.assertIs(record["installerFixture"], True)
        self.assertEqual(record["version"], "0.0.0-installer-fixture")
        self.assertRegex(record["appId"], r"^GSW\.Test\.[a-f0-9]{32}$")


class InstallerDefinitionTests(unittest.TestCase):
    def setUp(self):
        text = (ROOT / "packaging/windows/gsw.iss").read_text(encoding="utf-8-sig")
        self.definition = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith(";"))

    def test_installer_has_no_auto_launch_or_recursive_user_data_deletion(self):
        sections = re.findall(r"^\[([^]]+)\]$", self.definition, re.MULTILINE)
        self.assertNotIn("Run", sections)
        self.assertNotIn("UninstallRun", sections)
        self.assertNotIn("UninstallDelete", sections)
        self.assertRegex(self.definition, r"(?m)^CloseApplications=no$")
        self.assertRegex(self.definition, r"(?m)^RestartApplications=no$")

    def test_installer_is_per_user_and_supports_all_three_explicit_languages(self):
        self.assertRegex(self.definition, r"(?m)^PrivilegesRequired=lowest$")
        for locale in ("zh_CN", "en_US", "ja_JP"):
            self.assertIn('Name: "' + locale + '"; MessagesFile:', self.definition)
        self.assertRegex(self.definition, r"(?m)^AppId=\{#GswAppId\}$")

    def test_unrelated_nonempty_directories_require_matching_installation_marker(self):
        self.assertIn("gsw-installation.ini", self.definition)
        self.assertIn("GswAppId", self.definition)
        self.assertIn("GswNonemptyDirectory", self.definition)

    def test_complete_payload_and_notices_are_installed_and_desktop_icon_is_optional(self):
        self.assertIn('Source: "{#GswPayload}\\*"; DestDir: "{app}";', self.definition)
        self.assertIn("recursesubdirs", self.definition)
        self.assertIn("LicenseFile={#GswPayload}\\COPYING", self.definition)
        self.assertRegex(self.definition, r'Name: "desktopicon";[^\n]+Flags: unchecked')

    def test_upgrade_preflight_checks_embedded_owned_files_without_terminating_processes(self):
        self.assertIn('Source: "{#GswPayload}\\gsw-owned-files.txt"; Flags: dontcopy', self.definition)
        self.assertIn("CreateFileW@kernel32.dll", self.definition)
        self.assertIn("CloseHandle@kernel32.dll", self.definition)
        self.assertIn("GswBusyInstall", self.definition)
        self.assertNotRegex(self.definition, r"(?i)TerminateProcess|taskkill|Stop-Process")


class InnoStringBoundaryTests(unittest.TestCase):
    def test_preprocessor_literal_preserves_unicode_spaces_and_backslashes(self):
        self.assertEqual(installer.inno_string("D:\\测试 安装\\日本語"), '"D:\\测试 安装\\日本語"')

    def test_preprocessor_literal_rejects_quotation_and_control_injection(self):
        for text in ('bad"path', "line\rbreak", "line\nbreak", "null\x00byte"):
            with self.subTest(text=repr(text)):
                with self.assertRaises(ValueError):
                    installer.inno_string(text)


class InstallerWorkflowTests(unittest.TestCase):
    def test_definition_changes_trigger_full_native_acceptance_for_push_and_pr(self):
        workflow = (ROOT / ".github/workflows/native-windows.yml").read_text(encoding="utf-8")
        triggers = workflow.split("\npermissions:", 1)[0]
        for event in ("push", "pull_request"):
            with self.subTest(event=event):
                block = re.search(r"(?ms)^  " + event + r":\n(.*?)(?=^  \w|\Z)", triggers)
                self.assertIsNotNone(block)
                self.assertRegex(block.group(1), r"(?m)^      - [\"']packaging/windows/\*\*[\"']$")


if __name__ == "__main__":
    unittest.main(verbosity=2)
