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

    def helper_request(self, name, *arguments, success=True, intent=None, raw_intent=None):
        payload = raw_intent if raw_intent is not None else (
            json.dumps(intent, ensure_ascii=True).encode() if intent is not None else None)
        result = subprocess.run([str(self.helper), *arguments], input=payload, capture_output=True,
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

    def launch_intent(self):
        current = json.loads((self.installation / "current.json").read_bytes())
        return {"schema": "gsw.native_launch_intent/1", "buildId": current["version"],
                "executableSha256": current["executableSha256"], "arguments": [], "diagnostics": False}

    def launch(self, label, arguments=(), diagnostics=False):
        launcher = self.installation / "bin/Gaussian Scene Workbench Launcher.exe"
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
        environment["GSW_RENDER_DIAGNOSTICS_DIR"] = "unrequested-owned-fixture-diagnostics"
        for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH"):
            environment.pop(key, None)
        launch_arguments = (["--diagnostics"] if diagnostics else []) + list(arguments)
        process = subprocess.Popen([str(launcher), *launch_arguments], env=environment, stdout=subprocess.DEVNULL,
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
            self.assertEqual(record.get("arguments"), list(arguments))
            self.assertEqual(record.get("diagnosticsRoot"),
                str(self.installation / "diagnostics").replace("\\", "/") if diagnostics else "")
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
            return record
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    def refused_launch(self, label):
        """Click only this test-owned launcher's error box, never another app.

        The actual ordinary entry is exercised before the diagnostic plan. A
        vulnerable launcher exits 0 and starts the withdrawn fixture, producing
        an unambiguous RED rather than a private implementation assertion.
        """
        launcher = self.installation / "bin/Gaussian Scene Workbench Launcher.exe"
        identity = uuid.uuid4().hex
        marker = self.evidence / ("refused-launch-" + label + "-" + identity + ".json")
        window_evidence = self.evidence / ("rejection-window-" + label + "-" + identity + ".json")
        environment = os.environ.copy()
        environment["GSW_UPDATE_FIXTURE_MARKER"] = str(marker)
        environment["QT_QPA_PLATFORM"] = "windows"
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        # Win32 WNDENUMPROC returns BOOL (32-bit), not C/C++ bool (8-bit).
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
        user32.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
        user32.EnumWindows.restype = ctypes.c_int
        user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        user32.GetWindowThreadProcessId.restype = ctypes.c_ulong
        user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
        user32.GetClassNameW.restype = ctypes.c_int
        user32.GetDlgItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
        user32.GetDlgItem.restype = ctypes.c_void_p
        user32.GetParent.argtypes = [ctypes.c_void_p]
        user32.GetParent.restype = ctypes.c_void_p
        user32.IsChild.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        user32.IsChild.restype = ctypes.c_int
        user32.GetDlgCtrlID.argtypes = [ctypes.c_void_p]
        user32.GetDlgCtrlID.restype = ctypes.c_int
        user32.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
        user32.EnumChildWindows.restype = ctypes.c_int
        user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
        user32.SetForegroundWindow.restype = ctypes.c_int
        user32.GetForegroundWindow.argtypes = []
        user32.GetForegroundWindow.restype = ctypes.c_void_p
        user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
        user32.PostMessageW.restype = ctypes.c_int
        process = subprocess.Popen([str(launcher)], env=environment, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        observations = []
        enumeration_count, enumeration_failures = 0, 0
        started = time.monotonic()
        try:
            @callback_type
            def close_owned_dialog(window, _context):
                owner = ctypes.c_ulong()
                user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
                if owner.value != process.pid:
                    return 1
                name = ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(window, name, len(name))
                record = {"class": name.value, "controlFound": False, "postSucceeded": False}
                if name.value == "#32770":
                    button = user32.GetDlgItem(window, 1)  # IDOK, not window text.
                    button_owner = ctypes.c_ulong()
                    button_class = ctypes.create_unicode_buffer(256)
                    owner_match, direct_parent, descendant, control_id = False, False, False, None
                    if button:
                        user32.GetWindowThreadProcessId(button, ctypes.byref(button_owner))
                        owner_match = button_owner.value == process.pid
                        if owner_match:
                            direct_parent = user32.GetParent(button) == window
                            descendant = bool(user32.IsChild(window, button))
                            user32.GetClassNameW(button, button_class, len(button_class))
                            control_id = user32.GetDlgCtrlID(button)
                    owned_button = (bool(button) and owner_match and direct_parent and descendant and
                        control_id == 1 and button_class.value.casefold() == "button")
                    record.update(getDlgItemPresent=bool(button), ownerMatch=owner_match,
                        directParent=direct_parent, isChild=descendant, controlId=control_id,
                        controlFound=bool(owned_button), controlClass=button_class.value)
                    if len(observations) < 20:
                        children = []
                        @callback_type
                        def inspect_owned_child(child, _parameter):
                            child_owner = ctypes.c_ulong()
                            user32.GetWindowThreadProcessId(child, ctypes.byref(child_owner))
                            if child_owner.value == process.pid and user32.IsChild(window, child):
                                child_class = ctypes.create_unicode_buffer(256)
                                user32.GetClassNameW(child, child_class, len(child_class))
                                children.append({"class": child_class.value,
                                    "controlId": user32.GetDlgCtrlID(child),
                                    "directParent": user32.GetParent(child) == window})
                            return int(len(children) < 8)
                        user32.EnumChildWindows(window, inspect_owned_child, 0)
                        record.update(childObservationLimit=8, children=children)
                    if owned_button:
                        # Only this verified synthetic dialog may receive a
                        # foreground request. A denied request remains evidence;
                        # never attach input queues or synthesize global input.
                        foreground_requested = bool(user32.SetForegroundWindow(window))
                        record.update(foregroundRequestSucceeded=foreground_requested,
                            dialogIsForeground=user32.GetForegroundWindow() == window)
                        ctypes.set_last_error(0)
                        # Ask the actual same-PID IDOK Button to click itself.
                        # MessageBox may ignore a fabricated WM_COMMAND with no
                        # sending control handle; never close any other window.
                        posted = bool(user32.PostMessageW(button, 0x00F5, 0, 0))  # BM_CLICK
                        record.update(postSucceeded=posted,
                            postError=ctypes.get_last_error() if not posted else 0)
                    # Dismissing an error notification does not require a
                    # particular button hierarchy. Request the dialog's own
                    # WindowProc to close; never force a passing process exit.
                    # The launcher must still genuinely exit 2 afterwards.
                    rechecked_owner = ctypes.c_ulong()
                    user32.GetWindowThreadProcessId(window, ctypes.byref(rechecked_owner))
                    rechecked_class = ctypes.create_unicode_buffer(256)
                    if rechecked_owner.value == process.pid:
                        user32.GetClassNameW(window, rechecked_class, len(rechecked_class))
                    can_request_close = rechecked_owner.value == process.pid and rechecked_class.value == "#32770"
                    record.update(closeRequestAttempted=can_request_close, closeRequestSucceeded=False)
                    if can_request_close:
                        ctypes.set_last_error(0)
                        close_posted = bool(user32.PostMessageW(window, 0x0010, 0, 0))  # WM_CLOSE
                        record.update(closeRequestSucceeded=close_posted,
                            closeRequestError=ctypes.get_last_error() if not close_posted else 0)
                if len(observations) < 20:
                    observations.append(record)
                return 1
            deadline = time.monotonic() + 20
            while process.poll() is None and time.monotonic() < deadline:
                enumeration_count += 1
                if not user32.EnumWindows(close_owned_dialog, 0):
                    enumeration_failures += 1
                time.sleep(.05)
            self.assertEqual(process.wait(timeout=2), 2, "ordinary launcher started a known-revoked build")
            time.sleep(.4)
            self.assertFalse(marker.exists(), "withdrawn application received a normal-launch process")
            plan = subprocess.run([str(launcher), "--print-launch-plan"], capture_output=True,
                timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(plan.returncode, 2)
            self.assertFalse(json.loads(plan.stdout)["valid"])
            self.steps.append({"step": "refuse-normal-launch-" + label, "exitCode": 2,
                               "withdrawnChildStarted": False, "formalGswAcceptance": False,
                               "windowEvidence": window_evidence.name})
        finally:
            # Synthetic same-owner class/control facts only: no title, text,
            # screenshot, foreign-process information or desktop contents.
            write_json(window_evidence, {"schema": "gsw.launch_rejection_windows/1", "testOnly": True,
                "formalGswAcceptance": False, "label": label, "launcherPid": process.pid,
                "elapsedMilliseconds": round((time.monotonic() - started) * 1000),
                "exitCodeBeforeCleanup": process.poll(), "observationLimit": 20,
                "enumerations": enumeration_count, "enumerationFailures": enumeration_failures,
                "observations": observations})
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
                startup = self.helper_request(name + "-startup", "--verify-installed",
                    intent=self.launch_intent(), success=False)
                self.assertEqual(startup[-1].get("code"), code)
                self.refused_launch("A")
                self.assertEqual(self.active("fixture-a"), pointer_a)
                self.assertEqual(self.accepted(), accepted_before)
            finally:
                receipt.write_bytes(original_receipt)
        extra = self.installation / "versions/fixture-a/unrecorded-owned-fixture.txt"
        try:
            extra.write_bytes(b"not in the accepted payload")
            rejected = self.helper_request("extra-slot-file", "--rollback", "stable", "fixture-a", success=False)
            self.assertEqual(rejected[-1].get("code"), "activation_slot_file_set_differs")
            startup = self.helper_request("extra-slot-startup", "--verify-installed",
                intent=self.launch_intent(), success=False)
            self.assertEqual(startup[-1].get("code"), "activation_slot_file_set_differs")
            self.refused_launch("A")
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
            startup = self.helper_request("changed-slot-startup", "--verify-installed",
                intent=self.launch_intent(), success=False)
            self.assertEqual(startup[-1].get("code"), "activation_slot_digest_mismatch")
            self.refused_launch("A")
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

    def test_health_confirmation_is_bound_to_actual_window_surface_build_exe_and_pid(self):
        original_root, original_helper = self.installation, self.helper
        root = Path(self.preparation["root"]) / "startup-health-installation"
        root.mkdir()
        shutil.copytree(original_root / "bin", root / "bin")
        shutil.copytree(original_root / "user-data", root / "user-data")
        shutil.copyfile(original_root / "bootstrap.json", root / "bootstrap.json")
        self.installation, self.helper = root, root / "bin" / original_helper.name
        try:
            self.select("B")
            self.helper_request("health-install-B", "--install", "stable")
            pointer = self.active("fixture-b")
            slots, data = tree_hashes(root / "versions"), tree_hashes(root / "user-data")
            record = self.launch("B")
            self.assertIs(record.get("viewportPainted"), True)
            self.assertEqual(record.get("healthScope"), "qt-fixture-window-surface")
            status = self.helper_request("health-query-B", "--startup-status")[-1]
            self.assertEqual(status.get("phase"), "startup_status")
            self.assertEqual(status.get("code"), "HEALTHY")
            attempt = status["attempt"]
            self.assertEqual(attempt["buildId"], "fixture-b")
            self.assertEqual(attempt["executableSha256"], digest(root / "versions/fixture-b/bin/Gaussian Scene Workbench.exe"))
            self.assertEqual(attempt["pid"], record["pid"])
            self.assertRegex(attempt["attemptId"], "^[0-9a-f]{32}$")
            self.assertRegex(attempt["processCreationTime"], "^[0-9a-f]{16}$")
            self.assertNotEqual(attempt["processCreationTime"], "0" * 16)
            self.assertEqual(attempt["healthScope"], "qt-fixture-window-surface")
            self.assertNotIn("nonce", attempt)
            self.assertNotIn("pipeName", attempt)
            self.assertEqual(self.active("fixture-b"), pointer)
            self.assertEqual(tree_hashes(root / "versions"), slots)
            self.assertEqual(tree_hashes(root / "user-data"), data)
            write_json(self.evidence / "startup-health-result.json", {"passed": True, "testOnly": True,
                "formalGswAcceptance": False, "scope": "qt-fixture-window-surface",
                "attempt": attempt, "sourceCommit": self.preparation["sourceCommit"],
                "sourceTree": self.preparation["sourceTree"]})
        finally:
            self.installation, self.helper = original_root, original_helper

    def test_legacy_protocol1_without_trust_keeps_explicit_local_developer_startup(self):
        original_root, original_helper = self.installation, self.helper
        root = Path(self.preparation["root"]) / "legacy-local-startup-installation"
        root.mkdir()
        # A legacy local developer installation has no TUF authority or helper
        # grant. It is NOT an authenticated public update installation.
        shutil.copytree(original_root / "bin", root / "bin",
            ignore=shutil.ignore_patterns(original_helper.name))
        slot = root / "versions/fixture-a"
        shutil.copytree(Path(self.preparation["root"]) / "package-A", slot)
        executable = slot / "bin/Gaussian Scene Workbench.exe"
        write_json(root / "current.json", {"schema": "gsw.native_active_version/1", "version": "fixture-a",
                                          "executableSha256": digest(executable)})
        files = sorted(path for path in (root / "bin").rglob("*") if path.is_file())
        write_json(root / "bootstrap.json", {"schema": "gsw.native_bootstrap/1", "protocol": 1,
            "files": [{"path": path.relative_to(root).as_posix(), "size": path.stat().st_size,
                       "sha256": digest(path)} for path in files]})
        self.installation = root
        try:
            self.assertFalse((root / ".update-trust-anchor").exists())
            self.assertFalse((root / "updates/trust").exists())
            before = tree_hashes(root)
            self.launch("A")
            self.assertEqual(tree_hashes(root), before)
            write_json(self.evidence / "legacy-local-startup-result.json", {"passed": True, "testOnly": True,
                "formalGswAcceptance": False, "admission": "local-developer-unsigned", "publisherAuthenticated": False,
                "sourceCommit": self.preparation["sourceCommit"], "sourceTree": self.preparation["sourceTree"]})
        finally:
            self.installation, self.helper = original_root, original_helper

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

    def test_revoked_current_build_cannot_restart_and_explicit_safe_A_still_launches(self):
        original_root, original_helper = self.installation, self.helper
        root = Path(self.preparation["root"]) / "startup-revocation-installation"
        root.mkdir()
        shutil.copytree(original_root / "bin", root / "bin")
        shutil.copytree(original_root / "user-data", root / "user-data")
        shutil.copyfile(original_root / "bootstrap.json", root / "bootstrap.json")
        self.installation, self.helper = root, root / "bin" / original_helper.name
        try:
            self.select("A")
            self.helper_request("startup-install-A", "--install", "stable")
            pointer_a = self.active("fixture-a")
            self.select("B")
            self.helper_request("startup-install-B", "--install", "stable")
            pointer_b = self.active("fixture-b")
            self.launch("B")
            # Exercise the actual read-only helper before withdrawal. Request
            # data binds identity/arguments only; it cannot supply authority.
            guarded = tree_hashes(root)
            intent = self.launch_intent()
            verified = self.helper_request("startup-verify-installed-B", "--verify-installed", intent=intent)
            self.assertEqual(verified[-1].get("phase"), "launch_verified")
            self.assertEqual(verified[-1].get("buildId"), "fixture-b")
            self.assertEqual(tree_hashes(root), guarded)
            for name, patch in (("program", {"program": "not-a-launch-target.exe"}),
                    ("root", {"root": str(root)}), ("other-accepted-build", {"buildId": "fixture-a"}),
                    ("digest", {"executableSha256": "0" * 64}),
                    ("argument-type", {"arguments": [7]}),
                    ("argument-count", {"arguments": ["x"] * 129}),
                    ("nul-argument", {"arguments": ["before\x00after"]}),
                    ("oversized-argument", {"arguments": ["x" * 65537]})):
                self.helper_request("startup-invalid-" + name, "--launch-current",
                    intent={**intent, **patch}, success=False)
                self.assertEqual(tree_hashes(root), guarded)
            duplicate = json.dumps(intent).replace('"buildId": "fixture-b"',
                '"buildId": "fixture-a", "buildId": "fixture-b"').encode()
            self.assertNotEqual(duplicate, json.dumps(intent).encode())
            self.helper_request("startup-duplicate-request", "--launch-current", raw_intent=duplicate, success=False)
            self.assertEqual(tree_hashes(root), guarded)
            # Missing or hardlinked fixed helpers cannot be replaced by a
            # runtime helper-name override or by another executable's JSON.
            hidden_helper = self.helper.with_name(self.helper.name + ".owned-backup")
            os.replace(self.helper, hidden_helper)
            try:
                self.refused_launch("B")
                os.link(hidden_helper, self.helper)
                try:
                    self.refused_launch("B")
                    self.helper_request("startup-hardlinked-helper", "--verify-installed",
                        intent=intent, success=False)
                finally:
                    self.helper.unlink()
            finally:
                os.replace(hidden_helper, self.helper)
            self.assertEqual(tree_hashes(root), guarded)
            for missing_name in (".publish.lock", ".update-trust-anchor"):
                missing = root / missing_name
                retained = root / (missing_name + ".owned-backup")
                os.replace(missing, retained)
                try:
                    self.helper_request("startup-missing-" + missing_name.strip("."),
                        "--verify-installed", intent=intent, success=False)
                    self.refused_launch("B")
                    self.assertFalse(missing.exists(), "read-only guard recreated missing trusted state")
                finally:
                    os.replace(retained, missing)
                self.assertEqual(tree_hashes(root), guarded)
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
            kernel.CreateFileW.restype = ctypes.c_void_p
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            lease = kernel.CreateFileW(str(root / ".publish.lock"), 0xC0000000, 0, None, 3, 0x80, None)
            self.assertNotEqual(lease, ctypes.c_void_p(-1).value)
            try:
                self.refused_launch("B")
            finally:
                kernel.CloseHandle(lease)
            self.assertEqual(tree_hashes(root), guarded)
            self.select("revoked-b")
            withdrawal = self.helper_request("startup-adopt-withdrawal-B", "--check", "stable", success=False)
            self.assertEqual(withdrawal[-1].get("code"), "BUILD_REVOKED")
            accepted = self.accepted()
            self.assertIn("fixture-b", accepted["revokedBuildIds"])
            journal = root / "updates/trust/trust-journal.json"
            state_bytes = journal.read_bytes()
            slots, bootstrap, data = tree_hashes(root / "versions"), tree_hashes(root / "bin"), tree_hashes(root / "user-data")
            self.refused_launch("B")
            self.assertEqual(self.active("fixture-b"), pointer_b)
            self.assertEqual(journal.read_bytes(), state_bytes)
            self.assertEqual(self.accepted(), accepted)
            self.assertEqual(tree_hashes(root / "versions"), slots)
            self.assertEqual(tree_hashes(root / "bin"), bootstrap)
            self.assertEqual(tree_hashes(root / "user-data"), data)
            original_bootstrap = (root / "bootstrap.json").read_bytes()
            anchor = root / ".update-trust-anchor"
            retained_anchor = root / ".update-trust-anchor.downgrade-owned-backup"
            for retained_trust in ("anchor-and-state", "state-without-anchor"):
                with self.subTest(legacy_downgrade=retained_trust):
                    try:
                        if retained_trust == "state-without-anchor":
                            os.replace(anchor, retained_anchor)
                        legacy = json.loads(original_bootstrap)
                        legacy["protocol"] = 1
                        (root / "bootstrap.json").write_text(json.dumps(legacy), encoding="utf-8")
                        protected = tree_hashes(root)
                        # Historical public trust cannot be downgraded to the
                        # unsigned local-developer path by reverting metadata.
                        self.refused_launch("B")
                        self.assertEqual(self.active("fixture-b"), pointer_b)
                        self.assertEqual(tree_hashes(root), protected)
                    finally:
                        (root / "bootstrap.json").write_bytes(original_bootstrap)
                        if retained_trust == "state-without-anchor":
                            os.replace(retained_anchor, anchor)
            self.assertEqual(journal.read_bytes(), state_bytes)
            self.assertEqual(self.accepted(), accepted)
            self.helper_request("startup-explicit-safe-A", "--rollback", "stable", "fixture-a")
            self.assertEqual(self.active("fixture-a"), pointer_a)
            self.launch("A", arguments=("--owned-fixture", "空白 日本語 & ; $(not-a-command)"), diagnostics=True)
            self.assertEqual(journal.read_bytes(), state_bytes)
            self.assertEqual(self.accepted(), accepted)
            self.assertEqual(tree_hashes(root / "versions"), slots)
            self.assertEqual(tree_hashes(root / "bin"), bootstrap)
            self.assertEqual(tree_hashes(root / "user-data"), data)
            write_json(self.evidence / "startup-revocation-result.json", {"passed": True, "testOnly": True,
                "formalGswAcceptance": False, "knownRevokedNormalLaunchRefused": True,
                "explicitSafeRollbackStillLaunches": True, "guardStateReadOnly": True,
                "sourceCommit": self.preparation["sourceCommit"], "sourceTree": self.preparation["sourceTree"]})
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
