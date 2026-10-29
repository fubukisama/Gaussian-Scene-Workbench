#!/usr/bin/env python3
"""Independent, fail-closed checker for remote native UI performance receipts.

This reads raw host/presentation observations, not the application's FPS label.
It neither starts GSW nor measures GPU milliseconds. Run contracts on remote CI.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys
from typing import Any, Callable, Protocol


FROZEN_ACCEPTANCE_SHA256 = "fd80215e7b0bcb48ed28b7c70d1bc7675ddb2e8b18d8cd034f25b5fa27f46376"
PROCESS_SCHEMA = "gsw.native_ui_performance_process/1"
MANIFEST_SCHEMA = "gsw.native_ui_performance_manifest/1"
EXPECTED_COMBINATIONS = {("empty", 1), ("empty", 2), ("light", 1), ("medium", 1)}
CASE_IDS = {"empty": ["empty"], "light": ["points", "mesh", "3dgs", "2dgs"], "medium": ["medium"]}
MEDIUM_COUNT = 512202
QA_FILES = ("native/src/UiPaintObservation.h", "native/src/UiPerformanceSmokeTest.cpp", "native/src/UiPerformanceSmokeTest.h")
QA_MAIN = "native/src/main.cpp"
UPLOAD_COUNTERS = ("attributeUploads", "shUploads", "orderUploads")


class ContractError(ValueError):
    pass


class ArchivedReceiptReader(Protocol):
    """Read-only archive adapter; never a substitute for live build validation."""

    def resolve_file(self, declared: str) -> Path: ...

    def import_original_sources(self, manifest: dict[str, Any], acceptance_sha: str,
                                plan_sha: str) -> tuple[dict[str, Any], str]: ...


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ContractError(reason)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ContractError(f"nonfinite JSON: {value}")))


def integer(value: Any, name: str, minimum: int = 0) -> int:
    require(type(value) is int and value >= minimum, f"{name}: integer >= {minimum} required")
    return value


def number(value: Any, name: str, minimum: float = 0.0) -> float:
    require(type(value) in (int, float) and math.isfinite(value) and value >= minimum,
            f"{name}: finite number >= {minimum} required")
    return float(value)


def sha(value: Any, name: str, length: int = 64) -> str:
    require(isinstance(value, str) and len(value) == length and
            all(character in "0123456789abcdef" for character in value), f"{name}: lowercase SHA required")
    return value


def vector(value: Any, length: int, name: str) -> list[float]:
    require(isinstance(value, list) and len(value) == length, f"{name}: vector size")
    for element in value:
        require(type(element) in (int, float) and math.isfinite(element), f"{name}: finite vector")
    return value


def camera(value: Any, name: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{name}: camera object required")
    vector(value.get("target"), 3, name + ".target")
    vector(value.get("orbit"), 3, name + ".orbit")
    number(value.get("distance"), name + ".distance", 1e-30)
    require(type(value.get("orthographic")) is bool, f"{name}: projection flag")
    return value


def p95(values: list[float]) -> float:
    require(len(values) >= 50, "at least 50 raw observations required for the declared p95")
    return sorted(values)[math.ceil(0.95 * len(values)) - 1]


def limit(budget: dict[str, Any], baseline: float) -> float:
    require(budget.get("rule") == "max", "frozen max budget rule required")
    relative = budget.get("relative_to_A", budget.get("relative_to_A_max"))
    return max(number(budget.get("absolute"), "budget.absolute"),
               number(relative, "budget.relative") * baseline)


def fixture_parameters(case_id: str) -> dict[str, Any]:
    gaussian = case_id in ("3dgs", "2dgs", "medium")
    return {"generator": "scheduling-seed42-v1", "kind": case_id, "seed": 42,
            "vertices": MEDIUM_COUNT if case_id == "medium" else 4 if case_id == "mesh" else 441,
            "faces": 2 if case_id == "mesh" else 0, "shDegree": 0 if gaussian else -1,
            "scales": 2 if case_id == "2dgs" else 3 if gaussian else 0}


def validate_fixture(value: Any, case_id: str, cache: dict[Path, tuple[str, int]],
                     path_resolver: Callable[[str], Path] = Path) -> dict[str, Any]:
    require(isinstance(value, dict), f"{case_id}: fixture object required")
    if case_id == "empty":
        require(value == {}, "empty scene must not substitute a model fixture")
        return value
    require(value.get("schema") == "gsw.ui_performance_fixture/1", "fixture receipt schema")
    require(value.get("parameters") == fixture_parameters(case_id), f"{case_id}: fixture parameters changed")
    declared = sha(value.get("sha256"), "fixture.sha256")
    byte_count = integer(value.get("bytes"), "fixture.bytes", 1)
    path = path_resolver(value.get("path", ""))
    require(path.is_absolute() and path.is_file() and not path.is_symlink(), "actual shared fixture required")
    resolved = path.resolve()
    if resolved not in cache:
        cache[resolved] = (digest(path), path.stat().st_size)
    require(cache[resolved] == (declared, byte_count), f"{case_id}: actual fixture bytes/hash mismatch")
    # Header/size is independent of the producer's mode/count claim. Do not
    # regenerate PRNG data with another compiler and call seed equality proof.
    with path.open("rb") as stream:
        data = stream.read(4096)
    end = data.find(b"end_header\n")
    require(end >= 0, "fixture PLY header missing")
    header = data[:end + len(b"end_header\n")]
    parameters = fixture_parameters(case_id)
    require(f"element vertex {parameters['vertices']}\n".encode() in header, "fixture actual vertex declaration")
    if case_id == "medium":
        require(b"format binary_little_endian 1.0\n" in header and
                byte_count == len(header) + MEDIUM_COUNT * 14 * 4, "medium exact binary payload length")
    else:
        require(b"format ascii 1.0\n" in header, "light fixture format")
    require((b"property float scale_2\n" in header) == (case_id in ("3dgs", "medium")),
            "actual 2DGS/3DGS scale declarations")
    require((b"element face 2\n" in header) == (case_id == "mesh"), "actual mesh face declaration")
    return {"sha256": declared, "bytes": byte_count, "parameters": parameters}


def validate_model(value: Any, case_id: str) -> dict[str, Any]:
    require(isinstance(value, dict), "model state required")
    vector(value.get("translation"), 3, "model.translation")
    vector(value.get("scale"), 3, "model.scale")
    vector(value.get("rotation"), 4, "model.rotation")
    require(value.get("selected") is False, "performance fixture must remain unselected")
    for field in ("renderedPointCount", "residentMeshTriangles", "attributeUploads", "shUploads", "orderUploads"):
        integer(value.get(field), "model." + field)
    expected_mode = {"empty": 0, "points": 0, "mesh": 1, "3dgs": 2, "2dgs": 2, "medium": 2}
    # Empty's stored default mode is not a rendering/fidelity claim.
    if case_id != "empty":
        require(value.get("renderMode") == expected_mode[case_id], "actual render mode changed")
    gaussian = case_id in ("3dgs", "2dgs", "medium")
    require(type(value.get("gaussian")) is bool and value["gaussian"] == gaussian, "actual Gaussian attributes")
    require(type(value.get("surfel")) is bool and value["surfel"] == (case_id == "2dgs"), "actual surfel attributes")
    if gaussian:
        require(value.get("indexed") is True and value.get("sourceShDegree") == 0 and value.get("effectiveShDegree") == 0,
                "indexed full source SH0 required; no fidelity reduction")
        require(value["renderedPointCount"] == fixture_parameters(case_id)["vertices"], "no Gaussian decimation")
        require(value["attributeUploads"] >= 1, "actual Gaussian attributes must have been uploaded")
    if case_id == "mesh":
        # This public counter counts paged chunks, not the nonpaged mesh VBO.
        # The actual nonpaged renderer + sceneLoaded face count are the seam.
        require(value.get("meshRenderingAvailable") is True and value.get("meshPagingSettled") is True,
                "actual mesh renderer/upload readiness")
    if case_id == "points":
        require(value["renderedPointCount"] == 441, "actual full point count")
    return value


def validate_environment(value: Any, group: dict[str, Any]) -> dict[str, Any]:
    require(isinstance(value, dict), "environment required")
    require(value.get("logicalSize") == [640, 360], "actual fixed logical viewport required")
    dpr = number(value.get("actualDpr"), "actualDpr", 0.1)
    require(dpr == group["dpr"], "actual DPR does not match process group")
    require(value.get("physicalSize") == [round(640 * dpr), round(360 * dpr)], "actual physical size does not match DPR")
    actual = value.get("glViewport")
    require(isinstance(actual, list) and len(actual) == 4 and actual[2:] == value["physicalSize"], "actual GL viewport dimensions")
    require(value.get("fullyExposed") is True, "unexposed/clipped viewport cannot establish performance")
    require(value.get("captureDuringMeasurements") is False and value.get("glQueriesDuringMeasurements") is False,
            "capture/readback/query contamination")
    for field in ("qtVersion", "qpaPlatform", "glVendor", "glRenderer", "glVersion", "fontFamily", "qtScaleFactor", "theme"):
        require(isinstance(value.get(field), str) and bool(value[field]), f"environment.{field} missing")
    require(value["qpaPlatform"] == "windows" and value["theme"] == "light", "declared Windows light-theme slice required")
    require(float(value["qtScaleFactor"]) == group["dpr"], "requested Qt scale differs from group")
    for field in ("swapInterval", "requestedSwapInterval", "samples"):
        require(type(value.get(field)) is int, f"environment.{field} required")
    number(value.get("fontPointSize"), "fontPointSize", 0.1)
    number(value.get("screenRefreshRate"), "screenRefreshRate", 0.1)
    number(value.get("screenLogicalDpi"), "screenLogicalDpi", 0.1)
    return value


def _range(value: Any, count: int, name: str) -> tuple[int, int]:
    require(isinstance(value, list) and len(value) == 2, f"{name}: range required")
    begin, end = [integer(item, name) for item in value]
    require(begin <= end <= count, f"{name}: range out of bounds")
    return begin, end


def validate_sources(manifest: dict[str, Any], acceptance_sha: str, plan_sha: str) -> tuple[dict[str, Any], str]:
    """Verify the external build/overlay receipt, not producer identity labels."""
    path = Path(manifest.get("sources", ""))
    require(path.is_absolute() and path.is_file() and not path.is_symlink(), "external composite source receipt required")
    sources = read_json(path)
    require(sources.get("schema") == "gsw.ui_performance_sources/2" and sources.get("runId") == manifest["runnerId"],
            "composite source receipt schema/runner mismatch")
    require(sources.get("acceptanceSha256") == acceptance_sha and sources.get("planSha256") == plan_sha,
            "source receipt changed the frozen acceptance or plan identity")
    arms = sources.get("arms")
    require(isinstance(arms, list) and len(arms) == 2 and {arm.get("arm") for arm in arms} == {"A", "B"},
            "two unambiguous production source arms required")
    identities = {arm["arm"]: arm for arm in arms}
    overlay_path = Path(sources.get("commonObserverOverlayReceipt", ""))
    require(overlay_path.is_absolute() and overlay_path.is_file() and not overlay_path.is_symlink(),
            "actual common QA overlay receipt required")
    overlay_sha = digest(overlay_path)
    require(overlay_sha == sha(sources.get("commonObserverOverlayIdentity"), "overlay identity"), "common QA overlay receipt hash mismatch")
    overlay = read_json(overlay_path)
    require(overlay.get("schema") == "gsw.ui_performance_common_qa_overlay/1" and
            overlay.get("sourceProductionBaseCommit") == manifest["baselineSourceCommit"] and
            overlay.get("fromCommit") == manifest["candidateSourceCommit"], "QA overlay production-base/candidate identity mismatch")
    require(overlay.get("untouchedBaselineBinary") is False and overlay.get("productionOutsideWhitelistUnchanged") is True and
            overlay.get("unlistedTrackedDiff") == [], "baseline must explicitly disclose the whitelisted dirty QA composite")
    changed = overlay.get("changedPaths")
    require(isinstance(changed, list) and QA_MAIN in changed and len(changed) == len(set(changed)) and
            set(changed).issubset(set(QA_FILES) | {QA_MAIN}), "unlisted production source changes in QA overlay")
    diff_path = overlay_path.with_name("common-qa-overlay.diff")
    require(diff_path.is_file() and digest(diff_path) == sha(overlay.get("actualDiffSha256"), "actual overlay diff SHA"),
            "actual QA overlay diff missing/hash mismatch")
    headers = re.findall(r"^diff --git a/(\S+) b/(\S+)$", diff_path.read_text(encoding="utf-8-sig"), re.M)
    require(bool(headers) and all(left == right and left in set(QA_FILES) | {QA_MAIN} for left, right in headers) and
            {left for left, _ in headers} == set(changed), "actual retained diff contains unlisted production paths")
    records = overlay.get("files")
    require(isinstance(records, list) and {record.get("path") for record in records} == set(QA_FILES) and len(records) == len(QA_FILES),
            "common observer/helper whitelist changed")
    for arm, identity in identities.items():
        base = manifest["baselineSourceCommit"] if arm == "A" else manifest["candidateSourceCommit"]
        require(identity.get("sourceCommit") == identity.get("sourceProductionBaseCommit") == base,
                "source arm must distinguish its production base identity")
        sha(identity.get("baseSourceTree"), "base source tree", 40)
        sha(identity.get("sourceTree"), "actual composite source tree", 40)
        require(identity.get("workingTreeIsClean") is (arm == "B") and
                identity.get("commonObserverOverlayApplied") is (arm == "A"), "A must be declared dirty QA composite; B is its committed source")
        require(identity.get("commonObserverOverlayIdentity") == overlay_sha and
                identity.get("commonObserverOverlayFromCommit") == manifest["candidateSourceCommit"],
                "common QA overlay provenance differs between arms")
        directory = Path(identity.get("sourceDirectory", ""))
        require(directory.is_absolute() and directory.is_dir(), "actual arm source directory required")
        executable = Path(identity.get("executable", ""))
        require(executable.is_absolute() and executable.is_file() and
                digest(executable) == sha(identity.get("executableSha256"), "build executable SHA"), "actual built arm executable mismatch")
        for record in records:
            sha(record.get("beforeSha256"), "pre-overlay file SHA")
            actual = directory / record["path"]
            require(actual.is_file() and digest(actual) == sha(record.get("afterSha256"), "common QA file SHA"),
                    "actual common observer/helper bytes differ from external receipt")
    a, b = identities["A"], identities["B"]
    require(overlay.get("baseTree") == a["baseSourceTree"] and overlay.get("compositeTree") == a["sourceTree"] and
            a["baseSourceTree"] != a["sourceTree"] and b["baseSourceTree"] == b["sourceTree"],
            "actual overlay tree must not masquerade as clean frozen baseline tree")
    patch = overlay.get("mainPatch")
    require(isinstance(patch, dict) and patch.get("path") == QA_MAIN and
            patch.get("prefixUnchanged") is True and patch.get("suffixUnchanged") is True, "main.cpp non-QA preservation proof missing")
    sha(patch.get("baselineBlob"), "frozen main blob", 40)
    sha(patch.get("beforeSha256"), "frozen main raw SHA")
    sha(patch.get("beforeBlockSha256"), "frozen main QA-block SHA")
    main_a = Path(a["sourceDirectory"]) / QA_MAIN
    main_b = Path(b["sourceDirectory"]) / QA_MAIN
    require(digest(main_a) == sha(patch.get("afterSha256"), "composite main SHA"), "actual composite main.cpp byte hash mismatch")
    pattern = re.compile(r"^  if \(parser\.isSet\(uiPerformanceOption\)\) \{\r?\n(?:(?!^  \}).)*^  \}\r?\n(?=  window\.show\(\);)", re.M | re.S)
    blocks = [pattern.findall(path.read_text(encoding="utf-8-sig")) for path in (main_a, main_b)]
    require(all(len(block) == 1 for block in blocks) and blocks[0][0] == blocks[1][0] and
            "prepareUiPerformanceWindow(window)" in blocks[0][0] and
            hashlib.sha256(blocks[0][0].encode("utf-8")).hexdigest() == patch.get("afterBlockSha256"),
            "actual A/B pre-show QA blocks differ from shared preparation recipe")
    guards = overlay.get("protectedProductionFiles")
    require(isinstance(guards, list) and any(item.get("path") == "native/src/NativeViewport.cpp" for item in guards),
            "protected baseline viewport proof missing")
    for guard in guards:
        relative = guard.get("path")
        require(isinstance(relative, str) and relative.startswith("native/src/") and ".." not in Path(relative).parts and
                relative not in set(QA_FILES) | {QA_MAIN}, "invalid production guard path")
        require(guard.get("beforeSha256") == sha(guard.get("afterSha256"), "unchanged production SHA") and
                digest(Path(a["sourceDirectory"]) / relative) == guard["beforeSha256"], "baseline production viewport/render bytes changed")
    return identities, digest(path)


def validate_exposure(value: Any, recorded: bool, group: dict[str, Any]) -> None:
    require(isinstance(value, dict), "exposure predicate snapshot missing")
    require(value.get("logicalSize") == [640, 360] and value.get("actualDpr") == group["dpr"],
            "exposure snapshot does not describe actual fixed viewport/DPR")
    for field in ("globalViewportRect", "screenAvailableRect", "screenGeometryRect", "windowRect", "windowFrameGeometry"):
        rectangle = value.get(field)
        require(isinstance(rectangle, list) and len(rectangle) == 4 and all(type(n) is int for n in rectangle),
                "exposure rectangle missing: " + field)
    for field in ("screenContainsViewport", "visible", "windowExists", "windowExposed", "windowMinimized", "contextExists", "contextValid", "fullyExposed"):
        require(type(value.get(field)) is bool, "exposure predicate missing: " + field)
    ancestors = value.get("ancestors")
    require(isinstance(ancestors, list) and bool(ancestors), "ancestor exposure predicates missing")
    for parent in ancestors:
        require(type(parent.get("containsViewport")) is bool and type(parent.get("visible")) is bool and
                isinstance(parent.get("rect"), list) and len(parent["rect"]) == 4 and
                isinstance(parent.get("viewportRect"), list) and len(parent["viewportRect"]) == 4,
                "ancestor exposure geometry missing")
    if recorded:
        require(value["fullyExposed"] and value["screenContainsViewport"] and value["visible"] and
                value["windowExists"] and value["windowExposed"] and not value["windowMinimized"] and
                value["contextExists"] and value["contextValid"] and all(parent["containsViewport"] for parent in ancestors),
                "RECORDED cannot fake failed exposure predicates")


def validate_process(report: dict[str, Any], entry: dict[str, Any], group: dict[str, Any],
                     manifest: dict[str, Any], acceptance: dict[str, Any],
                     fixture_cache: dict[Path, tuple[str, int]], source_identity: dict[str, Any], source_receipt_sha: str,
                     fixture_path_resolver: Callable[[str], Path] = Path) -> dict[str, Any]:
    require(report.get("schema") == PROCESS_SCHEMA, "process receipt schema")
    require(report.get("status") in ("RECORDED", "FAIL", "INCONCLUSIVE"), "process status")
    require(report.get("status") != "FAIL" and not report.get("failures"), "producer reported a structural failure")
    require(report.get("runnerId") == manifest["runnerId"], "runner identity mismatch")
    require(report.get("arm") == entry["arm"] and report.get("round") == entry["round"] and
            report.get("scenario") == group["scenario"], "process arm/round/scenario mismatch")
    expected_source = manifest["baselineSourceCommit"] if entry["arm"] == "A" else manifest["candidateSourceCommit"]
    require(report.get("sourceCommit") == expected_source, "process source identity mismatch")
    sha(report.get("executableSha256"), "executableSha256")
    require(report.get("sourceIdentity") == source_identity and report.get("sourcesReceiptSha256") == source_receipt_sha and
            report["executableSha256"] == source_identity["executableSha256"],
            "process composite identity does not match independent external build receipt")
    validate_exposure(report.get("exposure"), report["status"] == "RECORDED", group)
    if report["status"] == "INCONCLUSIVE":
        require(isinstance(report.get("unready"), list) and bool(report["unready"]), "INCONCLUSIVE must retain an environment/deadline reason")
        # Partial recordings are evidence, never substituted with synthetic zero
        # measurements. Already recorded observer hard failures remain checked.
    else:
        require(report.get("unready") == [], "RECORDED cannot conceal unready phases")
    observation = report.get("observation")
    require(isinstance(observation, dict) and observation.get("enabled") is True and
            observation.get("capacity") == 512 and observation.get("overflow") is False,
            "same enabled bounded passive observer required, without overflow")
    samples = observation.get("samples")
    require(isinstance(samples, list) and 0 <= len(samples) <= observation["capacity"] and
            observation.get("count") == len(samples), "complete bounded raw observer records required")
    require(bool(samples) or report["status"] == "INCONCLUSIVE", "RECORDED requires actual passive samples")
    serial = -1
    for sample in samples:
        serial_now = integer(sample.get("frameSerial"), "sample.frameSerial", 1)
        require(serial_now > serial, "observer serials must increase")
        serial = serial_now
        integer(sample.get("hostNanoseconds"), "sample.hostNanoseconds")
        require(integer(sample.get("ownedCpuBytes"), "sample.ownedCpuBytes") <=
                acceptance["budgets"]["owned_CPU_bytes_per_viewport"], "owned CPU resource hard cap exceeded")
        require(integer(sample.get("requestedGpuBytes"), "sample.requestedGpuBytes") <=
                acceptance["budgets"]["owned_requested_GPU_bytes_per_viewport"], "requested GPU resource hard cap exceeded")
        integer(sample.get("cacheGeneration"), "cacheGeneration")
        require(type(sample.get("cacheHit")) is bool and sample.get("diagnosticOverlap") is False,
                "diagnostic overlap or invalid cache state")
    _range(report.get("preWindowWarmRange"), len(samples), "preWindowWarmRange")
    if report["status"] == "INCONCLUSIVE":
        return {"inconclusive": report["unready"], "metrics": {}, "cases": {}, "environment": report.get("environment", {}),
                "exposure": report["exposure"]}
    environment = validate_environment(report.get("environment"), group)
    for sample in samples:
        require(sample.get("logicalSize") == environment["logicalSize"] and
                sample.get("actualDpr") == environment["actualDpr"],
                "cold or warm overlay dimensions/DPR differ from the fixed viewport")
    cases = report.get("cases")
    require(isinstance(cases, list) and [case.get("id") for case in cases] == CASE_IDS[group["scenario"]],
            "missing, reordered or duplicate scenario case")
    result: dict[str, Any] = {"metrics": {}, "cases": {}, "environment": environment, "inconclusive": [], "exposure": report["exposure"]}
    result["metrics"]["process/cold_overlay_build_ms"] = (samples[0]["hostNanoseconds"] / 1e6, "cold_overlay_build_delta_ms")
    last_end = 0
    idle_budget = acceptance["budgets"]["idle"]
    minimum = acceptance["minimum_events_per_process"]
    for case in cases:
        case_id = case["id"]
        require(case.get("complete") is True, "incomplete case cannot be RECORDED")
        identity = validate_fixture(case.get("fixture"), case_id, fixture_cache, fixture_path_resolver)
        require(case.get("sourceVertexCount") == (0 if case_id == "empty" else fixture_parameters(case_id)["vertices"]) and
                case.get("sourceFaceCount") == (2 if case_id == "mesh" else 0), "actual sceneLoaded source counts")
        model_before = validate_model(case.get("modelBefore"), case_id)
        require(model_before == case.get("modelAfter"), "pan changed transforms, model fidelity or resident uploads")
        # These are lifetime upload totals, not a model's geometry/SH identity.
        # Async preparation may legitimately produce different starting totals
        # in fresh processes. Keep every absolute value and the original exact
        # same-process before/after guard; compare only zero pan deltas across
        # processes, without excluding any actual model/renderer capability.
        model_after = case["modelAfter"]
        counters_before = {field: model_before[field] for field in UPLOAD_COUNTERS}
        counters_after = {field: model_after[field] for field in UPLOAD_COUNTERS}
        upload_deltas = {field: counters_after[field] - counters_before[field] for field in UPLOAD_COUNTERS}
        require(all(delta == 0 for delta in upload_deltas.values()), "pan must retain zero attribute/SH/order upload deltas")
        semantic_model = {field: value for field, value in model_before.items() if field not in UPLOAD_COUNTERS}
        initial_camera = camera(case.get("cameraBefore"), "case.cameraBefore")
        warm_begin, warm_end = _range(case.get("warmupRange"), len(samples), "warmupRange")
        begin, end = _range(case.get("observationRange"), len(samples), "observationRange")
        require(last_end <= warm_begin < warm_end <= begin < end, "warm/measurement ranges overlap or are empty")
        last_end = end
        require(case.get("warmupEvents") == (2 if case_id == "medium" else 4), "frozen warmup event count")
        events, presentations = case.get("events"), case.get("presentations")
        wanted = minimum["empty"] if case_id == "empty" else minimum["medium_512202_gaussians"] if case_id == "medium" else minimum["light_per_model_type"]
        require(isinstance(events, list) and len(events) == wanted, "missing or substituted state-changing events")
        require(isinstance(presentations, list) and len(presentations) >= wanted, "missing fresh raw swaps")
        previous_serial = previous_ns = -1
        for presentation in presentations:
            current_serial = integer(presentation.get("serial"), "presentation.serial", 1)
            ns = integer(presentation.get("ns"), "presentation.ns")
            require(current_serial > previous_serial and ns > previous_ns, "fresh swap series must strictly advance")
            previous_serial, previous_ns = current_serial, ns
        latency: list[float] = []
        previous_event_present = -1
        previous_camera = initial_camera
        measured_serials = {sample["frameSerial"] for sample in samples[begin:end]}
        for event in events:
            start = integer(event.get("dispatchNs"), "event.dispatchNs")
            handled = integer(event.get("handlerDoneNs"), "event.handlerDoneNs")
            presented = integer(event.get("presentNs"), "event.presentNs")
            before_serial = integer(event.get("beforeRenderSerial"), "event.beforeRenderSerial")
            present_serial = integer(event.get("presentSerial"), "event.presentSerial", 1)
            require(previous_event_present < start <= handled <= presented and present_serial > before_serial,
                    "event/fresh swap timing or serial order")
            before = camera(event.get("cameraBefore"), "event.cameraBefore")
            after = camera(event.get("cameraAfterDispatch"), "event.cameraAfterDispatch")
            require(before == previous_camera and after != before and after == event.get("cameraAfterSwap"),
                    "expected public camera change not retained through fresh swap")
            require(before["distance"] == after["distance"] and before["orbit"] == after["orbit"] and
                    before["orthographic"] == after["orthographic"] and before["target"] != after["target"],
                    "frozen pan event must preserve projection/distance/orbit")
            matching = next((p for p in presentations if p["serial"] > before_serial and p["ns"] >= start), None)
            require(matching == {"serial": present_serial, "ns": presented} and present_serial in measured_serials,
                    "event must bind to the first fresh rendered/presented image and real overlay sample")
            previous_event_present, previous_camera = presented, after
            latency.append((presented - start) / 1e6)
        require(case.get("idleSettleMilliseconds") == idle_budget["settle_ms"], "idle settling policy changed")
        idle_windows = case.get("idle")
        require(isinstance(idle_windows, list) and len(idle_windows) == idle_budget["windows"], "missing idle windows")
        previous_idle_end = previous_event_present
        for window in idle_windows:
            idle_start = integer(window.get("beginNs"), "idle.beginNs")
            idle_end = integer(window.get("endNs"), "idle.endNs")
            require(idle_start >= previous_idle_end and idle_end - idle_start >= idle_budget["window_ms"] * 1e6,
                    "idle windows are too short or overlap")
            for field, budget_key in (("newRenderedFrames", "new_rendered_frames"), ("freshSwaps", "fresh_swaps"), ("metricsEmissions", "metrics_emissions")):
                require(integer(window.get(field), "idle." + field) == idle_budget[budget_key], "idle generated work: " + field)
            require(window.get("zeroTimerCallbacks") == 0 and window.get("zeroTimerActiveBefore") is False and
                    window.get("zeroTimerActiveAfter") is False and window.get("cameraUnchanged") is True,
                    "idle zero timer activity or camera mutation")
            previous_idle_end = idle_end
        require(idle_windows[0]["beginNs"] - previous_event_present >= idle_budget["settle_ms"] * 1e6,
                "idle did not actually settle before observation")
        overlay = [sample["hostNanoseconds"] / 1e6 for index, sample in enumerate(samples[begin:end], begin) if index != 0]
        intervals = [(right["ns"] - left["ns"]) / 1e6 for left, right in zip(presentations, presentations[1:])]
        require(len(overlay) >= wanted and len(intervals) >= wanted - 1, "missing warm raw samples")
        metrics = result["metrics"]
        metrics[case_id + "/overlay_p50_ms"] = (statistics.median(overlay), "warm_overlay_submit_p50_delta_ms")
        metrics[case_id + "/present_p50_ms"] = (statistics.median(intervals), "fresh_present_p50_delta_ms")
        metrics[case_id + "/response_p50_ms"] = (statistics.median(latency), "event_to_fresh_swap_p50_delta_ms")
        if case_id != "medium":
            metrics[case_id + "/overlay_p95_ms"] = (p95(overlay), "warm_overlay_submit_p95_delta_ms")
            metrics[case_id + "/present_p95_ms"] = (p95(intervals), "fresh_present_p95_delta_ms")
            metrics[case_id + "/response_p95_ms"] = (p95(latency), "event_to_fresh_swap_p95_delta_ms")
        else:
            metrics[case_id + "/present_max_ms"] = (max(intervals), "medium_max_delta_ms")
            metrics[case_id + "/response_max_ms"] = (max(latency), "medium_max_delta_ms")
        result["cases"][case_id] = {"fixture": identity, "model": semantic_model, "camera": initial_camera,
            "rawModelBefore": model_before, "rawModelAfter": model_after,
            "uploadCountersBefore": counters_before, "uploadCountersAfter": counters_after, "uploadDeltas": upload_deltas,
            "eventCameras": [event["cameraAfterSwap"] for event in events],
            "rawOverlayMilliseconds": overlay, "rawPresentIntervalsMilliseconds": intervals,
            "rawResponseMilliseconds": latency, "cacheHits": sum(sample["cacheHit"] for sample in samples[begin:end])}
    return result


def check_manifest(manifest_path: Path, acceptance_path: Path, plan_path: Path, *,
                   archive_reader: ArchivedReceiptReader | None = None) -> dict[str, Any]:
    failures: list[str] = []
    inconclusive: list[str] = []
    comparisons: list[dict[str, Any]] = []
    raw_summaries: list[dict[str, Any]] = []
    source_provenance: dict[str, Any] = {}
    frozen = False
    try:
        require(digest(acceptance_path) == FROZEN_ACCEPTANCE_SHA256, "frozen acceptance file changed; limits cannot be relaxed")
        acceptance = read_json(acceptance_path)
        require(acceptance.get("schema") == "gsw.native_ui_performance_acceptance/1", "acceptance schema")
        require(digest(plan_path) == acceptance.get("plan_sha256"), "frozen raw plan SHA mismatch")
        frozen = True
        manifest = read_json(manifest_path)
        require(manifest.get("schema") == MANIFEST_SCHEMA, "manifest schema")
        require(isinstance(manifest.get("runnerId"), str) and bool(manifest["runnerId"]), "manifest runner identity")
        sha(manifest.get("baselineSourceCommit"), "baseline source", 40)
        sha(manifest.get("candidateSourceCommit"), "candidate source", 40)
        identities, sources_sha = (validate_sources(manifest, FROZEN_ACCEPTANCE_SHA256, acceptance["plan_sha256"])
                                   if archive_reader is None else archive_reader.import_original_sources(
                                       manifest, FROZEN_ACCEPTANCE_SHA256, acceptance["plan_sha256"]))
        source_provenance = {"arms": identities, "sourcesReceiptSha256": sources_sha,
                             "baselineMeaning": "frozen pre-AA production plus explicit source-bound common QA overlay, not an untouched baseline binary"}
        if archive_reader is not None:
            source_provenance["validationMode"] = "ORIGINAL_LIVE_VALIDATION_IMPORTED_NOT_REVERIFIED"
        groups = manifest.get("expectedGroups")
        require(isinstance(groups, list) and len(groups) == len(EXPECTED_COMBINATIONS), "four frozen scenario/DPR groups required")
        require({(g.get("scenario"), g.get("dpr")) for g in groups} == EXPECTED_COMBINATIONS and
                len({g.get("id") for g in groups}) == len(groups), "group coverage/identity changed")
        group_by_id = {g["id"]: g for g in groups}
        expected = [(g["id"], round_index + 1, arm) for g in groups
                    for round_index, order in enumerate(acceptance["order"]) for arm in order]
        entries = manifest.get("processes")
        require(isinstance(entries, list) and [(e.get("group"), e.get("round"), e.get("arm")) for e in entries] == expected,
                "missing/duplicate/reordered fresh-process paired matrix; all 24 receipts required")
        results: dict[tuple[str, int, str], dict[str, Any]] = {}
        fixture_cache: dict[Path, tuple[str, int]] = {}
        exe_by_arm: dict[str, str] = {}
        for entry in entries:
            key = (entry["group"], entry["round"], entry["arm"])
            label = "/".join(map(str, key))
            try:
                if entry.get("timedOut") is True:
                    inconclusive.append(label + ": owned child reached its frozen wall deadline")
                    continue
                path = (Path(entry.get("path", "")) if archive_reader is None else
                        archive_reader.resolve_file(entry.get("path", "")))
                require(path.is_absolute() and path.is_file() and not path.is_symlink(), "real process result file required")
                require(digest(path) == sha(entry.get("sha256"), "result SHA"), "result file SHA mismatch")
                report = read_json(path)
                result = validate_process(report, entry, group_by_id[entry["group"]], manifest, acceptance, fixture_cache,
                                          identities[entry["arm"]], sources_sha,
                                          Path if archive_reader is None else archive_reader.resolve_file)
                if "exitCode" in entry:
                    require(entry["exitCode"] == (3 if report["status"] == "INCONCLUSIVE" else 0), "exit code/receipt status mismatch")
                old_exe = exe_by_arm.setdefault(entry["arm"], report["executableSha256"])
                require(old_exe == report["executableSha256"], "arm executable changed between fresh processes")
                results[key] = result
                inconclusive.extend(label + ": " + reason for reason in result["inconclusive"])
                raw_summaries.append({"group": key[0], "round": key[1], "arm": key[2], "metrics": result["metrics"],
                                      "cases": result["cases"], "exposure": result["exposure"]})
            except (ContractError, OSError, ValueError, TypeError, KeyError, IndexError, AttributeError, OverflowError) as error:
                failures.append(label + ": " + str(error))
        for group in groups:
            group_id = group["id"]
            pair_results = [(results.get((group_id, round_index, "A")), results.get((group_id, round_index, "B")))
                            for round_index in range(1, acceptance["paired_process_rounds"] + 1)]
            if any(not a or not b or a["inconclusive"] or b["inconclusive"] for a, b in pair_results):
                continue
            try:
                environment = pair_results[0][0]["environment"]
                semantic_cases = pair_results[0][0]["cases"]
                for a, b in pair_results:
                    require(a["environment"] == environment and b["environment"] == environment,
                            "DPR/dimensions/Qt/GL/swap/font/theme environment changed within group")
                    for case_id in CASE_IDS[group["scenario"]]:
                        # Timing arrays are nested observations and may differ;
                        # the exact fixture/model/public camera sequence may not.
                        for field in ("fixture", "model", "uploadDeltas", "camera", "eventCameras"):
                            require(a["cases"][case_id][field] == semantic_cases[case_id][field] == b["cases"][case_id][field],
                                    "paired fixture/fidelity/camera sequence mismatch: " + case_id + "/" + field)
                for metric in pair_results[0][0]["metrics"]:
                    baseline = [a["metrics"][metric][0] for a, _ in pair_results]
                    candidate = [b["metrics"][metric][0] for _, b in pair_results]
                    budget_key = pair_results[0][0]["metrics"][metric][1]
                    budget = acceptance["budgets"][budget_key]
                    noise_limit = limit(budget, statistics.median(baseline))
                    unstable = max(baseline) - min(baseline) > noise_limit
                    pair_rows = [{"round": index + 1, "A": a, "B": b, "delta": b - a, "limit": limit(budget, a),
                                  "withinLimit": b - a <= limit(budget, a)} for index, (a, b) in enumerate(zip(baseline, candidate))]
                    median_delta = statistics.median([b - a for a, b in zip(baseline, candidate)])
                    median_limit = limit(budget, statistics.median(baseline))
                    within = all(row["withinLimit"] for row in pair_rows) and median_delta <= median_limit
                    verdict = "INCONCLUSIVE" if unstable else "PASS" if within else "FAIL"
                    reason = group_id + "/" + metric
                    if unstable:
                        inconclusive.append(reason + ": baseline range exceeds frozen delta budget; no threshold enlarged")
                    elif not within:
                        failures.append(reason + ": paired or median-round regression exceeds frozen budget")
                    comparisons.append({"group": group_id, "metric": metric, "budgetKey": budget_key, "status": verdict,
                        "pairs": pair_rows, "baselineRange": max(baseline) - min(baseline), "noiseLimit": noise_limit,
                        "medianPairedDelta": median_delta, "medianLimit": median_limit})
            except (ContractError, KeyError, TypeError, ValueError, AttributeError, IndexError, OverflowError) as error:
                failures.append(group_id + ": " + str(error))
    except (ContractError, OSError, ValueError, TypeError, KeyError, IndexError, AttributeError, OverflowError) as error:
        failures.append(str(error))
    return {"schema": "gsw.native_ui_performance_acceptance_result/1",
            "status": "FAIL" if failures else "INCONCLUSIVE" if inconclusive else "PASS",
            "freeze": "NO_THRESHOLDS_RELAXED" if frozen else "INVALID_FREEZE",
            "acceptanceSha256": FROZEN_ACCEPTANCE_SHA256,
            "sourceProvenance": source_provenance,
            "measurementSemantics": {
                "revision": "lifetime_upload_offsets_separated_from_cross_process_model_identity",
                "sameProcessRawModelBeforeEqualsAfterRequired": True,
                "crossProcessUploadContract": "attributeUploads/shUploads/orderUploads measured-pan deltas must all equal zero",
                "rawAbsoluteUploadCountersRetained": True,
                "numericBudgetsAndNoisePolicyUnchanged": True,
                "priorRunResultsAreNotReclassifiedByThisContractCorrection": True},
            "scope": "paired Windows light-theme emptyDPR1/emptyDPR2/lightDPR1/mediumDPR1 host/presentation/idle slice",
            "unitOfReplication": "three fresh process pairs per group; frames are nested, not independent replicates",
            "failures": failures, "inconclusive": inconclusive, "comparisons": comparisons, "rawSummaries": raw_summaries,
            "notRun": ["GPU milliseconds", "driver VRAM residency", "physical input-to-photon", "dark-theme performance",
                       "live cross-monitor DPR", "user NVIDIA hardware", "20-cycle cache destruction lifecycle (separate gate)",
                       "medium points/mesh/2DGS stress", "visual quality (separate pixel gate)", "full installation acceptance"]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--acceptance", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if not args.output.is_absolute() or not args.output.parent.is_dir() or args.output.exists():
        parser.error("--output must be a fresh absolute file in an existing owned evidence directory")
    result = check_manifest(args.manifest, args.acceptance, args.plan)
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": result["status"], "freeze": result["freeze"], "failures": result["failures"],
                      "inconclusive": result["inconclusive"]}, ensure_ascii=False, allow_nan=False))
    return {"PASS": 0, "FAIL": 2, "INCONCLUSIVE": 3}[result["status"]]


if __name__ == "__main__":
    sys.exit(main())
