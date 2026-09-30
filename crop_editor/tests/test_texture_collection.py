"""Regression contracts for safely publishing multi-material mesh textures."""

import hashlib
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import server  # noqa: E402


def write_png(path, color):
    """Write a real 1x1 RGB PNG without adding a fixture dependency."""
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value) & 0xffffffff)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(b"\x00" + bytes(color)))
                     + chunk(b"IEND", b""))


def hashes(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def output_paths(root):
    return {"dir": root,
            "obj": root / "published.obj",
            "mtl": root / "published.mtl",
            "png": root / "primary.png",
            "zip": root / "published.zip",
            "glb": root / "published.glb"}


def write_bundle(root):
    root.mkdir(parents=True)
    write_png(root / "red page" / "atlas.png", (255, 0, 0))
    write_png(root / "green page" / "atlas.png", (0, 255, 0))
    (root / "material library.mtl").write_text(
        'newmtl red\nKd 1 1 1\nmap_Kd "red page/atlas.png"\n'
        'newmtl green\nKd 1 1 1\nmap_Kd green page\\atlas.png\n'
        'newmtl red_again\nKd 1 1 1\nmap_Kd red page/atlas.png\n', encoding="utf-8")
    obj = root / "textured mesh.obj"
    obj.write_text(
        'mtllib "material library.mtl"\n'
        'v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n'
        'vt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\n'
        'usemtl red\nf 1/1 2/2 3/3\n'
        'usemtl green\nf 2/2 4/4 3/3\n'
        'usemtl red_again\nf 1/1 3/3 4/4\n', encoding="utf-8")
    return obj


class TextureCollectionTests(unittest.TestCase):
    def assert_bundle(self, source, outputs, result, glb_materials):
        self.assertEqual(result["png"], str(outputs["png"]))
        self.assertEqual(result["texture_pages"], 2)
        self.assertEqual(result["pngs"], [str(outputs["png"]), str(outputs["dir"] / "primary-page-02.png")])
        maps = [line.split(None, 1)[1] for line in outputs["mtl"].read_text(encoding="utf-8").splitlines()
                if line.startswith("map_Kd ")]
        self.assertEqual(maps, ["primary.png", "primary-page-02.png", "primary.png"])
        self.assertEqual(glb_materials, [maps])
        self.assertEqual(outputs["png"].read_bytes(), (source / "red page" / "atlas.png").read_bytes())
        self.assertEqual((outputs["dir"] / maps[1]).read_bytes(), (source / "green page" / "atlas.png").read_bytes())
        obj = outputs["obj"].read_text(encoding="utf-8")
        self.assertIn("mtllib published.mtl\n", obj)
        self.assertIn("usemtl red\nf 1/1 2/2 3/3\n", obj)
        self.assertIn("usemtl green\nf 2/2 4/4 3/3\n", obj)
        with zipfile.ZipFile(outputs["zip"]) as archive:
            self.assertEqual(set(archive.namelist()), {
                "published.obj", "published.mtl", "primary.png", "primary-page-02.png"})
            for name in archive.namelist():
                self.assertEqual(archive.read(name), (outputs["dir"] / name).read_bytes())

    def fake_glb(self, calls):
        def export(paths):
            calls.append([line.split(None, 1)[1] for line in paths["mtl"].read_text(encoding="utf-8").splitlines()
                          if line.startswith("map_Kd ")])
            paths["glb"].write_bytes(b"glTF" + struct.pack("<II", 2, 12))
        return export

    def test_openmvs_preserves_multi_material_maps_and_all_images(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "OpenMVS source"
            write_bundle(source)
            original = hashes(source)
            outputs = output_paths(root / "output")
            glb_materials = []
            with mock.patch.object(server, "export_textured_obj_to_glb", side_effect=self.fake_glb(glb_materials)):
                result = server.collect_openmvs_texture_outputs(source, outputs)
            self.assert_bundle(source, outputs, result, glb_materials)
            self.assertEqual(hashes(source), original)
            self.assertIsNone(result["glb_error"])

    def test_sugar_preserves_multi_material_maps_source_and_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "SuGaR source"
            obj = write_bundle(source)
            original = hashes(source)
            outputs = output_paths(root / "output")
            mesh = root / "geometry.ply"
            glb_materials = []
            with mock.patch.object(server, "find_latest_sugar_obj", return_value=obj), \
                    mock.patch.object(server, "mesh_texture_paths", return_value=outputs), \
                    mock.patch.object(server, "mesh_output_path", return_value=mesh), \
                    mock.patch.object(server, "export_textured_obj_to_glb", side_effect=self.fake_glb(glb_materials)):
                result = server.collect_sugar_mesh_outputs("scene", 100, source, 0)
            self.assert_bundle(source, outputs, result, glb_materials)
            self.assertEqual(hashes(source), original)
            self.assertIn("element vertex 4\n", mesh.read_text(encoding="utf-8"))
            self.assertIn("element face 3\n", mesh.read_text(encoding="utf-8"))
            self.assertEqual(result["mesh"], str(mesh))

    def test_unsafe_image_references_are_rejected_before_output_is_written(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            outside = root / "secret.png"
            write_png(outside, (10, 20, 30))
            for reference in ("../secret.png", str(outside), "C:/secret.png", "\\\\server\\share\\secret.png"):
                with self.subTest(reference=reference):
                    (source / "material library.mtl").write_text("newmtl mat\nmap_Kd " + reference + "\n", encoding="utf-8")
                    original = hashes(source)
                    outputs = output_paths(root / "output")
                    with self.assertRaisesRegex(RuntimeError, "relative path|escapes"):
                        server.collect_openmvs_texture_outputs(source, outputs)
                    self.assertFalse(outputs["dir"].exists())
                    self.assertEqual(hashes(source), original)

    def test_unsafe_material_references_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            obj = write_bundle(source)
            outside = root / "secret.mtl"
            outside.write_text("newmtl secret\n", encoding="utf-8")
            for reference in ("../secret.mtl", str(outside)):
                with self.subTest(reference=reference):
                    obj.write_text("mtllib " + reference + "\n", encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "relative path|escapes"):
                        server.collect_openmvs_texture_outputs(source, output_paths(root / "output"))

    def test_multiple_material_libraries_are_not_silently_discarded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            obj = write_bundle(source)
            for contents in ('mtllib "material library.mtl"\nmtllib another.mtl\n',
                             'mtllib "material library.mtl" another.mtl\n'):
                with self.subTest(contents=contents):
                    obj.write_text(contents, encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "exactly one|does not exist"):
                        server.collect_openmvs_texture_outputs(source, output_paths(root / "output"))
                    self.assertFalse((root / "output").exists())

    def test_missing_texture_does_not_fall_back_to_unrelated_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            (source / "material library.mtl").write_text("newmtl mat\nmap_Kd missing.png\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "does not exist"):
                server.collect_openmvs_texture_outputs(source, output_paths(root / "output"))
            self.assertFalse((root / "output").exists())

    def test_map_options_are_explicitly_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            (source / "material library.mtl").write_text('newmtl mat\nmap_Kd -s 2 2 1 "red page/atlas.png"\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Unsupported"):
                server.collect_openmvs_texture_outputs(source, output_paths(root / "output"))

    def test_glb_failure_preserves_complete_obj_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            outputs = output_paths(root / "output")
            with mock.patch.object(server, "export_textured_obj_to_glb", side_effect=RuntimeError("converter unavailable")):
                result = server.collect_openmvs_texture_outputs(source, outputs)
            self.assertEqual(result["glb_error"], "converter unavailable")
            self.assertIsNone(result["glb"])
            self.assertEqual(result["texture_pages"], 2)
            self.assertTrue(all(Path(path).is_file() for path in result["pngs"]))
            self.assertTrue(outputs["zip"].is_file())

    def test_primary_png_is_diffuse_not_an_earlier_auxiliary_map(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            (source / "material library.mtl").write_text(
                "newmtl red\nmap_bump green page/atlas.png\nmap_Kd red page/atlas.png\n", encoding="utf-8")
            outputs = output_paths(root / "output")
            with mock.patch.object(server, "export_textured_obj_to_glb"):
                result = server.collect_openmvs_texture_outputs(source, outputs)
            self.assertEqual(result["texture_pages"], 2)
            self.assertEqual(outputs["png"].read_bytes(), (source / "red page" / "atlas.png").read_bytes())
            self.assertIn("map_bump primary-page-02.png\n", outputs["mtl"].read_text(encoding="utf-8"))

    def test_additional_non_png_page_is_converted_and_packaged(self):
        try:
            from PIL import Image
        except ImportError as exc:
            self.skipTest("Non-PNG conversion requires existing Pillow: " + str(exc))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            Image.new("RGB", (2, 2), (0, 200, 50)).save(source / "green page" / "atlas.bmp")
            mtl = source / "material library.mtl"
            mtl.write_text(mtl.read_text(encoding="utf-8").replace(
                "green page\\atlas.png", "green page/atlas.bmp"), encoding="utf-8")
            original = hashes(source)
            outputs = output_paths(root / "output")
            with mock.patch.object(server, "export_textured_obj_to_glb"):
                result = server.collect_openmvs_texture_outputs(source, outputs)
            with Image.open(result["pngs"][1]) as image:
                self.assertEqual(image.format, "PNG")
                self.assertEqual(image.getpixel((0, 0)), (0, 200, 50))
            with zipfile.ZipFile(outputs["zip"]) as archive:
                self.assertIn("primary-page-02.png", archive.namelist())
                self.assertNotIn("atlas.bmp", archive.namelist())
            self.assertEqual(hashes(source), original)

    def test_output_cannot_overwrite_any_source_asset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            obj = write_bundle(source)
            original = hashes(source)
            for key, target in (("obj", obj),
                                ("mtl", source / "material library.mtl"),
                                ("png", source / "green page" / "atlas.png")):
                with self.subTest(key=key):
                    outputs = output_paths(root / "output")
                    outputs[key] = target
                    with self.assertRaisesRegex(RuntimeError, "overwrite its source"):
                        server.collect_openmvs_texture_outputs(source, outputs)
                    self.assertEqual(hashes(source), original)
                    self.assertFalse(outputs["dir"].exists())

    def test_openmvs_official_opaque_material_is_normalized_only_in_owned_copy(self):
        # OpenMVS libs/IO/OBJ.cpp MaterialLib::Save emits this opaque block.
        # Its MaterialLib::Load does not consume Tr, unlike conventional MTL.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "OpenMVS source"
            write_bundle(source)
            mtl = source / "material library.mtl"
            blocks = []
            for material, image in (("red", "red page/atlas.png"),
                                    ("green", "green page/atlas.png"),
                                    ("red_again", "red page/atlas.png")):
                blocks.append("newmtl " + material + "\n"
                              "Ka 1.000000 1.000000 1.000000\n"
                              "Kd 1 1 1\n"
                              "Ks 0.000000 0.000000 0.000000\n"
                              "Tr 1.000000\n"
                              "illum 1\n"
                              "Ns 1.000000\n"
                              "map_Kd " + image + "\n")
            mtl.write_text("".join(blocks), encoding="utf-8")
            original = hashes(source)
            outputs = output_paths(root / "output")
            glb_materials = []
            with mock.patch.object(server, "export_textured_obj_to_glb", side_effect=self.fake_glb(glb_materials)):
                result = server.collect_openmvs_texture_outputs(source, outputs)
            self.assertEqual(result["material_normalization"], "openmvs_opaque_Tr")
            corrected = outputs["mtl"].read_text(encoding="utf-8")
            self.assertEqual(corrected.count("d 1\n"), 3)
            self.assertNotIn("Tr ", corrected)
            self.assert_bundle(source, outputs, result, glb_materials)
            self.assertEqual(hashes(source), original)
            with zipfile.ZipFile(outputs["zip"]) as archive:
                self.assertEqual(archive.read("published.mtl").decode("utf-8"), corrected)

    def test_sugar_and_generic_material_opacity_are_not_reinterpreted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "SuGaR source"
            obj = write_bundle(source)
            mtl = source / "material library.mtl"
            mtl.write_text(mtl.read_text(encoding="utf-8").replace("Kd 1 1 1\n", "Kd 1 1 1\nTr 1.000000\n"), encoding="utf-8")
            original = hashes(source)
            generic = output_paths(root / "generic")
            result = server.collect_textured_obj_bundle(obj, source, generic)
            self.assertNotIn("material_normalization", result)
            self.assertEqual(generic["mtl"].read_text(encoding="utf-8").count("Tr 1.000000\n"), 3)
            sugar = output_paths(root / "sugar")
            with mock.patch.object(server, "find_latest_sugar_obj", return_value=obj), \
                    mock.patch.object(server, "mesh_texture_paths", return_value=sugar), \
                    mock.patch.object(server, "mesh_output_path", return_value=root / "geometry.ply"), \
                    mock.patch.object(server, "export_textured_obj_to_glb"):
                result = server.collect_sugar_mesh_outputs("scene", 100, source, 0)
            self.assertNotIn("material_normalization", result)
            self.assertEqual(sugar["mtl"].read_text(encoding="utf-8").count("Tr 1.000000\n"), 3)
            self.assertEqual(hashes(source), original)

    def test_openmvs_does_not_normalize_other_or_nonfinite_transparency(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            write_bundle(source)
            mtl = source / "material library.mtl"
            for index, scalar in enumerate(("0.5", "nan", "inf", "1.000001", "1 extra")):
                with self.subTest(scalar=scalar):
                    mtl.write_text("newmtl red\nTr " + scalar + "\nmap_Kd red page/atlas.png\n", encoding="utf-8")
                    outputs = output_paths(root / ("output-" + str(index)))
                    with mock.patch.object(server, "export_textured_obj_to_glb"):
                        result = server.collect_openmvs_texture_outputs(source, outputs)
                    self.assertNotIn("material_normalization", result)
                    self.assertIn("Tr " + scalar + "\n", outputs["mtl"].read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
