"""Opt-in original-resolution loader checks in the actual 3DGS/2DGS runtimes.

Run with each backend's training Python and DLL PATH. Set
GSW_RESOLUTION_TEST_ROOT to an installed package and
GSW_RESOLUTION_TEST_BACKEND to 3dgs or 2dgs. CUDA is required.
"""
import json
import math
import os
from pathlib import Path
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
import uuid


class OriginalResolutionCudaTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("GSW_RESOLUTION_TEST_ROOT"),
                         "Set GSW_RESOLUTION_TEST_ROOT to opt in to installed trainer execution")
    def test_real_trainer_emits_loaded_input_report(self):
        """Exercise the real process event, not a reconstruction-quality benchmark."""
        import numpy as np
        import torch
        from PIL import Image

        self.assertTrue(torch.cuda.is_available(), "CUDA is required for this opt-in test")
        repository = Path(os.environ["GSW_RESOLUTION_TEST_ROOT"]).resolve()
        backend = os.environ.get("GSW_RESOLUTION_TEST_BACKEND", "3dgs")
        self.assertIn(backend, ("3dgs", "2dgs"))
        source = (Path(os.environ["TWO_DGS_DIR"]).resolve() if backend == "2dgs"
                  else repository / "gaussian-splatting")
        entry = (repository / "native/worker/two_dgs_train.py" if backend == "2dgs"
                 else source / "train.py")
        self.assertTrue(entry.is_file(), str(entry))
        self.assertTrue((repository / "native/worker/training_summary.py").is_file())
        # The caller sets TEMP/TMP to the data-drive validation directory. Every
        # input, output, control request and process below belongs to this test.
        with tempfile.TemporaryDirectory(prefix="native-cuda-input-event-") as temporary:
            root = Path(temporary)
            dataset = root / "dataset"
            (dataset / "images").mkdir(parents=True)
            width, height, count = 32, 24, 4
            frames = []
            for index in range(count):
                angle = index * math.pi / 2
                position = np.array([3 * math.sin(angle), -3 * math.cos(angle), 1.0])
                backward = position / np.linalg.norm(position)
                right = np.cross([0, 0, 1], backward)
                right /= np.linalg.norm(right)
                up = np.cross(backward, right)
                matrix = np.eye(4)
                matrix[:3, :3] = np.stack([right, up, backward], axis=1)
                matrix[:3, 3] = position
                frames.append({"file_path": "images/view-{}".format(index),
                               "transform_matrix": matrix.tolist()})
                photo = np.zeros((height, width, 4), dtype=np.uint8)
                photo[:, :, 3] = 255
                photo[6:18, 8:24, :3] = [180, 110 + index * 10, 60]
                Image.fromarray(photo).save(str(dataset / "images/view-{}.png".format(index)))
            for split in ("train", "test"):
                (dataset / ("transforms_" + split + ".json")).write_text(json.dumps({
                    "camera_angle_x": 0.7, "frames": frames if split == "train" else []}),
                    encoding="utf-8")
            point_rng = np.random.RandomState(71)
            points = []
            for x in range(4):
                for y in range(4):
                    for z in range(2):
                        jitter = point_rng.uniform(-.035, .035, 3)
                        points.append("{} {} {} 0 0 0 180 120 60".format(
                            x * .2 - .3 + jitter[0], y * .2 - .3 + jitter[1], z * .2 + jitter[2]))
            (dataset / "points3d.ply").write_text(
                "ply\nformat ascii 1.0\nelement vertex 32\nproperty float x\nproperty float y\nproperty float z\n"
                "property float nx\nproperty float ny\nproperty float nz\nproperty uchar red\nproperty uchar green\n"
                "property uchar blue\nend_header\n" + "\n".join(points) + "\n", encoding="ascii")

            configured = {
                "version": 1, "phase": "configured", "backend": backend, "quality": "quick",
                "iterations": 2, "resolution": 1, "optimizer": "adam" if backend == "2dgs" else "default",
                "densifyUntil": 0, "densificationInterval": 80, "densifyGradient": 0.00012,
            }
            if backend == "2dgs":
                configured["depthRatio"] = 0.0
            else:
                configured.update(antialiasing=False, exposureCompensation=False)
            output = root / "output"
            env = os.environ.copy()
            env["GSW_NATIVE_TRAINING_CONTROL"] = json.dumps({
                "output": str(output), "request": str(root / "pause-request.json"),
                "session": uuid.uuid4().hex, "backend": backend, "resume": False,
                "trainingSummary": configured})
            env["GSW_TWO_DGS_SOURCE"] = str(source)
            env["PYTHONUTF8"] = "1"
            # Match every parameter in the configured report to the actual CLI;
            # densification is disabled rather than disguising a huge threshold.
            command = [sys.executable, "-B", str(entry), "-s", str(dataset), "-m", str(output),
                       "--iterations", "2", "-r", "1", "--data_device", "cpu",
                       "--test_iterations", "2", "--save_iterations", "2", "--checkpoint_iterations", "2",
                       "--densify_until_iter", "0", "--densification_interval", "80",
                       "--densify_grad_threshold", "0.00012"]
            command += (["--depth_ratio", "0"] if backend == "2dgs"
                        else ["--disable_viewer", "--optimizer_type", "default"])
            result = subprocess.run(command, cwd=str(source), env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    encoding="utf-8", errors="replace", timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout[-12000:])
            prefix = "[gsw-training-input] "
            events = [line[len(prefix):] for line in result.stdout.splitlines() if line.startswith(prefix)]
            self.assertEqual(len(events), 1, result.stdout[-12000:])
            expected = dict(configured, phase="loaded", trainImageCount=count,
                            trainDimensionKinds=1, trainDimensions=[[width, height, count]],
                            trainPixels=width * height * count)
            self.assertEqual(json.loads(events[0]), expected)
            cameras = json.loads((output / "cameras.json").read_text(encoding="utf-8"))
            self.assertEqual(len(cameras), count)
            self.assertEqual([(camera["width"], camera["height"]) for camera in cameras],
                             [(width, height)] * count)
            self.assertTrue((output / "point_cloud/iteration_2/point_cloud.ply").is_file())
            print("{} actual installed trainer: 2 iterations; one loaded input event; "
                  "{} cameras at {}x{}, {} pixels; parameter report matches CLI".format(
                      backend, count, width, height, expected["trainPixels"]), flush=True)

    def test_colmap_pinhole_preparation_keeps_original_dimensions(self):
        from PIL import Image

        repository = Path(os.environ.get("GSW_RESOLUTION_TEST_ROOT", Path(__file__).resolve().parents[2]))
        sys.path.insert(0, str(repository / "crop_editor"))
        import server
        with tempfile.TemporaryDirectory(prefix="original-undistortion-") as temporary:
            dataset = Path(temporary)
            (dataset / "input").mkdir()
            model = dataset / "distorted" / "sparse" / "0"
            model.mkdir(parents=True)
            (model / "cameras.txt").write_text("1 PINHOLE 1928 1084 1600 1600 964 542\n", encoding="ascii")
            (model / "images.txt").write_text("".join(
                "{} 1 0 0 0 {} 0 0 1 view-{}.png\n\n".format(index, index, index)
                for index in range(1, 4)), encoding="ascii")
            (model / "points3D.txt").write_text("", encoding="ascii")
            for index in range(1, 4):
                Image.new("RGB", (1928, 1084), (30, 70, 110)).save(str(dataset / "input" / ("view-{}.png".format(index))))
            command = server.colmap_convert_commands(dataset, server.colmap_options_from_payload({}))[-1]
            result = subprocess.run([str(arg) for arg in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            for index in range(1, 4):
                for root in (dataset / "input", dataset / ".colmap-undistorted" / "images"):
                    with Image.open(str(root / ("view-{}.png".format(index)))) as image:
                        self.assertEqual(image.size, (1928, 1084))
            print("COLMAP uncapped PINHOLE undistortion: 3/3 images retained at 1928x1084", flush=True)

    def test_original_preset_reaches_upstream_camera_without_downsampling(self):
        import numpy as np
        import torch
        from PIL import Image

        self.assertTrue(torch.cuda.is_available(), "CUDA is required for this opt-in test")
        repository = Path(os.environ.get("GSW_RESOLUTION_TEST_ROOT", Path(__file__).resolve().parents[2]))
        backend = os.environ.get("GSW_RESOLUTION_TEST_BACKEND", "3dgs")
        self.assertIn(backend, ("3dgs", "2dgs"))
        sys.path.insert(0, str(repository / "crop_editor"))
        import server
        self.assertEqual(Path(server.__file__).resolve().parent, (repository / "crop_editor").resolve())
        from native.worker.training_summary import configured_training_summary, emit_loaded_training_summary
        source = Path(os.environ["TWO_DGS_DIR"]) if backend == "2dgs" else repository / "gaussian-splatting"
        sys.path.insert(0, str(source))
        # Follow the trainers' import order; importing camera_utils first would
        # create a cycle through scene.__init__ before its helpers are defined.
        import scene  # noqa: F401
        from utils.camera_utils import loadCam

        # A width above 1600 detects accidentally using the upstream automatic
        # resolution mode. Unequal dimensions catch width-only resizing too.
        width, height = 1928, 1084
        pixels = np.zeros((height, width, 3), dtype=np.uint8)
        pixels[:, :, 0] = np.arange(width, dtype=np.uint16)[None, :] % 256
        pixels[:, :, 1] = np.arange(height, dtype=np.uint16)[:, None] % 256
        pixels[:, :, 2] = 127
        with tempfile.TemporaryDirectory(prefix="original-resolution-") as temporary:
            image_path = Path(temporary) / "source.png"
            Image.fromarray(pixels).save(str(image_path))
            info = SimpleNamespace(uid=1, R=np.eye(3), T=np.array([0.0, 0.0, 3.0]),
                                   FovX=0.8, FovY=0.6, image_path=str(image_path), image_name="source",
                                   depth_path="", depth_params=None, is_test=False)
            with Image.open(str(image_path)) as image:
                info.image = image
                for quality, expected in (("original_quality", (width, height)),
                                          ("max_quality", (width // 2, height // 2))):
                    with self.subTest(backend=backend, quality=quality):
                        options = server.training_options_from_payload(backend, quality)
                        args = SimpleNamespace(resolution=options["resolution"], data_device="cpu", train_test_exp=False)
                        camera = (loadCam(args, 0, info, 1.0) if backend == "2dgs"
                                  else loadCam(args, 0, info, 1.0, False, False))
                        self.assertEqual((camera.image_width, camera.image_height), expected)
                        self.assertEqual(tuple(camera.original_image.shape), (3, expected[1], expected[0]))
                        summary_options = dict(options)
                        if backend == "3dgs":
                            summary_options["optimizer_type"] = "default"
                        configured = configured_training_summary(backend, quality, summary_options)
                        emitted = []
                        summary = emit_loaded_training_summary(lambda *event: emitted.append(event),
                                                               {"trainingSummary": configured}, [camera])
                        self.assertEqual(emitted, [("[gsw-training-input]", summary)])
                        self.assertEqual(summary["trainDimensions"], [[expected[0], expected[1], 1]])
                        self.assertEqual(summary["trainPixels"], expected[0] * expected[1])
                        self.assertEqual(summary["trainImageCount"], 1)
                        if quality == "original_quality":
                            actual = camera.original_image.cpu().numpy()
                            self.assertTrue(np.allclose(actual, pixels.transpose(2, 0, 1) / 255.0, atol=1e-7))
                        print("{} {}: source={}x{}, training={}x{}".format(
                            backend, quality, width, height, *expected), flush=True)
                        del camera
            with Image.open(str(image_path)) as image:
                self.assertEqual(image.size, (width, height))


if __name__ == "__main__":
    unittest.main()
