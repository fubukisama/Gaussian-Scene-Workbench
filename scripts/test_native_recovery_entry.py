"""Remote-only real native Recovery entry/cancel seam, never formal GSW health.

The existing signed synthetic installation is copied into an owned directory.
This test deliberately damages only its current.json, invokes the unchanged
stable launcher explicitly, and inspects/closes only the test-owned window.
No production root, runtime trust override, user project or training is used.
"""
from __future__ import annotations

import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest


support_path = Path(__file__).with_name("test_native_update_end_to_end.py")
support_spec = importlib.util.spec_from_file_location("gsw_owned_recovery_support", support_path)
support = importlib.util.module_from_spec(support_spec)
support_spec.loader.exec_module(support)


class RecoveryEntry(support.EndToEnd):
    def test_independent_recovery_opens_with_invalid_current_and_cancel_preserves_every_file(self):
        original_root, original_helper = self.installation, self.helper
        root = Path(self.preparation["root"]) / "recovery-invalid-current-installation"
        root.mkdir()
        shutil.copytree(original_root / "bin", root / "bin")
        shutil.copytree(original_root / "user-data", root / "user-data")
        shutil.copyfile(original_root / "bootstrap.json", root / "bootstrap.json")
        self.installation, self.helper = root, root / "bin" / original_helper.name
        process, recovery_handle = None, None
        owned_windows = []
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
        user32.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
        user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
        user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
        user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
        user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
        kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                                      ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        expected_image = root / "bin/GSW Update Fixture Recovery.exe"
        titles = {"版本恢复", "Version recovery", "バージョンの復元"}
        try:
            self.select("A")
            self.helper_request("recovery-entry-install-A", "--install", "stable")
            self.select("B")
            self.helper_request("recovery-entry-install-B", "--install", "stable")
            self.active("fixture-b")
            pointer = root / "current.json"
            malformed = b"owned malformed pointer: recovery must not need to parse this"
            pointer.write_bytes(malformed)
            before = support.tree_hashes(root)
            launcher = root / "bin/Gaussian Scene Workbench Launcher.exe"
            environment = os.environ.copy()
            # Any accidental application launch would write this marker and
            # fail the explicit-recovery contract, rather than count as health.
            unexpected_app = self.evidence / "recovery-entry-unexpected-application.json"
            environment["GSW_UPDATE_FIXTURE_MARKER"] = str(unexpected_app)
            environment.pop("GSW_UPDATE_FIXTURE_HEALTH_MODE", None)
            process = subprocess.Popen([str(launcher), "--recover"], env=environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 15
            found = None
            legacy_error = None
            while time.monotonic() < deadline and found is None and legacy_error is None:
                @callback_type
                def inspect(window, _):
                    nonlocal found, legacy_error, recovery_handle
                    if not user32.IsWindowVisible(window):
                        return 1
                    pid = ctypes.c_ulong()
                    user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
                    klass, title = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(512)
                    user32.GetClassNameW(window, klass, len(klass))
                    user32.GetWindowTextW(window, title, len(title))
                    if pid.value == process.pid:
                        row = {"pid": pid.value, "class": klass.value, "title": title.value,
                               "independentRecovery": False}
                        owned_windows.append(row)
                        if klass.value == "#32770":
                            legacy_error = row
                            user32.PostMessageW(window, 0x0111, 1, 0)  # this owned dialog's OK
                        return 1
                    if title.value not in titles or not klass.value.startswith("Qt"):
                        return 1
                    handle = kernel.OpenProcess(0x00101000, False, pid.value)
                    if not handle:
                        return 1
                    image, length = ctypes.create_unicode_buffer(32768), ctypes.c_ulong(32768)
                    if (not kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(length)) or
                            Path(image.value).resolve() != expected_image.resolve()):
                        kernel.CloseHandle(handle)
                        return 1
                    found = {"pid": pid.value, "class": klass.value, "title": title.value,
                             "image": image.value, "independentRecovery": True}
                    owned_windows.append(found)
                    recovery_handle = handle
                    # Explicit user-style cancel. Never focus/raise the window,
                    # terminate a process, reset trust or start an application.
                    user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE
                    return 0

                user32.EnumWindows(inspect, 0)
                if found is None and legacy_error is None:
                    time.sleep(.05)
            support.write_json(self.evidence / "recovery-entry-window.json", {
                "testOnly": True, "formalGswAcceptance": False, "windows": owned_windows,
                "independentRecoveryFound": found is not None,
                "sourceCommit": self.preparation["sourceCommit"], "sourceTree": self.preparation["sourceTree"]})
            self.assertIsNotNone(found, "independent recovery window missing; a launcher error box is not recovery")
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(kernel.WaitForSingleObject(recovery_handle, 5000), 0)
            exit_code = ctypes.c_ulong()
            self.assertTrue(kernel.GetExitCodeProcess(recovery_handle, ctypes.byref(exit_code)))
            self.assertEqual(exit_code.value, 0)
            self.assertFalse(unexpected_app.exists())
            self.assertEqual(pointer.read_bytes(), malformed)
            self.assertEqual(support.tree_hashes(root), before)
            support.write_json(self.evidence / "recovery-entry-result.json", {
                "passed": True, "testOnly": True, "formalGswAcceptance": False,
                "invalidCurrentIndependentEntry": True, "explicitCancel": True,
                "allInstallationFilesUnchanged": True, "automaticApplicationLaunch": False,
                "sourceCommit": self.preparation["sourceCommit"], "sourceTree": self.preparation["sourceTree"]})
        finally:
            if recovery_handle:
                kernel.CloseHandle(recovery_handle)
            if process is not None and process.poll() is None:
                # Only close this launcher's own dialog, never another app or
                # an unknown process. A stuck process is retained as failure.
                @callback_type
                def close_launcher(window, _):
                    pid = ctypes.c_ulong()
                    user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
                    if pid.value == process.pid:
                        user32.PostMessageW(window, 0x0010, 0, 0)
                    return 1
                user32.EnumWindows(close_launcher, 0)
            self.installation, self.helper = original_root, original_helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true")
    parser.add_argument("--preparation", type=Path, required=True)
    parser.add_argument("--trust-cli", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or not args.remote_fixture_only:
        raise ValueError("explicit_remote_windows_fixture_required")
    preparation = json.loads(args.preparation.read_text(encoding="utf-8-sig"))
    if (preparation.get("schema") != "gsw.update_end_to_end_preparation/1" or
            preparation.get("testOnly") is not True or preparation.get("formalGswAcceptance") is not False):
        raise ValueError("isolated_preparation_required")
    root = Path(preparation["root"]).absolute()
    installation = Path(preparation["installationRoot"]).absolute()
    if installation != root / "installation":
        raise ValueError("fixture_installation_path_invalid")
    helper = installation / "bin/GSW Update Fixture Helper.exe"
    if not helper.is_file():
        raise ValueError("previous_fixture_helper_deployment_required")
    args.evidence.mkdir(parents=True, exist_ok=True)
    RecoveryEntry.preparation, RecoveryEntry.installation = preparation, installation
    RecoveryEntry.fixture = json.loads(Path(preparation["fixture"]).read_text(encoding="utf-8"))
    RecoveryEntry.evidence, RecoveryEntry.helper = args.evidence.absolute(), helper
    RecoveryEntry.trust_cli, RecoveryEntry.steps = args.trust_cli.absolute(), []
    # Deliberately run just this new public seam, not inherited lifecycle tests.
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([
        RecoveryEntry("test_independent_recovery_opens_with_invalid_current_and_cancel_preserves_every_file")]))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
