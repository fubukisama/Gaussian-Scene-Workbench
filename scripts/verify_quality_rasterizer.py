"""Opt-in public CUDA numerical seam; CPU mode cannot produce GPU acceptance.

Only fresh synthetic Gaussians are used. No training or real dataset is read.
The finite-difference domain excludes alpha/visibility thresholds and saturated
RGB/depth clipping, whose inherited stock approximate gradients are unchanged.
"""
import argparse
import hashlib
import importlib
import json
import math
from pathlib import Path
import sys

REPOSITORY = Path(__file__).resolve().parents[1]
if str(REPOSITORY) not in sys.path:
    sys.path.insert(0, str(REPOSITORY))
from training_kit.quality_rasterizer.reference import aggregate_screen_gradients, composite_pixel


def cpu_checks():
    assert aggregate_screen_gradients([(1., -2.), (-1., 2.)]) == {
        'signed': [0., 0.], 'absolute': [2., 4.]}
    pixel = composite_pixel([.5, .25], [.5, .25], [(1., 0., 0.), (0., 1., 0.)])
    assert pixel['alpha'] == .625 and pixel['inverse_depth_moment'] == .28125
    return {'cancellation_literal': True, 'alpha_moment_literal': True}


def render(torch, module, flags=(False, False, False), gradients=False, opacities=None, means=None, covariance=None,
           background=(0., 0., 0.)):
    # One 16x16 tile removes inter-tile signed-atomic ordering from the strict regression case.
    width, height, focal = 16, 16, 10.
    view = torch.eye(4, dtype=torch.float32, device='cuda')
    projection = torch.zeros((4, 4), dtype=torch.float32, device='cuda')
    projection[0, 0] = projection[1, 1] = 2*focal/width
    projection[2, 2], projection[2, 3], projection[3, 2] = 50./49.8, -10./49.8, 1.
    settings = dict(image_height=height, image_width=width, tanfovx=width/(2*focal), tanfovy=height/(2*focal),
                    bg=torch.tensor(background, device='cuda'), scale_modifier=1., viewmatrix=view.T.contiguous(),
                    projmatrix=projection.T.contiguous(), sh_degree=0, campos=torch.zeros(3, device='cuda'),
                    prefiltered=False, debug=False, antialiasing=False)
    if hasattr(module, 'QualityRasterizationResult'):
        settings.update(enable_absgrad=flags[0], enable_contributor_pixels=flags[1], enable_alpha=flags[2])
    mean = torch.tensor(means if means is not None else [[0., 0., 2.]], dtype=torch.float32,
                        device='cuda', requires_grad=gradients)
    n = len(mean)
    cov = torch.tensor(covariance if covariance is not None else [[.02, 0., 0., .02, 0., .02]] * n,
                       dtype=torch.float32, device='cuda', requires_grad=gradients)
    opacity = torch.tensor(opacities if opacities is not None else [[.5]] * n,
                           dtype=torch.float32, device='cuda', requires_grad=gradients)
    colors = torch.tensor([[1., 0., 0.]] * n, dtype=torch.float32, device='cuda', requires_grad=gradients)
    screen = torch.zeros_like(mean, requires_grad=gradients)
    result = module.GaussianRasterizer(module.GaussianRasterizationSettings(**settings))(
        means3D=mean, means2D=screen, colors_precomp=colors, opacities=opacity, cov3D_precomp=cov)
    return result, {'mean': mean, 'covariance': cov, 'opacity': opacity, 'colors': colors, 'screen': screen}


