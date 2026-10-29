"""Remote-only FIRST-01 tracer; Python is setupLab, never the client runtime.

The native harness creates an actual restricted medium-integrity child token,
private desktop and an environment without developer tools or credentials.
The first recognized --install implementation deliberately returns a typed RED.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def write_json(path: Path, value: dict):
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, ensure_ascii=True, indent=2)
        output.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", type=Path, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--loader-probe", type=Path)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_OS") != "Windows":
        parser.error("remote_windows_lab_only")
    if not args.root.is_absolute() or args.root.drive.casefold() != "d:" or args.root.exists() or args.root.is_symlink():
        parser.error("fresh_owned_d_drive_fixture_root_required")
    args.evidence.mkdir(parents=True, exist_ok=True)
    args.root.mkdir(parents=True)
    destination = args.root / "First install with spaces 日本語 中文"
    with args.bootstrap.open("rb") as image:
        pinned = hashlib.file_digest(image, "sha256").hexdigest()
    # The controller selects an exact locally built CI artifact independently
    # of the update feed. This pin is only test-bootstrap source authorization,
    # not an Authenticode certificate or product/release acceptance proof.
    write_json(args.evidence / "bootstrap-source-pin.json", {"schema": "gsw.bootstrap_fixture_authorization/1",
        "testOnly": True, "sourceAuthority": "root-approved-out-of-band-ci-artifact-pin",
        "sourceCommit": os.environ.get("GITHUB_SHA"), "runId": os.environ.get("GITHUB_RUN_ID"),
        "runAttempt": os.environ.get("GITHUB_RUN_ATTEMPT"), "bootstrapSha256": pinned,
        "authenticodeClaim": False, "production": False, "privateKeyCreated": False})
    command = [str(args.harness.absolute()), "--bootstrap", str(args.bootstrap.absolute()),
        "--destination", str(destination), "--expected-sha256", pinned,
        "--evidence", str(args.evidence.absolute() / "terminal-native.json")]
    if args.loader_probe:
        with args.loader_probe.open("rb") as image:
            probe_pin = hashlib.file_digest(image, "sha256").hexdigest()
        command.extend(["--loader-probe", str(args.loader_probe.absolute()), "--expected-probe-sha256", probe_pin])
    result = subprocess.run(command, capture_output=True, timeout=210, creationflags=subprocess.CREATE_NO_WINDOW)
    native = json.loads(result.stdout)
    write_json(args.evidence / "first-install-tracer.json", {"schema": "gsw.bootstrap_first_install_tracer/1",
        "testOnly": True, "production": False, "harnessExitCode": result.returncode, "harness": native,
        "stderr": result.stderr.decode("utf-8", "replace"), "expectedBootstrapCode": "BOOTSTRAP_COMMITTED",
        "scope": "actual-restricted-medium-token-native-client-path", "physicalToolRemovalClaim": False})
    # Environment/setup failure is separate from the recognized semantic RED.
    assert result.returncode == 0 and native.get("ok") is True, native
    facts = native["terminalToken"]
    assert facts["restricted"] is True and facts["integrityRid"] == 8192
    assert facts["adminEnabled"] is False and facts["elevated"] is False and facts["sandboxInert"] is False
    assert facts["systemAccount"] is False and facts["unexpectedEnabledPrivilegeCount"] == 0
    assert native["setupLabTokenUsedByClient"] is False and native["passwordCreated"] is False
    assert native["authenticode"]["status"] == "NOT_SIGNED" and native["authenticode"]["publisherAuthenticated"] is False
    child = native["childResponse"]
    assert child["terminalEnvironment"]["discoverableDeveloperTools"] == []
    assert child["terminalEnvironment"]["sensitiveEnvironmentPresent"] is False
    # Do not call an administrator process, missing dependency or mocked true
    # a first-install RED. Only the real endpoint reaches this capability gate.
    assert native["childExitCode"] == 0 and child.get("ok") is True and child.get("code") == "BOOTSTRAP_COMMITTED", child
    pointer = json.loads((destination / "current.json").read_bytes())
    assert pointer["schema"] == "gsw.native_active_version/1"
    assert re.fullmatch(r"[a-f0-9]{64}", pointer["executableSha256"])
    write_json(args.evidence / "acceptance.json", {"schema": "gsw.bootstrap_first_install_acceptance/1",
        "testOnly": True, "production": False, "authenticodeClaim": False, "accepted": True,
        "scope": "restricted-native-first-install-fixture-only", "physicalToolRemovalClaim": False,
        "sourceCommit": os.environ.get("GITHUB_SHA"), "runId": os.environ.get("GITHUB_RUN_ID")})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
