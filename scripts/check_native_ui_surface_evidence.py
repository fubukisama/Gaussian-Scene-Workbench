#!/usr/bin/env python3
"""Independent owned native surface evidence checker; never starts an app.

Exit 0: selected evidence scope complete; 2: bytes/metadata or behavior failed;
3: coverage partial. Captured images are not a visual-quality acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any
import xml.etree.ElementTree as ET

from PIL import Image

LOCALES = ("zh_CN", "en_US", "ja_JP")
THEMES = ("light", "dark")
DPRS = ("1", "1.25", "1.5", "1.75", "2")
WIDTHS = ("narrow", "regular")
PROBE_GLYPHS = 17  # Adapter fixed probe: Latin 中文 日本語 0123.
NARROW_MAXIMUM = 720
PIXEL_LIMIT = 16 * 1024 * 1024
MACHINE_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_:.-]{0,95}$")


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def read_json(path: Path) -> Any:
    require(path.stat().st_size <= 32 * 1024 * 1024, "metadata file exceeds bound")
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError("nonfinite JSON: " + value)))


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def integer(value: Any, name: str, minimum=0) -> int:
    require(type(value) is int and value >= minimum, name + ": integer required")
    return value


def number(value: Any, name: str, minimum=0.0) -> float:
    require(type(value) in (int, float) and math.isfinite(value) and value >= minimum,
            name + ": finite number required")
    return float(value)


def hash_value(value: Any, name: str, length=64) -> str:
    require(isinstance(value, str) and re.fullmatch("[0-9a-f]{" + str(length) + "}", value) is not None,
            name + ": lowercase digest required")
    return value


def no_links(path: Path, root: Path) -> None:
    current = path
    while True:
        require(not current.is_symlink() and not (hasattr(current, "is_junction") and current.is_junction()),
                "linked evidence path refused: " + str(path))
        if current == root:
            break
        require(current.parent != current, "evidence escaped owned root")
        current = current.parent


def owned_path(root: Path, relative: Any) -> Path:
    require(isinstance(relative, str) and relative and "\\" not in relative, "portable relative evidence path required")
    value = PurePosixPath(relative)
    require(not value.is_absolute() and not any(part in (".", "..") for part in value.parts) and ":" not in relative,
            "evidence path traversal refused")
    result = root.joinpath(*value.parts)
    require(result.is_relative_to(root), "evidence escaped root")
    no_links(result, root)
    return result


def expected_batches(scope: str, icon_dprs: list[str]) -> list[dict[str, Any]]:
    batches = [{"id": "style-contracts", "dpr": "1", "tests": ["app_theme", "ui_visual_roles"]}]
    batches += [{"id": "icons-d" + dpr, "dpr": dpr,
                 "tests": ["native_icon_ui_" + locale for locale in LOCALES]} for dpr in icon_dprs]
    if scope == "forms":
        batches.append({"id": "forms-core", "dpr": "1", "tests": [
            "native_language_zh_CN", "native_language_en_US", "native_language_ja_JP",
            "native_task_actions_zh_CN", "native_task_actions_en_US", "native_task_actions_ja_JP",
            "native_generation_history", "native_resource_budget_ui", "native_quality_comparison_ui",
            "native_quality_measurement_ui", "native_scale_constraints_ui", "native_training_batch_ui",
            "native_viewport_scheduling_light", "native_viewport_scheduling_dark",
            "native_runtime_ui_surfaces", "native_initialization_ui_surfaces"]})
    return batches


def inventory_dialogs(path: Path) -> list[dict[str, str]]:
    inventory = read_json(path)
    dialogs = [{"id": item["id"], "class": item["name"]} for item in inventory["surfaces"]
               if re.fullmatch(r"DLG-[0-9]{2}", item.get("id", ""))]
    require(len(dialogs) == 16 and len({item["class"] for item in dialogs}) == 16,
            "inventory must enumerate sixteen distinct dialog classes")
    require([item["id"] for item in dialogs] == [f"DLG-{index:02d}" for index in range(1, 17)],
            "dialog inventory missing or reordered")
    return dialogs


def glyph_records(records: Any) -> tuple[int, set[str]]:
    require(isinstance(records, list) and 0 < len(records) <= 256, "visible widget font records missing/unbounded")
    families: set[str] = set()
    for item in records:
        require(item.get("fontRoleScope") == "QWidget-default-not-custom-painter", "font scope must remain explicit")
        require(isinstance(item.get("resolvedFamily"), str) and bool(item["resolvedFamily"]), "actual resolved font family missing")
        families.add(item["resolvedFamily"])
        require(type(item.get("enabled")) is bool and type(item.get("exactMatch")) is bool, "font state facts missing")
        point_size, pixel_size = item.get("pointSize"), item.get("pixelSize")
        require((type(point_size) in (int, float) and math.isfinite(point_size) and point_size > 0) or
                (type(pixel_size) is int and pixel_size > 0), "actual font size missing")
        rectangle = item.get("logicalRect")
        require(isinstance(rectangle, list) and len(rectangle) == 4 and
                all(type(value) is int for value in rectangle) and rectangle[2] >= 0 and rectangle[3] >= 0,
                "widget font logical rectangle missing")
        runs = item.get("mixedScriptGlyphRuns")
        require(isinstance(runs, list) and bool(runs), "actual mixed-script glyph runs missing")
        total = 0
        for run in runs:
            require(isinstance(run.get("family"), str) and bool(run["family"]), "glyph run font family missing")
            total += integer(run.get("glyphCount"), "glyphCount", 1)
            require(integer(run.get("missingGlyphCount"), "missingGlyphCount") == 0, "fixed probe contains missing glyphs")
        require(total == PROBE_GLYPHS, "fixed probe glyph count does not match 17 codepoints")
    return len(records), families


def width_bucket(record: dict[str, Any]) -> str | None:
    value = record.get("logicalSize")
    if isinstance(value, list) and len(value) == 2 and type(value[0]) is int and value[0] > 0:
        return "narrow" if value[0] <= NARROW_MAXIMUM else "regular"
    return None


def evaluate(root: Path, source_commit: str, inventory_path: Path) -> dict[str, Any]:
    errors, missing, skipped, images, captured, dialog_results, original_tests = [], [], [], [], [], [], []
    scope = "NOT_RUN"
    report_count = font_total = truncated_fonts = 0
    try:
        require(root.is_absolute() and root.is_dir(), "existing absolute owned evidence root required")
        no_links(root, root)
        hash_value(source_commit, "requested source commit", 40)
        manifest = read_json(root / "source-run.json")
        require(manifest.get("schema") == "gsw.ui_surface_run/1" and manifest.get("sourceCommit") == source_commit,
                "surface run source/schema mismatch")
        hash_value(manifest.get("sourceTree"), "source tree", 40)
        require(isinstance(manifest.get("runId"), str) and bool(manifest["runId"]), "run identity missing")
        scope = manifest.get("scope")
        require(scope in ("foundation", "forms") and type(manifest.get("fullIconDprMatrix")) is bool, "predeclared scope missing")
        requested_dprs = list(DPRS) if manifest["fullIconDprMatrix"] else ["1"]
        plan = expected_batches(scope, requested_dprs)
        if manifest.get("status") != "FINISHED":
            missing.append("original CTest execution did not finish; partial evidence only")
        if "plannedBatches" not in manifest:
            missing.append("CTest phase NOT_RUN")
        else:
            require(manifest["plannedBatches"] == plan and manifest.get("iconDprs") == requested_dprs,
                    "predeclared CTest/DPR plan changed")
        batches = manifest.get("batches")
        require(isinstance(batches, list), "original CTest receipts missing")
        require(len(batches) <= len(plan) and [b.get("id") for b in batches] == [p["id"] for p in plan[:len(batches)]],
                "duplicate/reordered/unexpected CTest batches")
        for intended in plan:
            batch = next((item for item in batches if item["id"] == intended["id"]), None)
            if not batch:
                missing.append("original behavior batch NOT_RUN: " + intended["id"])
                continue
            require(batch.get("expectedTests") == intended["tests"] and batch.get("dpr") == intended["dpr"], "CTest expected set changed")
            xml_path = owned_path(root, batch.get("junit"))
            require(xml_path.is_file() and xml_path.stat().st_size <= 16 * 1024 * 1024, "CTest XML missing/unbounded")
            tests = list(ET.parse(xml_path).getroot().iter("testcase"))
            require(len(tests) == len(intended["tests"]) and sorted(t.get("name") for t in tests) == sorted(intended["tests"]),
                    "missing/duplicate original CTest assertion boundary")
            failed = []
            for test in tests:
                status = "FAIL" if test.find("failure") is not None or test.find("error") is not None else (
                    "SKIP" if test.find("skipped") is not None or test.get("status") == "notrun" else "PASS")
                original_tests.append({"batch": intended["id"], "name": test.get("name"), "status": status})
                if status != "PASS":
                    failed.append(test.get("name", "unknown") + ":" + status)
            require(batch.get("exitCode") == 0 and not failed, "original CTest behavior failed/skipped: " + ",".join(failed))
        index = read_json(root / "png-index.json")
        require(index.get("schema") == "gsw.ui_surface_png_index/1" and index.get("sourceCommit") == source_commit,
                "PNG index source/schema mismatch")
        indexed = {}
        for item in index["images"]:
            path = owned_path(root, item.get("path"))
            require(path.suffix.lower() == ".png" and path.is_file() and path not in indexed, "missing/duplicate PNG index entry")
            actual_hash = digest(path)
            require(actual_hash == hash_value(item.get("sha256"), "PNG index SHA"), "indexed PNG byte hash mismatch")
            with Image.open(path) as png:
                require(png.format == "PNG", "indexed image is not PNG")
                physical = list(png.size)
                require(0 < physical[0] * physical[1] <= PIXEL_LIMIT, "PNG physical pixel bound exceeded")
                png.verify()
            entry = {"path": item["path"], "sha256": actual_hash, "physicalSize": physical,
                     "provenance": "test-owned raw PNG; not a native capture unless bound below"}
            indexed[path] = entry
            images.append(entry)
        require({path for path in root.rglob("*.png")} == set(indexed), "raw PNG inventory omitted/added files")
        for path in sorted(root.rglob("ui-surfaces.json")):
            no_links(path, root)
            relative = path.relative_to(root).as_posix()
            report_count += 1
            try:
                report = read_json(path)
                require(report.get("schema") == "gsw.ui_surface_evidence/1" and report.get("sourceCommit") == source_commit,
                        "actual surface source/schema mismatch")
                require(report.get("platform") == "windows" and isinstance(report.get("smoke"), str) and
                        re.fullmatch(r"--smoke-test(?:-[a-z0-9-]+)?", report["smoke"]) is not None,
                        "real isolated Windows smoke adapter required")
                require(report.get("imagesDoNotReplacePublicBehaviorAssertions") is True and
                        report.get("customPainterFontRolesNotInWidgetDefaultProbe") is True and
                        report.get("nativeOsDialogsWithoutOwnedQWidgetNotObserved") is True,
                        "evidence coverage limitations cannot be removed")
                rows = report.get("surfaces")
                require(isinstance(rows, list) and len(rows) <= 512, "bounded surface observations required")
                count = sum(row.get("status") == "captured" for row in rows)
                require(report.get("imageCount") == count and count <= 160, "capture count inconsistent")
                if report.get("finished") is not True or report.get("stoppedByLimit") is not False:
                    missing.append(relative + ": capture unfinished or bounded limit reached")
                for row in rows:
                    require(row.get("status") in ("captured", "skipped", "failed"), "unknown evidence status")
                    if row["status"] == "failed":
                        raise EvidenceError("native capture failed: " + str(row.get("reason")))
                    actual_class = row.get("class", "")
                    require(isinstance(actual_class, str) and MACHINE_NAME.fullmatch(actual_class), "actual Qt class identity missing")
                    if row["status"] == "skipped":
                        skipped.append({"report": relative, **{key: row.get(key) for key in
                            ("class", "typedRole", "locale", "theme", "logicalSize", "widgetDpr", "reason", "status")}})
                        continue
                    require(row.get("locale") in LOCALES and row.get("theme") in THEMES, "surface locale/theme missing")
                    logical = row.get("logicalSize")
                    require(isinstance(logical, list) and len(logical) == 2 and all(type(n) is int and n > 0 for n in logical),
                            "surface logical dimensions missing")
                    dpr = number(row.get("widgetDpr"), "actual widget DPR", .1)
                    require(dpr <= 8 and row.get("dimensionsMatchWidgetDpr") is True, "dimensions do not match actual DPR")
                    name = row.get("image")
                    require(isinstance(name, str) and re.fullmatch(r"surface-[0-9]{4}\.png", name), "owned surface image basename required")
                    image_path = path.parent / name
                    require(image_path in indexed, "native capture PNG missing from byte index")
                    bound = indexed[image_path]
                    require(row.get("sha256") == bound["sha256"], "native metadata/image SHA mismatch")
                    require(row.get("physicalImageSize") == bound["physicalSize"] and
                            all(abs(actual - expected * dpr) <= 1 for actual, expected in zip(bound["physicalSize"], logical)),
                            "native PNG dimensions mismatch metadata/widget DPR")
                    require(row.get("captureMethod") == "QScreen::grabWindow(owned existing native client id)" and
                            row.get("occlusionCheck") == "Win32-z-order-client-rect" and
                            isinstance(row.get("nativeWindowId"), str) and int(row["nativeWindowId"], 16) > 0,
                            "native client capture provenance missing")
                    for field in ("pixmapDpr", "nativeWindowDpr", "screenDpr", "screenLogicalDpi"):
                        number(row.get(field), field, .1)
                    integer(row.get("actualWindowsWindowDpi"), "actualWindowsWindowDpi", 1)
                    integer(row.get("uiScalePercent"), "uiScalePercent", 1)
                    font_count, families = glyph_records(row.get("visibleWidgetFontResolution"))
                    font_total += font_count
                    require(type(row.get("fontRecordsMayBeTruncated")) is bool, "font truncation fact missing")
                    truncated_fonts += row["fontRecordsMayBeTruncated"]
                    if row["fontRecordsMayBeTruncated"]:
                        missing.append(relative + "/" + name + ": widget font list reached cap")
                    captured.append({"report": relative, "image": bound["path"], "class": actual_class.split("::")[-1],
                        "actualQtClass": actual_class, "typedRole": row.get("typedRole"), "locale": row["locale"], "theme": row["theme"],
                        "logicalSize": logical, "widgetDpr": dpr, "uiScalePercent": row["uiScalePercent"],
                        "widthBucket": width_bucket(row), "fontRecords": font_count, "resolvedFamilies": sorted(families)})
                    bound["provenance"] = "source-bound owned native client with fixed-probe font facts"
            except (EvidenceError, OSError, ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
                errors.append(relative + ": " + str(error))
        if not report_count:
            missing.append("actual native ui-surfaces.json reports NOT_RUN")
        # Five DPR groups are the declared foundation. A core-only execution is
        # retained but partial; an atlas never substitutes for a native shell.
        for dpr in DPRS:
            for locale in LOCALES:
                for theme in THEMES:
                    for scale in (90, 100, 150):
                        prefix = f"icons-d{dpr}/original-png/{locale}-{theme}-{scale}"
                        for suffix in ("-icon-atlas.png", "-window.png"):
                            if owned_path(root, prefix + suffix) not in indexed:
                                missing.append(prefix + suffix + ": NOT_RUN")
                    native = [item for item in captured if item["report"].startswith(f"icons-d{dpr}/") and
                              item["typedRole"] == "main-window" and item["locale"] == locale and
                              item["theme"] == theme and item["widgetDpr"] == float(dpr)]
                    if not native:
                        missing.append(f"actual shell {locale}/{theme}/DPR{dpr}: NOT_RUN")
        for dialog in inventory_dialogs(inventory_path):
            rows = [item for item in captured if item["class"] == dialog["class"] and item["typedRole"] == "dialog"]
            cells = []
            for locale in LOCALES:
                for theme in THEMES:
                    for width in WIDTHS:
                        found = [item for item in rows if item["locale"] == locale and item["theme"] == theme and
                                 item["widthBucket"] == width and item["widgetDpr"] == 1.0]
                        skip = [item for item in skipped if item["class"].split("::")[-1] == dialog["class"] and
                                item.get("locale") == locale and item.get("theme") == theme and width_bucket(item) == width]
                        cells.append({"locale": locale, "theme": theme, "widthBucket": width, "widgetDpr": 1.0,
                            "status": "CAPTURED" if found else "SKIP" if skip else "NOT_RUN",
                            "images": [item["image"] for item in found], "skipReasons": sorted({str(item.get("reason")) for item in skip})})
            missing_cells = [item for item in cells if item["status"] != "CAPTURED"]
            dialog_results.append({**dialog, "status": "NOT_RUN" if not rows else "PARTIAL" if missing_cells else "EVIDENCE_COMPLETE",
                "captureCount": len(rows), "locales": sorted({item["locale"] for item in rows}),
                "themes": sorted({item["theme"] for item in rows}), "logicalWidths": sorted({item["logicalSize"][0] for item in rows}),
                "actualDprs": sorted({item["widgetDpr"] for item in rows}), "uiScales": sorted({item["uiScalePercent"] for item in rows}),
                "minimumMatrix": cells, "missingVariants": missing_cells, "visualReview": "NOT_RUN"})
            if scope == "forms" and missing_cells:
                missing.append(dialog["class"] + f": {len(missing_cells)} locale/theme/width variants SKIP/NOT_RUN")
    except (EvidenceError, OSError, ValueError, TypeError, KeyError, IndexError, AttributeError, ET.ParseError) as error:
        errors.append(str(error))
    status = "FAIL" if errors else "PARTIAL" if missing else "EVIDENCE_COMPLETE"
    return {"schema": "gsw.ui_surface_evidence_result/1", "sourceCommit": source_commit, "scope": scope,
        "status": status, "integrityStatus": "FAIL" if errors else "VALID",
        "behaviorStatus": "FAIL" if any(item["status"] != "PASS" for item in original_tests) else
                          "PASS_SELECTED_CONTRACTS" if original_tests and not any("original" in item or "CTest phase" in item for item in missing) else "NOT_RUN_OR_PARTIAL",
        "fullSixteenDialogCoverage": "EVIDENCE_COMPLETE" if len(dialog_results) == 16 and
            all(item["status"] == "EVIDENCE_COMPLETE" for item in dialog_results) else "PARTIAL_OR_NOT_RUN",
        "visualAcceptance": "NOT_RUN", "errors": errors, "missing": missing,
        "nativeReportCount": report_count, "nativeCaptureCount": len(captured), "rawPngCount": len(images),
        "widgetFontRecordCount": font_total, "fontListsAtCap": truncated_fonts,
        "dialogCoverage": dialog_results, "capturedSurfaces": captured, "skippedSurfaces": skipped,
        "originalTests": original_tests, "pngBytes": images,
        "nativeOsProvider": {"status": "UNSUPPORTED", "reason": "OS-provider dialogs without an owned QWidget cannot be observed; this does not prove they were opened or visually checked"},
        "limitations": ["fixed probe is fallback glyph availability, not actual text legibility or clipping",
            "QWidget-default font facts do not measure custom QPainter roles or viewport shaders",
            "captured combinations do not establish an unobserved Cartesian product",
            "full a11y, geometry, lifecycle, performance and installation remain separate gates"],
        "minimumFormWidthDefinition": {"narrowMaximumLogicalPixels": NARROW_MAXIMUM,
            "regularMinimumExclusiveLogicalPixels": NARROW_MAXIMUM}}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if not args.output.is_absolute() or not args.output.parent.is_dir() or args.output.exists():
        parser.error("fresh absolute output file in an existing owned evidence directory required")
    result = evaluate(args.root, args.source_commit, args.inventory)
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")
    print(json.dumps({key: result[key] for key in ("status", "scope", "integrityStatus", "behaviorStatus",
        "fullSixteenDialogCoverage", "nativeCaptureCount", "rawPngCount", "errors")}, ensure_ascii=False, allow_nan=False))
    return {"EVIDENCE_COMPLETE": 0, "FAIL": 2, "PARTIAL": 3}[result["status"]]


if __name__ == "__main__":
    sys.exit(main())
