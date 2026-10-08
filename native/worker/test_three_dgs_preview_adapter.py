"""Run the actual 3DGS loop with CPU tensors and owned CUDA/Win32 boundaries.

These tests cover optimization/observation separation, not CUDA image quality.
The upstream model and CUDA renderer are replaced at their external boundary;
the production training function, preview policy and publishers are real.
"""
import ast
import ctypes
import json
import os
from pathlib import Path
import random
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock


@unittest.skipUnless(sys.platform == "win32", "Shared preview uses Win32 control objects")
class ThreeDgsPreviewAdapterTests(unittest.TestCase):
    def test_preview_copy_and_cleanup_failures_do_not_abort_optimizer(self):
        self.run_training(free_vram=8 * 1024**3, fail_preview_copy=True)

    def test_low_vram_training_completes_without_observation_tensors(self):
        events = self.run_training(free_vram=128 * 1024**2)
        self.assertFalse(any(prefix == "[gsw-training-preview]" and
                             value.get("preview_kind") in ("gaussian_initial", "gaussian_live")
                             for prefix, value in events))

    def run_training(self, free_vram, fail_preview_copy=False):
        try:
            import torch
            import numpy as np
        except ImportError:
            self.skipTest("Run with the configured training Python for tensor observation tests")

        class Model:
            def __init__(self, degree, optimizer):
                self._xyz = torch.nn.Parameter(torch.full((4, 3), .4))
                self._scaling = torch.zeros(4, 3)
                self._features_dc = torch.zeros(4, 1, 3)
                self._opacity = torch.zeros(4, 1)
                self._rotation = torch.tensor([[1., 0., 0., 0.]] * 4)
                self.updates = 0
                models.append(self)

            @property
            def get_xyz(self): return self._xyz
            @property
            def get_scaling(self): return self._scaling.exp()
            @property
            def get_opacity(self): return self._opacity.sigmoid()
            @property
            def get_rotation(self): return self._rotation
            @property
            def get_features_dc(self): return self._features_dc

            def training_setup(self, opt):
                self.optimizer = torch.optim.Adam([self._xyz], lr=.01)
                self.exposure_optimizer = SimpleNamespace(step=lambda: None, zero_grad=lambda **kwargs: None)
                optimizer_step = self.optimizer.step
                def step():
                    self.updates += 1
                    optimizer_step()
                self.optimizer.step = step

            def update_learning_rate(self, iteration): pass

        class Scene:
            def __init__(self, dataset, model, **kwargs):
                self.model_path = dataset.model_path
                self.cameras_extent = 2.
                self.cameras = [SimpleNamespace(image_name="a", colmap_id=1,
                                 alpha_mask=None, depth_reliable=False,
                                 original_image=torch.zeros(4, 3))]
            def getTrainCameras(self): return self.cameras

        def render(camera, model, pipe, background, **kwargs):
            return dict(render=model._xyz, viewspace_points=model._xyz,
                        visibility_filter=torch.ones(4, dtype=torch.bool), radii=torch.ones(4))

        class Progress:
            def __init__(self, *args, **kwargs): pass
            def set_postfix(self, *args): pass
            def update(self, *args): pass
            def close(self): pass

        class DriverFunction:
            def __init__(self, name): self.name = name
            def __call__(self, *args):
                if self.name == "cuMemGetAllocationGranularity":
                    ctypes.cast(args[0], ctypes.POINTER(ctypes.c_size_t))[0] = 2 * 1024**2
                elif self.name in ("cuMemAddressReserve", "cuMemCreate", "cuMemExportToShareableHandle"):
                    ctypes.cast(args[0], ctypes.POINTER(ctypes.c_ulonglong))[0] = 0x100000
                if fail_preview_copy and self.name in ("cuMemcpyDtoDAsync_v2", "cuMemUnmap"):
                    return 999
                return 0

        class DriverDll:
            def __getattr__(self, name): return DriverFunction(name)

        real_windll = ctypes.WinDLL
        def windll(name, *args, **kwargs):
            return DriverDll() if name == "nvcuda.dll" else real_windll(name, *args, **kwargs)

        models, events = [], []
        source = Path(__file__).resolve().parents[2] / "gaussian-splatting" / "train.py"
        module = ast.parse(source.read_text(encoding="utf-8"))
        module.body = [node for node in module.body if isinstance(node, ast.FunctionDef)
                       and node.name == "training"]
        ns = dict(__file__=str(source), torch=torch, np=np, os=os, sys=sys, json=json, time=time,
                  SPARSE_ADAM_AVAILABLE=False, FUSED_SSIM_AVAILABLE=False, GPU_PREVIEW_PROTOCOL_VERSION=2,
                  GaussianModel=Model, Scene=Scene, prepare_output_and_logger=lambda dataset: None,
                  get_expon_lr_func=lambda *args, **kwargs: lambda iteration: 0.,
                  randint=random.randint, tqdm=Progress, render=render,
                  network_gui=SimpleNamespace(conn=None, try_connect=lambda: None),
                  l1_loss=lambda a, b: (a - b).abs().mean(), ssim=lambda a, b: 1 - ((a - b)**2).mean(),
                  psnr=lambda a, b: -10 * torch.log10(((a-b)**2).mean()),
                  training_report=lambda *args: None, emit_gsw_event=lambda *event: events.append(event))
        exec(compile(module, str(source), "exec"), ns)
        opt = SimpleNamespace(iterations=3, optimizer_type="default", lambda_dssim=.2,
                              depth_l1_weight_init=0., depth_l1_weight_final=0.,
                              random_background=False, densify_until_iter=0)
        event = SimpleNamespace(record=lambda *args: None, elapsed_time=lambda other: 1.,
                                query=lambda: True, synchronize=lambda: None)
        original_tensor = torch.tensor
        def tensor(*args, **kwargs):
            if kwargs.get("device") == "cuda": kwargs["device"] = "cpu"
            return original_tensor(*args, **kwargs)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = SimpleNamespace(source_path=str(root / "dataset"), model_path=str(root / "output"),
                                   sh_degree=3, white_background=False, train_test_exp=False)
            control = dict(output=data.model_path, request=str(root / "pause.json"), session="cpu-test", backend="3dgs")
            with mock.patch.dict(os.environ, {"GSW_NATIVE_TRAINING_CONTROL": json.dumps(control)}), \
                 mock.patch.object(torch, "tensor", side_effect=tensor), \
                 mock.patch.object(torch.Tensor, "cuda", lambda self: self), \
                 mock.patch.object(torch.cuda, "Event", return_value=event), \
                 mock.patch.object(torch.cuda, "is_available", return_value=True), \
                 mock.patch.object(torch.cuda, "current_device", return_value=0), \
                 mock.patch.object(torch.cuda, "current_stream", return_value=SimpleNamespace(cuda_stream=0)), \
                 mock.patch.object(torch.cuda, "mem_get_info", return_value=(free_vram, 12 * 1024**3)), \
                 mock.patch.object(torch.cuda, "get_device_name", return_value="CPU CUDA boundary"), \
                 mock.patch.object(ctypes, "WinDLL", side_effect=windll):
                self.assertFalse(ns["training"](data, opt, SimpleNamespace(debug=False), [], [], [], None,
                                                  -1, enable_gpu_preview=True))
            self.assertEqual(models[0].updates, 2)
            self.assertTrue(any(prefix == "[gsw-training-metrics]" and value["iteration"] == 3
                                for prefix, value in events))
        return events


if __name__ == "__main__":
    unittest.main()
