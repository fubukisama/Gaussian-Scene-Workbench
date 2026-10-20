"""Public independent-evaluation behavior, not training-fit assertions."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import copy

import numpy as np

from native.backend.geometry_evaluation import GeometryEvaluationError, evaluate_geometry


def identity(frame="survey"):
    return {"s": 1., "R": [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]],
            "t": [0., 0., 0.], "input_frame": frame, "output_frame": "survey"}


def write_ply(path, points, faces=()):
    header = "ply\nformat ascii 1.0\nelement vertex {}\nproperty double x\nproperty double y\nproperty double z\n".format(len(points))
    if len(faces):
        header += "element face {}\nproperty list uchar int vertex_indices\n".format(len(faces))
    header += "end_header\n"
    body = "".join("{} {} {}\n".format(*point) for point in points)
    body += "".join("3 {} {} {}\n".format(*face) for face in faces)
    path.write_text(header + body, encoding="utf-8")


class GeometryEvaluationTests(unittest.TestCase):
    def test_skinny_triangle_interior_is_surface_not_nearest_edge(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate, reference = root / "sample.ply", root / "surface.ply"
            write_ply(candidate, [[7.5e7, .25, 0.]])
            write_ply(reference, [[0., 0., 0.], [1e8, 0., 0.], [1e8, 1., 0.]], [[0, 1, 2]])
            request = self._review_request(candidate, reference)
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            self.assertEqual(report["candidate_to_reference"]["statistics"]["rmse"], 0.)

    @staticmethod
    def _review_request(candidate, reference):
        return {"schema": "gsw.geometry_evaluation_request/1", "evaluation_frame": {"id": "survey", "unit": "m"},
            "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
            "reference": {"path": str(reference), "frame": "survey", "unit": "m", "to_evaluation": identity(),
                "provenance": {"role": "independent_reference", "source": "independent analytic fixture"}},
            "sampling": {"count": 4, "seed": 0}, "coverage_threshold": .01}

    def test_fractional_mesh_indices_unknown_sampling_and_self_reference_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate, reference = root / "sample.ply", root / "surface.ply"
            write_ply(candidate, [[0., 0., 0.]])
            write_ply(reference, [[0., 0., 0.], [1., 0., 0.], [0., 1., 0.]], [[0, 1, 2]])
            request = self._review_request(candidate, reference)
            request["sampling"]["outlier_filter"] = {"drop_fraction": .9}
            with self.assertRaisesRegex(GeometryEvaluationError, "geometry_unsupported_sampling_options"):
                evaluate_geometry(request, root / "ignored-filter")
            request["sampling"].pop("outlier_filter")
            request["reference"]["path"] = str(candidate)
            with self.assertRaisesRegex(GeometryEvaluationError, "geometry_reference_is_candidate"):
                evaluate_geometry(request, root / "self-reference")
            alias = root / "hardlink.ply"
            os.link(candidate, alias)
            request["reference"]["path"] = str(alias)
            with self.assertRaisesRegex(GeometryEvaluationError, "geometry_reference_is_candidate"):
                evaluate_geometry(request, root / "aliased-reference")
            request["reference"]["path"] = str(reference)
            reference.write_text("ply\nformat ascii 1.0\nelement vertex 3\nproperty double x\nproperty double y\nproperty double z\n"
                "element face 1\nproperty list uchar float vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1.9 2\n", encoding="utf-8")
            with self.assertRaisesRegex(GeometryEvaluationError, "geometry_invalid_ply_indices"):
                evaluate_geometry(request, root / "fractional-indices")

    def test_nested_transform_geometry_roi_and_dimension_controls_are_not_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate, reference = root / "sample.ply", root / "surface.ply"
            write_ply(candidate, [[0., 0., 0.]])
            write_ply(reference, [[0., 0., 0.], [1., 0., 0.], [0., 1., 0.]], [[0, 1, 2]])
            base = self._review_request(candidate, reference)
            cases = []
            item = copy.deepcopy(base); item["candidate"]["to_evaluation"]["auto_scale_fit"] = True; cases.append(item)
            item = copy.deepcopy(base); item["candidate"]["smoothing_iterations"] = 10; cases.append(item)
            item = copy.deepcopy(base); item["evaluation_frame"]["auto_units"] = True; cases.append(item)
            item = copy.deepcopy(base); item["roi"] = {"min": [0, 0, 0], "max": [1, 1, 1],
                "frame": "survey", "unit": "m", "clip_targets": True}; cases.append(item)
            item = copy.deepcopy(base); item["dimensions"] = [{"id": "x", "kind": "axis_extent", "axis": [1, 0, 0],
                "trim_extremes": True}]; cases.append(item)
            item = copy.deepcopy(base); item["candidate"]["to_evaluation"]["t"] = ["1", 0, 0]; cases.append(item)
            for index, item in enumerate(cases):
                with self.subTest(index=index), self.assertRaises(GeometryEvaluationError):
                    evaluate_geometry(item, root / ("invalid-" + str(index)))

    def test_equivalent_units_and_large_origin_preserve_known_frame_measurements(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            physical = np.asarray([[0., 0., 2.25], [2., 0., 2.25], [2., 3., 2.25], [0., 3., 2.25]])
            cases = (("m", 1., 0.), ("mm", .001, 0.), ("mm", .001, 1e9))
            for index, (unit, factor, origin) in enumerate(cases):
                path = root / ("candidate-{}.ply".format(index))
                write_ply(path, physical / factor + origin, [[0, 1, 2], [0, 2, 3]])
                transform = identity("input")
                transform.update(s=factor, t=[-origin * factor] * 3)
                request = {"schema": "gsw.geometry_evaluation_request/1",
                    "evaluation_frame": {"id": "survey", "unit": "m"},
                    "candidate": {"path": str(path), "frame": "input", "unit": unit, "to_evaluation": transform},
                    "reference": {"kind": "plane_patch", "origin": [0., 0., 2.], "axis_u": [1., 0., 0.],
                        "axis_v": [0., 1., 0.], "range_u": [0., 2.], "range_v": [0., 3.],
                        "frame": "survey", "unit": "m", "to_evaluation": identity(),
                        "provenance": {"role": "independent_reference", "source": "analytic"}},
                    "sampling": {"count": 16, "seed": 8, "reference_grid_shape": [4, 4]}, "coverage_threshold": .1}
                output = evaluate_geometry(request, root / ("result-{}".format(index)))
                report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
                self.assertAlmostEqual(report["candidate_to_reference"]["statistics"]["rmse"], .25, places=8)
                self.assertAlmostEqual(report["reference_to_candidate"]["statistics"]["rmse"], .25, places=8)
                self.assertEqual(report["completeness"]["missing_count"], 16)
                self.assertEqual(report["candidate"]["transform"]["scale_relative_to_unit_conversion"], 1.)

    def test_empty_reconstruction_reports_all_reference_samples_missing_not_zero_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            empty = root / "empty.ply"
            write_ply(empty, [])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(empty), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [0., 1.], "range_v": [0., 1.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "analytic"}},
                "sampling": {"count": 16, "seed": 8}, "coverage_threshold": .1}
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "no_reconstruction")
            self.assertEqual(report["completeness"]["missing_count"], 16)
            self.assertEqual(report["reference_to_candidate"]["statistics"]["missing_or_nonfinite_count"], 16)
            self.assertIsNone(report["reference_to_candidate"]["statistics"]["rmse"])
            with self.assertRaisesRegex(GeometryEvaluationError, "fresh_output_directory_required"):
                evaluate_geometry(request, root / "result")

    def test_depth_geometry_manifest_binds_actual_points_and_declared_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            product = root / "geometry"
            product.mkdir()
            cloud = product / "depth-cloud.ply"
            write_ply(cloud, [[-.25, -.25, 1.], [.25, -.25, 1.], [-.25, .25, 1.], [.25, .25, 1.]])
            geometry = {"kind": "depth_point_cloud", "path": cloud.name,
                "sha256": hashlib.sha256(cloud.read_bytes()).hexdigest(), "point_count": 4,
                "frame": "canonical", "frame_id": "fixture-data/canonical", "unit": "m", "meters_per_unit": 1.,
                "scale_status": "source_declared", "fusion": "none", "surface_semantics": "appearance_proxy_not_material_boundary"}
            manifest = product / "manifest.json"
            manifest.write_text(json.dumps({"schema": "gsw.gs_geometry/1", "geometry": geometry,
                "source": {"data_identity": "a" * 64, "backend": "3dgs"}, "frames": {"note": "fixture-known-frame"}}), encoding="utf-8")
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"manifest_path": str(manifest), "to_evaluation": identity("fixture-data/canonical")},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 1.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [-.5, .5], "range_v": [-.5, .5],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "analytic"}},
                "sampling": {"count": 4, "seed": 0}, "coverage_threshold": .1}
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            self.assertEqual(report["candidate"]["geometry_manifest"]["geometry"]["surface_semantics"], "appearance_proxy_not_material_boundary")
            self.assertEqual(report["candidate"]["frame"], "fixture-data/canonical")
            self.assertEqual(report["candidate_to_reference"]["statistics"]["sample_count"], 4)
            cloud.write_bytes(cloud.read_bytes() + b"\n")
            with self.assertRaisesRegex(GeometryEvaluationError, "input_hash_mismatch"):
                evaluate_geometry(request, root / "tampered")
            self.assertFalse((root / "tampered").exists())

    def test_free_scale_fit_is_diagnostic_and_never_replaces_original_scale_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference.ply"
            candidate = root / "candidate.ply"
            expected = np.asarray([[0., 0., 1.], [2., 0., 1.], [2., 1., 1.], [0., 1., 1.]])
            write_ply(reference, expected, [[0, 1, 2], [0, 2, 3]])
            write_ply(candidate, expected * 2. + [5., -3., 2.], [[0, 1, 2], [0, 2, 3]])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"path": str(reference), "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "independent-anchors"}},
                "sampling": {"count": 64, "seed": 12}, "coverage_threshold": .02,
                "dimensions": [{"id": "width", "kind": "axis_extent", "axis": [1., 0., 0.]}],
                "diagnostic_registration": {"kind": "similarity", "candidate_indices": [0, 1, 2, 3],
                    "reference_indices": [0, 1, 2, 3],
                    "region": {"min": [-1., -4., 0.], "max": [10., 3., 5.], "frame": "survey", "unit": "m"}}}
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            self.assertEqual(report["dimensions"][0]["relative_bias"], 1.)
            self.assertGreater(report["candidate_to_reference"]["statistics"]["rmse"], 1.)
            diagnostic = report["diagnostic_registration"]
            self.assertAlmostEqual(diagnostic["transform"]["s"], .5, places=12)
            self.assertEqual(diagnostic["degrees_of_freedom"], 7)
            self.assertTrue(diagnostic["main_result_unchanged"])
            self.assertLess(diagnostic["after"]["candidate_to_reference"]["statistics"]["rmse"], 1e-10)
            self.assertFalse(report["physical_model_accuracy_certified"])

    def test_cli_finishes_with_open_control_pipe_and_cancel_preserves_existing_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "candidate.ply"
            write_ply(candidate, [[0., 0., .2]])
            source_bytes = candidate.read_bytes()
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": "candidate.ply", "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [-1., 1.], "range_v": [-1., 1.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "analytic"}},
                "sampling": {"count": 4, "seed": 0}, "coverage_threshold": .01}
            config = root / "request.json"
            config.write_text(json.dumps(request), encoding="utf-8")
            script = Path(__file__).resolve().parents[2] / "worker/geometry_evaluation.py"
            command = [sys.executable, "-B", str(script), "--config", str(config), "--output-directory", str(root / "result")]
            environment = dict(os.environ, PYTHONUTF8="1", PYTHONDONTWRITEBYTECODE="1")
            with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  env=environment) as process:
                try:
                    self.assertEqual(process.wait(timeout=15), 0, process.stderr.read().decode())
                finally:
                    process.stdin.close()
                report = json.loads(process.stdout.read())
                self.assertTrue(report["success"])
            command[-1] = str(root / "cancelled")
            cancelled = subprocess.run(command, input=b"cancel\n", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       timeout=15, env=environment)
            self.assertEqual(cancelled.returncode, 130, cancelled.stderr.decode())
            self.assertEqual(json.loads(cancelled.stdout)["code"], "geometry_evaluation_cancelled")
            self.assertFalse((root / "cancelled").exists())
            self.assertTrue((root / "result/report.json").is_file())
            self.assertEqual(candidate.read_bytes(), source_bytes)

    def test_raw_gaussian_centres_are_not_silently_treated_as_surface_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "gaussians.ply"
            candidate.write_text("ply\nformat ascii 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\n"
                "property float opacity\nproperty float f_dc_0\nproperty float scale_0\nend_header\n0 0 1 2 .1 -4\n", encoding="utf-8")
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [-1., 1.], "range_v": [-1., 1.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "analytic"}},
                "sampling": {"count": 4, "seed": 0}, "coverage_threshold": .01}
            with self.assertRaisesRegex(GeometryEvaluationError, "gaussian_centres_not_surface"):
                evaluate_geometry(request, root / "result")
            self.assertFalse((root / "result").exists())

    def test_unknown_controls_or_unknown_metric_units_are_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "point.ply"
            write_ply(candidate, [[0., 0., .1]])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [-1., 1.], "range_v": [-1., 1.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "analytic"}},
                "sampling": {"count": 4, "seed": 0}, "coverage_threshold": .01,
                "silently_fit_scale": True}
            with self.assertRaisesRegex(GeometryEvaluationError, "unsupported_evaluation_options"):
                evaluate_geometry(request, root / "unknown-control")
            self.assertFalse((root / "unknown-control").exists())
            del request["silently_fit_scale"]
            request["candidate"]["unit"] = "unknown"
            with self.assertRaisesRegex(GeometryEvaluationError, "unknown_or_incompatible_units"):
                evaluate_geometry(request, root / "unknown-unit")
            self.assertFalse((root / "unknown-unit").exists())

    def test_explicit_roi_retains_missing_reference_region(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "partial.ply"
            write_ply(candidate, [[0., 0., 0.], [.5, 0., 0.], [.5, 1., 0.], [0., 1., 0.]], [[0, 1, 2], [0, 2, 3]])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [0., 1.], "range_v": [0., 1.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "whole-plane"}},
                "sampling": {"count": 32, "seed": 4, "reference_grid_shape": [8, 4]}, "coverage_threshold": .01,
                "roi": {"min": [.5, 0., -.01], "max": [1., 1., .01], "frame": "survey", "unit": "m"}}
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            self.assertEqual(report["roi"]["definition"], "query_samples_in_closed_aabb_targets_not_clipped")
            self.assertEqual(report["candidate_to_reference"]["statistics"]["sample_count"], 0)
            self.assertIsNone(report["candidate_to_reference"]["statistics"]["mean"])
            self.assertEqual(report["completeness"]["reference_sample_count"], 16)
            self.assertEqual(report["completeness"]["missing_count"], 16)
            self.assertEqual(report["sampling"]["reference_outside_roi"], 16)

    def test_missing_half_plane_is_not_hidden_by_zero_one_way_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "half.ply"
            write_ply(candidate, [[0., 0., 0.], [.5, 0., 0.], [.5, 1., 0.], [0., 1., 0.]], [[0, 1, 2], [0, 2, 3]])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.],
                    "axis_v": [0., 1., 0.], "range_u": [0., 1.], "range_v": [0., 1.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "whole-plane"}},
                "sampling": {"count": 16, "seed": 19, "reference_grid_shape": [4, 4]}, "coverage_threshold": .01}
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            self.assertEqual(report["sampling"]["reference_grid_shape"], [4, 4])
            self.assertAlmostEqual(report["candidate_to_reference"]["statistics"]["rmse"], 0., places=12)
            self.assertEqual(report["completeness"]["covered_count"], 8)
            self.assertEqual(report["completeness"]["missing_count"], 8)
            self.assertEqual(report["completeness"]["coverage_fraction"], .5)
            self.assertGreater(report["reference_to_candidate"]["statistics"]["rmse"], .1)
            with np.load(output["distancesPath"], allow_pickle=False) as samples:
                self.assertEqual(np.count_nonzero(~samples["reference_covered"]), 8)

    def test_explicit_dimension_keeps_original_scale_bias_visible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "scale.ply"
            reference = root / "reference.ply"
            write_ply(candidate, [[10., 2., 0.], [15.5, 2., 0.], [15.5, 5., 0.], [10., 5., 0.]], [[0, 1, 2], [0, 2, 3]])
            write_ply(reference, [[10., 2., 0.], [15., 2., 0.], [15., 5., 0.], [10., 5., 0.]], [[0, 1, 2], [0, 2, 3]])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": identity()},
                "reference": {"path": str(reference), "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "known-plane"}},
                "sampling": {"count": 32, "seed": 1}, "coverage_threshold": .02,
                "dimensions": [{"id": "span", "kind": "axis_extent", "axis": [1., 0., 0.]}]}
            output = evaluate_geometry(request, root / "result")
            report = json.loads(Path(output["reportPath"]).read_text(encoding="utf-8"))
            measurement = report["dimensions"][0]
            self.assertEqual(measurement["id"], "span")
            self.assertEqual(measurement["candidate_value"], 5.5)
            self.assertEqual(measurement["reference_value"], 5.)
            self.assertEqual(measurement["signed_bias"], .5)
            self.assertAlmostEqual(measurement["relative_bias"], .1)
            self.assertEqual(report["alignment"]["main"], "explicit_recorded_known_transforms")

    def test_known_plane_offset_reports_both_directions_and_missing_surface(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "candidate.ply"
            write_ply(candidate, [[-1., 4., 2.25], [4., 4., 2.25],
                                  [4., 7., 2.25], [-1., 7., 2.25]], [[0, 1, 2], [0, 2, 3]])
            request = {"schema": "gsw.geometry_evaluation_request/1",
                "evaluation_frame": {"id": "survey", "unit": "m"},
                "candidate": {"path": str(candidate), "frame": "survey", "unit": "m",
                              "to_evaluation": identity()},
                "reference": {"kind": "plane_patch", "origin": [-1., 4., 2.],
                    "axis_u": [1., 0., 0.], "axis_v": [0., 1., 0.],
                    "range_u": [0., 5.], "range_v": [0., 3.],
                    "frame": "survey", "unit": "m", "to_evaluation": identity(),
                    "provenance": {"role": "independent_reference", "source": "analytic-plane"}},
                "sampling": {"count": 256, "seed": 7}, "coverage_threshold": .1,
                "quantiles": [0., .5, .9, .95, 1.]}
            result = evaluate_geometry(request, root / "result")
            report = json.loads(Path(result["reportPath"]).read_text(encoding="utf-8"))
            for direction in ("candidate_to_reference", "reference_to_candidate"):
                stats = report[direction]["statistics"]
                self.assertEqual(stats["sample_count"], 256)
                self.assertAlmostEqual(stats["mean"], .25, places=6)
                self.assertAlmostEqual(stats["median"], .25, places=6)
                self.assertAlmostEqual(stats["rmse"], .25, places=6)
            self.assertEqual(report["completeness"]["covered_count"], 0)
            self.assertEqual(report["completeness"]["missing_count"], 256)
            self.assertEqual(report["completeness"]["coverage_fraction"], 0.)
            self.assertFalse(report["physical_model_accuracy_certified"])
            with np.load(result["distancesPath"], allow_pickle=False) as arrays:
                self.assertEqual(arrays["reference_covered"].sum(), 0)
                np.testing.assert_allclose(arrays["candidate_distances"], .25, atol=1e-6)


if __name__ == "__main__":
    unittest.main()
