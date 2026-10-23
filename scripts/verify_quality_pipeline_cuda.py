"""Bounded, opt-in real CUDA adapter/topology/fused-render engineering checks."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def verify(runtime_root):
    import torch
    from native.worker.quality_rasterizer_runtime import configure_quality_rasterizer
    selected = configure_quality_rasterizer(ROOT, runtime_root)
    sys.path.insert(0, str(ROOT / 'gaussian-splatting'))
    import diff_gaussian_rasterization_quality as module
    from scene.gaussian_model import GaussianModel
    from arguments import OptimizationParams
    from native.worker.quality_render_adapter import QualityRenderAdapter
    from native.worker.quality_densification import DensificationStatistics
    from native.worker.training_density_control import NativeDensityControl
    from native.worker.quality_mip import MipState
    from native.worker.canonical_training import pinhole_projection
    parser = argparse.ArgumentParser()
    options = OptimizationParams(parser).extract(parser.parse_args([]))
    camera = SimpleNamespace(image_height=16, image_width=16, FoVx=2*math.atan(.8),
        FoVy=2*math.atan(.8), world_view_transform=torch.eye(4, device='cuda'),
        camera_center=torch.zeros(3, device='cuda'), active_sh_degree=0,
        image_name='owned', K=[[10., 0., 8.], [0., 10., 8.], [0., 0., 1.]], znear=.2, split='train')
    camera.full_proj_transform = torch.tensor(pinhole_projection(camera.K, 16, 16, .2, 50.), device='cuda').T.contiguous()
    pipeline = SimpleNamespace(debug=False, antialiasing=False, compute_cov3D_python=False, convert_SHs_python=False)
    background, red = torch.zeros(3, device='cuda'), torch.tensor([[1., 0., 0.]], device='cuda')

    def model():
        value = GaussianModel(0)
        value._xyz = torch.nn.Parameter(torch.tensor([[0., 0., 2.]], device='cuda'))
        value._features_dc = torch.nn.Parameter(torch.zeros((1, 1, 3), device='cuda'))
        value._features_rest = torch.nn.Parameter(torch.zeros((1, 0, 3), device='cuda'))
        value._scaling = torch.nn.Parameter(torch.full((1, 3), math.log(math.sqrt(.02)), device='cuda'))
        value._rotation = torch.nn.Parameter(torch.tensor([[1., 0., 0., 0.]], device='cuda'))
        value._opacity = torch.nn.Parameter(torch.zeros((1, 1), device='cuda'))
        value.max_radii2D = torch.zeros(1, device='cuda')
        value.spatial_lr_scale = 1.
        value.setup_exposure([camera])
        value.training_setup(options)
        return value

    checks, details = {}, {}
    value = model()
    result = QualityRenderAdapter(torch, module, absgrad=True)(camera, value, pipeline, background, override_color=red)
    (result['render'][0, 6, 6] + result['render'][0, 9, 9]).backward()
    stats = DensificationStatistics(torch, 'abs_gs', 1, 'cuda', 1.)
    stats.accumulate(result['viewspace_points'].grad, result['visibility_filter'],
                     absolute_screen_gradient=result['absolute_screen_gradient'])
    clone, split = stats.gradients()
    checks['precancellation_drives_actual_split'] = float(clone.max()) < 1e-5 and float(split.min()) > .1
    controller = NativeDensityControl(torch, value, 1., '3dgs')
    controller.densify_and_prune(value, .1, .005, None, 600, 3000,
        radii=result['radii'], clone_grads=clone, split_grads=split, split_threshold=.1)
    checks['precancellation_drives_actual_split'] &= len(value.get_xyz) == 2
    checks['topology_optimizer_shapes'] = all(group['params'][0].shape[0] == 2 for group in value.optimizer.param_groups)
    details['abs'] = dict(signed=float(clone.max()), absolute=float(split.min()), rows_after=len(value.get_xyz))
    value = model()
    result = QualityRenderAdapter(torch, module, contributor_pixels=True)(camera, value, pipeline, background, override_color=red)
    result['render'][0, 7, 8].backward()
    signed = result['viewspace_points'].grad.clone()
    stats = DensificationStatistics(torch, 'pixel_gs', 1, 'cuda', 10., .5)
    stats.accumulate(signed, result['visibility_filter'], contributor_pixels=result['contributor_pixels'],
                     camera_z=torch.tensor([2.], device='cuda'))
    expected = torch.linalg.vector_norm(signed[:, :2], dim=1) * .16
    checks['actual_pixels_drive_weighted_density'] = (result['contributor_pixels'].dtype == torch.int64
        and int(stats.denominator[0]) == int(result['contributor_pixels'][0]) == 24
        and torch.allclose(stats.gradients()[0][:, 0], expected) and torch.equal(signed, result['viewspace_points'].grad))
    details['pixel'] = dict(accepted_pixels=int(result['contributor_pixels'][0]), depth_scaled_statistic=float(expected[0]))
    value, fused = model(), model()
    mip = MipState(torch)
    mip.update(value.get_xyz, [dict(K=camera.K, w2c=torch.eye(4), width=16, height=16, znear=.2, split='train')], 0)
    active_s, active_o = mip.active(value.get_scaling, value.get_opacity)
    with torch.no_grad():
        fused._scaling.copy_(active_s.log())
        fused._opacity.copy_(torch.logit(active_o))
    active = QualityRenderAdapter(torch, module, mip=mip)(camera, value, pipeline, background, override_color=red)
    pipeline.antialiasing = True
    baked = QualityRenderAdapter(torch, module)(camera, fused, pipeline, background, override_color=red)
    checks['active_and_fused_same_camera'] = torch.allclose(active['render'], baked['render'], rtol=2e-5, atol=2e-6)
    image_variance = float(active_s[0, 0].square()) * 25
    amplitude = float(active_o[0, 0]) * image_variance / (image_variance + .3)
    expected_pixel = amplitude * math.exp(-.5 * (.5 ** 2 + .5 ** 2) / (image_variance + .3))
    observed_pixel = float(active['render'][0, 7, 8])
    checks['three_and_two_dimensional_mip_compensation'] = math.isclose(observed_pixel, expected_pixel, rel_tol=3e-4, abs_tol=2e-6)
    active['render'].sum().backward()
    checks['mip_raw_scale_and_opacity_gradients'] = (bool(torch.isfinite(value._scaling.grad).all())
        and bool(torch.isfinite(value._opacity.grad).all()) and bool(value._scaling.grad.abs().sum() > 0))
    details['mip'] = dict(expected_pixel=expected_pixel, observed_pixel=observed_pixel,
                          active_fused_max_difference=float((active['render'] - baked['render']).abs().max()))
    torch.cuda.synchronize()
    return checks, details, selected['buildSha256']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module-root', required=True)
    parser.add_argument('--stock-module-root', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    output = Path(args.output)
    if output.exists():
        raise ValueError('existing_evidence_refused')
    sys.path.insert(0, args.stock_module_root)
    evidence = dict(schema='gsw.quality_pipeline_cuda/1', accepted=False, gpu_executed=False,
        scope='synthetic_engineering_only_no_training_quality_or_native_viewer',
        script_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest())
    try:
        evidence['gpu_executed'] = True
        evidence['checks'], evidence['details'], evidence['kernel_build_sha256'] = verify(args.module_root)
        evidence['accepted'] = all(evidence['checks'].values())
    except Exception as error:
        evidence['error'] = dict(type=type(error).__name__, message=str(error))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True, allow_nan=False)
    print(json.dumps(evidence, sort_keys=True))
    return 0 if evidence['accepted'] else 1


if __name__ == '__main__':
    sys.exit(main())
