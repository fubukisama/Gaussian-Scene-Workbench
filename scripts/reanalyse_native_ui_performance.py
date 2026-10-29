#!/usr/bin/env python3
"""Reanalyse pinned archived receipts, without running GSW or taking measurements.

Caller-supplied pins bind bytes, not authenticity. The dedicated workflow checks
GitHub's original artifact/run metadata against its fixed, reviewed identity.
Live source/executable validation is imported from the pinned original result;
missing original source trees/binaries are never reconstructed or re-hashed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path, PureWindowsPath
import re
import stat
import sys
import zipfile
from typing import Any

# -I public CLI execution deliberately excludes the script directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_ui_performance as checker


WINDOWS_RESERVED = {"CON", "PRN", "AUX", "NUL"} | {f"COM{i}" for i in range(1, 10)} | {f"LPT{i}" for i in range(1, 10)}


def safe_extract(archive: Path, root: Path) -> None:
    checker.require(root.is_absolute() and not root.exists() and root.parent.is_dir(),
                    "fresh absolute archive extraction directory required")
    with zipfile.ZipFile(archive) as stream:
        entries = stream.infolist()
        checker.require(0 < len(entries) <= 512 and sum(item.file_size for item in entries) <= 256 * 1024 * 1024,
                        "archive entry count/uncompressed size limit")
        names: set[str] = set()
        validated: list[tuple[zipfile.ZipInfo, Path]] = []
        for item in entries:
            name = item.filename
            parts = name.rstrip("/").split("/")
            mode = item.external_attr >> 16
            checker.require(bool(name) and "\\" not in name and not name.startswith("/") and
                            all(part and part not in (".", "..") and ":" not in part and
                                part == part.rstrip(" .") and not any(ord(char) < 32 or char in '<>"|?*' for char in part) and
                                part.split(".")[0].upper() not in WINDOWS_RESERVED for part in parts) and
                            not stat.S_ISLNK(mode) and not (mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)),
                            "unsafe archive entry: " + name)
            folded = name.rstrip("/").casefold()
            checker.require(folded not in names, "duplicate/case-colliding archive entry: " + name)
            names.add(folded)
            checker.require(item.file_size <= 128 * 1024 * 1024 and not (item.flag_bits & 1),
                            "oversize/encrypted archive entry")
            target = root.joinpath(*parts)
            checker.require(target.suffix.lower() not in (".exe", ".dll", ".pyd"),
                            "archive reanalysis accepts receipts/fixtures, not executable payloads")
            validated.append((item, target))
        root.mkdir()
        for item, target in validated:
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            # ZipExtFile verifies each member CRC while reading to EOF.
            with stream.open(item) as source, target.open("xb") as destination:
                while block := source.read(1024 * 1024):
                    destination.write(block)


class ArchiveReader:
    def __init__(self, root: Path, manifest: dict[str, Any], original: dict[str, Any]):
        self.root = root.resolve()
        self.original = original
        declared = manifest.get("sources")
        checker.require(isinstance(declared, str), "archived source receipt path required")
        self.prefix = PureWindowsPath(declared).parent
        checker.require(self.prefix.is_absolute(), "original absolute archive prefix required")

    def resolve_file(self, declared: str) -> Path:
        checker.require(isinstance(declared, str) and bool(declared), "archived file path required")
        path = PureWindowsPath(declared)
        checker.require(path.is_absolute() and ".." not in path.parts and ":" not in "".join(path.parts[1:]),
                        "archived path is not a safe original absolute path")
        try:
            relative = path.relative_to(self.prefix)
        except ValueError as error:
            raise checker.ContractError("archived path outside original receipt/fixture root") from error
        # Deliberately no suffix search, basename fallback, source-dir mapping,
        # or arbitrary host path reads. Only artifact-contained bytes qualify.
        target = self.root.joinpath(*relative.parts)
        checker.require(target.is_file() and not target.is_symlink() and target.resolve().is_relative_to(self.root),
                        "archived receipt/fixture missing or unsafe")
        return target

    def import_original_sources(self, manifest: dict[str, Any], acceptance_sha: str,
                                plan_sha: str) -> tuple[dict[str, Any], str]:
        path = self.resolve_file(manifest["sources"])
        checker.require(path == self.root / "sources.json", "archive source receipt must be sources.json")
        sources = checker.read_json(path)
        checker.require(sources.get("schema") == "gsw.ui_performance_sources/2" and
                        sources.get("runId") == manifest["runnerId"] and
                        sources.get("acceptanceSha256") == acceptance_sha and sources.get("planSha256") == plan_sha,
                        "archived source receipt schema/runner/frozen identity mismatch")
        arms = sources.get("arms")
        checker.require(isinstance(arms, list) and len(arms) == 2 and
                        {arm.get("arm") for arm in arms} == {"A", "B"}, "two original source arms required")
        identities = {arm["arm"]: arm for arm in arms}
        prior = self.original
        checker.require(prior.get("schema") == "gsw.native_ui_performance_acceptance_result/1" and
                        prior.get("status") in ("PASS", "FAIL", "INCONCLUSIVE") and
                        prior.get("freeze") == "NO_THRESHOLDS_RELAXED" and prior.get("acceptanceSha256") == acceptance_sha,
                        "original published checker result/freeze required")
        provenance = prior.get("sourceProvenance", {})
        checker.require(provenance.get("arms") == identities and
                        provenance.get("sourcesReceiptSha256") == checker.digest(path) and
                        not provenance.get("validationMode"),
                        "original source-validation receipt binding mismatch")
        overlay_path = self.resolve_file(sources.get("commonObserverOverlayReceipt", ""))
        checker.require(overlay_path == self.root / "common-qa-overlay.json" and
                        checker.digest(overlay_path) == checker.sha(sources.get("commonObserverOverlayIdentity"), "overlay identity"),
                        "archived common QA overlay receipt hash mismatch")
        overlay = checker.read_json(overlay_path)
        checker.require(overlay.get("schema") == "gsw.ui_performance_common_qa_overlay/1" and
                        overlay.get("sourceProductionBaseCommit") == manifest["baselineSourceCommit"] and
                        overlay.get("fromCommit") == manifest["candidateSourceCommit"] and
                        overlay.get("untouchedBaselineBinary") is False and
                        overlay.get("productionOutsideWhitelistUnchanged") is True and overlay.get("unlistedTrackedDiff") == [],
                        "archived QA composite/source proof mismatch")
        diff = self.root / "common-qa-overlay.diff"
        checker.require(diff.is_file() and checker.digest(diff) == checker.sha(overlay.get("actualDiffSha256"), "overlay diff SHA"),
                        "archived QA overlay diff hash mismatch")
        headers = re.findall(r"^diff --git a/(\S+) b/(\S+)$", diff.read_text(encoding="utf-8-sig"), re.M)
        changed = overlay.get("changedPaths", [])
        checker.require(bool(headers) and len(changed) == len(set(changed)) and checker.QA_MAIN in changed and
                        all(left == right and left in set(checker.QA_FILES) | {checker.QA_MAIN} for left, right in headers) and
                        {left for left, _ in headers} == set(changed), "archived overlay whitelist mismatch")
        records = overlay.get("files")
        checker.require(isinstance(records, list) and len(records) == len(checker.QA_FILES) and
                        {item.get("path") for item in records} == set(checker.QA_FILES), "archived common helper whitelist mismatch")
        for record in records:
            checker.sha(record.get("beforeSha256"), "original helper before SHA")
            checker.sha(record.get("afterSha256"), "original helper after SHA")
        for arm, identity in identities.items():
            base = manifest["baselineSourceCommit"] if arm == "A" else manifest["candidateSourceCommit"]
            checker.require(identity.get("sourceCommit") == identity.get("sourceProductionBaseCommit") == base and
                            identity.get("workingTreeIsClean") is (arm == "B") and
                            identity.get("commonObserverOverlayApplied") is (arm == "A") and
                            identity.get("commonObserverOverlayIdentity") == checker.digest(overlay_path) and
                            identity.get("commonObserverOverlayFromCommit") == manifest["candidateSourceCommit"],
                            "archived source arm composite identity mismatch")
            checker.sha(identity.get("executableSha256"), "original executable SHA")
            checker.sha(identity.get("sourceTree"), "original source tree", 40)
            checker.sha(identity.get("baseSourceTree"), "original base tree", 40)
        a, b = identities["A"], identities["B"]
        checker.require(overlay.get("baseTree") == a["baseSourceTree"] and overlay.get("compositeTree") == a["sourceTree"] and
                        a["baseSourceTree"] != a["sourceTree"] and b["baseSourceTree"] == b["sourceTree"],
                        "archived source trees cannot masquerade as an untouched baseline")
        patch = overlay.get("mainPatch", {})
        checker.require(patch.get("path") == checker.QA_MAIN and patch.get("prefixUnchanged") is True and
                        patch.get("suffixUnchanged") is True, "archived main non-QA preservation proof missing")
        checker.sha(patch.get("baselineBlob"), "original main blob", 40)
        for name in ("beforeSha256", "afterSha256", "beforeBlockSha256", "afterBlockSha256"):
            checker.sha(patch.get(name), "original main " + name)
        guards = overlay.get("protectedProductionFiles")
        checker.require(isinstance(guards, list) and
                        any(item.get("path") == "native/src/NativeViewport.cpp" for item in guards),
                        "archived production preservation proof missing")
        for guard in guards:
            relative = guard.get("path", "")
            checker.require(relative.startswith("native/src/") and ".." not in Path(relative).parts and
                            relative not in set(checker.QA_FILES) | {checker.QA_MAIN} and
                            guard.get("beforeSha256") == checker.sha(guard.get("afterSha256"), "original protected source SHA"),
                            "archived production preservation proof changed")
        return identities, checker.digest(path)


def reanalyse(args: argparse.Namespace) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema": "gsw.native_ui_performance_archived_reanalysis/1", "status": "FAIL",
        "applicationExecuted": False, "newMeasurementsTaken": False, "fullAcceptance": False,
        "archiveSha256": args.expected_archive_sha256, "originalRunId": args.original_run_id,
        "originalArtifactId": args.artifact_id,
        "pinTrust": "explicit caller pins; authenticity requires independent original artifact/run metadata verification",
        "originalSourceVerification": {"currentSourceAndBinaryFiles": "NOT_REVERIFIED", "importStatus": "NOT_IMPORTED",
            "meaning": "Original live checker source/binary verification imported from pinned original results.json; no archived EXE or source trees exist."},
        "failures": [], "inconclusive": []}
    try:
        checker.sha(args.expected_archive_sha256, "archive SHA256")
        checker.sha(args.baseline_commit, "baseline commit", 40)
        checker.sha(args.candidate_commit, "candidate commit", 40)
        checker.require(checker.digest(args.archive) == args.expected_archive_sha256, "archive SHA256 pin mismatch")
        safe_extract(args.archive, args.extract_root)
        manifest_path = args.extract_root / "manifest.json"
        manifest = checker.read_json(manifest_path)
        checker.require(manifest.get("runnerId") == args.original_run_id and
                        manifest.get("baselineSourceCommit") == args.baseline_commit and
                        manifest.get("candidateSourceCommit") == args.candidate_commit,
                        "original manifest run/source pin mismatch")
        original_path = args.extract_root / "results.json"
        original = checker.read_json(original_path)
        original_sha = checker.digest(original_path)
        result["previousResult"] = {"sha256": original_sha, "status": original.get("status"),
            "failures": original.get("failures"), "inconclusive": original.get("inconclusive"),
            "readOnly": True, "reclassified": False}
        result["originalSourceVerification"]["importedFromResultSha256"] = original_sha
        analysis = checker.check_manifest(manifest_path, args.acceptance, args.plan,
                                          archive_reader=ArchiveReader(args.extract_root, manifest, original))
        if analysis["sourceProvenance"].get("validationMode") == "ORIGINAL_LIVE_VALIDATION_IMPORTED_NOT_REVERIFIED":
            result["originalSourceVerification"]["importStatus"] = "IMPORTED_FROM_PINNED_ORIGINAL_RESULT"
        result.update(status=analysis["status"], failures=analysis["failures"],
                      inconclusive=analysis["inconclusive"], analysis=analysis)
        result["archiveSha256AfterRead"] = checker.digest(args.archive)
        checker.require(result["archiveSha256AfterRead"] == args.expected_archive_sha256,
                        "archive changed during read-only reanalysis")
    except (checker.ContractError, OSError, ValueError, TypeError, KeyError, IndexError, AttributeError,
            OverflowError, zipfile.BadZipFile, RuntimeError) as error:
        result["status"] = "FAIL"
        result["failures"].append(str(error))
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("archive", "extract-root", "acceptance", "plan", "output"):
        parser.add_argument("--" + flag, type=Path, required=True)
    for flag in ("expected-archive-sha256", "original-run-id", "artifact-id", "baseline-commit", "candidate-commit"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args(argv)
    if not args.output.is_absolute() or not args.output.parent.is_dir() or args.output.exists():
        parser.error("--output must be a fresh absolute file in an existing owned evidence directory")
    result = reanalyse(args)
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")
    print(json.dumps({key: result[key] for key in ("status", "failures", "inconclusive")}, ensure_ascii=False))
    return {"PASS": 0, "FAIL": 2, "INCONCLUSIVE": 3}[result["status"]]


if __name__ == "__main__":
    sys.exit(main())
