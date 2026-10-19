"""Opt-in native worker acceptance using only independent generated data."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.verify_camera_scale_gpu import fixture, verify_output, atomic_json
from native.backend import compute
from native.worker.training_checkpoint import read_manifest


def run_worker(config_path, environment, log, audit, resume=False, pause=False, expected=0):
    # Python's audit hook observes real process creation; no training or COLMAP
    # function is replaced by a test double.
    bootstrap = (
        "import sys,json; from pathlib import Path; "
        "audit=Path(sys.argv.pop(1)); "
        "sys.addaudithook(lambda event,args: audit.open('a',encoding='utf-8').write("
        "json.dumps([str(x) for x in args[:3]],ensure_ascii=False)+'\\n') if event=='subprocess.Popen' else None); "
        "from native.worker.gsw_worker import main; raise SystemExit(main())")
    command = [sys.executable, '-B', '-c', bootstrap, str(audit), '--config', str(config_path)]
    if resume:
        command.append('--resume-training')
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.Popen(command, cwd=ROOT, env=environment, stdin=subprocess.PIPE,
                                   stdout=stream, stderr=subprocess.STDOUT, text=True, encoding='utf-8')
        if pause:
            process.stdin.write('pause\n')
            process.stdin.flush()
        try:
            code = process.wait(timeout=240)
        except subprocess.TimeoutExpired:
            process.stdin.write('cancel\n')
            process.stdin.flush()
            process.wait(timeout=30)
            raise RuntimeError('Owned worker timeout: ' + str(log))
        finally:
            process.stdin.close()
    if code != expected:
        raise RuntimeError('Worker exit {} expected {}: {}\n{}'.format(code, expected, log,
                           log.read_text(encoding='utf-8')[-8000:]))
    return {'exit': code, 'log': str(log)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    destination = Path(args.output).resolve()
    if destination.drive.upper() != 'D:':
        raise ValueError('D: output required')
    destination.mkdir(parents=True, exist_ok=False)
    data_root = destination / 'dataset'
    data = fixture(data_root, dict(size=.1, unit='m', mpu=1.))
    evidence = {'version': 1, 'cases': []}
    for backend in ('3dgs', '2dgs'):
        work = destination / backend
        work.mkdir()
        config = dict(repositoryRoot=str(ROOT), datasetPath=str(data_root), outputRoot=str(work / 'outputs'),
                      outputScene='owned-camera-short-training',
                      backend=backend, quality='quick', runColmap=False, overwrite=False,
                      jobStore=str(work / 'jobs'), nativeCheckpoint=True, posePolicy='locked',
                      cameraDatasetPath=str(data_root / 'camera_dataset.json'),
                      cameraDatasetIdentity=data['data_identity'], resolutionPolicy='preserve',
                      trainOptions={'iterations': 1000, 'resolution': 4, 'densify_until_iter': 0,
                                    'optimizer_type': 'default'})
        config_path = work / 'worker.json'
        atomic_json(config_path, config)
        environment = os.environ.copy()
        environment.update(PYTHONUTF8='1', PYTHONIOENCODING='utf-8',
                           COLMAP_PATH=str(work / 'COLMAP-MUST-NOT-RUN.exe'),
                           COLMAP_EXE=str(work / 'COLMAP-MUST-NOT-RUN.exe'))
        audit = work / 'process-audit.jsonl'
        paused = run_worker(config_path, environment, work / 'pause.log', audit, pause=True, expected=75)
        output = Path(config['outputRoot']) / config['outputScene']
        manifest, _ = read_manifest(output)
        assert manifest['camera_contract']['input_data_identity'] == data['data_identity']
        # Changing a held-out image must invalidate complete-state resume, even
        # though that image is excluded from all training updates.
        heldout = next(v for v in data['views'] if v['split'] == 'test')
        image = data_root / heldout['image_path']
        original = image.read_bytes()
        try:
            image.write_bytes(original + b'changed-test-input')
            rejected = run_worker(config_path, environment, work / 'changed-input.log', audit,
                                  resume=True, expected=1)
        finally:
            image.write_bytes(original)
        resumed = run_worker(config_path, environment, work / 'resume.log', audit, resume=True, expected=0)
        prepared_path = output / '.gsw/camera-input/training-camera-dataset.json'
        payload = json.loads(prepared_path.read_text(encoding='utf-8'))
        actual = verify_output(output, {'contract': payload['contract']}, 1000)
        assert payload['contract']['resolution_policy'] == 'preserve'
        assert all(view['training_size'] == [view['width'], view['height']] for view in payload['contract']['views'])
        audit_text = audit.read_text(encoding='utf-8')
        assert 'COLMAP-MUST-NOT-RUN' not in audit_text
        assert 'train.py' in audit_text or 'two_dgs_train.py' in audit_text
        evidence['cases'].append(dict(backend=backend, pause=paused, changedInputRejected=rejected,
                                      resume=resumed, colmapLaunchCount=0, processAudit=str(audit), **actual))
        atomic_json(destination / 'RESULTS.json', evidence)
        print(json.dumps({'backend': backend, 'workerAccepted': True, 'colmapLaunchCount': 0}), flush=True)


if __name__ == '__main__':
    main()
