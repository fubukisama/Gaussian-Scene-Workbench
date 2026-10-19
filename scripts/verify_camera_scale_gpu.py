"""Explicit, bounded P0 GPU acceptance on independent generated data only.

This is an opt-in acceptance tool, not a physical reconstruction benchmark.
It never discovers or reads user photos/projects/checkpoints.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

import numpy as np
from PIL import Image
from plyfile import PlyData

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from native.backend import compute, camera_dataset
from native.worker.canonical_training import prepare_training_dataset, transform_points
from native.worker.training_checkpoint import read_manifest
from scripts.camera_scale_fixtures import adapter_fixture


def atomic_json(path, value):
    from native.worker.training_checkpoint import atomic_json as write
    write(path, value)


def fixture(root, case):
    root.mkdir(parents=True, exist_ok=False)
    scale = case['size']
    origin = np.asarray(case.get('origin', [0., 0., 0.]), dtype=np.float64)
    views = []
    for index, split in enumerate(('train', 'train', 'val', 'test')):
        width, height = ((128, 96) if index % 2 == 0 else (160, 112))
        relative = split + '/view-' + str(index) + '/same.png'
        target = root / relative
        target.parent.mkdir(parents=True)
        y, x = np.indices((height, width))
        pixels = np.stack(((x * 2 + index * 11) % 256, (y * 3 + index * 7) % 256,
                           ((x + y) * 2 + 30) % 256), axis=-1).astype('uint8')
        Image.fromarray(pixels).save(target)
        pose = np.eye(4)
        pose[:3, 3] = origin + [(-.2 if index == 0 else .2) * scale, 0., -1.5 * scale]
        views.append(dict(image_path=relative, width=width, height=height,
                          K=[[width * .9, 0., width * .43], [0., height * 1.2, height * .47], [0., 0., 1.]],
                          c2w_opencv=pose.tolist(), split=split, pixel_coordinates='pixel_border'))
    bounds = [(origin + [-.4 * scale, -.3 * scale, -.2 * scale]).tolist(),
              (origin + [.4 * scale, .3 * scale, .2 * scale]).tolist()]
    payload = dict(schema='gsw.camera_dataset/1', pose_policy='locked',
                   source={'kind': 'synthetic_acceptance', 'world_axes': 'right-handed z-up'},
                   scale={'status': 'source_declared', 'source_unit': case['unit'],
                          'meters_per_unit': case['mpu'],
                          'evidence': [{'kind': 'generated_fixture', 'description': 'Declared synthetic coordinates; not measured geometry'}]},
                   views=views, initialization={'kind': 'random', 'count': 256, 'seed': 19,
                                                'frame': 'source', 'bounds': bounds})
    raw = root / 'camera_dataset.json'
    atomic_json(raw, payload)
    data = camera_dataset.load_camera_dataset(raw)
    camera_dataset.write_camera_dataset(data, raw)
    return data


def execute(command, cwd, env, log, expected):
    started = time.monotonic()
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.Popen([str(x) for x in command], cwd=cwd, env=env,
                                   stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=15)
            raise RuntimeError('Acceptance-owned short training timed out: ' + str(log))
    if code != expected:
        raise RuntimeError('Expected exit {}, got {}: {}\n{}'.format(expected, code, log, log.read_text(encoding='utf-8')[-6000:]))
    return {'exit': code, 'seconds': time.monotonic() - started, 'log': str(log)}


def verify_output(output, prepared, total):
    contract = json.loads((output / 'training-camera-contract.json').read_text(encoding='utf-8'))
    assert contract['input_data_identity'] == prepared['contract']['input_data_identity']
    assert contract['splits'] == prepared['contract']['splits']
    assert len(contract['loaded_cameras']) == 4
    by_id = {v['view_id']: v for v in prepared['contract']['views']}
    for actual in contract['loaded_cameras']:
        planned = by_id[actual['view_id']]
        assert actual['image_size'] == planned['training_size']
        assert actual['K'] == planned['training_K']
        assert actual['calibration_requires_grad'] is False
    samples = [json.loads(line) for line in (output / 'training-camera-samples.jsonl').read_text(encoding='utf-8').splitlines()]
    assert samples and {s['iteration'] for s in samples} >= set(range(1, total + 1))
    assert all(s['split'] == 'train' and s['view_id'] in contract['splits']['train'] for s in samples)
    assert not ({s['view_id'] for s in samples} & set(contract['splits']['val'] + contract['splits']['test']))
    ply = output / 'point_cloud' / ('iteration_' + str(total)) / 'point_cloud.ply'
    vertices = PlyData.read(str(ply))['vertex'].data
    assert len(vertices) > 0
    xyz = np.column_stack([vertices[x] for x in ('x', 'y', 'z')]).astype('float64')
    assert np.isfinite(xyz).all()
    for name in vertices.dtype.names:
        if name.startswith('scale_'):
            assert np.isfinite(vertices[name]).all()
    sidecar = json.loads(ply.with_suffix('.camera-scale.json').read_text(encoding='utf-8'))
    assert sidecar['input_data_identity'] == contract['input_data_identity']
    normalization = contract['transforms']['canonical_to_training']
    canonical = transform_points(normalization['inverse'], xyz)
    roundtrip = transform_points(normalization, canonical)
    error = float(np.max(np.abs(roundtrip - xyz)))
    assert error < 2e-7, error
    return {'points': len(vertices), 'sampleCount': len(samples), 'sampledTrainViews': sorted({s['view_id'] for s in samples}),
            'validationExcluded': True, 'testExcluded': True, 'coordinateRoundtripMaxError': error,
            'physicalGeometryAccuracyVerified': False, 'model': str(ply),
            'sha256': hashlib.sha256(ply.read_bytes()).hexdigest(), 'loadedCameras': contract['loaded_cameras']}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--backend', choices=('3dgs', '2dgs', 'both'), default='both')
    parser.add_argument('--cases', choices=('single', 'matrix'), default='matrix')
    parser.add_argument('--sources', choices=('generic', 'adapter_matrix', 'all'), default='generic')
    parser.add_argument('--fixtures-only', action='store_true', help='CPU-only source conversion and preparation; no trainer process')
    args = parser.parse_args()
    destination = Path(args.output).resolve()
    if destination.drive.upper() != 'D:':
        raise ValueError('Acceptance output must be on D:')
    destination.mkdir(parents=True, exist_ok=False)
    cases = [dict(name='small-m', size=.05, unit='m', mpu=1.),
             dict(name='millimeters', size=500., unit='mm', mpu=.001),
             dict(name='large-origin', size=.5, unit='m', mpu=1., origin=[1e6, 2e6, 3e6])]
    adapters = [dict(name='blender-transforms', adapter='blender_transforms', size=.05, unit='m', mpu=1.),
                dict(name='colmap-text', adapter='colmap_text', size=300., unit='cm', mpu=.01),
                dict(name='colmap-binary', adapter='colmap_binary', size=500., unit='mm', mpu=.001),
                dict(name='research-json', adapter='research_json', size=20., unit='m', mpu=1., origin=[1e6, 2e6, 3e6])]
    if args.sources == 'adapter_matrix':
        cases = adapters
    elif args.sources == 'all':
        cases += adapters
    if args.cases == 'single':
        cases = cases[:1]
    backends = ('3dgs', '2dgs') if args.backend == 'both' else (args.backend,)
    evidence = {'version': 1, 'sourceRoot': str(ROOT), 'cases': [], 'scope': 'bounded_real_training_not_geometry_accuracy'}
    for case in cases:
        data_root = destination / case['name'] / 'dataset'
        data = adapter_fixture(data_root, case) if case.get('adapter') else fixture(data_root, case)
        if args.fixtures_only:
            prepared = prepare_training_dataset(data_root, destination / case['name'] / 'cpu-prepared',
                                                 {'resolutionPolicy': 'preserve'})
            assert [len(data['splits'][name]) for name in ('train', 'val', 'test')] == [2, 1, 1]
            assert any(view['training_size'][0] > 1600 for view in prepared['contract']['views']) if case.get('adapter') else True
            evidence['cases'].append(dict(case=case, sourceAdapter=data['source']['adapter'],
                fixtureOnly=True, gpuExecuted=False, inputIdentity=data['data_identity'],
                trainingDimensions=[view['training_size'] for view in prepared['contract']['views']],
                clipping=prepared['contract']['clipping']))
            atomic_json(destination / 'RESULTS.json', evidence)
            continue
        for backend in backends:
            work = destination / case['name'] / backend
            prepared = prepare_training_dataset(data_root, work / 'prepared', {'resolutionPolicy': 'preserve'})
            output = work / 'model'
            env = compute.training_env(backend)
            env.update(GSW_CAMERA_DATASET=prepared['manifestPath'], GSW_CAMERA_IMAGE_ROOT=prepared['imageRoot'],
                       GSW_POSE_POLICY='locked', PYTHONUTF8='1', PYTHONIOENCODING='utf-8',
                       TEMP='D:/CodexData/Temp', TMP='D:/CodexData/Temp')
            if backend == '2dgs':
                env['GSW_TWO_DGS_SOURCE'] = str(compute.TWO_DGS_DIR)
            python = compute.training_python(backend)
            source = compute.GAUSSIAN_DIR if backend == '3dgs' else compute.TWO_DGS_DIR
            entry = source / 'train.py' if backend == '3dgs' else ROOT / 'native/worker/two_dgs_train.py'
            total = 12
            command = [python, '-B', entry, '-s', data_root, '-m', output, '-r', '1', '--data_device', 'cpu',
                       '--iterations', str(total), '--densify_until_iter', '0', '--test_iterations', str(total),
                       '--save_iterations', str(total)]
            if backend == '3dgs':
                command.append('--disable_viewer')
            session = uuid.uuid4().hex
            request = work / 'pause.json'
            atomic_json(request, {'session': session})
            control = dict(output=str(output), request=str(request), session=session, backend=backend,
                           resume=False, performance={'enabled': False})
            env['GSW_NATIVE_TRAINING_CONTROL'] = json.dumps(control)
            pause_result = execute(command, source, env, work / 'pause.log', 75)
            manifest, _ = read_manifest(output)
            assert manifest['camera_contract']['input_data_identity'] == data['data_identity']
            request.unlink()
            control['resume'] = True
            control['session'] = uuid.uuid4().hex
            env['GSW_NATIVE_TRAINING_CONTROL'] = json.dumps(control)
            resume_result = execute(command, source, env, work / 'resume.log', 0)
            result = verify_output(output, prepared, total)
            evidence['cases'].append(dict(case=case, backend=backend, pauseIteration=manifest['iteration'],
                                          fullStatePause=pause_result, fullStateResume=resume_result, **result))
            atomic_json(destination / 'RESULTS.json', evidence)
            print(json.dumps({'case': case['name'], 'backend': backend, 'accepted': True, 'points': result['points']}), flush=True)
    print(str(destination / 'RESULTS.json'))


if __name__ == '__main__':
    main()
