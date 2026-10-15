"""Public converter boundary checks using a real, test-owned Node process.

The receiver is not a splat codec. These tests verify deployment discovery,
literal argv operands, bounded cancellation and refusal of batch-only launches.
They never install Node, start a web server or launch a training runtime.
"""

import json
import errno
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RECEIVER = r"""
import fs from 'node:fs';
import path from 'node:path';
const args = process.argv.slice(2);
if (args.length !== 4 || args[0] !== '-w' || args[1] !== '--' ||
    !path.isAbsolute(args[2]) || !path.isAbsolute(args[3])) {
    console.error('Receiver requires -w -- followed by two absolute operands:', JSON.stringify(args));
    process.exit(65);
}
fs.writeFileSync(args[3] + '.argv.json', JSON.stringify(args));
if (process.env.GSW_COMMAND_FIXTURE_FAIL === '1') {
    console.error('test-owned converter failure');
    process.exit(23);
}
if (process.env.GSW_COMMAND_FIXTURE_WAIT === '1') {
    // A timeout bounds the test-owned process even if its parent fails.
    setTimeout(() => process.exit(0), 4000);
} else {
    fs.copyFileSync(args[2], args[3]);
}
"""
DRIVER = r"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path.cwd() / 'crop_editor'))
import server
result = server.run_splat_transform(Path(sys.argv[1]), Path(sys.argv[2]))
print(json.dumps({'output': str(result)}, ensure_ascii=False))
"""
EXPORT_DRIVER = r"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path.cwd() / 'crop_editor'))
import server
outputs = [server.export_splat_format('scene_splat', 7, fmt) for fmt in ('spz', 'sog')]
print(json.dumps({'outputs': [str(path) for path in outputs]}, ensure_ascii=False))
"""
CANCEL_DRIVER = r"""
import json, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path.cwd() / 'crop_editor'))
import server
job = {'id': 'owned-cancel', 'scene': 'fixture', 'iteration': 7, 'format': 'spz',
       'cancel_requested': False, 'process': None, 'log': []}
observed = []
def request_cancel():
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        process = job.get('process')
        if process is not None:
            observed.append(process)
            job['cancel_requested'] = True
            return
        time.sleep(0.01)
thread = threading.Thread(target=request_cancel)
thread.start()
cancelled = False
try:
    server.run_splat_transform(Path(sys.argv[1]), Path(sys.argv[2]), job=job)
except RuntimeError as exc:
    cancelled = 'cancelled' in str(exc).lower()
thread.join(timeout=3)
print(json.dumps({'cancelled': cancelled, 'cleared': job['process'] is None,
                  'exited': bool(observed) and observed[0].poll() is not None}))
"""
REPORT_DRIVER = r"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path.cwd() / 'crop_editor'))
import server
reports = {backend: server.training_environment_report(backend) for backend in ('3dgs', '2dgs')}
print(json.dumps({backend: {'helper': report['crop_node_helper'],
                           'ready': report['crop_node_helper_exists']}
                  for backend, report in reports.items()}, ensure_ascii=False))
