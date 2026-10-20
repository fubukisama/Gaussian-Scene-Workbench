"""Initialization recipes through the admitted-data public inspection seam."""
import copy
import math
import os
from pathlib import Path
import tempfile
import unittest

import numpy as np
from PIL import Image

from native.backend import camera_dataset
from native.backend import initialization


IDENTITY = [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]


def looking_at(center, target):
    forward = np.asarray(target, dtype=float) - np.asarray(center, dtype=float)
    forward /= np.linalg.norm(forward)
    right = np.cross([0., 1., 0.], forward)
    right /= np.linalg.norm(right)
    up = np.cross(forward, right)
    pose = np.eye(4)
    pose[:3, :3], pose[:3, 3] = np.column_stack((right, up, forward)), center
    return pose.tolist()


class InitializationTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_INITIALIZATION_TEST_ROOT", tempfile.gettempdir()))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def dataset(self, transform=None, views=None, scale=None, normalization=None):
        Image.new("RGB", (16, 12), (31, 61, 91)).save(self.root / "image.png")
        pose = copy.deepcopy(IDENTITY)
        pose[0][3], pose[1][3] = .5, .5
        data = {"schema": camera_dataset.SCHEMA, "pose_policy": "locked",
            "source": {"kind": "synthetic"}, "scale": scale or {"status": "arbitrary"},
            "views": views or [{"view_id": "train-a", "image_path": "image.png", "split": "train",
                "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]],
                "pixel_coordinates": "pixel_border", "c2w_opencv": pose}]}
        if transform is not None:
            data["transforms"] = {"source_to_canonical": transform}
        if normalization is not None:
            data.setdefault("transforms", {})["canonical_to_training"] = normalization
        path = self.root / camera_dataset.MANIFEST_NAME
        camera_dataset.write_camera_dataset(data, path)
        return camera_dataset.load_camera_dataset(path)

    def request(self, dataset, **changes):
        value = {"schema": "gsw.initialization_request/1", "input_data_identity": dataset["data_identity"],
            "backend": "3dgs", "resolution": 1, "initialization": {"kind": "random", "count": 4,
                "seed": 0, "frame": "source", "bounds": [[0., 0., 2.], [1., 1., 3.]], "priors": []}}
        value.update(changes)
        return value

    def test_random_points_are_sampled_in_declared_box_before_a_single_similarity(self):
        dataset = self.dataset({"s": 2., "R": [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]],
                                "t": [10., 20., 0.]})
        report = initialization.inspect_initialization(dataset, self.root, self.request(dataset))
        self.assertEqual(report["schema"], "gsw.initialization_report/1")
        self.assertEqual(report["status"], "ready")
        self.assertEqual(report["point_count"], 4)
        self.assertEqual(report["frames"]["canonical_unit"], "arbitrary")
        # Worked MT19937 seed-0 unit-cube sample, then s=2/Rz=90/t=(10,20,0).
        np.testing.assert_allclose(report["preview"]["points"][0],
            [8.569621267255162, 21.097627007854648, 5.205526752143288], rtol=0., atol=1e-12)
        self.assertEqual(len(report["preview"]["bounds_corners"]), 8)
        self.assertEqual(report["coverage"]["per_view"][0]["inside_image"], 4)
        self.assertTrue(report["coverage"]["not_surface_evidence"])
        self.assertNotIn("npz_sha256", report)

    def test_inspection_refuses_a_recipe_bound_to_another_input_identity(self):
        dataset = self.dataset()
        request = self.request(dataset, input_data_identity="f" * 64)
        with self.assertRaises(initialization.InitializationError) as raised:
            initialization.inspect_initialization(dataset, self.root, request)
        self.assertEqual(raised.exception.code, "initialization_input_identity_mismatch")

    def test_unsupported_or_unsafe_recipe_options_are_rejected_not_silently_ignored(self):
        dataset = self.dataset()
        cases = [("unknown-option", {"extra": True}), ("backend", {"backend": "sugar"}),
                 ("schema", {"schema": "gsw.initialization_request/9"}),
                 ("boolean-resolution", {"resolution": True})]
        for name, update in cases:
            with self.subTest(name=name):
                with self.assertRaises(initialization.InitializationError):
                    initialization.inspect_initialization(dataset, self.root, self.request(dataset, **update))
        for field, value in (("seed", True), ("seed", 2**32), ("count", 3), ("count", 1.5),
                ("frame", "training"), ("bounds", [[0., 0., 0.], [0., 1., 1.]]),
                ("bounds", [[0., 0., 0.], [float("nan"), 1., 1.]]),
                ("unknown-active-option", 12), ("priors", ["ground_truth"])):
            with self.subTest(field=field, value=value):
                request = self.request(dataset)
                request["initialization"][field] = value
                with self.assertRaises(initialization.InitializationError):
                    initialization.inspect_initialization(dataset, self.root, request)

    def test_camera_coverage_alone_does_not_become_a_guessed_object_box(self):
        dataset = self.dataset()
        request = self.request(dataset)
        request["initialization"].pop("bounds")
        report = initialization.inspect_initialization(dataset, self.root, request)
        self.assertEqual(report["status"], "needs_input")
        self.assertEqual(report["point_count"], 0)
        self.assertIsNone(report["point_sha256"])
        self.assertEqual(report["preview"]["points"], [])
        self.assertIn("initialization_bounds_required", report["errors"])

    def test_stock_near_clip_and_float32_boundary_fail_with_actual_coverage_diagnostics(self):
        dataset = self.dataset(normalization={"mode": "explicit", "s": 1.,
            "R": [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]], "t": [0., 0., 0.]})
        for backend in ("3dgs", "2dgs"):
            for low, high in ((.05, .1), (.200000001, .200000002)):
                with self.subTest(backend=backend, depths=(low, high)):
                    request = self.request(dataset, backend=backend)
                    request["initialization"]["bounds"] = [[0., 0., low], [1., 1., high]]
                    report = initialization.inspect_initialization(dataset, self.root, request)
                    self.assertEqual(report["status"], "invalid")
                    self.assertIn("initialization_backend_near_excludes_all_points", report["errors"])
                    self.assertEqual(report["coverage"]["per_view"][0]["forward_past_backend_near"], 0)
                    self.assertEqual(report["point_count"], 4)
                    if backend == "2dgs":
                        self.assertEqual(report["clipping"]["per_pixel_minimum_depth"], .2)
                        self.assertEqual(report["clipping"]["distortion_far"], 100.)

    def test_shared_resolver_returns_the_actual_points_inspected_for_both_backends(self):
        dataset = self.dataset({"s": 2., "R": [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]],
                                "t": [10., 20., 0.]})
        declaration = self.request(dataset)["initialization"]
        configured = copy.deepcopy(dataset)
        configured["initialization"] = declaration
        configured = camera_dataset.refresh_dataset_identity(configured)
        points, colors, record = initialization.resolve_initialization_points(configured, self.root)
        for backend in ("3dgs", "2dgs"):
            report = initialization.inspect_initialization(configured, self.root,
                self.request(configured, backend=backend))
            np.testing.assert_array_equal(points, report["preview"]["points"])
            self.assertEqual(report["point_sha256"], record["point_sha256"])
        np.testing.assert_array_equal(colors, np.full((4, 3), .5))

    def test_explicit_point_cloud_is_hash_bound_and_remains_a_declared_geometry_prior(self):
        dataset = self.dataset()
        path = self.root / "prior.ply"
        path.write_text("ply\nformat ascii 1.0\nelement vertex 4\nproperty double x\nproperty double y\n"
            "property double z\nend_header\n0.4 0.4 2\n0.6 0.4 2\n0.4 0.6 2\n0.6 0.6 2\n", encoding="ascii")
        original = path.read_bytes()
        request = self.request(dataset, initialization={"kind": "external_point_cloud", "frame": "source",
            "path": "prior.ply", "provenance": {"source": "explicit independently supplied initialization"}})
        report = initialization.inspect_initialization(dataset, self.root, request)
        self.assertEqual(report["status"], "ready")
        self.assertEqual(report["point_count"], 4)
        self.assertIn("external_geometry", report["initialization"]["priors"])
        self.assertEqual(len(report["initialization"]["sha256"]), 64)
        self.assertEqual(path.read_bytes(), original)
        np.testing.assert_allclose(report["preview"]["points"],
            [[.4, .4, 2.], [.6, .4, 2.], [.4, .6, 2.], [.6, .6, 2.]], atol=0., rtol=0.)

    def test_report_echoes_the_exact_original_request_separately_from_normalized_defaults(self):
        dataset = self.dataset()
        request = self.request(dataset)
        request["initialization"].pop("priors")
        request["initialization"]["provenance"] = {"note": "三语言 / 日本語 / English"}
        original = copy.deepcopy(request)
        report = initialization.inspect_initialization(dataset, self.root, request)
        self.assertEqual(report["request"], original)
        self.assertEqual(request, original)
        self.assertNotIn("priors", report["request"]["initialization"])
        self.assertEqual(report["normalized_request"]["initialization"]["priors"], [])

    def test_axis_intersection_suggests_only_a_hypothesis_and_requires_explicit_extent(self):
        Image.new("RGB", (16, 12), (32, 62, 92)).save(self.root / "other.png")
        q = 1. / math.sqrt(5.)
        views = [{"view_id": "left", "split": "train", "image_path": "image.png",
            "pixel_coordinates": "pixel_border", "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]],
            "c2w_opencv": [[2*q, 0., q, -1.], [0., 1., 0., 0.], [-q, 0., 2*q, 0.], [0., 0., 0., 1.]]},
            {"view_id": "right", "split": "train", "image_path": "other.png",
             "pixel_coordinates": "pixel_border", "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]],
             "c2w_opencv": [[2*q, 0., -q, 1.], [0., 1., 0., 0.], [q, 0., 2*q, 0.], [0., 0., 0., 1.]]}]
        dataset = self.dataset(views=views)
        request = self.request(dataset, suggestion={"method": "optical_axis_convergence"})
        report = initialization.suggest_initialization(dataset, request)
        self.assertEqual(report["status"], "needs_input")
        self.assertNotIn("suggested_initialization", report)
        np.testing.assert_allclose(report["suggestion_diagnostics"]["center"], [0., 0., 2.], atol=1e-12)
        request["suggestion"]["half_extents"] = [.5, .5, 1.]
        report = initialization.suggest_initialization(dataset, request)
        self.assertEqual(report["status"], "suggestion_requires_confirmation")
        np.testing.assert_allclose(report["suggested_initialization"]["bounds"],
            [[-.5, -.5, 1.], [.5, .5, 3.]], atol=1e-12)
        self.assertIn("optical_axes_are_only_a_target_center_hypothesis", report["assumptions"])

    def test_depth_interval_uses_the_selected_training_camera_full_k_not_an_object_guess(self):
        pose = copy.deepcopy(IDENTITY)
        pose[0][3], pose[1][3] = .5, .5
        dataset = self.dataset(views=[{"view_id": "train-a", "split": "train", "image_path": "image.png",
            "pixel_coordinates": "pixel_border", "K": [[20., 0., 3.], [0., 10., 2.], [0., 0., 1.]],
            "c2w_opencv": pose}])
        request = self.request(dataset, suggestion={"method": "user_depth_interval", "view_id": "train-a",
                                                   "near": 2., "far": 4.})
        report = initialization.suggest_initialization(dataset, request)
        self.assertEqual(report["status"], "suggestion_requires_confirmation")
        np.testing.assert_allclose(report["suggested_initialization"]["bounds"],
            [[-.1, -.3, 2.], [3.1, 4.5, 4.]], atol=1e-12, rtol=0.)
        self.assertEqual(report["suggestion_diagnostics"]["depth_definition"], "camera_z")
        self.assertIn("selected_view_frustum_aabb_is_not_object_bounds", report["assumptions"])

    def test_cancelled_inspection_exits_before_reading_any_declared_geometry(self):
        dataset = self.dataset()
        request = self.request(dataset, initialization={"kind": "external_point_cloud", "frame": "source",
                                                       "path": "must-not-open-this-file.ply"})
        with self.assertRaises(initialization.InitializationError) as raised:
            initialization.inspect_initialization(dataset, self.root, request, cancelled=lambda: True)
        self.assertEqual(raised.exception.code, "camera_import_cancelled")

    def test_camera_preview_is_bounded_without_subsampling_training_coverage(self):
        views = []
        for index in range(260):
            image_path = "image-{:03d}.png".format(index)
            Image.new("RGB", (16, 12), (31, 61, 91)).save(self.root / image_path)
            pose = copy.deepcopy(IDENTITY)
            pose[0][3], pose[1][3] = .5, .5
            views.append({"view_id": "train-{:03d}".format(index), "split": "train", "image_path": image_path,
                "pixel_coordinates": "pixel_border", "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]],
                "c2w_opencv": pose})
        dataset = self.dataset(views=views)
        report = initialization.inspect_initialization(dataset, self.root, self.request(dataset))
        self.assertEqual(len(report["coverage"]["per_view"]), 260)
        self.assertLessEqual(len(report["preview"]["views"]), 256)
        self.assertEqual(report["preview"]["total_views"], 260)
        self.assertEqual(report["preview"]["sampled_views"], len(report["preview"]["views"]))

    def test_bad_suggestion_view_selectors_are_typed_rejections(self):
        dataset = self.dataset()
        for value in ([{}], [["train-a"]], [True], "train-a"):
            with self.subTest(value=value):
                request = self.request(dataset, suggestion={"method": "optical_axis_convergence", "view_ids": value})
                with self.assertRaises(initialization.InitializationError):
                    initialization.suggest_initialization(dataset, request)
        request = self.request(dataset, suggestion={"method": "user_depth_interval", "view_id": {}})
        with self.assertRaises(initialization.InitializationError):
            initialization.suggest_initialization(dataset, request)

    def test_orbit_parallel_and_outward_layouts_do_not_share_a_guessed_object_volume(self):
        centers = [[-2., 0., 0.], [2., 0., 0.], [0., -2., 0.], [0., 2., 0.]]
        for layout in ("inward-orbit", "parallel-forward", "outward-room"):
            views = []
            for index, center in enumerate(centers):
                image = "layout-{}.png".format(index)
                Image.new("RGB", (16, 12), (31, 61, 91)).save(self.root / image)
                target = ([0., 0., 3.] if layout == "inward-orbit" else
                          [center[0], center[1], center[2] + 3.] if layout == "parallel-forward" else
                          [2. * center[0], 2. * center[1], -3.])
                views.append({"view_id": str(index), "split": "train", "image_path": image,
                    "pixel_coordinates": "pixel_border", "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]],
                    "c2w_opencv": looking_at(center, target)})
            dataset = self.dataset(views=views)
            request = self.request(dataset, suggestion={"method": "optical_axis_convergence", "half_extents": [.1]*3})
            report = initialization.suggest_initialization(dataset, request)
            with self.subTest(layout=layout):
                if layout == "inward-orbit":
                    self.assertEqual(report["status"], "suggestion_requires_confirmation")
                    np.testing.assert_allclose(report["suggestion_diagnostics"]["center"], [0., 0., 3.], atol=2e-14)
                    adopted = self.request(dataset, initialization=report["suggested_initialization"])
                    inspected = initialization.inspect_initialization(dataset, self.root, adopted)
                    self.assertEqual(inspected["status"], "ready")
                    self.assertEqual([r["inside_image"] for r in inspected["coverage"]["per_view"]], [4]*4)
                else:
                    self.assertEqual(report["status"], "needs_input")
                    self.assertNotIn("suggested_initialization", report)
                    wanted = ("initialization_parallel_or_rank_deficient_axes" if layout == "parallel-forward"
                              else "initialization_axis_target_behind_camera")
                    self.assertIn(wanted, report["errors"])

    def test_metric_unit_conversion_rotation_and_large_origin_preserve_the_declared_physical_volume(self):
        reports = []
        for unit, factor, origin in (("m", 1., 0.), ("mm", .001, 1_000_000.)):
            pose = copy.deepcopy(IDENTITY)
            pose[0][3], pose[1][3] = origin + .5/factor, .5/factor
            dataset = self.dataset(transform={"s": factor, "R": [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]],
                "t": [10., 20. - factor*origin, 0.]}, views=[{"view_id": "train-a", "split": "train",
                "image_path": "image.png", "pixel_coordinates": "pixel_border",
                "K": [[20., 0., 8.], [0., 20., 6.], [0., 0., 1.]], "c2w_opencv": pose}],
                scale={"status": "source_declared", "source_unit": unit, "meters_per_unit": factor,
                       "evidence": [{"kind": "explicit_test_unit_declaration"}]})
            request = self.request(dataset)
            request["initialization"]["bounds"] = [[origin, 0., 2./factor], [origin+1./factor, 1./factor, 3./factor]]
            reports.append(initialization.inspect_initialization(dataset, self.root, request))
        np.testing.assert_allclose(reports[0]["preview"]["points"], reports[1]["preview"]["points"], atol=2e-12, rtol=0.)
        self.assertEqual([report["frames"]["canonical_unit"] for report in reports], ["m", "m"])
        self.assertEqual([report["status"] for report in reports], ["ready", "ready"])
        self.assertEqual([report["coverage"]["per_view"][0]["inside_image"] for report in reports], [4, 4])
        self.assertEqual(reports[1]["frames"]["source_unit"], "mm")

    def test_heldout_cameras_do_not_change_initial_points_normalization_or_training_coverage(self):
        base = self.dataset()
        first = initialization.inspect_initialization(base, self.root, self.request(base))
        modified = copy.deepcopy(base)
        for index, split in enumerate(("val", "test")):
            image = split + ".png"
            Image.new("RGB", (16, 12), (31, 61, 91)).save(self.root / image)
            extra = copy.deepcopy(base["views"][0])
            extra.update(view_id=split, split=split, image_path=image)
            extra["source_c2w_opencv"][0][3] = 1.e7 * (index + 1)
            extra["c2w_opencv"][0][3] = 1.e7 * (index + 1)
            modified["views"].append(extra)
        modified.pop("splits", None)
        modified.pop("data_identity", None)
        modified = camera_dataset.validate_camera_dataset(modified, self.root)
        second = initialization.inspect_initialization(modified, self.root, self.request(modified))
        self.assertEqual(first["point_sha256"], second["point_sha256"])
        self.assertEqual(first["transforms"], second["transforms"])
        self.assertEqual(first["coverage"], second["coverage"])

    def test_declared_geometry_prior_uses_the_same_explicit_binary64_sim3_as_random_points(self):
        x, y, z, w = [value / math.sqrt(.95) for value in (.1, .2, .3, .9)]
        rotation = [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                    [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                    [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]
        dataset = self.dataset({"s": 1.7, "R": rotation, "t": [1.1, 1.2, .23]})
        path = self.root / "prior.ply"
        point = [0.9095935277196137, 0.2133853535799155, 2.452123961817683]
        path.write_text("ply\nformat ascii 1.0\nelement vertex 4\nproperty double x\nproperty double y\n"
            "property double z\nend_header\n" + "".join("%.17g %.17g %.17g\n" % tuple(point) for _ in range(4)), encoding="ascii")
        report = initialization.inspect_initialization(dataset, self.root, self.request(dataset,
            initialization={"kind": "external_point_cloud", "frame": "source", "path": path.name}))
        # Persisted operation order has an exact binary64 contract. Python3.12
        # compensated sum previously produced x=3.8751496486421106 instead.
        self.assertEqual(report["preview"]["points"][0], [3.87514964864211, 2.1671672632005556, 3.586055743174006])

    def test_finite_but_unrepresentable_sampling_intervals_are_typed_rejections(self):
        dataset = self.dataset()
        request = self.request(dataset)
        request["initialization"]["bounds"] = [[-1.e308, 0., 2.], [1.e308, 1., 3.]]
        with self.assertRaises(initialization.InitializationError):
            initialization.inspect_initialization(dataset, self.root, request)

    def test_initialization_input_transform_is_validated_and_cannot_be_silently_ignored(self):
        dataset = self.dataset()
        for frame, transform in (("source", {"s": 2.}), ("canonical", {"s": -1.}),
                                 ("canonical", {"s": 1., "applied": True}), ("canonical", "invalid")):
            request = self.request(dataset)
            request["initialization"].update(frame=frame, input_to_canonical=transform)
            with self.subTest(frame=frame, transform=transform):
                with self.assertRaises(initialization.InitializationError):
                    initialization.inspect_initialization(dataset, self.root, request)


if __name__ == "__main__":
    unittest.main()
