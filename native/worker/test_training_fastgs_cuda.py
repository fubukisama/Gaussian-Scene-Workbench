# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Opt-in real native FastGS fixed-density checkpoint/continuation acceptance.

Run from the isolated FastGS Python with GSW_FASTGS_TEST_ROOT and an actual
GSW_FASTGS_RUNTIME_PROOF. Synthetic photos, outputs and checkpoint copies belong
only to this test. No stock runtime, real project or user checkpoint is edited.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

from native.worker.training_checkpoint import atomic_json, read_manifest, restore_state


@unittest.skipUnless(os.environ.get("GSW_FASTGS_TEST_ROOT"), "Opt-in real FastGS CUDA lifecycle test")
class FastGsNativeLifecycleTests(unittest.TestCase):
    def test_complete_native_checkpoint_keeps_engine_optimizer_sampler_and_exposure(self):
        import numpy as np
        import torch
        from PIL import Image
        from plyfile import PlyData
        self.assertTrue(torch.cuda.is_available(), "Real CUDA is required")
        repository = Path(os.environ["GSW_FASTGS_TEST_ROOT"]).resolve()
        source = repository / "gaussian-splatting"
        self.assertTrue(Path(os.environ["GSW_FASTGS_RUNTIME_PROOF"]).is_file())
        artifact = Path(os.environ.get("GSW_FASTGS_TEST_ARTIFACTS", tempfile.gettempdir()))
        artifact.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="native-fastgs-lifecycle-", dir=str(artifact)) as temporary:
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
                            {"camera_angle_x": .7, "frames": frames if split == "train" else []})
            rng = np.random.RandomState(71)
            points = []
            for x in range(4):
                for y in range(4):
                    for z in range(2):
                        jitter = rng.uniform(-.035, .035, 3)
                        points.append("{} {} {} 0 0 0 180 120 60".format(
                            x * .2 - .3 + jitter[0], y * .2 - .3 + jitter[1], z * .2 + jitter[2]))
            (dataset / "points3d.ply").write_text(
                "ply\nformat ascii 1.0\nelement vertex 32\nproperty float x\nproperty float y\nproperty float z\n"
                "property float nx\nproperty float ny\nproperty float nz\nproperty uchar red\nproperty uchar green\n"
                "property uchar blue\nend_header\n" + "\n".join(points) + "\n", encoding="ascii")

            def input_hashes():
                return {path.relative_to(dataset).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in dataset.rglob("*") if path.is_file()}

            before = input_hashes()
            runs = {}
            def run(name, resume=False, pause=False):
                output = root / name
                request = root / (name + "-pause.json")
                session = uuid.uuid4().hex
                if pause:
                    atomic_json(request, {"session": session})
                elif request.exists():
                    request.unlink()
                env = os.environ.copy()
                control = {"output": str(output), "request": str(request), "session": session,
                           "backend": "3dgs", "resume": resume}
                # Resume reads the stored runtime policy; it must not silently
                # change engines or conflate a PLY warm start with a checkpoint.
                if not resume:
                    control["performance"] = {"enabled": True, "engine": "fastergs", "loss_mode": "stock",
                                              "profile_interval": 1}
                env["GSW_NATIVE_TRAINING_CONTROL"] = json.dumps(control)
                env["PYTHONUTF8"] = "1"
                command = [sys.executable, "-B", "train.py", "-s", str(dataset), "-m", str(output),
                           "--iterations", "16", "-r", "1", "--data_device", "cpu",
                           "--test_iterations", "16", "--save_iterations", "16", "--checkpoint_iterations", "16",
                           "--densify_until_iter", "0", "--disable_viewer", "--optimizer_type", "default",
                           "--random_background", "--train_test_exp"]
                result = subprocess.run(command, cwd=str(source), env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, encoding="utf-8", errors="replace", timeout=90)
                (artifact / ("fastgs-native-" + name + "-" + session + ".log")).write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 75 if pause else 0, result.stdout[-12000:])
                def events(prefix):
                    return [json.loads(line.split(prefix + " ", 1)[1]) for line in result.stdout.splitlines()
                            if prefix + " " in line]
                engines = events("[gsw-training-acceleration]")
                self.assertTrue(engines, result.stdout[-6000:])
                self.assertEqual(engines[-1]["effective"], "fastergs", engines)
                profiles = events("[gsw-training-performance]")
                # The renderer identity in step records independently confirms
                # the admitted engine actually reaches the real training loop.
                self.assertTrue(profiles, result.stdout[-6000:])
                self.assertTrue(all(item["renderer_engine"] == "fastergs" for item in profiles), profiles)
                previews = events("[gsw-training-preview]")
                self.assertTrue(previews, result.stdout[-6000:])
                self.assertTrue(all(Path(item["point_cloud_path"]).is_file() for item in previews))
                runs[name] = events("[gsw-training-metrics]")
                return output

            seed = run("seed", pause=True)
            manifest, state_path = read_manifest(seed)
            state = torch.load(str(state_path))
            self.assertEqual(manifest["iteration"], 1)
            self.assertEqual(manifest["renderer_engine"], "fastergs")
            self.assertEqual(state["renderer_engine"], "fastergs")
            self.assertEqual(state["performance"], manifest["performance"])
            self.assertEqual(state["performance"]["loss_mode"], "stock")
            self.assertEqual(len(state["pending_indices"]), 3)
            self.assertTrue(state["exposure_optimizer"]["state"])
            self.assertTrue(state["model"][-2]["state"])
            sys.path.insert(0, str(source))
            from argparse import ArgumentParser
            from arguments import OptimizationParams
            from scene import GaussianModel
            parser = ArgumentParser()
            params = OptimizationParams(parser)
            opt = params.extract(parser.parse_args(["--iterations", "16", "--optimizer_type", "default"]))
            restored = GaussianModel(3, "default")
            restore_state(torch, np, restored, opt, state, manifest["identity"], state["camera_names"])

            def equal_tree(expected, actual):
                if torch.is_tensor(expected):
                    self.assertTrue(torch.equal(expected, actual))
                elif isinstance(expected, np.ndarray):
                    self.assertTrue(np.array_equal(expected, actual))
                elif isinstance(expected, dict):
                    self.assertEqual(set(expected), set(actual))
                    for key in expected:
                        equal_tree(expected[key], actual[key])
                elif isinstance(expected, (tuple, list)):
                    self.assertEqual(len(expected), len(actual))
                    for left, right in zip(expected, actual):
                        equal_tree(left, right)
                else:
                    self.assertEqual(expected, actual)

            equal_tree(state["model"], restored.capture())
            equal_tree(state["exposure"], restored._exposure)
            equal_tree(state["exposure_optimizer"], restored.exposure_optimizer.state_dict())
            equal_tree(state["random"], random.getstate())
            equal_tree(state["numpy"], np.random.get_state())
            equal_tree(state["torch"], torch.get_rng_state())
            equal_tree(state["cuda"], torch.cuda.get_rng_state_all())
            del restored
            del state
            torch.cuda.empty_cache()
            for name in ("reference", "interrupted"):
                target = root / name / ".gsw-resume"
                target.mkdir(parents=True)
                shutil.copyfile(str(state_path), str(target / state_path.name))
                shutil.copyfile(str(seed / ".gsw-resume/ready.json"), str(target / "ready.json"))
            reference = run("reference", resume=True)
            interrupted = run("interrupted", resume=True, pause=True)
            self.assertEqual(read_manifest(interrupted)[0]["iteration"], 2)
            self.assertEqual(read_manifest(interrupted)[0]["renderer_engine"], "fastergs")
            run("interrupted", resume=True)
            final = "point_cloud/iteration_16/point_cloud.ply"
            expected = PlyData.read(str(reference / final), mmap=False)["vertex"]
            actual = PlyData.read(str(interrupted / final), mmap=False)["vertex"]
            self.assertEqual(len(actual), 32)  # fixed-density capability, not hidden densification
            self.assertEqual(len(expected), len(actual))
            max_error = 0.
            for field in expected.data.dtype.names:
                self.assertTrue(np.all(np.isfinite(actual[field])), field)
                np.testing.assert_allclose(actual[field], expected[field], rtol=1e-4, atol=1e-5)
                max_error = max(max_error, float(np.max(np.abs(expected[field] - actual[field]))))
            expected_exp = json.loads((reference / "exposure.json").read_text())
            actual_exp = json.loads((interrupted / "exposure.json").read_text())
            for name in expected_exp:
                np.testing.assert_allclose(actual_exp[name], expected_exp[name], rtol=1e-4, atol=1e-5)
            self.assertTrue(runs["reference"] and runs["interrupted"])
            psnr_delta = abs(runs["reference"][-1]["psnr"] - runs["interrupted"][-1]["psnr"])
            self.assertLess(psnr_delta, .05)
            self.assertEqual(input_hashes(), before)
            report = {"ready": True, "engine": "fastergs", "scope": "native fixed-density 3DGS lifecycle",
                      "iterations": 16, "gaussians": len(actual), "fullStateRestoreExact": True,
                      "pausedIterations": [1, 2], "sourceInputsUnchanged": True,
                      "finalParameterMaxError": max_error, "finalPsnrDelta": psnr_delta}
            atomic_json(artifact / "fastgs-native-lifecycle-proof.json", report)
            print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    unittest.main()
