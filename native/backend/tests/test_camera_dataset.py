"""P0 camera protocol tests through public parsing/numeric interfaces."""

import json
import math
import os
from pathlib import Path
import tempfile
import unittest
import copy
import struct
import numpy as np
from plyfile import PlyData, PlyElement

from PIL import Image

from native.backend import camera_dataset


IDENTITY = [[1., 0., 0., 0.], [0., 1., 0., 0.],
            [0., 0., 1., 0.], [0., 0., 0., 1.]]


class CameraDatasetTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_TEST_TEMP", "D:/Codex/qa-camera-p0"))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def image(self, relative, size=(12, 8), color=(90, 60, 30)):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.new("RGB", size, color).save(path)
        return path

    def generic(self, **overrides):
        self.image("nested/frame.png")
        data = {
            "schema": "gsw.camera_dataset/1",
            "source": {"kind": "manual", "world_axes": "right-handed z-up"},
            "pose_policy": "locked",
            "scale": {"status": "source_declared", "source_unit": "cm",
                      "meters_per_unit": .01, "evidence": [{"kind": "user_declaration"}]},
            "views": [{"image_path": "nested/frame.png", "width": 12, "height": 8,
                       "K": [[20., 0., 3.], [0., 30., 2.], [0., 0., 1.]],
                       "c2w_opencv": IDENTITY, "split": "train", "pixel_coordinates": "pixel_border"}],
        }
        data.update(overrides)
        path = self.root / "cameras.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_generic_preserves_full_intrinsics_and_declared_metric_scale(self):
        dataset = camera_dataset.load_camera_dataset(self.generic(), adapter="generic_json")
        view = dataset["views"][0]
        self.assertEqual(view["K"], [[20., 0., 3.], [0., 30., 2.], [0., 0., 1.]])
        projection = camera_dataset.project_points(view, [[.02, .03, .1]])[0]
        for actual, expected in zip(projection, [7., 11., .1]):
            self.assertAlmostEqual(actual, expected, places=12)
        self.assertEqual(dataset["scale"]["status"], "source_declared")
        self.assertEqual(dataset["transforms"]["source_to_canonical"]["s"], .01)
        self.assertEqual(dataset["splits"]["test"], [])
        self.assertEqual(dataset["initialization"]["kind"], "random")

    def test_blender_keeps_nested_names_splits_and_converts_camera_axes_once(self):
        frames = []
        for split, size, color in (("train", (12, 8), (90, 20, 1)),
                                   ("val", (10, 6), (10, 30, 2)),
                                   ("test", (14, 10), (11, 60, 3))):
            self.image(split + "/same.png", size, color)
            data = {"camera_angle_x": math.pi / 2,
                    "frames": [{"file_path": "./" + split + "/same", "transform_matrix": IDENTITY}]}
            (self.root / ("transforms_" + split + ".json")).write_text(json.dumps(data), encoding="utf-8")
        dataset = camera_dataset.load_camera_dataset(self.root, adapter="blender_transforms")
        self.assertEqual([len(dataset["splits"][key]) for key in ("train", "val", "test")], [1, 1, 1])
        self.assertEqual(len({view["view_id"] for view in dataset["views"]}), 3)
        self.assertEqual(dataset["views"][0]["c2w_opencv"],
                         [[1., 0., 0., 0.], [0., -1., 0., 0.], [0., 0., -1., 0.], [0., 0., 0., 1.]])
        self.assertAlmostEqual(dataset["views"][0]["K"][0][0], 6.)
        original_identity = dataset["data_identity"]
        path = self.root / "roundtrip.json"
        camera_dataset.write_camera_dataset(dataset, path)
        reopened = camera_dataset.load_camera_dataset(path, adapter="generic_json")
        self.assertEqual(reopened["views"], dataset["views"])
        self.assertEqual(reopened["data_identity"], original_identity)

    def test_colmap_text_and_binary_produce_equivalent_locked_cameras_without_sparse_points(self):
        self.image("images/one/view.png", (12, 8))
        model = self.root / "sparse" / "0"
        model.mkdir(parents=True)
        (model / "cameras.txt").write_text("1 PINHOLE 12 8 20 30 3 2\n", encoding="utf-8")
        (model / "images.txt").write_text("7 1 0 0 0 -2 -3 -4 1 one/view.png\n\n", encoding="utf-8")
        text = camera_dataset.load_camera_dataset(self.root, adapter="colmap")
        self.assertEqual(text["views"][0]["c2w_opencv"],
                         [[1., 0., 0., 2.], [0., 1., 0., 3.], [0., 0., 1., 4.], [0., 0., 0., 1.]])
        self.assertEqual(text["initialization"]["kind"], "random")
        (model / "cameras.bin").write_bytes(struct.pack("<QiiQQdddd", 1, 1, 1, 12, 8, 20., 30., 3., 2.))
        (model / "images.bin").write_bytes(struct.pack("<Qidddddddi", 1, 7, 1., 0., 0., 0., -2., -3., -4., 1)
                                            + b"one/view.png\0" + struct.pack("<Q", 0))
        binary = camera_dataset.load_camera_dataset(self.root, adapter="colmap")
        self.assertEqual(binary["views"], text["views"])
        self.assertEqual(binary["pose_policy"], "locked")
        self.assertEqual(binary["scale"]["status"], "arbitrary")

    def test_invalid_camera_and_image_contracts_fail_closed(self):
        path = self.generic()
        original = json.loads(path.read_text(encoding="utf-8"))
        changes = [
            ("camera_nonfinite_number", lambda data: data["views"][0]["K"][0].__setitem__(0, float("nan"))),
            ("camera_invalid_rotation", lambda data: data["views"][0]["c2w_opencv"][0].__setitem__(0, 2.)),
            ("camera_unsupported_skew", lambda data: data["views"][0]["K"][0].__setitem__(1, .1)),
            ("camera_unsupported_distortion", lambda data: data["views"][0].__setitem__("distortion", [.01])),
            ("camera_conflicting_units", lambda data: data["scale"].__setitem__("meters_per_unit", 1.)),
            ("camera_image_dimensions_mismatch", lambda data: data["views"][0].__setitem__("width", 13)),
            ("camera_path_outside_dataset", lambda data: data["views"][0].__setitem__("image_path", "../out.png")),
            ("camera_unsupported_pose_policy", lambda data: data.__setitem__("pose_policy", "optimize")),
        ]
        for code, change in changes:
            with self.subTest(code=code):
                data = copy.deepcopy(original)
                change(data)
                path.write_text(json.dumps(data), encoding="utf-8")
                with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
                    camera_dataset.load_camera_dataset(path)
                self.assertEqual(raised.exception.code, code)

    def test_source_pixel_conventions_and_world_to_camera_are_explicit(self):
        path = self.generic()
        data = json.loads(path.read_text(encoding="utf-8"))
        view = data["views"][0]
        view.pop("pixel_coordinates")
        view["w2c_opencv"] = view.pop("c2w_opencv")
        path.write_text(json.dumps(data), encoding="utf-8")
        loaded = camera_dataset.load_camera_dataset(path)
        canonical = loaded["views"][0]
        self.assertEqual(canonical["source_K"][0][2], 3.)
        self.assertEqual(canonical["source_pixel_coordinates"], "opencv_integer")
        self.assertEqual(canonical["pixel_coordinates"], "pixel_border")
        self.assertEqual(canonical["K"][0][2], 3.5)
        self.assertEqual(camera_dataset.project_points(canonical, [[0., 0., 1.]])[0][:2], [3.5, 2.5])
        self.assertEqual(camera_dataset.project_points(canonical, [[0., 0., 1.]],
                                                       pixel_coordinates="opencv_integer")[0][:2], [3., 2.])

    def test_research_adapter_requires_declared_units_and_validates_redundant_poses(self):
        self.image("images/research.png")
        data = {"conventions": {"world": "right-handed +Z up", "units": "meters",
                                 "pixels": "continuous coordinates from top-left image border"},
                "cameras": [{"file_name": "images/research.png", "split": "train",
                             "K": [[20., 0., 6.], [0., 21., 4.], [0., 0., 1.]],
                             "camera_to_world_blender_m": IDENTITY,
                             "world_to_camera_opencv_m": [[1., 0., 0., 0.], [0., -1., 0., 0.],
                                                           [0., 0., -1., 0.], [0., 0., 0., 1.]]}]}
        path = self.root / "research.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        loaded = camera_dataset.load_camera_dataset(path, adapter="research_json")
        self.assertEqual(loaded["scale"]["source_unit"], "meters")
        self.assertEqual(loaded["scale"]["status"], "source_declared")
        self.assertEqual(loaded["views"][0]["K"][0][2], 6.)
        data["cameras"][0]["world_to_camera_opencv_m"][0][3] = 3.
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="research_json")
        self.assertEqual(raised.exception.code, "camera_conflicting_pose")

    def test_colmap_points_are_explicit_initialization_and_converted_with_units(self):
        self.test_colmap_text_and_binary_produce_equivalent_locked_cameras_without_sparse_points()
        model = self.root / "sparse" / "0"
        (model / "points3D.bin").write_bytes(struct.pack("<QQdddBBBdQ", 1, 9, 100., 200., 300.,
                                                        255, 128, 0, .2, 0))
        dataset = camera_dataset.load_camera_dataset(self.root, adapter="colmap", options={
            "scale": {"status": "source_declared", "source_unit": "cm", "meters_per_unit": .01,
                      "evidence": [{"kind": "user_declaration"}]}})
        xyz, colors = camera_dataset.read_initialization_points(dataset, self.root)
        self.assertEqual(xyz, [[1., 2., 3.]])
        self.assertEqual(colors[0][0], 1.)
        self.assertAlmostEqual(colors[0][1], 128. / 255.)
        self.assertEqual(dataset["initialization"]["kind"], "sfm_sparse")
        self.assertEqual(dataset["initialization"]["count"], 1)

    def test_identity_detects_image_changes_and_cancellation_does_not_edit_source(self):
        path = self.generic()
        loaded = camera_dataset.load_camera_dataset(path)
        original = (self.root / "nested/frame.png").read_bytes()
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, cancelled=lambda: True)
        self.assertEqual(raised.exception.code, "camera_import_cancelled")
        self.assertEqual((self.root / "nested/frame.png").read_bytes(), original)
        self.image("nested/frame.png", color=(1, 2, 3))
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.validate_camera_dataset(loaded, self.root)
        self.assertEqual(raised.exception.code, "camera_image_hash_mismatch")

    def test_large_origin_unit_roundtrips_preserve_projections_without_fixed_scene_size(self):
        for unit, factor, extent in (("mm", .001, 18.), ("cm", .01, 340.), ("m", 1., 1800.)):
            with self.subTest(unit=unit):
                path = self.generic()
                data = json.loads(path.read_text(encoding="utf-8"))
                data["scale"].update(source_unit=unit, meters_per_unit=factor)
                data["views"][0]["c2w_opencv"][0][3] = 1000000.
                data["transforms"] = {"source_to_canonical": {
                    "s": factor, "R": [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]],
                    "t": [0., -1000000. * factor, 0.]}}
                path.write_text(json.dumps(data), encoding="utf-8")
                loaded = camera_dataset.load_camera_dataset(path)
                transform = loaded["transforms"]["source_to_canonical"]
                source = [[1000000. + extent, 2. * extent, 10. * extent]]
                canonical = camera_dataset.apply_sim3(transform, source)
                recovered = camera_dataset.apply_sim3(camera_dataset.invert_sim3(transform), canonical)
                for actual, expected in zip(recovered[0], source[0]):
                    self.assertTrue(math.isclose(actual, expected, rel_tol=2e-13, abs_tol=2e-9))
                projected = camera_dataset.project_points(loaded["views"][0], canonical)[0]
                self.assertAlmostEqual(projected[0], 5., places=7)
                self.assertAlmostEqual(projected[1], 8., places=7)
                written = self.root / "large_roundtrip.json"
                camera_dataset.write_camera_dataset(loaded, written)
                self.assertEqual(camera_dataset.load_camera_dataset(written)["data_identity"], loaded["data_identity"])

    def test_split_overlap_is_rejected_and_duplicate_bytes_are_not_independent_test(self):
        path = self.generic()
        data = json.loads(path.read_text(encoding="utf-8"))
        self.image("test/same.png")
        view = copy.deepcopy(data["views"][0])
        view.update(image_path="test/same.png", split="test")
        data["views"].append(view)
        path.write_text(json.dumps(data), encoding="utf-8")
        loaded = camera_dataset.load_camera_dataset(path)
        self.assertFalse(loaded["independent_test"])
        self.assertEqual(loaded["warnings"][0]["code"], "camera_cross_split_duplicate_content")
        data["views"][0]["view_id"] = "same"
        data["views"][1]["view_id"] = "same"
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_duplicate_view")

    def test_empty_colmap_points_remain_valid_locked_input_and_external_claim_not_promoted(self):
        self.test_colmap_text_and_binary_produce_equivalent_locked_cameras_without_sparse_points()
        (self.root / "sparse/0/points3D.bin").write_bytes(struct.pack("<Q", 0))
        dataset = camera_dataset.load_camera_dataset(self.root, adapter="colmap", options={
            "scale": {"status": "independently_checked", "source_unit": "m", "meters_per_unit": 1.,
                      "evidence": [{"kind": "external_check", "reference": "declared report"}]}})
        self.assertEqual(dataset["initialization"]["kind"], "random")
        self.assertEqual(dataset["initialization"]["reason"], "empty_sfm_sparse")
        self.assertEqual(dataset["scale"]["verification_origin"], "external_claim")
        self.assertIsNone(camera_dataset.read_initialization_points(dataset, self.root))

    def test_conflicting_inverse_and_preapplied_pose_are_not_silently_accepted(self):
        path = self.generic()
        data = json.loads(path.read_text(encoding="utf-8"))
        data["transforms"] = {"source_to_canonical": {"s": .01, "inverse": {
            "s": 1., "R": [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]], "t": [0., 0., 0.]}}}
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_conflicting_inverse")
        data["transforms"]["source_to_canonical"].pop("inverse")
        data["transforms"]["source_to_canonical"]["applied"] = True
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_ambiguous_applied_transform")

    def test_normalized_pose_or_intrinsics_cannot_disagree_with_source_record(self):
        path = self.generic()
        loaded = camera_dataset.load_camera_dataset(path)
        for key in ("K", "c2w_opencv"):
            data = copy.deepcopy(loaded)
            if key == "K":
                data["views"][0][key][0][2] += 1.
            else:
                data["views"][0][key][0][3] += 1.
            # Even a manifest newly serialized by another tool must reject its
            # contradictory source/canonical records, not guess which is true.
            data.pop("data_identity")
            with self.subTest(key=key):
                with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
                    camera_dataset.validate_camera_dataset(data, self.root)
                self.assertIn(raised.exception.code, ("camera_conflicting_pose", "camera_conflicting_intrinsics"))

    def test_external_ply_initialization_records_geometry_prior_and_unknown_scale_stays_unknown(self):
        path = self.generic(scale={"status": "arbitrary", "source_unit": "unknown"})
        vertices = np.asarray([(2., 3., 4., 20, 40, 60), (5., 6., 7., 255, 255, 255)],
                              dtype=[("x", "f8"), ("y", "f8"), ("z", "f8"),
                                     ("red", "u1"), ("green", "u1"), ("blue", "u1")])
        points = self.root / "initial.ply"
        PlyData([PlyElement.describe(vertices, "vertex")]).write(points)
        dataset = camera_dataset.load_camera_dataset(path, options={
            "initialization": {"kind": "external_point_cloud", "path": "initial.ply", "frame": "source"}})
        self.assertEqual(dataset["scale"]["status"], "arbitrary")
        self.assertIsNone(dataset["scale"]["meters_per_unit"])
        self.assertEqual(dataset["scale"]["canonical_unit"], "arbitrary")
        self.assertEqual(dataset["initialization"]["priors"], ["external_geometry"])
        self.assertEqual(dataset["initialization"]["point_bounds"], [[2., 3., 4.], [5., 6., 7.]])
        xyz, rgb = camera_dataset.read_initialization_points(dataset, self.root)
        self.assertEqual(xyz, [[2., 3., 4.], [5., 6., 7.]])
        self.assertEqual(rgb[1], [1., 1., 1.])
        self.assertEqual(camera_dataset.camera_dataset_root(path), self.root)

    def test_missing_fields_and_untrusted_evaluation_initialization_return_typed_errors(self):
        path = self.generic()
        original = json.loads(path.read_text(encoding="utf-8"))
        original["views"][0].pop("K")
        path.write_text(json.dumps(original), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_invalid_manifest")
        path = self.generic()
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, options={"initialization": {
                "kind": "external_point_cloud", "path": "ground-truth.ply", "priors": ["evaluation_geometry"]}})
        self.assertEqual(raised.exception.code, "camera_evaluation_prior_forbidden")

    def test_duplicate_json_keys_and_nonstructured_scale_evidence_are_rejected(self):
        path = self.generic()
        original = path.read_text(encoding="utf-8")
        path.write_text(original[:-1] + ',"pose_policy":"solve"}', encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_duplicate_json_key")
        data = json.loads(original)
        data["scale"]["evidence"] = "somebody says so"
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_missing_scale_evidence")

    def test_large_origin_does_not_hide_conflicting_redundant_pose_and_normalized_records_recheck_sources(self):
        path = self.generic()
        raw = json.loads(path.read_text(encoding="utf-8"))
        raw["views"][0]["c2w_opencv"][0][3] = 1000000.
        w2c = copy.deepcopy(IDENTITY)
        w2c[0][3] = -1000001.
        raw["views"][0]["w2c_opencv"] = w2c
        path.write_text(json.dumps(raw), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path)
        self.assertEqual(raised.exception.code, "camera_conflicting_pose")
        path = self.generic()
        normalized = camera_dataset.load_camera_dataset(path)
        normalized.pop("data_identity")
        normalized["views"][0]["w2c_opencv"] = copy.deepcopy(IDENTITY)
        normalized["views"][0]["w2c_opencv"][0][3] = 5.
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.validate_camera_dataset(normalized, self.root)
        self.assertEqual(raised.exception.code, "camera_conflicting_pose")

    def test_explicit_json_file_is_not_superseded_by_incidental_colmap_files_in_its_directory(self):
        path = self.generic()
        self.image("images/old.png")
        (self.root / "cameras.txt").write_text("1 PINHOLE 12 8 20 20 6 4\n", encoding="utf-8")
        (self.root / "images.txt").write_text("1 1 0 0 0 0 0 0 1 old.png\n\n", encoding="utf-8")
        dataset = camera_dataset.load_camera_dataset(path, adapter="auto")
        self.assertEqual(dataset["source"]["adapter"], "generic_json")
        self.assertEqual(dataset["views"][0]["image_path"], "nested/frame.png")
        self.assertEqual(camera_dataset.camera_dataset_root(path, adapter="auto"), self.root)


if __name__ == "__main__":
    unittest.main()
