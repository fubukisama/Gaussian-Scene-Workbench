"""Public explicit-contract comparison of supplied independent measurements."""
import unittest


def measured(metrics, split="held_out"):
    return {"version": 1, "identity": {
        "dataset_id": "dataset-sha256-literal", "viewset_id": "held-out-views-0-7",
        "resolution_id": "3804x2142", "mask_id": "none",
        "metric_protocol_id": "linear-rgb-v1-depth-meters-normal-degrees"},
        "split": split, "metrics": metrics}


class QualityComparisonTests(unittest.TestCase):
    def test_held_out_image_measurements_pass_only_the_explicit_user_contract(self):
        from native.worker.training_quality import compare_quality
        baseline = measured({"psnr": 30.0})
        candidate = measured({"psnr": 29.8})
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"absolute_min": 29.0, "max_regression": .25}}}
        result = compare_quality(baseline, candidate, contract)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["domains"]["image"]["status"], "pass")
        self.assertAlmostEqual(result["metrics"]["psnr"]["delta"], -.2)
        self.assertAlmostEqual(result["metrics"]["psnr"]["regression"], .2)

    def test_lower_is_better_uses_user_tolerance_and_absolute_ceiling(self):
        from native.worker.training_quality import compare_quality
        contract = {"version": 1, "required_domains": ["image"], "metrics": {
            "psnr": {"max_regression": 0},
            "lpips": {"absolute_max": .13, "max_regression": .015}}}
        result = compare_quality(measured({"psnr": 30., "lpips": .12}),
                                 measured({"psnr": 30.5, "lpips": .14}), contract)
        self.assertEqual(result["status"], "regressed")
        self.assertEqual(result["metrics"]["psnr"]["status"], "pass")
        self.assertEqual(result["metrics"]["lpips"]["status"], "regressed")
        self.assertAlmostEqual(result["metrics"]["lpips"]["delta"], .02)
        self.assertAlmostEqual(result["metrics"]["lpips"]["regression"], .02)
        self.assertEqual(result["metrics"]["lpips"]["reasons"],
                         ["above_absolute_max", "exceeds_max_regression"])

    def test_different_dataset_views_resolution_mask_protocol_or_split_are_not_comparable(self):
        from copy import deepcopy
        from native.worker.training_quality import compare_quality
        baseline = measured({"psnr": 30.})
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .5}}}
        for field in tuple(baseline["identity"]) + ("split",):
            candidate = deepcopy(baseline)
            if field == "split":
                candidate[field] = "training_fit"
            else:
                candidate["identity"][field] += "-different"
            with self.subTest(field=field):
                result = compare_quality(baseline, candidate, contract)
                self.assertEqual(result["status"], "not_comparable")
                self.assertEqual(result["metrics"]["psnr"]["status"], "not_comparable")
                self.assertIsNone(result["metrics"]["psnr"]["delta"])
                self.assertIn(field, result["identity_mismatches"])

    def test_training_psnr_is_fit_evidence_never_independent_quality_pass(self):
        from native.worker.training_quality import compare_quality
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .25}}}
        result = compare_quality(measured({"psnr": 30.}, "training_fit"),
                                 measured({"psnr": 31.}, "training_fit"), contract)
        self.assertEqual(result["status"], "unverified")
        self.assertFalse(result["independent_validation"])
        self.assertEqual(result["metrics"]["psnr"]["delta"], 1.)
        self.assertEqual(result["metrics"]["psnr"]["threshold_status"], "pass")
        self.assertEqual(result["domains"]["image"]["status"], "unverified")

    def test_image_pass_does_not_establish_geometry_quality_or_missing_metrics(self):
        from native.worker.training_quality import compare_quality
        baseline, candidate = measured({"psnr": 30.}), measured({"psnr": 30.1})
        contract = {"version": 1, "required_domains": ["image", "geometry"],
                    "metrics": {"psnr": {"max_regression": .1}}}
        result = compare_quality(baseline, candidate, contract)
        self.assertEqual(result["status"], "unverified")
        self.assertEqual(result["domains"]["image"]["status"], "pass")
        self.assertEqual(result["domains"]["geometry"]["status"], "unverified")
        contract["metrics"]["depth_rmse"] = {"absolute_max": .02}
        result = compare_quality(baseline, candidate, contract)
        self.assertEqual(result["status"], "unverified")
        self.assertEqual(result["metrics"]["depth_rmse"]["status"], "unverified")
        self.assertIn("missing_measurement", result["metrics"]["depth_rmse"]["reasons"])

    def test_missing_user_tolerance_is_unverified_not_an_implicit_zero_tolerance(self):
        from native.worker.training_quality import compare_quality
        baseline, candidate = measured({"psnr": 30.}), measured({"psnr": 29.})
        for thresholds in ({}, {"psnr": {}}):
            with self.subTest(thresholds=thresholds):
                result = compare_quality(baseline, candidate, {"version": 1,
                    "required_domains": ["image"], "metrics": thresholds})
                self.assertEqual(result["status"], "unverified")
                self.assertEqual(result["metrics"]["psnr"]["delta"], -1.)
                self.assertIn("user_threshold_missing", result["metrics"]["psnr"]["reasons"])

    def test_geometry_contract_can_regress_without_changing_image_result(self):
        from native.worker.training_quality import compare_quality
        baseline = measured({"psnr": 30., "depth_rmse": .01, "normal_mae": 5., "mesh_distance": .02})
        candidate = measured({"psnr": 30.1, "depth_rmse": .02, "normal_mae": 4., "mesh_distance": .04})
        contract = {"version": 1, "required_domains": ["image", "geometry"], "metrics": {
            "psnr": {"max_regression": .1}, "depth_rmse": {"max_regression": .02},
            "normal_mae": {"absolute_max": 6.}, "mesh_distance": {"max_regression": .01}}}
        result = compare_quality(baseline, candidate, contract)
        self.assertEqual(result["status"], "regressed")
        self.assertEqual(result["domains"]["image"]["status"], "pass")
        self.assertEqual(result["domains"]["geometry"]["status"], "regressed")
        self.assertEqual(result["metrics"]["normal_mae"]["regression"], 0.)

    def test_malformed_or_nonfinite_measurements_and_thresholds_are_rejected(self):
        from copy import deepcopy
        from native.worker.training_quality import compare_quality
        baseline = measured({"psnr": 30.})
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .1}}}
        invalid_candidates = []
        for value in (True, False, float("nan"), float("inf"), float("-inf"), "30.0", 10 ** 500):
            report = deepcopy(baseline)
            report["metrics"]["psnr"] = value
            invalid_candidates.append(report)
        for key, value in (("version", True), ("split", "unknown"),
                           ("metrics", {"gaussian_count": 1000000})):
            report = deepcopy(baseline)
            report[key] = value
            invalid_candidates.append(report)
        report = deepcopy(baseline)
        del report["identity"]["mask_id"]
        invalid_candidates.append(report)
        for candidate in invalid_candidates:
            with self.subTest(candidate=candidate), self.assertRaises(ValueError):
                compare_quality(baseline, candidate, contract)
        for thresholds in ({"max_regression": -.1}, {"max_regression": True},
                           {"absolute_min": float("nan")}, {"absolute_max": float("inf")},
                           {"absolute_min": 31., "absolute_max": 30.}, {"quality_percent": 99.}):
            bad_contract = deepcopy(contract)
            bad_contract["metrics"]["psnr"] = thresholds
            with self.subTest(thresholds=thresholds), self.assertRaises(ValueError):
                compare_quality(baseline, baseline, bad_contract)

    def test_psnr_zero_mse_has_explicit_infinite_semantics_without_json_nan(self):
        import json
        from native.worker.training_quality import compare_quality
        perfect = {"value": None, "special": "positive_infinity", "reason": "zero_mse"}
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"absolute_min": 25., "max_regression": .1}}}
        improved = compare_quality(measured({"psnr": 30.}), measured({"psnr": perfect}), contract)
        self.assertEqual(improved["status"], "pass")
        self.assertIsNone(improved["metrics"]["psnr"]["delta"])
        self.assertEqual(improved["metrics"]["psnr"]["delta_kind"], "positive_infinity")
        worsened = compare_quality(measured({"psnr": perfect}), measured({"psnr": 30.}), contract)
        self.assertEqual(worsened["status"], "regressed")
        self.assertEqual(worsened["metrics"]["psnr"]["regression_kind"], "positive_infinity")
        equivalent = compare_quality(measured({"psnr": perfect}), measured({"psnr": perfect}), contract)
        self.assertEqual(equivalent["status"], "pass")
        self.assertEqual(equivalent["metrics"]["psnr"]["delta_kind"], "both_zero_mse")
        for result in (improved, worsened, equivalent):
            json.dumps(result, allow_nan=False)

    def test_setting_changes_are_visible_but_do_not_invalidate_comparable_measurements(self):
        from copy import deepcopy
        from native.worker.training_quality import compare_quality
        baseline, candidate = measured({"psnr": 30.}), measured({"psnr": 29.9})
        baseline["settings"] = {"engine": "stock", "optimization": {"iterations": 30000,
                                "densify": True}, "legacy": None}
        candidate["settings"] = {"engine": "fastergs", "optimization": {"iterations": 15000,
                                 "densify": True}, "precision": "fp32"}
        before = deepcopy((baseline, candidate))
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .2}}}
        result = compare_quality(baseline, candidate, contract)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["identity_mismatches"], [])
        changes = {tuple(change["path"]): change for change in result["settings_changes"]}
        self.assertEqual(changes[("engine",)]["before"], "stock")
        self.assertEqual(changes[("engine",)]["after"], "fastergs")
        self.assertEqual(changes[("optimization", "iterations")]["after"], 15000)
        self.assertFalse(changes[("legacy",)]["after_present"])
        self.assertFalse(changes[("precision",)]["before_present"])
        self.assertNotIn(("optimization", "densify"), changes)
        self.assertEqual((baseline, candidate), before)

    def test_cli_compares_json_without_modifying_input_reports_or_overwriting_them(self):
        import hashlib
        import json
        from pathlib import Path
        import subprocess
        import sys
        import tempfile
        source = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="gsw-quality-") as directory:
            folder = Path(directory)
            inputs = {"baseline": measured({"psnr": 30.}), "candidate": measured({"psnr": 29.9}),
                      "contract": {"version": 1, "required_domains": ["image"],
                                   "metrics": {"psnr": {"max_regression": .2}}}}
            arguments = []
            for name, report in inputs.items():
                path = folder / (name + ".json")
                path.write_text(json.dumps(report), encoding="utf-8")
                arguments.extend(("--" + name, str(path)))
            original = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in folder.iterdir()}
            command = [sys.executable, str(source / "scripts/compare_training_quality.py")] + arguments
            process = subprocess.run(command, cwd=str(source), capture_output=True, text=True,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(json.loads(process.stdout)["status"], "pass")
            blocked = subprocess.run(command + ["--output", str(folder / "baseline.json")],
                                     cwd=str(source), capture_output=True, text=True,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.assertEqual(blocked.returncode, 2)
            self.assertEqual({path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in folder.iterdir()}, original)

    def test_exact_decimal_threshold_boundary_is_not_a_hidden_binary_float_penalty(self):
        from native.worker.training_quality import compare_quality
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .1}}}
        result = compare_quality(measured({"psnr": 30.}), measured({"psnr": 29.9}), contract)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["metrics"]["psnr"]["regression"], .1)

    def test_missing_measured_pairs_cannot_claim_independent_validation(self):
        from native.worker.training_quality import compare_quality
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .1}}}
        result = compare_quality(measured({}), measured({}), contract)
        self.assertEqual(result["status"], "unverified")
        self.assertFalse(result["independent_validation"])

    def test_result_shape_is_stable_when_evaluation_inputs_do_not_match(self):
        from native.worker.training_quality import compare_quality
        baseline, candidate = measured({"psnr": 30.}), measured({"psnr": 30.1})
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .1}}}
        comparable = compare_quality(baseline, candidate, contract)
        candidate["identity"]["mask_id"] = "another-mask"
        mismatch = compare_quality(baseline, candidate, contract)
        self.assertEqual(set(comparable), set(mismatch))
        self.assertEqual(set(comparable["metrics"]["psnr"]), set(mismatch["metrics"]["psnr"]))
        self.assertEqual(set(comparable["domains"]["image"]), set(mismatch["domains"]["image"]))

    def test_cli_exit_codes_distinguish_regression_missing_evidence_and_wrong_inputs(self):
        import contextlib
        import io
        import json
        from pathlib import Path
        import tempfile
        from scripts.compare_training_quality import main
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"psnr": {"max_regression": .1}}}
        cases = [(measured({"psnr": 29.}), contract, 1, "regressed"),
                 (measured({"psnr": 30.}), {"version": 1, "required_domains": ["image"], "metrics": {}},
                  3, "unverified")]
        different = measured({"psnr": 30.})
        different["identity"]["viewset_id"] = "different-views"
        cases.append((different, contract, 4, "not_comparable"))
        with tempfile.TemporaryDirectory(prefix="gsw-quality-codes-") as directory:
            folder = Path(directory)
            for candidate, selected_contract, expected_code, expected_status in cases:
                arguments = []
                for name, report in (("baseline", measured({"psnr": 30.})),
                                     ("candidate", candidate), ("contract", selected_contract)):
                    path = folder / (name + ".json")
                    path.write_text(json.dumps(report), encoding="utf-8")
                    arguments.extend(("--" + name, str(path)))
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(arguments), expected_code)
                self.assertEqual(json.loads(stdout.getvalue())["status"], expected_status)
            for invalid in ('{"version":1,"version":1}', '{"metrics":{"psnr":NaN}}'):
                (folder / "candidate.json").write_text(invalid, encoding="utf-8")
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(arguments), 2)

    def test_measurements_reject_impossible_metric_ranges_without_arbitrary_upper_caps(self):
        from native.worker.training_quality import compare_quality
        contract = {"version": 1, "required_domains": ["image"],
                    "metrics": {"ssim": {"max_regression": 0}}}
        for name, value in (("ssim", -1.0001), ("ssim", 1.0001), ("lpips", -.001),
                            ("depth_rmse", -.001), ("normal_mae", -.001), ("mesh_distance", -.001)):
            with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                compare_quality(measured({name: value}), measured({name: value}), contract)
        baseline = measured({"psnr": -2., "ssim": -.6, "lpips": 1.5, "normal_mae": 1000.})
        candidate = measured({"psnr": -1., "ssim": -.5, "lpips": 1.4, "normal_mae": 999.})
        result = compare_quality(baseline, candidate, {"version": 1,
            "required_domains": ["image", "geometry"], "metrics": {
                name: {"max_regression": 0} for name in baseline["metrics"]}})
        self.assertEqual(result["status"], "pass")

    def test_absolute_quality_thresholds_follow_metric_ranges_not_quality_percentages(self):
        from native.worker.training_quality import compare_quality
        for name, threshold in (("ssim", -1.001), ("ssim", 1.001), ("lpips", -.1),
                                ("depth_rmse", -.1), ("normal_mae", -.1), ("mesh_distance", -.1)):
            for limit in ("absolute_min", "absolute_max"):
                with self.subTest(name=name, limit=limit), self.assertRaises(ValueError):
                    compare_quality(measured({name: 0.}), measured({name: 0.}), {
                        "version": 1, "required_domains": ["image"],
                        "metrics": {name: {limit: threshold}}})

    def test_exported_comparison_retains_evaluation_identity_and_original_user_contract(self):
        from copy import deepcopy
        from native.worker.training_quality import compare_quality
        for mismatch in (False, True):
            baseline, candidate = measured({"psnr": 30.}), measured({"psnr": 29.9})
            if mismatch:
                candidate["identity"]["mask_id"] = "different-mask"
            contract = {"version": 1, "required_domains": ["image"],
                        "metrics": {"psnr": {"max_regression": .1}}}
            original_baseline, original_candidate, original_contract = deepcopy((baseline, candidate, contract))
            result = compare_quality(baseline, candidate, contract)
            with self.subTest(mismatch=mismatch):
                self.assertEqual(result["baseline_identity"], original_baseline["identity"])
                self.assertEqual(result["candidate_identity"], original_candidate["identity"])
                self.assertEqual(result["baseline_split"], "held_out")
                self.assertEqual(result["candidate_split"], "held_out")
                self.assertEqual(result["contract"], original_contract)
                baseline["identity"]["dataset_id"] = "mutated-after-comparison"
                candidate["identity"]["resolution_id"] = "mutated-after-comparison"
                contract["required_domains"].append("geometry")
                contract["metrics"]["psnr"]["max_regression"] = 100.
                self.assertEqual(result["baseline_identity"], original_baseline["identity"])
                self.assertEqual(result["candidate_identity"], original_candidate["identity"])
                self.assertEqual(result["contract"], original_contract)

    def test_mutating_an_input_special_measurement_cannot_change_an_exported_result(self):
        from native.worker.training_quality import compare_quality
        for mismatch in (False, True):
            baseline = measured({"psnr": {"value": None, "special": "positive_infinity", "reason": "zero_mse"}})
            candidate = measured({"psnr": 30.})
            if mismatch:
                candidate["identity"]["mask_id"] = "different-mask"
            result = compare_quality(baseline, candidate, {"version": 1,
                "required_domains": ["image"], "metrics": {"psnr": {"max_regression": .1}}})
            baseline["metrics"]["psnr"]["reason"] = "mutated-after-comparison"
            self.assertEqual(result["metrics"]["psnr"]["baseline"]["reason"], "zero_mse")

    def test_measured_image_pairs_without_split_proof_are_comparable_but_not_independently_validated(self):
        from native.worker.training_quality import compare_quality
        baseline, candidate = measured({"psnr": 30.}, "unverified"), measured({"psnr": 29.9}, "unverified")
        for report in (baseline, candidate):
            report["evidence"] = {"version": 1, "producer": "gsw-image-quality-v1",
                                  "held_out_verified": False, "views": [{"id": "one", "mse": .001}]}
        contract = {"version": 1, "required_domains": ["image"], "metrics": {"psnr": {"max_regression": .2}}}
        result = compare_quality(baseline, candidate, contract)
        self.assertEqual(result["status"], "unverified")
        self.assertEqual(result["metrics"]["psnr"]["threshold_status"], "pass")
        self.assertFalse(result["independent_validation"])
        # Editing only the split label must not override a producer's explicit
        # absence of held-out evidence.
        baseline["split"] = candidate["split"] = "held_out"
        self.assertEqual(compare_quality(baseline, candidate, contract)["status"], "unverified")

    def test_list_settings_changes_do_not_treat_boolean_and_numeric_values_as_equal(self):
        from native.worker.training_quality import compare_quality
        baseline, candidate = measured({"psnr": 30.}), measured({"psnr": 30.})
        baseline["settings"], candidate["settings"] = {"flags": [True, {"nested": False}]}, {"flags": [1, {"nested": 0}]}
        result = compare_quality(baseline, candidate, {"version": 1, "required_domains": ["image"],
                                                    "metrics": {"psnr": {"max_regression": 0}}})
        self.assertEqual(len(result["settings_changes"]), 1)
        self.assertEqual(result["settings_changes"][0]["path"], ["flags"])


if __name__ == "__main__":
    unittest.main()
