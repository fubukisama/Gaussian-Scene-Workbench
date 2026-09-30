import hashlib
import io
import math
import struct
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from native.worker import mesh_material_preview as adapter

try:
    from PIL import Image
    PILLOW_RUNTIME = True
except ImportError:
    PILLOW_RUNTIME = False


OBJ = ("mtllib material.mtl\n"
       "v 123456789.12345679 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
       "vt 0.25 0.25\nvt 0.75 0.25\nvt 0.25 0.75\n"
       "usemtl surface\nf 1/1 2/2 3/3\n")
MTL = "newmtl surface\nKa 1 1 1\nKd 1 1 1\nKs 0 0 0\nNs 0\nillum 2\nmap_Kd image.png\n"


def read_ply(path):
    with Path(path).open("rb") as source:
        header = []
        while True:
            line = source.readline().decode("ascii").strip()
            header.append(line)
            if line == "end_header":
                break
        vertex_count = int(next(line for line in header if line.startswith("element vertex ")).split()[2])
        face_count = int(next(line for line in header if line.startswith("element face ")).split()[2])
        vertices = [struct.unpack("<ddd", source.read(24)) for _ in range(vertex_count)]
        faces = []
        for _ in range(face_count):
            packed = struct.unpack("<B3iB6f", source.read(38))
            if packed[0] != 3 or packed[4] != 6:
                raise AssertionError("Invalid PLY lists")
            faces.append((packed[1:4], packed[5:]))
        if source.read(1):
            raise AssertionError("Trailing PLY data")
    return header, vertices, faces


def sample_uv(image, u, v):
    x = min(image.width - 1, max(0, math.floor(u * image.width)))
    y = min(image.height - 1, max(0, math.floor((1 - v) * image.height)))
    return image.getpixel((x, y))


