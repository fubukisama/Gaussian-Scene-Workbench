"""Prepare then execute bounded owned T0 engineering trials; no quality claim.

Uses already frozen generated inputs. It never accepts real-photo directories,
downloads assets, changes runtime options or silently retries failed trials.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

MIP_RESET_OBSERVER = r'''"""Passive real Full Mip reset observer; never replaces the method."""
import json
import math
import os
from pathlib import Path
import runpy
import sys

repository, entry = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path.insert(0, str(repository))
sys.path.insert(0, str(entry.parent))
import torch

target = os.path.normcase(str(repository / 'native/worker/quality_training_runtime.py'))
entry_key = os.path.normcase(str(entry))

def observe(frame, event, value):
    if event != 'return' or frame.f_code.co_name != 'reset_opacity':
        return
    if os.path.normcase(frame.f_code.co_filename) != target:
        return
    owner = frame.f_locals.get('self')
    if owner is None or owner.mip is None:
        return
    caller, iteration = frame.f_back, None
    while caller:
        if caller.f_code.co_name == 'training' and os.path.normcase(caller.f_code.co_filename) == entry_key:
            iteration = caller.f_locals.get('iteration')
            break
        caller = caller.f_back
    logits = owner.model._opacity
    checked, finite = 0, True
    for state in owner.model.optimizer.state.values():
        for item in state.values():
            if isinstance(item, torch.Tensor):
                checked += 1
                finite = finite and bool(torch.isfinite(item).all())
            elif isinstance(item, float):
                checked += 1
                finite = finite and math.isfinite(item)
    groups = [group for group in owner.model.optimizer.param_groups if group.get('name') == 'opacity']
    bound = len(groups) == 1 and len(groups[0]['params']) == 1 and groups[0]['params'][0] is logits
    payload = dict(iteration=iteration, raw_logits_finite=bool(torch.isfinite(logits).all()),
        optimizer_finite=finite, optimizer_state_items_checked=checked, optimizer_parameter_bound=bound,
        rows=len(logits), method='real_QualityTrainingController.reset_opacity_return', quality_accepted=False)
    stream = getattr(sys, '__stdout__', None) or sys.stdout
    stream.write('[owned-mip-opacity-reset] ' + json.dumps(payload, sort_keys=True, allow_nan=False) + '\n')
    stream.flush()

sys.setprofile(observe)
sys.argv = [str(entry)] + sys.argv[sys.argv.index('--') + 1:]
runpy.run_path(str(entry), run_name='__main__')
'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)


def prepare(manifest_path, output, stock_runtime, quality_runtime, mip_opacity_reset_interval=None):
    import numpy as np
    from PIL import Image
    from native.worker.canonical_training import load_prepared_training_dataset, training_contract
    from native.worker.training_quality_plan import resolve_quality_plan
    if mip_opacity_reset_interval not in (None, 16):
        raise ValueError('quality_training_mip_reset_diagnostic_requires_interval16')
    output = Path(output).resolve()
    if output.drive.upper() != 'D:' or output.exists():
        raise ValueError('quality_training_fresh_d_output_required')
    manifest_path = Path(manifest_path).resolve()
    baseline = json.loads(manifest_path.read_text(encoding='utf-8'))
    runs = [run for run in baseline['gpu_execution_plan']['runs'] if run['fixture_id'] == 'T0' and run['seed'] == 17]
    if len(runs) != 2 or {run['backend'] for run in runs} != {'3dgs', '2dgs'}:
        raise ValueError('owned_frozen_T0_baselines_required')
    output.mkdir(parents=True)
    diagnostic_sources = {}
    if mip_opacity_reset_interval is not None:
        observer = output / 'diagnostics/mip_opacity_reset_observer.py'
        observer.parent.mkdir()
        with observer.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(MIP_RESET_OBSERVER)
        compile(MIP_RESET_OBSERVER, str(observer), 'exec')
        diagnostic_sources[str(observer)] = digest(observer)
    original = next(run for run in runs if run['backend'] == '3dgs')
    environment = original['environment_overrides']
    contract = training_contract(load_prepared_training_dataset(environment['GSW_CAMERA_DATASET'], environment['GSW_CAMERA_IMAGE_ROOT']))
    evidence_id = hashlib.sha256(json.dumps(contract['transforms'], sort_keys=True).encode()).hexdigest()
    asset_root = output / 'supervision'
    asset_root.mkdir()

    def array(name, values):
        file = asset_root / name
        with file.open('xb') as stream:
            np.save(stream, values, allow_pickle=False)
        return dict(path=name, sha256=digest(file))

    mask_rows, depth_rows = [], []
    for view in contract['views']:
        if view['split'] != 'train':
            continue
        index = int(view['view_id'].rsplit('-', 1)[1])
        references = manifest_path.parent / 'references/T0'
        depth = np.load(str(references / ('camera-z-%02d.npy' % index)), allow_pickle=False).astype(np.float32)
        with Image.open(references / ('background-%02d.png' % index)) as image:
            background = np.asarray(image.convert('L')) > 0
        geometry = dict(width=view['training_size'][0], height=view['training_size'][1], K=view['training_K'],
                        pixel_coordinates='pixel_border', frame='camera', distortion=[])
        base = dict(view_id=view['view_id'], split='train', image_sha256=view['sha256'], geometry=geometry)
        mask_rows.append(dict(base, mask=dict(array('mask-%d.npy' % index, np.where(background, 2, 1).astype(np.uint8)),
            labels=dict(invalid=0, foreground=1, background=2))))
        valid = np.isfinite(depth) & (depth > 0)
        record = dict(array('depth-%d.npy' % index, depth), definition='camera_z', unit='m', frame='camera',
            confidence=array('confidence-%d.npy' % index, valid.astype(np.float32)),
            validity=array('validity-%d.npy' % index, valid.astype(np.uint8)),
            materials=dict(array('material-%d.npy' % index, valid.astype(np.uint8)),
                labels=dict(unknown=0, opaque=1, transparent=2, specular=3, occluded=4)),
            provenance=dict(kind='oracle', source='owned analytic T0 training-only reference', role='training',
                oracle_training_authorized=True, alignment=dict(source='none', view_ids=[])))
        depth_rows.append(dict(base, depth=record))
    for kind, rows in (('mask', mask_rows), ('depth', depth_rows)):
        write(asset_root / (kind + '.json'), dict(schema='gsw.quality_supervision/1',
            dataset_identity=contract['input_data_identity'], views=rows))
    requests = {'standard': {'version': 1}, 'abs': {'version': 1, 'densification': {'strategy': 'abs_gs'}},
        'pixel': {'version': 1, 'densification': {'strategy': 'pixel_gs'}},
        'mip': {'version': 1, 'filter': {'mode': 'full_mip'}},
        'mask': {'version': 1, 'supervision': {'mask': {'manifest': str(asset_root / 'mask.json'),
            'foreground_weight': 1., 'background_alpha_weight': 0.}}},
        'depth': {'version': 1, 'supervision': {'depth': {'manifest': str(asset_root / 'depth.json'), 'weight': .1,
            'allow_oracle': True, 'unit_to_training_scale': contract['transforms']['canonical_to_training']['s'],
            'scale_evidence_identity': evidence_id}}}}
    trials = []
    for original in runs:
        names = ('standard', 'abs', 'pixel', 'mip', 'mask', 'depth') if original['backend'] == '3dgs' else ('standard', 'mask', 'depth')
        for name in names:
            run = copy.deepcopy(original)
            destination = output / ('run-' + run['backend'] + '-' + name)
            run['method'], run['output'] = name, str(destination)
            run['argv'][run['argv'].index('-m') + 1] = str(destination)
            if name == 'mip' and mip_opacity_reset_interval is not None:
                flag = '--opacity_reset_interval'
                if flag in run['argv']:
                    run['argv'][run['argv'].index(flag) + 1] = str(mip_opacity_reset_interval)
                else:
                    run['argv'].extend((flag, str(mip_opacity_reset_interval)))
                run['argv'] = run['argv'][:2] + [str(observer), str(ROOT), run['argv'][2], '--'] + run['argv'][3:]
                run['reset_observation'] = dict(expected_iterations=[16, 32, 48], source=str(observer),
                    sha256=digest(observer), primitive='actual_controller_return_not_method_replacement')
            env = run['environment_overrides']
            control = json.loads(env['GSW_NATIVE_TRAINING_CONTROL'])
            control.update(output=str(destination), request=str(destination / 'pause-request.json'), session='quality-' + run['backend'] + '-' + name,
                           qualityPlan=resolve_quality_plan(requests[name], backend=run['backend']))
            env['GSW_NATIVE_TRAINING_CONTROL'] = json.dumps(control, separators=(',', ':'))
            env['GSW_CAMERA_RASTERIZER_ROOT'] = str(Path(stock_runtime).resolve())
            env['GSW_QUALITY_KERNEL_ROOT'] = str(Path(quality_runtime).resolve())
            env['CUDA_CACHE_PATH'] = 'D:/CodexData/CUDA/ComputeCache'
            env['PYTHONDONTWRITEBYTECODE'] = '1'
            trials.append(run)
    runtime_files = {}
    for runtime in (Path(stock_runtime), Path(quality_runtime)):
        for path in sorted(runtime.rglob('*')):
            if path.is_file() and path.suffix in ('.py', '.pyd', '.json'):
                runtime_files[str(path.resolve())] = digest(path)
    for candidate in baseline['runtime_candidates']['backends'].values():
        for file in candidate['files_sha256']:
            runtime_files[file] = digest(file)
        site = Path(candidate['python']).parent / 'Lib/site-packages'
        for file in [site / 'fused_ssim/__init__.py'] + list(site.glob('fused_ssim_cuda*.pyd')):
            if file.is_file(): runtime_files[str(file)] = digest(file)
    frozen = dict(schema='gsw.quality_training_engineering/1', baseline_sha256=digest(manifest_path),
        scope='64_step_direct_trainers_not_worker_native_or_quality_acceptance', seed=17, steps=64,
        oracle='synthetic_train_only_masks_and_depth_not_independent_geometry',
        maximum_total_wall_seconds=300, maximum_trial_wall_seconds=90, automatic_retry=False,
        stop_on_first_failure=True, quality_accepted=False, trials=trials,
        mip_opacity_reset_interval=mip_opacity_reset_interval, diagnostic_sources=diagnostic_sources,
        runtime_sha256=runtime_files,
        sources={str(path.relative_to(ROOT)).replace('\\', '/'): hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
                 for path in [Path(__file__).resolve(), ROOT / 'gaussian-splatting/train.py']
                 + [ROOT / 'native/worker' / name for name in (
                     'quality_training_runtime.py', 'quality_render_adapter.py', 'quality_densification.py',
                     'quality_mip.py', 'quality_supervision.py', 'quality_random.py', 'quality_export.py',
                     'quality_rasterizer_runtime.py', 'training_quality_plan.py', 'training_checkpoint.py',
                     'training_runtime.py', 'training_density_control.py', 'training_resource_policy.py',
                     'training_preview.py', 'surfel_depth_adapter.py', 'two_dgs_train.py')]})
    write(output / 'protocol.json', frozen)
    print(json.dumps(dict(prepared=str(output / 'protocol.json'), trials=len(trials), executed=False)))


def reset_observations(stdout, expected_iterations):
    decoder, events = json.JSONDecoder(), []
    try:
        for line in stdout.decode('utf-8', errors='replace').splitlines():
            if line.startswith('[owned-mip-opacity-reset] '):
                event, unused = decoder.raw_decode(line[len('[owned-mip-opacity-reset] '):])
                events.append(event)
        passed = ([event['iteration'] for event in events] == expected_iterations
            and all(event['raw_logits_finite'] is True and event['optimizer_finite'] is True
                    and event['optimizer_parameter_bound'] is True
                    and event['optimizer_state_items_checked'] > 0 for event in events))
        return dict(passed=passed, expected_iterations=expected_iterations, events=events)
    except (ValueError, KeyError, TypeError) as error:
        return dict(passed=False, expected_iterations=expected_iterations, events=events,
                    error=dict(type=type(error).__name__, message=str(error)))


def verify_frozen_files(protocol):
    for relative, expected in protocol['sources'].items():
        if hashlib.sha256((ROOT / relative).read_bytes().replace(b'\r\n', b'\n')).hexdigest() != expected:
            raise ValueError('quality_training_frozen_source_changed: ' + relative)
    for absolute, expected in dict(protocol.get('runtime_sha256', {}), **protocol.get('diagnostic_sources', {})).items():
        if digest(absolute) != expected:
            raise ValueError('quality_training_frozen_runtime_or_observer_changed: ' + absolute)


def execute(output):
    output = Path(output).resolve()
    protocol_path = output / 'protocol.json'
    protocol = json.loads(protocol_path.read_text(encoding='utf-8'))
    if (output / 'execution.json').exists() or (output / 'attempt-started').exists():
        raise ValueError('existing_evidence_no_retry')
    verify_frozen_files(protocol)
    with (output / 'attempt-started').open('x', encoding='utf-8') as stream:
        stream.write(str(time.time()))
    results, started = [], time.monotonic()
    for run in protocol['trials']:
        if time.monotonic() - started > protocol['maximum_total_wall_seconds']:
            break
        environment = dict(os.environ, **run['environment_overrides'])
        before = time.monotonic()
        try:
            process = subprocess.run(run['argv'], cwd=run['cwd'], env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=min(protocol['maximum_trial_wall_seconds'],
                    protocol['maximum_total_wall_seconds'] - (time.monotonic() - started)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            stdout, stderr, code = process.stdout, process.stderr, process.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code = error.stdout or b'', error.stderr or b'', -1
        name = run['backend'] + '-' + run['method']
        (output / (name + '.stdout.log')).write_bytes(stdout)
        (output / (name + '.stderr.log')).write_bytes(stderr)
        result = dict(backend=run['backend'], method=run['method'], exit_code=code, seconds=time.monotonic() - before)
        if 'reset_observation' in run:
            result['mip_reset_observation'] = reset_observations(stdout, run['reset_observation']['expected_iterations'])
        try:
            verify_frozen_files(protocol)
        except (OSError, ValueError) as error:
            result['frozen_identity_error'] = str(error)
        results.append(result)
        print(json.dumps(result), flush=True)
        if code or result.get('frozen_identity_error') or not result.get('mip_reset_observation', {'passed': True})['passed']:
            break
    passed = len(results) == len(protocol['trials']) and all(row['exit_code'] == 0
        and not row.get('frozen_identity_error') and row.get('mip_reset_observation', {'passed': True})['passed'] for row in results)
    write(output / 'execution.json', dict(protocol_sha256=digest(protocol_path), passed=passed, results=results,
        total_wall_seconds=time.monotonic() - started, quality_accepted=False, worker_accepted=False, native_accepted=False))
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('prepare', 'execute'))
    parser.add_argument('--baseline-manifest')
    parser.add_argument('--stock-runtime')
    parser.add_argument('--quality-runtime')
    parser.add_argument('--mip-opacity-reset-interval', type=int, choices=(16,),
        help='Owned engineering diagnostic: only Full Mip resets every16 steps; normal defaults are unchanged')
    parser.add_argument('--output-root', required=True)
    args = parser.parse_args()
    if args.stage == 'prepare':
        if not args.stock_runtime or not args.quality_runtime:
            parser.error('prepare requires explicit --stock-runtime and --quality-runtime')
        prepare(args.baseline_manifest, args.output_root, args.stock_runtime, args.quality_runtime, args.mip_opacity_reset_interval)
        return 0
    return execute(args.output_root)


if __name__ == '__main__':
    sys.exit(main())
