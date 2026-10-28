"""Remote real-process A/B update seam, NOT formal GSW startup acceptance.

Uses the unchanged stable native launcher source, distinct test-only helper
trust, the real HTTPS issuer and actual deployed Qt windows. Never starts an
installed GSW, reads a user's project, supplies runtime trust overrides or
obtains production signing credentials.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest
import uuid


def digest(path: Path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tree_hashes(root: Path):
    return {path.relative_to(root).as_posix(): digest(path)
            for path in root.rglob("*") if path.is_file()}


def write_json(path: Path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=True, sort_keys=True)
        stream.write("\n")


class EndToEnd(unittest.TestCase):
    preparation: dict
    fixture: dict
    installation: Path
    evidence: Path
    helper: Path
    trust_cli: Path
    steps: list

    def select(self, repository):
        control = Path(self.preparation["controlFile"])
        temporary = control.with_name(control.name + ".new-" + uuid.uuid4().hex)
        write_json(temporary, {"repository": repository})
        os.replace(temporary, control)

    def helper_request(self, name, *arguments, success=True):
        result = subprocess.run([str(self.helper), *arguments], capture_output=True,
            timeout=100, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertLessEqual(len(result.stdout), 4 * 1024 * 1024)
        # The public event boundary must not expose archive URLs/query secrets.
        self.assertNotIn(b"://", result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        self.assertTrue(records)
        self.assertTrue(all(row.get("schema") == "gsw.update_event/1" for row in records))
        write_json(self.evidence / (name + ".json"), {"exitCode": result.returncode, "events": records,
            "stderrPresent": bool(result.stderr), "testOnly": True, "formalGswAcceptance": False})
        self.steps.append({"step": name, "exitCode": result.returncode,
                           "phase": records[-1].get("phase"), "code": records[-1].get("code")})
        self.assertEqual(result.returncode, 0 if success else 2, records[-1])
        return records

    def active(self, build_id):
        current = json.loads((self.installation / "current.json").read_text(encoding="utf-8"))
        self.assertEqual(set(current), {"schema", "version", "executableSha256"})
        self.assertEqual(current["schema"], "gsw.native_active_version/1")
        self.assertEqual(current["version"], build_id)
        exe = self.installation / "versions" / build_id / "bin/Gaussian Scene Workbench.exe"
        self.assertEqual(current["executableSha256"], digest(exe))
        return (self.installation / "current.json").read_bytes()

    def launch(self, label):
        launcher = Path(self.preparation["launcher"])
        plan = subprocess.run([str(launcher), "--print-launch-plan"], capture_output=True,
            timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(plan.returncode, 0)
        planned = json.loads(plan.stdout)
        self.assertTrue(planned["valid"])
        build = "fixture-" + label.lower()
        self.assertEqual(planned["version"], build)
        marker = self.evidence / ("launch-" + label + "-" + uuid.uuid4().hex + ".json")
        environment = os.environ.copy()
        environment["GSW_UPDATE_FIXTURE_MARKER"] = str(marker)
        environment["QT_QPA_PLATFORM"] = "windows"
        for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH"):
            environment.pop(key, None)
        process = subprocess.Popen([str(launcher)], env=environment, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.assertEqual(process.wait(timeout=15), 0)
            deadline = time.monotonic() + 25
            record = {}
            while time.monotonic() < deadline:
                if marker.exists():
                    record = json.loads(marker.read_text(encoding="utf-8"))
                    if record.get("phase") == "normal_exit":
                        break
                time.sleep(.05)
            self.assertEqual(record.get("schema"), "gsw.update_application_fixture/1")
            self.assertIs(record.get("testOnly"), True)
            self.assertIs(record.get("formalGswAcceptance"), False)
            self.assertEqual(record.get("phase"), "normal_exit")
            self.assertEqual(record.get("label"), label)
            self.assertEqual(record.get("buildId"), build)
            self.assertEqual(record.get("exitCode"), 0)
            self.assertIs(record.get("windowShown"), True)
            self.assertEqual(record.get("platform"), "windows")
            expected = self.installation / "versions" / build / "bin/Gaussian Scene Workbench.exe"
            self.assertEqual(Path(record["executable"]).resolve(), expected.resolve())
            self.assertEqual(Path(record["installationRoot"]).resolve(), self.installation.resolve())
            # Wait for the actual detached fixture PID, not just the launcher.
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x00100000, False, int(record["pid"]))
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0)
                finally:
                    kernel.CloseHandle(handle)
            self.steps.append({"step": "launch-" + label, "buildId": build, "realQtWindow": True,
                               "formalGswAcceptance": False})
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    def accepted(self):
        request = {"schema": 1, "operation": "inspect_accepted",
                   "installation_root": str(self.installation), "channel": "stable"}
        result = subprocess.run([str(self.trust_cli), "-"], input=json.dumps(request).encode(),
            capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode, 0)
        response = json.loads(result.stdout)
        self.assertTrue(response["ok"])
        return response["acceptedState"]

    def test_real_https_A_B_badC_explicit_A_through_the_same_stable_launcher(self):
        bootstrap = tree_hashes(self.installation / "bin")
        bootstrap_json = (self.installation / "bootstrap.json").read_bytes()
        user_data = tree_hashes(self.installation / "user-data")
        self.select("A")
        self.helper_request("check-A", "--check", "stable")
        self.assertFalse((self.installation / "current.json").exists())
        self.helper_request("install-A", "--install", "stable")
        pointer_a = self.active("fixture-a")
        a_files = tree_hashes(self.installation / "versions/fixture-a")
        self.launch("A")
        self.select("B")
        self.helper_request("install-B", "--install", "stable")
        pointer_b = self.active("fixture-b")
        self.assertNotEqual(pointer_a, pointer_b)
        b_files = tree_hashes(self.installation / "versions/fixture-b")
        self.launch("B")
        self.select("badC")
        failure = self.helper_request("reject-badC", "--install", "stable", success=False)
        self.assertEqual(failure[-1].get("code"), "download_hash_mismatch")
        self.assertEqual(self.active("fixture-b"), pointer_b)
        self.assertFalse((self.installation / "versions/fixture-badc").exists())
        accepted_before = self.accepted()
        self.assertEqual(accepted_before["highestByChannel"]["stable"]["releaseSequence"], 3)
        self.helper_request("rollback-A", "--rollback", "stable", "fixture-a")
        self.assertEqual(self.active("fixture-a"), pointer_a)
        self.launch("A")
        self.assertEqual(self.accepted(), accepted_before)
        self.assertEqual(tree_hashes(self.installation / "versions/fixture-a"), a_files)
        self.assertEqual(tree_hashes(self.installation / "versions/fixture-b"), b_files)
        self.assertEqual(tree_hashes(self.installation / "bin"), bootstrap)
        self.assertEqual((self.installation / "bootstrap.json").read_bytes(), bootstrap_json)
        self.assertEqual(tree_hashes(self.installation / "user-data"), user_data)
        # A local receipt is completion evidence, not publisher authority. Check
        # the accepted-descriptor binding and exact installed bytes separately.
        receipt = self.installation / "updates/receipts/fixture-a.json"
        original_receipt = receipt.read_bytes()
        for name, patch, code in (
                ("receipt-descriptor", {"descriptorSha256": "0" * 64}, "rollback_receipt_not_accepted"),
                ("developer-receipt", {"mode": "developer"}, "rollback_receipt_invalid")):
            try:
                changed = json.loads(original_receipt)
                changed.update(patch)
                receipt.write_text(json.dumps(changed), encoding="utf-8")
                rejected = self.helper_request(name, "--rollback", "stable", "fixture-a", success=False)
                self.assertEqual(rejected[-1].get("code"), code)
                self.assertEqual(self.active("fixture-a"), pointer_a)
                self.assertEqual(self.accepted(), accepted_before)
            finally:
                receipt.write_bytes(original_receipt)
        extra = self.installation / "versions/fixture-a/unrecorded-owned-fixture.txt"
        try:
            extra.write_bytes(b"not in the accepted payload")
            rejected = self.helper_request("extra-slot-file", "--rollback", "stable", "fixture-a", success=False)
            self.assertEqual(rejected[-1].get("code"), "activation_slot_file_set_differs")
            self.assertEqual(self.active("fixture-a"), pointer_a)
            self.assertEqual(self.accepted(), accepted_before)
        finally:
            extra.unlink()
        source_file = self.installation / "versions/fixture-a/source/native/CMakeLists.txt"
        original_source = source_file.read_bytes()
        try:
            source_file.write_bytes(original_source + b"\n# tampered owned fixture\n")
            rejected = self.helper_request("changed-slot-file", "--rollback", "stable", "fixture-a", success=False)
            self.assertEqual(rejected[-1].get("code"), "activation_slot_digest_mismatch")
            self.assertEqual(self.active("fixture-a"), pointer_a)
            self.assertEqual(self.accepted(), accepted_before)
        finally:
            source_file.write_bytes(original_source)
        # Explicit recovery does not trust or require parsing a damaged pointer.
        (self.installation / "current.json").write_bytes(b"owned malformed pointer")
        self.helper_request("recover-malformed-pointer", "--rollback", "stable", "fixture-a")
        self.assertEqual(self.active("fixture-a"), pointer_a)
        self.assertEqual(self.accepted(), accepted_before)
        self.assertEqual(tree_hashes(self.installation / "versions/fixture-a"), a_files)
        self.assertEqual(tree_hashes(self.installation / "versions/fixture-b"), b_files)
        self.assertEqual(tree_hashes(self.installation / "bin"), bootstrap)
        self.assertEqual(tree_hashes(self.installation / "user-data"), user_data)
        write_json(self.evidence / "result.json", {"schema": "gsw.update_end_to_end_evidence/1", "passed": True,
            "testOnly": True, "production": False, "formalGswAcceptance": False,
            "scope": "real-qt-fixture-stable-launcher-only", "steps": self.steps,
            "trustHighWaterAfterRollback": 3, "bootstrapUnchanged": True, "userSentinelsUnchanged": True,
            "sourceCommit": self.preparation["sourceCommit"], "sourceTree": self.preparation["sourceTree"]})

    def test_revoked_installed_build_is_refused_without_changing_trust_or_pointer(self):
        original_root, original_helper = self.installation, self.helper
        root = Path(self.preparation["root"]) / "revocation-installation"
        root.mkdir()
        shutil.copytree(original_root / "bin", root / "bin")
        shutil.copyfile(original_root / "bootstrap.json", root / "bootstrap.json")
        self.installation = root
        self.helper = root / "bin" / original_helper.name
        try:
            self.select("A")
            self.helper_request("revocation-install-A", "--install", "stable")
            self.select("B")
            self.helper_request("revocation-install-B", "--install", "stable")
            pointer = self.active("fixture-b")
            slots = tree_hashes(root / "versions")
            bootstrap = tree_hashes(root / "bin")
            self.select("revoked-a")
            self.helper_request("adopt-revocation", "--check", "stable")
            accepted = self.accepted()
            self.assertIn("fixture-a", accepted["revokedBuildIds"])
            rejected = self.helper_request("refuse-revoked-A", "--rollback", "stable", "fixture-a", success=False)
            self.assertEqual(rejected[-1].get("code"), "BUILD_REVOKED")
            self.assertEqual(self.active("fixture-b"), pointer)
            self.assertEqual(self.accepted(), accepted)
            self.assertEqual(tree_hashes(root / "versions"), slots)
            self.assertEqual(tree_hashes(root / "bin"), bootstrap)
            write_json(self.evidence / "revocation-result.json", {"passed": True, "testOnly": True,
                "formalGswAcceptance": False, "revokedBuildRefused": True,
                "sourceCommit": self.preparation["sourceCommit"]})
        finally:
            self.installation, self.helper = original_root, original_helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true")
    parser.add_argument("--preparation", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--trust-cli", type=Path, required=True)
    parser.add_argument("--windeployqt", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or not args.remote_fixture_only:
        raise ValueError("explicit_remote_windows_fixture_required")
    preparation = json.loads(args.preparation.read_text(encoding="utf-8-sig"))
    if (preparation.get("schema") != "gsw.update_end_to_end_preparation/1" or
            preparation.get("testOnly") is not True or preparation.get("formalGswAcceptance") is not False):
        raise ValueError("isolated_preparation_required")
    installation = Path(preparation["installationRoot"]).absolute()
    if installation != Path(preparation["root"]).absolute() / "installation":
        raise ValueError("fixture_installation_path_invalid")
    args.evidence.mkdir(parents=True, exist_ok=True)
    helper = installation / "bin/GSW Update Fixture Helper.exe"
    if helper.exists():
        raise ValueError("fixture_helper_already_exists")
    shutil.copyfile(args.helper, helper)
    deployed = subprocess.run([str(args.windeployqt), "--release", "--compiler-runtime", "--no-translations",
        "--no-opengl-sw", "--no-system-d3d-compiler", "--dir", str(helper.parent), str(helper)],
        capture_output=True, timeout=180)
    if deployed.returncode:
        raise ValueError("fixture_helper_deployment_failed")
    EndToEnd.preparation, EndToEnd.installation = preparation, installation
    EndToEnd.fixture = json.loads(Path(preparation["fixture"]).read_text(encoding="utf-8"))
    EndToEnd.evidence, EndToEnd.helper = args.evidence.absolute(), helper
    EndToEnd.trust_cli, EndToEnd.steps = args.trust_cli.absolute(), []
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(EndToEnd)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        write_json(EndToEnd.evidence / "failure-summary.json", {"testOnly": True, "formalGswAcceptance": False,
            "steps": EndToEnd.steps, "sourceCommit": preparation["sourceCommit"], "sourceTree": preparation["sourceTree"]})
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
