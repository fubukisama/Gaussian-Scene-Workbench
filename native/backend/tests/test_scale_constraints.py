"""P1 constraints through the approved public solve/replay interface."""
import copy
import json
import unittest

import numpy as np

from native.backend import camera_dataset
from native.backend import scale_constraints as solver


def dataset():
    return camera_dataset.refresh_dataset_identity({
        "schema": "gsw.camera_dataset/1", "pose_policy": "locked",
        "scale": {"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None},
        "views": [{"view_id": "a", "split": "train", "source_c2w_opencv":
                   [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]},
                  {"view_id": "b", "split": "test", "source_c2w_opencv":
                   [[1., 0., 0., 4.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]}],
        "splits": {"train": ["a"], "val": [], "test": ["b"]}})


def distance(identifier="d1", **changes):
    row = {"id": identifier, "kind": "distance", "role": "fit", "reference_id": identifier,
           "reference_unit": "m", "source_a": [0., 0., 0.], "source_b": [4., 0., 0.],
           "reference_distance": 1., "provenance": {}}
    row.update(changes)
    return row


def request(data, rows=None, **changes):
    value = {"schema": "gsw.scale_constraints/1", "input_data_identity": data["data_identity"],
             "input_frame": "source", "mode": "scale_only", "fit_tolerance_m": 0.,
             "constraints": rows if rows is not None else [distance()]}
    value.update(changes)
    return value


def control(identifier, source, reference, **changes):
    row = {"id": identifier, "kind": "control", "role": "fit", "reference_id": identifier,
           "reference_unit": "m", "source_xyz": source, "reference_xyz": reference}
    row.update(changes)
    return row


class ScaleConstraintsTests(unittest.TestCase):
    def test_one_known_distance_only_sets_scale_and_never_model_accuracy(self):
        data = dataset()
        original = copy.deepcopy(data)
        result = solver.solve_scale_constraints(data, request(data))
        self.assertEqual(result["source_to_metric"]["s"], .25)
        self.assertEqual(result["source_to_metric"]["R"], np.eye(3).tolist())
        self.assertEqual(result["source_to_metric"]["t"], [0., 0., 0.])
        self.assertEqual(result["source_to_metric"]["inverse"]["s"], 4.)
        self.assertEqual(result["independent_checks"]["status"], "not_provided")
        self.assertEqual(result["model_geometry_accuracy"], "not_measured")
        self.assertEqual(data, original)

    def test_metric_weighted_scale_uses_all_distances_not_mean_ratios(self):
        data = dataset()
        rows = [distance("first", source_b=[1., 0., 0.], reference_distance=200.,
                         reference_unit="cm", reference_sigma=50.),
                distance("second", source_b=[3., 0., 0.], reference_distance=6200.,
                         reference_unit="mm", reference_sigma=1000.)]
        result = solver.solve_scale_constraints(data, request(data, rows, fit_tolerance_m=.1))
        self.assertAlmostEqual(result["source_to_metric"]["s"], 2.046153846153846, places=14)
        self.assertEqual(result["fit"]["source_constraint_count"], 2)
        self.assertEqual(result["fit"]["scalar_observation_count"], 2)
        self.assertEqual(result["reference_uncertainty"][0]["sigma_m"], .5)
        self.assertEqual(result["reference_uncertainty"][0]["effective_weight"], 4.)
        self.assertAlmostEqual(result["fit"]["max_residual_m"], .0615384615384613, places=13)

    def test_camera_baseline_uses_original_centers_and_records_holdout_calibration(self):
        data = dataset()
        for view in data["views"]:
            view["c2w_opencv"] = [[1., 0., 0., 99.], [0., 1., 0., 99.],
                                 [0., 0., 1., 99.], [0., 0., 0., 1.]]
        data = camera_dataset.refresh_dataset_identity(data)
        row = {"id": "rig-baseline", "kind": "baseline", "role": "fit", "reference_id": "caliper-1",
               "view_a": "a", "view_b": "b", "reference_distance": 100., "reference_unit": "cm"}
        result = solver.solve_scale_constraints(data, request(data, [row]))
        self.assertEqual(result["source_to_metric"]["s"], .25)
        self.assertEqual(result["geometry_diagnostics"]["fit_nontraining_camera_ids"], ["b"])
        self.assertEqual(data["splits"]["test"], ["b"])
        row["source_b"] = [5., 0., 0.]
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(data, request(data, [row]))
        self.assertEqual(raised.exception.code, "scale_constraints_baseline_endpoint_conflict")

    def test_heldout_check_failure_never_changes_the_fitted_transform(self):
        data = dataset()
        check = distance("independent", role="check", source_a=[0., 2., 0.], source_b=[0., 2., 8.],
                         reference_distance=2., independent_of_fit=True, tolerance_m=.01,
                         provenance={"source": "survey-run-b", "measurement_id": "tape-b-42"})
        good = solver.solve_scale_constraints(data, request(data, [distance(), check]))
        self.assertEqual(good["independent_checks"]["status"], "pass")
        check["reference_distance"] = 3.
        bad = solver.solve_scale_constraints(data, request(data, [distance(), check]))
        self.assertEqual(bad["independent_checks"]["status"], "fail")
        self.assertEqual(good["source_to_metric"], bad["source_to_metric"])
        self.assertEqual(good["fit"], bad["fit"])
        self.assertNotEqual(good["solution_sha256"], bad["solution_sha256"])
        check.pop("provenance")
        self.assertEqual(solver.solve_scale_constraints(data, request(data, [distance(), check]))
                         ["independent_checks"]["status"], "unverified")

    def test_reversed_or_reidentified_fit_geometry_cannot_be_an_independent_check(self):
        data = dataset()
        for changed in ({"source_a": [4., 0., 0.], "source_b": [0., 0., 0.]},
                        {"source_a": [0., 2., 0.], "source_b": [0., 6., 0.], "reference_id": "d1"}):
            with self.subTest(changed=changed):
                check = distance("check", role="check", **changed)
                with self.assertRaises(solver.ScaleConstraintError) as raised:
                    solver.solve_scale_constraints(data, request(data, [distance(), check]))
                self.assertEqual(raised.exception.code, "scale_constraints_fit_check_overlap")

    def test_control_scale_only_uses_correlated_pair_lengths_not_world_position(self):
        data = dataset()
        rows = [control("p0", [10., 20., 30.], [100., 200., 300.], weight=2.),
                control("p1", [12., 20., 30.], [100., 205., 300.]),
                control("p2", [10., 22., 30.], [95., 200., 300.])]
        result = solver.solve_scale_constraints(data, request(data, rows))
        self.assertAlmostEqual(result["source_to_metric"]["s"], 2.5, places=14)
        self.assertEqual(result["source_to_metric"]["R"], np.eye(3).tolist())
        self.assertEqual(result["source_to_metric"]["t"], [0., 0., 0.])
        self.assertEqual(result["fit"]["source_constraint_count"], 3)
        self.assertEqual(result["fit"]["derived_pair_count"], 3)
        self.assertEqual(result["fit"]["constraint_ids"], ["p0", "p1", "p2"])
        self.assertAlmostEqual(sum(pair["effective_weight"] for pair in result["fit"]["derived_pairs"]), 4.)
        self.assertTrue(result["fit"]["derived_pairs_correlated"])
        self.assertIsNone(result["fit"]["independent_observation_count"])

    def test_sim3_noncollinear_controls_restore_scale_rotation_and_large_origin(self):
        data = dataset()
        source = [[1000000., 2000000., -3000000.], [1000002., 2000000., -3000000.],
                  [1000000., 2000003., -3000000.], [1000000., 2000000., -2999996.]]
        target = [[5., 7., 11.], [5., 12., 11.], [-2.5, 7., 11.], [5., 7., 21.]]
        rows = [control("p" + str(index), a, b) for index, (a, b) in enumerate(zip(source, target))]
        result = solver.solve_scale_constraints(data, request(data, rows, mode="sim3", fit_tolerance_m=1e-8))
        np.testing.assert_allclose(result["source_to_metric"]["R"], [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]], atol=1e-13)
        self.assertAlmostEqual(result["source_to_metric"]["s"], 2.5, places=13)
        # SVD dispatch can change R by a few machine epsilons. At a million-unit
        # origin t=ybar-sR*xbar amplifies that harmless component drift. Use the
        # solver's declared 64-ULP replay budget for t, not a fixed absolute
        # component tolerance; keep actual mapped points and round trips at
        # the original 1e-8 metre acceptance boundary below.
        np.testing.assert_array_max_ulp(np.asarray(result["source_to_metric"]["t"]),
            np.asarray([5000005., -2499993., 7500011.]), maxulp=64)
        transform = result["source_to_metric"]
        mapped = transform["s"] * (np.asarray(source) @ np.asarray(transform["R"]).T) + transform["t"]
        np.testing.assert_allclose(mapped, target, rtol=0., atol=1e-8)
        inverse = transform["inverse"]
        restored = inverse["s"] * (mapped @ np.asarray(inverse["R"]).T) + inverse["t"]
        np.testing.assert_allclose(restored, source, rtol=0., atol=1e-8)
        self.assertEqual(result["fit"]["fit_tolerance_m"], 1e-8)
        self.assertEqual(result["geometry_diagnostics"]["source_rank"], 3)
        self.assertLess(result["fit"]["max_residual_m"], 1e-12)

    def test_sim3_distance_and_controls_share_the_declared_joint_objective(self):
        data = dataset()
        rows = [control("o", [0., 0., 0.], [3., 4., 5.]),
                control("x", [1., 0., 0.], [5., 4., 5.]),
                control("y", [0., 1., 0.], [3., 6., 5.]),
                distance("length", source_a=[2., 0., 0.], source_b=[3., 0., 0.], reference_distance=3.)]
        result = solver.solve_scale_constraints(data, request(data, rows, mode="sim3", fit_tolerance_m=1.))
        self.assertAlmostEqual(result["source_to_metric"]["s"], 17. / 7., places=14)
        np.testing.assert_allclose(result["source_to_metric"]["t"], [20. / 7., 27. / 7., 5.], atol=1e-14)
        self.assertTrue(result["geometry_diagnostics"]["planar_handedness_limitation"])

    def test_sim3_and_scale_only_controls_have_separate_heldout_checks(self):
        data = dataset()
        fit = [control("o", [0., 0., 0.], [3., 4., 5.]),
               control("x", [1., 0., 0.], [5., 4., 5.]),
               control("y", [0., 1., 0.], [3., 6., 5.])]
        check = control("z-check", [0., 0., 1.], [3., 4., 7.], role="check",
                        independent_of_fit=True, tolerance_m=0.,
                        provenance={"source": "independent-survey", "measurement_id": "point-z"})
        report = solver.solve_scale_constraints(data, request(data, fit + [check], mode="sim3"))
        self.assertEqual(report["independent_checks"]["status"], "pass")
        self.assertEqual(report["independent_checks"]["per_constraint"][0]["kind"], "control")
        checks = [control("check1", [10., 0., 0.], [100., 200., 300.], role="check", independent_of_fit=True,
                          tolerance_m=0., provenance={"source": "survey-b", "measurement_id": "q1"}),
                  control("check2", [10., 0., 8.], [100., 202., 300.], role="check", independent_of_fit=True,
                          tolerance_m=0., provenance={"source": "survey-b", "measurement_id": "q2"})]
        result = solver.solve_scale_constraints(data, request(data, [distance()] + checks))
        self.assertEqual(result["independent_checks"]["status"], "pass")
        self.assertEqual(result["independent_checks"]["derived_pair_count"], 1)
        self.assertTrue(result["independent_checks"]["derived_pairs_correlated"])

    def test_replay_refuses_even_digest_only_tampering_and_changed_source_identity(self):
        data = dataset()
        value = request(data)
        solution = solver.solve_scale_constraints(data, value)
        self.assertEqual(solver.validate_scale_solution(data, value, solution), solution)
        changed = copy.deepcopy(solution)
        changed["solution_sha256"] = "0" * 64
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.validate_scale_solution(data, value, changed)
        self.assertEqual(raised.exception.code, "scale_constraints_solution_mismatch")
        data["views"][0]["source_c2w_opencv"][0][3] = 1.
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.validate_scale_solution(data, value, solution)
        self.assertEqual(raised.exception.code, "scale_constraints_dataset_identity_mismatch")

    def test_model_input_envelope_needs_no_fake_cameras_or_training_splits(self):
        model_input = {"data_identity": "9" * 64, "views": []}
        result = solver.solve_scale_constraints(model_input, request(model_input))
        self.assertEqual(result["source_to_metric"]["s"], .25)
        self.assertEqual(result["input_data_identity"], "9" * 64)
        baseline = {"id": "base", "kind": "baseline", "role": "fit", "reference_id": "base",
                    "reference_unit": "m", "reference_distance": 1., "view_a": "a", "view_b": "b"}
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(model_input, request(model_input, [baseline]))
        self.assertEqual(raised.exception.code, "scale_constraints_unknown_view")

    def test_invalid_uncertainty_and_unrepresentable_units_are_typed_refusals(self):
        data = dataset()
        for row in (distance(reference_sigma=0.), distance(reference_sigma=-1.), distance(reference_sigma=None),
                    distance(reference_sigma=1e-323, reference_unit="mm"),
                    distance(reference_distance=float("inf")), distance(weight=False),
                    distance(source_b=[0., 0., 0.])):
            with self.subTest(row=row), self.assertRaises(solver.ScaleConstraintError):
                solver.solve_scale_constraints(data, request(data, [row]))

    def test_cancellation_during_a_control_pair_solve_leaves_inputs_unchanged(self):
        data = dataset()
        rows = [control("p" + str(index), [float(index), float(index % 3), 1.],
                        [2. * index, 2. * (index % 3), 2.]) for index in range(8)]
        value = request(data, rows)
        original = copy.deepcopy(value)
        calls = [0]
        def cancelled():
            calls[0] += 1
            return calls[0] > 12
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(data, value, cancelled)
        self.assertEqual(raised.exception.code, "scale_constraints_cancelled")
        self.assertEqual(value, original)

    def test_control_pairs_cannot_repackage_a_fitted_distance_as_heldout_geometry(self):
        data = dataset()
        checks = [control("q0", [0., 0., 0.], [0., 0., 0.], role="check", tolerance_m=0.,
                          independent_of_fit=True, provenance={"source": "relabelled", "measurement_id": "q0"}),
                  control("q1", [4., 0., 0.], [1., 0., 0.], role="check", tolerance_m=0.,
                          independent_of_fit=True, provenance={"source": "relabelled", "measurement_id": "q1"})]
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(data, request(data, [distance()] + checks))
        self.assertEqual(raised.exception.code, "scale_constraints_fit_check_overlap")

    def test_conflicting_fit_preserves_raw_residual_diagnostics(self):
        data = dataset()
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(data, request(data, [distance(), distance("other", reference_distance=2.)]))
        self.assertEqual(raised.exception.code, "scale_constraints_fit_conflict")
        self.assertEqual(raised.exception.diagnostics["max_residual_m"], .5)
        self.assertFalse(raised.exception.diagnostics["accepted"])

    def test_sim3_reflection_and_collinear_correspondences_are_refused(self):
        data = dataset()
        tetra = [[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        reflected = [[0., 0., 0.], [-1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        cases = [("scale_constraints_reflection_unsupported", tetra, reflected),
                 ("scale_constraints_degenerate_controls", [[0., 0., 0.], [1., 0., 0.], [2., 0., 0.]],
                  [[0., 0., 0.], [2., 0., 0.], [4., 0., 0.]])]
        for code, source, reference in cases:
            rows = [control("p" + str(index), a, b) for index, (a, b) in enumerate(zip(source, reference))]
            with self.subTest(code=code), self.assertRaises(solver.ScaleConstraintError) as raised:
                solver.solve_scale_constraints(data, request(data, rows, mode="sim3", fit_tolerance_m=100.))
            self.assertEqual(raised.exception.code, code)

    def test_check_provenance_or_threshold_missing_never_becomes_a_pass(self):
        data = dataset()
        complete = distance("q", role="check", source_a=[0., 1., 0.], source_b=[0., 1., 8.],
                            reference_distance=2., independent_of_fit=True, tolerance_m=0.,
                            provenance={"source": "survey-b", "measurement_id": "check-q"})
        for missing in ("independent_of_fit", "tolerance_m", "provenance"):
            check = copy.deepcopy(complete)
            check.pop(missing)
            with self.subTest(missing=missing):
                result = solver.solve_scale_constraints(data, request(data, [distance(), check]))
                self.assertEqual(result["independent_checks"]["status"], "unverified")
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(data, request(data, [complete]))
        self.assertEqual(raised.exception.code, "scale_constraints_no_fit_constraints")

    def test_solution_json_roundtrip_and_provenance_are_replayable_not_uncertainty_estimates(self):
        data = dataset()
        row = distance(provenance={"source": "external operator", "measurement_id": "first measurement",
                                   "uncertainty_notes": "not supplied"})
        value = request(data, [row])
        result = solver.solve_scale_constraints(data, value)
        reloaded = json.loads(json.dumps(result, ensure_ascii=False, allow_nan=False))
        self.assertEqual(solver.validate_scale_solution(data, value, reloaded), result)
        self.assertEqual(result["reference_uncertainty"][0]["status"], "unknown")
        self.assertIsNone(result["reference_uncertainty"][0]["sigma_m"])
        self.assertEqual(result["request"]["constraints"][0]["provenance"], row["provenance"])

    def test_explicit_pair_expansion_and_raw_request_limits_never_truncate_inputs(self):
        data = dataset()
        many = [control("p" + str(index), [float(index), float(index % 7), float(index % 11)],
                        [2. * index, 2. * (index % 7), 2. * (index % 11)]) for index in range(449)]
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.solve_scale_constraints(data, request(data, many))
        self.assertEqual(raised.exception.code, "scale_constraints_resource_limit")
        self.assertEqual(raised.exception.diagnostics["requested_derived_pairs"], 100576)
        # Sim3 operates on controls linearly, not through pair expansion.
        aligned = solver.solve_scale_constraints(data, request(data, many, mode="sim3"))
        self.assertAlmostEqual(aligned["source_to_metric"]["s"], 2., places=13)
        too_large = request(data)
        too_large["notes"] = "x" * (16 * 1024 * 1024)
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.canonical_request(too_large)
        self.assertEqual(raised.exception.code, "scale_constraints_resource_limit")

    def test_numeric_replay_preserves_original_identity_but_never_changes_a_check_verdict(self):
        data = dataset()
        rows = [control("o", [0., 0., 0.], [3., 4., 5.]),
                control("x", [1., 0., 0.], [5., 4., 5.]),
                control("y", [0., 1., 0.], [3., 6., 5.])]
        value = request(data, rows, mode="sim3")
        original = solver.solve_scale_constraints(data, value)
        alternate = copy.deepcopy(original)
        alternate["source_to_metric"]["R"][0][0] += 2e-16
        alternate["fit"]["max_residual_m"] += 1e-15
        alternate["solution_sha256"] = solver.solution_sha256(alternate)
        self.assertEqual(solver.validate_scale_solution(data, value, alternate), alternate)
        invalid = copy.deepcopy(alternate)
        invalid["fit"]["accepted"] = False
        invalid["solution_sha256"] = solver.solution_sha256(invalid)
        with self.assertRaises(solver.ScaleConstraintError):
            solver.validate_scale_solution(data, value, invalid)

    def test_malformed_or_overflowing_external_numbers_never_escape_as_raw_exceptions(self):
        data = dataset()
        for row in (distance(reference_unit=[]), distance(reference_distance=10 ** 1000),
                    distance(source_b=[1e308, 0., 0.])):
            with self.subTest(row=str(row)[:100]), self.assertRaises(solver.ScaleConstraintError):
                solver.solve_scale_constraints(data, request(data, [row]))

    def test_replay_cannot_promote_failed_checks_even_with_a_recomputed_digest(self):
        data = dataset()
        check = distance("check", role="check", source_a=[0., 1., 0.], source_b=[0., 1., 8.],
                         reference_distance=2.0000000001, independent_of_fit=True, tolerance_m=0.,
                         provenance={"source": "survey-b", "measurement_id": "check"})
        value = request(data, [distance(), check])
        solution = solver.solve_scale_constraints(data, value)
        self.assertEqual(solution["independent_checks"]["status"], "fail")
        self.assertGreater(solution["independent_checks"]["per_constraint"][0]["residual_m"], 0.)
        solution["independent_checks"]["status"] = "pass"
        solution["independent_checks"]["per_constraint"][0]["accepted"] = True
        solution["independent_checks"]["per_constraint"][0]["status"] = "pass"
        solution["solution_sha256"] = solver.solution_sha256(solution)
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.validate_scale_solution(data, value, solution)
        self.assertEqual(raised.exception.code, "scale_constraints_solution_mismatch")

    def test_numeric_replay_only_excludes_the_top_level_solution_digest(self):
        data = dataset()
        value = request(data)
        value["notes"] = {"solution_sha256": "user-provided-note-not-a-derived-digest"}
        solution = solver.solve_scale_constraints(data, value)
        solution["request"]["notes"]["solution_sha256"] = "changed source note"
        solution["solution_sha256"] = solver.solution_sha256(solution)
        with self.assertRaises(solver.ScaleConstraintError) as raised:
            solver.validate_scale_solution(data, value, solution)
        self.assertEqual(raised.exception.code, "scale_constraints_solution_mismatch")


if __name__ == "__main__":
    unittest.main()
