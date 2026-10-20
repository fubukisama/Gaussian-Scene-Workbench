"""Public CPU geometry semantics; real rasterization is a separate GPU gate."""

import unittest
import copy
import json
import os
from pathlib import Path
import tempfile
from unittest import mock

import numpy as np
from PIL import Image
from plyfile import PlyData, PlyElement

from native.backend import camera_dataset
from native.backend.gs_geometry import (backproject_camera_z, camera_z_from_moments, inspect_geometry_source,
                                        geometry_coordinates, GsGeometryError)
from native.worker.canonical_training import prepare_training_dataset, pinhole_projection
from native.worker.training_checkpoint import file_digest


class GsGeometryProjectionTests(unittest.TestCase):
    def test_length_and_origin_chain_is_applied_once_and_unknown_units_stay_unknown(self):
        contract = {"schema": "gsw.training_camera_contract/1", "pose_policy": "locked", "input_data_identity": "a" * 64,
            "scale": {"status": "source_declared", "source_unit": "mm", "canonical_unit": "m",
                      "meters_per_unit": .001, "evidence": [{"kind": "declared_unit"}]},
            "transforms": {"source_to_canonical": {"s": .001, "R": np.eye(3).tolist(),
                "t": [1000., 2000., 3000.], "input_frame": "source", "output_frame": "canonical", "applied": True},
                "canonical_to_training": {"s": 4., "R": np.eye(3).tolist(), "t": [-4000., -8000., -12000.],
                    "input_frame": "canonical", "output_frame": "training", "applied": True}}}
        source = geometry_coordinates(contract, "source")
        canonical = geometry_coordinates(contract, "canonical")
        self.assertEqual(source["unit"], "mm")
        self.assertEqual(source["meters_per_unit"], .001)
        self.assertEqual(source["depth_training_to_output_ratio"], 250.)
        self.assertEqual(source["transform"]["t"], [0., 0., 0.])
        self.assertEqual(canonical["meters_per_unit"], 1.)
        self.assertEqual(canonical["depth_training_to_output_ratio"], .25)
        self.assertEqual(canonical["transform"]["t"], [1000., 2000., 3000.])
        self.assertEqual(geometry_coordinates(contract, "training")["meters_per_unit"], .25)
        contract["scale"] = {"status": "arbitrary", "source_unit": "unknown", "canonical_unit": "unknown",
                             "meters_per_unit": None, "evidence": []}
        arbitrary = geometry_coordinates(contract, "canonical")
        self.assertIsNone(arbitrary["meters_per_unit"])
        self.assertEqual(arbitrary["unit"], "unknown")

    def test_alpha_normalized_camera_z_is_not_stock_inverse_depth_or_clamped_rgb(self):
        # Front-to-back weights .5 and .25 at camera Z2 and Z4: A=.75,
        # S=2. The arithmetic expected-Z proxy is 8/3, not 1/raw_invdepth=3.2.
        alpha = np.asarray([[.75, 0.], [.8, 1.]], dtype=np.float32)
        moment = np.asarray([[2., 0.], [np.inf, 4.]], dtype=np.float32)
        result = camera_z_from_moments(alpha, moment, alpha_threshold=.5, depth_min=.2, depth_max=10.)
        np.testing.assert_array_equal(result["mask"], [[True, False], [False, True]])
        np.testing.assert_allclose(result["depth"], [[8. / 3., 0.], [0., 4.]], rtol=1e-7)
        self.assertEqual(result["statistics"]["valid_pixels"], 2)

    def test_full_k_pixel_centers_backproject_masked_depth_not_gaussian_centers(self):
        depth = np.full((2, 2), 2., dtype=np.float32)
        mask = np.asarray([[True, True], [False, True]])
        k = [[2., 0., .75], [0., 4., .25], [0., 0., 1.]]
        pose = [[1., 0., 0., 10.], [0., 1., 0., -20.], [0., 0., 1., 30.], [0., 0., 0., 1.]]
        points = backproject_camera_z(depth, mask, k, pose)
        np.testing.assert_array_equal(points, [[9.75, -19.875, 32.], [10.75, -19.875, 32.],
                                               [10.75, -19.375, 32.]])
        self.assertEqual(points.dtype, np.float64)


class GsGeometrySourceTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_GEOMETRY_TEST_ROOT", tempfile.gettempdir())) / "gsw-geometry-source"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.dataset, self.model = self.root / "dataset", self.root / "model"
        self.dataset.mkdir()
        views = []
        for index, (split, x) in enumerate((("train", -.2), ("train", .2), ("test", 0.))):
            name = str(index) + ".png"
            Image.new("RGB", (16, 12), (20 + index, 30, 40)).save(self.dataset / name)
            pose = np.eye(4)
            pose[:3, 3] = [x, 0., -4.]
            views.append({"view_id": "v-" + str(index), "image_path": name, "split": split,
                "K": [[12., 0., 7.25], [0., 13., 5.75], [0., 0., 1.]],
                "pixel_coordinates": "pixel_border", "c2w_opencv": pose.tolist()})
        raw = {"schema": camera_dataset.SCHEMA, "source": {"kind": "generated_fixture"},
            "pose_policy": "locked", "scale": {"status": "source_declared", "source_unit": "m",
                "meters_per_unit": 1., "evidence": [{"kind": "fixture_unit_not_accuracy"}]},
            "views": views, "initialization": {"kind": "random", "frame": "source", "seed": 7,
                "count": 16, "bounds": [[-.1, -.1, -.1], [.1, .1, .1]]}}
        camera_dataset.write_camera_dataset(raw, self.dataset / camera_dataset.MANIFEST_NAME)
        prepared = prepare_training_dataset(self.dataset, self.model / ".gsw/camera-input", resolution=1)
        self.contract = copy.deepcopy(prepared["contract"])
        self.contract.update(backend="3dgs", backend_identity={"fixture": "CPU_process_seam_not_GPU_proof"})
        loaded = []
        for view in self.contract["views"]:
            w2c = np.linalg.inv(np.asarray(view["training_c2w_opencv"]))
            projection = pinhole_projection(view["training_K"], *view["training_size"],
                self.contract["clipping"]["znear"], self.contract["clipping"]["zfar"])
            loaded.append({"view_id": view["view_id"], "split": view["split"],
                "K": view["training_K"], "image_size": view["training_size"],
                "world_view_transform_float32_transposed": w2c.astype(np.float32).T.tolist(),
                "projection_matrix_float32_transposed": np.asarray(projection, dtype=np.float32).T.tolist(),
                "calibration_requires_grad": False})
        self.contract["loaded_cameras"] = loaded
        self.ply = self.model / "point_cloud/iteration_7/point_cloud.ply"
        self.ply.parent.mkdir(parents=True)
        names = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2",
                 "rot_0", "rot_1", "rot_2", "rot_3"]
        values = np.zeros(2, dtype=[(name, "f4") for name in names])
        values["z"] = 1.
        values["rot_0"] = 1.
        values["opacity"] = 2.
        values["scale_0"] = values["scale_1"] = values["scale_2"] = -2.
        PlyData([PlyElement.describe(values, "vertex")]).write(str(self.ply))
        camera_dataset.write_camera_dataset(self.contract, self.model / "training-camera-contract.json")
        camera_dataset.write_camera_dataset(dict(self.contract, ply_sha256=file_digest(self.ply)),
                                             self.ply.with_suffix(".camera-scale.json"))
        camera_dataset.write_camera_dataset({"backend": "3dgs"}, self.model / "training_backend.json")

    def test_source_admission_consumes_locked_full_k_and_train_ids_without_photo_decode(self):
        with mock.patch.object(Image.Image, "convert", side_effect=AssertionError("GT pixels must not decode")):
            source = inspect_geometry_source(self.model, 7, self.dataset)
        self.assertEqual(source["input_data_identity"], self.contract["input_data_identity"])
        self.assertEqual([view["view_id"] for view in source["views"]], ["v-0", "v-1"])
        self.assertEqual(source["views"][0]["training_K"][0][2], 7.25)
        self.assertEqual(source["model_sha256"], file_digest(self.ply))
        self.assertEqual(source["contract"], self.contract)

    def rewrite_contract(self):
        camera_dataset.write_camera_dataset(self.contract, self.model / "training-camera-contract.json")
        camera_dataset.write_camera_dataset(dict(self.contract, ply_sha256=file_digest(self.ply)),
                                             self.ply.with_suffix(".camera-scale.json"))

    def test_malformed_executed_camera_is_a_typed_refusal_before_renderer(self):
        self.contract["loaded_cameras"][0]["projection_matrix_float32_transposed"] = "not-a-matrix"
        self.rewrite_contract()
        with self.assertRaisesRegex(GsGeometryError, "geometry_executed_camera_mismatch"):
            inspect_geometry_source(self.model, 7, self.dataset)

    def test_changed_model_and_changed_heldout_photo_are_integrity_failures_not_new_geometry_inputs(self):
        original = self.ply.read_bytes()
        self.ply.write_bytes(original + b"changed")
        with self.assertRaisesRegex(GsGeometryError, "geometry_model_hash_mismatch"):
            inspect_geometry_source(self.model, 7, self.dataset)
        self.ply.write_bytes(original)
        Image.new("RGB", (16, 12), (1, 2, 3)).save(self.dataset / "2.png")
        with self.assertRaisesRegex(GsGeometryError, "geometry_dataset_identity_mismatch"):
            inspect_geometry_source(self.model, 7, self.dataset)


if __name__ == "__main__":
    unittest.main()
