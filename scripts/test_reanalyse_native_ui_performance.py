"""Remote-only archive CLI contracts. No application or timing is executed."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_native_ui_performance as live_fixtures


REPO = Path(__file__).resolve().parents[1]
CLI = REPO / "scripts/reanalyse_native_ui_performance.py"


class ArchivedPerformanceCliContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.environ.get("GITHUB_ACTIONS") != "true":
            raise RuntimeError("Remote CI contracts only; no local automated-test fallback.")
        live_fixtures.UiPerformanceCliContracts.setUpClass()

    @classmethod
    def tearDownClass(cls):
        live_fixtures.UiPerformanceCliContracts.tearDownClass()

    def setUp(self):
        # Existing public-CLI receipt fixture; its byte/source files are never
        # executable. Only data and original proof records enter our archive.
        self.fixture = live_fixtures.UiPerformanceCliContracts(
            methodName="test_complete_same_runner_paired_receipts_are_accepted")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.shared = self.root / "shared-fixtures"
        self.shared.mkdir()
        for case_id, identity in self.fixture.identities.items():
            copied = self.shared / (case_id + ".ply")
            shutil.copyfile(identity["path"], copied)
            for _, report in self.fixture.reports.values():
                for case in report["cases"]:
                    if case["id"] == case_id:
                        case["fixture"]["path"] = str(copied.resolve())
        code, prior = self.fixture.evaluate()
        self.assertEqual((code, prior["status"]), (0, "PASS"), prior)
        self.prior = prior
        self.prior_path = self.root / "results.json"
        live_fixtures.dump(self.prior_path, prior)

    def persist_reports(self):
        for entry in self.fixture.manifest["processes"]:
            path, report = self.fixture.reports[(entry["group"], entry["round"], entry["arm"])]
            live_fixtures.dump(path, report)
            entry["sha256"] = live_fixtures.sha(path)
        live_fixtures.dump(self.root / "manifest.json", self.fixture.manifest)

    def archive(self, *, refresh=True, extra_entry=None):
        if refresh:
            self.persist_reports()
        archive = self.root / "input.zip"
        selected = [self.root / name for name in
                    ("manifest.json", "sources.json", "common-qa-overlay.json", "common-qa-overlay.diff", "results.json")]
        selected += [path for path, _ in self.fixture.reports.values()]
        selected += list(self.shared.iterdir())
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as stream:
            for path in selected:
                stream.write(path, path.relative_to(self.root).as_posix())
            if extra_entry is not None:
                stream.writestr(extra_entry, b"must not escape")
        return archive

    def evaluate_archive(self, archive, *, expected_sha=None):
        output = self.root / "reanalysis.json"
        extracted = self.root / "extracted"
        process = subprocess.run([
            sys.executable, "-I", "-B", str(CLI), "--archive", str(archive),
            "--expected-archive-sha256", expected_sha or live_fixtures.sha(archive),
            "--original-run-id", "contract-owned-runner", "--artifact-id", "fixture-artifact",
            "--baseline-commit", "a" * 40, "--candidate-commit", "b" * 40,
            "--extract-root", str(extracted.resolve()), "--acceptance", str(live_fixtures.ACCEPTANCE),
            "--plan", str(live_fixtures.PLAN), "--output", str(output.resolve())],
            text=True, capture_output=True, timeout=45, check=False)
        self.assertTrue(output.is_file(), process.stdout + process.stderr)
        return process.returncode, json.loads(output.read_text(encoding="utf-8"))

    def assert_archive_failure(self, fragment, *, refresh=True, extra_entry=None):
        code, result = self.evaluate_archive(self.archive(refresh=refresh, extra_entry=extra_entry))
        self.assertEqual((code, result["status"]), (2, "FAIL"), result)
        self.assertIn(fragment, "\n".join(result["failures"]))

    def test_reanalysis_uses_raw_receipts_and_keeps_upload_offsets(self):
        for boundary in ("modelBefore", "modelAfter"):
            self.fixture.receipt()["cases"][2][boundary]["attributeUploads"] += 25
            self.fixture.receipt()["cases"][2][boundary]["orderUploads"] += 30
        archive = self.archive()
        original_bytes = archive.read_bytes()
        code, result = self.evaluate_archive(archive)
        self.assertEqual((code, result["status"]), (0, "PASS"), result)
        self.assertEqual(24, len(result["analysis"]["rawSummaries"]))
        self.assertEqual("NOT_REVERIFIED", result["originalSourceVerification"]["currentSourceAndBinaryFiles"])
        self.assertFalse(result["applicationExecuted"])
        self.assertFalse(result["newMeasurementsTaken"])
        self.assertFalse(result["fullAcceptance"])
        observed = next(row for row in result["analysis"]["rawSummaries"] if
                        row["group"] == "light-dpr1" and row["round"] == 1 and row["arm"] == "B")
        self.assertEqual(26, observed["cases"]["3dgs"]["rawModelBefore"]["attributeUploads"])
        self.assertEqual(0, observed["cases"]["3dgs"]["uploadDeltas"]["attributeUploads"])
        self.assertEqual(original_bytes, archive.read_bytes())
        self.assertEqual(self.prior, json.loads((self.root / "extracted/results.json").read_text(encoding="utf-8")))
        self.assertFalse(list((self.root / "extracted").rglob("*.exe")))

    def test_modified_raw_receipt_without_manifest_rebinding_is_rejected(self):
        path, report = self.fixture.reports[("light-dpr1", 1, "B")]
        report["cases"][0]["modelAfter"]["attributeUploads"] += 1
        live_fixtures.dump(path, report)
        self.assert_archive_failure("result file SHA mismatch", refresh=False)

    def test_source_label_change_does_not_inherit_original_validation(self):
        self.fixture.receipt()["sourceCommit"] = "c" * 40
        self.assert_archive_failure("process source identity mismatch")

    def test_same_process_upload_change_remains_a_hard_failure(self):
        self.fixture.receipt()["cases"][2]["modelAfter"]["orderUploads"] += 1
        self.assert_archive_failure("pan changed transforms, model fidelity or resident uploads")

    def test_gaussian_decimation_cannot_be_hidden_by_archive_mode(self):
        case = self.fixture.receipt()["cases"][2]
        case["modelBefore"]["renderedPointCount"] = case["modelAfter"]["renderedPointCount"] = 440
        self.assert_archive_failure("no Gaussian decimation")

    def test_fixture_bytes_are_rehashed_not_imported_from_old_result(self):
        path = self.shared / "points.ply"
        path.write_bytes(path.read_bytes() + b"tampered")
        self.assert_archive_failure("actual fixture bytes/hash mismatch")

    def test_source_loaded_counts_remain_independently_checked(self):
        self.fixture.receipt()["cases"][2]["sourceVertexCount"] = 440
        self.assert_archive_failure("actual sceneLoaded source counts")

    def test_receipt_cannot_redirect_fixture_reads_outside_the_original_archive(self):
        self.fixture.receipt()["cases"][0]["fixture"]["path"] = "D:/outside-original-archive/points.ply"
        self.assert_archive_failure("archived path outside original receipt/fixture root")

    def test_changed_source_receipt_cannot_borrow_the_prior_provenance(self):
        sources = json.loads(self.fixture.sources_path.read_text(encoding="utf-8"))
        sources["arms"][1]["executableSha256"] = "0" * 64
        live_fixtures.dump(self.fixture.sources_path, sources)
        self.assert_archive_failure("original source-validation receipt binding mismatch")

    def test_archived_analysis_cannot_be_recursively_imported_as_live_source_proof(self):
        self.prior["sourceProvenance"]["validationMode"] = "ORIGINAL_LIVE_VALIDATION_IMPORTED_NOT_REVERIFIED"
        live_fixtures.dump(self.prior_path, self.prior)
        self.assert_archive_failure("original source-validation receipt binding mismatch")

    def test_original_failure_and_new_noisy_result_are_separate(self):
        self.prior["status"] = "FAIL"
        self.prior["failures"] = ["light-dpr1: paired fixture/fidelity/camera sequence mismatch: 3dgs/model"]
        live_fixtures.dump(self.prior_path, self.prior)
        report = self.fixture.receipt(round_index=2, arm="A")
        begin, end = report["cases"][0]["observationRange"]
        for sample in report["observation"]["samples"][begin:end]:
            sample["hostNanoseconds"] = 3_000_000
        code, result = self.evaluate_archive(self.archive())
        self.assertEqual((code, result["status"]), (3, "INCONCLUSIVE"), result)
        self.assertEqual("FAIL", result["previousResult"]["status"])
        self.assertIn("baseline range exceeds frozen delta budget", "\n".join(result["inconclusive"]))

    def test_zip_digest_pin_is_checked_before_any_receipt_is_trusted(self):
        archive = self.archive()
        code, result = self.evaluate_archive(archive, expected_sha="0" * 64)
        self.assertEqual((code, result["status"]), (2, "FAIL"), result)
        self.assertIn("archive SHA256 pin mismatch", "\n".join(result["failures"]))

    def test_traversal_zip_entry_is_rejected_without_outside_writes(self):
        self.assert_archive_failure("unsafe archive entry", extra_entry="../escaped.txt")
        self.assertFalse((self.root / "escaped.txt").exists())


if __name__ == "__main__":
    unittest.main()
