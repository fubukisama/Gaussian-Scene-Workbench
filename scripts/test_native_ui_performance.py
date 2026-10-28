#!/usr/bin/env python3
"""Remote-only public CLI contracts; these do not execute GSW or a GPU test.

Synthetic receipts deliberately include invalid observations. A valid-looking
producer PASS cannot hide a missing case, byte change, idle frame or regression.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
CHECKER = REPO / "scripts/check_native_ui_performance.py"
ACCEPTANCE = REPO / "docs/ui_upgrade_20261028/PERFORMANCE_ACCEPTANCE.json"
PLAN = REPO / "docs/ui_upgrade_20261028/PERFORMANCE_PLAN.md"
GROUPS = [{"id": "empty-dpr1", "scenario": "empty", "dpr": 1},
          {"id": "empty-dpr2", "scenario": "empty", "dpr": 2},
          {"id": "light-dpr1", "scenario": "light", "dpr": 1},
          {"id": "medium-dpr1", "scenario": "medium", "dpr": 1}]


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def dump(path: Path, data: object) -> None:
    path.write_text(json.dumps(data, allow_nan=False), encoding="utf-8")


def camera(target: float = 0.0) -> dict:
    return {"target": [target, 0.0, 0.0], "distance": 4.0,
            "orbit": [42.0, 24.0, 0.0], "orthographic": False}


def sample(serial: int) -> dict:
    return {"frameSerial": serial, "hostNanoseconds": 100_000,
            "ownedCpuBytes": 4096, "requestedGpuBytes": 0,
            "cacheGeneration": 0, "cacheHit": False, "diagnosticOverlap": False}


class UiPerformanceCliContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if os.environ.get("GITHUB_ACTIONS") != "true":
            raise RuntimeError("Remote CI contracts only; no local automated-test fallback.")
        base = REPO / "native/.deps"
        base.mkdir(parents=True, exist_ok=True)
        cls.shared_temporary = tempfile.TemporaryDirectory(prefix="ui-performance-contract-fixtures-", dir=base)
        cls.shared = Path(cls.shared_temporary.name)
        cls.identities = {}
        # Public checker fixtures are independent, tiny/constant geometry. They
        # are not claimed as the real viewport generator's seed42 benchmark.
        # The medium payload still has the exact specified size/attributes.
        for case_id in ("points", "mesh", "3dgs", "2dgs", "medium"):
            gaussian = case_id in ("3dgs", "2dgs", "medium")
            count = 512202 if case_id == "medium" else 4 if case_id == "mesh" else 441
            fields = ["x", "y", "z"]
            if gaussian:
                fields += ["f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1"]
                if case_id != "2dgs":
                    fields += ["scale_2"]
                fields += ["rot_0", "rot_1", "rot_2", "rot_3"]
            fmt = "binary_little_endian" if case_id == "medium" else "ascii"
            header = f"ply\nformat {fmt} 1.0\nelement vertex {count}\n"
            header += "".join(f"property float {field}\n" for field in fields)
            if not gaussian:
                header += "property uchar red\nproperty uchar green\nproperty uchar blue\n"
            if case_id == "mesh":
                header += "element face 2\nproperty list uchar int vertex_indices\n"
            header += "end_header\n"
            path = cls.shared / (case_id + ".ply")
            if case_id == "medium":
                row = struct.pack("<14f", 0, 0, 0, .7, -.3, .1, 0, -5, -6, -7, .7, .1, .2, .4)
                with path.open("xb") as stream:
                    stream.write(header.encode("ascii"))
                    for _ in range(count):
                        stream.write(row)
            else:
                row = "0 0 0 225 100 30\n" if not gaussian else (
                    "0 0 0 1 -1 -1 3 -3.5 -3.5 1 0 0 0\n" if case_id == "2dgs" else
                    "0 0 0 1 -1 -1 3 -3.5 -3.5 -3.5 1 0 0 0\n")
                path.write_text(header + row * count + ("3 0 1 2\n3 2 1 3\n" if case_id == "mesh" else ""), encoding="ascii", newline="\n")
            cls.identities[case_id] = {"schema": "gsw.ui_performance_fixture/1", "path": str(path.resolve()),
                "sha256": sha(path), "bytes": path.stat().st_size,
                "parameters": {"generator": "scheduling-seed42-v1", "kind": case_id, "seed": 42,
                    "vertices": count, "faces": 2 if case_id == "mesh" else 0,
                    "shDegree": 0 if gaussian else -1, "scales": 2 if case_id == "2dgs" else 3 if gaussian else 0}}

    @classmethod
    def tearDownClass(cls) -> None:
        cls.shared_temporary.cleanup()

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="receipt-case-", dir=self.shared)
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.reports = {}
        self.manifest = {"schema": "gsw.native_ui_performance_manifest/1", "runnerId": "contract-owned-runner",
            "baselineSourceCommit": "a" * 40, "candidateSourceCommit": "b" * 40,
            "expectedGroups": copy.deepcopy(GROUPS), "processes": []}
        for group in GROUPS:
            for round_index, order in enumerate((("A", "B"), ("B", "A"), ("A", "B")), 1):
                for arm in order:
                    report = self.report(group, round_index, arm)
                    path = self.root / f"{group['id']}-{round_index}-{arm}.json"
                    dump(path, report)
                    self.reports[(group["id"], round_index, arm)] = (path, report)
                    self.manifest["processes"].append({"group": group["id"], "round": round_index, "arm": arm,
                        "path": str(path.resolve()), "sha256": sha(path), "exitCode": 0, "timedOut": False})

    def report(self, group: dict, round_index: int, arm: str) -> dict:
        observed = [sample(1)]
        observed[0]["hostNanoseconds"] = 1_000_000
        cases = []
        cursor = 10_000_000
        case_ids = {"empty": ["empty"], "light": ["points", "mesh", "3dgs", "2dgs"], "medium": ["medium"]}[group["scenario"]]
        for case_id in case_ids:
            warm_start = len(observed)
            warm_events = 2 if case_id == "medium" else 4
            for _ in range(warm_events):
                observed.append(sample(len(observed) + 1))
            begin = len(observed)
            events, swaps = [], []
            wanted = 8 if case_id == "medium" else 60 if case_id == "empty" else 52
            previous_camera = camera()
            for index in range(wanted):
                before_serial = observed[-1]["frameSerial"]
                observed.append(sample(before_serial + 1))
                after = camera(.01 if index % 2 == 0 else 0.0)
                event = {"dispatchNs": cursor, "handlerDoneNs": cursor + 1000,
                    "presentNs": cursor + 1_000_000, "beforeRenderSerial": before_serial,
                    "presentSerial": before_serial + 1, "cameraBefore": previous_camera,
                    "cameraAfterDispatch": after, "cameraAfterSwap": after}
                events.append(event)
                swaps.append({"serial": before_serial + 1, "ns": cursor + 1_000_000})
                previous_camera = after
                cursor += 2_000_000
            idle_start = swaps[-1]["ns"] + 600_000_000
            idle = []
            for _ in range(2):
                idle.append({"beginNs": idle_start, "endNs": idle_start + 1_000_000_000,
                    "newRenderedFrames": 0, "freshSwaps": 0, "metricsEmissions": 0,
                    "zeroTimerCallbacks": 0, "zeroTimerActiveBefore": False,
                    "zeroTimerActiveAfter": False, "cameraUnchanged": True})
                idle_start += 1_000_000_000
            cursor = idle_start + 1_000_000
            gaussian = case_id in ("3dgs", "2dgs", "medium")
            count = 0 if case_id == "empty" else 4 if case_id == "mesh" else 512202 if case_id == "medium" else 441
            model = {"translation": [0, 0, 0], "scale": [1, 1, 1], "rotation": [1, 0, 0, 0], "selected": False,
                "sourceShDegree": 0, "effectiveShDegree": 0, "renderedPointCount": count,
                "residentMeshTriangles": 0, "meshRenderingAvailable": case_id == "mesh",
                "meshPagingSettled": True, "pointPagingSettled": True,
                "gaussian": gaussian, "surfel": case_id == "2dgs", "indexed": gaussian,
                "attributeUploads": 1 if gaussian else 0, "shUploads": 1 if gaussian else 0, "orderUploads": 1 if gaussian else 0,
                "renderMode": 2 if gaussian else 1 if case_id == "mesh" else 0}
            cases.append({"id": case_id, "fixture": {} if case_id == "empty" else copy.deepcopy(self.identities[case_id]),
                "sourceVertexCount": count, "sourceFaceCount": 2 if case_id == "mesh" else 0,
                "modelBefore": model, "modelAfter": copy.deepcopy(model), "cameraBefore": camera(),
                "warmupEvents": warm_events, "warmupRange": [warm_start, begin], "observationRange": [begin, len(observed)],
                "events": events, "presentations": swaps, "idleSettleMilliseconds": 600, "idle": idle, "complete": True})
        dpr = group["dpr"]
        for observation in observed:
            observation["logicalSize"] = [640, 360]
            observation["actualDpr"] = dpr
        return {"schema": "gsw.native_ui_performance_process/1", "status": "RECORDED", "arm": arm,
            "round": round_index, "scenario": group["scenario"], "runnerId": "contract-owned-runner",
            "sourceCommit": ("a" if arm == "A" else "b") * 40, "executableSha256": ("a" if arm == "A" else "b") * 64,
            "environment": {"logicalSize": [640, 360], "physicalSize": [640 * dpr, 360 * dpr],
                "glViewport": [0, 0, 640 * dpr, 360 * dpr], "actualDpr": dpr, "qtScaleFactor": str(dpr),
                "qtVersion": "6.8.3", "qpaPlatform": "windows", "glVendor": "contract vendor",
                "glRenderer": "contract renderer", "glVersion": "4.5", "swapInterval": 1,
                "requestedSwapInterval": 1, "samples": 0, "theme": "light", "fontFamily": "Segoe UI",
                "fontPointSize": 9.0, "screenRefreshRate": 60.0, "screenLogicalDpi": 96.0,
                "sessionName": "contract", "fullyExposed": True, "captureDuringMeasurements": False,
                "glQueriesDuringMeasurements": False},
            "cases": cases, "preWindowWarmRange": [0, 1],
            "observation": {"enabled": True, "capacity": 512, "count": len(observed), "overflow": False, "samples": observed},
            "failures": [], "unready": [], "wallNanoseconds": cursor}

    def receipt(self, group="light-dpr1", round_index=1, arm="B") -> dict:
        return self.reports[(group, round_index, arm)][1]

    def evaluate(self) -> tuple[int, dict]:
        for entry in self.manifest["processes"]:
            path, report = self.reports[(entry["group"], entry["round"], entry["arm"])]
            dump(path, report)
            entry["sha256"] = sha(path)
        manifest = self.root / "manifest.json"
        dump(manifest, self.manifest)
        output = self.root / "accepted.json"
        process = subprocess.run([sys.executable, "-I", "-B", str(CHECKER), "--manifest", str(manifest),
                                  "--acceptance", str(ACCEPTANCE), "--plan", str(PLAN), "--output", str(output.resolve())],
                                 capture_output=True, text=True, timeout=45, check=False)
        self.assertTrue(output.is_file(), process.stdout + process.stderr)
        return process.returncode, json.loads(output.read_text(encoding="utf-8"))

    def assert_failed(self, expected_fragment: str) -> None:
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (2, "FAIL"), result)
        self.assertIn(expected_fragment, "\n".join(result["failures"]))

    def test_complete_same_runner_paired_receipts_are_accepted(self):
        code, result = self.evaluate()
        self.assertEqual((code, result["status"], result["freeze"]), (0, "PASS", "NO_THRESHOLDS_RELAXED"), result)
        self.assertEqual(len(result["rawSummaries"]), 24)

    def test_missing_true_2dgs_case_is_rejected(self):
        self.receipt()["cases"].pop()
        self.assert_failed("missing, reordered or duplicate scenario case")

    def test_wrong_actual_fixture_bytes_are_rejected(self):
        value = self.receipt()["cases"][0]["fixture"]
        wrong = self.root / "wrong-points.ply"
        wrong.write_bytes(Path(value["path"]).read_bytes() + b"changed")
        value["path"] = str(wrong.resolve())
        self.assert_failed("actual fixture bytes/hash mismatch")

    def test_dpr_and_physical_size_mismatch_is_rejected(self):
        self.receipt()["environment"]["physicalSize"] = [641, 360]
        self.assert_failed("actual physical size does not match DPR")

    def test_diagnostic_overlap_is_rejected_even_outside_warm_range(self):
        self.receipt()["observation"]["samples"][0]["diagnosticOverlap"] = True
        self.assert_failed("diagnostic overlap")

    def test_startup_cold_dimensions_are_not_substituted_with_later_dimensions(self):
        self.receipt()["observation"]["samples"][0]["logicalSize"] = [1000, 700]
        self.assert_failed("cold or warm overlay dimensions/DPR differ")

    def test_idle_new_viewport_frame_is_rejected(self):
        self.receipt()["cases"][0]["idle"][0]["newRenderedFrames"] = 1
        self.assert_failed("idle generated work")

    def test_owned_resource_cap_cannot_be_hidden_as_noise(self):
        policy = json.loads(ACCEPTANCE.read_text(encoding="utf-8"))
        self.receipt()["observation"]["samples"][0]["ownedCpuBytes"] = policy["budgets"]["owned_CPU_bytes_per_viewport"] + 1
        self.assert_failed("owned CPU resource hard cap exceeded")

    def test_stable_baseline_does_not_hide_candidate_regression(self):
        report = self.receipt()
        begin, end = report["cases"][0]["observationRange"]
        for observation in report["observation"]["samples"][begin:end]:
            observation["hostNanoseconds"] = 2_000_000
        self.assert_failed("regression exceeds frozen budget")

    def test_noisy_baseline_is_inconclusive_not_green(self):
        report = self.receipt(round_index=2, arm="A")
        begin, end = report["cases"][0]["observationRange"]
        for observation in report["observation"]["samples"][begin:end]:
            observation["hostNanoseconds"] = 3_000_000
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (3, "INCONCLUSIVE"), result)
        self.assertIn("baseline range exceeds frozen delta budget", "\n".join(result["inconclusive"]))

    def test_unrelated_composition_cannot_replace_first_fresh_swap(self):
        event = self.receipt()["cases"][0]["events"][0]
        event["presentSerial"] = event["beforeRenderSerial"]
        self.assert_failed("event/fresh swap timing or serial order")

    def test_process_source_identity_is_not_a_label_only(self):
        self.receipt()["sourceCommit"] = "c" * 40
        self.assert_failed("process source identity mismatch")


if __name__ == "__main__":
    unittest.main()
