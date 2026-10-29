"""Prepare a fresh signed owned root and run the real Recovery Qt UI seam remotely.

The private QtTest executable is not distributed. Its root locator exists only
in that test source; production Recovery accepts no root/feed/key/time override.
The actual installed fixture Helper provides TUF history and complete-slot
verification. No production keys, installed GSW, projects or training are used.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess


support_spec = importlib.util.spec_from_file_location("gsw_recovery_chooser_support",
    Path(__file__).with_name("test_native_update_end_to_end.py"))
support = importlib.util.module_from_spec(support_spec)
support_spec.loader.exec_module(support)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true")
    parser.add_argument("--preparation", type=Path, required=True)
    parser.add_argument("--trust-cli", type=Path, required=True)
    parser.add_argument("--qt-test", type=Path, required=True)
    parser.add_argument("--windeployqt", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or not args.remote_fixture_only:
        raise ValueError("explicit_remote_windows_fixture_required")
    preparation = json.loads(args.preparation.read_text(encoding="utf-8-sig"))
    if (preparation.get("schema") != "gsw.update_end_to_end_preparation/1" or
            preparation.get("testOnly") is not True or preparation.get("formalGswAcceptance") is not False):
        raise ValueError("isolated_preparation_required")
    work_root = Path(preparation["root"]).absolute()
    original = Path(preparation["installationRoot"]).absolute()
    if original != work_root / "installation":
        raise ValueError("fixture_installation_path_invalid")
    helper_name = "GSW Update Fixture Helper.exe"
    if not (original / "bin" / helper_name).is_file():
        raise ValueError("previous_fixture_helper_deployment_required")
    evidence = args.evidence.absolute()
    evidence.mkdir(parents=True, exist_ok=False)
    root = work_root / "recovery-trusted-chooser-installation"
    root.mkdir(exist_ok=False)
    shutil.copytree(original / "bin", root / "bin")
    shutil.copytree(original / "user-data", root / "user-data")
    shutil.copyfile(original / "bootstrap.json", root / "bootstrap.json")
    setup = support.EndToEnd("test_health_confirmation_is_bound_to_actual_window_surface_build_exe_and_pid")
    setup.preparation, setup.installation = preparation, root
    setup.fixture = json.loads(Path(preparation["fixture"]).read_text(encoding="utf-8"))
    setup.evidence, setup.helper = evidence, root / "bin" / helper_name
    setup.trust_cli, setup.steps = args.trust_cli.absolute(), []
    setup.select("A")
    setup.helper_request("chooser-install-A", "--install", "stable")
    setup.select("B")
    setup.helper_request("chooser-install-B", "--install", "stable")
    setup.active("fixture-b")
    setup.select("revoked-b")
    withdrawal = setup.helper_request("chooser-accept-withdrawal-B", "--check", "stable", success=False)
    setup.assertEqual(withdrawal[-1]["code"], "BUILD_REVOKED")
    accepted = setup.accepted()
    setup.assertIn("fixture-b", accepted["revokedBuildIds"])
    setup.assertIn("fixture-a", accepted["accepted"])
    unaccepted = root / "versions/fixture-unaccepted"
    unaccepted.mkdir()
    (unaccepted / "owned-not-trusted.txt").write_bytes(b"directory presence is not update authority")
    (root / "current.json").write_bytes(b"owned invalid current for trusted Recovery chooser")
    before = support.tree_hashes(root)
    support.write_json(evidence / "preparation.json", {"schema": "gsw.recovery_chooser_preparation/1",
        "testOnly": True, "formalGswAcceptance": False, "root": str(root),
        "acceptedBuildIds": sorted(accepted["accepted"]), "revokedBuildIds": accepted["revokedBuildIds"],
        "expectedSelectableBuildIds": ["fixture-a"], "installationDigests": before,
        "sourceCommit": preparation["sourceCommit"], "sourceTree": preparation["sourceTree"]})
    qt_test = args.qt_test.absolute()
    deployed = subprocess.run([str(args.windeployqt), "--release", "--compiler-runtime", "--no-translations",
        "--no-opengl-sw", "--no-system-d3d-compiler", str(qt_test)], capture_output=True, timeout=180,
        creationflags=subprocess.CREATE_NO_WINDOW)
    if deployed.returncode:
        raise RuntimeError("private_qttest_deployment_failed")
    environment = os.environ.copy()
    environment["GSW_RECOVERY_QT_FIXTURE"] = "1"
    environment["GSW_RECOVERY_TEST_INSTALLATION"] = str(root)
    environment["GSW_RECOVERY_TEST_EVIDENCE"] = str(evidence)
    environment["GSW_UPDATE_FIXTURE_MARKER"] = str(evidence / "unexpected-application.json")
    environment["QT_QPA_PLATFORM"] = "windows"
    environment["QT_FORCE_STDERR_LOGGING"] = "1"
    for key in ("GSW_UPDATE_FIXTURE_HEALTH_MODE", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH"):
        environment.pop(key, None)
    result = subprocess.run([str(qt_test), "-o", "-,txt"], env=environment,
        capture_output=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
    (evidence / "recovery-chooser.log").write_bytes(result.stdout + b"\n" + result.stderr)
    support.write_json(evidence / "completion.json", {"schema": "gsw.recovery_chooser_completion/1",
        "testOnly": True, "formalGswAcceptance": False, "exitCode": result.returncode,
        "beforeDigests": before, "afterDigests": support.tree_hashes(root),
        "sourceCommit": preparation["sourceCommit"], "sourceTree": preparation["sourceTree"]})
    print(result.stdout.decode("utf-8", errors="replace"))
    if result.stderr:
        print(result.stderr.decode("utf-8", errors="replace"))
    if result.returncode == 0:
        proof = json.loads((evidence / "recovery-chooser-result.json").read_text(encoding="utf-8"))
        if (proof.get("passed") is not True or proof.get("actualHelper") is not True or
                proof.get("postListingTamperRejected") is not True or
                proof.get("explicitRestoredBuild") != "fixture-a" or proof.get("automaticApplicationLaunch") is not False):
            raise RuntimeError("recovery_ui_evidence_incomplete")
    return 0 if result.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
