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
        self.create_source_receipts()
        for group in GROUPS:
            for round_index, order in enumerate((("A", "B"), ("B", "A"), ("A", "B")), 1):
                for arm in order:
                    report = self.report(group, round_index, arm)
                    path = self.root / f"{group['id']}-{round_index}-{arm}.json"
                    dump(path, report)
                    self.reports[(group["id"], round_index, arm)] = (path, report)
                    self.manifest["processes"].append({"group": group["id"], "round": round_index, "arm": arm,
                        "path": str(path.resolve()), "sha256": sha(path), "exitCode": 0, "timedOut": False})

    def create_source_receipts(self):
        # Actual owned files behind the external receipt. These are never run;
        # tests exercise the checker CLI, not a fake program's behavior.
        self.source_dirs = {}
        qa_files = ("native/src/UiPaintObservation.h", "native/src/UiPerformanceSmokeTest.cpp", "native/src/UiPerformanceSmokeTest.h")
        main_block = (
            "  if (parser.isSet(uiPerformanceOption)) {\n"
            "    if (!gsw::prepareUiPerformanceWindow(window)) return 2;\n"
            "    if (auto *viewport = window.findChild<gsw::NativeViewport *>())\n"
            "      viewport->setUiPaintObservationEnabled(true);\n"
            "  }\n")
        for arm in ("A", "B"):
            directory = self.root / ("source-" + arm)
            (directory / "native/src").mkdir(parents=True)
            self.source_dirs[arm] = directory
            for relative in qa_files:
                (directory / relative).write_text("// common passive QA fixture\n", encoding="utf-8", newline="\n")
            (directory / "native/src/main.cpp").write_text("// non-QA prefix\n" + main_block + "  window.show();\n", encoding="utf-8", newline="\n")
            (directory / "native/src/NativeViewport.cpp").write_text("// production " + arm + "\n", encoding="utf-8", newline="\n")
            (directory / "owned.exe").write_bytes(("not executable; CLI receipt contract " + arm).encode())
        diff = self.root / "common-qa-overlay.diff"
        diff.write_text("diff --git a/native/src/main.cpp b/native/src/main.cpp\n"
                        "--- a/native/src/main.cpp\n+++ b/native/src/main.cpp\n"
                        "@@ -1 +1 @@\n-old QA block\n+common QA block\n", encoding="utf-8", newline="\n")
        a = self.source_dirs["A"]
        protected = sha(a / "native/src/NativeViewport.cpp")
        overlay = {"schema": "gsw.ui_performance_common_qa_overlay/1", "sourceProductionBaseCommit": "a" * 40,
            "fromCommit": "b" * 40, "files": [{"path": relative, "beforeSha256": "c" * 64,
                "afterSha256": sha(a / relative)} for relative in qa_files],
            "mainPatch": {"path": "native/src/main.cpp", "baselineBlob": "c" * 40, "beforeSha256": "c" * 64,
                "afterSha256": sha(a / "native/src/main.cpp"), "beforeBlockSha256": "c" * 64,
                "afterBlockSha256": hashlib.sha256(main_block.encode()).hexdigest(), "prefixUnchanged": True, "suffixUnchanged": True},
            "changedPaths": ["native/src/main.cpp"], "protectedProductionFiles": [
                {"path": "native/src/NativeViewport.cpp", "beforeSha256": protected, "afterSha256": protected}],
            "unlistedTrackedDiff": [], "actualDiffSha256": sha(diff), "baseTree": "c" * 40,
            "compositeTree": "d" * 40, "untouchedBaselineBinary": False, "productionOutsideWhitelistUnchanged": True}
        self.overlay_path = self.root / "common-qa-overlay.json"
        dump(self.overlay_path, overlay)
        self.source_identities = {}
        for arm in ("A", "B"):
            directory = self.source_dirs[arm]
            self.source_identities[arm] = {"arm": arm, "sourceCommit": ("a" if arm == "A" else "b") * 40,
                "sourceProductionBaseCommit": ("a" if arm == "A" else "b") * 40,
                "baseSourceTree": ("c" if arm == "A" else "e") * 40, "sourceTree": ("d" if arm == "A" else "e") * 40,
                "workingTreeIsClean": arm == "B", "commonObserverOverlayApplied": arm == "A",
                "commonObserverOverlayFromCommit": "b" * 40, "commonObserverOverlayIdentity": sha(self.overlay_path),
                "sourceDirectory": str(directory.resolve()), "executable": str((directory / "owned.exe").resolve()),
                "executableSha256": sha(directory / "owned.exe")}
        self.sources_path = self.root / "sources.json"
        acceptance = json.loads(ACCEPTANCE.read_text(encoding="utf-8-sig"))
        dump(self.sources_path, {"schema": "gsw.ui_performance_sources/2", "runId": "contract-owned-runner",
            "arms": list(self.source_identities.values()), "commonObserverOverlayReceipt": str(self.overlay_path.resolve()),
            "commonObserverOverlayIdentity": sha(self.overlay_path), "acceptanceSha256": sha(ACCEPTANCE),
            "planSha256": acceptance["plan_sha256"]})
        self.manifest["sources"] = str(self.sources_path.resolve())

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
            "sourceCommit": ("a" if arm == "A" else "b") * 40, "executableSha256": self.source_identities[arm]["executableSha256"],
            "sourceIdentity": copy.deepcopy(self.source_identities[arm]), "sourcesReceiptSha256": sha(self.sources_path),
            "exposure": {"logicalSize": [640, 360], "actualDpr": dpr, "globalViewportRect": [10, 10, 640, 360],
                "screenAvailableRect": [0, 0, 1920, 1080], "screenGeometryRect": [0, 0, 1920, 1080],
                "windowRect": [0, 0, 640, 390], "windowFrameGeometry": [10, 10, 650, 420],
                "screenContainsViewport": True, "visible": True, "windowExists": True, "windowExposed": True,
                "windowMinimized": False, "contextExists": True, "contextValid": True, "fullyExposed": True,
                "ancestors": [{"class": "QMainWindow", "rect": [0, 0, 640, 390], "viewportRect": [0, 20, 640, 360],
                    "containsViewport": True, "visible": True}]},
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

    def test_upload_during_measured_pan_remains_rejected(self):
        case = next(value for value in self.receipt()["cases"] if value["id"] == "3dgs")
        case["modelAfter"]["attributeUploads"] += 1
        case["modelAfter"]["orderUploads"] += 1
        self.assert_failed("pan changed transforms, model fidelity or resident uploads")

    def test_setup_upload_offsets_preserve_raw_totals_and_zero_pan_deltas(self):
        # Preserve the original +1 observed input case under the corrected
        # lifetime-counter contract. This does not permit uploads during pan.
        for case in self.receipt()["cases"]:
            if case["id"] in ("3dgs", "2dgs"):
                for boundary in ("modelBefore", "modelAfter"):
                    case[boundary]["attributeUploads"] += 1
                    case[boundary]["orderUploads"] += 1
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (0, "PASS"), result)
        process = next(item for item in result["rawSummaries"] if
                       item["group"] == "light-dpr1" and item["round"] == 1 and item["arm"] == "B")
        for case_id in ("3dgs", "2dgs"):
            case = process["cases"][case_id]
            self.assertEqual(case["rawModelBefore"]["attributeUploads"], 2)
            self.assertEqual(case["rawModelAfter"]["attributeUploads"], 2)
            self.assertEqual(case["rawModelBefore"]["orderUploads"], 2)
            self.assertEqual(case["rawModelAfter"]["orderUploads"], 2)
            self.assertEqual(case["uploadDeltas"], {"attributeUploads": 0, "shUploads": 0, "orderUploads": 0})

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

    def test_partial_millisecond_settling_remains_rejected(self):
        case = self.receipt()["cases"][0]
        case["idle"][0]["beginNs"] = case["events"][-1]["presentNs"] + 599_999_999
        self.assert_failed("idle did not actually settle before observation")

    def test_partial_millisecond_idle_window_remains_rejected(self):
        window = self.receipt(group="medium-dpr1")["cases"][0]["idle"][0]
        window["endNs"] = window["beginNs"] + 999_999_999
        self.assert_failed("idle windows are too short or overlap")

    def test_composite_identity_must_match_external_source_receipt(self):
        self.receipt()["sourceIdentity"]["workingTreeIsClean"] = False
        self.assert_failed("process composite identity does not match independent external build receipt")

    def test_actual_common_qa_bytes_cannot_be_a_producer_label(self):
        (self.source_dirs["B"] / "native/src/UiPerformanceSmokeTest.cpp").write_text("// different QA\n", encoding="utf-8")
        self.assert_failed("actual common observer/helper bytes differ")

    def test_baseline_production_viewport_must_remain_untouched_by_overlay(self):
        (self.source_dirs["A"] / "native/src/NativeViewport.cpp").write_text("// changed production\n", encoding="utf-8")
        self.assert_failed("baseline production viewport/render bytes changed")

    def test_recorded_cannot_claim_true_exposure_when_ancestor_clips(self):
        self.receipt()["exposure"]["ancestors"][0]["containsViewport"] = False
        self.assert_failed("RECORDED cannot fake failed exposure predicates")

    def test_unexposed_dpr2_keeps_diagnostics_and_is_inconclusive(self):
        report = self.receipt(group="empty-dpr2")
        report["status"] = "INCONCLUSIVE"
        report["unready"] = ["fixed_viewport_not_fully_exposed"]
        report["environment"] = {}
        report["cases"] = []
        report["observation"]["samples"] = report["observation"]["samples"][:1]
        report["observation"]["count"] = 1
        report["exposure"]["fullyExposed"] = False
        report["exposure"]["ancestors"][0]["containsViewport"] = False
        for entry in self.manifest["processes"]:
            if entry["group"] == "empty-dpr2" and entry["round"] == 1 and entry["arm"] == "B":
                entry["exitCode"] = 3
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (3, "INCONCLUSIVE"), result)
        observed = next(item for item in result["rawSummaries"] if
                        item["group"] == "empty-dpr2" and item["round"] == 1 and item["arm"] == "B")
        self.assertFalse(observed["exposure"]["ancestors"][0]["containsViewport"])

    def test_independent_lifetime_upload_offsets_do_not_change_model_identity(self):
        report = self.receipt(round_index=2, arm="B")
        for case in report["cases"]:
            if case["id"] not in ("3dgs", "2dgs"):
                continue
            for boundary in ("modelBefore", "modelAfter"):
                case[boundary]["attributeUploads"] += 25
                case[boundary]["shUploads"] += 7
                case[boundary]["orderUploads"] += 30
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (0, "PASS"), result)
        process = next(item for item in result["rawSummaries"] if
                       item["group"] == "light-dpr1" and item["round"] == 2 and item["arm"] == "B")
        case = process["cases"]["3dgs"]
        self.assertEqual(case["rawModelBefore"]["attributeUploads"], 26)
        self.assertEqual(case["rawModelAfter"]["attributeUploads"], 26)
        self.assertEqual(case["uploadDeltas"], {"attributeUploads": 0, "shUploads": 0, "orderUploads": 0})
        self.assertNotIn("attributeUploads", case["model"])
        self.assertTrue(result["measurementSemantics"]["sameProcessRawModelBeforeEqualsAfterRequired"])

    def test_attribute_upload_during_measured_pan_is_still_rejected(self):
        self.receipt()["cases"][2]["modelAfter"]["attributeUploads"] += 1
        self.assert_failed("pan changed transforms, model fidelity or resident uploads")

    def test_sh_upload_during_measured_pan_is_still_rejected(self):
        self.receipt()["cases"][2]["modelAfter"]["shUploads"] += 1
        self.assert_failed("pan changed transforms, model fidelity or resident uploads")

    def test_order_upload_during_unchanged_direction_pan_is_still_rejected(self):
        self.receipt()["cases"][2]["modelAfter"]["orderUploads"] += 1
        self.assert_failed("pan changed transforms, model fidelity or resident uploads")

    def test_upload_offset_separation_does_not_hide_gaussian_decimation(self):
        case = self.receipt()["cases"][2]
        case["modelBefore"]["renderedPointCount"] = 440
        case["modelAfter"]["renderedPointCount"] = 440
        self.assert_failed("no Gaussian decimation")


if __name__ == "__main__":
    unittest.main()
