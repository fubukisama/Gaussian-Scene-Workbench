#!/usr/bin/env python3
"""Remote-only public CLI tests. Synthetic PNG/JSON receipts are not UI results."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
CHECKER = REPO / "scripts/check_native_ui_surface_evidence.py"
INVENTORY = REPO / "docs/ui_upgrade_20261028/SURFACE_INVENTORY.json"
SOURCE = "a" * 40
LOCALES = ("zh_CN", "en_US", "ja_JP")
THEMES = ("light", "dark")
DPRS = ("1", "1.25", "1.5", "1.75", "2")
FORMS_TESTS = [
    "native_language_zh_CN", "native_language_en_US", "native_language_ja_JP",
    "native_task_actions_zh_CN", "native_task_actions_en_US", "native_task_actions_ja_JP",
    "native_generation_history", "native_resource_budget_ui", "native_quality_comparison_ui",
    "native_quality_measurement_ui", "native_scale_constraints_ui", "native_training_batch_ui",
    "native_viewport_scheduling_light", "native_viewport_scheduling_dark",
    "native_runtime_ui_surfaces", "native_initialization_ui_surfaces"]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, allow_nan=False), encoding="utf-8")


def unqueried_font_fact(font):
    font.update(exactMatch=None, exactMatchQuery="not_queried_qt683_multifamily_crash",
                requestedFamily="Contract font", requestedFamilies=["Contract font", "Contract CJK fallback"],
                requestedPointSize=10.0, requestedPixelSize=-1, weight=400, deviceLogicalDpi=[96, 96])


class SurfaceEvidenceCliContracts(unittest.TestCase):
    def setUp(self):
        if os.environ.get("GITHUB_ACTIONS") != "true":
            raise RuntimeError("Remote-only contracts; do not run automated tests locally.")
        base = REPO / "native/.deps"
        base.mkdir(exist_ok=True, parents=True)
        temporary = tempfile.TemporaryDirectory(prefix="ui-surface-cli-contract-", dir=base)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.next_process = 100
        self.plan = [{"id": "style-contracts", "dpr": "1", "tests": ["app_theme", "ui_visual_roles"]}]
        self.plan += [{"id": "icons-d" + dpr, "dpr": dpr,
                       "tests": ["native_icon_ui_" + locale for locale in LOCALES]} for dpr in DPRS]
        self.manifest = {"schema": "gsw.ui_surface_run/1", "sourceCommit": SOURCE, "sourceTree": "b" * 40,
                         "runId": "owned-contract-run", "scope": "foundation", "fullIconDprMatrix": True,
                         "status": "FINISHED", "batches": [], "plannedBatches": self.plan, "iconDprs": list(DPRS)}
        for batch in self.plan:
            self.add_batch(batch)
        for dpr in DPRS:
            for locale in LOCALES:
                for theme in THEMES:
                    for scale in (90, 100, 150):
                        for suffix in ("-icon-atlas.png", "-window.png"):
                            path = self.root / f"icons-d{dpr}/original-png/{locale}-{theme}-{scale}{suffix}"
                            path.parent.mkdir(exist_ok=True, parents=True)
                            Image.new("RGB", (32, 32), (80, 80, 80)).save(path)
                self.add_native_report("icons-d" + dpr,
                    [("MainWindow", "main-window", locale, theme, 640, float(dpr)) for theme in THEMES])

    def add_batch(self, batch):
        directory = self.root / batch["id"]
        directory.mkdir(exist_ok=True)
        suite = ET.Element("testsuite", tests=str(len(batch["tests"])), failures="0")
        for name in batch["tests"]:
            ET.SubElement(suite, "testcase", name=name, status="run", time="0.1")
        ET.ElementTree(suite).write(directory / "ctest.xml", encoding="utf-8", xml_declaration=True)
        self.manifest["batches"].append({"id": batch["id"], "dpr": batch["dpr"], "expectedTests": batch["tests"],
            "exitCode": 0, "junit": batch["id"] + "/ctest.xml", "log": batch["id"] + "/ctest.log"})

    def add_native_report(self, prefix, cases):
        self.next_process += 1
        directory = self.root / prefix / "surfaces" / ("process-" + str(self.next_process))
        directory.mkdir(parents=True)
        rows = []
        for index, (class_name, role, locale, theme, width, dpr) in enumerate(cases, 1):
            name = f"surface-{index:04d}.png"
            path = directory / name
            physical = [round(width * dpr), round(360 * dpr)]
            Image.new("RGB", tuple(physical), (80, 80, 80)).save(path)
            rows.append({"status": "captured", "reason": "stable-owned-surface", "class": "gsw::" + class_name,
                "typedRole": role, "locale": locale, "theme": theme, "logicalSize": [width, 360], "widgetDpr": dpr,
                "uiScalePercent": 100, "dimensionsMatchWidgetDpr": True, "image": name, "sha256": sha(path),
                "physicalImageSize": physical, "captureMethod": "QScreen::grabWindow(owned existing native client id)",
                "occlusionCheck": "Win32-z-order-client-rect", "nativeWindowId": hex(self.next_process)[2:],
                "pixmapDpr": dpr, "nativeWindowDpr": dpr, "screenDpr": dpr, "screenLogicalDpi": 96.0,
                "actualWindowsWindowDpi": 96, "fontRecordsMayBeTruncated": False,
                "visibleWidgetFontResolution": [{"fontRoleScope": "QWidget-default-not-custom-painter",
                    "resolvedFamily": "Contract font", "pointSize": 10.0, "pixelSize": 13,
                    "enabled": True, "exactMatch": True, "logicalRect": [0, 0, width, 360],
                    "mixedScriptGlyphRuns": [{"family": "Contract font", "glyphCount": 17, "missingGlyphCount": 0}]}]})
        report = {"schema": "gsw.ui_surface_evidence/1", "sourceCommit": SOURCE, "platform": "windows",
            "smoke": "--smoke-test-ui-forms", "finished": True, "stoppedByLimit": False, "imageCount": len(rows),
            "imagesDoNotReplacePublicBehaviorAssertions": True, "customPainterFontRolesNotInWidgetDefaultProbe": True,
            "nativeOsDialogsWithoutOwnedQWidgetNotObserved": True, "surfaces": rows}
        write_json(directory / "ui-surfaces.json", report)
        return directory / "ui-surfaces.json"

    def forms(self, complete=False):
        self.manifest["scope"] = "forms"
        batch = {"id": "forms-core", "dpr": "1", "tests": FORMS_TESTS}
        self.plan.append(batch)
        self.add_batch(batch)
        if complete:
            inventory = json.loads(INVENTORY.read_text(encoding="utf-8-sig"))
            for item in inventory["surfaces"]:
                if item["id"].startswith("DLG-"):
                    self.add_native_report("forms-core", [
                        (item["name"], "dialog", locale, theme, width, 1.0)
                        for locale in LOCALES for theme in THEMES for width in (640, 800)])

    def first_report(self):
        return sorted(self.root.rglob("ui-surfaces.json"))[0]

    def change_report(self, callback):
        path = self.first_report()
        value = json.loads(path.read_text(encoding="utf-8"))
        callback(value)
        write_json(path, value)

    def evaluate(self, index_callback=None):
        write_json(self.root / "source-run.json", self.manifest)
        index = {"schema": "gsw.ui_surface_png_index/1", "sourceCommit": SOURCE,
                 "images": [{"path": path.relative_to(self.root).as_posix(), "sha256": sha(path)}
                            for path in sorted(self.root.rglob("*.png"))]}
        if index_callback:
            index_callback(index)
        write_json(self.root / "png-index.json", index)
        output = self.root / "results.json"
        process = subprocess.run([sys.executable, "-I", "-B", str(CHECKER), "--root", str(self.root),
                                  "--source-commit", SOURCE, "--inventory", str(INVENTORY), "--output", str(output)],
                                 capture_output=True, text=True, timeout=45, check=False)
        self.assertTrue(output.is_file(), process.stdout + process.stderr)
        return process.returncode, json.loads(output.read_text(encoding="utf-8"))

    def assert_failed(self, fragment, index_callback=None):
        code, result = self.evaluate(index_callback)
        self.assertEqual((code, result["status"]), (2, "FAIL"), result)
        self.assertIn(fragment, "\n".join(result["errors"]))

    def test_foundation_does_not_claim_unobserved_dialogs_or_visual_review_passed(self):
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (0, "EVIDENCE_COMPLETE"), result)
        self.assertEqual(result["fullSixteenDialogCoverage"], "PARTIAL_OR_NOT_RUN")
        self.assertEqual(result["visualAcceptance"], "NOT_RUN")
        self.assertEqual(result["nativeOsProvider"]["status"], "UNSUPPORTED")
        self.assertEqual(len(result["dialogCoverage"]), 16)

    def test_forms_missing_all_dialog_classes_stays_partial(self):
        self.forms()
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (3, "PARTIAL"), result)
        self.assertTrue(all(item["status"] == "NOT_RUN" for item in result["dialogCoverage"]))

    def test_all_declared_form_variants_are_evidence_complete_not_visual_acceptance(self):
        self.forms(complete=True)
        code, result = self.evaluate()
        self.assertEqual((code, result["fullSixteenDialogCoverage"]), (0, "EVIDENCE_COMPLETE"), result)
        self.assertEqual(result["visualAcceptance"], "NOT_RUN")

    def test_source_mismatch_is_rejected(self):
        self.change_report(lambda report: report.update(sourceCommit="c" * 40))
        self.assert_failed("actual surface source/schema mismatch")

    def test_native_image_hash_mismatch_is_rejected(self):
        self.change_report(lambda report: report["surfaces"][0].update(sha256="0" * 64))
        self.assert_failed("native metadata/image SHA mismatch")

    def test_native_image_dimensions_are_independently_checked(self):
        self.change_report(lambda report: report["surfaces"][0].update(physicalImageSize=[1, 1]))
        self.assert_failed("native PNG dimensions mismatch")

    def test_missing_fixed_probe_glyph_is_rejected(self):
        self.change_report(lambda report: report["surfaces"][0]["visibleWidgetFontResolution"][0]["mixedScriptGlyphRuns"][0].update(missingGlyphCount=1))
        self.assert_failed("fixed probe contains missing glyphs")

    def test_wrong_probe_glyph_count_is_rejected(self):
        self.change_report(lambda report: report["surfaces"][0]["visibleWidgetFontResolution"][0]["mixedScriptGlyphRuns"][0].update(glyphCount=16))
        self.assert_failed("glyph count does not match 17")

    def test_raw_png_omission_is_not_ignored(self):
        self.assert_failed("raw PNG inventory omitted", lambda index: index["images"].pop())

    def test_original_ctest_skip_is_not_a_pass(self):
        xml_path = self.root / "style-contracts/ctest.xml"
        tree = ET.parse(xml_path)
        ET.SubElement(next(tree.getroot().iter("testcase")), "skipped")
        tree.write(xml_path, encoding="utf-8", xml_declaration=True)
        self.assert_failed("original CTest behavior failed/skipped")

    def test_capture_path_cannot_escape_owned_directory(self):
        self.change_report(lambda report: report["surfaces"][0].update(image="../outside.png"))
        self.assert_failed("owned surface image basename required")

    def test_capture_limit_remains_partial_not_green(self):
        self.change_report(lambda report: report.update(stoppedByLimit=True))
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (3, "PARTIAL"), result)
        self.assertIn("bounded limit reached", "\n".join(result["missing"]))

    def test_unqueried_exact_match_preserves_fallback_and_other_font_facts(self):
        self.change_report(lambda report: [
            unqueried_font_fact(font) for row in report["surfaces"] for font in row["visibleWidgetFontResolution"]])
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (0, "EVIDENCE_COMPLETE"), result)
        self.assertEqual(result["visualAcceptance"], "NOT_RUN")
        self.assertEqual(result["nativeCaptureCount"], 30)

    def test_null_exact_match_without_precise_reason_is_rejected(self):
        def change(report):
            font = report["surfaces"][0]["visibleWidgetFontResolution"][0]
            unqueried_font_fact(font)
            font.pop("exactMatchQuery")
        self.change_report(change)
        self.assert_failed("null exactMatch requires the precise unsafe-query reason")

    def test_unknown_exact_match_query_reason_is_rejected(self):
        def change(report):
            font = report["surfaces"][0]["visibleWidgetFontResolution"][0]
            unqueried_font_fact(font)
            font["exactMatchQuery"] = "unknown_or_ignored"
        self.change_report(change)
        self.assert_failed("null exactMatch requires the precise unsafe-query reason")

    def test_unqueried_exact_match_cannot_be_fabricated_true(self):
        def change(report):
            font = report["surfaces"][0]["visibleWidgetFontResolution"][0]
            unqueried_font_fact(font)
            font["exactMatch"] = True
        self.change_report(change)
        self.assert_failed("legacy exactMatch boolean cannot claim an unqueried predicate")

    def test_unqueried_font_still_requires_requested_fallback_families(self):
        def change(report):
            font = report["surfaces"][0]["visibleWidgetFontResolution"][0]
            unqueried_font_fact(font)
            font.pop("requestedFamilies")
        self.change_report(change)
        self.assert_failed("unqueried exactMatch must preserve requested fallback families")

    def test_unqueried_font_does_not_bypass_missing_glyph_failure(self):
        def change(report):
            font = report["surfaces"][0]["visibleWidgetFontResolution"][0]
            unqueried_font_fact(font)
            font["mixedScriptGlyphRuns"][0]["missingGlyphCount"] = 1
        self.change_report(change)
        self.assert_failed("fixed probe contains missing glyphs")

    def test_behavior_failure_retains_all_images_and_sixteen_dialog_matrix(self):
        self.forms(complete=True)
        path = self.root / "style-contracts/ctest.xml"
        tree = ET.parse(path)
        ET.SubElement(next(tree.getroot().iter("testcase")), "skipped")
        tree.write(path, encoding="utf-8", xml_declaration=True)
        code, result = self.evaluate()
        self.assertEqual((code, result["status"], result["behaviorStatus"]), (2, "FAIL", "FAIL"), result)
        self.assertEqual(result["integrityStatus"], "VALID")
        self.assertEqual(result["rawPngCount"], 402)
        self.assertEqual(result["nativeCaptureCount"], 222)
        self.assertEqual(len(result["originalTests"]), 33)
        self.assertEqual(len(result["dialogCoverage"]), 16)
        self.assertTrue(all(item["status"] == "EVIDENCE_COMPLETE" for item in result["dialogCoverage"]))
        self.assertEqual(result["visualAcceptance"], "NOT_RUN")

    def test_bad_native_row_does_not_suppress_later_valid_row_in_same_report(self):
        self.change_report(lambda report: report["surfaces"][0]["visibleWidgetFontResolution"][0]["mixedScriptGlyphRuns"][0].update(missingGlyphCount=1))
        code, result = self.evaluate()
        self.assertEqual((code, result["status"]), (2, "FAIL"), result)
        self.assertEqual(result["rawPngCount"], 210)
        self.assertEqual(result["nativeCaptureCount"], 29)
        self.assertEqual(len(result["dialogCoverage"]), 16)

    def test_behavior_failure_does_not_hide_independent_bad_image_evidence(self):
        path = self.root / "style-contracts/ctest.xml"
        tree = ET.parse(path)
        ET.SubElement(next(tree.getroot().iter("testcase")), "failure")
        tree.write(path, encoding="utf-8", xml_declaration=True)
        self.change_report(lambda report: report["surfaces"][0].update(sha256="0" * 64))
        code, result = self.evaluate()
        self.assertEqual((code, result["status"], result["integrityStatus"], result["behaviorStatus"]),
                         (2, "FAIL", "FAIL", "FAIL"), result)
        messages = "\n".join(result["errors"])
        self.assertIn("original CTest behavior failed/skipped", messages)
        self.assertIn("native metadata/image SHA mismatch", messages)
        self.assertEqual(result["rawPngCount"], 210)
        self.assertEqual(result["nativeCaptureCount"], 29)
        self.assertEqual(len(result["dialogCoverage"]), 16)


if __name__ == "__main__":
    unittest.main()
