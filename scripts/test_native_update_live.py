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


def owned_worker(queue_path, stop_path):
    require_remote()
    state = json.loads(queue_path.read_text(encoding="utf-8"))
    if state.get("schema") != "gsw.owned_cpu_queue_fixture/1" or state.get("step") != 0:
        raise ValueError("owned_queue_fixture_required")
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


class LiveUpdate(unittest.TestCase):
    def wait_json(self, path, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        value = {}
        while time.monotonic() < deadline:
            if path.exists():
                value = json.loads(path.read_text(encoding="utf-8"))
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
        a_process = None
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
            queue_after = self.wait_json(queue_path, lambda row: row.get("step", 0) > worker_before["step"])
            self.assertEqual(queue_after["taskId"], task_id)
            self.assertEqual(queue_after["pausedTask"], initial["pausedTask"])
            rows = [json.loads(line) for line in queue_path.with_suffix(".events.jsonl").read_bytes(
                ).splitlines(keepends=True) if line.endswith(b"\n")]
            during = [row for row in rows if install_started <= row["monotonicNs"] <= install_finished]
            self.assertTrue(during, "CPU queue must actually advance while B installs")
            self.assertTrue(all(row["pid"] == worker.pid and row["taskId"] == task_id for row in rows))
            b_record = helper.launch("B")
            self.assertNotEqual(b_record["pid"], a_record["pid"])
            after_b = a_process.snapshot()
            self.assertIs(after_b["signalled"], False, "A must still be alive after B uses the same entry")
            self.assertEqual(after_b["processCreationTime"], before["processCreationTime"])
            self.assertEqual(self.window_for_pid(a_record["pid"]), a_window)
            self.assertIsNone(worker.poll())
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
                "newWindowPid": b_record["pid"], "workerPid": worker.pid,
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
