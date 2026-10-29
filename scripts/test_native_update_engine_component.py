"""Remote-only public CLI tracer for the official-name, disabled update engine."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile


HELPER = "Gaussian Scene Workbench Update Helper.exe"


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime-root", "repository", "trust-library", "cmake-cache", "build-executable",
                 "toolchain-receipt", "cargo-home", "root", "evidence"):
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
    with zipfile.ZipFile(archive) as payload:
        names = payload.namelist()
        assert "bin/" + HELPER in names
        assert "bin/Qt6Core.dll" in names and "bin/Qt6Network.dll" in names
        assert "bin/tls/qschannelbackend.dll" in names
        assert all(name == "bin/" + HELPER for name in names if name.startswith("bin/") and name.casefold().endswith(".exe"))
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
        payload.extractall(check)
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
    write_json(args.evidence / "acceptance.json", {"schema": "gsw.native_update_engine_acceptance/1", "ok": True,
        "scope": "official-helper-default-disabled-component-only", "productionProfile": False,
        "configurationStatus": "NOT_CONFIGURED", "publicDistributionReady": False, "applicationStarted": False,
        "publisherAuthenticated": False, "sourceCommit": args.source_commit, "sourceTree": args.source_tree,
        "archiveSha256": sha(archive), "componentManifestSha256": sha(manifest), "helperSha256": sha(helper)})
    print("Native update engine component contract PASS (default NOT_CONFIGURED only).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
