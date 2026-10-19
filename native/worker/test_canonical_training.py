"""P0 training camera contract at the loader/projection/checkpoint seams.

These CPU tests complement, and do not replace, real backend GPU short runs.
"""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

from native.worker import canonical_training as training


class CanonicalProjectionTests(unittest.TestCase):
    def test_off_center_full_k_reaches_rasterizer_pixel_centers(self):
        # K uses pixel-border coordinates; CUDA returns integer-centered pixels.
        projection = np.asarray(training.pinhole_projection(
            [[300., 0., 123.], [0., 420., 87.], [0., 0., 1.]], 640, 360, .001, 50.))
        camera_points = np.asarray([[.2, -.1, 2., 1.], [0., 0., 3., 1.]])
        clip = camera_points @ projection.T
        ndc = clip[:, :2] / clip[:, 3:4]
        pixels = ((ndc + 1.) * [640., 360.] - 1.) / 2.
        np.testing.assert_allclose(pixels, [[152.5, 65.5], [122.5, 86.5]], atol=1e-10)

    def test_scaling_intrinsics_preserves_noncentral_principal_point(self):
        actual = training.resized_intrinsics(
            [[300., 0., 123.], [0., 420., 87.], [0., 0., 1.]], (640, 360), (320, 180))
        self.assertEqual(actual, [[150., 0., 61.5], [0., 210., 43.5], [0., 0., 1.]])

    def test_large_origin_is_localized_in_double_precision_and_round_trips(self):
        cameras = []
        for center in ([1000000.01, 2000000., 3000000.],
                       [999999.99, 2000000., 3000000.]):
            pose = np.eye(4)
            pose[:3, 3] = center
            cameras.append({"c2w_opencv": pose.tolist()})
        transform = training.training_normalization(cameras)
        localized = training.transform_points(transform, [[1000000.005, 2000000., 3000000.]])
        restored = training.transform_points(transform["inverse"], localized)
        np.testing.assert_allclose(localized, [[.5, 0., 0.]], atol=2e-8)
        np.testing.assert_allclose(restored, [[1000000.005, 2000000., 3000000.]], atol=1e-9)

    def test_explicit_training_transform_is_not_recomputed_or_applied_twice(self):
        views = [{"c2w_opencv": np.eye(4).tolist()}]
        explicit = {"s": 7., "R": np.eye(3).tolist(), "t": [1., 2., 3.],
                    "input_frame": "canonical", "output_frame": "training", "applied": False}
        result = training.training_normalization(views, explicit=explicit)
        self.assertEqual(result["s"], 7.)
        self.assertEqual(result["t"], [1., 2., 3.])
        self.assertFalse(result["applied"])
        self.assertEqual(training.training_normalization(views, explicit=result), result)

    def test_zero_coverage_without_declared_bounds_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "initialization|coverage|bounds"):
            training.training_normalization([{"c2w_opencv": np.eye(4).tolist()}])


