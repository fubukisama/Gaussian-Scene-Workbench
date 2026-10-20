"""Explicit bounded actual-CUDA camera-Z semantics, not training/accuracy proof.

Run separately in each configured backend Python. This generates no photos,
opens no GUI and reads no user model. Root acceptance owns GPU scheduling.
"""

import argparse
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", required=True, choices=("3dgs", "2dgs"))
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    output = Path(args.report)
    if not output.is_absolute() or output.exists() or not output.parent.is_dir():
        raise ValueError("fresh absolute existing-parent report required")
    if args.backend == "3dgs":
        os.environ["GSW_POSE_POLICY"] = "locked"
        os.environ["GSW_CAMERA_DATASET"] = "owned-camera-z-kernel-probe-no-images"
        from native.worker.camera_rasterizer_runtime import configure_camera_rasterizer
        selected = configure_camera_rasterizer(ROOT)
    else:
        selected = None
    import numpy as np
    import torch
    from native.worker.gs_geometry import _CudaRenderer, _write_json
    from native.backend.gs_geometry import backproject_camera_z, camera_z_from_moments, file_sha256
    from native.worker.canonical_training import pinhole_projection
    module = importlib.import_module("diff_gaussian_rasterization" if args.backend == "3dgs" else "diff_surfel_rasterization")
    assert torch.cuda.is_available()
    width, height = 32, 24
    k = [[26., 0., 14.5], [0., 31., 13.5], [0., 0., 1.]]
    view = {"view_id": "owned-probe", "training_K": k, "training_size": [width, height]}
    actual = {"world_view_transform_float32_transposed": np.eye(4).tolist(),
        "projection_matrix_float32_transposed": np.asarray(pinhole_projection(k, width, height, .2, 100.), dtype=np.float32).T.tolist()}
    renderer = _CudaRenderer.__new__(_CudaRenderer)
    renderer.backend, renderer.torch, renderer.rasterizer = args.backend, torch, module
    renderer.source = {"loaded_cameras": {"owned-probe": actual}}
    if args.backend == "3dgs":
        positions = torch.tensor([[0., 0., 2.], [0., 0., 4.]], device="cuda")
        scales = torch.tensor([[.2, .2, .2], [.4, .4, .4]], device="cuda")
        rotations = torch.tensor([[1., 0., 0., 0.], [1., 0., 0., 0.]], device="cuda")
        opacity = torch.full((2, 1), .5, device="cuda")
    else:
        theta = math.radians(30.)
        positions = torch.tensor([[.1, -.05, 3.]], device="cuda")
        scales = torch.tensor([[.5, .4]], device="cuda")
        rotations = torch.tensor([[math.cos(theta / 2), 0., math.sin(theta / 2), 0.]], device="cuda")
        opacity = torch.full((1, 1), .8, device="cuda")
    renderer.model = SimpleNamespace(get_xyz=positions, get_scaling=scales, get_rotation=rotations, get_opacity=opacity, active_sh_degree=0)
    moments = renderer.render(view)
    decoded = camera_z_from_moments(moments["alpha"], moments["camera_z_moment"],
        alpha_threshold=.01, depth_min=.2, depth_max=100.)
    assert decoded["mask"].any()
    if args.backend == "3dgs":
        # Pixel(u14,v13) is exactly on both means: alpha weights .5,.25.
        assert abs(float(moments["alpha"][13, 14]) - .75) < 2e-6
        assert abs(float(moments["camera_z_moment"][13, 14]) - 2.) < 2e-6
        error = abs(float(decoded["depth"][13, 14]) - 8. / 3.)
        assert error < 2e-6
        target_mask = np.zeros((height, width), dtype=bool); target_mask[13, 14] = True
        point = backproject_camera_z(decoded["depth"], target_mask, k, np.eye(4))
        assert np.max(np.abs(point[0] - [0., 0., 8. / 3.])) < 2e-6
        observation = {"expectedAlpha": .75, "actualAlpha": float(moments["alpha"][13, 14]),
            "expectedCameraZMoment": 2., "actualCameraZMoment": float(moments["camera_z_moment"][13, 14]),
            "expectedCameraZ": 8. / 3., "actualCameraZ": float(decoded["depth"][13, 14]),
            "cameraZAbsoluteError": error, "backprojectedPoint": point[0].tolist(),
            "rawCameraZMomentAboveOnePreserved": True}
    else:
        columns, rows = np.meshgrid(np.arange(width) + .5, np.arange(height) + .5)
        rays = np.stack(((columns - k[0][2]) / k[0][0], (rows - k[1][2]) / k[1][1], np.ones_like(rows)), axis=-1)
        normal = np.asarray([math.sin(theta), 0., math.cos(theta)])
        expected = normal @ np.asarray([.1, -.05, 3.]) / (rays @ normal)
        error = float(np.max(np.abs(decoded["depth"][decoded["mask"]] - expected[decoded["mask"]])))
        assert error < 2e-5, error
        observation = {"expectedDefinition": "independent_ray_plane_camera_z", "normal": normal.tolist(),
            "center": [.1, -.05, 3.], "maximumCameraZAbsoluteError": error,
            "cameraZIsNotRayRange": True}
    result = {"schema": "gsw.gs_geometry_kernel_acceptance/1", "backend": args.backend, "gpuExecuted": True,
        "torch": torch.__version__, "cuda": torch.version.cuda, "gpu": torch.cuda.get_device_name(0),
        "rasterizerWrapperSha256": file_sha256(module.__file__), "rasterizerBinarySha256": file_sha256(module._C.__file__),
        "cameraPinholeProofSha256": selected["proofSha256"] if selected else None,
        "workerSha256": file_sha256(ROOT / "native/worker/gs_geometry.py"),
        "K": k, "imageSize": [width, height], "validPixels": int(decoded["mask"].sum()), "observation": observation,
        "scope": "controlled_actual_kernel_semantics_not_trained_model_or_physical_surface_accuracy",
        "sourcePhotoPixelsRead": False, "userModelRead": False, "applicationLaunched": False}
    _write_json(output, result)
    print(json.dumps(result), flush=True)
    renderer.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
