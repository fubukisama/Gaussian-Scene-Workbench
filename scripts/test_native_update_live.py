"""Remote-only live A/CPU-worker/queue preservation during signed B install.

The A/B programs are real Qt windows, not GSW. The worker is a deterministic
owned CPU lifecycle fixture, not 3DGS/2DGS or an optimizer-resume substitute.
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
import sys
import time
import unittest
import uuid


def require_remote():
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true":
        raise ValueError("explicit_remote_windows_fixture_required")


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".owned-" + str(os.getpid()))
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def read_owned_json_shared(path):
    # The worker atomically replaces its queue. The test reader must permit
    # FILE_SHARE_DELETE rather than accidentally breaking that real write.
    # No retry or exception swallowing: genuine access errors remain failures.
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0x80000000, 0x1 | 0x2 | 0x4,
        None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        kernel.CloseHandle(handle)
        raise
    with os.fdopen(descriptor, "rb") as stream:
        raw = stream.read(8193)
    if len(raw) > 8192:
        raise ValueError("owned_fixture_json_oversized")
    return json.loads(raw)


def owned_worker(queue_path, stop_path):
    require_remote()
    state = json.loads(queue_path.read_text(encoding="utf-8"))
    if state.get("schema") != "gsw.owned_cpu_queue_fixture/1" or state.get("step") != 0:
        raise ValueError("owned_queue_fixture_required")
    state["workerIdentity"] = {"selfPid": os.getpid(), "parentPid": os.getppid(),
        "sysExecutable": sys.executable, "baseExecutable": sys._base_executable}
    events = queue_path.with_suffix(".events.jsonl")
    deadline = time.monotonic() + 50
    with events.open("x", encoding="utf-8", newline="\n") as stream:
        while not stop_path.exists() and time.monotonic() < deadline:
            state["step"] += 1
            state["phase"] = "running"
            stamp = time.monotonic_ns()
            stream.write(json.dumps({"pid": os.getpid(), "taskId": state["taskId"],
                "step": state["step"], "monotonicNs": stamp}) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
            atomic_json(queue_path, state)
            time.sleep(.025)
    state["phase"] = "stopped"
    atomic_json(queue_path, state)
    return 0


def owned_parent_ids(process_ids):
    # The documented Toolhelp snapshot is queried only for these known owned
    # fixture PIDs; never record unrelated process names, paths or arguments.
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
            ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    found = {}
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        more = kernel.Process32FirstW(handle, ctypes.byref(entry))
        while more:
            if entry.th32ProcessID in process_ids:
                found[str(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
                if len(found) == len(process_ids):
                    break
            more = kernel.Process32NextW(handle, ctypes.byref(entry))
        return found
    finally:
        kernel.CloseHandle(handle)


class LiveUpdate(unittest.TestCase):
    def assert_same_live_process(self, diagnostic, original):
        current = diagnostic.snapshot()
        self.assertIs(current.get("opened"), True)
        self.assertIs(current.get("signalled"), False)
        self.assertEqual(current["processCreationTime"], original["processCreationTime"])
        self.assertEqual(Path(current["image"]).resolve(), Path(original["image"]).resolve())
        return current

    def wait_json(self, path, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        value = {}
        while time.monotonic() < deadline:
            if path.exists():
                value = read_owned_json_shared(path)
                if predicate(value):
                    return value
            time.sleep(.025)
        self.fail("owned fixture did not reach requested public state: " + json.dumps(value))

    def window_for_pid(self, pid):
        user = ctypes.WinDLL("user32", use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
        user.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        user.IsWindowVisible.argtypes = [ctypes.c_void_p]
        user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
        found = []

        @callback_type
        def observe(window, _parameter):
            owner = ctypes.c_ulong()
            user.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if owner.value == pid and user.IsWindowVisible(window):
                found.append(int(window))
            return 1

        self.assertTrue(user.EnumWindows(observe, 0))
        self.assertEqual(len(found), 1, "exactly one owned fixture top-level window required")
        return found[0]

    def test_live_old_window_cpu_worker_and_persistent_queue_survive_B(self):
        preparation = json.loads(ARGS.preparation.read_text(encoding="utf-8-sig"))
        self.assertEqual(preparation.get("schema"), "gsw.update_end_to_end_preparation/1")
        self.assertIs(preparation.get("testOnly"), True)
        self.assertIs(preparation.get("formalGswAcceptance"), False)
        base = Path(preparation["root"]).absolute()
        original = Path(preparation["installationRoot"]).absolute()
        self.assertEqual(original, base / "installation")
        root = base / "live-owned-installation"
        root.mkdir(exist_ok=False)
        shutil.copytree(original / "bin", root / "bin")
        shutil.copytree(original / "user-data", root / "user-data")
        shutil.copyfile(original / "bootstrap.json", root / "bootstrap.json")
        helper = MODULE.EndToEnd(methodName="runTest")
        helper.preparation, helper.installation = preparation, root
        helper.helper = root / "bin/GSW Update Fixture Helper.exe"
        self.assertTrue(helper.helper.is_file(), "run after E2E helper deployment")
        helper.evidence, helper.steps = ARGS.evidence, []
        helper.fixture = json.loads(Path(preparation["fixture"]).read_text(encoding="utf-8"))
        helper.trust_cli = ARGS.trust_cli
        helper.select("A")
        helper.helper_request("live-install-A", "--install", "stable")
        helper.active("fixture-a")
        protected = {name: MODULE.tree_hashes(root / name)
                     for name in ("bin", "user-data", "versions/fixture-a")}
        bootstrap = (root / "bootstrap.json").read_bytes()

        worker_root = ARGS.evidence / "owned-worker"
        worker_root.mkdir()
        queue_path, stop_path = worker_root / "queue.json", worker_root / "stop"
        task_id = uuid.uuid4().hex
        initial = {"schema": "gsw.owned_cpu_queue_fixture/1", "taskId": task_id,
            "step": 0, "phase": "pending", "backend": "owned-cpu-lifecycle-fixture",
            "pausedTask": {"checkpoint": "owned-checkpoint-sentinel", "state": "paused"}}
        atomic_json(queue_path, initial)
        worker = subprocess.Popen([sys.executable, "-I", "-B", str(Path(__file__).absolute()),
            "--owned-worker", str(queue_path), str(stop_path)], stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        a_marker = ARGS.evidence / "live-A.json"
        environment = os.environ.copy()
        environment.update(GSW_UPDATE_FIXTURE_MARKER=str(a_marker),
            GSW_UPDATE_FIXTURE_HEALTH_MODE="long-running-no-ack", QT_QPA_PLATFORM="windows")
        for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH"):
            environment.pop(key, None)
        launcher = subprocess.Popen([str(root / "bin/Gaussian Scene Workbench Launcher.exe")],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW)
        a_process, worker_process, worker_launcher_process = None, None, None
        try:
            self.assertEqual(launcher.wait(timeout=15), 0)
            a_record = self.wait_json(a_marker, lambda row: row.get("phase") == "shown")
            self.assertEqual(a_record["label"], "A")
            self.assertIs(a_record["windowShown"], True)
            self.assertIs(a_record["viewportPainted"], True)
            a_process = MODULE.ProcessDiagnostic(a_record["pid"])
            before = a_process.snapshot()
            self.assertIs(before.get("opened"), True)
            self.assertIs(before.get("signalled"), False)
            self.assertEqual(Path(before["image"]).resolve(),
                (root / "versions/fixture-a/bin/Gaussian Scene Workbench.exe").resolve())
            a_window = self.window_for_pid(a_record["pid"])
            worker_before = self.wait_json(queue_path, lambda row: row.get("step", 0) >= 2)
            self.assertIsNone(worker.poll())
            self.assertEqual(worker_before["taskId"], task_id)
            identity = worker_before["workerIdentity"]
            actual_worker_pid = identity["selfPid"]
            self.assertIsInstance(actual_worker_pid, int)
            self.assertGreater(actual_worker_pid, 0)
            self.assertLessEqual(actual_worker_pid, 0xFFFFFFFF)
            parent_ids = owned_parent_ids({worker.pid, actual_worker_pid})
            self.assertEqual(parent_ids[str(worker.pid)], os.getpid())
            expected_parent = worker.pid if actual_worker_pid != worker.pid else os.getpid()
            self.assertEqual(parent_ids[str(actual_worker_pid)], expected_parent)
            self.assertEqual(identity["parentPid"], expected_parent)
            self.assertEqual(Path(identity["sysExecutable"]).resolve(), Path(sys.executable).resolve())
            self.assertEqual(Path(identity["baseExecutable"]).resolve(), Path(sys._base_executable).resolve())
            # Windows venv python.exe may be a redirector. Bind the actual
            # script process only after independently proving this exact owned
            # parent chain, expected image and OS birth; never accept any PID.
            worker_launcher_process = MODULE.ProcessDiagnostic(worker.pid)
            worker_process = MODULE.ProcessDiagnostic(actual_worker_pid)
            launcher_before, actual_before = worker_launcher_process.snapshot(), worker_process.snapshot()
            for snapshot in (launcher_before, actual_before):
                self.assertIs(snapshot.get("opened"), True)
                self.assertIs(snapshot.get("signalled"), False)
                self.assertRegex(snapshot["processCreationTime"], "^[0-9a-f]{16}$")
            self.assertEqual(Path(launcher_before["image"]).resolve(), Path(sys.executable).resolve())
            expected_image = sys._base_executable if actual_worker_pid != worker.pid else sys.executable
            self.assertEqual(Path(actual_before["image"]).resolve(), Path(expected_image).resolve())
            self.assertGreaterEqual(int(actual_before["processCreationTime"], 16),
                                    int(launcher_before["processCreationTime"], 16))
            helper.select("B")
            install_started = time.monotonic_ns()
            helper.helper_request("live-install-B", "--install", "stable")
            install_finished = time.monotonic_ns()
            helper.active("fixture-b")
            after_install = a_process.snapshot()
            self.assertIs(after_install["signalled"], False, "updater must not end the old A process")
            self.assertEqual(after_install["processCreationTime"], before["processCreationTime"])
            self.assertEqual(self.window_for_pid(a_record["pid"]), a_window)
            self.assertIsNone(worker.poll(), "updater must not end the owned CPU worker")
            actual_after_install = self.assert_same_live_process(worker_process, actual_before)
            self.assert_same_live_process(worker_launcher_process, launcher_before)
            queue_after = self.wait_json(queue_path, lambda row: row.get("step", 0) > worker_before["step"])
            self.assertEqual(queue_after["taskId"], task_id)
            self.assertEqual(queue_after["pausedTask"], initial["pausedTask"])
            rows = [json.loads(line) for line in queue_path.with_suffix(".events.jsonl").read_bytes(
                ).splitlines(keepends=True) if line.endswith(b"\n")]
            during = [row for row in rows if install_started <= row["monotonicNs"] <= install_finished]
            self.assertTrue(during, "CPU queue must actually advance while B installs")
            self.assertEqual(queue_after["workerIdentity"], identity)
            facts = {"schema": "gsw.owned_worker_identity_diagnostic/1", "testOnly": True,
                "formalGswAcceptance": False, "taskId": task_id,
                "popenPid": worker.pid, "controllerPid": os.getpid(),
                "controllerExecutable": sys.executable, "controllerBaseExecutable": sys._base_executable,
                "workerSelfReport": identity, "eventPids": sorted({row["pid"] for row in rows}),
                "osParentIds": parent_ids, "lineageValidated": True,
                "popenProcessBefore": launcher_before, "actualWorkerBefore": actual_before,
                "actualWorkerAfterInstall": actual_after_install}
            MODULE.write_json(ARGS.evidence / "worker-identity-diagnostic.json", facts)
            self.assertTrue(all(row["pid"] == actual_worker_pid and row["taskId"] == task_id for row in rows))
            b_record = helper.launch("B")
            self.assertNotEqual(b_record["pid"], a_record["pid"])
            after_b = a_process.snapshot()
            self.assertIs(after_b["signalled"], False, "A must still be alive after B uses the same entry")
            self.assertEqual(after_b["processCreationTime"], before["processCreationTime"])
            self.assertEqual(self.window_for_pid(a_record["pid"]), a_window)
            self.assertIsNone(worker.poll())
            self.assert_same_live_process(worker_process, actual_before)
            self.assert_same_live_process(worker_launcher_process, launcher_before)
            for name, hashes in protected.items():
                self.assertEqual(MODULE.tree_hashes(root / name), hashes, name)
            self.assertEqual((root / "bootstrap.json").read_bytes(), bootstrap)
            # A's existing fixed16s fixture lifetime ends normally. This never
            # increases the product15s health observer or kills the A process.
            self.assertEqual(a_process.kernel.WaitForSingleObject(a_process.handle, 18000), 0)
            self.assertEqual(a_process.snapshot()["exitCode"], 0)
            final_a = self.wait_json(a_marker, lambda row: row.get("phase") == "normal_exit")
            self.assertEqual(final_a["exitCode"], 0)
            stop_path.write_bytes(b"owned-worker-release\n")
            self.assertEqual(worker_process.kernel.WaitForSingleObject(worker_process.handle, 5000), 0)
            self.assertEqual(worker_process.snapshot()["exitCode"], 0)
            self.assertEqual(worker.wait(timeout=5), 0)
            final_queue = json.loads(queue_path.read_text(encoding="utf-8"))
            self.assertEqual(final_queue["taskId"], task_id)
            self.assertEqual(final_queue["pausedTask"], initial["pausedTask"])
            self.assertEqual(final_queue["phase"], "stopped")
            self.assertGreater(final_queue["step"], queue_after["step"])
            MODULE.write_json(ARGS.evidence / "live-result.json", {
                "schema": "gsw.update_live_fixture_evidence/1", "passed": True,
                "testOnly": True, "production": False, "formalGswAcceptance": False,
                "scope": "signed-update-real-qt-window-and-owned-cpu-queue-only",
                "oldWindowPid": a_record["pid"], "oldWindowBirth": before["processCreationTime"],
                "newWindowPid": b_record["pid"], "workerPid": actual_worker_pid,
                "workerLauncherPid": worker.pid, "workerProcessCreationTime": actual_before["processCreationTime"],
                "workerImage": actual_before["image"], "workerLineageValidated": True,
                "workerTicksDuringInstall": len(during), "workerFinalStep": final_queue["step"],
                "bootstrapUnchanged": True, "oldSlotUnchanged": True, "userDataUnchanged": True,
                "pausedCpuSentinelUnchanged": True, "optimizerResume3DGS": "NOT_RUN",
                "optimizerResume2DGS": "NOT_RUN", "sourceCommit": preparation["sourceCommit"],
                "sourceTree": preparation["sourceTree"], "steps": helper.steps})
        finally:
            if launcher.poll() is None:
                launcher.kill()
                launcher.wait(timeout=5)
            stop_path.write_bytes(b"owned-worker-release\n")
            if worker.poll() is None:
                try:
                    worker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait(timeout=5)
            worker.stderr.close()
            if worker_process:
                worker_process.close()
            if worker_launcher_process:
                worker_launcher_process.close()
            if a_process:
                # Read-only process handle; never terminate a detached window.
                a_process.close()


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--owned-worker":
        raise SystemExit(owned_worker(Path(sys.argv[2]), Path(sys.argv[3])))
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
    ARGS.evidence.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name("test_native_update_end_to_end.py")
    spec = importlib.util.spec_from_file_location("gsw_update_e2e_helpers", source)
    MODULE = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(MODULE)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LiveUpdate)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