class CanonicalPreparationTests(unittest.TestCase):
    def fixture(self, root):
        from PIL import Image
        views = []
        for index, split in enumerate(("train", "train", "val", "test")):
            relative = "camera-{}/same.png".format(index)
            path = root / relative
            path.parent.mkdir(parents=True)
            Image.new("RGB", (1924, 24), (30 + index, 80, 130)).save(path)
            pose = np.eye(4)
            pose[0, 3] = index * .02 + 1000000.
            pose[2, 3] = -.05
            views.append(dict(image_path=relative, width=1924, height=24,
                              K=[[1000., 0., 823.], [0., 900., 10.], [0., 0., 1.]],
                              c2w_opencv=pose.tolist(), split=split, pixel_coordinates="pixel_border"))
        data = {"schema": "gsw.camera_dataset/1", "pose_policy": "locked",
                "source": {"kind": "synthetic", "world_axes": "right-handed z-up"},
                "scale": {"status": "arbitrary", "source_unit": "unknown"},
                "views": views, "initialization": {"kind": "random", "count": 32, "seed": 7,
                    "frame": "source", "bounds": [[999999.99, -.0001, 0.], [1000000.04, .0001, .001]]}}
        manifest = root / "camera_dataset.json"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        return manifest

    def test_explicit_holdouts_are_never_folded_into_training_and_default_keeps_pixels(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            self.fixture(root)
            prepared = training.prepare_training_dataset(root)
            self.assertEqual({key: len(value) for key, value in prepared["splits"].items()},
                             {"train": 2, "val": 1, "test": 1})
            self.assertEqual(len({view["view_id"] for view in prepared["views"]}), 4)
            self.assertEqual([view["training_size"] for view in prepared["views"]], [[1924, 24]] * 4)
            self.assertEqual(prepared["views"][0]["training_K"][0][2], 823.)
            self.assertEqual(prepared["initial_points"].shape, (32, 3))
            self.assertEqual(prepared["initialization"]["kind"], "random")
            self.assertEqual(prepared["initialization"]["seed"], 7)
            self.assertTrue(prepared["transforms"]["canonical_to_training"]["applied"])

    def test_user_resolution_is_recorded_with_scaled_full_k(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            self.fixture(root)
            prepared = training.prepare_training_dataset(root, resolution=2)
            view = prepared["views"][0]
            self.assertEqual(view["training_size"], [962, 12])
            self.assertEqual(view["training_K"], [[500., 0., 411.5], [0., 450., 5.], [0., 0., 1.]])
            self.assertEqual(prepared["resolution_policy"], "manual_scale")

    def test_prepared_manifest_reloads_without_relocalizing_and_checks_current_image_bytes(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            manifest = self.fixture(source)
            report = training.prepare_training_dataset(manifest, root / "prepared", {"resolution": 2})
            loaded = training.load_prepared_training_dataset(report["manifestPath"], report["imageRoot"])
            self.assertEqual(loaded["transforms"], report["contract"]["transforms"])
            self.assertEqual(loaded["views"][0]["training_size"], [962, 12])
            from PIL import Image
            Image.new("RGB", (1924, 24), (1, 2, 3)).save(source / "camera-0/same.png")
            with self.assertRaisesRegex(ValueError, "hash|identity"):
                training.load_prepared_training_dataset(report["manifestPath"], report["imageRoot"])

    def test_rgba_composition_uses_unsigned_bytes_for_pillow_12_compatibility(self):
        from PIL import Image
        image = Image.fromarray(np.asarray([[[250, 120, 40, 255], [255, 255, 255, 0]]], dtype=np.uint8))
        result = training.composite_training_image(image)
        self.assertEqual(np.asarray(result).dtype, np.uint8)
        self.assertEqual(np.asarray(result).tolist(), [[[250, 120, 40], [0, 0, 0]]])

    def test_test_camera_pose_cannot_change_training_normalization_or_initial_points(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            manifest = self.fixture(root)
            before = training.prepare_training_dataset(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["views"][-1]["c2w_opencv"][0][3] += 9000000.
            manifest.write_text(json.dumps(data), encoding="utf-8")
            after = training.prepare_training_dataset(root)
            self.assertEqual(before["transforms"], after["transforms"])
            self.assertEqual(before["clipping"], after["clipping"])
            np.testing.assert_array_equal(before["initial_points"], after["initial_points"])

    def test_explicit_unlocalized_near_geometry_is_rejected_not_silently_culled(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            manifest = self.fixture(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["transforms"] = {"canonical_to_training": {"mode": "explicit", "s": 1.,
                "R": np.eye(3).tolist(), "t": [0., 0., 0.]}}
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "camera_no_forward_initialization"):
                training.prepare_training_dataset(root)

    def test_opencv_integer_principal_point_projects_exactly_after_canonical_conversion(self):
        from native.backend import camera_dataset
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            manifest = self.fixture(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["views"][0]["pixel_coordinates"] = "opencv_integer"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            view = camera_dataset.load_camera_dataset(manifest)["views"][0]
            projection = np.asarray(training.pinhole_projection(view["K"], 1924, 24, .2, 10.))
            clip = projection @ [0., 0., 1., 1.]
            actual = ((clip[:2] / clip[3] + 1.) * [1924., 24.] - 1.) / 2.
            np.testing.assert_allclose(actual, [823., 10.], atol=1e-10)

    def test_explicit_solve_strategy_ignores_old_nearby_locked_manifest(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            manifest = self.fixture(root)
            original = manifest.read_bytes()
            with mock.patch.dict(os.environ, {"GSW_POSE_POLICY": "solve"}):
                self.assertIsNone(training.camera_dataset_source(root))
            self.assertEqual(manifest.read_bytes(), original)


class CanonicalCheckpointTests(unittest.TestCase):
    def test_complete_checkpoint_carries_and_rejects_changed_camera_scale_contract(self):
        from types import SimpleNamespace
        from native.worker import training_checkpoint
        contract = {"input_data_identity": "data-a", "splits": {"train": ["a"], "val": [], "test": ["b"]},
                    "pose_policy": "locked", "scale": {"status": "arbitrary"},
                    "transforms": {"canonical_to_training": {"s": 2.}}}
        model = SimpleNamespace(_gsw_camera_contract=copy.deepcopy(contract), capture=lambda: ("optimizer-state",))
        cpu_torch = SimpleNamespace(get_rng_state=lambda: "cpu-rng",
                                    cuda=SimpleNamespace(is_available=lambda: False))
        state = training_checkpoint.capture_state(cpu_torch, np, model, 3, 10, "identity", ["a"], [], 1., .1, 0., backend="2dgs")
        self.assertEqual(state["camera_contract"], contract)
        model._gsw_camera_contract["transforms"]["canonical_to_training"]["s"] = 3.
        with self.assertRaisesRegex(ValueError, "camera|scale|contract"):
            training_checkpoint.restore_state(cpu_torch, np, model, SimpleNamespace(iterations=10),
                                             state, "identity", ["a"], backend="2dgs")


if __name__ == "__main__":
    unittest.main()
