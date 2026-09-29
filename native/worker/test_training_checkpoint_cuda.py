"""Opt-in real CUDA/Graphdeco and surfel pause/resume integration test.

Run with the configured training Python, its DLL PATH and TEMP on a data drive:
    python -m unittest native.worker.test_training_checkpoint_cuda
Only test-owned synthetic data and subprocesses are used. No user models touched.
Set GSW_CHECKPOINT_TEST_BACKEND=2dgs to test the separate surfel runtime.
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
        backend = os.environ.get("GSW_CHECKPOINT_TEST_BACKEND", "3dgs")
        self.assertIn(backend, ("3dgs", "2dgs"))
        iterations = int(os.environ.get("GSW_CHECKPOINT_TEST_ITERATIONS", "16"))
        self.assertGreaterEqual(iterations, 16)
        source = (Path(os.environ["TWO_DGS_DIR"]) if backend == "2dgs"
                  else repository / "gaussian-splatting")
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

            if backend == "2dgs":
                from native.worker.training_preflight import probe_training_environment
                report = probe_training_environment(repository, dataset, backend=backend)
                self.assertTrue(report["ready"], report)
                self.assertEqual(Path(report["python"]).resolve(), Path(sys.executable).resolve())

            def run(name, resume=False, pause=False):
                output = root / name
                request = root / (name + "-pause.json")
                session = uuid.uuid4().hex
                if pause:
                    atomic_json(request, {"session": session})
                env = os.environ.copy()
                env["GSW_NATIVE_TRAINING_CONTROL"] = json.dumps({
                    "output": str(output), "request": str(request), "session": session,
                    "backend": backend, "resume": resume})
                env["GSW_TWO_DGS_SOURCE"] = str(source)
                env["PYTHONUTF8"] = "1"
                entry = str(repository / "native/worker/two_dgs_train.py") if backend == "2dgs" else "train.py"
                command = [sys.executable, entry, "-s", str(dataset), "-m", str(output),
                           "--iterations", str(iterations), "-r", "1", "--data_device", "cpu",
                           "--test_iterations", str(iterations), "--save_iterations", str(iterations), "--checkpoint_iterations", str(iterations),
                           "--densify_from_iter", "0", "--densify_until_iter", "14",
                           "--densification_interval", "2", "--densify_grad_threshold", "1000",
                           "--opacity_reset_interval", "8"]
                command += (["--opacity_cull", "0.001"] if backend == "2dgs" else [
                    "--disable_viewer", "--optimizer_type", "default", "--random_background", "--train_test_exp"])
                result = subprocess.run(command, cwd=str(source), env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        encoding="utf-8", errors="replace", timeout=300)
                self.assertEqual(result.returncode, 75 if pause else 0, result.stdout[-12000:])
                metrics[name] = [json.loads(line.split("[gsw-training-metrics] ", 1)[1])
                                 for line in result.stdout.splitlines() if "[gsw-training-metrics] " in line]
                previews = [json.loads(line.split("[gsw-training-preview] ", 1)[1])
                            for line in result.stdout.splitlines() if "[gsw-training-preview] " in line]
                self.assertTrue(previews, result.stdout[-4000:])
                self.assertTrue(all(Path(item["point_cloud_path"]).is_file() for item in previews[-4:]))
                if backend == "2dgs":
                    self.assertTrue(any(item.get("preview_kind") == "gaussian_initial" for item in previews), result.stdout[-12000:])
                    self.assertFalse((output / "exposure.json").exists())
                    if iterations >= 1000 and not pause:
                        self.assertTrue(any(item.get("preview_kind") == "gaussian_live" for item in previews), result.stdout[-4000:])
                return output

            metrics = {}
            # Both branches start from the SAME tensor state. Independent CUDA
            # KNN/rasterizer initializations are not a deterministic baseline.
            seed = run("seed", pause=True)
            manifest, state_path = read_manifest(seed)
            self.assertEqual(manifest["iteration"], 1)
            self.assertEqual(manifest["backend"], backend)
            state = torch.load(str(state_path))
            self.assertEqual(len(state["pending_indices"]), 3)
            if backend == "3dgs":
                self.assertTrue(state["exposure_optimizer"]["state"])
            else:
                self.assertNotIn("exposure_optimizer", state)
                self.assertEqual(state["model"][4].shape[1], 2)
                self.assertIn("ema_normal", state)
            self.assertGreater(float(state["model"][8].sum()), 0)  # densification gradient accumulator
            # Verify every saved CUDA tensor and optimizer slot exactly, before
            # performing more potentially non-deterministic rasterizer updates.
            sys.path.insert(0, str(source))
            from argparse import ArgumentParser
            from arguments import OptimizationParams
            from scene import GaussianModel
            from native.worker.training_checkpoint import restore_state
            parser = ArgumentParser()
            params = OptimizationParams(parser)
            options = ["--iterations", str(iterations)] + (["--optimizer_type", "default"] if backend == "3dgs" else [])
            opt = params.extract(parser.parse_args(options))
            restored = GaussianModel(3, "default") if backend == "3dgs" else GaussianModel(3)
            restore_state(torch, np, restored, opt, state, manifest["identity"], state["camera_names"], backend=backend)

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
            if backend == "3dgs":
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
            final = "point_cloud/iteration_{}/point_cloud.ply".format(iterations)
            expected = PlyData.read(str(reference / final), mmap=False)["vertex"]
            repeated = PlyData.read(str(repeat / final), mmap=False)["vertex"]
            actual = PlyData.read(str(interrupted / final), mmap=False)["vertex"]
            self.assertEqual(len(expected), len(actual))
            self.assertGreater(len(actual), 0)
            if backend == "2dgs":
                self.assertIn("scale_1", actual.data.dtype.names)
                self.assertNotIn("scale_2", actual.data.dtype.names)
            max_error = 0.0
            for field in expected.data.dtype.names:
                max_error = max(max_error, float(np.max(np.abs(expected[field] - actual[field]))))
                self.assertTrue(np.all(np.isfinite(actual[field])), field)
            if backend == "3dgs":
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
            print(backend + " CUDA: exact checkpoint/Adam/RNG restore; {} Gaussians; "
                  "final PSNR continuous/repeat/resume {:.6f}/{:.6f}/{:.6f}; max field delta {:.8g}".format(
                      len(actual), reference_psnr, repeat_psnr, resumed_psnr, max_error))


if __name__ == "__main__":
    unittest.main()
