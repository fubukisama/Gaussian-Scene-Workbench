"""Remote-only public CLI tracer for the official-name, disabled update engine."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile


HELPER = "Gaussian Scene Workbench Update Helper.exe"
RECOVERY = "Gaussian Scene Workbench Recovery.exe"


def sha(path: Path) -> str:
    if os.name == "nt" and path.is_absolute() and not str(path).startswith("\\\\?\\"):
        path = Path("\\\\?\\" + str(path))
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def tree_hashes(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): sha(path) for path in root.rglob("*") if path.is_file()}


def cli(args, name: str, operation: str, *arguments, ok=True, code=None) -> dict:
    result = subprocess.run([sys.executable, "-I", "-B", str(args.repository / "scripts/prepare_native_update_engine_component.py"),
        operation, *map(str, arguments)], capture_output=True, timeout=1800, creationflags=subprocess.CREATE_NO_WINDOW)
    response = json.loads(result.stdout)
    write_json(args.evidence / (name + ".json"), {"exitCode": result.returncode, "response": response,
        "stderr": result.stderr.decode("utf-8", "replace")})
    assert result.returncode == (0 if ok else 2) and response.get("ok") is ok, response
    assert response.get("publisherAuthenticated") is False and response.get("publicDistributionReady") is False
    if code is not None:
        assert response.get("code") == code, response
    return response


def merge_contract(args, archive: Path, manifest: Path, metadata: dict) -> None:
    original = args.root / "original-app-artifact-fixture"
    (original / "bin").mkdir(parents=True)
    (original / "source/native").mkdir(parents=True)
    app = original / "bin/Gaussian Scene Workbench.exe"
    app.write_bytes(b"OWNED APPLICATION MERGE FIXTURE: NOT EXECUTABLE; NEVER START\r\n")
    shutil.copyfile(args.runtime_root / "bin/Qt6Core.dll", original / "bin/Qt6Core.dll")
    old_source = original / "source/native/CMakeLists.txt"
    old_source.write_bytes(b"# old artifact CRLF source; canonical Git blobs replace this only in fresh output\r\n")
    old_manifest = {"schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
        "executableSha256": sha(app), "sourceFileCount": 1,
        "files": [{"path": "native/CMakeLists.txt", "size": old_source.stat().st_size, "sha256": sha(old_source)}]}
    write_json(original / "source-manifest.json", old_manifest)
    identity = args.root / "application-identity.json"
    write_json(identity, {"schema": "gsw.native_application_artifact/1", "sourceCommit": args.source_commit,
        "sourceTree": args.source_tree, "applicationSha256": sha(app),
        "runId": os.environ["GITHUB_RUN_ID"], "artifactId": 1, "scope": "non-executable-owned-merge-fixture"})
    before = tree_hashes(original)
    merged = args.root / "merged-preview-fixture"
    common = ["--repository", args.repository, "--archive", archive, "--manifest", manifest]
    response = cli(args, "merge-positive", "merge", *common, "--application-root", original,
        "--application-identity", identity, "--output-root", merged, code="ENGINE_COMPONENT_MERGED")
    receipt = json.loads((merged / "native-update-engine-merge.json").read_bytes())
    assert receipt["schema"] == "gsw.native_update_engine_merge/1" and receipt["applicationStarted"] is False
    assert receipt["publisherAuthenticated"] is False and receipt["publicDistributionReady"] is False
    assert response["mergeReceipt"]["inputApplicationPreserved"] is True
    assert sha(merged / "bin/Gaussian Scene Workbench.exe") == sha(app)
    assert tree_hashes(original) == before
    canonical_rows = {row["path"][len("native-update-engine-source/"):]: row for row in metadata["files"]
        if row["path"].startswith("native-update-engine-source/")}
    new_source = json.loads((merged / "source-manifest.json").read_bytes())
    assert new_source["executableSha256"] == sha(app) and new_source["sourceFileCount"] == len(canonical_rows)
    assert {row["path"]: (row["size"], row["sha256"]) for row in new_source["files"]} == {
        path: (row["size"], row["sha256"]) for path, row in canonical_rows.items()}
    for path, row in canonical_rows.items():
        assert sha(merged / "source" / path) == row["sha256"], path
    assert (merged / "source/native/CMakeLists.txt").read_bytes() != old_source.read_bytes()
    assert (merged / "source-provenance.json").read_bytes() == (merged / "native-update-engine-source-provenance.json").read_bytes()
    assert (merged / "native-update-engine-original-application-source.json").read_bytes() == (original / "source-manifest.json").read_bytes()

    conflict = args.root / "conflicting-runtime-app"
    shutil.copytree(original, conflict)
    (conflict / "bin/Qt6Core.dll").write_bytes(b"conflicting DLL bytes, never load")
    conflict_before = tree_hashes(conflict)
    rejected_output = args.root / "must-not-create-conflict"
    cli(args, "merge-conflicting-runtime", "merge", *common, "--application-root", conflict,
        "--application-identity", identity, "--output-root", rejected_output, ok=False, code="ENGINE_RUNTIME_CONFLICT")
    assert not rejected_output.exists() and tree_hashes(conflict) == conflict_before
    bad_identity = args.root / "wrong-source-identity.json"
    wrong = json.loads(identity.read_bytes())
    wrong["sourceCommit"] = "0" * 40
    write_json(bad_identity, wrong)
    rejected_output = args.root / "must-not-create-wrong-source"
    cli(args, "merge-wrong-source", "merge", *common, "--application-root", original,
        "--application-identity", bad_identity, "--output-root", rejected_output, ok=False, code="ENGINE_APPLICATION_SOURCE_DIFFERS")
    assert not rejected_output.exists() and tree_hashes(original) == before
    corrupt = args.root / "corrupt-engine.zip"
    shutil.copyfile(archive, corrupt)
    with corrupt.open("ab") as output:
        output.write(b"tampered archive after manifest")
    cli(args, "verify-corrupt-archive", "verify", "--repository", args.repository, "--archive", corrupt,
        "--manifest", manifest, ok=False, code="ENGINE_ARCHIVE_DIGEST_MISMATCH")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime-root", "repository", "trust-library", "cmake-cache", "build-executable",
                 "recovery-build-executable", "toolchain-receipt", "cargo-home", "root", "evidence"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_OS") != "Windows":
        parser.error("remote_windows_fixture_only")
    if args.root.exists() or args.root.is_symlink():
        parser.error("fresh_owned_root_required")
    args.root.mkdir(parents=True)
    args.evidence.mkdir(parents=True, exist_ok=True)
    recovery = args.runtime_root / "bin" / RECOVERY
    assert recovery.is_file() and sha(recovery) == sha(args.recovery_build_executable), "official_recovery_build_identity_differs"
    write_json(args.evidence / "official-recovery-input.json", {"schema": "gsw.engine_recovery_input/1",
        "testOnly": False, "fixtureBuild": False, "recoverySha256": sha(recovery),
        "buildExecutableSha256": sha(args.recovery_build_executable), "applicationStarted": False})
    output = args.root / "output"
    output.mkdir()
    archive, manifest = output / "native-update-engine-windows-x64.zip", output / "native-update-engine-component.json"
    command = [sys.executable, "-I", "-B", str(args.repository / "scripts/prepare_native_update_engine_component.py"), "prepare"]
    for name in ("runtime-root", "repository", "trust-library", "cmake-cache", "build-executable",
                 "toolchain-receipt", "cargo-home", "source-commit", "source-tree"):
        command.extend(["--" + name, str(getattr(args, name.replace("-", "_")))])
    command.extend(["--work-root", str(args.root / "component"), "--output-archive", str(archive),
                    "--output-manifest", str(manifest)])
    result = subprocess.run(command, capture_output=True, timeout=1800, creationflags=subprocess.CREATE_NO_WINDOW)
    response = json.loads(result.stdout)
    write_json(args.evidence / "prepare-cli.json", {"exitCode": result.returncode, "response": response,
        "stderr": result.stderr.decode("utf-8", "replace")})
    # Semantic RED: a real recognized CLI must produce a distributable engine,
    # not merely report a build/probe result or a missing command/file.
    assert result.returncode == 0 and response.get("ok") is True, response
    assert archive.is_file() and manifest.is_file(), response
    record = json.loads(manifest.read_bytes())
    assert record["schema"] == "gsw.native_update_engine_component/1", record
    assert record["sourceCommit"] == args.source_commit and record["sourceTree"] == args.source_tree
    assert record["configurationStatus"] == "NOT_CONFIGURED" and record["publicDistributionReady"] is False
    assert record["fixtureBuild"] is False and record["publisherAuthenticated"] is False
    assert record["recoveryPath"] == "bin/" + RECOVERY and record["recoverySha256"] == sha(recovery)
    with zipfile.ZipFile(archive) as payload:
        names = payload.namelist()
        assert "bin/" + HELPER in names
        assert "bin/" + RECOVERY in names
        assert "bin/Qt6Widgets.dll" in names and "bin/Qt6Gui.dll" in names and "bin/platforms/qwindows.dll" in names
        assert "bin/Qt6Core.dll" in names and "bin/Qt6Network.dll" in names
        assert "bin/tls/qschannelbackend.dll" in names
        assert all(name in {"bin/" + HELPER, "bin/" + RECOVERY} for name in names if name.startswith("bin/") and name.casefold().endswith(".exe"))
        assert any(name.startswith("native-update-engine-source/.github/") for name in names)
        assert any(name.startswith("native-update-engine-source/native/updater-trust/vendor-archives/") for name in names)
        assert {"msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll"} <= {
            Path(name).name.casefold() for name in names if name.startswith("bin/")}
        check = args.root / "extracted-check"
        check.mkdir()
        # This trusted own output is still read back with the packager's verifier
        # before any extracted executable is run.
        verify = subprocess.run([sys.executable, "-I", "-B", str(args.repository / "scripts/prepare_native_update_engine_component.py"),
            "verify", "--repository", str(args.repository), "--archive", str(archive), "--manifest", str(manifest)],
            capture_output=True, timeout=1800, creationflags=subprocess.CREATE_NO_WINDOW)
        verified = json.loads(verify.stdout)
        write_json(args.evidence / "verify-cli.json", {"exitCode": verify.returncode, "response": verified})
        assert verify.returncode == 0 and verified.get("ok") is True, verified
        for name in names:
            if name.startswith("bin/"):
                payload.extract(name, check)
    helper = check / "bin" / HELPER
    assert sha(helper) == sha(args.runtime_root / "bin" / HELPER)
    environment = dict(os.environ, PATH=str(check / "bin") + os.pathsep + os.environ.get("PATH", ""))
    for name in ("GSW_UPDATER_FIXTURE_PROFILE_PATH", "GSW_UPDATER_PRODUCTION_PROFILE_PATH"):
        environment.pop(name, None)
    outcome = subprocess.run([str(helper), "--check"], input=b"", capture_output=True, timeout=30,
        env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
    events = [json.loads(line) for line in outcome.stdout.splitlines()]
    write_json(args.evidence / "official-helper-check.json", {"exitCode": outcome.returncode, "events": events,
        "stderr": outcome.stderr.decode("utf-8", "replace"), "helperSha256": sha(helper)})
    assert outcome.returncode == 2 and events[-1]["code"] == "NOT_CONFIGURED", events
    assert not (check / ".update-trust-anchor").exists() and not (check / "updates/trust").exists()
    assert not (check / "current.json").exists() and not (check / "bin/Gaussian Scene Workbench.exe").exists()
    recovery = check / "bin" / RECOVERY
    assert sha(recovery) == record["recoverySha256"]
    before_recovery = tree_hashes(check)
    for index, arguments in enumerate((("--root", str(args.root / "unaccepted-root")),
            ("--feed", "https://example.invalid/not-accepted/"), ("--key", "not-a-key"), ("--verified",))):
        rejected = subprocess.run([str(recovery), *arguments], capture_output=True, timeout=30,
            env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
        write_json(args.evidence / ("official-recovery-reject-" + str(index) + ".json"), {
            "exitCode": rejected.returncode, "arguments": list(arguments),
            "stdout": rejected.stdout.decode("utf-8", "replace"), "stderr": rejected.stderr.decode("utf-8", "replace"),
            "scope": "official-recovery-unsafe-cli-rejection-only", "guiRecoveryAcceptance": False})
        assert rejected.returncode == 2, (arguments, rejected.returncode)
        assert tree_hashes(check) == before_recovery
    assert not (args.root / "unaccepted-root").exists()
    merge_contract(args, archive, manifest, record)
    write_json(args.evidence / "acceptance.json", {"schema": "gsw.native_update_engine_acceptance/1", "ok": True,
        "scope": "official-helper-default-disabled-component-only", "productionProfile": False,
        "configurationStatus": "NOT_CONFIGURED", "publicDistributionReady": False, "applicationStarted": False,
        "publisherAuthenticated": False, "sourceCommit": args.source_commit, "sourceTree": args.source_tree,
        "mergeScope": "non-executable-owned-app-fixture-only", "originalApplicationArtifactPreserved": True,
        "contracts": ["official-default-helper", "NOT_CONFIGURED", "whole-component-zip", "exact-git-and-raw-crate-source",
            "official-recovery-presence-and-identity", "recovery-unsafe-cli-rejected",
            "canonical-source-rebuild", "matching-runtime-shared", "conflicting-runtime-refused", "wrong-source-refused",
            "tampered-archive-refused", "fresh-output-only"],
        "archiveSha256": sha(archive), "componentManifestSha256": sha(manifest), "helperSha256": sha(helper),
        "recoverySha256": sha(recovery), "guiRecoveryAcceptance": False})
    print("Native update engine component contract PASS (default NOT_CONFIGURED only).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