"""


class CommandSecurityTests(unittest.TestCase):
    def setUp(self):
        self.node = shutil.which('node.exe' if os.name == 'nt' else 'node')
        self.assertIsNotNone(self.node, 'These process-boundary checks require an already installed Node.')
        self.node = Path(self.node).resolve()
        self.temporary = tempfile.TemporaryDirectory(prefix='gsw-command-security-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / '模型 space & 100% project'
        self.root.mkdir()
        # Exercise an isolated deployment of the real backend, not mocked Popen.
        for relative in (
            'crop_editor/server.py',
            'crop_editor/reconstruction_quality.py',
            'native/worker/training_summary.py',
            'native/worker/training_step_profile.py',
            'native/worker/training_resource_policy.py',
            'native/worker/training_camera_lookahead.py',
            'native/worker/training_capacity_storage.py',
            'native/worker/training_quality.py',
            'native/worker/surfel_render_adapter.py',
        ):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPOSITORY_ROOT / relative, target)
        self.cli = self.root / 'crop_editor/node_modules/@playcanvas/splat-transform/bin/cli.mjs'
        self.cli.parent.mkdir(parents=True)
        self.cli.write_text(RECEIVER, encoding='utf-8')
        self.shim_marker = self.root / 'legacy-shim-ran.txt'
        shim_directory = self.root / 'crop_editor/node_modules/.bin'
        shim_directory.mkdir()
        if os.name == 'nt':
            shim = shim_directory / 'splat-transform.cmd'
            shim.write_text(
                '@echo off\n'
                f'echo launched>"{self.shim_marker}"\n'
                f'"{self.node}" "{self.cli}" %*\n',
                encoding='utf-8',
            )
        else:
            shim = shim_directory / 'splat-transform'
            import shlex
            shim.write_text(
                '#!/bin/sh\n'
                f'printf launched > {shlex.quote(str(self.shim_marker))}\n'
                f'exec {shlex.quote(str(self.node))} {shlex.quote(str(self.cli))} "$@"\n',
                encoding='utf-8',
            )
            shim.chmod(0o755)
        self.source = self.root / '--input.ply'
        self.source.write_bytes(b'test-owned converter input')
        self.target = self.root / '--output.spz'
        self.environment = os.environ.copy()
        self.environment['PATH'] = str(self.node.parent) + os.pathsep + self.environment.get('PATH', '')
        self.environment['GS_EDITOR_WORKSPACE_ROOT'] = str(self.root / 'workspace')
        self.environment['PYTHONUTF8'] = '1'
        self.environment.pop('PYTHONPATH', None)

    def call_backend(self, driver, *arguments):
        return subprocess.run(
            [sys.executable, '-B', '-c', driver, *[str(argument) for argument in arguments]],
            cwd=self.root,
            env=self.environment,
            capture_output=True,
            text=True,
            encoding='utf-8',
            errors='replace',
            timeout=10,
            check=False,
        )

    def call_transform(self, source=None, target=None):
        return self.call_backend(DRIVER, source or self.source, target or self.target)

    def prepare_report_environment(self):
        # Report probes must never select a user's real CUDA interpreter.
        runtime = self.root / 'test-owned-inactive-runtime'
        runtime.mkdir()
        (runtime / 'python.exe').write_bytes(b'not an executable: report fixture only')
        colmap = self.root / 'test-owned-colmap.exe'
        colmap.write_bytes(b'not an executable: discovery fixture only')
        self.environment.update({
            'GAUSSIAN_SPLATTING_CONDA_PREFIX': str(runtime),
            'GS_CONDA_PREFIX': str(runtime),
            'CONDA_PREFIX': str(runtime),
            'TWO_DGS_PYTHON': str(self.root / 'missing-2dgs-python.exe'),
            'TWO_DGS_DIR': str(self.root / 'missing-2dgs'),
            'COLMAP_EXE': str(colmap),
            'ProgramFiles(x86)': str(self.root / 'missing-program-files'),
        })

    def test_real_converter_receives_special_paths_as_literal_absolute_operands(self):
        outcome = self.call_transform()
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        self.assertEqual(self.target.read_bytes(), b'test-owned converter input')
        argv = json.loads(Path(str(self.target) + '.argv.json').read_text(encoding='utf-8'))
        self.assertEqual(argv, ['-w', '--', str(self.source.resolve()), str(self.target.resolve())])
        self.assertFalse(self.shim_marker.exists(), 'The .cmd/.bin shim must never run.')

    def test_relative_leading_dash_names_are_passed_as_absolute_file_operands(self):
        outcome = self.call_transform(Path('--input.ply'), Path('--output.spz'))
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        self.assertEqual(self.target.read_bytes(), b'test-owned converter input')
        argv = json.loads(Path(str(self.target) + '.argv.json').read_text(encoding='utf-8'))
        self.assertEqual(argv, ['-w', '--', str(self.source.resolve()), str(self.target.resolve())])

    def test_spz_and_sog_export_routes_use_the_real_converter_process(self):
        cloud = self.root / 'workspace/output/scene_splat/point_cloud/iteration_7/point_cloud.ply'
        cloud.parent.mkdir(parents=True)
        cloud.write_bytes(b'test-owned export input')
        outcome = self.call_backend(EXPORT_DRIVER)
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        paths = [Path(path) for path in json.loads(outcome.stdout)['outputs']]
        self.assertEqual([path.suffix for path in paths], ['.spz', '.sog'])
        for path in paths:
            self.assertEqual(path.read_bytes(), b'test-owned export input')
            argv = json.loads(Path(str(path) + '.argv.json').read_text(encoding='utf-8'))
            self.assertEqual(argv, ['-w', '--', str(cloud.resolve()), str(path.resolve())])
        self.assertFalse(self.shim_marker.exists())

    def test_exports_refuse_linked_outputs_outside_the_owned_model(self):
        cloud = self.root / 'workspace/output/scene_splat/point_cloud/iteration_7/point_cloud.ply'
        cloud.parent.mkdir(parents=True)
        cloud.write_bytes(b'test-owned export input')
        outside = self.root / 'outside-original.spz'
        outside.write_bytes(b'original outside the model')
        os.utime(outside, (1, 1))
        try:
            cloud.with_suffix('.spz').symlink_to(outside)
        except OSError as exc:
            if exc.errno in (errno.EPERM, errno.EACCES, errno.ENOSYS) or getattr(exc, 'winerror', None) == 1314:
                self.skipTest('Platform does not permit the owned file symbolic link; Ubuntu CI covers this boundary')
            raise
        outcome = self.call_backend(EXPORT_DRIVER)
        self.assertNotEqual(outcome.returncode, 0, outcome.stdout)
        self.assertIn('Unsupported asset file', outcome.stderr)
        self.assertEqual(outside.read_bytes(), b'original outside the model')
        self.assertEqual(cloud.read_bytes(), b'test-owned export input')
        self.assertFalse(Path(str(outside) + '.argv.json').exists(), 'The converter must not start for an escaping target')

    def test_batch_only_installation_is_refused_without_running_its_shim(self):
        self.cli.unlink()
        outcome = self.call_transform()
        self.assertNotEqual(outcome.returncode, 0)
        self.assertIn('FileNotFoundError: Missing splat-transform.', outcome.stderr)
        self.assertFalse(self.shim_marker.exists(), 'Do not fall back to a .cmd/.bin shim.')
        self.assertFalse(self.target.exists())
        self.assertEqual(self.source.read_bytes(), b'test-owned converter input')

    def test_converter_error_preserves_existing_output_and_input(self):
        self.environment['GSW_COMMAND_FIXTURE_FAIL'] = '1'
        self.target.write_bytes(b'previous output must survive')
        outcome = self.call_transform()
        self.assertNotEqual(outcome.returncode, 0)
        self.assertIn('RuntimeError: splat-transform failed: test-owned converter failure', outcome.stderr)
        self.assertEqual(self.target.read_bytes(), b'previous output must survive')
        self.assertEqual(self.source.read_bytes(), b'test-owned converter input')

    def test_cancel_terminates_only_the_test_owned_converter_and_clears_handle(self):
        self.environment['GSW_COMMAND_FIXTURE_WAIT'] = '1'
        outcome = self.call_backend(CANCEL_DRIVER, self.source, self.target)
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        self.assertEqual(json.loads(outcome.stdout), {'cancelled': True, 'cleared': True, 'exited': True})
        self.assertEqual(self.source.read_bytes(), b'test-owned converter input')

    def test_report_marks_direct_node_and_cli_ready_without_an_npm_shim(self):
        self.prepare_report_environment()
        for shim in (self.root / 'crop_editor/node_modules/.bin').iterdir():
            shim.unlink()
        outcome = self.call_backend(REPORT_DRIVER)
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        expected = {'helper': str(self.cli.resolve()), 'ready': True}
        self.assertEqual(json.loads(outcome.stdout), {'3dgs': expected, '2dgs': expected})

    def test_report_marks_an_npm_shim_without_the_fixed_cli_unready(self):
        self.prepare_report_environment()
        expected_path = str(self.cli.resolve())
        self.cli.unlink()
        outcome = self.call_backend(REPORT_DRIVER)
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        expected = {'helper': expected_path, 'ready': False}
        self.assertEqual(json.loads(outcome.stdout), {'3dgs': expected, '2dgs': expected})
        self.assertFalse(self.shim_marker.exists())

    def test_report_marks_a_cli_without_direct_node_unready(self):
        self.prepare_report_environment()
        # Only the owned subprocess loses Node discovery; host PATH is untouched.
        self.environment['PATH'] = ''
        outcome = self.call_backend(REPORT_DRIVER)
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        expected = {'helper': str(self.cli.resolve()), 'ready': False}
        self.assertEqual(json.loads(outcome.stdout), {'3dgs': expected, '2dgs': expected})
        self.assertFalse(self.shim_marker.exists())


if __name__ == '__main__':
    unittest.main()
