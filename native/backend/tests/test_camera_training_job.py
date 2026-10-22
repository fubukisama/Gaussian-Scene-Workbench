"""Public training-job admission. Actual GPU verification is separate."""
import unittest
import builtins
import io
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
import tempfile
from unittest import mock

from PIL import Image

from native.backend import compute


class CameraTrainingJobTests(unittest.TestCase):
    def test_solve_portable_copy_failure_preserves_previous_model_and_checkpoint(self):
        from native.backend import camera_admission
        from native.worker.test_reconstruction_quality import write_model
        base = Path('D:/Apps/GSW/validation/acceptance-followup-oct22/paths/training-unit')
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            source = root / 'source'
            source.mkdir()
            views = []
            for index in range(11):
                image = source / ('photo%d.png' % index)
                Image.new('RGB', (11, 9), (20 + index, 40, 60)).save(image)
                views.append({'image_path': image.name, 'K': [[14., 0., 5.5], [0., 15., 4.5], [0., 0., 1.]],
                    'pixel_coordinates': 'pixel_border', 'split': 'train' if index < 9 else ('val' if index == 9 else 'test'),
                    'c2w_opencv': [[1., 0., 0., index * .2], [0., 1., 0., .1],
                                   [0., 0., 1., -.6], [0., 0., 0., 1.]]})
            manifest = source / 'camera_dataset.json'
            manifest.write_text(json.dumps({'schema': 'gsw.camera_dataset/1', 'pose_policy': 'locked',
                'source': {'kind': 'synthetic'}, 'scale': {'status': 'arbitrary'}, 'views': views}), encoding='utf-8')
            identity = camera_admission.admit_locked_dataset(source)['dataset']['data_identity']
            output = root / 'output' / 'result'
            (output / '.gsw/checkpoints').mkdir(parents=True)
            (output / 'previous.ply').write_bytes(b'previous user model')
            (output / '.gsw/checkpoints/state.bin').write_bytes(b'complete previous optimizer checkpoint')
            (output / 'training_backend.json').write_text(json.dumps({'backend': '3dgs'}), encoding='utf-8')
            job = {'id': 'copy-failure', 'scene': 'source', 'output_scene': 'result', 'backend': '3dgs',
                   'dataset_path': str(source), 'camera_dataset_path': str(manifest),
                   'camera_dataset_identity': identity, 'pose_policy': 'solve', 'train_options': {},
                   'created_at': 0, 'log': [], 'cancel_requested': False}

            class ExternalFixtureProcess:
                def __init__(self, command, **unused):
                    self.stdout = io.StringIO('external-process contract fixture only\n')
                    self.returncode = 0
                    if command[1] == 'mapper':
                        write_model(Path(command[command.index('--output_path') + 1]) / '0')
                    elif command[1] == 'image_undistorter':
                        target = Path(command[command.index('--output_path') + 1])
                        incoming = Path(command[command.index('--image_path') + 1])
                        (target / 'images').mkdir(parents=True)
                        for image in incoming.iterdir():
                            (target / 'images' / image.name).write_bytes(image.read_bytes())
                        write_model(target / 'sparse')
                    elif command[1] not in ('feature_extractor', 'exhaustive_matcher', 'sequential_matcher'):
                        raise AssertionError('training must not start after a portable-copy error')
                def wait(self, **unused): return self.returncode
                def poll(self): return self.returncode
                def terminate(self): pass
                def kill(self): pass

            real_io_open, real_open = io.open, builtins.open
            real_copy2 = shutil.copy2
            real_windows_copy = getattr(getattr(shutil, '_winapi', None), 'CopyFile2', None)
            def fail_copy2(source_file, destination, **options):
                if '/.gsw/solve-dataset/' in os.fsdecode(os.fspath(destination)).replace('\\', '/'):
                    raise OSError('controlled portable copy failure')
                return real_copy2(source_file, destination, **options)
            def fail_windows_copy(source_file, destination, flags):
                if '/.gsw/solve-dataset/' in os.fsdecode(destination).replace('\\', '/'):
                    raise OSError('controlled portable copy failure')
                return real_windows_copy(source_file, destination, flags)
            def fail_portable_copy(real, file, mode='r', *args, **kwargs):
                if isinstance(file, (str, bytes, os.PathLike)) and any(flag in mode for flag in 'wax'):
                    if '/.gsw/solve-dataset/' in os.fsdecode(os.fspath(file)).replace('\\', '/'):
                        raise OSError('controlled portable copy failure')
                return real(file, mode, *args, **kwargs)

            with mock.patch.object(shutil._winapi, 'CopyFile2', side_effect=fail_windows_copy) if real_windows_copy else mock.patch.object(shutil, 'copy2', side_effect=fail_copy2), \
                 mock.patch.object(compute, 'OUTPUT_DIR', root / 'output'), \
                 mock.patch.object(compute, 'TRAIN_JOBS_DIR', root / 'jobs'), \
                 mock.patch.dict(os.environ, {'COLMAP_EXE': sys.executable}), \
                 mock.patch.object(subprocess, 'run', side_effect=lambda command, **unused:
                                   subprocess.CompletedProcess(command, 0, 'external-runtime contract fixture', '')), \
                 mock.patch.object(subprocess, 'Popen', side_effect=ExternalFixtureProcess), \
                 mock.patch.object(shutil, 'copy2', side_effect=fail_copy2), \
                 mock.patch.object(io, 'open', side_effect=lambda file, mode='r', *args, **kwargs:
                                   fail_portable_copy(real_io_open, file, mode, *args, **kwargs)), \
                 mock.patch.object(builtins, 'open', side_effect=lambda file, mode='r', *args, **kwargs:
                                   fail_portable_copy(real_open, file, mode, *args, **kwargs)):
                compute.run_training_job(job, True, 'quick', True)
            self.assertEqual(job['status'], 'failed')
            self.assertIn('controlled portable copy failure', job['error'])
            self.assertEqual((output / 'previous.ply').read_bytes(), b'previous user model')
            self.assertEqual((output / '.gsw/checkpoints/state.bin').read_bytes(),
                             b'complete previous optimizer checkpoint')

    @unittest.skipUnless(os.name == 'nt', 'legacy Windows path admission')
    def test_solve_output_budget_rejects_before_environment_for_both_backends(self):
        from native.backend import camera_admission
        from scripts.verify_camera_scale_gpu import fixture
        base = Path('D:/Apps/GSW/validation/acceptance-followup-oct22/paths/early-budget')
        base.mkdir(parents=True, exist_ok=True)
        for backend in ('3dgs', '2dgs'):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory(dir=base) as temporary:
                root = Path(temporary).resolve()
                data = fixture(root / 'source', dict(size=.1, unit='m', mpu=1.))
                output_root = root / ('p' * (188 - len(str(root))))
                output = output_root / 'result'
                output.mkdir(parents=True)
                (output / 'previous.ply').write_bytes(b'old model remains before admission')
                job = {'id': 'budget', 'scene': 'source', 'output_scene': 'result', 'backend': backend,
                       'dataset_path': str(root / 'source'),
                       'camera_dataset_path': str(root / 'source/camera_dataset.json'),
                       'camera_dataset_identity': data['data_identity'], 'pose_policy': 'solve',
                       'train_options': {}, 'created_at': 0, 'log': [], 'cancel_requested': False}
                with mock.patch.object(compute, 'OUTPUT_DIR', output_root), \
                     mock.patch.object(compute, 'TRAIN_JOBS_DIR', root / 'jobs'), \
                     mock.patch.object(subprocess, 'run', side_effect=AssertionError('no environment process')) as probes, \
                     mock.patch.object(subprocess, 'Popen', side_effect=AssertionError('no solver/trainer process')) as launch:
                    compute.run_training_job(job, True, 'quick', True)
                self.assertEqual(job['status'], 'failed')
                self.assertIn('camera_solve_path_budget_exceeded', job['error'])
                self.assertEqual((output / 'previous.ply').read_bytes(), b'old model remains before admission')
                probes.assert_not_called()
                launch.assert_not_called()

    def test_source_code_identity_normalizes_only_line_endings_not_binary_payloads(self):
        from native.worker.training_checkpoint import file_digest
        base = Path('D:/Codex/qa-camera-p0')
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            a, b = Path(temporary) / 'lf.py', Path(temporary) / 'crlf.py'
            a.write_bytes(b'line1\nline2\n')
            b.write_bytes(b'line1\r\nline2\r\n')
            self.assertNotEqual(file_digest(a), file_digest(b))
            self.assertEqual(file_digest(a, text_source=True), file_digest(b, text_source=True))

    def test_solve_resume_verifies_owned_version_provenance_and_image_bytes(self):
        from native.backend import camera_admission
        from scripts.verify_camera_scale_gpu import fixture
        base = Path('D:/Codex/qa-camera-p0')
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            data = fixture(root / 'source', dict(size=.1, unit='m', mpu=1.))
            output = root / 'output'
            version = camera_admission.prepare_solve_dataset_version(
                root / 'source', output / '.gsw/solve-dataset', data['data_identity'])
            accepted = camera_admission.validate_solve_dataset_version(
                version['datasetPath'], output, data['data_identity'])
            self.assertEqual(accepted['solveDataIdentity'], version['solveDataIdentity'])
            image = Path(version['datasetPath']) / version['files'][0]['inputImagePath']
            image.write_bytes(image.read_bytes() + b'changed')
            with self.assertRaisesRegex(ValueError, 'hash|identity'):
                camera_admission.validate_solve_dataset_version(
                    version['datasetPath'], output, data['data_identity'])

    def test_locked_colmap_conflict_is_rejected_before_queueing(self):
        with self.assertRaisesRegex(ValueError, 'camera_locked_colmap_conflict'):
            compute.start_training('camera-source', 'camera-output', run_convert=True,
                                   pose_policy='locked', camera_dataset_path='camera_dataset.json')

    def test_runtime_probe_does_not_inherit_open_desktop_control_pipe(self):
        # The real UI worker has an input pipe held open for pause/cancel.
        # A dependency probe is non-interactive and must see EOF, not that pipe.
        code = (
            'import sys,json,threading; from native.backend import compute; '
            'threading.Thread(target=lambda:list(sys.stdin),daemon=True).start(); '
            'result=compute.python_probe(sys.executable,compute.os.environ.copy(),'
            + repr("import sys; assert sys.stdin.read() == ''; print('probe-eof')")
            + ',timeout=1); print(json.dumps(result),flush=True)')
        process = subprocess.Popen([sys.executable, '-B', '-c', code],
                                   cwd=Path(__file__).resolve().parents[3],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            result = json.loads(process.stdout.readline())
            self.assertTrue(result[0], result[1])
            self.assertEqual(result[1], 'probe-eof')
        finally:
            process.stdin.close()
            process.wait(timeout=10)
            process.stdout.close()
            process.stderr.close()

    def test_logged_command_does_not_inherit_open_desktop_control_pipe(self):
        code = (
            'import sys,threading; from native.backend import compute; '
            'threading.Thread(target=lambda:list(sys.stdin),daemon=True).start(); '
            "job={'backend':'3dgs','log':[]}; "
            'compute.run_logged(job,[sys.executable,"-c",'
            + repr("import sys; assert sys.stdin.read() == ''; print('command-eof')")
            + '],compute.ROOT); print("complete",flush=True)')
        process = subprocess.Popen([sys.executable, '-B', '-c', code],
                                   cwd=Path(__file__).resolve().parents[3],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.wait(timeout=2), 0)
            self.assertIn('complete', process.stdout.read())
        finally:
            process.stdin.close()
            process.wait(timeout=10)
            process.stdout.close()
            process.stderr.close()


if __name__ == '__main__':
    unittest.main()
