# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Independent implementation of the Mip-Splatting filtering equations.

This is not the author's renderer. The camera sampling calculation uses the
largest singular value of the full pinhole Jacobian, including skew, and only
training views. A stored filter is part of model state, not an evaluation-view
dependent effect. Product admission additionally requires renderer/export gates.
"""
import math


class MipError(ValueError):
    pass


def _positive(value):
    return type(value) in (int, float) and math.isfinite(value) and value > 0


def training_filter(torch, xyz, cameras, pixel_variance=.2, margin=.15):
    """Return immutable N x 1 world-space widths and explicit coverage evidence.

    Camera entries contain K, w2c, width, height, znear and split. Intrinsics are
    already in pixel-border coordinates. No implicit resize, unit conversion,
    camera optimization or validation/test-camera use is allowed.
    """
    if (xyz.ndim != 2 or xyz.shape[1] != 3 or not len(xyz)
            or not bool(torch.isfinite(xyz).all()) or not _positive(pixel_variance)
            or not _positive(margin + 1) or margin < 0 or margin > 1 or not cameras):
        raise MipError("quality_mip_input_invalid")
    widths = xyz.new_full((len(xyz),), float('inf'))
    with torch.no_grad():
        for camera in cameras:
            if camera.get('split') != 'train':
                raise MipError('quality_mip_training_views_only')
            k, w2c = (torch.as_tensor(camera[name], device=xyz.device, dtype=xyz.dtype)
                      for name in ('K', 'w2c'))
            width, height, near = camera['width'], camera['height'], camera['znear']
            if (k.shape != (3, 3) or w2c.shape != (4, 4)
                    or not bool(torch.isfinite(k).all() and torch.isfinite(w2c).all())
                    or not bool(torch.allclose(k[2], k.new_tensor([0, 0, 1])))
                    or k[0, 0] <= 0 or k[1, 1] <= 0
                    or type(width) is not int or type(height) is not int
                    or width <= 0 or height <= 0 or not _positive(near)):
                raise MipError('quality_mip_camera_invalid')
            point = xyz.detach() @ w2c[:3, :3].T + w2c[:3, 3]
            z = point[:, 2]
            positive = z > near
            safe_z = torch.where(positive, z, torch.ones_like(z))
            projected = point @ k.T
            uv = projected[:, :2] / safe_z[:, None]
            valid = (positive & (uv[:, 0] >= -margin * width)
                     & (uv[:, 0] <= (1 + margin) * width)
                     & (uv[:, 1] >= -margin * height)
                     & (uv[:, 1] <= (1 + margin) * height))
            if not bool(valid.any()):
                continue
            p, zv = point[valid], z[valid]
            # d(K p / Z)/dp: principal point cancels analytically; skew does not.
            numerator = p @ k[:2].T
            jacobian = k[:2][None].expand(len(p), -1, -1) / zv[:, None, None]
            jacobian = jacobian.clone()
            jacobian[:, :, 2] -= numerator / zv[:, None].square()
            frequency = torch.linalg.svdvals(jacobian)[:, 0]
            candidate = math.sqrt(pixel_variance) / frequency
            widths[valid] = torch.minimum(widths[valid], candidate)
        visible = torch.isfinite(widths)
        if not bool(visible.any()):
            raise MipError('quality_mip_no_visible_training_points')
        # Unseen points inherit the coarsest observed constraint, not zero blur.
        fallback = widths[visible].max()
        widths[~visible] = fallback
    return widths[:, None].detach(), {'visible': int(visible.sum().item()),
        'unseen_fallback': int((~visible).sum().item()), 'pixel_variance': float(pixel_variance),
        'sampling': 'full_pinhole_max_singular_frequency_v1'}


def apply_filter(torch, raw_scales, raw_opacity, widths):
    """Differentiable 3D convolution with determinant opacity compensation."""
    if (raw_scales.ndim != 2 or raw_scales.shape[1] != 3
            or raw_opacity.shape != (len(raw_scales), 1) or widths.shape != raw_opacity.shape
            or not bool(torch.isfinite(raw_scales).all() and torch.isfinite(raw_opacity).all()
                        and torch.isfinite(widths).all())
            or bool((raw_scales <= 0).any() or (widths < 0).any()
                    or (raw_opacity < 0).any() or (raw_opacity > 1).any())):
        raise MipError('quality_mip_attributes_invalid')
    active_scales = torch.sqrt(raw_scales.square() + widths.square())
    coefficient = torch.exp((raw_scales.log() - active_scales.log()).sum(dim=1, keepdim=True))
    return active_scales, raw_opacity * coefficient


def filter_2d(torch, covariance, opacity, pixel_variance=.3):
    """Reference 2D Mip convolution; not a substitute for real renderer proof."""
    if covariance.shape[-2:] != (2, 2) or not _positive(pixel_variance):
        raise MipError('quality_mip_covariance_invalid')
    filtered = covariance + torch.eye(2, dtype=covariance.dtype, device=covariance.device) * pixel_variance
    determinant = torch.linalg.det(covariance)
    if bool((determinant <= 0).any()) or not bool(torch.isfinite(covariance).all()):
        raise MipError('quality_mip_covariance_invalid')
    compensation = torch.sqrt(determinant / torch.linalg.det(filtered))
    return filtered, opacity * compensation


class MipState:
    VERSION = 1

    def __init__(self, torch, pixel_variance=.2, margin=.15):
        self.torch, self.pixel_variance, self.margin = torch, pixel_variance, margin
        self.widths, self.report, self.iteration = None, None, None

    def update(self, xyz, cameras, iteration):
        widths, report = training_filter(self.torch, xyz, cameras, self.pixel_variance, self.margin)
        self.widths, self.report, self.iteration = widths, report, iteration
        return report

    def active(self, scales, opacity):
        if self.widths is None:
            raise MipError('quality_mip_filter_state_missing')
        return apply_filter(self.torch, scales, opacity, self.widths)

    def state_dict(self):
        if self.widths is None:
            raise MipError('quality_mip_filter_state_missing')
        return {'version': self.VERSION, 'pixel_variance': self.pixel_variance,
                'margin': self.margin, 'iteration': self.iteration,
                'widths': self.widths.detach().clone(), 'report': dict(self.report)}

    def load_state_dict(self, state, xyz):
        widths = state.get('widths')
        if (state.get('version') != self.VERSION or state.get('pixel_variance') != self.pixel_variance
                or state.get('margin') != self.margin or widths is None
                or widths.shape != (len(xyz), 1) or not bool(self.torch.isfinite(widths).all())
                or bool((widths <= 0).any()) or type(state.get('iteration')) is not int
                or not isinstance(state.get('report'), dict)):
            raise MipError('quality_mip_checkpoint_invalid')
        self.widths = widths.to(xyz).detach().clone()
        self.report, self.iteration = dict(state['report']), state['iteration']
