"""Native model discovery preserves complete files and backend identities."""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from native.backend import compute


class SceneWorkspaceTests(unittest.TestCase):
    def test_native_alpha_masks_preserve_source_images_and_pixels(self):
        from PIL import Image

        with tempfile.TemporaryDirectory(prefix="gsw-native-masks-") as temporary:
            dataset = Path(temporary)
            images = dataset / "images"
            masks = dataset / "source" / "masks"
            originals = dataset / "source" / "originals"
            for directory in (images, masks, originals):
                directory.mkdir(parents=True)
            source = originals / "photo.png"
            Image.new("RGB", (2, 2), (23, 79, 181)).save(source)
            (images / source.name).write_bytes(source.read_bytes())
            mask = Image.new("L", (2, 2))
            mask.putdata([0, 64, 128, 255])
            mask.save(masks / "photo_mask.png")
            original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
            self.assertEqual(compute.apply_alpha_masks_to_dataset(dataset), 1)
            with Image.open(images / "photo.png") as result:
                self.assertEqual(result.mode, "RGBA")
                self.assertEqual(result.getchannel("A").tobytes(), bytes([0, 64, 128, 255]))
                self.assertEqual(result.convert("RGB").tobytes(), bytes([23, 79, 181]) * 4)
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), original_hash)

    def test_model_lookup_keeps_original_gaussian_attributes_for_both_backends(self):
        with tempfile.TemporaryDirectory(prefix="gsw-native-workspace-") as temporary:
            output = Path(temporary) / "output"
            with mock.patch.object(compute, "OUTPUT_DIR", output):
                for backend in ("3dgs", "2dgs"):
                    scene = backend + "-scene"
                    model = output / scene
                    cloud = model / "point_cloud" / "iteration_30000" / "point_cloud.ply"
                    cloud.parent.mkdir(parents=True)
                    original = (b"ply\nformat binary_little_endian 1.0\n"
                                b"element vertex 1\nproperty float x\nproperty float f_dc_0\n"
                                b"property float opacity\nproperty float scale_0\nproperty float rot_0\n"
                                b"end_header\n\x00\x01\x02\x03")
                    cloud.write_bytes(original)
                    (model / "training_backend.json").write_text(json.dumps({"backend": backend}), encoding="utf-8")
                    before = hashlib.sha256(cloud.read_bytes()).hexdigest()
                    with self.subTest(backend=backend):
                        self.assertEqual(compute.model_dir(scene), model.resolve())
                        self.assertEqual(compute.latest_iteration(scene), 30000)
                        self.assertEqual(compute.scene_backend(scene), backend)
                        self.assertEqual(compute.ply_path(scene, 30000), cloud.resolve())
                        self.assertEqual(hashlib.sha256(cloud.read_bytes()).hexdigest(), before)
                        self.assertEqual(cloud.read_bytes(), original)

    def test_all_mesh_modes_keep_their_output_paths_inside_the_model(self):
        with tempfile.TemporaryDirectory(prefix="gsw-native-mesh-path-") as temporary:
            with mock.patch.object(compute, "OUTPUT_DIR", Path(temporary)):
                for mode, name in (("bounded", "fuse_post.ply"), ("unbounded", "fuse_unbounded_post.ply"),
                                   ("sugar", "sugar_mesh_post.ply"), ("gs2mesh", "gs2mesh_post.ply")):
                    with self.subTest(mode=mode):
                        model = compute.model_dir("scene")
                        result = compute.mesh_output_path("scene", 7, mode)
                        self.assertEqual(result, model / "train" / "ours_7" / name)
                        self.assertEqual(compute.mesh_texture_dir("scene", 7, mode).relative_to(model),
                                         Path("train") / "ours_7" / (Path(name).stem + "_texture"))


if __name__ == "__main__":
    unittest.main()
