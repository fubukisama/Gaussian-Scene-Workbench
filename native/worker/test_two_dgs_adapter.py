"""Exercise the actual adapter loop with a CPU differentiable test renderer.

This verifies control/state/preview semantics, NOT surfel CUDA image quality.
No optional external 2DGS imports are needed; import-free function extraction
lets the exact production training loop run against tiny owned fixtures.
"""
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from native.worker import training_checkpoint as cp
from native.worker.training_preview import TrainingPreviewPublisher
from native.worker.training_density_control import NativeDensityControl
from native.worker.training_summary import emit_loaded_training_summary


class TwoDgsAdapterTests(unittest.TestCase):
    def test_pause_resume_preserves_surfel_adam_sampler_and_previews(self):
        try:
            import torch
            import numpy as np
        except ImportError:
            self.skipTest("Run with training Python to verify tensor state")

        class Model:
            def __init__(self, degree):
                self._xyz = torch.nn.Parameter(torch.rand(4, 3))
                self._scaling = torch.nn.Parameter(torch.zeros(4, 2))
                self._features_dc = torch.zeros(4, 1, 3)
                self._opacity = torch.zeros(4, 1)
                self._rotation = torch.tensor([[1., 0., 0., 0.]] * 4)
                self.max_radii2D = torch.zeros(4)
                self.accumulator = torch.zeros(4)
                self.xyz_gradient_accum = torch.zeros(4, 1)
                self.denom = torch.ones(4, 1)
                self.active_sh_degree = 0

            @property
            def get_xyz(self):
                return self._xyz

            @property
            def get_scaling(self):
                return self._scaling.exp()

            @property
            def get_opacity(self):
                return self._opacity.sigmoid()

            def training_setup(self, opt):
                self.optimizer = torch.optim.Adam([self._xyz, self._scaling], lr=.01)

            def capture(self):
                return (self._xyz, self._scaling, self.optimizer.state_dict(),
                        self.accumulator, self.max_radii2D, self.active_sh_degree)

            def restore(self, state, opt):
                (self._xyz, self._scaling, optimizer, self.accumulator,
                 self.max_radii2D, self.active_sh_degree) = state
                self.training_setup(opt)
                self.optimizer.load_state_dict(optimizer)

            def update_learning_rate(self, iteration):
                for group in self.optimizer.param_groups:
                    group["lr"] = .01 / (1 + iteration)

            def oneupSHdegree(self):
                self.active_sh_degree += 1

            def add_densification_stats(self, points, visible):
                self.accumulator[visible] += points.grad[visible].abs().sum(dim=1)

            def densify_and_clone(self, *args):
                self.accumulator *= .5

            def densify_and_split(self, *args):
                pass

            def prune_points(self, mask):
                if mask.any():
                    raise AssertionError("The state fixture must not be pruned")

            def reset_opacity(self):
                self._opacity.zero_()

        class Scene:
            def __init__(self, dataset, model):
                self.gaussians = model
                self.model_path = dataset.model_path
                self.cameras_extent = 2.
                self.cameras = [SimpleNamespace(image_name=str(i), colmap_id=i,
                                original_image=torch.full((4, 3), i * .1)) for i in range(4)]
                random.shuffle(self.cameras)
                models[self.model_path] = model

            def getTrainCameras(self):
                return self.cameras

            def save(self, iteration):
                path = Path(self.model_path) / "point_cloud" / ("iteration_{}".format(iteration)) / "point_cloud.ply"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("surfel source fixture: exactly two scales", encoding="ascii")

        def render(camera, model, pipe, background):
            # All RNG families influence subsequent updates. The renderer is a
            # deterministic differentiable test double, not the CUDA rasterizer.
            image = model._xyz * (1 + model._scaling.mean()) + torch.rand(1) * .01 + np.random.rand() * .01
            return dict(render=image, viewspace_points=model._xyz,
                        visibility_filter=torch.ones(4, dtype=torch.bool), radii=torch.ones(4),
                        rend_dist=image * .1, rend_normal=image, surf_normal=image * .5)

        class Progress:
            def __init__(self, *args, **kwargs): pass
            def set_postfix(self, *args): pass
            def update(self, *args): pass
            def close(self): pass

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            for name in ("scene/gaussian_model.py", "scene/__init__.py", "scene/dataset_readers.py",
                         "arguments/__init__.py", "gaussian_renderer/__init__.py"):
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture", encoding="ascii")
            dataset = root / "dataset"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images/a.png").write_bytes(b"input fingerprint")
            module = ast.parse(Path(__file__).with_name("two_dgs_train.py").read_text(encoding="utf-8"))
            module.body = [node for node in module.body if isinstance(node, ast.FunctionDef)
                           and node.name in ("training", "emit_checkpoint_preview")]
            models, events = {}, []
            ns = dict(torch=torch, np=np, os=os, json=json, time=time, hashlib=hashlib, Path=Path,
                      source_root=source, randint=random.randint, GaussianModel=Model, Scene=Scene,
                      prepare_output_and_logger=lambda args: None, TrainingCheckpoint=cp.TrainingCheckpoint,
                      training_identity=cp.training_identity, capture_state=cp.capture_state,
                      restore_state=cp.restore_state, file_digest=cp.file_digest,
                      TrainingPreviewPublisher=TrainingPreviewPublisher, tqdm=Progress, render=render,
                      NativeDensityControl=NativeDensityControl,
                      emit_loaded_training_summary=emit_loaded_training_summary,
                      l1_loss=lambda a, b: (a - b).abs().mean(), ssim=lambda a, b: 1 - ((a - b)**2).mean(),
                      psnr=lambda a, b: -10 * torch.log10(((a-b)**2).mean()),
                      training_report=lambda *args: None, emit_gsw_event=lambda *event: events.append(event))
            exec(compile(module, "two_dgs_train.py", "exec"), ns)
            opt = SimpleNamespace(iterations=12, lambda_dssim=.2, lambda_normal=.05, lambda_dist=100,
                                  densify_until_iter=10, densify_from_iter=0, densification_interval=2,
                                  opacity_reset_interval=4, densify_grad_threshold=.1, opacity_cull=.005)
            original_tensor = torch.tensor
            def tensor(*args, **kwargs):
                if kwargs.get("device") == "cuda": kwargs["device"] = "cpu"
                return original_tensor(*args, **kwargs)
            event = SimpleNamespace(record=lambda: None, elapsed_time=lambda other: 1.)

            def run(name, pause=False, resume=False):
                random.seed(999 if resume else 42)
                np.random.seed(999 if resume else 42)
                torch.manual_seed(999 if resume else 42)
                output = root / name
                request = root / (name + "-pause.json")
                control = dict(output=str(output), request=str(request), session="test", backend="2dgs", resume=resume)
                if pause: cp.atomic_json(request, {"session": "test"})
                elif request.exists(): request.unlink()
                data = SimpleNamespace(source_path=str(dataset), model_path=str(output), white_background=False, sh_degree=3)
                with mock.patch.dict(os.environ, {cp.CONTROL_ENV: json.dumps(control)}), \
                     mock.patch.object(torch, "tensor", side_effect=tensor), \
                     mock.patch.object(torch.Tensor, "cuda", lambda self: self), \
                     mock.patch.object(torch.cuda, "Event", return_value=event), \
                     mock.patch.object(torch.cuda, "synchronize"):
                    self.assertEqual(ns["training"](data, opt, SimpleNamespace(debug=False), [], [12], [], None), pause)
                return models[str(output)]

            reference = run("reference")
            run("resumed", pause=True)
            manifest, state_path = cp.read_manifest(root / "resumed")
            self.assertEqual(manifest["backend"], "2dgs")
            state = torch.load(str(state_path))
            self.assertEqual(state["iteration"], 1)
            self.assertNotIn("exposure", state)
            self.assertEqual(len(state["pending_indices"]), 3)
            self.assertEqual(state["model"][1].shape[1], 2)
            self.assertTrue(state["model"][2]["state"])
            saved = copy.deepcopy(state["model"])
            restored = Model(3)
            cp.restore_state(torch, np, restored, opt, state, manifest["identity"], state["camera_names"], "2dgs")
            self.assertTrue(torch.equal(restored._xyz, saved[0]))
            self.assertTrue(torch.equal(restored._scaling, saved[1]))
            resumed = run("resumed", resume=True)
            self.assertTrue(torch.equal(reference._xyz, resumed._xyz))
            self.assertTrue(torch.equal(reference._scaling, resumed._scaling))
            self.assertTrue(torch.equal(reference.accumulator, resumed.accumulator))
            for key, expected in reference.optimizer.state_dict()["state"].items():
                for field, value in expected.items():
                    self.assertTrue(torch.equal(value, resumed.optimizer.state_dict()["state"][key][field]))
            previews = [value for prefix, value in events if prefix == "[gsw-training-preview]"]
            self.assertTrue(any(value.get("preview_kind") == "gaussian_initial" for value in previews))
            self.assertTrue(any(value["iteration"] == 12 for value in previews))
            # Snapshot keeps the two-scale source contract and finite attributes.
            preview = next(Path(value["point_cloud_path"]) for value in previews
                           if value.get("preview_kind") == "gaussian_initial")
            header, payload = preview.read_bytes().split(b"end_header\n", 1)
            self.assertIn(b"property float scale_1", header)
            self.assertNotIn(b"property float scale_2", header)
            rows = np.frombuffer(payload, dtype="<f4").reshape(-1, 13)
            self.assertTrue(np.isfinite(rows).all())
            self.assertEqual(rows.shape[1], 13)
            self.assertEqual(resumed._scaling.shape[1], 2)


if __name__ == "__main__":
    unittest.main()
