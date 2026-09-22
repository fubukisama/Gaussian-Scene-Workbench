"""Opt-in real CUDA/Graphdeco pause/resume integration test.

Run with the configured training Python, its DLL PATH and TEMP on a data drive:
    python -m unittest native.worker.test_training_checkpoint_cuda
Only test-owned synthetic data and subprocesses are used. No user models touched.
"""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import shutil
import uuid

from native.worker.training_checkpoint import atomic_json, read_manifest


class CudaResumeTests(unittest.TestCase):
    def test_real_training_pause_resume(self):
        import numpy as np
        import torch
        from PIL import Image
        from plyfile import PlyData
        self.assertTrue(torch.cuda.is_available(), "CUDA is required for this opt-in test")
        repository = Path(os.environ.get("GSW_CHECKPOINT_TEST_ROOT", Path(__file__).resolve().parents[2]))
        with tempfile.TemporaryDirectory(prefix="native-cuda-resume-") as temporary:
            root = Path(temporary)
            dataset = root / "dataset"
            (dataset / "images").mkdir(parents=True)
            frames = []
            for index in range(4):
                angle = index * math.pi / 2
                position = np.array([3 * math.sin(angle), -3 * math.cos(angle), 1.0])
                backward = position / np.linalg.norm(position)
                right = np.cross([0, 0, 1], backward)
                right /= np.linalg.norm(right)
                up = np.cross(backward, right)
                matrix = np.eye(4)
                matrix[:3, :3] = np.stack([right, up, backward], axis=1)
                matrix[:3, 3] = position
                frames.append({"file_path": "images/view-{}".format(index), "transform_matrix": matrix.tolist()})
                photo = np.zeros((32, 32, 4), dtype=np.uint8)
                photo[:, :, 3] = 255
                photo[8:24, 8:24, :3] = [180, 110 + index * 10, 60]
                Image.fromarray(photo).save(str(dataset / "images/view-{}.png".format(index)))
            for split in ("train", "test"):
                atomic_json(dataset / ("transforms_" + split + ".json"),
                            {"camera_angle_x": 0.7, "frames": frames if split == "train" else []})
            points = []
            point_rng = np.random.RandomState(71)
            for x in range(4):
                for y in range(4):
                    for z in range(2):
                        # Avoid exactly coincident camera depths: tie ordering in
                        # the upstream CUDA rasterizer is not a resume contract.
                        jitter = point_rng.uniform(-.035, .035, 3)
                        points.append("{} {} {} 0 0 0 180 120 60".format(
                            x * .2 - .3 + jitter[0], y * .2 - .3 + jitter[1], z * .2 + jitter[2]))
            (dataset / "points3d.ply").write_text(
                "ply\nformat ascii 1.0\nelement vertex 32\nproperty float x\nproperty float y\nproperty float z\n"
                "property float nx\nproperty float ny\nproperty float nz\nproperty uchar red\nproperty uchar green\n"
                "property uchar blue\nend_header\n" + "\n".join(points) + "\n", encoding="ascii")

            def run(name, resume=False, pause=False):
                output = root / name
                request = root / (name + "-pause.json")
                session = uuid.uuid4().hex
                if pause:
                    atomic_json(request, {"session": session})
                env = os.environ.copy()
                env["GSW_NATIVE_TRAINING_CONTROL"] = json.dumps({
                    "output": str(output), "request": str(request), "session": session, "resume": resume})
                env["PYTHONUTF8"] = "1"
                command = [sys.executable, "train.py", "-s", str(dataset), "-m", str(output),
                           "--iterations", "16", "-r", "1", "--data_device", "cpu", "--disable_viewer",
                           "--test_iterations", "16", "--save_iterations", "16", "--checkpoint_iterations", "16",
                           "--optimizer_type", "default", "--random_background", "--train_test_exp",
                           "--densify_from_iter", "0", "--densify_until_iter", "14",
                           "--densification_interval", "2", "--densify_grad_threshold", "1000",
                           "--opacity_reset_interval", "8"]
                result = subprocess.run(command, cwd=str(repository / "gaussian-splatting"), env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        encoding="utf-8", errors="replace", timeout=120)
                self.assertEqual(result.returncode, 75 if pause else 0, result.stdout[-12000:])
                metrics[name] = [json.loads(line.split("[gsw-training-metrics] ", 1)[1])
                                 for line in result.stdout.splitlines() if line.startswith("[gsw-training-metrics] ")]
                return output

            metrics = {}
            # Both branches start from the SAME tensor state. Independent CUDA
            # KNN/rasterizer initializations are not a deterministic baseline.
            seed = run("seed", pause=True)
            manifest, state_path = read_manifest(seed)
            self.assertEqual(manifest["iteration"], 1)
            state = torch.load(str(state_path))
            self.assertEqual(len(state["pending_indices"]), 3)
            self.assertTrue(state["exposure_optimizer"]["state"])
            self.assertGreater(float(state["model"][8].sum()), 0)  # densification gradient accumulator
            # Verify every saved CUDA tensor and optimizer slot exactly, before
            # performing more potentially non-deterministic rasterizer updates.
            sys.path.insert(0, str(repository / "gaussian-splatting"))
            from argparse import ArgumentParser
            from arguments import OptimizationParams
            from scene import GaussianModel
            from native.worker.training_checkpoint import restore_state
            parser = ArgumentParser()
            params = OptimizationParams(parser)
            opt = params.extract(parser.parse_args(["--iterations", "16", "--optimizer_type", "default"]))
            restored = GaussianModel(3, "default")
            restore_state(torch, np, restored, opt, state, manifest["identity"], state["camera_names"])

            def equal_tree(expected, actual):
                if torch.is_tensor(expected):
                    self.assertTrue(torch.equal(expected, actual))
                elif isinstance(expected, dict):
                    self.assertEqual(set(expected), set(actual))
                    for key in expected:
                        equal_tree(expected[key], actual[key])
                elif isinstance(expected, (list, tuple)):
                    self.assertEqual(len(expected), len(actual))
                    for left, right in zip(expected, actual):
                        equal_tree(left, right)
                else:
                    self.assertEqual(expected, actual)

            equal_tree(state["model"], restored.capture())
            equal_tree(state["exposure"], restored._exposure)
            equal_tree(state["exposure_optimizer"], restored.exposure_optimizer.state_dict())
            equal_tree(state["cuda"], torch.cuda.get_rng_state_all())
            del restored
            del state
            for name in ("reference", "reference-repeat", "interrupted"):
                target = root / name / ".gsw-resume"
                target.mkdir(parents=True)
                shutil.copyfile(str(state_path), str(target / state_path.name))
                shutil.copyfile(str(seed / ".gsw-resume/ready.json"), str(target / "ready.json"))
            reference = run("reference", resume=True)
            repeat = run("reference-repeat", resume=True)
            interrupted = run("interrupted", resume=True, pause=True)
            self.assertEqual(read_manifest(interrupted)[0]["iteration"], 2)
            run("interrupted", resume=True)
            final = "point_cloud/iteration_16/point_cloud.ply"
            expected = PlyData.read(str(reference / final), mmap=False)["vertex"]
            repeated = PlyData.read(str(repeat / final), mmap=False)["vertex"]
            actual = PlyData.read(str(interrupted / final), mmap=False)["vertex"]
            self.assertEqual(len(expected), len(actual))
            max_error = 0.0
            for field in expected.data.dtype.names:
                max_error = max(max_error, float(np.max(np.abs(expected[field] - actual[field]))))
                self.assertTrue(np.all(np.isfinite(actual[field])), field)
            expected_exp = json.loads((reference / "exposure.json").read_text())
            actual_exp = json.loads((interrupted / "exposure.json").read_text())
            for name in expected_exp:
                np.testing.assert_allclose(expected_exp[name], actual_exp[name], rtol=1e-3, atol=1e-3)
            # Independent continuous CUDA runs also vary. Test image-quality
            # continuity separately from the exact serialized-state contract.
            reference_psnr = metrics["reference"][-1]["psnr"]
            repeat_psnr = metrics["reference-repeat"][-1]["psnr"]
            resumed_psnr = metrics["interrupted"][-1]["psnr"]
            self.assertLess(abs(resumed_psnr - reference_psnr), 0.05)
            print("CUDA: exact checkpoint/Adam/exposure/RNG restore; {} Gaussians; "
                  "final PSNR continuous/repeat/resume {:.6f}/{:.6f}/{:.6f}; max field delta {:.8g}".format(
                      len(actual), reference_psnr, repeat_psnr, resumed_psnr, max_error))


if __name__ == "__main__":
    unittest.main()
