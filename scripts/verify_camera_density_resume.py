"""Opt-in bounded, actual density/resume audit; generated inputs only.

This is not a default-schedule, reconstruction-quality, or performance benchmark.
No production function is replaced.  The child uses Python's profile observer to
read actual inputs/results of the real density/checkpoint methods.  The only
training control it writes is the existing iteration-boundary pause request.
GPU execution is intentionally opt-in; --plan-only prepares fixtures on CPU.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
TOTAL = 8
INITIAL_POINTS = 32
GROWTH_ITERATIONS = (2, 4, 6)
PAUSE_ITERATIONS = (3, 5)
MAX_POINTS = INITIAL_POINTS * 2 ** len(GROWTH_ITERATIONS)


def tree_digest(value):
    """Content-only digest: ignores CPU/GPU location, not dtype/shape/order."""
    import numpy as np
    digest = hashlib.sha256()

    def visit(item):
        if hasattr(item, 'detach'):
            item = item.detach().contiguous().cpu().numpy()
        if isinstance(item, np.ndarray):
            digest.update(b'array:' + item.dtype.str.encode() + repr(item.shape).encode())
            digest.update(item.tobytes(order='C'))
        elif isinstance(item, dict):
            digest.update(b'dict:')
            for key in sorted(item, key=lambda key: (type(key).__name__, repr(key))):
                visit(key)
                visit(item[key])
        elif isinstance(item, (list, tuple)):
            digest.update(type(item).__name__.encode() + str(len(item)).encode())
            for part in item:
                visit(part)
        elif isinstance(item, float):
            digest.update(b'float:' + struct.pack('>d', item))
        else:
            digest.update(type(item).__name__.encode() + b':' + repr(item).encode('utf-8'))
    visit(value)
    return digest.hexdigest()


def model_summary(model):
    import numpy as np
    # Both vendored backends expose the same complete-state tuple ABI.
    captured = model.capture()
    count = int(captured[1].shape[0])
    assert 0 < count <= MAX_POINTS, ('bounded_point_count', count)
    for tensor in captured[1:10]:
        assert int(tensor.shape[0]) == count
        assert np.isfinite(tensor.detach().cpu().numpy()).all()
    names = {'xyz': 1, 'f_dc': 2, 'f_rest': 3, 'scaling': 4, 'rotation': 5, 'opacity': 6}
    optimizer = captured[10]
    groups = []
    for group in optimizer['param_groups']:
        assert len(group['params']) == 1
        param = captured[names[group['name']]]
        state = optimizer['state'].get(group['params'][0], {})
        for key in ('exp_avg', 'exp_avg_sq'):
            if key in state:
                assert tuple(state[key].shape) == tuple(param.shape), (group['name'], key)
                assert np.isfinite(state[key].detach().cpu().numpy()).all()
        groups.append({'name': group['name'], 'parameterShape': list(param.shape),
                       'initializedMoments': all(key in state for key in ('exp_avg', 'exp_avg_sq'))})
    return {'points': count, 'modelAdamStatisticsSha256': tree_digest(captured),
            'denomSum': float(captured[9].sum().item()),
            'gradientAccumulatorSum': float(captured[8].sum().item()),
            'optimizerGroups': groups}


def run_child(args, trainer_args):
    """Run the unchanged trainer with a numeric read-only observer attached."""
    from native.worker.training_checkpoint import atomic_json
    path = Path(args.audit)
    path.parent.mkdir(parents=True, exist_ok=True)
    control = json.loads(os.environ['GSW_NATIVE_TRAINING_CONTROL'])
    latest = {}
    calls = {}
    scheduled = False
    stream = path.open('x', encoding='utf-8')

    def emit(event, **record):
        stream.write(json.dumps(dict(event=event, **record), sort_keys=True, allow_nan=False) + '\n')
        stream.flush()

    def observe(frame, event, result):
        nonlocal scheduled
        if event not in ('call', 'return'):
            return
        name = frame.f_code.co_name
        filename = frame.f_code.co_filename.replace('\\', '/')
        local = frame.f_locals
        if filename.endswith('/canonical_training.py') and name == 'audit_sample' and event == 'return':
            camera, iteration = local['camera'], int(local['iteration'])
            latest.update(iteration=iteration, view_id=camera.view_id, split=camera.split)
            emit('sample', **latest)
            if iteration == args.pause_after and not scheduled:
                atomic_json(control['request'], {'session': control['session']})
                scheduled = True
                emit('pause_requested', iteration=iteration)
        elif filename.endswith('/scene/gaussian_model.py') and name in (
                'add_densification_stats', 'densify_and_clone', 'densify_and_split'):
            model = local['self']
            if event == 'call':
                before = {'beforePoints': int(model.get_xyz.shape[0]), **latest}
                if name == 'add_densification_stats':
                    before.update(denomBefore=float(model.denom.sum().item()),
                        gradientBefore=float(model.xyz_gradient_accum.sum().item()),
                        visible=int(local['update_filter'].sum().item()))
                calls[id(frame)] = before
            else:
                before = calls.pop(id(frame))
                record = dict(before, afterPoints=int(model.get_xyz.shape[0]))
                if name == 'add_densification_stats':
                    record.update(denomAfter=float(model.denom.sum().item()),
                        gradientAfter=float(model.xyz_gradient_accum.sum().item()))
                emit(name, **record)
        elif filename.endswith('/training_density_control.py') and name == 'densify_and_prune':
            model = local['model']
            if event == 'call':
                calls[id(frame)] = model_summary(model)
            else:
                emit('density_event', iteration=int(local['iteration']), before=calls.pop(id(frame)),
                     after=model_summary(model), **{key: latest[key] for key in ('view_id', 'split')})
        elif filename.endswith('/training_checkpoint.py') and name == 'restore_state':
            if event == 'return':
                import random
                import numpy as np
                import torch
                model, state = local['gaussians'], local['state']
                restored = model_summary(model)
                assert tree_digest(state['model']) == restored['modelAdamStatisticsSha256']
                rng = {'random': random.getstate(), 'numpy': np.random.get_state(),
                       'torch': torch.get_rng_state(), 'cuda': torch.cuda.get_rng_state_all()}
                assert tree_digest(rng) == tree_digest({key: state[key] for key in rng})
                assert local['camera_names'] == state['camera_names']
                if state['backend'] == '3dgs':
                    assert tree_digest(model._exposure) == tree_digest(state['exposure'])
                    assert tree_digest(model.exposure_optimizer.state_dict()) == tree_digest(state['exposure_optimizer'])
                    assert model.exposure_mapping == state['exposure_mapping']
                emit('restore_verified', iteration=int(state['iteration']), model=restored,
                     cameraNames=state['camera_names'], pendingIndices=state['pending_indices'],
                     cameraContractSha256=tree_digest(state['camera_contract']),
                     optimizerStatisticsRngExact=True)
        elif filename.endswith('/training_checkpoint.py') and name == 'save' and event == 'return':
            state = local['state']
            model = state['model']
            # The trainer published this payload after real optimizer.step().
            assert float(model[9].sum().item()) > 0
            assert float(model[8].sum().item()) > 0
            assert state['iteration'] in PAUSE_ITERATIONS
            emit('checkpoint_saved', iteration=state['iteration'], points=int(model[1].shape[0]),
                 modelAdamStatisticsSha256=tree_digest(model), identity=state['identity'],
                 cameraNames=state['camera_names'], pendingIndices=state['pending_indices'],
                 cameraContractSha256=tree_digest(state['camera_contract']),
                 gradientAccumulatorSum=float(model[8].sum().item()),
                 denomSum=float(model[9].sum().item()), manifest=result)

    sys.argv = [args.entry, *trainer_args]
    # Match a normal Python script launch: upstream scene/arguments imports must
    # resolve from the selected 3DGS or 2DGS source, not this diagnostics folder.
    sys.path.insert(0, str(Path(args.entry).parent))
    sys.setprofile(observe)
    try:
        runpy.run_path(args.entry, run_name='__main__')
    finally:
        sys.setprofile(None)
        stream.close()


def execute(command, cwd, environment, log, expected):
    started = time.monotonic()
    with log.open('x', encoding='utf-8') as stream:
        child = subprocess.Popen([str(part) for part in command], cwd=cwd, env=environment,
            stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
        try:
            code = child.wait(timeout=180)
        except subprocess.TimeoutExpired:
            child.terminate()  # This acceptance-owned short child only.
            child.wait(timeout=15)
            raise RuntimeError('Owned bounded trainer timed out: ' + str(log))
    assert code == expected, (code, expected, str(log), log.read_text(encoding='utf-8')[-7000:])
    return {'exit': code, 'seconds': time.monotonic() - started, 'log': str(log)}


def verify_audits(work, prepared, mode):
    from scripts.verify_camera_scale_gpu import verify_output
    records = [json.loads(line) for phase in ('pause3', 'pause5', 'finish')
               for line in (work / (phase + '.jsonl')).read_text(encoding='utf-8').splitlines()]
    train_ids = set(prepared['contract']['splits']['train'])
    heldout_ids = set(prepared['contract']['splits']['val'] + prepared['contract']['splits']['test'])
    samples = [row for row in records if row['event'] == 'sample']
    assert [row['iteration'] for row in samples] == list(range(1, TOTAL + 1))
    stats = [row for row in records if row['event'] == 'add_densification_stats']
    assert [row['iteration'] for row in stats] == list(range(1, 7))
    assert all(row['split'] == 'train' and row['view_id'] in train_ids and row['view_id'] not in heldout_ids
               for row in samples + stats)
    assert all(row['visible'] > 0 and row['denomAfter'] > row['denomBefore'] for row in stats)
    assert all(row['gradientAfter'] > row['gradientBefore'] for row in stats)
    events = [row for row in records if row['event'] == 'density_event']
    assert [row['iteration'] for row in events] == list(GROWTH_ITERATIONS)
    for row in events:
        assert row['after']['points'] == 2 * row['before']['points']
        assert row['after']['points'] <= MAX_POINTS
        assert row['before']['gradientAccumulatorSum'] > 0 and row['before']['denomSum'] > 0
        assert row['after']['gradientAccumulatorSum'] == 0 and row['after']['denomSum'] == 0
    selected = [row for row in records if row['event'] == 'densify_and_' + mode]
    assert len(selected) == len(GROWTH_ITERATIONS)
    assert all(row['afterPoints'] == 2 * row['beforePoints'] for row in selected)
    other = 'split' if mode == 'clone' else 'clone'
    assert all(row['afterPoints'] == row['beforePoints'] for row in records if row['event'] == 'densify_and_' + other)
    saved = [row for row in records if row['event'] == 'checkpoint_saved']
    restored = [row for row in records if row['event'] == 'restore_verified']
    assert [row['iteration'] for row in saved] == list(PAUSE_ITERATIONS)
    assert [row['iteration'] for row in restored] == list(PAUSE_ITERATIONS)
    for before, after in zip(saved, restored):
        assert before['points'] == after['model']['points']
        assert before['modelAdamStatisticsSha256'] == after['model']['modelAdamStatisticsSha256']
        for key in ('cameraNames', 'pendingIndices', 'cameraContractSha256'):
            assert before[key] == after[key]
        assert after['optimizerStatisticsRngExact']
    final = verify_output(work / 'model', prepared, TOTAL)
    assert final['points'] == MAX_POINTS
    return {'densityEvents': events, 'statisticsCalls': len(stats), 'splitIsolationVerified': True,
            'fullStateResumeVerifiedIterations': list(PAUSE_ITERATIONS), 'maxPoints': MAX_POINTS,
            'mode': mode, **final}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output')
    parser.add_argument('--backend', choices=('3dgs', '2dgs', 'both'), default='both')
    parser.add_argument('--mode', choices=('clone', 'split', 'both'), default='both')
    parser.add_argument('--plan-only', action='store_true')
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--entry', help=argparse.SUPPRESS)
    parser.add_argument('--audit', help=argparse.SUPPRESS)
    parser.add_argument('--pause-after', type=int, default=0, help=argparse.SUPPRESS)
    args, trainer_args = parser.parse_known_args()
    if args.child:
        if trainer_args and trainer_args[0] == '--':
            trainer_args.pop(0)
        return run_child(args, trainer_args)
    if trainer_args or not args.output:
        parser.error('--output is required; unexpected trainer arguments are child-only')
    from native.backend import camera_dataset, compute
    from native.worker.canonical_training import prepare_training_dataset
    from native.worker.training_checkpoint import atomic_json, read_manifest
    from scripts.verify_camera_scale_gpu import fixture
    destination = Path(args.output).resolve()
    if destination.drive.upper() != 'D:':
        raise ValueError('Acceptance-owned new output must be on D:')
    destination.mkdir(parents=True, exist_ok=False)
    data_root = destination / 'generated-input'
    data = fixture(data_root, dict(name='density', size=.1, unit='m', mpu=1.))
    data['initialization']['count'] = INITIAL_POINTS
    data = camera_dataset.refresh_dataset_identity(data)
    data = camera_dataset.validate_camera_dataset(data, data_root)
    camera_dataset.write_camera_dataset(data, data_root / 'camera_dataset.json')
    backends = ('3dgs', '2dgs') if args.backend == 'both' else (args.backend,)
    modes = ('clone', 'split') if args.mode == 'both' else (args.mode,)
    evidence = {'schema': 'gsw.acceptance_density_resume/1', 'gpuExecuted': False, 'cases': [],
                'scope': 'bounded_real_density_and_optimizer_resume_not_reconstruction_quality',
                'productionSourceChanged': False, 'observation': 'sys.setprofile_no_function_replacement',
                'iterations': TOTAL, 'initialPoints': INITIAL_POINTS, 'maxPoints': MAX_POINTS,
                'expectedGrowthIterations': list(GROWTH_ITERATIONS),
                'expectedPauseIterations': list(PAUSE_ITERATIONS)}
    for backend in backends:
        for mode in modes:
            work = destination / backend / mode
            prepared = prepare_training_dataset(data_root, work / 'prepared', {'resolutionPolicy': 'preserve'})
            output = work / 'model'
            env = compute.training_env(backend)
            env.update(GSW_CAMERA_DATASET=prepared['manifestPath'], GSW_CAMERA_IMAGE_ROOT=prepared['imageRoot'],
                GSW_POSE_POLICY='locked', PYTHONUTF8='1', PYTHONIOENCODING='utf-8',
                TEMP='D:/CodexData/Temp', TMP='D:/CodexData/Temp')
            if backend == '2dgs':
                env['GSW_TWO_DGS_SOURCE'] = str(compute.TWO_DGS_DIR)
            source = compute.GAUSSIAN_DIR if backend == '3dgs' else compute.TWO_DGS_DIR
            entry = source / 'train.py' if backend == '3dgs' else ROOT / 'native/worker/two_dgs_train.py'
            train = ['-s', str(data_root), '-m', str(output), '-r', '1', '--data_device', 'cpu',
                '--iterations', str(TOTAL), '--densify_from_iter', '1', '--densification_interval', '2',
                '--densify_until_iter', '7', '--densify_grad_threshold', '0',
                '--percent_dense', '100' if mode == 'clone' else '0', '--opacity_reset_interval', '1000',
                '--test_iterations', str(TOTAL), '--save_iterations', str(TOTAL)]
            if backend == '3dgs':
                train.extend(['--disable_viewer', '--optimizer_type', 'default'])
            case = {'backend': backend, 'mode': mode, 'command': train, 'prepared': prepared['manifestPath']}
            evidence['cases'].append(case)
            atomic_json(destination / 'RESULTS.json', evidence)
            if args.plan_only:
                continue
            phases = []
            for index, (phase, pause_at, expected) in enumerate((('pause3', 3, 75), ('pause5', 5, 75), ('finish', 0, 0))):
                request = work / (phase + '-request.json')
                control = dict(output=str(output), request=str(request), session=uuid.uuid4().hex,
                    backend=backend, resume=index > 0, performance={'enabled': False})
                env['GSW_NATIVE_TRAINING_CONTROL'] = json.dumps(control)
                command = [compute.training_python(backend), '-B', Path(__file__).resolve(), '--child',
                    '--entry', entry, '--audit', work / (phase + '.jsonl'), '--pause-after', str(pause_at), '--', *train]
                phases.append(execute(command, source, env, work / (phase + '.log'), expected))
                if pause_at:
                    manifest, _ = read_manifest(output)
                    assert manifest['iteration'] == pause_at
                    request.unlink()
            case.update(phases=phases, **verify_audits(work, prepared, mode))
            evidence['gpuExecuted'] = True
            atomic_json(destination / 'RESULTS.json', evidence)
            print(json.dumps({'backend': backend, 'mode': mode, 'accepted': True, 'points': case['points']}), flush=True)
    print(str(destination / 'RESULTS.json'))


if __name__ == '__main__':
    main()
