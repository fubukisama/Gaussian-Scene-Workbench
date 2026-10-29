"""Remote-only production-UI -> signed fixture helper -> next stable-launch B.

prepare creates a new A archive before an ephemeral test root is issued. Build
the distinct fixture Rust library/helper from its returned public profile, then
run. No production root, official runtime override, user application or real
training is involved. The official Engine must remain separately unconfigured.
"""
from __future__ import annotations

import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import unittest


PRODUCT = "Fubukisama.GaussianSceneWorkbench.UpdateFixture"
ORIGINAL_HELPER = "GSW Update Fixture Helper.exe"
UI_HELPER_ALIAS = "Gaussian Scene Workbench Update Helper.exe"
REPO = Path(__file__).absolute().parent.parent


def load_script(name):
    spec = importlib.util.spec_from_file_location("gsw_automatic_" + name,
        REPO / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require_remote():
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true":
        raise ValueError("explicit_remote_windows_fixture_required")


def package_automatic_a(root, executable, tool, commit, tree):
    (root / "bin").mkdir(parents=True, exist_ok=False)
    target = root / "bin/Gaussian Scene Workbench.exe"
    shutil.copyfile(executable, target)
    PREPARE.deploy(tool, target, target.parent)
    for name in ("COPYING", "LICENSE", "THIRD_PARTY_LICENSES.md", "native/LICENSE.md"):
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / name, destination)
    source_root = root / "source"
    sources = ["native/tests/NativeUpdateAutomaticFlowFixture.cpp",
        "native/tests/NativeUpdateUiHelperFixture.cpp", "native/tests/NativeUpdateUiTests.cpp",
        "native/updater-ui/CMakeLists.txt", "native/i18n/catalog.json", "scripts/native_i18n.py"]
    for stem in ("AppLanguage", "NativeInstallation", "NativeStartupHealth", "NativeUpdateUi"):
        sources.extend("native/src/" + stem + suffix for suffix in (".cpp", ".h"))
    sources.append("native/src/NativeUpdateLocalPath.h")
    for prefix in ("native/third_party/nlohmann", "native/third_party/qt-process-quote"):
        sources.extend(path.relative_to(REPO).as_posix()
            for path in sorted((REPO / prefix).rglob("*")) if path.is_file())
    for relative in sources:
        destination = source_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / relative, destination)
    (source_root / "native/CMakeLists.txt").write_text(
        "# Private signed automatic-update fixture, not the GSW application\n"
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(GswSignedAutomaticFixture LANGUAGES CXX)\n"
        "add_subdirectory(updater-ui)\n", encoding="utf-8", newline="\n")
    (source_root / "BUILD_FIXTURE.md").write_text(
        "# Private signed automatic-update fixture A\n\n"
        "Not the GSW application and not a production distribution. With Qt6.8.3 "
        "Core/Widgets/Test/LinguistTools and Python3.12 available, rebuild using:\n\n"
        "cmake -S native/updater-ui -B build -DGSW_UPDATER_SIGNED_AUTOMATIC_FIXTURE=ON\n"
        "cmake --build build --config Release --target gsw_update_ui_signed_fixture_a\n\n"
        "The production UI library is unchanged. Test-only code owns a separate "
        "INI settings identity and observes the real public widgets. The signed "
        "helper uses a separately compiled ephemeral Fixture profile; it is not "
        "the unconfigured official Engine.\n", encoding="utf-8", newline="\n")
    files = sorted(path for path in source_root.rglob("*") if path.is_file())
    PREPARE.write_json(root / "source-manifest.json", {
        "schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
        "executableSha256": PREPARE.digest(target), "sourceFileCount": len(files),
        "files": [{"path": path.relative_to(source_root).as_posix(),
            "size": path.stat().st_size, "sha256": PREPARE.digest(path)} for path in files]})
    PREPARE.write_json(root / "build_manifest.json", {"product": "Gaussian Scene Workbench",
        "legacyWebRuntimeIncluded": False, "testOnly": True, "formalGswAcceptance": False,
        "fixtureLabel": "A", "scope": "production-ui-signed-automatic-fixture-only",
        "sourceCommit": commit, "sourceTree": tree})
    PREPARE.write_json(root / "backend_manifest.json", {
        "schema": "gsw.qt_update_fixture_backend/1", "testOnly": True,
        "trainingBackendIncluded": False})


