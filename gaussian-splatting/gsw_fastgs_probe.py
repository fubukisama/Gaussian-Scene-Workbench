# SPDX-FileCopyrightText: 2026 fubukisama
# SPDX-License-Identifier: MIT
"""Bounded real CUDA render/backward/Adam-state probe on test-owned tensors."""
import argparse
import copy
import io
import json
import math
import hashlib
import sys
from pathlib import Path
from types import SimpleNamespace


def run_probe():
    import torch
    import FasterGSCudaBackend._C as extension
    from gsw_fastgs_adapter import render, UPSTREAM_COMMIT, ADAPTER_VERSION

    class Model:
        def __init__(self, degree=0):
            self._xyz = torch.nn.Parameter(torch.tensor([[0., 0., 2.], [.3, .1, 2.4]], device="cuda"))
            self._scaling = torch.nn.Parameter(torch.full((2, 3), -2., device="cuda"))
            self._rotation = torch.nn.Parameter(torch.tensor([[1., 0., 0., 0.]] * 2, device="cuda"))
            self._opacity = torch.nn.Parameter(torch.full((2, 1), -.3, device="cuda"))
            self._features_dc = torch.nn.Parameter(torch.full((2, 1, 3), .1, device="cuda"))
            self._features_rest = torch.nn.Parameter(torch.zeros((2, 15, 3), device="cuda"))
            self.active_sh_degree = degree
            self._exposure = torch.nn.Parameter(torch.cat((torch.eye(3, device="cuda"), torch.zeros((3, 1), device="cuda")), 1))

        def parameters(self):
            return [self._xyz, self._scaling, self._rotation, self._opacity,
                    self._features_dc, self._features_rest, self._exposure]

        def get_exposure_from_name(self, _name):
            return self._exposure

    camera = SimpleNamespace(image_width=32, image_height=32, FoVx=math.pi/2,
        FoVy=math.pi/2, world_view_transform=torch.eye(4, device="cuda"),
        camera_center=torch.zeros(3, device="cuda"), znear=.01, zfar=100., image_name="probe")
    pipe = SimpleNamespace(compute_cov3D_python=False, convert_SHs_python=False,
                           debug=False, antialiasing=False)
    background = torch.zeros(3, device="cuda")
    report = {"ready": False, "engine": "fastergs", "upstreamCommit": UPSTREAM_COMMIT,
              "adapterVersion": ADAPTER_VERSION, "torch": torch.__version__,
              "cuda": torch.version.cuda, "device": torch.cuda.get_device_name(0),
              "extension": str(Path(extension.__file__).resolve()), "checks": {},
              "extensionSha256": hashlib.sha256(Path(extension.__file__).read_bytes()).hexdigest(),
              "pythonMajorMinor": "{}.{}".format(*sys.version_info[:2]),
              "sameEngineAdamStateRoundtrip": False}
    max_gradient = 0.
    for degree in (0, 1, 2, 3):
        for aa in (False, True):
            model = Model(degree)
            pipe.antialiasing = aa
            output = render(camera, model, pipe, background, use_trained_exp=True)
            image = output["render"]
            if tuple(image.shape) != (3, 32, 32) or not bool(torch.isfinite(image).all()) or float(image.max()) <= 0:
                raise RuntimeError("fastergs_render_probe_failed")
            image.square().mean().backward()
            torch.cuda.synchronize()
            for parameter in model.parameters():
                if parameter.grad is not None and parameter.grad.numel():
                    if not bool(torch.isfinite(parameter.grad).all()):
                        raise RuntimeError("fastergs_backward_probe_failed")
                    max_gradient = max(max_gradient, float(parameter.grad.abs().max()))
    if max_gradient <= 0:
        raise RuntimeError("fastergs_backward_zero_gradient")
    report["checks"].update(render=True, backward=True, sh0to3=True,
                             antialiasing=True, exposureBackward=True)
    pipe.antialiasing = False
    target = torch.zeros((3, 32, 32), device="cuda")
    target[:, 13:19, 13:19] = .3

    def run_steps(model, optimizer, count):
        losses = []
        for _ in range(count):
            optimizer.zero_grad(set_to_none=True)
            loss = (render(camera, model, pipe, background)["render"] - target).square().mean()
            loss.backward()
            optimizer.step()
            torch.cuda.synchronize()
            losses.append(float(loss.detach()))
        return losses

    initial = Model()
    initial_parameters = [parameter.detach().clone() for parameter in initial.parameters()]
    optimizer = torch.optim.Adam(initial.parameters(), lr=.02)
    first_losses = run_steps(initial, optimizer, 8)
    checkpoint = {"parameters": [parameter.detach().clone() for parameter in initial.parameters()],
                  "optimizer": copy.deepcopy(optimizer.state_dict())}
    serialized = io.BytesIO()
    torch.save(checkpoint, serialized)
    serialized.seek(0)
    checkpoint = torch.load(serialized)
    resumed = Model()
    with torch.no_grad():
        for destination, value in zip(resumed.parameters(), checkpoint["parameters"]):
            destination.copy_(value)
    resumed_optimizer = torch.optim.Adam(resumed.parameters(), lr=.02)
    resumed_optimizer.load_state_dict(checkpoint["optimizer"])
    continued_losses = run_steps(initial, optimizer, 8)
    resumed_losses = run_steps(resumed, resumed_optimizer, 8)
    max_state_error = max(float((left.detach()-right.detach()).abs().max())
                          for left, right in zip(initial.parameters(), resumed.parameters()))
    if max_state_error > 1e-6 or abs(continued_losses[-1]-resumed_losses[-1]) > 1e-8:
        raise RuntimeError("fastergs_same_engine_adam_state_roundtrip_failed")
    if continued_losses[-1] >= first_losses[0]:
        raise RuntimeError("fastergs_training_loss_did_not_improve")
    if all(torch.equal(before, after.detach()) for before, after in zip(initial_parameters, initial.parameters())):
        raise RuntimeError("fastergs_optimizer_did_not_update_parameters")
    report["checks"]["train"] = True
    report["sameEngineAdamStateRoundtrip"] = True
    report.update(ready=True, firstLoss=first_losses[0], finalLoss=continued_losses[-1],
                  stateMaxError=max_state_error, steps=16,
                  scope="tiny fixed-density stock-Adam CUDA probe; not full GSW lifecycle acceptance")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = run_probe()
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
