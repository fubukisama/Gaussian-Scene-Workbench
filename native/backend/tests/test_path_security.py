"""Containment contracts at the native compute boundary, without an HTTP shim."""

import errno
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from native.backend import compute


class BackendPathSecurityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-native-path-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name, relative in (("OUTPUT_DIR", "output"), ("DATASETS_DIR", "datasets")):
            path = self.root / relative
            path.mkdir()
            patcher = mock.patch.object(compute, name, path)
            patcher.start()
            self.addCleanup(patcher.stop)

    def write_fixture(self, relative, contents):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def make_link(self, relative, target, directory=False):
        link = self.root / relative
        link.parent.mkdir(parents=True, exist_ok=True)
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as exc:
            if exc.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS) and getattr(exc, "winerror", None) != 1314:
                raise
            if directory and os.name == "nt":
                for path in (link, target):
                    Path(path).absolute().relative_to(self.root.absolute())
                    if any(character in str(path) for character in '"&|<>^%\r\n'):
                        raise ValueError("Invalid owned fixture path")
                result = subprocess.run(
                    [os.environ.get("ComSpec", "cmd.exe"), "/d", "/c", "mklink", "/J", str(link), str(target)],
                    capture_output=True, text=True, timeout=10,
                )
                if result.returncode == 0:
                    return link
                failure = (result.stdout + result.stderr).lower()
                if not any(reason in failure for reason in ("not supported", "privilege", "access is denied")):
                    self.fail("Unable to prepare the owned junction fixture: " + failure)
            self.skipTest("Platform does not permit the owned symbolic-link fixture; Linux CI covers it")
        return link

    def test_managed_paths_reject_parent_sibling_and_root_targets(self):
        for path in ("../output-sibling/private.ply", "..", ".", "nested/../../outside"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                compute.managed_path(compute.OUTPUT_DIR, path)
        self.assertEqual(compute.managed_path(compute.OUTPUT_DIR, "scene", "cloud.ply"),
                         compute.OUTPUT_DIR / "scene" / "cloud.ply")

    def test_invalid_scene_names_cannot_create_or_replace_managed_outputs(self):
        for scene in ("..", "../outside", "nested/model", "nested\\model", ".hidden", "CON", "NUL.ply", "name."):
            with self.subTest(scene=scene), self.assertRaises(ValueError):
                compute.prepare_dataset_import(scene, True)
        self.assertEqual(list(compute.DATASETS_DIR.iterdir()), [])

    def test_native_import_preserves_external_unicode_paths_and_metadata(self):
        source = self.write_fixture("原始 日本語/photos/模型.png", b"original source pixels\x00\xff")
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        with mock.patch.object(compute, "archive_local_source", wraps=compute.archive_local_source):
            result = compute.save_path_dataset({
                "scene": "native-import", "files": [{"path": str(source), "name": source.name,
                    "relativePath": "photos/模型.png", "lastModified": 1234567890000,
                    "size": source.stat().st_size, "type": "image/png"}],
            })
        dataset = Path(result["path"])
        manifest = json.loads((dataset / "source" / "metadata_manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(result["image_count"], 1)
        self.assertEqual(manifest["files"][0]["originalFilename"], source.name)
        self.assertEqual(manifest["files"][0]["relativePath"], "photos/模型.png")
        self.assertEqual(manifest["files"][0]["sourcePath"], str(source))
        self.assertEqual(manifest["files"][0]["lastModified"], 1234567890000)
        self.assertEqual((dataset / manifest["files"][0]["archivePath"]).read_bytes(), source.read_bytes())
        self.assertEqual((dataset / manifest["files"][0]["trainingPath"]).read_bytes(), source.read_bytes())
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)

    def test_import_rejects_linked_dataset_before_overwriting_external_files(self):
        outside = self.write_fixture("outside/dataset/keep.txt", b"keep original")
        source = self.write_fixture("source/photo.png", b"selected input")
        self.make_link("datasets/linked", outside.parent, directory=True)
        with self.assertRaises(ValueError):
            compute.save_path_dataset({"scene": "linked", "overwrite": True,
                                       "files": [{"path": str(source)}]})
        self.assertEqual(outside.read_bytes(), b"keep original")
        self.assertEqual(sorted(path.name for path in outside.parent.iterdir()), ["keep.txt"])

    def test_dangling_import_output_cannot_create_an_external_file(self):
        source = self.write_fixture("source/photo.png", b"selected input")
        outside = self.root / "outside" / "not-created.png"
        outside.parent.mkdir()
        self.make_link("datasets/scene/source/originals/photo.png", outside)
        with self.assertRaises(ValueError):
            compute.save_path_dataset({"scene": "scene", "files": [{"path": str(source)}]})
        self.assertFalse(outside.exists())
        self.assertEqual(source.read_bytes(), b"selected input")

    def test_model_and_nested_mesh_paths_cannot_escape_through_links(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-scene"
            external = self.write_fixture("outside/" + scene + "/marker.ply", b"private marker")
            self.make_link("output/" + scene + "/train", external.parent, directory=True)
            for mode in ("bounded", "unbounded", "sugar", "gs2mesh"):
                with self.subTest(backend=backend, mode=mode), self.assertRaises(ValueError):
                    compute.mesh_output_path(scene, 7, mode)
            self.assertEqual(external.read_bytes(), b"private marker")

    def test_model_root_link_cannot_expose_external_point_cloud(self):
        external = self.write_fixture("outside/model/point_cloud/iteration_1/point_cloud.ply", b"private cloud")
        self.make_link("output/linked", external.parents[2], directory=True)
        with self.assertRaises(ValueError):
            compute.ply_path("linked", 1)
        self.assertEqual(external.read_bytes(), b"private cloud")

    def test_texture_directory_cannot_escape_owned_mesh(self):
        outside = self.write_fixture("outside/textures/texture.png", b"private texture")
        self.make_link("output/scene/train/ours_7/fuse_post_texture", outside.parent, directory=True)
        with self.assertRaises(ValueError):
            compute.mesh_texture_paths("scene", 7)
        self.assertEqual(outside.read_bytes(), b"private texture")

    def test_configuration_cannot_execute_python_expressions(self):
        for backend in ("3dgs", "2dgs"):
            model = compute.OUTPUT_DIR / backend
            model.mkdir()
            marker = self.root / (backend + "-executed")
            expression = "Namespace(source_path=__import__('pathlib').Path({!r}).write_text('executed'))".format(str(marker))
            (model / "cfg_args").write_text(expression, encoding="utf-8")
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                compute.load_cfg_args(model)
            self.assertFalse(marker.exists())

    def test_configuration_preserves_literal_parameters_for_both_backends(self):
        values = {"source_path": str(self.root / "源 モデル with spaces"), "iterations": 30000,
                  "white_background": True, "resolution": -1, "depth_ratio": .25, "optional": None,
                  "save_iterations": [7, 30000], "crop_box": (-1., 2.),
                  "metadata": {"label": "原始 パラメータ", "active": False}}
        for backend in ("3dgs", "2dgs"):
            model = compute.OUTPUT_DIR / backend
            model.mkdir()
            expression = "Namespace(" + ", ".join(key + "=" + repr(value) for key, value in values.items()) + ")"
            (model / "cfg_args").write_text(expression, encoding="utf-8")
            with self.subTest(backend=backend):
                self.assertEqual(vars(compute.load_cfg_args(model)), values)

    def test_managed_removal_refuses_root_and_sibling_trees(self):
        outside = self.write_fixture("output-sibling/keep.txt", b"keep sibling")
        for target in (compute.OUTPUT_DIR, outside.parent):
            with self.subTest(target=target), self.assertRaisesRegex(RuntimeError, "Refusing to remove"):
                compute.remove_tree_inside(compute.OUTPUT_DIR, target)
        self.assertTrue(compute.OUTPUT_DIR.is_dir())
        self.assertEqual(outside.read_bytes(), b"keep sibling")


if __name__ == "__main__":
    unittest.main()
