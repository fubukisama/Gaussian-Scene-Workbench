# SPDX-License-Identifier: MIT
"""Real target-Python CPU admission of optional quality kernels, never GPU proof."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

from native.worker.gsw_worker import import_backend, preflight_quality_kernel, run_training
from native.worker.training_quality_plan import resolve_quality_plan


REPOSITORY = Path(__file__).resolve().parents[2]
TRAINING_PYTHON = Path('D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe')
ACCEPTED_ROOT = Path('D:/Codex/builds/gsw-quality-rasterizer-v1-20261023-build3/runtime')


@contextlib.contextmanager
def environment(**values):
    previous = {name: os.environ.get(name) for name in values}
    try:
        os.environ.update(values)
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


class QualityKernelPreflightTests(unittest.TestCase):
    def setUp(self):
        base = Path('D:/Codex/validation/gsw-quality-upgrade-20261023/p8/kernel-preflight-tests')
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.server = import_backend(REPOSITORY)
        self.plan = resolve_quality_plan({'version': 1, 'filter': {'mode': 'full_mip'}})

    def test_actual_training_python_accepts_intact_component_without_torch_or_gpu(self):
        with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GSW_QUALITY_KERNEL_ROOT=str(ACCEPTED_ROOT), CUDA_VISIBLE_DEVICES='17'):
            before = self.server.training_env('3dgs')
            report = preflight_quality_kernel(self.server, {'qualityPlan': self.plan},
                                              '3dgs', REPOSITORY)
            self.assertEqual(self.server.training_env('3dgs'), before)
        self.assertEqual(report['python_version'], '3.8.20')
        self.assertEqual(Path(report['target_python']), TRAINING_PYTHON.resolve())
        self.assertEqual(report['target_python_sha256'], hashlib.sha256(TRAINING_PYTHON.read_bytes()).hexdigest())
        self.assertEqual(Path(report['root']), ACCEPTED_ROOT.resolve())
        self.assertTrue(report['cpu_only'])
        self.assertFalse(report['torch_imported'])
        self.assertFalse(report['gpu_executed'])
        self.assertFalse(report['quality_accepted'])
        self.assertEqual(report['cuda_visible_devices'], '-1')

    def test_worker_rejects_missing_component_before_creating_a_training_job(self):
        config = dict(repositoryRoot=str(REPOSITORY), outputRoot=str(self.root / 'output'),
            jobStore=str(self.root / 'jobs'), outputScene='owned-fixture',
            datasetPath=str(self.root / 'absent-dataset'), backend='3dgs', quality='quick',
            nativeCheckpoint=True, qualityPlan=self.plan['requested'], runColmap=False,
            # Independent CPU guard: if the pre-enqueue call is missing, the
            # real backend refuses this incomplete camera contract before it
            # can create a job, import Torch or launch any training process.
            posePolicy='locked')
        with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GSW_QUALITY_KERNEL_ROOT=str(self.root / 'missing-component')):
            with self.assertRaisesRegex(ValueError, '^quality_rasterizer_build_missing'):
                run_training(config)
        self.assertEqual(list((self.root / 'jobs').glob('*.json')), [])

    def test_old_abi_missing_files_and_damaged_acceptance_fail_closed(self):
        for name, expected in (
                ('old-abi', 'quality_rasterizer_build_invalid'),
                ('missing-file', 'quality_rasterizer_integrity_mismatch'),
                ('source-mismatch', 'quality_rasterizer_integrity_mismatch'),
                ('acceptance-false', 'quality_rasterizer_gpu_acceptance_invalid'),
                ('acceptance-check-missing', 'quality_rasterizer_gpu_acceptance_invalid'),
                ('acceptance-missing', 'quality_rasterizer_gpu_acceptance_missing'),
                ('acceptance-json-corrupt', 'quality_kernel_preflight_failed')):
            with self.subTest(name=name):
                component = self.root / name
                shutil.copytree(ACCEPTED_ROOT, component)
                build_path = component / 'quality-rasterizer-build.json'
                acceptance_path = component / 'quality-rasterizer-acceptance.json'
                build = json.loads(build_path.read_text(encoding='utf-8'))
                acceptance = json.loads(acceptance_path.read_text(encoding='utf-8'))
                if name == 'old-abi':
                    build['quality_abi'] = 0
                    build_path.write_text(json.dumps(build), encoding='utf-8')
                elif name == 'missing-file':
                    relative = next(key for key in build['runtime_files'] if key.endswith('.pyd'))
                    (component / relative).unlink()
                elif name == 'source-mismatch':
                    build['source_files'][next(iter(build['source_files']))] = '0' * 64
                    build_path.write_text(json.dumps(build), encoding='utf-8')
                elif name == 'acceptance-false':
                    acceptance['accepted'] = False
                    acceptance_path.write_text(json.dumps(acceptance), encoding='utf-8')
                elif name == 'acceptance-check-missing':
                    del acceptance['checks']['alpha_gradients']
                    acceptance_path.write_text(json.dumps(acceptance), encoding='utf-8')
                elif name == 'acceptance-missing':
                    acceptance_path.unlink()
                else:
                    acceptance_path.write_text('{not JSON', encoding='utf-8')
                with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                                 GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                                 GSW_QUALITY_KERNEL_ROOT=str(component)):
                    with self.assertRaisesRegex(ValueError, '^' + expected):
                        preflight_quality_kernel(self.server, {'qualityPlan': self.plan}, '3dgs', REPOSITORY)

    def test_using_worker_python_instead_of_selected_training_python_is_rejected(self):
        # The actual test runner is the desktop worker's 3.12 environment; the
        # accepted component is explicitly bound to training Python 3.8.
        self.assertEqual(sys.version_info[:2], (3, 12))
        with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(Path(sys.executable).parent),
                         GS_CONDA_PREFIX=str(Path(sys.executable).parent),
                         GSW_QUALITY_KERNEL_ROOT=str(ACCEPTED_ROOT)):
            with self.assertRaisesRegex(ValueError, '^quality_rasterizer_python_mismatch'):
                preflight_quality_kernel(self.server, {'qualityPlan': self.plan}, '3dgs', REPOSITORY)

    def test_off_standard_lr_two_dgs_and_stock_mcmc_do_not_probe_missing_component(self):
        requests = [
            ('3dgs', None), ('3dgs', {'version': 1}),
            ('3dgs', {'version': 1, 'geometry_lr_multiplier': .3}),
            ('3dgs', {'version': 1, 'densification': {'strategy': 'mcmc', 'parameters': {'cap': 128}}}),
            ('2dgs', {'version': 1}),
            ('2dgs', {'version': 1, 'supervision': {'mask': {'manifest': 'unread-mask.json'}}}),
            ('2dgs', {'version': 1, 'supervision': {'depth': {'manifest': 'unread-depth.json',
                'unit_to_training_scale': 1., 'scale_evidence_identity': 'a' * 64}}}),
        ]
        with environment(GSW_QUALITY_KERNEL_ROOT=str(self.root / 'missing-component'),
                         TWO_DGS_PYTHON=str(self.root / 'nonexistent-python.exe')):
            for backend, request in requests:
                with self.subTest(backend=backend, request=request):
                    control = {} if request is None else {'qualityPlan': resolve_quality_plan(request, backend=backend)}
                    before = json.dumps(control, sort_keys=True)
                    self.assertIsNone(preflight_quality_kernel(self.server, control, backend, REPOSITORY))
                    self.assertEqual(json.dumps(control, sort_keys=True), before)

    def test_every_three_dgs_quality_kernel_consumer_requires_admission(self):
        requests = [
            {'version': 1, 'densification': {'strategy': 'abs_gs'}},
            {'version': 1, 'densification': {'strategy': 'pixel_gs'}},
            {'version': 1, 'filter': {'mode': 'full_mip'}},
            {'version': 1, 'supervision': {'mask': {'manifest': 'unread-mask.json'}}},
            {'version': 1, 'supervision': {'depth': {'manifest': 'unread-depth.json',
                'unit_to_training_scale': 1., 'scale_evidence_identity': 'a' * 64}}},
        ]
        with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GSW_QUALITY_KERNEL_ROOT=str(self.root / 'missing-component')):
            for request in requests:
                with self.subTest(request=request):
                    plan = resolve_quality_plan(request)
                    with self.assertRaisesRegex(ValueError, '^quality_rasterizer_build_missing'):
                        preflight_quality_kernel(self.server, {'qualityPlan': plan}, '3dgs', REPOSITORY)

    def test_a_real_child_exceeding_output_cap_by_one_byte_is_killed_without_waiting_for_eof(self):
        # The external-process seam supplies an adversarial real Python child,
        # not a mocked selector/module. A two-second independent cleanup guard
        # makes the red test safe if a buffered read waits for a complete block.
        real_popen, children, guards = subprocess.Popen, [], []
        script = ("import os,sys,time; assert os.environ['CUDA_VISIBLE_DEVICES']=='-1'; "
                  "sys.stdout.buffer.write(b'x'*16385); sys.stdout.buffer.flush(); time.sleep(60)")
        def launch(command, **options):
            child = real_popen([command[0], '-I', '-B', '-c', script], **options)
            children.append(child)
            guard = threading.Timer(2, child.kill)
            guard.daemon = True
            guards.append(guard)
            guard.start()
            return child
        started = time.monotonic()
        try:
            with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                             GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent)), \
                    mock.patch.object(subprocess, 'Popen', side_effect=launch):
                with self.assertRaisesRegex(ValueError, '^quality_kernel_preflight_output_invalid'):
                    preflight_quality_kernel(self.server, {'qualityPlan': self.plan}, '3dgs', REPOSITORY)
            self.assertLess(time.monotonic() - started, 1.5)
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].poll())
        finally:
            for guard in guards:
                guard.cancel()
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=3)

    def test_a_real_stalled_cpu_child_times_out_once_after_45_seconds_and_is_reaped(self):
        real_popen, children = subprocess.Popen, []
        script = "import os,time; assert os.environ['CUDA_VISIBLE_DEVICES']=='-1'; time.sleep(60)"
        def launch(command, **options):
            child = real_popen([command[0], '-I', '-B', '-c', script], **options)
            children.append(child)
            return child
        started = time.monotonic()
        try:
            with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                             GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent)), \
                    mock.patch.object(subprocess, 'Popen', side_effect=launch):
                with self.assertRaisesRegex(ValueError, '^quality_kernel_preflight_timeout'):
                    preflight_quality_kernel(self.server, {'qualityPlan': self.plan}, '3dgs', REPOSITORY)
            self.assertGreaterEqual(time.monotonic() - started, 45)
            self.assertLess(time.monotonic() - started, 52)
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].poll())
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=3)

    def test_resume_inherits_quality_plan_before_admission_without_loading_checkpoint_payload(self):
        output = self.root / 'output' / 'owned-resume'
        resume = output / '.gsw-resume'
        resume.mkdir(parents=True)
        state = resume / ('state-' + 'a' * 32 + '.pth')
        # Not a pickle or tensor payload: admission only verifies its bytes and
        # inherits the JSON plan. Attempted model/state loading would fail.
        state.write_bytes(b'owned CPU manifest-only admission fixture')
        manifest = dict(version=1, backend='3dgs', file=state.name, iteration=16, total=1000,
            sha256=hashlib.sha256(state.read_bytes()).hexdigest(), quality_plan=self.plan)
        # Use the actual runtime schema/defaults as a saved complete-state
        # document, independently of the omitted fresh request fields.
        from native.worker.training_runtime import runtime_configuration
        manifest['runtime_options'] = runtime_configuration({'backend': '3dgs'})['requested']
        (resume / 'ready.json').write_text(json.dumps(manifest), encoding='utf-8')
        before = {path.name: path.read_bytes() for path in resume.iterdir()}
        config = dict(repositoryRoot=str(REPOSITORY), outputRoot=str(self.root / 'output'),
            jobStore=str(self.root / 'jobs'), outputScene='owned-resume',
            datasetPath=str(self.root / 'absent-dataset'), backend='3dgs', quality='quick',
            nativeCheckpoint=True, runColmap=False, posePolicy='locked')
        self.assertNotIn('qualityPlan', config)
        with environment(GAUSSIAN_SPLATTING_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GS_CONDA_PREFIX=str(TRAINING_PYTHON.parent),
                         GSW_QUALITY_KERNEL_ROOT=str(self.root / 'missing-component')):
            with self.assertRaisesRegex(ValueError, '^quality_rasterizer_build_missing'):
                run_training(config, resume=True)
        self.assertEqual(list((self.root / 'jobs').glob('*.json')), [])
        self.assertEqual({path.name: path.read_bytes() for path in resume.iterdir()}, before)


if __name__ == '__main__':
    unittest.main()
