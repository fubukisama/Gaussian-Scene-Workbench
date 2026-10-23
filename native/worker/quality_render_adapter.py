# Copyright (C) 2023, Inria, GRAPHDECO research group.
# Adapted from gaussian-splatting/gaussian_renderer/__init__.py under
# gaussian-splatting/LICENSE.md (research/evaluation, not MIT or GPL).
"""Explicit quality-kernel renderer; never monkey-patch the stock renderer."""
import math


class QualityRenderAdapter:
    def __init__(self, torch, module, *, absgrad=False, contributor_pixels=False,
                 alpha=False, mip=None):
        self.torch, self.module = torch, module
        self.absgrad, self.contributor_pixels, self.alpha = absgrad, contributor_pixels, alpha
        self.mip = mip

    def __call__(self, camera, model, pipe, background, scaling_modifier=1.,
                 separate_sh=False, override_color=None, use_trained_exp=False):
        torch, module = self.torch, self.module
        screen = torch.zeros_like(model.get_xyz, requires_grad=True) + 0
        if screen.requires_grad:
            screen.retain_grad()
        settings = module.GaussianRasterizationSettings(
            image_height=int(camera.image_height), image_width=int(camera.image_width),
            tanfovx=math.tan(camera.FoVx * .5), tanfovy=math.tan(camera.FoVy * .5),
            bg=background, scale_modifier=scaling_modifier,
            viewmatrix=camera.world_view_transform, projmatrix=camera.full_proj_transform,
            sh_degree=model.active_sh_degree, campos=camera.camera_center,
            prefiltered=False, debug=pipe.debug,
            antialiasing=True if self.mip is not None else pipe.antialiasing,
            enable_absgrad=self.absgrad, enable_contributor_pixels=self.contributor_pixels,
            enable_alpha=self.alpha)
        scales, opacity = model.get_scaling, model.get_opacity
        if self.mip is not None:
            scales, opacity = self.mip.active(scales, opacity)
        covariance = None
        rotations = model.get_rotation
        if pipe.compute_cov3D_python:
            covariance = model.covariance_activation(scales, scaling_modifier, model._rotation)
            scales, rotations = None, None
        colors, sh, dc = override_color, None, None
        if override_color is None:
            if pipe.convert_SHs_python:
                from utils.sh_utils import eval_sh
                features = model.get_features.transpose(1, 2).view(-1, 3, (model.max_sh_degree + 1) ** 2)
                direction = model.get_xyz - camera.camera_center
                direction = direction / direction.norm(dim=1, keepdim=True)
                colors = torch.clamp_min(eval_sh(model.active_sh_degree, features, direction) + .5, 0.)
            else:
                # Quality ABI1 (like its current accelerated stock ancestor)
                # has a mandatory separate DC pointer. A concatenated SH tensor
                # with an empty DC would be an invalid CUDA memory access.
                dc, sh = model.get_features_dc, model.get_features_rest
        result = module.GaussianRasterizer(settings)(means3D=model.get_xyz, means2D=screen,
            dc=dc, shs=sh, colors_precomp=colors, opacities=opacity,
            scales=scales, rotations=rotations, cov3D_precomp=covariance)
        image = result.image
        if use_trained_exp:
            exposure = model.get_exposure_from_name(camera.image_name)
            image = torch.matmul(image.permute(1, 2, 0), exposure[:3, :3]).permute(2, 0, 1) + exposure[:3, 3, None, None]
        return {'render': image.clamp(0, 1), 'viewspace_points': screen,
                'visibility_filter': (result.radii > 0).nonzero(), 'radii': result.radii,
                'depth': result.inverse_depth_moment, 'alpha': result.alpha,
                'absolute_screen_gradient': result.absolute_screen_gradient,
                'contributor_pixels': result.contributor_pixels}
