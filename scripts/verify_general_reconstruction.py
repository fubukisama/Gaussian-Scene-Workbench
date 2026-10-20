"""Opt-in, bounded real train -> depth geometry -> independent evaluation.

All inputs are new analytical-plane image fixtures. Reference geometry lives
outside datasets and is first loaded by the evaluator after extraction. No
application, original photograph, Blender project or old checkpoint is opened.
This tests software contracts, not long-run convergence or physical accuracy.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from native.backend import camera_dataset, compute
from native.backend.geometry_evaluation import evaluate_geometry
from native.worker.initialization import create_initialized_dataset_version
from native.worker.training_checkpoint import atomic_json
from scripts.verify_camera_scale_gpu import verify_output


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def plane_images(root):
    """A test-only finite diffuse plane, not a product initialization rule."""
    root.mkdir()
    views = []
    for index, (position, split) in enumerate((([-.65, -.25, -2.4], 'train'),
            ([.6, .15, -2.8], 'train'), ([.1, -.55, -2.5], 'val'), ([-.2, .5, -2.6], 'test'))):
        width, height = (128, 96) if index % 2 == 0 else (144, 104)
        centre = np.asarray(position, dtype=np.float64)
        forward = -centre / np.linalg.norm(centre)
        right = np.cross([0., 1., 0.], forward); right /= np.linalg.norm(right)
        down = np.cross(forward, right)
        pose = np.eye(4); pose[:3, :3] = np.column_stack((right, down, forward)); pose[:3, 3] = centre
        k = np.asarray([[width * .88, 0., width * .43], [0., height * 1.18, height * .47], [0., 0., 1.]])
        columns, rows = np.meshgrid(np.arange(width) + .5, np.arange(height) + .5)
        rays = np.stack(((columns - k[0, 2]) / k[0, 0], (rows - k[1, 2]) / k[1, 1], np.ones_like(rows)), axis=-1)
        rays = rays @ pose[:3, :3].T
        z = -centre[2] / rays[:, :, 2]
        hits = centre + z[:, :, None] * rays
        valid = (z > 0) & (np.abs(hits[:, :, 0]) <= 1.5) & (np.abs(hits[:, :, 1]) <= 1.5)
        x, y = hits[:, :, 0], hits[:, :, 1]
        texture = np.stack((120 + 80 * np.sin(5. * x), 125 + 75 * np.cos(4. * y),
                            120 + 65 * np.sin(3. * x + 2. * y)), axis=-1)
        pixels = np.where(valid[:, :, None], np.clip(texture, 0, 255), 0).astype('uint8')
        relative = split + '/image-' + str(index) + '.png'
        path = root / relative; path.parent.mkdir(exist_ok=True)
        Image.fromarray(pixels, 'RGB').save(str(path))
        views.append({'view_id': 'plane-camera-' + str(index), 'image_path': relative,
            'width': width, 'height': height, 'split': split, 'K': k.tolist(),
            'c2w_opencv': pose.tolist(), 'pixel_coordinates': 'pixel_border'})
    raw = {'schema': 'gsw.camera_dataset/1', 'pose_policy': 'locked',
        'source': {'kind': 'generated_acceptance', 'description': 'Synthetic plane images; no measured-object claim'},
        'scale': {'status': 'source_declared', 'source_unit': 'm', 'meters_per_unit': 1.,
            'evidence': [{'kind': 'analytic_fixture', 'description': 'Declared synthetic length, not independent material measurement'}]},
        'views': views, 'initialization': {'kind': 'random', 'count': 128, 'seed': 27,
            'frame': 'source', 'bounds': [[-1.1, -.9, -.4], [1.1, .9, .4]], 'priors': [],
            'provenance': {'source': 'explicit_coarse_test_volume_not_reference_surface'}}}
    manifest = root / 'camera_dataset.json'
    camera_dataset.write_camera_dataset(raw, manifest)
    data = camera_dataset.load_camera_dataset(manifest)
    camera_dataset.write_camera_dataset(data, manifest)
    return data


def execute(command, log, environment, timeout=240):
    before = time.monotonic()
    with log.open('x', encoding='utf-8') as stream:
        process = subprocess.Popen([str(x) for x in command], cwd=str(ROOT), env=environment,
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            result = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.stdin.write(b'cancel\n'); process.stdin.flush()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=10)  # Only this owned bounded acceptance child.
            raise RuntimeError('Owned acceptance timed out: ' + str(log))
        finally:
            process.stdin.close()
    if result:
        raise RuntimeError('Owned acceptance failed {}: {}\n{}'.format(result, log, log.read_text(encoding='utf-8')[-10000:]))
    return {'exit': result, 'seconds': time.monotonic() - before, 'log': str(log)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--backend', choices=('3dgs', '2dgs', 'both'), default='both')
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if output.drive.upper() != 'D:':
        raise ValueError('D output required')
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'generated-source'
    data = plane_images(source)
    initialization = create_initialized_dataset_version(source, {
        'schema': 'gsw.initialization_request/1', 'input_data_identity': data['data_identity'],
        'backend': '3dgs', 'resolution': 1, 'initialization': data['initialization']},
        output / 'reusable-initialization', expected_identity=data['data_identity'])
    dataset = Path(initialization['datasetPath'])
    initial_info = initialization['report']
    # Reference is never stored inside the admitted dataset or passed to train/extract.
    reference = {'kind': 'plane_patch', 'origin': [0., 0., 0.], 'axis_u': [1., 0., 0.], 'axis_v': [0., 1., 0.],
        'range_u': [-1.5, 1.5], 'range_v': [-1.5, 1.5], 'frame': 'analytic-reference', 'unit': 'm',
        'to_evaluation': {'s': 1., 'R': np.eye(3).tolist(), 't': [0., 0., 0.],
            'input_frame': 'analytic-reference', 'output_frame': 'analytic-reference'},
        'provenance': {'role': 'independent_reference', 'source': 'analytical image-generation surface',
            'scope': 'synthetic_software_regression_not_physical_model_accuracy'}}
    atomic_json(output / 'independent-reference.json', reference)
    source_hashes = {p.relative_to(source).as_posix(): sha(p) for p in source.rglob('*') if p.is_file()}
    dataset_hashes = {p.relative_to(dataset).as_posix(): sha(p) for p in dataset.rglob('*') if p.is_file()}
    evidence = {'schema': 'gsw.general_reconstruction_acceptance/1', 'cases': [],
        'initialization_point_sha256': initial_info['point_sha256'], 'initialization_kind': 'random',
        'scope': 'bounded_actual_training_and_geometry_evaluation_not_long_run_or_physical_accuracy',
        'reference_feedback_to_initialization_training_extraction': False}
    backends = ('3dgs', '2dgs') if args.backend == 'both' else (args.backend,)
    for backend in backends:
        work = output / backend; work.mkdir()
        config = {'task': 'training', 'repositoryRoot': str(ROOT), 'projectRoot': str(work),
            'datasetPath': str(dataset), 'cameraDatasetPath': str(dataset / 'camera_dataset.json'),
            'cameraDatasetIdentity': initialization['summary']['dataIdentity'], 'posePolicy': 'locked',
            'resolutionPolicy': 'preserve', 'outputRoot': str(work / 'training'), 'outputScene': 'generated-plane',
            'jobStore': str(work / 'jobs'), 'backend': backend, 'quality': 'quick', 'trainingSeed': 31,
            'runColmap': False, 'nativeCheckpoint': True, 'overwrite': False,
            'performanceOptions': {'enabled': False},
            'trainOptions': {'iterations': 1000, 'resolution': 1, 'densify_grad_threshold': .00001,
                'densification_interval': 100, 'densify_until_iter': 901}}
        atomic_json(work / 'training-request.json', config)
        env = compute.training_env(backend)
        env.update(PYTHONUTF8='1', PYTHONIOENCODING='utf-8', TEMP='D:/CodexData/Temp', TMP='D:/CodexData/Temp',
            COLMAP_EXE=str(work / 'COLMAP-MUST-NOT-RUN.exe'), COLMAP_PATH=str(work / 'COLMAP-MUST-NOT-RUN.exe'))
        if backend == '2dgs': env['GSW_TWO_DGS_SOURCE'] = str(compute.TWO_DGS_DIR)
        execution = execute([sys.executable, '-B', ROOT / 'native/worker/gsw_worker.py',
            '--config', work / 'training-request.json'], work / 'training.log', env)
        model = Path(config['outputRoot']) / config['outputScene']
        prepared = json.loads((model / '.gsw/camera-input/training-camera-dataset.json').read_text(encoding='utf-8'))
        assert prepared['contract']['initialization']['point_sha256'] == initial_info['point_sha256']
        actual = verify_output(model, {'contract': prepared['contract']}, 1000)
        model_hash = actual['sha256']
        extracts, common_training_points = [], None
        for mode in ('canonical', 'source', 'training'):
            geometry = {'schema': 'gsw.gs_geometry_request/1', 'model_directory': str(model),
                'dataset_path': str(dataset), 'iteration': 1000, 'output_directory': str(work / ('geometry-' + mode)),
                'coordinate_mode': mode, 'alpha_threshold': .05, 'pixel_stride': 1,
                'max_points': 100000, 'memory_budget_mb': 128}
            atomic_json(work / ('extract-' + mode + '.json'), geometry)
            run = execute([compute.training_python(backend), '-B', ROOT / 'native/worker/gs_geometry.py',
                '--config', work / ('extract-' + mode + '.json'), '--report', work / ('receipt-' + mode + '.json')],
                work / ('extract-' + mode + '.log'), env)
            manifest = Path(geometry['output_directory']) / 'manifest.json'
            product = json.loads(manifest.read_text(encoding='utf-8'))
            assert product['geometry']['point_count'] > 0
            assert {v['view_id'] for v in product['views']} == set(prepared['contract']['splits']['train'])
            assert sha(actual['model']) == model_hash
            from plyfile import PlyData
            vertices = PlyData.read(str(manifest.parent / product['geometry']['path']))['vertex'].data
            points = np.column_stack([vertices[name] for name in ('x', 'y', 'z')]).astype(np.float64)
            transform = product['frames']['transform']
            in_training = (points - transform['t']) @ np.asarray(transform['R']) / transform['s']
            if common_training_points is None:
                common_training_points = in_training
            assert common_training_points.shape == in_training.shape
            frame_error = float(np.max(np.abs(common_training_points - in_training)))
            assert frame_error < 1e-9, frame_error
            extracts.append({'frame': mode, 'execution': run, 'manifest': str(manifest),
                'point_count': product['geometry']['point_count'], 'geometry_sha256': product['geometry']['sha256'],
                'common_training_frame_max_error': frame_error})
            if mode == 'canonical':
                transform = {'s': 1., 'R': np.eye(3).tolist(), 't': [0., 0., 0.],
                    'input_frame': product['geometry']['frame_id'], 'output_frame': 'analytic-reference'}
                request = {'schema': 'gsw.geometry_evaluation_request/1',
                    'evaluation_frame': {'id': 'analytic-reference', 'unit': 'm'},
                    'candidate': {'manifest_path': str(manifest), 'to_evaluation': transform}, 'reference': reference,
                    'sampling': {'count': 2000, 'seed': 91, 'reference_grid_shape': [40, 50]},
                    'coverage_threshold': .05, 'quantiles': [0., .5, .9, .95, .99, 1.],
                    'dimensions': [{'id': 'x-extent', 'kind': 'axis_extent', 'axis': [1., 0., 0.]}],
                    'metadata': {'scope': 'synthetic_short_training_no_accuracy_target',
                        'coverage_threshold_is_fixture_parameter_not_research_accuracy_requirement': True}}
                atomic_json(work / 'evaluation-request.json', request)
                evaluation = evaluate_geometry(request, work / 'independent-evaluation')
        evidence['cases'].append({'backend': backend, 'training': execution, 'model': actual,
            'extracts': extracts, 'independent_evaluation': evaluation})
        atomic_json(output / 'RESULTS.json', evidence)
        print(json.dumps({'backend': backend, 'accepted_software_flow': True,
            'gaussians': actual['points'], 'depth_points': extracts[0]['point_count']}), flush=True)
    assert source_hashes == {p.relative_to(source).as_posix(): sha(p) for p in source.rglob('*') if p.is_file()}
    assert dataset_hashes == {p.relative_to(dataset).as_posix(): sha(p) for p in dataset.rglob('*') if p.is_file()}
    evidence['original_generated_inputs_unchanged'] = True
    atomic_json(output / 'RESULTS.json', evidence)
    print(str(output / 'RESULTS.json'))


if __name__ == '__main__':
    main()
