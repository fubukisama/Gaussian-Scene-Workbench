"""Public training-job admission. Actual GPU verification is separate."""
import unittest
import json
import subprocess
import sys
from pathlib import Path
import tempfile

from native.backend import compute


class CameraTrainingJobTests(unittest.TestCase):
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
