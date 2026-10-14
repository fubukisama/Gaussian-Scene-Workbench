# SPDX-FileCopyrightText: 2026 fubukisama
# SPDX-License-Identifier: MIT
"""Isolated adapter to the pinned Apache-2.0 Faster-GS Torch extension.

This is not a replacement for the Graphdeco/2DGS rasterizer. The caller must
admit a fixed-density, stock-Adam 3DGS configuration before calling it. No
screen-radius, depth, normal or screen-mean gradient output is fabricated.
"""
import math

UPSTREAM_COMMIT = "c60ea269b7cf3ebda527d640b6fdd31462f91b41"
ADAPTER_VERSION = 1


def render(viewpoint_camera, pc, pipe, bg_color, scaling_modifier=1.0,
           separate_sh=False, override_color=None, use_trained_exp=False):
    import torch
    from FasterGSCudaBackend.torch_bindings import diff_rasterize, RasterizerSettings

    if (override_color is not None or pipe.compute_cov3D_python
            or pipe.convert_SHs_python or pipe.debug):
        raise ValueError("fastergs_configuration_not_supported")
    if not math.isfinite(scaling_modifier) or scaling_modifier <= 0:
        raise ValueError("fastergs_scale_modifier_invalid")
    if not isinstance(pc.active_sh_degree, int) or not 0 <= pc.active_sh_degree <= 3:
        raise ValueError("fastergs_sh_degree_not_supported")
    tensors = (pc._xyz, pc._scaling, pc._rotation, pc._opacity,
               pc._features_dc, pc._features_rest)
    device = pc._xyz.device
    if device.type != "cuda":
        raise ValueError("fastergs_cuda_required")
    if any(tensor.device != device or tensor.dtype != torch.float32 for tensor in tensors):
        raise ValueError("fastergs_float32_parameters_on_same_device_required")
    if torch.cuda.current_stream(device).cuda_stream != torch.cuda.default_stream(device).cuda_stream:
        raise ValueError("fastergs_nondefault_stream_not_supported")
    if bg_color.device != device or bg_color.dtype != torch.float32 or bg_color.numel() != 3:
        raise ValueError("fastergs_background_invalid")
    width, height = int(viewpoint_camera.image_width), int(viewpoint_camera.image_height)
    fov_x, fov_y = float(viewpoint_camera.FoVx), float(viewpoint_camera.FoVy)
    if width <= 0 or height <= 0 or not all(math.isfinite(v) and 0 < v < math.pi
                                         for v in (fov_x, fov_y)):
        raise ValueError("fastergs_camera_invalid")
    near = float(getattr(viewpoint_camera, "znear", 0.01))
    far = float(getattr(viewpoint_camera, "zfar", 10000.0))
    if not math.isfinite(near) or not math.isfinite(far) or near <= 0 or far <= near:
        raise ValueError("fastergs_camera_clip_invalid")
    settings = RasterizerSettings(
        w2c=viewpoint_camera.world_view_transform.transpose(0, 1).contiguous(),
        cam_position=viewpoint_camera.camera_center.contiguous(),
        bg_color=bg_color.contiguous(), active_sh_bases=(pc.active_sh_degree + 1) ** 2,
        width=width, height=height,
        focal_x=width / (2 * math.tan(fov_x / 2)),
        focal_y=height / (2 * math.tan(fov_y / 2)),
        center_x=width / 2, center_y=height / 2,
        near_plane=near, far_plane=far, proper_antialiasing=bool(pipe.antialiasing))
    # This auxiliary output has upstream visibility/gradient semantics, not a
    # projected radius or a substitute for Graphdeco's density control input.
    density_info = torch.zeros((2, pc._xyz.shape[0]), device=device, dtype=torch.float32)
    # Native PLY coordinates may be column-major. Copies are differentiable;
    # never replace the model Parameters or invalidate their Adam state/layout.
    means, scales, rotations, opacities, sh_dc, sh_rest = (
        tensor.contiguous() for tensor in tensors)
    scales = scales if scaling_modifier == 1.0 else scales + math.log(scaling_modifier)
    with torch.cuda.device(device):
        image = diff_rasterize(means, scales, rotations, opacities,
            sh_dc, sh_rest, density_info, settings)
    if use_trained_exp:
        exposure = pc.get_exposure_from_name(viewpoint_camera.image_name)
        image = (image.permute(1, 2, 0) @ exposure[:3, :3]).permute(2, 0, 1)
        image = image + exposure[:3, 3, None, None]
    return {"render": image.clamp(0, 1), "densification_info": density_info,
            "radii": None, "visibility_filter": None, "viewspace_points": None,
            "depth": None, "normal": None, "engine": "fastergs"}
