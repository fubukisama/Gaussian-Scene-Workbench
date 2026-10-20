"""Existing-model scale workflows through real PLY files and the public CLI."""

import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image
from plyfile import PlyData, PlyElement


class ModelScaleVersionTests(unittest.TestCase):
    def test_metric_output_removes_obsolete_crs_but_retains_source_declaration(self):
        ply = PlyData.read(str(self.model), mmap=False)
        ply.comments = ["CRS: EPSG:32654", "units mm", "retained survey note"]
        ply.write(str(self.model))
        before = self.model.read_bytes()
        code, inspection, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        code, result, log = self.cli("create-version", identity=inspection["inputDataIdentity"],
                                     destination=self.root / "local-metric")
        self.assertEqual(code, 0, log)
        output = Path(result["modelPath"])
        comments = PlyData.read(str(output)).comments
        self.assertFalse(any("EPSG" in value for value in comments))
        self.assertIn("retained survey note", comments)
        sidecar = json.loads(output.with_suffix(".camera-scale.json").read_text(encoding="utf-8"))
        self.assertEqual(sidecar["input_declared_crs"], "EPSG:32654")
        self.assertNotIn("output_crs", sidecar)
        self.assertEqual(self.model.read_bytes(), before)

    def setUp(self):
        base = Path(os.environ.get("GSW_CAMERA_P1_TEST_ROOT", tempfile.gettempdir())) / "gsw-model-scale-p1"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repository = Path(__file__).resolve().parents[3]
        vertices = np.zeros(3, dtype=[("x", "f8"), ("y", "f8"), ("z", "f8"), ("red", "u1")])
        vertices["x"], vertices["y"], vertices["z"] = [1., 2., 1.], [2., 2., 3.], [3., 3., 3.]
        vertices["red"] = [10, 20, 30]
        self.model = self.root / "original model.ply"
        PlyData([PlyElement.describe(vertices, "vertex")]).write(self.model)

    def cli(self, operation, model=None, identity=None, destination=None, cancel=False, request=None):
        report = self.root / (operation + "-" + str(len(list(self.root.glob("*.json")))) + ".json")
        command = [sys.executable, "-B", str(self.repository / "native/worker/scale_constraints.py"),
                   "--operation", operation, "--model", str(model or self.model), "--report", str(report)]
        if operation != "inspect":
            request = request or {"schema": "gsw.scale_constraints/1", "input_data_identity": identity,
                       "input_frame": "source", "mode": "scale_only", "fit_tolerance_m": 1e-8,
                       "constraints": [{"id": "model-length", "kind": "distance", "role": "fit",
                           "source_a": [0., 0., 0.], "source_b": [2., 0., 0.], "reference_distance": 6.,
                           "reference_unit": "m", "reference_id": "ruler-A", "provenance": {"source": "generated"}}]}
            request_path = self.root / (report.stem + "-request.json")
            request_path.write_text(json.dumps(request), encoding="utf-8")
            command.extend(["--request", str(request_path), "--expected-identity", identity])
        if destination is not None:
            command.extend(["--output-directory", str(destination)])
        completed = subprocess.run(command, cwd=str(self.root), input="cancel\n" if cancel else "",
            capture_output=True, text=True, encoding="utf-8", timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        lines = completed.stdout.splitlines()
        output = json.loads(lines[-1]) if lines else {}
        return completed.returncode, output, completed.stdout + completed.stderr

    def alignment_request(self, identity):
        rows = []
        for index, source in enumerate(([0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [0., 0., 1.])):
            rows.append({"id": "control-" + str(index), "kind": "control", "role": "fit",
                "source_xyz": source, "reference_xyz": [10. - 2. * source[1], 20. + 2. * source[0], 30. + 2. * source[2]],
                "reference_unit": "m", "reference_id": "control-ref-" + str(index),
                "provenance": {"source": "generated"}})
        return {"schema": "gsw.scale_constraints/1", "input_data_identity": identity,
                "input_frame": "source", "mode": "sim3", "fit_tolerance_m": 1e-8, "constraints": rows}

    def gaussian(self, dimensions=3, sh=False):
        names = ["x", "y", "z", "opacity", "f_dc_0", "f_dc_1", "f_dc_2", "nx", "ny", "nz"]
        names += ["scale_" + str(index) for index in range(dimensions)]
        names += ["rot_" + str(index) for index in range(4)] + ["cov_" + str(index) for index in range(6)]
        if sh:
            names += ["f_rest_" + str(index) for index in range(9)]
        vertices = np.zeros(1, dtype=[(name, "f4") for name in names])
        for name, value in zip(("x", "y", "z"), (1., 2., 3.)):
            vertices[name] = value
        vertices["opacity"], vertices["rot_0"], vertices["nx"] = .7, 1., 1.
        vertices["f_dc_0"] = .3
        for index in range(dimensions):
            vertices["scale_" + str(index)] = -2. - index
        for index, value in enumerate((1., 0., 0., 4., 0., 9.)):
            vertices["cov_" + str(index)] = value
        if sh:
            vertices["f_rest_0"] = .123
        path = self.root / ("gaussian-" + str(dimensions) + ("-sh" if sh else "") + ".ply")
        PlyData([PlyElement.describe(vertices, "vertex")]).write(path)
        return path, vertices

    def test_model_only_inspection_and_scale_version_need_no_fake_cameras_and_preserve_input(self):
        before = self.model.read_bytes()
        code, inspection, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        envelope = json.loads(Path(inspection["reportPath"]).read_text(encoding="utf-8"))
        self.assertEqual(envelope["schema"], "gsw.scale_input/1")
        self.assertEqual(envelope["kind"], "model")
        self.assertEqual(envelope["views"], [])
        self.assertEqual(envelope["data_identity"], inspection["inputDataIdentity"])
        self.assertEqual(envelope["model"]["sha256"], hashlib.sha256(before).hexdigest())
        self.assertNotIn("splits", envelope)
        destination = self.root / "new-model-version"
        code, result, log = self.cli("create-version", identity=envelope["data_identity"], destination=destination)
        self.assertEqual(code, 0, log)
        self.assertEqual(result["summary"]["representation"], "baked_model")
        output = Path(result["modelPath"])
        data = PlyData.read(output)["vertex"].data
        np.testing.assert_array_equal(data["x"], [3., 6., 3.])
        np.testing.assert_array_equal(data["y"], [6., 6., 9.])
        np.testing.assert_array_equal(data["z"], [9., 9., 9.])
        np.testing.assert_array_equal(data["red"], [10, 20, 30])
        sidecar = json.loads(output.with_suffix(".camera-scale.json").read_text(encoding="utf-8"))
        self.assertEqual(sidecar["schema"], "gsw.model_scale_export/1")
        self.assertEqual(sidecar["coordinate_frame"], "metric")
        self.assertEqual(sidecar["model_geometry_accuracy"], "not_measured")
        self.assertEqual(sidecar["ply_sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
        self.assertEqual(sidecar["scale"]["status"], "constraint_derived")
        self.assertEqual(sidecar["input_model"]["sha256"], hashlib.sha256(before).hexdigest())
        self.assertEqual(self.model.read_bytes(), before)
        self.assertFalse(os.path.samefile(self.model, destination / "source/original.ply"))

    def test_both_gaussian_dimensions_rotate_scale_covariance_and_normals_with_wxyz_quaternions(self):
        for dimensions in (2, 3):
            with self.subTest(dimensions=dimensions):
                model, original = self.gaussian(dimensions)
                code, inspected, log = self.cli("inspect", model)
                self.assertEqual(code, 0, log)
                identity = inspected["inputDataIdentity"]
                code, result, log = self.cli("create-version", model, identity, self.root / ("rotated-" + str(dimensions)),
                                             request=self.alignment_request(identity))
                self.assertEqual(code, 0, log)
                vertices = PlyData.read(result["modelPath"])["vertex"].data
                np.testing.assert_allclose([vertices[name][0] for name in ("x", "y", "z")], [6., 22., 36.], atol=2e-13)
                np.testing.assert_allclose([vertices["rot_" + str(index)][0] for index in range(4)],
                                           [math.sqrt(.5), 0., 0., math.sqrt(.5)], atol=1e-7)
                np.testing.assert_allclose([vertices["cov_" + str(index)][0] for index in range(6)],
                                           [16., 0., 0., 4., 0., 36.], atol=2e-13)
                np.testing.assert_allclose([vertices[name][0] for name in ("nx", "ny", "nz")], [0., 1., 0.], atol=1e-14)
                for index in range(dimensions):
                    self.assertAlmostEqual(float(vertices["scale_" + str(index)][0]), -2. - index + math.log(2.), places=6)
                for name in ("opacity", "f_dc_0", "f_dc_1", "f_dc_2"):
                    self.assertEqual(vertices[name].tobytes(), original[name].tobytes())

    def test_directional_sh_rotation_is_an_explicit_unbaked_manifest_not_a_model(self):
        model, _ = self.gaussian(sh=True)
        before = model.read_bytes()
        code, inspected, log = self.cli("inspect", model)
        self.assertEqual(code, 0, log)
        identity = inspected["inputDataIdentity"]
        destination = self.root / "unbaked-rotation"
        code, result, log = self.cli("create-version", model, identity, destination, request=self.alignment_request(identity))
        self.assertEqual(code, 0, log)
        self.assertEqual(result["summary"]["representation"], "transform_manifest")
        self.assertEqual(result["modelPath"], "")
        self.assertFalse((destination / "scaled.ply").exists())
        transform = json.loads((destination / "transform.json").read_text(encoding="utf-8"))
        self.assertFalse(transform["transform"]["applied"])
        self.assertEqual(transform["reason"], "directional_sh_rotation_unsupported")
        self.assertEqual((destination / "source/original.ply").read_bytes(), before)
        self.assertEqual(model.read_bytes(), before)

    def test_mesh_faces_uv_and_owned_texture_survive_while_asset_bytes_bind_identity(self):
        vertices = PlyData.read(self.model)["vertex"].data.copy()
        faces = np.empty(1, dtype=[("vertex_indices", "O"), ("texcoord", "O"), ("material", "u1")])
        faces["vertex_indices"][0] = np.array([0, 1, 2], dtype="i4")
        faces["texcoord"][0] = np.array([0., 0., 1., 0., 0., 1.], dtype="f4")
        faces["material"] = 7
        texture = self.root / "nested/texture image.png"
        texture.parent.mkdir()
        Image.new("RGB", (4, 4), (120, 30, 50)).save(texture)
        texture_bytes = texture.read_bytes()
        PlyData([PlyElement.describe(vertices, "vertex"), PlyElement.describe(faces, "face")],
                comments=['TextureFile "nested/texture image.png"']).write(self.model)
        code, inspected, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        identity = inspected["inputDataIdentity"]
        relocated = self.root / "relocated"
        (relocated / "nested").mkdir(parents=True)
        shutil.copyfile(self.model, relocated / "renamed.ply")
        shutil.copyfile(texture, relocated / "nested/texture image.png")
        code, moved, log = self.cli("inspect", relocated / "renamed.ply")
        self.assertEqual(code, 0, log)
        self.assertEqual(moved["inputDataIdentity"], identity)
        destination = self.root / "mesh-version"
        code, result, log = self.cli("create-version", identity=identity, destination=destination)
        self.assertEqual(code, 0, log)
        output = PlyData.read(result["modelPath"])
        np.testing.assert_array_equal(output["face"].data["vertex_indices"][0], [0, 1, 2])
        np.testing.assert_array_equal(output["face"].data["texcoord"][0], faces["texcoord"][0])
        np.testing.assert_array_equal(output["face"].data["material"], [7])
        comment = next(value for value in output.comments if value.startswith("TextureFile "))
        owned_texture = destination / comment[len("TextureFile "):]
        self.assertEqual(owned_texture.read_bytes(), texture_bytes)
        self.assertFalse(os.path.samefile(owned_texture, texture))
        self.assertEqual((destination / "source/nested/texture image.png").read_bytes(), texture_bytes)
        Image.new("RGB", (4, 4), (1, 2, 3)).save(texture)
        code, changed, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        self.assertNotEqual(changed["inputDataIdentity"], identity)
        code, error, log = self.cli("create-version", identity=identity, destination=self.root / "stale-texture")
        self.assertEqual(code, 2, log)
        self.assertEqual(error["code"], "model_scale_input_identity_mismatch")
        self.assertFalse((self.root / "stale-texture").exists())

    def test_large_origin_keeps_submillimetre_detail_in_double_output(self):
        vertices = PlyData.read(self.model)["vertex"].data.copy()
        vertices["x"] = [1_000_000.001, 1_000_000.002, 1_000_000.003]
        PlyData([PlyElement.describe(vertices, "vertex")]).write(self.model)
        before = self.model.read_bytes()
        code, inspected, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        code, result, log = self.cli("create-version", identity=inspected["inputDataIdentity"],
                                     destination=self.root / "large-origin")
        self.assertEqual(code, 0, log)
        output = PlyData.read(result["modelPath"])["vertex"].data
        self.assertEqual(output.dtype.fields["x"][0].itemsize, 8)
        np.testing.assert_allclose(output["x"], [3_000_000.003, 3_000_000.006, 3_000_000.009], rtol=0., atol=5e-10)
        self.assertEqual(self.model.read_bytes(), before)

    def test_cancel_during_actual_transform_removes_only_owned_staging(self):
        from native.backend import model_scale_version as model_versions
        envelope = model_versions.inspect_model_scale_input(self.model)
        request = self.alignment_request(envelope["data_identity"])
        original = self.model.read_bytes()
        cancelled = [False]
        def progress(value):
            if value["stage"] == "transform_model":
                cancelled[0] = True
        destination = self.root / "cancelled-version"
        with self.assertRaises(model_versions.ModelScaleVersionError) as raised:
            model_versions.create_scaled_model_version(self.model, request, destination, envelope["data_identity"],
                cancelled=lambda: cancelled[0], progress=progress)
        self.assertEqual(raised.exception.code, "model_scale_cancelled")
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob(".*.preparing-model-*")))
        self.assertEqual(self.model.read_bytes(), original)

    def test_legal_deep_model_version_is_not_lengthened_by_staging(self):
        before = self.model.read_bytes()
        code, inspection, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        parent = self.root
        destination = parent / "version"
        while len(str(destination)) < 211:
            missing = 211 - len(str(destination))
            if missing == 1:
                destination = destination.with_name(destination.name + "p")
                break
            parent = parent / ("p" * min(30, missing - 1))
            destination = parent / "version"
        parent.mkdir(parents=True, exist_ok=True)
        final_descriptor = destination / "source/input_descriptor.json"
        old_stage = destination.with_name("." + destination.name + ".preparing-model-" + "a" * 32)
        self.assertLessEqual(len(str(final_descriptor)), 240)
        self.assertGreater(len(str(old_stage / "source/input_descriptor.json")), 260)
        code, result, log = self.cli("create-version", identity=inspection["inputDataIdentity"], destination=destination)
        self.assertEqual(code, 0, "final={} chars, old staging={} chars\n{}".format(len(str(final_descriptor)), len(str(old_stage / "source/input_descriptor.json")), log))
        self.assertTrue(final_descriptor.is_file())
        self.assertTrue(Path(result["modelPath"]).is_file())
        self.assertEqual(self.model.read_bytes(), before)
        self.assertEqual(set(parent.iterdir()), {destination})

    def test_sidecar_changes_are_identity_changes_and_stale_model_hash_is_refused(self):
        code, inspected, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        code, result, log = self.cli("create-version", identity=inspected["inputDataIdentity"],
                                     destination=self.root / "first-version")
        self.assertEqual(code, 0, log)
        model = Path(result["modelPath"])
        code, initial, log = self.cli("inspect", model)
        self.assertEqual(code, 0, log)
        sidecar = model.with_suffix(".camera-scale.json")
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        metadata["operator_note"] = "different reference provenance"
        sidecar.write_text(json.dumps(metadata), encoding="utf-8")
        code, changed, log = self.cli("inspect", model)
        self.assertEqual(code, 0, log)
        self.assertNotEqual(initial["inputDataIdentity"], changed["inputDataIdentity"])
        model.write_bytes(model.read_bytes() + b"\n")
        code, error, log = self.cli("inspect", model)
        self.assertEqual(code, 2, log)
        self.assertEqual(error["code"], "model_scale_sidecar_hash_mismatch")

    def test_texture_path_escape_is_refused_not_copied_from_outside_model_root(self):
        vertices = PlyData.read(self.model)["vertex"].data.copy()
        PlyData([PlyElement.describe(vertices, "vertex")], comments=["TextureFile ../outside.png"]).write(self.model)
        code, error, log = self.cli("inspect")
        self.assertEqual(code, 2, log)
        self.assertEqual(error["code"], "model_scale_texture_outside_source")

    def test_failed_independent_check_is_reported_but_never_bakes_a_model(self):
        code, inspected, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        identity = inspected["inputDataIdentity"]
        request = self.alignment_request(identity)
        request["constraints"].append({"id": "independent-bad", "kind": "distance", "role": "check",
            "source_a": [0., 0., 0.], "source_b": [2., 0., 0.], "reference_distance": 5.,
            "reference_unit": "m", "reference_id": "separate-ruler", "independent_of_fit": True,
            "tolerance_m": .01, "provenance": {"source": "separate_generated_reference", "measurement_id": "independent-ruler"}})
        code, report, log = self.cli("solve", identity=identity, request=request)
        self.assertEqual(code, 0, log)
        solution = json.loads(Path(report["reportPath"]).read_text(encoding="utf-8"))
        self.assertEqual(solution["independent_checks"]["status"], "fail")
        code, error, log = self.cli("create-version", identity=identity, request=request,
                                    destination=self.root / "failed-model")
        self.assertEqual(code, 2, log)
        self.assertEqual(error["code"], "model_scale_independent_checks_failed")
        self.assertFalse((self.root / "failed-model").exists())

    def test_ascii_ply_and_repeated_owned_texture_references_do_not_lose_attributes(self):
        vertices = PlyData.read(self.model)["vertex"].data.copy()
        texture = self.root / "texture.png"
        Image.new("RGB", (2, 2), (5, 10, 15)).save(texture)
        PlyData([PlyElement.describe(vertices, "vertex")], text=True,
                comments=["TextureFile texture.png", "TextureFile texture.png"]).write(self.model)
        code, inspected, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        code, result, log = self.cli("create-version", identity=inspected["inputDataIdentity"],
                                     destination=self.root / "ascii-textured")
        self.assertEqual(code, 0, log)
        result_ply = PlyData.read(result["modelPath"])
        np.testing.assert_array_equal(result_ply["vertex"].data["red"], vertices["red"])
        comments = [value for value in result_ply.comments if value.startswith("TextureFile ")]
        self.assertEqual(len(comments), 2)
        for comment in comments:
            self.assertEqual((Path(result["versionPath"]) / comment[12:]).read_bytes(), texture.read_bytes())

    def test_native_unit_comment_syntaxes_do_not_survive_baked_metric_output(self):
        vertices = PlyData.read(self.model)["vertex"].data.copy()
        declarations = ["units=mm", "unit:mm", "coordinate_unit=cm", "coordinate unit : millimeters"]
        for index, declaration in enumerate(declarations):
            with self.subTest(declaration=declaration):
                PlyData([PlyElement.describe(vertices, "vertex")], comments=[declaration, "keep original note"]).write(self.model)
                before = self.model.read_bytes()
                code, inspected, log = self.cli("inspect")
                self.assertEqual(code, 0, log)
                code, result, log = self.cli("create-version", identity=inspected["inputDataIdentity"],
                                           destination=self.root / ("unit-syntax-" + str(index)))
                self.assertEqual(code, 0, log)
                output = PlyData.read(result["modelPath"], mmap=False)
                self.assertNotIn(declaration, output.comments)
                self.assertIn("units m", output.comments)
                self.assertIn("keep original note", output.comments)
                self.assertEqual(self.model.read_bytes(), before)
                self.assertEqual((Path(result["versionPath"]) / "source/original.ply").read_bytes(), before)

    def test_element_texture_and_metadata_comments_rebind_in_the_original_global_order(self):
        vertices = PlyData.read(self.model)["vertex"].data.copy()
        faces = np.empty(1, dtype=[("vertex_indices", "O"), ("texcoord", "O")])
        faces["vertex_indices"][0] = np.array([0, 1, 2], dtype="i4")
        faces["texcoord"][0] = np.array([0., 0., 1., 0., 0., 1.], dtype="f4")
        texture = self.root / "nested/element texture.png"
        texture.parent.mkdir()
        Image.new("RGB", (3, 2), (120, 60, 30)).save(texture)
        texture_bytes = texture.read_bytes()
        vertex = PlyElement.describe(vertices, "vertex", comments=[
            '  TextureFile "nested/element texture.png"', "coordinate_unit=cm", "crs EPSG:32654", "keep vertex note"])
        face = PlyElement.describe(faces, "face", comments=[
            'TextureFile "nested/element texture.png"', "unit:mm", "coordinate_system=source-reference", "keep face note"])
        PlyData([vertex, face], comments=["keep global note"], obj_info=["units=mm", "keep object note"]).write(self.model)
        before = self.model.read_bytes()
        code, inspected, log = self.cli("inspect")
        self.assertEqual(code, 0, log)
        code, result, log = self.cli("create-version", identity=inspected["inputDataIdentity"], destination=self.root / "element-texture")
        self.assertEqual(code, 0, log)
        output = PlyData.read(result["modelPath"], mmap=False)
        self.assertEqual(output["vertex"].comments[-1], "keep vertex note")
        self.assertEqual(output["face"].comments[-1], "keep face note")
        for element in output.elements:
            comments = [value for value in element.comments if value.startswith("TextureFile ")]
            self.assertEqual(len(comments), 1)
            reference = comments[0][12:]
            self.assertTrue(reference.startswith("assets/"), reference)
            self.assertEqual((Path(result["modelPath"]).parent / reference).read_bytes(), texture_bytes)
            self.assertFalse(os.path.samefile(texture, Path(result["modelPath"]).parent / reference))
            self.assertFalse(any("EPSG" in item or "coordinate_system" in item or "unit" in item for item in element.comments))
        self.assertEqual(output.obj_info, ["keep object note"])
        np.testing.assert_array_equal(output["face"].data["vertex_indices"][0], faces["vertex_indices"][0])
        np.testing.assert_array_equal(output["face"].data["texcoord"][0], faces["texcoord"][0])
        self.assertIn("keep global note", output.comments)
        self.assertEqual(self.model.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
