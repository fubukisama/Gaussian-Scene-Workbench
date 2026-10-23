"""Owned synthetic saved-model -> actual camera -> isolated loss CUDA gradients.

This reads only test-owned checkpoints, which use trusted PyTorch pickle. It
does not certify full-state resume or independent geometry/appearance quality.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-root', required=True)
    parser.add_argument('--backend', choices=('3dgs', '2dgs'), required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    run_root = Path(args.run_root).resolve()
    owned = Path('D:/Codex/validation/gsw-quality-upgrade-20261023/p4/training-smoke-v2').resolve()
    run_root.relative_to(owned)
    output = Path(args.output)
    if output.exists() or output.drive.upper() != 'D:':
        raise ValueError('fresh_D_evidence_required')
    sys.path.insert(0, str(ROOT / 'gaussian-splatting' if args.backend == '3dgs' else Path('D:/Apps/GSW/runtime/2dgs')))
    if args.backend == '3dgs':
        sys.path.insert(0, 'D:/Codex/builds/gsw-pinhole-quality-20261023-v1/runtime')
    import torch
    from scene import GaussianModel
    from gaussian_renderer import render
    from arguments import OptimizationParams, PipelineParams
    from native.worker.canonical_training import _camera_from_view, _install_two_dgs_full_k_depth
    from native.worker.quality_training_runtime import QualityTrainingController
    from native.worker.training_checkpoint import read_scheduled_manifest, file_digest
    manifest, path = read_scheduled_manifest(run_root, 64)
    state = torch.load(str(path))
    if manifest['sha256'] != file_digest(path) or state['backend'] != args.backend:
        raise ValueError('owned_checkpoint_hash_or_backend_changed')
    contract = state['camera_contract']
    image_root = Path('D:/Codex/validation/gsw-quality-upgrade-20261023/p0/baseline-v1/datasets/T0/seed-17')
    cameras = [_camera_from_view(view, index, SimpleNamespace(data_device='cpu', white_background=False),
        image_root, args.backend, contract['clipping']) for index, view in enumerate(contract['views']) if view['split'] == 'train']
    option_parser = argparse.ArgumentParser()
    opt = OptimizationParams(option_parser).extract(option_parser.parse_args([]))
    pipe_parser = argparse.ArgumentParser()
    pipe = PipelineParams(pipe_parser).extract(pipe_parser.parse_args([]))
    if args.backend == '3dgs':
        pipe.antialiasing = True
    model = GaussianModel(3)
    if args.backend == '3dgs':
        model.setup_exposure(cameras)
    model.restore(state['model'], opt)
    if args.backend == '3dgs':
        model._exposure = state['exposure']
        model.exposure_mapping = state['exposure_mapping']
        model.pretrained_exposures = state['pretrained_exposures']
    model._gsw_camera_contract = copy.deepcopy(contract)
    scene = SimpleNamespace(getTrainCameras=lambda: cameras, model_path=str(run_root),
                            cameras_extent=state['density_control']['camera_extent'])
    controller = QualityTrainingController(torch, model, scene, pipe, state['quality_plan'], args.backend,
        kernel_root='D:/Codex/builds/gsw-quality-rasterizer-v1-20261023-build2/runtime')
    renderer = controller.renderer
    if renderer is None:
        from native.worker.surfel_depth_adapter import SurfelDepthAdapter
        _install_two_dgs_full_k_depth()
        renderer = SurfelDepthAdapter(render) if 'depth' in controller.providers else render
    camera = cameras[0]
    result = renderer(camera, model, pipe, torch.zeros(3, device='cuda'))
    target = camera.original_image.cuda()
    zero = result['render'].sum() * 0
    loss, unused_l1 = controller.loss(camera, result, target, zero, zero, .2)
    model.optimizer.zero_grad(set_to_none=True)
    loss.backward()
    gradients = {}
    for name in ('_xyz', '_scaling', '_opacity'):
        gradient = getattr(model, name).grad
        gradients[name] = dict(finite=gradient is not None and bool(torch.isfinite(gradient).all()),
            nonzero=int(torch.count_nonzero(gradient)) if gradient is not None else 0,
            absolute_sum=float(gradient.abs().sum()) if gradient is not None else 0.)
    passed = float(loss) > 0 and all(row['finite'] for row in gradients.values()) and any(row['nonzero'] for row in gradients.values())
    evidence = dict(schema='gsw.supervision_isolated_gradient/1', backend=args.backend, gpu_executed=True,
        scope='isolated_selected_supervision_without_global_photo_loss', passed=passed,
        loss=float(loss), gradients=gradients, counts=controller.last_loss_report,
        plan_hash=state['quality_plan']['plan_hash'], checkpoint_sha256=file_digest(path),
        script_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest(),
        independent_geometry_accepted=False, quality_accepted=False)
    with output.open('x', encoding='utf-8') as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True, allow_nan=False)
    print(json.dumps(evidence, sort_keys=True))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
