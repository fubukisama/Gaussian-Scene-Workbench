"""Remote-only signed rollback refusal/retry under real Windows pointer guards.

Uses an actual compiled Fixture Helper/TUF history, never unsigned admission.
The first case preserves a non-revoked B and independently verifies its slot;
the second retains an already-known withdrawal of B and explicitly restores A.
Neither case starts an application, renders GSW, or tests GPU optimizer resume.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import unittest


PRODUCT = "Fubukisama.GaussianSceneWorkbench.UpdateFixture"
HELPER_NAME = "GSW Update Fixture Helper.exe"
INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_ATTRIBUTE_READONLY = 0x1
GENERIC_READ = 0x80000000
DELETE_ACCESS = 0x10000
FILE_SHARE_READ = 0x1
OPEN_EXISTING = 3


def require_remote():
    if (os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or
            os.environ.get("GITHUB_REPOSITORY_ID") != "1286130219" or
            os.environ.get("GITHUB_REPOSITORY_OWNER_ID") != "237571720" or
            os.environ.get("GITHUB_REF") != "refs/heads/main"):
        raise ValueError("explicit_remote_main_windows_fixture_required")


def ordinary_d_path(path):
    if not path.is_absolute() or path.drive.casefold() != "d:" or ".." in path.parts:
        raise ValueError("fresh_owned_d_drive_fixture_required")
    for item in (path, *path.parents):
        if item.exists():
            attributes = getattr(item.lstat(), "st_file_attributes", 0)
            if item.is_symlink() or attributes & 0x400:
                raise ValueError("rollback_resource_reparse_path_forbidden")


def kernel_api():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
    kernel.GetFileAttributesW.restype = ctypes.c_uint32
    kernel.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    kernel.SetFileAttributesW.restype = ctypes.c_int
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    return kernel


@contextmanager
def read_only_pointer(path):
    kernel = kernel_api()
    previous = kernel.GetFileAttributesW(str(path))
    if previous == INVALID_FILE_ATTRIBUTES:
        raise ctypes.WinError(ctypes.get_last_error())
    read_only = (previous & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_READONLY
    if not kernel.SetFileAttributesW(str(path), read_only):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        actual = kernel.GetFileAttributesW(str(path))
        if actual == INVALID_FILE_ATTRIBUTES or not actual & FILE_ATTRIBUTE_READONLY:
            raise AssertionError("actual_owned_pointer_readonly_attribute_required")
        yield {"kind": "FILE_ATTRIBUTE_READONLY", "actualAttributeApplied": True}
    finally:
        # Only the exact owned fixture pointer is restored, never a directory
        # ACL, user installation, system policy or unrelated file attribute.
        if not kernel.SetFileAttributesW(str(path), previous):
            raise ctypes.WinError(ctypes.get_last_error())


@contextmanager
def pointer_without_delete_sharing(path):
    kernel = kernel_api()
    invalid = ctypes.c_void_p(-1).value
    handle = kernel.CreateFileW(str(path), GENERIC_READ, FILE_SHARE_READ,
        None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
    if handle == invalid:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        # Independently demonstrate this lease really denies DELETE access;
        # opening and closing a probe handle never deletes or changes bytes.
        ctypes.set_last_error(0)
        probe = kernel.CreateFileW(str(path), DELETE_ACCESS, 0x1 | 0x2 | 0x4,
            None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
        error = ctypes.get_last_error()
        if probe != invalid:
            kernel.CloseHandle(probe)
            raise AssertionError("owned_pointer_delete_lease_not_effective")
        if error != 32:  # ERROR_SHARING_VIOLATION, not a missing file/ACL guess.
            raise ctypes.WinError(error)
        yield {"kind": "read_handle_without_FILE_SHARE_DELETE",
               "deleteAccessProbeRejected": True, "probeOsError": error}
    finally:
        kernel.CloseHandle(handle)


def accepted_digest(value):
    raw = json.dumps(value, ensure_ascii=True, sort_keys=True,
                     separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def protected(root):
    result = {}
    for prefix in ("bin", "versions", "user-data", "updates/trust", "updates/receipts"):
        for path in (root / prefix).rglob("*"):
            if path.is_file():
                result[path.relative_to(root).as_posix()] = SUPPORT.digest(path)
    for relative in ("current.json", "bootstrap.json", ".update-trust-anchor"):
        path = root / relative
        if not path.is_file():
            raise AssertionError("owned_protected_file_missing:" + relative)
        result[relative] = SUPPORT.digest(path)
    return result


class RollbackResourceAcceptance(unittest.TestCase):
    def prepare(self, name, *, withdraw_b):
        preparation = json.loads(ARGS.preparation.read_text(encoding="utf-8-sig"))
        self.assertEqual(preparation.get("schema"), "gsw.update_end_to_end_preparation/1")
        self.assertIs(preparation.get("testOnly"), True)
        self.assertIs(preparation.get("production"), False)
        self.assertIs(preparation.get("formalGswAcceptance"), False)
        work_root = Path(preparation["root"]).absolute()
        original = Path(preparation["installationRoot"]).absolute()
        ordinary_d_path(work_root)
        self.assertEqual(original, work_root / "installation")
        fixture = json.loads(Path(preparation["fixture"]).read_text(encoding="utf-8"))
        self.assertEqual(fixture.get("product"), PRODUCT)
        self.assertIs(fixture.get("testOnly"), True)
        self.assertTrue((original / "bin" / HELPER_NAME).is_file(),
                        "run after original fixture Helper deployment")
        case_evidence = ARGS.evidence / name
        case_evidence.mkdir(exist_ok=False)
        root = work_root / ("rollback-resource-" + name)
        ordinary_d_path(root)
        root.mkdir(exist_ok=False)
        shutil.copytree(original / "bin", root / "bin")
        shutil.copytree(original / "user-data", root / "user-data")
        shutil.copyfile(original / "bootstrap.json", root / "bootstrap.json")
        helper = SUPPORT.EndToEnd(methodName="runTest")
        helper.preparation, helper.installation, helper.fixture = preparation, root, fixture
        helper.evidence, helper.helper = case_evidence, root / "bin" / HELPER_NAME
        helper.trust_cli, helper.steps = ARGS.trust_cli, []
        helper.select("A")
        helper.helper_request("install-A", "--install", "stable")
        pointer_a = helper.active("fixture-a")
        helper.select("B")
        helper.helper_request("install-B", "--install", "stable")
        pointer_b = helper.active("fixture-b")
        self.assertNotEqual(pointer_a, pointer_b)
        if withdraw_b:
            helper.select("revoked-b")
            withdrawn = helper.helper_request("accept-signed-withdrawal-B", "--check", "stable", success=False)
            self.assertEqual(withdrawn[-1].get("code"), "BUILD_REVOKED")
        accepted = helper.accepted()
        high_water = accepted["highestByChannel"]["stable"]
        self.assertEqual(high_water["releaseSequence"], 2)
        self.assertEqual(high_water["buildId"], "fixture-b")
        self.assertIn("fixture-a", accepted["accepted"])
        self.assertIn("fixture-b", accepted["accepted"])
        if withdraw_b:
            self.assertIn("fixture-b", accepted["revokedBuildIds"])
            rejected = helper.helper_request("known-withdrawn-B-refuses-verification", "--verify-installed",
                intent=helper.launch_intent(), success=False)
            self.assertEqual(rejected[-1].get("code"), "BUILD_REVOKED")
        else:
            self.assertNotIn("fixture-b", accepted["revokedBuildIds"])
        return preparation, root, helper, case_evidence, pointer_a, pointer_b, accepted

    def exercise(self, name, guard, *, withdraw_b):
        preparation, root, helper, evidence, pointer_a, pointer_b, accepted_before = self.prepare(
            name, withdraw_b=withdraw_b)
        before = protected(root)
        pointer = root / "current.json"
        marker = evidence / "unexpected-application.json"
        original_marker = os.environ.get("GSW_UPDATE_FIXTURE_MARKER")
        os.environ["GSW_UPDATE_FIXTURE_MARKER"] = str(marker)
        try:
            with guard(pointer) as guard_facts:
                refused = helper.helper_request("rollback-A-resource-refused", "--rollback", "stable",
                    "fixture-a", success=False)
                self.assertEqual(refused[-1].get("phase"), "failed")
                self.assertEqual(refused[-1].get("code"), "activation_atomic_write_failed")
                self.assertEqual(helper.active("fixture-b"), pointer_b)
                self.assertEqual(protected(root), before)
                self.assertEqual(helper.accepted(), accepted_before)
                if not withdraw_b:
                    # No launch occurs: this is the actual read-only offline
                    # complete-slot admission for the still-non-revoked B.
                    valid = helper.helper_request("preserved-B-independently-verified", "--verify-installed",
                        intent=helper.launch_intent())
                    self.assertEqual(valid[-1].get("phase"), "launch_verified")
                    self.assertEqual(valid[-1].get("code"), "VERIFIED")
                    self.assertEqual(valid[-1].get("buildId"), "fixture-b")
                    self.assertEqual(protected(root), before)
                    self.assertEqual(helper.accepted(), accepted_before)
                SUPPORT.write_json(evidence / "refusal.json", {
                    "schema": "gsw.rollback_resource_refusal/1", "testOnly": True,
                    "formalGswAcceptance": False, "guard": guard_facts,
                    "failureCode": refused[-1]["code"], "protectedDigests": protected(root),
                    "acceptedStateSha256": accepted_digest(accepted_before),
                    "currentBuild": "fixture-b", "currentKnownRevoked": withdraw_b,
                    "currentIndependentlyVerified": not withdraw_b,
                    "applicationLaunchRequested": False})
            restored = helper.helper_request("explicit-rollback-A-after-release", "--rollback", "stable", "fixture-a")
            self.assertEqual(restored[-1].get("phase"), "rolled_back")
            self.assertEqual(restored[-1].get("code"), "NEXT_LAUNCH")
            self.assertEqual(helper.active("fixture-a"), pointer_a)
            after = protected(root)
            self.assertEqual({key: value for key, value in after.items() if key != "current.json"},
                             {key: value for key, value in before.items() if key != "current.json"})
            self.assertEqual(helper.accepted(), accepted_before)
            self.assertFalse(marker.exists(), "rollback must not launch the fixture application")
            SUPPORT.write_json(evidence / "result.json", {
                "schema": "gsw.rollback_resource_case_acceptance/1", "passed": True,
                "testOnly": True, "production": False, "formalGswAcceptance": False,
                "scope": "actual-signed-history-pointer-io-refusal-and-explicit-retry-only",
                "guard": guard_facts, "currentBefore": "fixture-b", "explicitRestoredBuild": "fixture-a",
                "knownWithdrawnBRetained": withdraw_b,
                "preservedNonRevokedBIndependentlyVerified": not withdraw_b,
                "acceptedStateSha256Before": accepted_digest(accepted_before),
                "acceptedStateSha256After": accepted_digest(helper.accepted()),
                "highWaterAfter": accepted_before["highestByChannel"]["stable"],
                "revokedBuildIdsAfter": accepted_before["revokedBuildIds"],
                "protectedBefore": before, "protectedAfter": after,
                "applicationLaunchRequested": False, "unexpectedFixtureMarkerObserved": False,
                "rendererAcceptance": False, "optimizerResume3DGS": "NOT_RUN",
                "optimizerResume2DGS": "NOT_RUN", "sourceCommit": preparation["sourceCommit"],
                "sourceTree": preparation["sourceTree"], "steps": helper.steps})
        finally:
            if original_marker is None:
                os.environ.pop("GSW_UPDATE_FIXTURE_MARKER", None)
            else:
                os.environ["GSW_UPDATE_FIXTURE_MARKER"] = original_marker

    def test_readonly_current_preserves_verified_non_revoked_B_until_explicit_retry(self):
        self.exercise("readonly-current", read_only_pointer, withdraw_b=False)

    def test_occupied_current_retains_known_withdrawal_then_explicitly_restores_A(self):
        self.exercise("occupied-current-with-withdrawal", pointer_without_delete_sharing, withdraw_b=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true")
    parser.add_argument("--preparation", type=Path, required=True)
    parser.add_argument("--trust-cli", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    ARGS = parser.parse_args()
    require_remote()
    if not ARGS.remote_fixture_only:
        parser.error("explicit_remote_windows_fixture_required")
    ARGS.preparation = ARGS.preparation.absolute()
    ARGS.trust_cli = ARGS.trust_cli.absolute()
    ARGS.evidence = ARGS.evidence.absolute()
    ordinary_d_path(ARGS.evidence)
    ARGS.evidence.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location("gsw_rollback_resource_e2e_support",
        Path(__file__).with_name("test_native_update_end_to_end.py"))
    SUPPORT = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(SUPPORT)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RollbackResourceAcceptance)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    SUPPORT.write_json(ARGS.evidence / "completion.json", {
        "schema": "gsw.rollback_resource_acceptance/1", "testOnly": True,
        "production": False, "formalGswAcceptance": False,
        "passed": result.wasSuccessful(), "testsRun": result.testsRun,
        "failureCount": len(result.failures), "errorCount": len(result.errors),
        "applicationLaunchRequested": False, "scope": "pointer-attribute-and-sharing-failures-only"})
    raise SystemExit(0 if result.wasSuccessful() else 1)
