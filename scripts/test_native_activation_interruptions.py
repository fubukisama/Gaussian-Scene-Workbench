"""Remote Windows only: real shared-core process termination and lock recovery.

This is the separately compiled developer-admission fault fixture, not the
production helper, online trust, stable launcher or formal GSW acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import unittest
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")


class ActivationInterruptions(unittest.TestCase):
    def command(self, base, checkpoint="-"):
        return [str(ARGS.driver), "--publish-fixture", str(base / "installation"),
                str(base / "candidate.zip"), str(base / "candidate.json"), checkpoint]

    def run_process(self, command, *, isolated_runtime=False):
        environment = None
        if isolated_runtime:
            environment = {key: value for key, value in os.environ.items()
                           if not key.upper().startswith(("QT_", "QML_"))}
            windows = Path(os.environ["SystemRoot"])
            environment["PATH"] = os.pathsep.join((str(windows / "System32"), str(windows)))
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
            timeout=20, creationflags=subprocess.CREATE_NO_WINDOW, env=environment)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        self.assertTrue(rows, result.stdout + result.stderr)
        return result.returncode, rows[-1]

    def prepare(self, checkpoint):
        base = ARGS.evidence / checkpoint
        base.mkdir(parents=True, exist_ok=False)
        generated = subprocess.run([sys.executable, "-I", "-B", str(ARGS.fixture), str(base)],
            capture_output=True, text=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(generated.returncode, 0, generated.stderr)
        with zipfile.ZipFile(base / "B.zip") as archive:
            payload = {name: archive.read(name) for name in archive.namelist()
                       if name != "release-files.json"}
        # A real executable is used so the surviving complete slot can actually
        # start a console-only probe after independent full manifest readback.
        payload["bin/Gaussian Scene Workbench.exe"] = ARGS.driver.read_bytes()
        runtime = {path.relative_to(ARGS.runtime_dir).as_posix(): path
                   for path in ARGS.runtime_dir.rglob("*") if path.is_file()}
        self.assertTrue({"qt6core.dll", "msvcp140.dll", "vcruntime140.dll"}.issubset(
            {name.casefold() for name in runtime}), "explicit app-local Qt/CRT runtime required")
        for name, path in runtime.items():
            self.assertFalse(path.is_symlink(), name)
            self.assertFalse(name.casefold().endswith(".exe"), "runtime directory must contain no tool EXEs")
            self.assertNotIn("bin/" + name, payload)
            payload["bin/" + name] = path.read_bytes()
        source = json.loads(payload["source-manifest.json"])
        source["executableSha256"] = sha(payload["bin/Gaussian Scene Workbench.exe"])
        payload["source-manifest.json"] = json.dumps(source, sort_keys=True).encode()
        original_offer = json.loads((base / "offer.json").read_text(encoding="utf-8"))
        self.package(base, payload, original_offer, "fixture-B")
        code, record = self.run_process(self.command(base))
        self.assertEqual((code, record.get("ok")), (0, True), record)
        root = base / "installation"
        (root / "user-data").mkdir()
        (root / "user-data/paused-checkpoint-sentinel").write_bytes(b"owned-paused-state\0\1")
        (root / "user-data/project-sentinel").write_bytes(b"owned-project-unchanged")
        preserved = {str(path.relative_to(root)): path.read_bytes()
                     for prefix in ("bin", "user-data", "versions/fixture-B")
                     for path in (root / prefix).rglob("*") if path.is_file()}
        preserved["bootstrap.json"] = (root / "bootstrap.json").read_bytes()
        self.package(base, payload, original_offer, "fixture-C")
        return base, preserved

    def package(self, base, payload, offer, build_id):
        offer = dict(offer, buildId=build_id)
        manifest = {"schema": "gsw.native_update_files/1", "product": offer["product"],
            "platform": offer["platform"], "buildId": build_id,
            "sourceCommit": offer["sourceCommit"], "sourceTree": offer["sourceTree"],
            "files": [{"path": name, "size": len(data), "sha256": sha(data)}
                      for name, data in sorted(payload.items())]}
        raw = json.dumps(manifest, sort_keys=True).encode()
        with zipfile.ZipFile(base / "candidate.zip", "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in payload.items():
                archive.writestr(name, data)
            archive.writestr("release-files.json", raw)
        blob = (base / "candidate.zip").read_bytes()
        offer.update(archiveLength=len(blob), archiveSha256=sha(blob),
            payloadManifestSha256=sha(raw),
            correspondingSourceSha256=sha(payload["source-manifest.json"]),
            unpackedLength=len(raw) + sum(map(len, payload.values())), fileCount=len(payload))
        write_json(base / "candidate.json", offer)

    def verify_current_can_start(self, root, expected):
        pointer = json.loads((root / "current.json").read_text(encoding="utf-8"))
        self.assertEqual(pointer["version"], expected)
        slot = root / "versions" / expected
        manifest = json.loads((slot / "release-files.json").read_text(encoding="utf-8"))
        actual = {path.relative_to(slot).as_posix() for path in slot.rglob("*") if path.is_file()}
        self.assertEqual(actual, {row["path"] for row in manifest["files"]} | {"release-files.json"})
        for row in manifest["files"]:
            data = (slot / row["path"]).read_bytes()
            self.assertEqual((len(data), sha(data)), (row["size"], row["sha256"]))
        executable = slot / "bin/Gaussian Scene Workbench.exe"
        self.assertEqual(sha(executable.read_bytes()), pointer["executableSha256"])
        code, probe = self.run_process([str(executable), "--slot-probe"], isolated_runtime=True)
        self.assertEqual((code, probe.get("probeReady")), (0, True), probe)

    def check_interruption(self, checkpoint):
        base, preserved = self.prepare(checkpoint)
        root = base / "installation"
        process = subprocess.Popen(self.command(base, checkpoint), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True, encoding="utf-8",
            creationflags=subprocess.CREATE_NO_WINDOW)
        lines, received = [], queue.Queue()

        def consume():
            for line in process.stdout:
                lines.append(line)
                received.put(line)
            received.put(None)

        reader = threading.Thread(target=consume, daemon=True)
        reader.start()
        try:
            deadline = time.monotonic() + 10
            marker = None
            while time.monotonic() < deadline:
                try:
                    line = received.get(timeout=max(.01, deadline - time.monotonic()))
                except queue.Empty:
                    break
                if line is None:
                    break
                if line.startswith("{"):
                    row = json.loads(line)
                    if row.get("checkpoint") == checkpoint:
                        marker = row
                        break
            self.assertIsNotNone(marker, "actual activation checkpoint not reached: " + "".join(lines))
            self.assertEqual(marker["pid"], process.pid)
            self.assertIsNone(process.poll(), "fixture must still own the process and lock")
            # This is a second OS process, not two C++ lock objects in one PID.
            code, contender = self.run_process(self.command(base))
            self.assertNotEqual(code, 0)
            self.assertEqual(contender["code"], "activation_busy_or_lock_invalid")
            process.kill()
            process.wait(timeout=5)
            self.assertNotEqual(process.returncode, 0)
            reader.join(timeout=2)
            expected = "fixture-C" if checkpoint == "pointer-after" else "fixture-B"
            self.verify_current_can_start(root, expected)
            if checkpoint == "extract-file-written":
                stages = list((root / "updates/staging").iterdir())
                self.assertTrue(stages)
                self.assertTrue(any(path.is_file() for stage in stages for path in stage.rglob("*")))
                self.assertFalse((root / "versions/fixture-C").exists())
            elif checkpoint != "pointer-after":
                self.assertTrue((root / "versions/fixture-C/release-files.json").is_file())
            for name, data in preserved.items():
                self.assertEqual((root / name).read_bytes(), data, name)
            code, result = self.run_process(self.command(base))
            self.assertEqual((code, result.get("ok")), (0, True), result)
            self.verify_current_can_start(root, "fixture-C")
            for name, data in preserved.items():
                self.assertEqual((root / name).read_bytes(), data, name)
            write_json(base / "result.json", {"checkpoint": checkpoint, "result": "PASS",
                "actualTerminatedPid": process.pid, "secondProcessBusy": True,
                "lockRecovered": True, "slotConsoleProbeStarted": True,
                "probeUsesOnlyAppLocalRuntimeAndWindowsPath": True,
                "onlineTrustAcceptance": False, "stableLauncherAcceptance": False,
                "formalGswAcceptance": False, "testOnly": True})
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            reader.join(timeout=2)
            (base / "activation-process.log").write_text("".join(lines), encoding="utf-8")

    def test_01_killed_during_extraction_preserves_startable_previous_slot(self):
        self.check_interruption("extract-file-written")

    def test_02_killed_after_slot_publication_recovers_without_overwrite(self):
        self.check_interruption("slot-published")

    def test_03_killed_before_pointer_switch_retains_previous_version(self):
        self.check_interruption("pointer-before")

    def test_04_killed_after_pointer_switch_retains_complete_new_version(self):
        self.check_interruption("pointer-after")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true")
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    ARGS, remaining = parser.parse_known_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or not ARGS.remote_fixture_only:
        parser.error("explicit_remote_windows_fixture_required")
    ARGS.driver = ARGS.driver.absolute()
    ARGS.fixture = ARGS.fixture.absolute()
    ARGS.runtime_dir = ARGS.runtime_dir.absolute()
    ARGS.evidence = ARGS.evidence.absolute()
    ARGS.evidence.mkdir(parents=True, exist_ok=False)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
