"""Prepare remote-only real Qt fixture packages and one HTTPS/TUF issuer.

Never execute locally by default. This creates only a new owned test tree. A/B
are small Qt windows, NOT GSW startup, rendering or installed-product evidence.
The helper's trust profile is compiled after this preparation; no public helper
runtime root, feed, key or unsafe-TLS parameter is introduced.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def write_json(path: Path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=True, sort_keys=True)
        stream.write("\n")


def digest(path: Path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def deploy(tool: Path, executable: Path, directory: Path):
    result = subprocess.run([str(tool), "--release", "--compiler-runtime", "--no-translations",
        "--no-opengl-sw", "--no-system-d3d-compiler", "--dir", str(directory), str(executable)],
        capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError("fixture_qt_deployment_failed")


def package(root: Path, executable: Path, label: str, repo: Path, tool: Path,
            commit: str, tree: str):
    (root / "bin").mkdir(parents=True, exist_ok=False)
    target = root / "bin/Gaussian Scene Workbench.exe"
    shutil.copyfile(executable, target)
    deploy(tool, target, target.parent)
    for name in ("COPYING", "LICENSE", "THIRD_PARTY_LICENSES.md", "native/LICENSE.md"):
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / name, destination)
    source_root = root / "source"
    for relative in ("native/tests/NativeUpdateApplicationFixture.cpp", "native/src/NativeStartupHealth.cpp",
            "native/src/NativeStartupHealth.h", "native/src/NativeInstallation.h", "native/src/NativeUpdateLocalPath.h",
            "native/third_party/qt-process-quote/QtProcessQuote.h", "native/third_party/qt-process-quote/README.md",
            "native/third_party/qt-process-quote/LICENSE.txt"):
        destination = source_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / relative, destination)
    cmake = ("# Isolated Qt update fixture, not the GSW product\n"
        "cmake_minimum_required(VERSION 3.24)\nproject(GswUpdateFixture LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets)\n"
        "qt_add_executable(fixture WIN32 tests/NativeUpdateApplicationFixture.cpp src/NativeStartupHealth.cpp)\n"
        "target_include_directories(fixture PRIVATE src)\n"
        "target_link_libraries(fixture PRIVATE Qt6::Core Qt6::Widgets bcrypt advapi32 user32)\n"
        f'target_compile_definitions(fixture PRIVATE GSW_UPDATE_FIXTURE_LABEL="{label}" GSW_UPDATER_FIXTURE_BUILD=1 NOMINMAX WIN32_LEAN_AND_MEAN)\n'
        'set_target_properties(fixture PROPERTIES OUTPUT_NAME "Gaussian Scene Workbench")\n'
        "if(MSVC)\n target_compile_options(fixture PRIVATE /utf-8 /Zc:__cplusplus)\nendif()\n")
    (source_root / "native/CMakeLists.txt").write_text(cmake, encoding="utf-8", newline="\n")
    (source_root / ".github/workflows").mkdir(parents=True)
    (source_root / ".github/workflows/fixture.yml").write_text(
        "# Preserved hidden source path; real CI is source-bound separately.\n", encoding="utf-8")
    sources = sorted(path for path in source_root.rglob("*") if path.is_file())
    write_json(root / "source-manifest.json", {
        "schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
        "executableSha256": digest(target), "sourceFileCount": len(sources),
        "files": [{"path": path.relative_to(source_root).as_posix(), "size": path.stat().st_size,
                   "sha256": digest(path)} for path in sources]})
    write_json(root / "build_manifest.json", {"product": "Gaussian Scene Workbench",
        "legacyWebRuntimeIncluded": False, "testOnly": True, "formalGswAcceptance": False,
        "fixtureLabel": label, "sourceCommit": commit, "sourceTree": tree})
    write_json(root / "backend_manifest.json", {"schema": "gsw.qt_update_fixture_backend/1",
        "testOnly": True, "trainingBackendIncluded": False})


def prepare(args):
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or not args.remote_fixture_only:
        raise ValueError("explicit_remote_windows_fixture_required")
    if not re.fullmatch("[a-f0-9]{40}", args.source_commit) or not re.fullmatch("[a-f0-9]{40}", args.source_tree):
        raise ValueError("source_identity_invalid")
    root = args.root.absolute()
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).absolute().parent.parent
    a, b = root / "package-A", root / "package-B"
    package(a, args.fixture_a.absolute(), "A", repo, args.windeployqt.absolute(), args.source_commit, args.source_tree)
    package(b, args.fixture_b.absolute(), "B", repo, args.windeployqt.absolute(), args.source_commit, args.source_tree)
    installation = root / "installation"
    (installation / "bin").mkdir(parents=True)
    launcher = installation / "bin/Gaussian Scene Workbench Launcher.exe"
    shutil.copyfile(args.launcher, launcher)
    deploy(args.windeployqt.absolute(), launcher, launcher.parent)
    if args.recovery:
        recovery = installation / "bin/GSW Update Fixture Recovery.exe"
        if args.recovery.name != recovery.name:
            raise ValueError("fixture_recovery_identity_invalid")
        shutil.copyfile(args.recovery, recovery)
        deploy(args.windeployqt.absolute(), recovery, recovery.parent)
    write_json(installation / "bootstrap.json", {"schema": "gsw.native_bootstrap/1", "protocol": 2,
        "product": "Fubukisama.GaussianSceneWorkbench.UpdateFixture", "platform": "windows-x64",
        "files": [{"path": path.relative_to(installation).as_posix(), "size": path.stat().st_size,
                   "sha256": digest(path)} for path in sorted((installation / "bin").rglob("*")) if path.is_file()]})
    (installation / "user-data").mkdir()
    (installation / "user-data/project-sentinel.gsw").write_bytes(b"owned-test-project-do-not-modify\x00\x01")
    (installation / "user-data/settings-sentinel.json").write_bytes(b'{"training":"paused","keep":true}\n')
    ready = root / "server-info.json"
    command = [sys.executable, "-I", "-B", str(repo / "scripts/native_update_network_fixture.py"),
        "--root", str(root / "https"), "--ready", str(ready)]
    flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    with (root / "server-stdout.log").open("xb") as stdout, (root / "server-stderr.log").open("xb") as stderr:
        server = subprocess.Popen(command, stdout=stdout, stderr=stderr, creationflags=flags, close_fds=True)
    try:
        deadline = time.monotonic() + 20
        while not ready.exists():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("fixture_https_unavailable")
            time.sleep(.1)
        issued = root / "issued"
        result = subprocess.run([sys.executable, "-I", "-B", str(repo / "scripts/create_native_update_fixture.py"),
            "--fixture-only", "--output-root", str(issued), "--server-info", str(ready),
            "--package-a", str(a), "--package-b", str(b), "--source-commit", args.source_commit,
            "--source-tree", args.source_tree], capture_output=True, timeout=240)
        if result.returncode:
            raise RuntimeError("fixture_tuf_issuance_failed")
        fixture = json.loads((issued / "fixture.json").read_text(encoding="utf-8"))
        report = {"schema": "gsw.update_end_to_end_preparation/1", "testOnly": True, "production": False,
            "formalGswAcceptance": False, "scope": "real-qt-fixture-stable-launcher-only", "root": str(root),
            "installationRoot": str(installation), "launcher": str(launcher), "serverPid": server.pid,
            "serverInfo": str(ready), "profile": str(issued / "profile.json"), "fixture": str(issued / "fixture.json"),
            "controlFile": fixture["controlFile"], "sourceCommit": args.source_commit, "sourceTree": args.source_tree}
        write_json(root / "preparation.json", report)
        return report
    except BaseException:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--fixture-a", type=Path, required=True)
    parser.add_argument("--fixture-b", type=Path, required=True)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--recovery", type=Path)
    parser.add_argument("--windeployqt", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    try:
        print(json.dumps(prepare(parser.parse_args()), ensure_ascii=True))
        return 0
    except Exception as error:
        print(json.dumps({"schema": "gsw.update_end_to_end_preparation/1", "testOnly": True,
            "ok": False, "code": "fixture_preparation_failed", "errorType": type(error).__name__}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
