# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Optional density criteria; never alter the optimizer's signed gradients.

AbsGS keeps signed cloning and pre-cancellation absolute splitting separate.
Pixel-GS weights by actual accepted compositing pixels and scales only the
screen statistic by dimensionless camera depth. This module does not implement
or claim a surfel equivalent, nor does it replace resource-admission protection.
"""
import math


class DensificationStatistics:
    VERSION = 1

    def __init__(self, torch, strategy, count, device, camera_extent, depth_factor=.37):
        if (strategy not in ('abs_gs', 'pixel_gs') or type(count) is not int or count < 0
                or type(camera_extent) not in (int, float) or not math.isfinite(camera_extent)
                or camera_extent <= 0 or type(depth_factor) not in (int, float)
                or not math.isfinite(depth_factor) or depth_factor <= 0):
            raise ValueError('quality_density_configuration_invalid')
        self.torch, self.strategy, self.device = torch, strategy, device
        self.camera_extent, self.depth_factor = float(camera_extent), float(depth_factor)
        self.reset(count)

    def reset(self, count):
        if type(count) is not int or count < 0:
            raise ValueError('quality_density_topology_invalid')
        self.clone_sum = self.torch.zeros((count, 1), dtype=self.torch.float32, device=self.device)
        self.split_sum = self.torch.zeros_like(self.clone_sum)
        self.denominator = self.torch.zeros((count, 1), dtype=self.torch.int64, device=self.device)

    def accumulate(self, signed_gradient, visibility, *, absolute_screen_gradient=None,
                   contributor_pixels=None, camera_z=None):
        torch, count = self.torch, len(self.clone_sum)
        if signed_gradient is None or signed_gradient.shape != (count, 3):
            raise ValueError('quality_density_signed_gradient_missing')
        if visibility.dtype == torch.bool and visibility.shape == (count,):
            visible = visibility
        elif visibility.dtype in (torch.int32, torch.int64) and visibility.ndim in (1, 2):
            indices = visibility.reshape(-1)
            if bool(((indices < 0) | (indices >= count)).any()):
                raise ValueError('quality_density_visibility_invalid')
            visible = torch.zeros(count, dtype=torch.bool, device=self.device)
            visible[indices] = True
        else:
            raise ValueError('quality_density_visibility_invalid')
        if not bool(torch.isfinite(signed_gradient[visible, :2]).all()):
            raise ValueError('quality_density_gradient_nonfinite')
        with torch.no_grad():
            signed = torch.linalg.vector_norm(signed_gradient.detach()[:, :2], dim=1)
            if self.strategy == 'abs_gs':
                if (absolute_screen_gradient is None or absolute_screen_gradient.shape != (count, 2)
                        or not bool(torch.isfinite(absolute_screen_gradient).all())
                        or bool((absolute_screen_gradient < 0).any())):
                    raise ValueError('quality_density_absolute_statistic_missing')
                split = torch.linalg.vector_norm(absolute_screen_gradient, dim=1)
                weight = visible.to(torch.int64)
            else:
                if (contributor_pixels is None or contributor_pixels.shape != (count,)
                        or contributor_pixels.dtype != torch.int64 or bool((contributor_pixels < 0).any())):
                    raise ValueError('quality_density_contributor_statistic_missing')
                if camera_z is None or camera_z.shape != (count,):
                    raise ValueError('quality_density_camera_depth_missing')
                accepted = contributor_pixels > 0
                if not bool(torch.isfinite(camera_z[accepted]).all()) or bool((camera_z[accepted] <= 0).any()):
                    raise ValueError('quality_density_camera_depth_invalid')
                scale = (camera_z.detach() / (self.depth_factor * self.camera_extent)).square().clamp(max=1)
                scale = torch.where(accepted, scale, torch.zeros_like(scale))
                signed = signed * scale
                split, weight = signed, contributor_pixels
                # A positive contributor count is a stronger predicate than a
                # projected radius; a contradictory ABI must fail, not guess.
                if bool((accepted & ~visible).any()):
                    raise ValueError('quality_density_contributor_visibility_mismatch')
            maximum = torch.iinfo(torch.int64).max
            if bool((self.denominator[:, 0] > maximum - weight).any()):
                raise ValueError('quality_density_count_overflow')
            self.clone_sum[:, 0] += torch.where(weight > 0, signed, torch.zeros_like(signed)) * weight
            self.split_sum[:, 0] += torch.where(weight > 0, split, torch.zeros_like(split)) * weight
            self.denominator[:, 0] += weight

    def gradients(self):
        divisor = self.denominator.clamp(min=1).to(self.clone_sum.dtype)
        return self.clone_sum / divisor, self.split_sum / divisor

    def state_dict(self):
        return {'version': self.VERSION, 'strategy': self.strategy,
                'camera_extent': self.camera_extent, 'depth_factor': self.depth_factor,
                'clone_sum': self.clone_sum.clone(), 'split_sum': self.split_sum.clone(),
                'denominator': self.denominator.clone()}

    def load_state_dict(self, state, count):
        torch = self.torch
        if (not isinstance(state, dict) or state.get('version') != self.VERSION
                or state.get('strategy') != self.strategy or state.get('camera_extent') != self.camera_extent
                or state.get('depth_factor') != self.depth_factor):
            raise ValueError('quality_density_checkpoint_invalid')
        values = []
        for key, dtype in (('clone_sum', torch.float32), ('split_sum', torch.float32), ('denominator', torch.int64)):
            value = state.get(key)
            if (not isinstance(value, torch.Tensor) or value.shape != (count, 1) or value.dtype != dtype
                    or not bool(torch.isfinite(value).all()) or bool((value < 0).any())):
                raise ValueError('quality_density_checkpoint_invalid')
            values.append(value.to(self.device).detach().clone())
        self.clone_sum, self.split_sum, self.denominator = values
