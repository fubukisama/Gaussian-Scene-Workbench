"""Recorded-frame PLY exports through the public CPU interface and real files."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np
from plyfile import PlyData, PlyElement

from native.backend import camera_scale_export


class RecordedPlyExportTests(unittest.TestCase):
    def setUp(self):
        selected = os.environ.get("GSW_RECORDED_PLY_TEST_ROOT")
        base = Path(selected) if selected else (
            Path("D:/Apps/GSW/validation/v3-remaining-oct21/backend")
            if os.name == "nt" and Path("D:/").is_dir()
            else Path(tempfile.gettempdir()) / "gsw-recorded-ply-tests")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def point_fixture(self):
        vertices = np.zeros(3, dtype=[
            ("x", "f4"), ("y", "f4"), ("z", "f4"),
            ("nx", "f4"), ("ny", "f4"), ("nz", "f4"),
            ("confidence", "f4"), ("label", "u2")])
        vertices["x"] = [6., 10., 8.]
        vertices["y"] = [-4., -2., -6.]
        vertices["z"] = [9., 5., 7.]
        vertices["ny"] = 1.
        vertices["confidence"] = [.25, .5, .75]
        vertices["label"] = [7, 8, 9]
        source = self.root / "owned-training.ply"
        PlyData([PlyElement.describe(vertices, "vertex")],
                comments=["owned generated points", "units training"]).write(source)
        identity = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        rotation = [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]]
        contract = {
            "schema": "gsw.training_camera_contract/1", "backend": "3dgs",
            "input_data_identity": hashlib.sha256(b"owned asymmetric recorded geometry").hexdigest(),
            "pose_policy": "locked",
            "scale": {"status": "source_declared", "source_unit": "cm",
                      "meters_per_unit": .01, "canonical_unit": "m",
                      "evidence": [{"kind": "source_declaration", "source": "generated-fixture"}]},
            "transforms": {
                "source_to_canonical": {"s": .01, "R": identity, "t": [0., 0., 0.],
                    "input_frame": "source", "output_frame": "canonical", "applied": True},
                "canonical_to_training": {"s": 2., "R": rotation, "t": [10., -6., 3.],
                    "input_frame": "canonical", "output_frame": "training", "applied": True}},
            "views": [{"view_id": "owned-camera-a", "split": "train", "image_path": "missing-pose-only.png",
                "width": 640, "height": 480,
                "K": [[510., 13., 321.25], [0., 490., 207.75], [0., 0., 1.]],
                "c2w_opencv": [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]],
                "projection_model": "PINHOLE", "pixel_coordinates": "pixel_border"}],
            "clipping": {"frame": "training", "znear": .2, "backend_near": .4, "zfar": 40.}}
        contract["ply_sha256"] = self.hash(source)
        metadata = source.with_suffix(".camera-scale.json")
        metadata.write_text(json.dumps(contract), encoding="utf-8")
        return source, vertices, contract

    @staticmethod
    def hash(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    def test_plain_points_restore_metric_positions_normals_attributes_and_bound_cameras(self):
        source, original, contract = self.point_fixture()
        metadata = source.with_suffix(".camera-scale.json")
        original_hashes = (self.hash(source), self.hash(metadata))
        destination = self.root / "metric-points.ply"
        report = camera_scale_export.export_recorded_ply(
            source, destination, mode="metric", representation="point_cloud")
        data = PlyData.read(destination, mmap=False)["vertex"].data
        np.testing.assert_array_equal(
            np.column_stack([data[name] for name in ("x", "y", "z")]),
            [[1., 2., 3.], [2., 0., 1.], [0., 1., 2.]])
        self.assertTrue(all(data.dtype[name] == np.dtype("f8") for name in ("x", "y", "z")))
        np.testing.assert_array_equal(
            np.column_stack([data[name] for name in ("nx", "ny", "nz")]),
            [[1., 0., 0.], [1., 0., 0.], [1., 0., 0.]])
        for name in ("confidence", "label"):
            self.assertEqual(data[name].dtype, original[name].dtype)
            self.assertEqual(data[name].tobytes(), original[name].tobytes())
        self.assertEqual(report["representation"], "point_cloud")
        self.assertEqual(report["export_version"], 3)
        self.assertEqual(report["output_unit"], "m")
        self.assertFalse(report["surface_accuracy_certified"])
        self.assertEqual(report["training_contract"], contract)
        cameras = json.loads(destination.with_suffix(".cameras.json").read_text(encoding="utf-8"))
        self.assertEqual(cameras["views"][0]["K"], contract["views"][0]["K"])
        np.testing.assert_array_equal(cameras["views"][0]["c2w_opencv"],
            [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]])
        self.assertEqual(cameras["clipping"], {"znear": .1, "zfar": 20., "backend_near": .2})
        self.assertEqual(cameras["model_sha256"], self.hash(destination))
        self.assertEqual(camera_scale_export.inspect_coordinate_metadata(destination)["current_unit"], "m")
        self.assertEqual((self.hash(source), self.hash(metadata)), original_hashes)

    def test_scene_similarity_is_baked_once_into_both_gaussian_models_and_cameras(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                source, _, contract = self.point_fixture()
                dimensions = 2 if backend == "2dgs" else 3
                names = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity"]
                names += ["scale_{}".format(i) for i in range(dimensions)]
                names += ["rot_{}".format(i) for i in range(4)]
                vertices = np.zeros(1, dtype=[(name, "f4") for name in names])
                vertices["x"], vertices["y"], vertices["z"] = 1., 2., 3.
                vertices["rot_0"] = 1.
                vertices["opacity"] = .7
                for i in range(dimensions):
                    vertices["scale_{}".format(i)] = -2. - i
                PlyData([PlyElement.describe(vertices, "vertex")]).write(source)
                contract["backend"] = backend
                contract["ply_sha256"] = self.hash(source)
                source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
                source_hash = self.hash(source)
                request = {"schema": "gsw.scene_transform/1", "frame": "training", "unit": "training",
                    "transform": {"s": 3., "R": [[0., 0., 1.], [0., 1., 0.], [-1., 0., 0.]], "t": [1., 2., 3.]}}
                edited = self.root / (backend + "-scene-metric.ply")
                report = camera_scale_export.export_camera_scale_ply(source, edited, mode="metric", scene_transform=request)
                points = PlyData.read(edited, mmap=False)["vertex"].data
                np.testing.assert_array_equal([points[name][0] for name in ("x", "y", "z")], [7., 0., -1.5])
                for i in range(dimensions):
                    self.assertAlmostEqual(float(points["scale_{}".format(i)][0]), -2. - i + np.log(1.5), places=6)
                self.assertEqual(report["training_contract"], contract)
                self.assertEqual(report["canonical_scene_transform"]["s"], 3.)
                np.testing.assert_array_equal(report["canonical_scene_transform"]["R"],
                    [[1., 0., 0.], [0., 0., -1.], [0., 1., 0.]])
                np.testing.assert_allclose(report["canonical_scene_transform"]["t"], [-5., 0., -15.], atol=1e-12, rtol=0)
                self.assertTrue(report["physical_scale_modified"])
                cameras = json.loads(edited.with_suffix(".cameras.json").read_text(encoding="utf-8"))
                np.testing.assert_allclose(cameras["views"][0]["c2w_opencv"],
                    [[1., 0., 0., -5.], [0., 0., -1., 0.], [0., 1., 0., -15.], [0., 0., 0., 1.]], atol=1e-12, rtol=0)
                self.assertEqual(set(cameras["clipping"]), {"znear", "zfar", "backend_near"})
                for name, expected in (("znear", .3), ("zfar", 60.), ("backend_near", .6)):
                    self.assertAlmostEqual(cameras["clipping"][name], expected, places=12)
                repeated = self.root / (backend + "-repeated.ply")
                again = camera_scale_export.export_camera_scale_ply(edited, repeated, mode="metric")
                repeated_points = PlyData.read(repeated, mmap=False)["vertex"].data
                for name in points.dtype.names:
                    np.testing.assert_array_equal(repeated_points[name], points[name])
                self.assertEqual(again["canonical_scene_transform"], report["canonical_scene_transform"])
                again_cameras = json.loads(repeated.with_suffix(".cameras.json").read_text(encoding="utf-8"))
                self.assertEqual(again_cameras["views"], cameras["views"])
                self.assertEqual(again["applied_transform"]["s"], 1.)
                self.assertEqual(self.hash(source), source_hash)

    def mesh_fixture(self):
        source, _, contract = self.point_fixture()
        vertices = np.zeros(5, dtype=[("x", "f4"), ("y", "f4"), ("z", "f4"),
            ("nx", "f4"), ("ny", "f4"), ("nz", "f4"), ("quality", "u2"), ("spectrum", "O")])
        vertices["x"], vertices["y"], vertices["z"] = [6, 10, 8, 12, 4], [-4, -2, -6, -8, 0], [9, 5, 7, 11, 1]
        vertices["ny"] = 1.
        vertices["quality"] = [101, 102, 103, 104, 105]
        vertices["spectrum"] = [np.array([i + .125, i + .875], dtype="f8") for i in range(5)]
        faces = np.zeros(3, dtype=[("vertex_indices", "O"), ("texcoord", "O"),
            ("texture_index", "u1"), ("nx", "f4"), ("ny", "f4"), ("nz", "f4"), ("classification", "u2")])
        faces["vertex_indices"] = [np.array([0, 1, 2, 3], dtype="i4"), np.array([0, 2, 4], dtype="i4"),
            np.array([4, 3, 2, 1, 0], dtype="i4")]
        faces["texcoord"] = [np.arange(size, dtype="f4") / 16. for size in (8, 6, 10)]
        faces["texture_index"] = [0, 1, 0]
        faces["ny"] = 1.
        faces["classification"] = [401, 402, 403]
        material = np.array([(17, .625), (29, .875)], dtype=[("material_id", "u2"), ("roughness", "f4")])
        elements = [PlyElement.describe(vertices, "vertex", val_types={"spectrum": "f8"}, comments=["vertex attributes"]),
            PlyElement.describe(faces, "face", val_types={"texcoord": "f4"}, comments=["polygons not triangles"]),
            PlyElement.describe(material, "material")]
        PlyData(elements, comments=["owned polygon mesh", "units training"], obj_info=["preserve this information"]).write(source)
        contract["ply_sha256"] = self.hash(source)
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        return source, vertices, faces, material, contract

    def test_mesh_restores_coordinates_without_triangulating_or_losing_typed_attributes(self):
        source, vertices, faces, material, contract = self.mesh_fixture()
        before = self.hash(source)
        output = self.root / "metric-mesh.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="mesh")
        result = PlyData.read(output, mmap=False)
        self.assertEqual(report["representation"], "mesh")
        self.assertEqual(report["face_count"], 3)
        self.assertEqual(report["training_contract"], contract)
        np.testing.assert_array_equal(np.column_stack([result["vertex"][name] for name in ("x", "y", "z")]),
            [[1, 2, 3], [2, 0, 1], [0, 1, 2], [-1, -1, 4], [3, 3, -1]])
        self.assertEqual(result.obj_info, ["preserve this information"])
        self.assertEqual(result["vertex"].comments, ["vertex attributes"])
        self.assertEqual(result["face"].comments, ["polygons not triangles"])
        self.assertEqual(result["vertex"].properties[-1].val_dtype, "f8")
        self.assertEqual(result["face"].properties[1].val_dtype, "f4")
        for index, expected in enumerate(faces["vertex_indices"]):
            np.testing.assert_array_equal(result["face"]["vertex_indices"][index], expected)
            np.testing.assert_array_equal(result["face"]["texcoord"][index], faces["texcoord"][index])
        for name in ("texture_index", "classification"):
            self.assertEqual(result["face"][name].tobytes(), faces[name].tobytes())
        for index in range(len(vertices)):
            np.testing.assert_array_equal(result["vertex"]["spectrum"][index], vertices["spectrum"][index])
        self.assertEqual(result["vertex"]["quality"].tobytes(), vertices["quality"].tobytes())
        np.testing.assert_array_equal(np.column_stack([result["face"][name] for name in ("nx", "ny", "nz")]), [[1, 0, 0]] * 3)
        self.assertEqual(result["material"].data.tobytes(), material.tobytes())
        self.assertEqual(camera_scale_export.inspect_coordinate_metadata(output)["current_unit"], "m")
        self.assertEqual(self.hash(source), before)

    def test_cli_explicit_representation_and_scene_request_use_the_shared_exporter(self):
        source, _, _ = self.point_fixture()
        request = self.root / "scene-request.json"
        request.write_text(json.dumps({"schema": "gsw.scene_transform/1", "frame": "training", "unit": "training",
            "transform": {"s": 1., "R": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "t": [2, 0, 0]}}), encoding="utf-8")
        output = self.root / "cli-points.ply"
        command = [sys.executable, "-B", "-m", "native.backend.camera_scale_export", "--input", str(source),
            "--output", str(output), "--coordinates", "metric", "--representation", "point_cloud",
            "--scene-transform", str(request), "--progress-json"]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertTrue(any(line.startswith("[gsw-coordinate-export-progress] ") for line in lines))
        report = json.loads(lines[-1])
        self.assertTrue(report["success"])
        self.assertEqual(report["representation"], "point_cloud")
        data = PlyData.read(output, mmap=False)["vertex"].data
        np.testing.assert_array_equal([data[name][0] for name in ("x", "y", "z")], [1., 1., 3.])
        default_command = command[:command.index("--representation")] + ["--progress-json"]
        default_command[default_command.index("--output") + 1] = str(self.root / "cli-default.ply")
        default = subprocess.run(default_command,
            capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(default.returncode, 2)
        self.assertEqual(json.loads(default.stdout.splitlines()[-1])["code"], "camera_scale_export_requires_supported_gaussian_ply")

    def test_mesh_crop_remaps_surviving_polygons_and_rejects_unknown_relational_indices(self):
        source, vertices, faces, _, _ = self.mesh_fixture()
        source_hash = self.hash(source)
        output = self.root / "cropped.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="mesh",
            deleted_indices=[1, 3], deleted_vertex_count=5)
        result = PlyData.read(output, mmap=False)
        self.assertEqual(report["vertex_count"], 3)
        self.assertEqual(report["face_count"], 1)
        self.assertEqual(report["deleted_faces"], 2)
        np.testing.assert_array_equal(result["face"]["vertex_indices"][0], [0, 1, 2])
        np.testing.assert_array_equal(result["face"]["texcoord"][0], faces["texcoord"][1])
        self.assertEqual(result["face"]["classification"].tolist(), [402])
        self.assertEqual(result["vertex"]["quality"].tolist(), [101, 103, 105])
        self.assertNotEqual(report["output_sha256"], source_hash)
        self.assertEqual(report["export_operations"][-1]["deleted_faces"], 2)
        self.assertEqual(camera_scale_export.inspect_coordinate_metadata(output)["model_sha256"], self.hash(output))
        self.assertEqual(self.hash(source), source_hash)
        relational = self.root / "relational.ply"
        original = PlyData.read(source, mmap=False)
        edge = np.array([(0, 2), (1, 4)], dtype=[("vertex1", "i4"), ("vertex2", "i4")])
        PlyData(list(original.elements) + [PlyElement.describe(edge, "edge")]).write(relational)
        contract = json.loads(source.with_suffix(".camera-scale.json").read_text(encoding="utf-8"))
        contract["ply_sha256"] = self.hash(relational)
        relational.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        rejected = self.root / "unsafe-remap.ply"
        with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
            camera_scale_export.export_recorded_ply(relational, rejected, mode="preserve", representation="mesh", deleted_indices=[1])
        self.assertEqual(caught.exception.code, "camera_scale_export_unsupported_index_relation")
        self.assertFalse(rejected.exists())

    def test_textured_mesh_copies_and_binds_assets_and_reexport_remains_usable(self):
        source, _, _, _, contract = self.mesh_fixture()
        texture_root = self.root / "textures"
        texture_root.mkdir()
        textures = [texture_root / "first.png", texture_root / "second.jpg"]
        payloads = [b"owned-opaque-texture-bytes-A\x00\xff", b"owned-opaque-texture-bytes-B\x00\xfe"]
        for path, payload in zip(textures, payloads):
            path.write_bytes(payload)
        original = PlyData.read(source, mmap=False)
        original.comments = list(original.comments) + ["TextureFile textures/first.png", "TextureFile textures/second.jpg"]
        original.write(source)
        contract["ply_sha256"] = self.hash(source)
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        before = [self.hash(path) for path in [source] + textures]
        destination = self.root / "elsewhere"
        destination.mkdir()
        output = destination / "mesh.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="mesh")
        self.assertEqual(len(report["assets"]), 2)
        header = PlyData.read(output, mmap=False)
        references = [comment[12:] for comment in header.comments if comment.startswith("TextureFile ")]
        self.assertEqual(references, [asset["path"] for asset in report["assets"]])
        self.assertEqual(header["face"]["texture_index"].tolist(), [0, 1, 0])
        for index, asset in enumerate(report["assets"]):
            self.assertEqual(set(asset), {"kind", "path", "sha256", "bytes", "source_reference"})
            self.assertEqual(asset["kind"], "texture_file")
            self.assertEqual(asset["source_reference"], "textures/" + textures[index].name)
            self.assertEqual((destination / asset["path"]).read_bytes(), payloads[index])
            self.assertEqual(asset["sha256"], hashlib.sha256(payloads[index]).hexdigest())
            self.assertEqual(asset["bytes"], len(payloads[index]))
        manifest_hash = hashlib.sha256(json.dumps(report["assets"], sort_keys=True, ensure_ascii=False,
            separators=(",", ":")).encode("utf-8")).hexdigest()
        self.assertEqual(report["assets_sha256"], manifest_hash)
        expected_bundle = {"model_sha256": self.hash(output), "camera_sha256": self.hash(output.with_suffix(".cameras.json")),
            "input_data_identity": contract["input_data_identity"], "coordinate_frame": "canonical", "coordinate_unit": "m",
            "assets_sha256": manifest_hash}
        self.assertEqual(report["bundle_sha256"], hashlib.sha256(json.dumps(expected_bundle,
            sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("utf-8")).hexdigest())
        repeated = destination / "repeat.ply"
        second = camera_scale_export.export_recorded_ply(output, repeated, mode="preserve", representation="mesh")
        self.assertEqual(len(second["assets"]), 2)
        self.assertEqual(camera_scale_export.inspect_coordinate_metadata(repeated)["current_unit"], "m")
        self.assertEqual([self.hash(path) for path in [source] + textures], before)
        (destination / second["assets"][0]["path"]).write_bytes(b"changed-output-asset")
        with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
            camera_scale_export.inspect_coordinate_metadata(repeated)
        self.assertEqual(caught.exception.code, "camera_scale_export_asset_hash_mismatch")

    def test_textured_export_cancellation_and_publication_conflict_preserve_other_writers(self):
        source, _, _, _, contract = self.mesh_fixture()
        texture = self.root / "original.png"
        texture.write_bytes(b"owned pixels not decoded by export")
        original = PlyData.read(source, mmap=False)
        original.comments = ["TextureFile original.png"]
        original["face"]["texture_index"] = 0
        original.write(source)
        contract["ply_sha256"] = self.hash(source)
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        source_hash = self.hash(source)
        for scenario in ("cancel", "metadata-race", "directory-race", "source-change"):
            with self.subTest(scenario=scenario):
                folder = self.root / scenario
                folder.mkdir()
                output = folder / "result.ply"
                cancel = []
                def progress(value):
                    if value != 95:
                        return
                    if scenario == "cancel":
                        cancel.append(True)
                    elif scenario == "metadata-race":
                        output.with_suffix(".camera-scale.json").write_bytes(b"other writer metadata")
                    elif scenario == "directory-race":
                        resources = folder / "result.assets"
                        resources.mkdir()
                        (resources / "other.txt").write_bytes(b"other writer resource")
                    else:
                        texture.write_bytes(b"changed during export")
                with self.assertRaises((camera_scale_export.CameraScaleExportError, FileExistsError)) as caught:
                    camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="mesh",
                        progress=progress, cancelled=lambda: bool(cancel))
                if scenario in ("cancel", "source-change"):
                    self.assertEqual(caught.exception.code,
                        "camera_scale_export_cancelled" if scenario == "cancel" else "camera_scale_export_source_changed")
                    self.assertEqual(list(folder.iterdir()), [])
                elif scenario == "metadata-race":
                    self.assertEqual(output.with_suffix(".camera-scale.json").read_bytes(), b"other writer metadata")
                    self.assertEqual(sorted(path.name for path in folder.iterdir()), ["result.camera-scale.json"])
                else:
                    self.assertEqual((folder / "result.assets/other.txt").read_bytes(), b"other writer resource")
                    self.assertEqual([path.name for path in folder.iterdir()], ["result.assets"])
                self.assertFalse(output.exists())
                self.assertFalse(output.with_suffix(".cameras.json").exists())
                self.assertEqual(self.hash(source), source_hash)

    def test_unicode_model_and_parent_paths_use_safe_owned_resource_names(self):
        source, _, _, _, contract = self.mesh_fixture()
        unicode_parent = self.root / "資料 原模型"
        unicode_parent.mkdir()
        relocated = unicode_parent / "入力.ply"
        texture = unicode_parent / "texture.png"
        texture.write_bytes(b"same original opaque image bytes")
        original = PlyData.read(source, mmap=False)
        original.comments = ["TextureFile texture.png"]
        original["face"]["texture_index"] = 0
        original.write(relocated)
        contract["ply_sha256"] = self.hash(relocated)
        relocated.with_suffix(".camera-scale.json").write_text(json.dumps(contract, ensure_ascii=False), encoding="utf-8")
        folder = self.root / "出力 モデル"
        folder.mkdir()
        output = folder / "模型 出力.ply"
        report = camera_scale_export.export_recorded_ply(relocated, output, mode="metric", representation="mesh")
        reference = report["assets"][0]["path"]
        self.assertTrue(reference.isascii())
        self.assertTrue(reference.startswith("model-"))
        self.assertEqual((folder / reference).read_bytes(), texture.read_bytes())
        self.assertEqual(camera_scale_export.inspect_coordinate_metadata(output)["current_unit"], "m")

    def test_explicit_point_attributes_are_not_interpreted_as_gaussian_parameters(self):
        source, original, contract = self.point_fixture()
        dtype = original.dtype.descr + [("scale_0", "f4"), ("rot_0", "i4"), ("opacity", "u1")]
        vertices = np.zeros(len(original), dtype=dtype)
        for name in original.dtype.names:
            vertices[name] = original[name]
        vertices["scale_0"], vertices["rot_0"], vertices["opacity"] = .125, 91, 53
        PlyData([PlyElement.describe(vertices, "vertex")]).write(source)
        contract["ply_sha256"] = self.hash(source)
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        output = self.root / "explicit-points.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="point_cloud")
        result = PlyData.read(output, mmap=False)["vertex"].data
        for name in ("scale_0", "rot_0", "opacity"):
            self.assertEqual(result[name].dtype, vertices[name].dtype)
            self.assertEqual(result[name].tobytes(), vertices[name].tobytes())
        self.assertEqual(report["representation"], "point_cloud")
        with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
            camera_scale_export.export_recorded_ply(source, self.root / "ambiguous.ply", mode="metric")
        self.assertEqual(caught.exception.code, "camera_scale_export_requires_supported_gaussian_ply")

    def test_unsafe_texture_references_are_rejected_without_publishing(self):
        source, _, _, _, contract = self.mesh_fixture()
        original = PlyData.read(source, mmap=False)
        for index, reference in enumerate(("../outside.png", "C:/absolute.png", "https://invalid/image.png", "a\\b.png", "NUL")):
            with self.subTest(reference=reference):
                original.comments = ["TextureFile " + reference]
                original.write(source)
                contract["ply_sha256"] = self.hash(source)
                source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
                output = self.root / ("unsafe-{}.ply".format(index))
                with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
                    camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="mesh")
                self.assertEqual(caught.exception.code, "camera_scale_export_unsafe_asset_reference")
                self.assertFalse(output.exists())

    def test_scene_request_rejects_wrong_frames_units_and_nonproper_similarities(self):
        source, _, _ = self.point_fixture()
        base = {"schema": "gsw.scene_transform/1", "frame": "training", "unit": "training",
            "transform": {"s": 1., "R": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "t": [0, 0, 0]}}
        requests = []
        for field, value in (("frame", "canonical"), ("unit", "m")):
            request = json.loads(json.dumps(base))
            request[field] = value
            requests.append(request)
        for scale in (0., -1., float("nan"), True):
            request = json.loads(json.dumps(base))
            request["transform"]["s"] = scale
            requests.append(request)
        for rotation in ([[[-1, 0, 0], [0, 1, 0], [0, 0, 1]]], [[[2, 0, 0], [0, 1, 0], [0, 0, 1]]]):
            request = json.loads(json.dumps(base))
            request["transform"]["R"] = rotation[0]
            requests.append(request)
        for index, request in enumerate(requests):
            with self.subTest(index=index):
                output = self.root / ("invalid-scene-{}.ply".format(index))
                with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
                    camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="point_cloud", scene_transform=request)
                self.assertEqual(caught.exception.code, "camera_scale_export_invalid_scene_transform")
                self.assertFalse(output.exists())

    def test_v3_validation_checks_active_edit_history_and_camera_semantics_not_hashes_alone(self):
        source, _, _ = self.point_fixture()
        output = self.root / "edited-bound.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="point_cloud",
            scene_transform={"schema": "gsw.scene_transform/1", "frame": "training", "unit": "training",
                "transform": {"s": 1., "R": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "t": [1, 0, 0]}})
        sidecar = output.with_suffix(".camera-scale.json")
        saved = sidecar.read_bytes()
        corrupt = json.loads(saved)
        corrupt["canonical_scene_transform"]["t"][0] += 1.
        sidecar.write_text(json.dumps(corrupt), encoding="utf-8")
        with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
            camera_scale_export.inspect_coordinate_metadata(output)
        self.assertEqual(caught.exception.code, "camera_scale_export_invalid_scene_history")
        sidecar.write_bytes(saved)
        cameras_path = output.with_suffix(".cameras.json")
        cameras = json.loads(cameras_path.read_text(encoding="utf-8"))
        cameras["views"][0]["c2w_opencv"][0][3] += 1.
        cameras_path.write_text(json.dumps(cameras), encoding="utf-8")
        report["camera_reference"]["sha256"] = self.hash(cameras_path)
        payload = {"model_sha256": report["output_sha256"], "camera_sha256": self.hash(cameras_path),
            "input_data_identity": report["input_data_identity"], "coordinate_frame": report["output_frame"],
            "coordinate_unit": report["output_unit"], "assets_sha256": report["assets_sha256"]}
        report["bundle_sha256"] = hashlib.sha256(json.dumps(payload, sort_keys=True, ensure_ascii=True,
            separators=(",", ":")).encode("utf-8")).hexdigest()
        sidecar.write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
            camera_scale_export.inspect_coordinate_metadata(output)
        self.assertEqual(caught.exception.code, "camera_scale_export_camera_binding_mismatch")

    def test_points_preserve_unknown_units_and_known_unit_reexports_do_not_double_scale(self):
        source, _, contract = self.point_fixture()
        centimetres = self.root / "centimetres.ply"
        first = camera_scale_export.export_recorded_ply(source, centimetres, mode="metric", output_unit="cm", representation="point_cloud")
        points = PlyData.read(centimetres, mmap=False)["vertex"].data
        np.testing.assert_array_equal([points[name][0] for name in ("x", "y", "z")], [100, 200, 300])
        metres = self.root / "metres.ply"
        second = camera_scale_export.export_recorded_ply(centimetres, metres, mode="metric", representation="point_cloud")
        points = PlyData.read(metres, mmap=False)["vertex"].data
        np.testing.assert_array_equal([points[name][0] for name in ("x", "y", "z")], [1, 2, 3])
        self.assertEqual(second["canonical_scene_transform"], first["canonical_scene_transform"])
        self.assertEqual(second["training_contract"], contract)
        contract["scale"] = {"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None,
            "canonical_unit": "arbitrary", "evidence": []}
        contract["transforms"]["source_to_canonical"]["s"] = 1.
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        arbitrary = self.root / "arbitrary-source.ply"
        report = camera_scale_export.export_recorded_ply(source, arbitrary, mode="source", representation="point_cloud")
        self.assertEqual(report["output_unit"], "unknown")
        self.assertFalse(camera_scale_export.inspect_coordinate_metadata(arbitrary)["metric_available"])
        for mode, unit in (("metric", None), ("preserve", "cm")):
            with self.subTest(mode=mode):
                with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
                    camera_scale_export.export_recorded_ply(arbitrary, self.root / (mode + "-refused.ply"),
                        mode=mode, output_unit=unit, representation="point_cloud")
                self.assertEqual(caught.exception.code, "camera_scale_export_unknown_metric_scale")

    def test_mesh_invalid_index_types_ranges_and_unadapted_materials_are_explicit_failures(self):
        source, _, _, _, contract = self.mesh_fixture()
        original = PlyData.read(source, mmap=False)
        for index, indices in enumerate((np.array([-1, 1, 2], dtype="i4"), np.array([0, 1, 5], dtype="i4"),
                                         np.array([0, 1, 2], dtype="f4"))):
            with self.subTest(index=index):
                faces = np.empty(1, dtype=[("vertex_indices", "O")])
                faces["vertex_indices"][0] = indices
                invalid = self.root / ("invalid-mesh-{}.ply".format(index))
                PlyData([original["vertex"], PlyElement.describe(faces, "face", val_types={"vertex_indices": indices.dtype.str[1:]})]).write(invalid)
                contract["ply_sha256"] = self.hash(invalid)
                invalid.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
                with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
                    camera_scale_export.export_recorded_ply(invalid, self.root / ("not-published-{}.ply".format(index)),
                        mode="metric", representation="mesh")
                self.assertEqual(caught.exception.code, "camera_scale_export_invalid_mesh_indices")
        original.comments = ["MaterialFile external-material.mtl"]
        original.write(source)
        contract["ply_sha256"] = self.hash(source)
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
            camera_scale_export.export_recorded_ply(source, self.root / "material-not-guessed.ply", mode="metric", representation="mesh")
        self.assertEqual(caught.exception.code, "camera_scale_export_unsupported_material_reference")

    def test_texture_comments_at_element_header_positions_keep_order_and_are_rewritten_in_place(self):
        source, _, _, _, contract = self.mesh_fixture()
        (self.root / "first.png").write_bytes(b"first header-position resource")
        (self.root / "second.png").write_bytes(b"second header-position resource")
        original = PlyData.read(source, mmap=False)
        original["vertex"].comments = ["vertex before texture", "TextureFile first.png", "vertex after texture"]
        original["face"].comments = ["face before texture", "TextureFile second.png", "face after texture"]
        original.write(source)
        contract["ply_sha256"] = self.hash(source)
        source.with_suffix(".camera-scale.json").write_text(json.dumps(contract), encoding="utf-8")
        output = self.root / "element-materials.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="mesh")
        self.assertEqual([asset["source_reference"] for asset in report["assets"]], ["first.png", "second.png"])
        result = PlyData.read(output, mmap=False)
        self.assertEqual(result["vertex"].comments, ["vertex before texture", "TextureFile " + report["assets"][0]["path"], "vertex after texture"])
        self.assertEqual(result["face"].comments, ["face before texture", "TextureFile " + report["assets"][1]["path"], "face after texture"])
        self.assertEqual(result["face"]["texture_index"].tolist(), [0, 1, 0])
        for asset, payload in zip(report["assets"], (b"first header-position resource", b"second header-position resource")):
            self.assertEqual((output.parent / asset["path"]).read_bytes(), payload)
        self.assertEqual(camera_scale_export.inspect_coordinate_metadata(output)["model_sha256"], self.hash(output))

    def test_public_depth_extraction_keeps_first_geometry_semantics_through_metric_unit_and_repeat_exports(self):
        from native.backend.tests.test_gs_geometry import GsGeometrySourceTests
        from native.worker.gs_geometry import extract_geometry
        from native.backend import camera_dataset
        class CpuDepthRenderer:
            identity = {"kind": "owned_CPU_depth_seam_not_actual_GPU_accuracy"}
            def render(self, view):
                width, height = view["training_size"]
                return {"alpha": np.full((height, width), .75, dtype=np.float32),
                        "camera_z_moment": np.full((height, width), 2., dtype=np.float32)}
            def close(self):
                pass
        for backend, definition in (("3dgs", "expected_camera_z_gaussian_center_attribute_alpha_compositing"),
                                    ("2dgs", "expected_camera_z_surfel_ray_intersection_alpha_compositing")):
            with self.subTest(backend=backend):
                fixture = GsGeometrySourceTests()
                with mock.patch.dict(os.environ, {"GSW_GEOMETRY_TEST_ROOT": str(self.root)}):
                    fixture.setUp()
                self.addCleanup(fixture.doCleanups)
                if backend == "2dgs":
                    original = PlyData.read(fixture.ply, mmap=False)["vertex"].data
                    names = [name for name in original.dtype.names if name != "scale_2"]
                    surfels = np.empty(len(original), dtype=[(name, original.dtype[name]) for name in names])
                    for name in names:
                        surfels[name] = original[name]
                    PlyData([PlyElement.describe(surfels, "vertex")]).write(fixture.ply)
                    fixture.contract["backend"] = backend
                    fixture.rewrite_contract()
                    camera_dataset.write_camera_dataset({"backend": backend}, fixture.model / "training_backend.json")
                before = {path: self.hash(path) for root in (fixture.model, fixture.dataset) for path in root.rglob("*") if path.is_file()}
                extracted = extract_geometry({"schema": "gsw.gs_geometry_request/1", "model_directory": str(fixture.model),
                    "dataset_path": str(fixture.dataset), "iteration": 7,
                    "output_directory": str(self.root / (backend + "-depth")), "coordinate_mode": "training",
                    "alpha_threshold": .5, "depth_range": {"min": .2, "max": 10., "frame": "training"},
                    "pixel_stride": 1, "max_points": 1000, "memory_budget_mb": 8},
                    self.root / (backend + "-depth-report.json"), renderer_factory=lambda source: CpuDepthRenderer())
                self.assertTrue(extracted["success"])
                source = Path(extracted["geometryPath"])
                metadata = source.with_suffix(".camera-scale.json")
                original_record = json.loads(metadata.read_text(encoding="utf-8"))
                expected = {"schema": "gsw.geometry_source_provenance/1", "source_model_sha256": self.hash(source),
                    "source_metadata_sha256": self.hash(metadata), "source_frame": "training", "source_unit": "training",
                    "geometry_kind": "depth_point_cloud", "depth_definition": definition, "fusion": "none",
                    "surface_semantics": "appearance_proxy_not_material_boundary", "model_geometry_accuracy": "not_measured",
                    "source_applied_transform": original_record["applied_transform"],
                    "depth_definition_scope": "extraction_camera_depth_not_current_file_axes_or_units"}
                previous = source
                first_provenance = None
                for index, (mode, unit) in enumerate((("metric", None), ("metric", "cm"), ("source", None), ("preserve", None))):
                    destination = self.root / (backend + "-geometry-{}.ply".format(index))
                    report = camera_scale_export.export_recorded_ply(previous, destination, mode=mode, output_unit=unit, representation="point_cloud")
                    self.assertEqual(report["source_geometry_provenance"], expected)
                    self.assertFalse(report["surface_accuracy_certified"])
                    self.assertEqual(report["training_contract"], fixture.contract)
                    camera_scale_export.inspect_coordinate_metadata(destination)
                    previous = destination
                    first_provenance = report["source_geometry_provenance"] if first_provenance is None else first_provenance
                self.assertEqual(first_provenance, expected)
                self.assertEqual({path: self.hash(path) for path in before}, before)

    def test_malformed_geometry_provenance_is_rejected_before_reexport(self):
        source, _, contract = self.point_fixture()
        metadata = source.with_suffix(".camera-scale.json")
        value = {"schema": "gsw.coordinate_export/1", "output_frame": "training", "output_unit": "training",
            "ply_sha256": self.hash(source), "training_contract": contract,
            "applied_transform": {"s": 1., "R": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "t": [0, 0, 0],
                "input_frame": "training", "output_frame": "training", "applied": True},
            "geometry_kind": "depth_point_cloud", "depth_definition": "owned_CPU_camera_Z_fixture",
            "fusion": "none", "surface_semantics": "appearance_proxy_not_material_boundary", "model_geometry_accuracy": "not_measured"}
        metadata.write_text(json.dumps(value), encoding="utf-8")
        output = self.root / "bound-geometry.ply"
        report = camera_scale_export.export_recorded_ply(source, output, mode="metric", representation="point_cloud")
        sidecar = output.with_suffix(".camera-scale.json")
        for index, (key, replacement) in enumerate((("source_model_sha256", "a" * 63), ("source_metadata_sha256", "INVALID"),
                ("source_frame", "undefined"), ("source_unit", "unknown"), ("geometry_kind", False),
                ("depth_definition_scope", "current_file_units"), ("source_applied_transform", {"s": -1, "R": [], "t": []}))):
            with self.subTest(field=key):
                changed = json.loads(json.dumps(report))
                changed["source_geometry_provenance"][key] = replacement
                sidecar.write_text(json.dumps(changed), encoding="utf-8")
                target = self.root / ("malformed-output-{}.ply".format(index))
                with self.assertRaises(camera_scale_export.CameraScaleExportError) as caught:
                    camera_scale_export.export_recorded_ply(output, target, mode="preserve", representation="point_cloud")
                self.assertEqual(caught.exception.code, "camera_scale_export_invalid_geometry_provenance")
                self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
