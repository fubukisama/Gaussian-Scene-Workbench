"""Owned CPU process seams; these are not actual CUDA/trained quality proof."""

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

import numpy as np
from plyfile import PlyData

from native.backend.tests import test_gs_geometry as fixtures
from native.worker.gs_geometry import extract_geometry


class PlaneRenderer:
    identity = {"kind": "owned_CPU_fixture_not_GPU_proof"}

    def render(self, view):
        width, height = view["training_size"]
        return {"alpha": np.full((height, width), .75, dtype=np.float32),
                "camera_z_moment": np.full((height, width), 2., dtype=np.float32)}

    def close(self):
        pass


class GsGeometryWorkerTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.GsGeometrySourceTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.config = {"schema": "gsw.gs_geometry_request/1", "model_directory": str(self.fixture.model),
            "dataset_path": str(self.fixture.dataset), "iteration": 7,
            "output_directory": str(self.fixture.root / "geometry-output"), "coordinate_mode": "canonical",
            "alpha_threshold": .5, "depth_range": {"min": .2, "max": 10., "frame": "training"},
            "pixel_stride": 1, "max_points": 1000, "memory_budget_mb": 8}

    def test_owned_output_is_pixel_depth_pointcloud_with_receipt_and_unchanged_model(self):
        before = self.fixture.ply.read_bytes()
        report = self.fixture.root / "receipt.json"
        result = extract_geometry(self.config, report, renderer_factory=lambda source: PlaneRenderer())
        self.assertTrue(result["success"])
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["manifestPath"], result["manifestPath"])
        manifest = json.loads(Path(result["manifestPath"]).read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "gsw.gs_geometry/1")
        self.assertEqual(manifest["geometry"]["fusion"], "none")
        self.assertEqual(manifest["geometry"]["surface_semantics"], "appearance_proxy_not_material_boundary")
        self.assertEqual(manifest["geometry"]["point_count"], 384)
        self.assertEqual([view["view_id"] for view in manifest["views"]], ["v-0", "v-1"])
        cloud = PlyData.read(result["geometryPath"])
        self.assertEqual(len(cloud["vertex"].data), 384)
        self.assertEqual(set(cloud["vertex"].data.dtype.names), {"x", "y", "z"})
        sidecar_path = Path(result["manifestPath"]).parent / manifest["geometry"]["coordinate_sidecar"]["path"]
        sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
        from native.backend.gs_geometry import file_sha256
        self.assertEqual(sidecar["schema"], "gsw.coordinate_export/1")
        self.assertEqual(sidecar["ply_sha256"], file_sha256(result["geometryPath"]))
        self.assertEqual(sidecar["training_contract"], self.fixture.contract)
        self.assertEqual(sidecar["output_frame"], "canonical")
        self.assertEqual(sidecar["output_unit"], "m")
        self.assertTrue(sidecar["applied_transform"]["applied"])
        self.assertEqual(sidecar["gaussian_dimensions"], 0)
        self.assertEqual(manifest["geometry"]["coordinate_sidecar"]["sha256"], file_sha256(sidecar_path))
        depth = np.load(Path(result["manifestPath"]).parent / manifest["views"][0]["depth"]["path"], allow_pickle=False)
        np.testing.assert_allclose(depth, 8. / 3., rtol=1e-7)
        self.assertEqual(self.fixture.ply.read_bytes(), before)

    def test_budget_failure_retains_depth_evidence_without_claiming_geometry(self):
        self.config["max_points"] = 100
        result = extract_geometry(self.config, self.fixture.root / "budget.json", renderer_factory=lambda source: PlaneRenderer())
        self.assertFalse(result["success"])
        self.assertEqual(result["error"]["code"], "geometry_point_budget_exceeded")
        manifest = json.loads(Path(result["manifestPath"]).read_text(encoding="utf-8"))
        self.assertEqual(manifest["status"], "failed")
        self.assertIsNone(manifest["geometry"])
        self.assertTrue((Path(result["manifestPath"]).parent / manifest["views"][0]["depth"]["path"]).is_file())
        self.assertFalse((Path(result["manifestPath"]).parent / "point_cloud.ply").exists())

    def test_cancel_between_views_preserves_previous_completed_depth_and_source(self):
        before = self.fixture.ply.read_bytes()
        stop = [False]
        result = extract_geometry(self.config, self.fixture.root / "cancel.json", renderer_factory=lambda source: PlaneRenderer(),
            cancelled=lambda: stop[0], progress=lambda value: stop.__setitem__(0, True))
        self.assertFalse(result["success"])
        self.assertEqual(result["error"]["code"], "geometry_cancelled")
        manifest = json.loads(Path(result["manifestPath"]).read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["views"]), 1)
        self.assertIsNone(manifest["geometry"])
        self.assertEqual(self.fixture.ply.read_bytes(), before)

    def test_existing_outputs_and_source_overlap_rejected_before_renderer(self):
        from native.backend.gs_geometry import GsGeometryError
        self.config["output_directory"] = str(self.fixture.model / "never")
        with mock.patch("native.worker.gs_geometry._CudaRenderer") as renderer:
            with self.assertRaisesRegex(GsGeometryError, "geometry_output_overlaps_source"):
                extract_geometry(self.config, self.fixture.root / "bad.json")
            renderer.assert_not_called()

    def test_metadata_and_photo_pixels_not_decoded_and_stride_explicit(self):
        self.config["pixel_stride"] = 2
        with mock.patch("PIL.Image.Image.convert", side_effect=AssertionError("Photo pixels decoded")):
            result = extract_geometry(self.config, self.fixture.root / "stride.json", renderer_factory=lambda source: PlaneRenderer())
        self.assertTrue(result["success"])
        self.assertEqual(result["summary"]["pointCount"], 96)

    def test_cli_invalid_request_exits_while_input_pipe_remains_open(self):
        request = self.fixture.root / "invalid.json"
        request.write_text('{"schema":"unsupported"}', encoding="utf-8")
        script = Path(__file__).with_name("gs_geometry.py")
        child = subprocess.Popen([sys.executable, "-B", str(script), "--config", str(request),
            "--report", str(self.fixture.root / "invalid-report.json")], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            code = child.wait(timeout=10)
            self.assertEqual(code, 1)
            self.assertFalse(child.stdin.closed)
            result = json.loads(child.stdout.read().strip().splitlines()[-1])
            self.assertEqual(result["error"]["code"], "geometry_invalid_request")
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=5)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()

    def test_native_job_maps_explicit_dataset_and_preserves_pointcloud_result_kind(self):
        from native.worker import mesh_generation
        events = []
        config = {"mode": "gs_depth", "modelDirectory": str(self.fixture.model), "cameraDatasetPath": str(self.fixture.dataset),
            "iteration": 7, "outputRoot": str(self.fixture.root / "native-output"), "runName": "owned-depth",
            "meshOptions": {key: self.config[key] for key in ("coordinate_mode", "alpha_threshold", "depth_range",
                "pixel_stride", "max_points", "memory_budget_mb")}}
        class Environment:
            @staticmethod
            def training_env(backend):
                return os.environ.copy()
            @staticmethod
            def training_python(backend):
                return Path(sys.executable)
        def child_boundary(command, request, control, factory, on_line):
            value = json.loads(request.read_text(encoding="utf-8"))
            self.assertEqual(value["dataset_path"], str(self.fixture.dataset))
            self.assertEqual(value["schema"], "gsw.gs_geometry_request/1")
            extract_geometry(value, command[-1], renderer_factory=lambda source: PlaneRenderer())
            return 0, set()
        with mock.patch("native.worker.training_batch._run_child", side_effect=child_boundary):
            code = mesh_generation.run(config, Environment(), lambda *values: events.append(values))
        self.assertEqual(code, 0)
        record = json.loads((Path(config["outputRoot"]) / config["runName"] / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(record["kind"], "depth_point_cloud")
        self.assertNotIn("meshPath", record)
        self.assertEqual(record["completedStages"], ["depth", "pointcloud"])
        self.assertEqual(events[-1][3]["previewKind"], "points")

    def test_2dgs_uses_same_product_contract_but_distinct_depth_definition(self):
        from plyfile import PlyElement
        from native.backend import camera_dataset
        original = PlyData.read(str(self.fixture.ply), mmap=False)["vertex"].data
        names = [name for name in original.dtype.names if name != "scale_2"]
        values = np.empty(len(original), dtype=[(name, original.dtype[name]) for name in names])
        for name in names:
            values[name] = original[name]
        PlyData([PlyElement.describe(values, "vertex")]).write(str(self.fixture.ply))
        self.fixture.contract["backend"] = "2dgs"
        self.fixture.rewrite_contract()
        camera_dataset.write_camera_dataset({"backend": "2dgs"}, self.fixture.model / "training_backend.json")
        result = extract_geometry(self.config, self.fixture.root / "2dgs.json", renderer_factory=lambda source: PlaneRenderer())
        self.assertTrue(result["success"])
        manifest = json.loads(Path(result["manifestPath"]).read_text(encoding="utf-8"))
        self.assertEqual(manifest["source"]["backend"], "2dgs")
        self.assertEqual(manifest["views"][0]["depth_definition"], "expected_camera_z_surfel_ray_intersection_alpha_compositing")
        self.assertEqual(manifest["geometry"]["point_count"], 384)

    def test_extraction_detects_source_mutation_before_publication(self):
        class ChangedSourceRenderer(PlaneRenderer):
            def render(inner, view):
                if view["view_id"] == "v-1":
                    with self.fixture.ply.open("ab") as stream:
                        stream.write(b"concurrent source modification")
                return super(ChangedSourceRenderer, inner).render(view)
        result = extract_geometry(self.config, self.fixture.root / "changed.json", renderer_factory=lambda source: ChangedSourceRenderer())
        self.assertFalse(result["success"])
        self.assertEqual(result["error"]["code"], "geometry_model_hash_mismatch")
        manifest = json.loads(Path(result["manifestPath"]).read_text(encoding="utf-8"))
        self.assertIsNone(manifest["geometry"])
        self.assertEqual(len(manifest["views"]), 2)


if __name__ == "__main__":
    unittest.main()