@unittest.skipUnless(PILLOW_RUNTIME, "Run in a training Python containing Pillow")
class MaterialPreviewTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-material-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assets = self.root / "owned job"
        self.assets.mkdir()
        self.obj = self.assets / "mesh.obj"
        self.obj.write_text(OBJ, encoding="utf-8")
        self.mtl = self.assets / "material.mtl"
        self.mtl.write_text(MTL, encoding="utf-8")
        with Image.new("RGB", (2, 2)) as image:
            image.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)])
            image.save(str(self.assets / "image.png"))
        self.files = {"obj": str(self.obj)}
        self.destination = self.assets / "preview"

    def hashes(self):
        return {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.assets.rglob("*") if path.is_file() and self.destination not in path.parents}

    def prepare(self):
        return adapter.prepare_material_preview(self.files, self.destination, self.assets)

    def assert_failure(self, exception=ValueError):
        hashes = self.hashes()
        with self.assertRaises(exception):
            self.prepare()
        self.assertFalse(self.destination.exists())
        self.assertEqual(hashes, self.hashes())

    def test_single_page_pixels_orientation_precision_and_unchanged_sources(self):
        before = self.hashes()
        result = self.prepare()
        self.assertEqual(before, self.hashes())
        self.assertEqual((result["vertices"], result["faces"], result["materials"], result["texturePages"]), (4, 1, 1, 1))
        header, vertices, faces = read_ply(result["ply"])
        self.assertIn("comment TextureFile atlas.png", header)
        self.assertIn("property double x", header)
        self.assertEqual(vertices[0][0], 123456789.12345679)
        self.assertEqual(faces[0], ((0, 1, 2), (0.25, 0.25, 0.75, 0.25, 0.25, 0.75)))
        with Image.open(result["atlas"]) as image, Image.open(str(self.assets / "image.png")) as source:
            self.assertEqual(image.size, source.size)
            self.assertEqual(list(image.getdata()), list(source.convert("RGBA").getdata()))
            self.assertEqual(sample_uv(image, *faces[0][1][0:2]), (0, 0, 255, 255))
            self.assertEqual(sample_uv(image, *faces[0][1][4:6]), (255, 0, 0, 255))

    def test_per_corner_uv_seam_not_welded(self):
        self.obj.write_text(OBJ + "f 1/3 3/2 4/1\n", encoding="utf-8")
        result = self.prepare()
        _, vertices, faces = read_ply(result["ply"])
        self.assertEqual(len(vertices), 4)
        self.assertEqual(faces[1][0], (0, 2, 3))
        self.assertEqual(faces[0][1][0:2], (0.25, 0.25))
        self.assertEqual(faces[1][1][0:2], (0.25, 0.75))

    def test_multiple_pages_keep_each_material_original_resolution_and_gutters(self):
        with Image.new("RGB", (3, 2), (13, 37, 59)) as image:
            image.putpixel((0, 0), (80, 90, 100))
            image.save(str(self.assets / "second page.png"))
        self.mtl.write_text(MTL + "newmtl second\nKd 1 1 1\nmap_Kd second page.png\n", encoding="utf-8")
        self.obj.write_text(OBJ + "usemtl second\nf 1/1 3/2 4/3\n", encoding="utf-8")
        before = self.hashes()
        result = self.prepare()
        self.assertEqual(before, self.hashes())
        self.assertEqual((result["materials"], result["texturePages"]), (2, 2))
        _, _, faces = read_ply(result["ply"])
        with Image.open(result["atlas"]) as image:
            self.assertEqual(sample_uv(image, *faces[0][1][0:2]), (0, 0, 255, 255))
            self.assertEqual(sample_uv(image, *faces[1][1][0:2]), (13, 37, 59, 255))
            self.assertEqual(sample_uv(image, *faces[1][1][4:6]), (80, 90, 100, 255))
            # Every source pixel occurs unmodified in its complete native-size page.
            self.assertIn((80, 90, 100, 255), list(image.getdata()))
            self.assertLessEqual(max(image.size), adapter.MAX_ATLAS_SIZE)
            self.assertGreater(image.width * image.height, 10)

    def test_negative_obj_indices(self):
        self.obj.write_text(OBJ.replace("f 1/1 2/2 3/3", "f -4/-3 -3/-2 -2/-1"), encoding="utf-8")
        _, _, faces = read_ply(self.prepare()["ply"])
        self.assertEqual(faces[0][0], (0, 1, 2))

    def test_forward_positive_vertex_and_uv_indices(self):
        self.obj.write_text("mtllib material.mtl\nusemtl surface\nf 1/1 2/2 3/3\n" +
                            OBJ.split("mtllib material.mtl\n")[1].split("usemtl surface\n")[0], encoding="utf-8")
        self.assertEqual(self.prepare()["faces"], 1)

    def test_disk_uv_cache_crosses_blocks_and_has_bounded_capacity(self):
        data = b"".join(struct.pack("<dd", index / 10000, 0.5) for index in range(10000))
        reader = adapter._UvReader(io.BytesIO(data))
        with mock.patch.object(adapter, "_UV_CACHE_BLOCK", 64), mock.patch.object(adapter, "_UV_CACHE_BLOCKS", 2):
            for index in (0, 9, 9999, 0, 300, 9):
                self.assertEqual(reader.read(index), (index / 10000, 0.5))
                self.assertLessEqual(len(reader.blocks), 2)

    def test_owned_raw_openmvs_obj_preferred(self):
        raw = self.assets / "raw mesh.obj"
        raw.write_text(OBJ, encoding="utf-8")
        self.obj.write_text("corrupted normalized copy\n", encoding="utf-8")
        self.files["openmvs_obj"] = str(raw)
        result = self.prepare()
        self.assertEqual(result["sourceObj"], str(raw))

    def test_normalized_openmvs_material_uses_owned_copy_not_raw_legacy_Tr(self):
        raw_folder = self.assets / "openmvs raw"
        raw_folder.mkdir()
        raw_obj = raw_folder / "mesh.obj"
        raw_obj.write_text(OBJ, encoding="utf-8")
        (raw_folder / "material.mtl").write_text(MTL + "Tr 1.000000\n", encoding="utf-8")
        with Image.open(str(self.assets / "image.png")) as image:
            image.save(str(raw_folder / "image.png"))
        self.mtl.write_text(MTL + "d 1.000000\n", encoding="utf-8")
        self.files.update(openmvs_obj=str(raw_obj), material_normalization="openmvs_opaque_Tr")
        before = self.hashes()
        result = self.prepare()
        self.assertEqual(result["sourceObj"], str(self.obj))
        self.assertEqual(before, self.hashes())
        self.assertEqual(result["texturePages"], 1)

    def test_external_raw_provenance_ignored_for_owned_copy(self):
        self.files.update(openmvs_obj=str(self.root / "missing external.obj"), source_obj="C:/upstream/sugar.obj")
        self.assertEqual(self.prepare()["sourceObj"], str(self.obj))

    def test_relative_subfolder_and_spaces(self):
        folder = self.assets / "material folder"
        folder.mkdir()
        self.mtl.rename(folder / "scene material.mtl")
        (self.assets / "image.png").rename(folder / "image page.png")
        (folder / "scene material.mtl").write_text(MTL.replace("image.png", "image page.png"), encoding="utf-8")
        self.obj.write_text(OBJ.replace("material.mtl", '"material folder/scene material.mtl"'), encoding="utf-8")
        self.assertEqual(self.prepare()["texturePages"], 1)

    def test_duplicate_page_shared_by_multiple_materials(self):
        self.mtl.write_text(MTL + "newmtl another\nmap_Kd image.png\n", encoding="utf-8")
        self.obj.write_text(OBJ + "usemtl another\nf 1/1 3/2 4/3\n", encoding="utf-8")
        result = self.prepare()
        self.assertEqual((result["materials"], result["texturePages"]), (2, 1))

    def test_invalid_obj_records_indices_uvs_and_geometry_fail_atomically(self):
        replacements = [("f 1/1 2/2 3/3", "f 1/1 2/2 3/3 4/1"),
                        ("f 1/1 2/2 3/3", "f 0/1 2/2 3/3"),
                        ("f 1/1 2/2 3/3", "f 99/1 2/2 3/3"),
                        ("f 1/1 2/2 3/3", "f -99/1 2/2 3/3"),
                        ("f 1/1 2/2 3/3", "f 1/99 2/2 3/3"),
                        ("f 1/1 2/2 3/3", "f 1 2 3"),
                        ("f 1/1 2/2 3/3", "f 1//1 2//1 3//1"),
                        ("f 1/1 2/2 3/3", "f 1/1/9 2/2/9 3/3/9"),
                        ("v 1 0 0", "v nan 0 0"),
                        ("v 1 0 0", "v 1 0 0 1"),
                        ("vt 0.25 0.25", "vt inf 0.25"),
                        ("vt 0.25 0.25", "vt 1.25 0.25"),
                        ("vt 0.25 0.25", "vt 0.25 -0.25"),
                        ("usemtl surface\n", ""),
                        ("usemtl surface", "usemtl missing"),
                        ("f 1/1 2/2 3/3", "l 1 2 3")]
        for old, new in replacements:
            with self.subTest(new=new):
                self.obj.write_text(OBJ.replace(old, new), encoding="utf-8")
                self.assert_failure()

    def test_unsupported_material_effects_fail(self):
        additions = ["map_Kd -s 1 1 1 image.png", "Kd .5 1 1", "d 0.5", "Tr 1",
                     "Ks 1 1 1", "Ke 1 0 0", "illum 4", "map_Bump image.png",
                     "Tf .5 .5 .5", "Ka .2 .2 .2"]
        for addition in additions:
            with self.subTest(addition=addition):
                self.mtl.write_text("newmtl surface\n" + addition + "\n" +
                                    ("map_Kd image.png\n" if not addition.startswith("map_Kd") else ""), encoding="utf-8")
                self.assert_failure()

    def test_missing_or_duplicate_material_map_fails(self):
        for mtl in ("newmtl surface\nKd 1 1 1\n", MTL + "map_Kd image.png\n", MTL + "newmtl surface\n"):
            with self.subTest(mtl=mtl):
                self.mtl.write_text(mtl, encoding="utf-8")
                self.assert_failure()

    def test_relative_path_escapes_absolute_unc_and_uri_rejected(self):
        for path in ("../outside.png", str(self.assets / "image.png"), "C:\\texture.png", "//host/share.png",
                     "\\\\host\\share.png", "https://host/image.png", "file:///image.png"):
            with self.subTest(path=path):
                self.mtl.write_text(MTL.replace("image.png", path), encoding="utf-8")
                self.assert_failure()
        self.obj.write_text(OBJ.replace("material.mtl", "../outside.mtl"), encoding="utf-8")
        self.assert_failure()

    def test_symlink_escape_rejected_where_supported(self):
        outside = self.root / "outside.png"
        with Image.new("RGB", (1, 1)) as image:
            image.save(str(outside))
        link = self.assets / "linked.png"
        try:
            link.symlink_to(outside)
        except OSError as error:
            self.skipTest("Symbolic links unavailable: " + str(error))
        self.mtl.write_text(MTL.replace("image.png", "linked.png"), encoding="utf-8")
        self.assert_failure()

    def test_missing_and_corrupt_images_fail(self):
        image = self.assets / "image.png"
        image.unlink()
        self.assert_failure()
        image.write_bytes(b"not an image")
        self.assert_failure(OSError)

    def test_transparent_image_fails(self):
        with Image.new("RGBA", (2, 2), (1, 2, 3, 100)) as image:
            image.save(str(self.assets / "image.png"))
        self.assert_failure()

    def test_exif_orientation_and_animated_texture_fail(self):
        with Image.new("RGB", (2, 2)) as image:
            exif = Image.Exif()
            exif[274] = 6
            image.save(str(self.assets / "oriented.jpg"), exif=exif)
        self.mtl.write_text(MTL.replace("image.png", "oriented.jpg"), encoding="utf-8")
        self.assert_failure()
        with Image.new("RGB", (2, 2), (255, 0, 0)) as first, Image.new("RGB", (2, 2), (0, 255, 0)) as second:
            first.save(str(self.assets / "animated.gif"), save_all=True, append_images=[second], duration=100, loop=0)
        self.mtl.write_text(MTL.replace("image.png", "animated.gif"), encoding="utf-8")
        self.assert_failure()

    def test_atlas_size_and_memory_budget_fail_without_downsample(self):
        with mock.patch.object(adapter, "MAX_IMAGE_BYTES", 1):
            self.assert_failure()
        with mock.patch.object(adapter, "MAX_ATLAS_SIZE", 1):
            self.assert_failure()

    def test_cancel_before_and_during_conversion_publishes_nothing(self):
        cancel = threading.Event()
        cancel.set()
        with self.assertRaises(InterruptedError):
            adapter.prepare_material_preview(self.files, self.destination, self.assets, cancel)
        self.assertFalse(self.destination.exists())
        class DuringConversion:
            calls = 0
            def is_set(self):
                self.calls += 1
                return self.calls >= 6
        with self.assertRaises(InterruptedError):
            adapter.prepare_material_preview(self.files, self.destination, self.assets, DuringConversion())
        self.assertFalse(self.destination.exists())

    def test_exclusive_output_never_overwrites_existing_preview(self):
        result = self.prepare()
        before = Path(result["ply"]).read_bytes()
        with self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual(Path(result["ply"]).read_bytes(), before)

    def test_source_obj_and_output_must_be_owned(self):
        with self.assertRaises(ValueError):
            adapter.prepare_material_preview({"obj": str(self.root / "outside.obj")}, self.destination, self.assets)
        with self.assertRaises(ValueError):
            adapter.prepare_material_preview(self.files, self.root / "outside", self.assets)
        self.assertFalse(self.destination.exists())

    def test_failed_ply_publication_removes_own_complete_atlas(self):
        real_replace = adapter.os.replace
        def fail_ply(source, destination):
            if str(destination).endswith("preview.ply"):
                raise OSError("owned publication fixture")
            return real_replace(source, destination)
        with mock.patch.object(adapter.os, "replace", side_effect=fail_ply):
            self.assert_failure(OSError)

    def test_source_changed_during_conversion_rejects_preview(self):
        real_make_atlas = adapter._make_atlas
        def changed_source(*args):
            result = real_make_atlas(*args)
            self.obj.write_text(OBJ + "# changed by the fixture\n", encoding="utf-8")
            return result
        with mock.patch.object(adapter, "_make_atlas", side_effect=changed_source):
            with self.assertRaisesRegex(ValueError, "source changed"):
                self.prepare()
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    unittest.main()
