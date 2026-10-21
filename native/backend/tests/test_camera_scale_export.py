"""Gaussian-aware scale restoration through real PLY files, without CUDA."""
import copy
import ast
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

import numpy as np
from plyfile import PlyData, PlyElement

from native.backend import camera_scale_export


class CameraScaleExportTests(unittest.TestCase):
    def setUp(self):
        base = Path("D:/Codex/qa-camera-p0")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def fixture(self, backend="3dgs", scale=None, rotation=None, sh=False):
        dimensions = 2 if backend == "2dgs" else 3
        names = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity"]
        names += ["scale_{}".format(i) for i in range(dimensions)]
        names += ["rot_{}".format(i) for i in range(4)]
        names += ["cov_{}".format(i) for i in range(6)]
        if sh:
            names += ["f_rest_{}".format(i) for i in range(9)]
        vertices = np.zeros(1, dtype=[(name, "f4") for name in names])
        for name, value in zip(("x", "y", "z"), (1., 2., 3.)):
            vertices[name] = value
        for index in range(dimensions):
            vertices["scale_{}".format(index)] = -2. - index
        vertices["rot_0"] = 1.
        vertices["opacity"] = .7
        vertices["f_dc_0"] = .3
        for index, value in enumerate((1., 0., 0., 4., 0., 9.)):
            vertices["cov_{}".format(index)] = value
        if sh:
            vertices["f_rest_0"] = .123
        path = self.root / "training.ply"
        PlyData([PlyElement.describe(vertices, "vertex")]).write(path)
        identity = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        metadata = {"schema": "gsw.training_camera_contract/1", "backend": backend,
            "input_data_identity": "", "pose_policy": "locked",
            "scale": scale or {"status": "source_declared", "source_unit": "cm",
                "meters_per_unit": .01, "canonical_unit": "m", "evidence": [{"kind": "source_declaration"}]},
            "transforms": {"source_to_canonical": {"s": .01, "R": identity, "t": [0., 0., 0.],
                "input_frame": "source", "output_frame": "canonical", "applied": True},
                "canonical_to_training": {"s": 20., "R": rotation or identity, "t": [0., 0., -.5],
                "input_frame": "canonical", "output_frame": "training", "applied": True}}}
        metadata["input_data_identity"] = hashlib.sha256(json.dumps({
            "generated_source_positions": [[1., 2., 3.]], "scale": metadata["scale"],
            "transforms": metadata["transforms"]}, sort_keys=True).encode("utf-8")).hexdigest()
        sidecar = path.with_suffix(".camera-scale.json")
        metadata["ply_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        sidecar.write_text(json.dumps(metadata), encoding="utf-8")
        return path, vertices, metadata

    def test_source_export_restores_xyz_log_scales_and_covariance_together_for_both_backends(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                source, original, _ = self.fixture(backend)
                digest = hashlib.sha256(source.read_bytes()).hexdigest()
                destination = self.root / (backend + "-source.ply")
                report = camera_scale_export.export_camera_scale_ply(source, destination, mode="source")
                data = PlyData.read(destination)["vertex"].data
                np.testing.assert_allclose([[data[name][0] for name in ("x", "y", "z")]], [[5., 10., 17.5]], atol=1e-12)
                for index in range(2 if backend == "2dgs" else 3):
                    self.assertAlmostEqual(float(data["scale_{}".format(index)][0]), -2. - index + math.log(5.), places=6)
                np.testing.assert_allclose([data["cov_{}".format(i)][0] for i in range(6)], [25., 0., 0., 100., 0., 225.])
                self.assertEqual(data["opacity"].tobytes(), original["opacity"].tobytes())
                self.assertEqual(data["rot_0"].tobytes(), original["rot_0"].tobytes())
                self.assertEqual(report["output_unit"], "cm")
                self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), digest)

    def test_metric_export_uses_inverse_training_chain_once_not_unit_scale_twice(self):
        source, _, _ = self.fixture()
        destination = self.root / "metric.ply"
        report = camera_scale_export.export_camera_scale_ply(source, destination, mode="metric")
        data = PlyData.read(destination)["vertex"].data
        np.testing.assert_allclose([data[name][0] for name in ("x", "y", "z")], [.05, .1, .175], atol=1e-12)
        self.assertAlmostEqual(float(data["scale_0"][0]), -2. + math.log(.05), places=6)
        self.assertEqual(report["output_unit"], "m")
        self.assertTrue(destination.with_suffix(".camera-scale.json").is_file())

    def test_metric_bundle_binds_model_and_exact_transformed_cameras_without_images(self):
        source, _, contract = self.fixture()
        contract["views"] = [{"view_id": "stable-a", "split": "train", "image_path": "missing/photo.png",
            "width": 640, "height": 480, "training_size": [320, 240],
            "K": [[500., 0., 200.], [0., 600., 150.], [0., 0., 1.]],
            "training_K": [[250., 0., 100.], [0., 300., 75.], [0., 0., 1.]],
            "training_c2w_opencv": [[1., 0., 0., 2.], [0., 1., 0., 3.], [0., 0., 1., 4.], [0., 0., 0., 1.]],
            "projection_model": "PINHOLE", "pixel_coordinates": "pixel_border"}]
        contract["clipping"] = {"znear": .2, "zfar": 50., "frame": "training"}
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        destination = self.root / "camera-metric.ply"
        report = camera_scale_export.export_camera_scale_ply(source, destination, mode="metric")
        cameras_path = destination.with_suffix(".cameras.json")
        self.assertTrue(cameras_path.is_file())
        cameras = json.loads(cameras_path.read_text(encoding="utf-8"))
        self.assertEqual(cameras["schema"], "gsw.model_cameras/1")
        self.assertEqual(cameras["model_sha256"], hashlib.sha256(destination.read_bytes()).hexdigest())
        self.assertEqual(cameras["coordinate_frame"], "canonical")
        self.assertEqual(cameras["coordinate_unit"], "m")
        self.assertEqual(cameras["input_data_identity"], contract["input_data_identity"])
        self.assertEqual(cameras["views"][0]["K"], contract["views"][0]["training_K"])
        self.assertEqual([cameras["views"][0][key] for key in ("width", "height", "view_id", "split")], [320, 240, "stable-a", "train"])
        np.testing.assert_allclose(np.asarray(cameras["views"][0]["c2w_opencv"])[:3, 3], [.1, .15, .225], atol=1e-15)
        self.assertAlmostEqual(cameras["clipping"]["znear"], .01, places=15)
        self.assertEqual(cameras["clipping"]["zfar"], 2.5)
        self.assertEqual(report["camera_reference"]["path"], "camera-metric.cameras.json")
        observed = camera_scale_export.inspect_coordinate_metadata(destination)
        self.assertTrue(observed["metric_available"])
        self.assertEqual(observed["current_frame"], "canonical")
        self.assertEqual(observed["current_unit"], "m")
        cameras_path.write_bytes(cameras_path.read_bytes() + b" ")
        with self.assertRaisesRegex(ValueError, "camera.*hash|bundle"):
            camera_scale_export.inspect_coordinate_metadata(destination)

    def test_default_delivery_is_metric_retryable_and_unknown_scale_is_not_success(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                source, _, _ = self.fixture(backend)
                before = source.read_bytes()
                output = self.root / (backend + "-delivery")
                first = camera_scale_export.deliver_training_result(source, output)
                self.assertEqual(first["training_status"], "done")
                self.assertEqual(first["export_status"], "ready")
                self.assertEqual(Path(first["model_path"]).name, "training.metric-m.ply")
                self.assertEqual(first["report"]["output_unit"], "m")
                again = camera_scale_export.deliver_training_result(source, output)
                self.assertEqual(again["model_path"], first["model_path"])
                self.assertTrue(again["already_available"])
                self.assertEqual(source.read_bytes(), before)
        source, _, _ = self.fixture(scale={"status": "arbitrary", "source_unit": "unknown",
            "meters_per_unit": None, "canonical_unit": "arbitrary", "evidence": []})
        blocked = camera_scale_export.deliver_training_result(source, self.root / "unknown-delivery")
        self.assertEqual(blocked["training_status"], "done")
        self.assertEqual(blocked["export_status"], "blocked")
        self.assertEqual(Path(blocked["model_path"]), source)
        self.assertEqual(blocked["reason"], "camera_scale_export_unknown_metric_scale")
        self.assertFalse((self.root / "unknown-delivery").exists())

    def test_bundle_publication_race_rolls_back_only_own_files(self):
        source, _, _ = self.fixture()
        destination = self.root / "race.ply"
        competitor = destination.with_suffix(".cameras.json")
        def race(percent):
            if percent == 95:
                competitor.write_bytes(b"another writer owns this path")
        with self.assertRaises(FileExistsError):
            camera_scale_export.export_camera_scale_ply(source, destination, mode="metric", progress=race)
        self.assertFalse(destination.exists())
        self.assertFalse(destination.with_suffix(".camera-scale.json").exists())
        self.assertEqual(competitor.read_bytes(), b"another writer owns this path")
        self.assertFalse(any(path.name.endswith(".tmp") for path in self.root.iterdir()))

    def test_nested_legacy_sidecars_are_unwrapped_but_mismatched_history_is_rejected(self):
        source, _, _ = self.fixture()
        first = self.root / "first.ply"
        report = camera_scale_export.export_camera_scale_ply(source, first, mode="metric")
        nested = self.root / "nested.ply"
        nested.write_bytes(first.read_bytes())
        outer = {"schema": "gsw.coordinate_export/1", "success": True, "output_frame": "canonical", "output_unit": "m",
                 "input_sha256": report["ply_sha256"], "output_sha256": report["ply_sha256"],
                 "ply_sha256": report["ply_sha256"], "training_contract": report}
        nested.with_suffix(".camera-scale.json").write_text(json.dumps(outer), encoding="utf-8")
        output = self.root / "flattened.ply"
        flattened = camera_scale_export.export_camera_scale_ply(nested, output, mode="metric")
        self.assertEqual(flattened["training_contract"]["schema"], "gsw.training_camera_contract/1")
        actual = PlyData.read(output, mmap=False)["vertex"].data
        np.testing.assert_allclose([actual[name][0] for name in ("x", "y", "z")], [.05, .1, .175], atol=1e-12)
        outer["training_contract"]["ply_sha256"] = "0" * 64
        nested.with_suffix(".camera-scale.json").write_text(json.dumps(outer), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "nested_hash_mismatch"):
            camera_scale_export.export_camera_scale_ply(nested, self.root / "wrong-nested.ply", mode="metric")

    def test_applied_state_and_transform_corruption_are_not_accepted_as_metric_delivery(self):
        source, _, _ = self.fixture()
        metric = self.root / "verified.ply"
        camera_scale_export.export_camera_scale_ply(source, metric, mode="metric")
        metadata = metric.with_suffix(".camera-scale.json")
        original = json.loads(metadata.read_text(encoding="utf-8"))
        wrong_applied = copy.deepcopy(original)
        wrong_applied["transform_applied"] = False
        wrong_chain = copy.deepcopy(original)
        wrong_chain["applied_transform"]["s"] *= 2
        wrong_dataset = copy.deepcopy(original)
        wrong_dataset["input_data_identity"] = "another-dataset"
        for index, invalid in enumerate((wrong_applied, wrong_chain, wrong_dataset)):
            with self.subTest(index=index):
                metadata.write_text(json.dumps(invalid), encoding="utf-8")
                destination = self.root / ("invalid-export-" + str(index) + ".ply")
                with self.assertRaises(ValueError):
                    camera_scale_export.export_camera_scale_ply(metric, destination, mode="metric")
                self.assertFalse(destination.exists())
                delivery = camera_scale_export.deliver_training_result(metric, self.root / ("invalid-delivery-" + str(index)))
                self.assertEqual(delivery["training_status"], "done")
                self.assertEqual(delivery["export_status"], "failed")
        metadata.write_text(json.dumps(original), encoding="utf-8")

    def test_plain_point_cloud_and_indexed_mesh_are_not_labelled_complete_gaussians(self):
        source, base, contract = self.fixture()
        point = np.zeros(2, dtype=[("x", "f8"), ("y", "f8"), ("z", "f8"), ("red", "u1")])
        PlyData([PlyElement.describe(point, "vertex")]).write(source)
        contract["ply_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "requires_supported_gaussian"):
            camera_scale_export.export_camera_scale_ply(source, self.root / "not-full-gs.ply", mode="metric")
        face = np.array([([0, 0, 0],)], dtype=[("vertex_indices", "i4", (3,))])
        PlyData([PlyElement.describe(base, "vertex"), PlyElement.describe(face, "face")]).write(source)
        contract["ply_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "mesh_not_supported"):
            camera_scale_export.export_camera_scale_ply(source, self.root / "not-gaussian-mesh.ply", mode="metric")

    def test_direction_attributes_and_covariance_rotate_but_unrelated_properties_do_not(self):
        rotation = [[1., 0., 0.], [0., 0., -1.], [0., 1., 0.]]
        source, base, contract = self.fixture("2dgs", rotation=rotation)
        names = list(base.dtype.names) + ["nx", "ny", "nz", "tangent_x", "tangent_y", "tangent_z", "confidence"]
        data = np.zeros(1, dtype=[(name, "f4") for name in names])
        for name in base.dtype.names:
            data[name] = base[name]
        data["ny"], data["tangent_x"], data["confidence"] = 1., 1., .8123
        PlyData([PlyElement.describe(data, "vertex")]).write(source)
        contract["ply_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        output = self.root / "surfel-directions.ply"
        camera_scale_export.export_camera_scale_ply(source, output, mode="metric")
        changed = PlyData.read(output, mmap=False)["vertex"].data
        np.testing.assert_allclose([changed[name][0] for name in ("nx", "ny", "nz")], [0., 0., -1.], atol=1e-7)
        np.testing.assert_allclose([changed[name][0] for name in ("tangent_x", "tangent_y", "tangent_z")], [1., 0., 0.], atol=1e-7)
        np.testing.assert_allclose([changed["cov_" + str(index)][0] for index in range(6)], [.0025, 0., 0., .0225, 0., .01], atol=1e-8)
        self.assertEqual(changed["confidence"].tobytes(), data["confidence"].tobytes())
        self.assertNotIn("scale_2", changed.dtype.names)

    def test_unknown_scale_cannot_claim_meters_and_source_is_never_overwritten(self):
        source, _, _ = self.fixture(scale={"status": "arbitrary", "source_unit": "unknown",
            "meters_per_unit": None, "canonical_unit": "arbitrary", "evidence": []})
        before = source.read_bytes()
        with self.assertRaisesRegex(ValueError, "unknown|metric|scale"):
            camera_scale_export.export_camera_scale_ply(source, self.root / "wrong-metric.ply", mode="metric")
        with self.assertRaisesRegex(ValueError, "source|overwrite|new"):
            camera_scale_export.export_camera_scale_ply(source, source, mode="source")
        self.assertEqual(source.read_bytes(), before)
        arbitrary = self.root / "explicit-arbitrary.ply"
        report = camera_scale_export.export_camera_scale_ply(source, arbitrary, mode="canonical")
        cameras = json.loads(arbitrary.with_suffix(".cameras.json").read_text(encoding="utf-8"))
        self.assertEqual(report["output_unit"], "arbitrary")
        self.assertEqual(cameras["coordinate_unit"], "arbitrary")
        self.assertFalse(camera_scale_export.inspect_coordinate_metadata(arbitrary)["metric_available"])

    def test_units_normalization_origins_and_rotations_roundtrip_both_gaussian_representations(self):
        rz = np.array([[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]])
        rx = np.array([[1., 0., 0.], [0., 0., -1.], [0., 1., 0.]])
        case = 0
        for backend in ("3dgs", "2dgs"):
            for unit, meters in (("m", 1.), ("cm", .01), ("mm", .001)):
                for normalized in (False, True):
                    for large in (False, True):
                        for rotated in (False, True):
                            case += 1
                            with self.subTest(backend=backend, unit=unit, normalized=normalized, large=large, rotated=rotated):
                                source, base, contract = self.fixture(backend)
                                origin = np.array([1e8, -2e8, 3e8]) if large else np.array([.4, -.8, 1.2])
                                source_r, training_r = (rz, rx) if rotated else (np.eye(3), np.eye(3))
                                scale = 13.7 if normalized else 1.
                                offset = np.array([.1, -.2, .3])
                                original = np.array([[1., 2., 3.], [4., 2., 3.]])
                                canonical = meters * (original @ source_r.T) + origin
                                training = scale * ((canonical - origin) @ training_r.T) + offset
                                data = np.empty(2, dtype=[(name, "f8" if name in ("x", "y", "z") else "f4") for name in base.dtype.names])
                                for name in base.dtype.names:
                                    data[name] = base[name][0]
                                for index, name in enumerate(("x", "y", "z")):
                                    data[name] = training[:, index]
                                PlyData([PlyElement.describe(data, "vertex")]).write(source)
                                contract["scale"].update(source_unit=unit, meters_per_unit=meters)
                                contract["transforms"]["source_to_canonical"].update(s=meters, R=source_r.tolist(), t=origin.tolist())
                                contract["transforms"]["canonical_to_training"].update(s=scale, R=training_r.tolist(), t=(offset - scale * training_r @ origin).tolist())
                                contract["ply_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
                                source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
                                before = source.read_bytes()
                                metric = self.root / ("matrix-" + str(case) + ".ply")
                                camera_scale_export.export_camera_scale_ply(source, metric, mode="metric")
                                values = PlyData.read(metric, mmap=False)["vertex"].data
                                actual = np.column_stack([values[name] for name in ("x", "y", "z")])
                                np.testing.assert_allclose(actual, canonical, rtol=0, atol=2e-7 if large else 1e-12)
                                # Independent literal source distance=3units;
                                # neither target dimensions nor fitted scale.
                                self.assertAlmostEqual(float(np.linalg.norm(actual[1] - actual[0])), 3. * meters, delta=2e-7 if large else 1e-12)
                                restored = self.root / ("roundtrip-" + str(case) + ".ply")
                                camera_scale_export.export_camera_scale_ply(metric, restored, mode="source")
                                result = PlyData.read(restored, mmap=False)["vertex"].data
                                xyz = np.column_stack([result[name] for name in ("x", "y", "z")])
                                np.testing.assert_allclose(xyz, original, rtol=0, atol=2e-7 / meters if large else 1e-10)
                                self.assertEqual(source.read_bytes(), before)

    def test_rotation_of_all_supported_sh_bands_preserves_upstream_direction_function(self):
        # Independent renderer reference: the existing BSD PlenOctree evaluator,
        # loaded without its unrelated Torch import (eval_sh accepts NumPy).
        reference_path = Path(__file__).resolve().parents[3] / "gaussian-splatting/utils/sh_utils.py"
        parsed = ast.parse(reference_path.read_text(encoding="utf-8"))
        nodes = [node for node in parsed.body if isinstance(node, ast.Assign) or
                 isinstance(node, ast.FunctionDef) and node.name == "eval_sh"]
        scope = {}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(reference_path), "exec"), scope)
        reference = scope["eval_sh"]
        a, b = .73, -.41
        rotation = np.array([[math.cos(a), -math.sin(a), 0], [math.sin(a), math.cos(a), 0], [0, 0, 1]]) @ np.array(
            [[1, 0, 0], [0, math.cos(b), -math.sin(b)], [0, math.sin(b), math.cos(b)]])
        rng = np.random.default_rng(2031)
        directions = rng.normal(size=(257, 3))
        directions /= np.linalg.norm(directions, axis=1, keepdims=True)
        for backend in ("3dgs", "2dgs"):
            for degree in (1, 2, 3, 4):
                with self.subTest(backend=backend, degree=degree):
                    source, base, contract = self.fixture(backend, rotation=rotation.tolist())
                    count = (degree + 1) ** 2
                    names = list(base.dtype.names) + ["f_rest_" + str(index) for index in range(3 * (count - 1))]
                    data = np.empty(1, dtype=[(name, "f4") for name in names])
                    for name in base.dtype.names:
                        data[name] = base[name]
                    coefficients = rng.normal(scale=.2, size=(3, count)).astype("f4")
                    for channel in range(3):
                        data["f_dc_" + str(channel)] = coefficients[channel, 0]
                    for index, value in enumerate(coefficients[:, 1:].reshape(-1)):
                        data["f_rest_" + str(index)] = value
                    PlyData([PlyElement.describe(data, "vertex")]).write(source)
                    contract["ply_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
                    source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
                    destination = self.root / (backend + "-sh" + str(degree) + ".ply")
                    report = camera_scale_export.export_camera_scale_ply(source, destination, mode="metric")
                    changed = PlyData.read(destination, mmap=False)["vertex"].data
                    actual = np.empty_like(coefficients)
                    actual[:, 0] = [changed["f_dc_" + str(channel)][0] for channel in range(3)]
                    actual[:, 1:] = np.array([changed["f_rest_" + str(index)][0] for index in range(3 * (count - 1))]).reshape(3, count - 1)
                    # File rotation is inverse of canonical->training. A target
                    # direction maps to original by R_target^T (row form below).
                    original_directions = directions @ rotation.T
                    expected_rgb = reference(degree, np.broadcast_to(coefficients, (len(directions), 3, count)), original_directions)
                    actual_rgb = reference(degree, np.broadcast_to(actual, (len(directions), 3, count)), directions)
                    np.testing.assert_allclose(actual_rgb, expected_rgb, rtol=0, atol=2e-7)
                    self.assertEqual(report["appearance_rotation"], "real_sh_basis_rotation")
                    self.assertEqual(report["sh_rotation"]["degree"], degree)

    def test_cancelled_export_keeps_original_and_does_not_publish(self):
        source, _, _ = self.fixture()
        destination = self.root / "cancelled.ply"
        before = source.read_bytes()
        with self.assertRaisesRegex(ValueError, "cancel"):
            camera_scale_export.export_camera_scale_ply(source, destination, cancelled=lambda: True)
        self.assertEqual(source.read_bytes(), before)
        self.assertFalse(destination.exists())

    def test_dc_only_rotation_changes_orientation_and_covariance_consistently(self):
        rotation = [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]]
        source, _, _ = self.fixture(rotation=rotation)
        destination = self.root / "rotated-dc.ply"
        camera_scale_export.export_camera_scale_ply(source, destination, mode="metric")
        data = PlyData.read(destination)["vertex"].data
        np.testing.assert_allclose([data[name][0] for name in ("x", "y", "z")], [.1, -.05, .175], atol=1e-12)
        np.testing.assert_allclose([data["rot_{}".format(i)][0] for i in range(4)],
                                   [math.sqrt(.5), 0., 0., -math.sqrt(.5)], atol=1e-7)
        np.testing.assert_allclose([data["cov_{}".format(i)][0] for i in range(6)],
                                   [.01, 0., 0., .0025, 0., .0225], rtol=1e-6)

    def test_stale_model_sidecar_hash_is_rejected_before_publication(self):
        source, _, _ = self.fixture()
        source.write_bytes(source.read_bytes() + b"\n")
        destination = self.root / "stale.ply"
        with self.assertRaisesRegex(ValueError, "hash_mismatch"):
            camera_scale_export.export_camera_scale_ply(source, destination, mode="metric")
        self.assertFalse(destination.exists())

    def test_public_cli_handles_unicode_paths_and_reports_real_progress(self):
        source, _, _ = self.fixture()
        destination = self.root / "尺度 復元 metric.ply"
        result = subprocess.run([sys.executable, "-B", "-m", "native.backend.camera_scale_export",
                                 "--input", str(source), "--output", str(destination),
                                 "--coordinates", "metric", "--progress-json"],
            cwd=str(Path(__file__).resolve().parents[3]), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            encoding="utf-8", errors="strict", timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertTrue(any(line.startswith("[gsw-coordinate-export-progress] ") for line in lines))
        report = json.loads(lines[-1])
        self.assertTrue(report["success"])
        self.assertEqual(report["output_unit"], "m")
        self.assertEqual(report["ply_sha256"], hashlib.sha256(destination.read_bytes()).hexdigest())

    def test_training_coordinate_subset_retains_training_chain_and_updates_model_hash(self):
        source, vertices, original_contract = self.fixture()
        PlyData([PlyElement.describe(np.repeat(vertices, 2), "vertex")]).write(source)
        original_contract["ply_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        source.with_suffix(".camera-scale.json").write_text(json.dumps(original_contract), encoding="utf-8")
        destination = self.root / "subset.ply"
        camera_scale_export.export_camera_scale_ply(source, destination, mode="training", deleted_indices=[1])
        data = PlyData.read(destination)["vertex"].data
        self.assertEqual(len(data), 1)
        np.testing.assert_array_equal([data[name][0] for name in ("x", "y", "z")], [1., 2., 3.])
        self.assertEqual(float(data["scale_0"][0]), -2.)
        sidecar = json.loads(destination.with_suffix(".camera-scale.json").read_text(encoding="utf-8"))
        self.assertEqual(sidecar["schema"], "gsw.coordinate_export/1")
        self.assertEqual(sidecar["transforms"], original_contract["transforms"])
        self.assertEqual(sidecar["scale"], original_contract["scale"])
        self.assertEqual(sidecar["export_operations"][-1]["deleted_vertices"], 1)
        self.assertEqual(sidecar["ply_sha256"], hashlib.sha256(destination.read_bytes()).hexdigest())

    def test_restored_coordinate_copy_never_applies_inverse_chain_twice(self):
        source, _, _ = self.fixture()
        metric = self.root / "first-metric.ply"
        copied = self.root / "copied-metric.ply"
        camera_scale_export.export_camera_scale_ply(source, metric, mode="metric")
        camera_scale_export.export_camera_scale_ply(metric, copied, mode="preserve")
        expected, actual = PlyData.read(metric)["vertex"].data, PlyData.read(copied)["vertex"].data
        for name in ("x", "y", "z", "scale_0", "scale_1", "scale_2"):
            np.testing.assert_array_equal(actual[name], expected[name])
        sidecar = json.loads(copied.with_suffix(".camera-scale.json").read_text(encoding="utf-8"))
        self.assertEqual(sidecar["schema"], "gsw.coordinate_export/1")
        self.assertEqual(sidecar["output_frame"], "canonical")
        self.assertEqual(sidecar["output_unit"], "m")
        repeated = self.root / "same-frame-metric.ply"
        again = camera_scale_export.export_camera_scale_ply(metric, repeated, mode="metric")
        same = PlyData.read(repeated)["vertex"].data
        for name in ("x", "y", "z", "scale_0", "scale_1", "scale_2"):
            np.testing.assert_array_equal(same[name], expected[name])
        self.assertEqual(again["applied_transform"]["s"], 1.0)
        self.assertTrue(again["applied_transform"]["applied"])
        self.assertEqual(again["training_contract"]["schema"], "gsw.training_camera_contract/1")
        millimeters = self.root / "millimeters.ply"
        camera_scale_export.export_camera_scale_ply(metric, millimeters, mode="metric", output_unit="mm")
        mm = PlyData.read(millimeters)["vertex"].data
        np.testing.assert_allclose([mm[name][0] for name in ("x", "y", "z")], [50., 100., 175.], atol=1e-12)


if __name__ == "__main__":
    unittest.main()