def prepare(args):
    require_remote()
    if not args.remote_fixture_only:
        raise ValueError("explicit_remote_windows_fixture_required")
    for value in (args.source_commit, args.source_tree):
        if not re.fullmatch("[a-f0-9]{40}", value):
            raise ValueError("source_identity_invalid")
    root = args.root.absolute()
    root.mkdir(parents=True, exist_ok=False)
    a, b = root / "package-A", root / "package-B"
    package_automatic_a(a, args.fixture_a.absolute(), args.windeployqt.absolute(),
        args.source_commit, args.source_tree)
    PREPARE.package(b, args.fixture_b.absolute(), "B", REPO, args.windeployqt.absolute(),
        args.source_commit, args.source_tree)
    installation = root / "installation"
    (installation / "bin").mkdir(parents=True)
    launcher = installation / "bin/Gaussian Scene Workbench Launcher.exe"
    shutil.copyfile(args.launcher, launcher)
    PREPARE.deploy(args.windeployqt.absolute(), launcher, launcher.parent)
    PREPARE.write_json(installation / "bootstrap.json", {
        "schema": "gsw.native_bootstrap/1", "protocol": 2, "product": PRODUCT,
        "platform": "windows-x64", "files": [{"path": path.relative_to(installation).as_posix(),
            "size": path.stat().st_size, "sha256": PREPARE.digest(path)}
            for path in sorted((installation / "bin").rglob("*")) if path.is_file()]})
    (installation / "user-data").mkdir()
    (installation / "user-data/project-sentinel.gsw").write_bytes(b"owned-automatic-project\x00\x01")
    (installation / "user-data/paused-checkpoint-sentinel").write_bytes(b"owned-paused-state-unchanged")
    ready = root / "server-info.json"
    command = [sys.executable, "-I", "-B", str(REPO / "scripts/native_update_network_fixture.py"),
        "--root", str(root / "https"), "--ready", str(ready)]
    with (root / "server-stdout.log").open("xb") as stdout, (root / "server-stderr.log").open("xb") as stderr:
        server = subprocess.Popen(command, stdout=stdout, stderr=stderr,
            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP, close_fds=True)
    try:
        deadline = time.monotonic() + 20
        while not ready.exists():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("fixture_https_unavailable")
            time.sleep(.1)
        issued = root / "issued"
        result = subprocess.run([sys.executable, "-I", "-B",
            str(REPO / "scripts/create_native_update_fixture.py"), "--fixture-only",
            "--output-root", str(issued), "--server-info", str(ready),
            "--package-a", str(a), "--package-b", str(b),
            "--source-commit", args.source_commit, "--source-tree", args.source_tree],
            capture_output=True, timeout=240, creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:
            raise RuntimeError("fixture_tuf_issuance_failed")
        fixture = json.loads((issued / "fixture.json").read_text(encoding="utf-8"))
        if fixture.get("product") != PRODUCT or fixture.get("testOnly") is not True:
            raise ValueError("fixture_product_invalid")
        report = {"schema": "gsw.update_end_to_end_preparation/1", "testOnly": True,
            "production": False, "formalGswAcceptance": False,
            "scope": "production-ui-signed-automatic-fixture-only", "root": str(root),
            "installationRoot": str(installation), "launcher": str(launcher), "serverPid": server.pid,
            "serverInfo": str(ready), "profile": str(issued / "profile.json"),
            "fixture": str(issued / "fixture.json"), "controlFile": fixture["controlFile"],
            "sourceCommit": args.source_commit, "sourceTree": args.source_tree,
            "fixtureAExecutableSha256": PREPARE.digest(a / "bin/Gaussian Scene Workbench.exe")}
        PREPARE.write_json(root / "preparation.json", report)
        return report
    except BaseException:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        raise


class AutomaticFlow(unittest.TestCase):
    @staticmethod
    def read_marker(path):
        # QSaveFile must be free to replace this advisory marker while it is
        # observed. A normal Python Windows open omits FILE_SHARE_DELETE.
        import msvcrt
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong,
            ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except BaseException:
            kernel.CloseHandle(handle)
            raise
        with os.fdopen(descriptor, "rb") as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise ValueError("fixture_marker_too_large")
        return json.loads(raw)

    def wait_marker(self, path, phase, timeout):
        deadline, observed = time.monotonic() + timeout, {}
        while time.monotonic() < deadline:
            if path.is_file():
                observed = self.read_marker(path)
                self.assertNotEqual(observed.get("phase"), "failed", observed)
                if observed.get("phase") == phase:
                    return observed
            time.sleep(.05)
        self.fail("actual automatic UI state not reached: " + json.dumps(observed))

    def test_opted_in_production_controller_installs_signed_B_for_next_stable_launch(self):
        prepared = json.loads(ARGS.preparation.read_text(encoding="utf-8-sig"))
        self.assertEqual(prepared.get("scope"), "production-ui-signed-automatic-fixture-only")
        self.assertIs(prepared.get("testOnly"), True)
        self.assertIs(prepared.get("formalGswAcceptance"), False)
        base, root = Path(prepared["root"]).absolute(), Path(prepared["installationRoot"]).absolute()
        self.assertEqual(root, base / "installation")
        self.assertEqual(ARGS.helper.name, ORIGINAL_HELPER)
        fixture = json.loads(Path(prepared["fixture"]).read_text(encoding="utf-8"))
        profile = json.loads(Path(prepared["profile"]).read_text(encoding="utf-8"))
        self.assertEqual((fixture["product"], profile["product"]), (PRODUCT, PRODUCT))
        self.assertEqual(PREPARE.digest(Path(prepared["profile"])), fixture["profileSha256"])
        native_helper = root / "bin" / ORIGINAL_HELPER
        alias = root / "bin" / UI_HELPER_ALIAS
        self.assertFalse(native_helper.exists())
        self.assertFalse(alias.exists())
        shutil.copyfile(ARGS.helper, native_helper)
        shutil.copyfile(ARGS.helper, alias)
        PREPARE.deploy(ARGS.windeployqt, native_helper, root / "bin")
        self.assertEqual(PREPARE.digest(alias), PREPARE.digest(native_helper))
        bootstrap = {"schema": "gsw.native_bootstrap/1", "protocol": 2,
            "product": PRODUCT, "platform": "windows-x64",
            "files": [{"path": path.relative_to(root).as_posix(), "size": path.stat().st_size,
                       "sha256": PREPARE.digest(path)}
                      for path in sorted((root / "bin").rglob("*")) if path.is_file()]}
        (root / "bootstrap.json").write_text(json.dumps(bootstrap), encoding="utf-8")
        helper = E2E.EndToEnd(methodName="runTest")
        helper.preparation, helper.installation, helper.evidence = prepared, root, ARGS.evidence
        helper.helper, helper.trust_cli = native_helper, ARGS.trust_cli
        helper.fixture, helper.steps = fixture, []
        helper.select("A")
        # Prove the actual linked fixture trust, not merely a test-only label.
        trust = subprocess.run([str(ARGS.trust_cli), "-"], input=json.dumps({"schema": 1,
            "operation": "resolve", "installation_root": str(root), "channel": "stable"}).encode(),
            capture_output=True, timeout=40, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(trust.returncode, 0, trust.stderr.decode(errors="replace"))
        trust_result = json.loads(trust.stdout)
        self.assertIs(trust_result.get("ok"), True, trust_result)
        self.assertEqual(trust_result["offer"]["product"], PRODUCT)
        self.assertEqual(trust_result["offer"]["buildId"], "fixture-a")
        self.assertEqual(trust_result["offer"]["archiveSha256"], fixture["archives"]["A"]["archiveSha256"])
        E2E.write_json(ARGS.evidence / "fixture-identity.json", {
            "schema": "gsw.signed_automatic_fixture_identity/1", "testOnly": True,
            "production": False, "formalGswAcceptance": False,
            "actualResolvedProduct": trust_result["offer"]["product"],
            "fixtureProfileSha256": fixture["profileSha256"],
            "nativeHelperSha256": PREPARE.digest(native_helper),
            "uiHelperAliasSha256": PREPARE.digest(alias),
            "sourceCommit": prepared["sourceCommit"], "sourceTree": prepared["sourceTree"]})
        checked = helper.helper_request("automatic-fixture-check-A", "--check", "stable")
        self.assertEqual(checked[-1]["code"], "VERIFIED")
        helper.helper_request("automatic-install-A", "--install", "stable")
        helper.active("fixture-a")
        self.assertEqual(PREPARE.digest(root / "versions/fixture-a/bin/Gaussian Scene Workbench.exe"),
            prepared["fixtureAExecutableSha256"])
        protected = {name: E2E.tree_hashes(root / name)
                     for name in ("bin", "user-data", "versions/fixture-a")}
        bootstrap_bytes = (root / "bootstrap.json").read_bytes()
        helper.select("B")
        request_log = Path(json.loads(Path(prepared["serverInfo"]).read_text(
            encoding="utf-8"))["requestLog"])
        log_before = request_log.stat().st_size
        marker, release = root / "fixture-evidence/automatic-A.json", root / "fixture-evidence/release-A"
        environment = os.environ.copy()
        environment["QT_QPA_PLATFORM"] = "windows"
        for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH",
                    "GSW_UPDATE_UI_FIXTURE_APPLICATION", "GSW_UPDATE_FIXTURE_HEALTH_MODE",
                    "GSW_UPDATE_FIXTURE_MARKER"):
            environment.pop(key, None)
        launcher = subprocess.Popen([str(root / "bin/Gaussian Scene Workbench Launcher.exe")],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW)
        a_process = None
        try:
            self.assertEqual(launcher.wait(timeout=15), 0)
            armed = self.wait_marker(marker, "armed", 15)
            self.assertIs(armed["automaticCheck"], True)
            self.assertIs(armed["prepareForNextLaunch"], True)
            a_process = E2E.ProcessDiagnostic(armed["pid"])
            before = a_process.snapshot()
            self.assertIs(before.get("opened"), True)
            self.assertIs(before.get("signalled"), False)
            self.assertEqual(Path(before["image"]).resolve(),
                (root / "versions/fixture-a/bin/Gaussian Scene Workbench.exe").resolve())
            activated = self.wait_marker(marker, "activated", 75)
            self.assertIs(activated["foregroundUnchanged"], True)
            self.assertEqual(activated["foregroundWindow"], armed["foregroundWindow"])
            pointer_b = helper.active("fixture-b")
            # The real current/slot verification plus an actual B process are
            # acceptance. The widget marker alone cannot grant an installation.
            b = helper.launch("B")
            self.assertNotEqual(b["pid"], armed["pid"])
            after = a_process.snapshot()
            self.assertIs(after["signalled"], False)
            self.assertEqual(after["processCreationTime"], before["processCreationTime"])
            self.assertEqual(helper.active("fixture-b"), pointer_b)
            for name, hashes in protected.items():
                self.assertEqual(E2E.tree_hashes(root / name), hashes, name)
            self.assertEqual((root / "bootstrap.json").read_bytes(), bootstrap_bytes)
            wire = request_log.read_bytes()[log_before:]
            requests = [json.loads(row) for row in wire.splitlines() if row.strip()]
            self.assertTrue(any(row.get("path", "").startswith("/repository/stable/") for row in requests))
            self.assertTrue(any(row.get("path", "").startswith("/downloads/fixture-b-") for row in requests))
            self.assertTrue(all(not row.get("authorization_present") and not row.get("cookie_present")
                for row in requests))
            release.write_bytes(b"owned-signed-automatic-fixture-release\n")
            self.assertEqual(a_process.kernel.WaitForSingleObject(a_process.handle, 10000), 0)
            self.assertEqual(a_process.snapshot()["exitCode"], 0)
            final = self.wait_marker(marker, "normal_exit", 5)
            self.assertIs(final["foregroundUnchanged"], True)
            E2E.write_json(ARGS.evidence / "automatic-result.json", {
                "schema": "gsw.signed_automatic_flow_evidence/1", "passed": True,
                "testOnly": True, "production": False, "formalGswAcceptance": False,
                "scope": "production-controller-signed-fixture-helper-next-stable-launch-only",
                "trustProduct": PRODUCT, "fixtureProfileSha256": fixture["profileSha256"],
                "nativeHelperSha256": PREPARE.digest(native_helper),
                "privateUiHelperAliasSameBytes": True, "originalFixtureHelperNameRetained": True,
                "uiFixtureRootOverrideCompiled": False, "productScheduleChanged": False,
                "armed": armed, "activated": activated, "oldWindowBirth": before["processCreationTime"],
                "actualNextLaunchBuild": b["buildId"], "actualNextLaunchPid": b["pid"],
                "oldAStayedAlive": True, "bootstrapUnchanged": True, "userDataUnchanged": True,
                "originalSlotUnchanged": True, "sourceCommit": prepared["sourceCommit"],
                "sourceTree": prepared["sourceTree"], "optimizerResume3DGS": "NOT_RUN",
                "optimizerResume2DGS": "NOT_RUN", "steps": helper.steps})
        finally:
            if launcher.poll() is None:
                launcher.kill()
                launcher.wait(timeout=5)
            release.parent.mkdir(parents=True, exist_ok=True)
            if not release.exists():
                release.write_bytes(b"owned-signed-automatic-fixture-release\n")
            if a_process:
                a_process.kernel.WaitForSingleObject(a_process.handle, 10000)
                a_process.close()
            try:
                if marker.is_file():
                    E2E.write_json(ARGS.evidence / "automatic-A-observation.json", self.read_marker(marker))
                # Server logs deliberately exclude queries, credentials and
                # capabilities. Retain only this bounded test's network rows.
                with request_log.open("rb") as stream:
                    stream.seek(log_before)
                    wire = stream.read(1024 * 1024 + 1)
                if len(wire) <= 1024 * 1024:
                    (ARGS.evidence / "automatic-https-requests.jsonl").write_bytes(wire)
            except Exception:
                pass  # Diagnostics must not replace a primary failure verdict.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    first = commands.add_parser("prepare")
    first.add_argument("--remote-fixture-only", action="store_true")
    first.add_argument("--root", type=Path, required=True)
    first.add_argument("--fixture-a", type=Path, required=True)
    first.add_argument("--fixture-b", type=Path, required=True)
    first.add_argument("--launcher", type=Path, required=True)
    first.add_argument("--windeployqt", type=Path, required=True)
    first.add_argument("--source-commit", required=True)
    first.add_argument("--source-tree", required=True)
    second = commands.add_parser("run")
    second.add_argument("--remote-fixture-only", action="store_true")
    second.add_argument("--preparation", type=Path, required=True)
    second.add_argument("--helper", type=Path, required=True)
    second.add_argument("--trust-cli", type=Path, required=True)
    second.add_argument("--windeployqt", type=Path, required=True)
    second.add_argument("--evidence", type=Path, required=True)
    global ARGS, PREPARE, E2E
    ARGS = parser.parse_args()
    require_remote()
    if not ARGS.remote_fixture_only:
        parser.error("explicit_remote_windows_fixture_required")
    PREPARE = load_script("prepare_native_update_end_to_end")
    if ARGS.command == "prepare":
        print(json.dumps(prepare(ARGS), ensure_ascii=True))
        return 0
    for name in ("preparation", "helper", "trust_cli", "windeployqt", "evidence"):
        setattr(ARGS, name, getattr(ARGS, name).absolute())
    ARGS.evidence.mkdir(parents=True, exist_ok=False)
    E2E = load_script("test_native_update_end_to_end")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AutomaticFlow)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