def gpu_checks(torch, quality, stock):
    checks, details = {}, {}
    background = (.1, .2, .3)
    old, old_inputs = render(torch, stock, gradients=True, background=background)
    new, new_inputs = render(torch, quality, gradients=True, background=background)
    checks['flags_off_outputs'] = all(torch.equal(old[i], new[i]) for i in range(3))
    checks['flags_off_outputs'] = checks['flags_off_outputs'] and all(new[i].numel() == 0 for i in range(3, 6))
    objective = lambda value: value[0][0, 7, 6] + value[0][0, 7, 9] + .3*value[2][0, 7, 8]
    objective(old).backward()
    objective(new).backward()
    checks['flags_off_signed_gradients'] = all(torch.equal(old_inputs[k].grad, new_inputs[k].grad)
                                                for k in old_inputs)
    stats, inputs = render(torch, quality, flags=(True, True, False), gradients=True, background=background)
    objective(stats).backward()
    checks['flags_off_signed_gradients'] &= all(torch.equal(old_inputs[k].grad, inputs[k].grad) for k in inputs)
    # Pure RGB symmetric opposing x derivatives, no depth term or background variation.
    cancellation, inputs = render(torch, quality, flags=(True, False, False), gradients=True)
    (cancellation[0][0, 7, 6] + cancellation[0][0, 7, 9]).backward()
    # Black background avoids importing stock background-gradient approximations into this analytic case.
    intensity = .5 * math.exp(-.5 * (1.5**2 + .5**2) / .8)
    expected_abs_x = 2 * intensity * (1.5/.8) * 8
    observed_abs = float(cancellation.absolute_screen_gradient[0, 0])
    signed = float(inputs['screen'].grad[0, 0])
    checks['absolute_precancellation'] = abs(signed) < 1e-5 and math.isclose(observed_abs, expected_abs_x, rel_tol=3e-4, abs_tol=1e-5)
    details['absolute_gradient'] = {'expected_ndc_x': expected_abs_x, 'observed_ndc_x': observed_abs, 'signed_ndc_x': signed}
    count = sum(.5 * math.exp(-.5*((x-7.5)**2+(y-7.5)**2)/.8) >= 1./255.
                for y in range(16) for x in range(16))
    counts = render(torch, quality, flags=(False, True, False))[0].contributor_pixels
    checks['contributor_pixels'] = float(counts[0]) == count
    details['contributing_pixels'] = {'expected': count, 'observed': float(counts[0])}
    derivative_details = {}
    for mode in ('alpha', 'normalized_inverse_depth'):
        # Two different depths are necessary: one splat's moment/alpha quotient cancels opacity exactly.
        means = [[0., 0., 2.], [.1, 0., 3.]]
        opacity_values = [[.5], [.25]]
        result, leaves = render(torch, quality, flags=(False, False, True), gradients=True,
                                means=means, opacities=opacity_values)
        def loss(value):
            alpha = value.alpha[0, 7, 8]
            return alpha if mode == 'alpha' else value.inverse_depth_moment[0, 7, 8] / alpha
        loss(result).backward()
        observed, finite = {}, {}
        for field, values, eps in [('opacity', opacity_values, 1e-3), ('mean', means, 2e-4),
                                   ('covariance', [[.02, 0., 0., .02, 0., .02]] * 2, 2e-5)]:
            observed[field] = leaves[field].grad.detach().cpu().tolist()
            field_finite = []
            for i, row in enumerate(values):
                gradients = []
                for j in range(len(row)):
                    plus, minus = [list(v) for v in values], [list(v) for v in values]
                    plus[i][j] += eps
                    minus[i][j] -= eps
                    args = dict(flags=(False, False, True), means=means, opacities=opacity_values)
                    argument = {'opacity': 'opacities', 'mean': 'means', 'covariance': 'covariance'}[field]
                    args[argument] = plus
                    upper = float(loss(render(torch, quality, **args)[0]))
                    args[argument] = minus
                    lower = float(loss(render(torch, quality, **args)[0]))
                    gradients.append((upper-lower)/(2*eps))
                field_finite.append(gradients)
            finite[field] = field_finite
        okay = all(math.isclose(o, f, rel_tol=.025, abs_tol=.004)
                   for field in observed for rows in zip(observed[field], finite[field]) for o, f in zip(*rows))
        checks[mode + '_gradients'] = okay
        derivative_details[mode] = {'autograd': observed, 'finite_difference': finite}
    details['smooth_domain_derivatives'] = derivative_details
    # Reuse existing independent pinhole oracle through the same public renderer seam.
    from scripts.verify_pinhole_rasterizer_gpu import cases, run_case
    details['pinhole_cases'] = [run_case(torch, quality, case, 2e-4, 2e-6) for case in cases()]
    checks['full_pinhole_pixel_border'] = all(case['accepted'] for case in details['pinhole_cases'])
    return checks, details


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module-root')
    parser.add_argument('--stock-module-root')
    parser.add_argument('--output', required=True)
    parser.add_argument('--cpu-only', action='store_true')
    args = parser.parse_args(argv)
    output = Path(args.output).resolve()
    if output.drive.upper() != 'D:' or output.exists():
        raise ValueError('quality_verification_requires_new_d_output')
    evidence = {'schema': 'gsw.quality_rasterizer_acceptance/1', 'gpu_executed': False, 'accepted': False,
                'scope': 'synthetic_numeric_only_not_training_or_quality',
                'derivative_domain': 'smooth nonthreshold nonsaturated; inherited stock saturated RGB/depth approximate derivatives unchanged'}
    status = 2
    try:
        evidence['cpu_checks'] = cpu_checks()
        if args.cpu_only:
            evidence['cpu_reference_passed'] = True
            status = 0
        else:
            if not args.module_root or not args.stock_module_root:
                raise ValueError('quality_and_stock_module_roots_required')
            root, stock_root = Path(args.module_root).resolve(), Path(args.stock_module_root).resolve()
            build_path = root / 'quality-rasterizer-build.json'
            # Validate build source/binary identity without trusting any acceptance claim.
            from native.worker.quality_rasterizer_runtime import inspect_quality_rasterizer_build
            inspected = inspect_quality_rasterizer_build(REPOSITORY, root)
            build = inspected['build']
            evidence['build_manifest_sha256'] = inspected['buildSha256']
            import torch
            if build.get('torch') != torch.__version__ or build.get('cuda') != torch.version.cuda:
                raise ValueError('quality_verification_runtime_mismatch')
            sys.path.insert(0, str(stock_root))
            sys.path.insert(0, str(root))
            quality = importlib.import_module('diff_gaussian_rasterization_quality')
            stock = importlib.import_module('diff_gaussian_rasterization')
            Path(quality._C.__file__).resolve().relative_to(root)
            Path(stock._C.__file__).resolve().relative_to(stock_root)
            if (getattr(quality._C, 'GSW_QUALITY_RASTERIZER_ABI', 0) != 1 or
                    getattr(quality._C, 'GSW_PINHOLE_COVARIANCE_VERSION', 0) != 1 or
                    getattr(stock._C, 'GSW_PINHOLE_COVARIANCE_VERSION', 0) != 1):
                raise ValueError('quality_verification_abi_mismatch')
            if not torch.cuda.is_available():
                raise RuntimeError('actual_cuda_required')
            evidence['runtime'] = {'torch': torch.__version__, 'cuda': torch.version.cuda,
                'device': torch.cuda.get_device_name(), 'device_sm': list(torch.cuda.get_device_capability()),
                'quality_binary_sha256': hashlib.sha256(Path(quality._C.__file__).read_bytes()).hexdigest(),
                'stock_binary_sha256': hashlib.sha256(Path(stock._C.__file__).read_bytes()).hexdigest()}
            evidence['gpu_executed'] = True
            evidence['checks'], evidence['details'] = gpu_checks(torch, quality, stock)
            torch.cuda.synchronize()
            evidence['accepted'] = all(evidence['checks'].values())
            status = 0 if evidence['accepted'] else 1
    except Exception as error:
        evidence['error'] = {'type': type(error).__name__, 'message': str(error)}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as stream:
        json.dump(evidence, stream, sort_keys=True, indent=2, allow_nan=False)
    print(json.dumps({'accepted': evidence['accepted'], 'gpu_executed': evidence['gpu_executed'], 'output': str(output)}))
    return status


if __name__ == '__main__':
    sys.exit(main())
