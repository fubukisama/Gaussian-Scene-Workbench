"""Gaussian-aware scale restoration through real PLY files, without CUDA."""
import copy
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
            "input_data_identity": "test-data-identity", "pose_policy": "locked",
            "scale": scale or {"status": "source_declared", "source_unit": "cm",
                "meters_per_unit": .01, "canonical_unit": "m", "evidence": [{"kind": "source_declaration"}]},
            "transforms": {"source_to_canonical": {"s": .01, "R": identity, "t": [0., 0., 0.],
                "input_frame": "source", "output_frame": "canonical", "applied": True},
                "canonical_to_training": {"s": 20., "R": rotation or identity, "t": [0., 0., -.5],
                "input_frame": "canonical", "output_frame": "training", "applied": True}}}
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

    def test_unknown_scale_cannot_claim_meters_and_source_is_never_overwritten(self):
        source, _, _ = self.fixture(scale={"status": "arbitrary", "source_unit": "unknown",
            "meters_per_unit": None, "canonical_unit": "arbitrary", "evidence": []})
        before = source.read_bytes()
        with self.assertRaisesRegex(ValueError, "unknown|metric|scale"):
            camera_scale_export.export_camera_scale_ply(source, self.root / "wrong-metric.ply", mode="metric")
        with self.assertRaisesRegex(ValueError, "source|overwrite|new"):
            camera_scale_export.export_camera_scale_ply(source, source, mode="source")
        self.assertEqual(source.read_bytes(), before)

    def test_rotation_with_directional_sh_is_rejected_without_partial_publication(self):
        rotation = [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]]
        source, _, _ = self.fixture(rotation=rotation, sh=True)
        destination = self.root / "rotated.ply"
        with self.assertRaisesRegex(ValueError, "SH|rotation|unsupported"):
            camera_scale_export.export_camera_scale_ply(source, destination, mode="metric")
        self.assertFalse(destination.exists())
        self.assertFalse(destination.with_suffix(".camera-scale.json").exists())

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
        self.assertEqual(sidecar["schema"], "gsw.training_camera_contract/1")
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
        with self.assertRaisesRegex(ValueError, "already_restored"):
            camera_scale_export.export_camera_scale_ply(metric, self.root / "incorrect-double.ply", mode="metric")


if __name__ == "__main__":
    unittest.main()
