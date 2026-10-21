"""Stress harness process-boundary checks, without CUDA or training."""
import os
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys


class StressObserverTests(unittest.TestCase):
    def _runtime_plan(self, *arguments):
        repository = Path(__file__).resolve().parents[2]
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        result = subprocess.run([sys.executable, '-B', str(repository / 'scripts/training_stress.py'),
            '--runtime-plan', *arguments], cwd=str(repository), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding='utf-8', creationflags=flags, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_runtime_plan_can_disable_only_guard_without_loading_inputs_or_starting_training(self):
        report = self._runtime_plan('--guard', 'off', '--optimizations', 'on')
        policy = report['runtime']['requested']
        self.assertFalse(policy['resourceGuard']['enabled'])
        self.assertTrue(policy['execution']['image_staging'])
        self.assertTrue(policy['execution']['camera_prefetch'])
        self.assertTrue(policy['execution']['capacity_storage'])
        self.assertEqual(policy['execution']['loss_mode'], 'auto')
        self.assertFalse(report['training_started'])

    def test_four_guard_optimization_combinations_and_legacy_case_are_explicit(self):
        for guard, optimized in (('on', 'on'), ('on', 'off'), ('off', 'on'), ('off', 'off')):
            with self.subTest(guard=guard, optimized=optimized):
                policy = self._runtime_plan('--guard', guard, '--optimizations', optimized)['runtime']['requested']
                self.assertEqual(policy['resourceGuard']['enabled'], guard == 'on')
                self.assertEqual(policy['execution']['image_staging'], optimized == 'on')
                self.assertEqual(policy['execution']['capacity_storage'], optimized == 'on')
                self.assertEqual(policy['execution']['loss_mode'], 'auto' if optimized == 'on' else 'stock')
        legacy = self._runtime_plan('--case', 'legacy')
        self.assertEqual(legacy['legacy_case_mapping'], 'legacy_effective_guard_and_execution_off')
        self.assertFalse(legacy['runtime']['requested']['resourceGuard']['enabled'])
        self.assertFalse(legacy['runtime']['requested']['execution']['camera_prefetch'])

    def test_individual_prefetch_dependency_and_legacy_baseline_flag_do_not_change_guard_or_loss(self):
        report = self._runtime_plan('--guard', 'off', '--optimizations', 'on', '--image-staging', 'off')
        self.assertTrue(report['runtime']['requested']['execution']['camera_prefetch'])
        self.assertFalse(report['runtime']['effective']['execution']['camera_prefetch'])
        self.assertEqual(report['runtime']['reasons'], [
            {'feature': 'execution.camera_prefetch', 'code': 'requires_image_staging'}])
        baseline = self._runtime_plan('--guard', 'off', '--optimizations', 'on', '--baseline-runtime')
        self.assertFalse(baseline['runtime']['requested']['resourceGuard']['enabled'])
        self.assertTrue(baseline['runtime']['requested']['execution']['image_staging'])
        self.assertEqual(baseline['runtime']['requested']['execution']['loss_mode'], 'auto')
        self.assertFalse(baseline['runtime']['requested']['execution']['surfel_auxiliary'])

    def test_windows_monitor_does_not_depend_on_inherited_path(self):
        from scripts.training_stress import memory_observer_command
        with tempfile.TemporaryDirectory() as temporary:
            system = Path(temporary)
            executable = system / 'System32/WindowsPowerShell/v1.0/powershell.exe'
            executable.parent.mkdir(parents=True)
            executable.touch()
            command = memory_observer_command(Path('D:/owned-repository'), 123, system)
            self.assertEqual(Path(command[0]), executable)
            self.assertEqual(command[-2:], ['-TrainerPid', '123'])
            executable.unlink()
            with self.assertRaises(FileNotFoundError):
                memory_observer_command(Path('D:/owned-repository'), 123, system)

    def test_failed_observer_cleanup_only_stops_the_owned_child(self):
        from scripts.training_stress import pause_owned_process
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        with tempfile.TemporaryDirectory() as temporary:
            request = Path(temporary) / 'pause.json'
            process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                                       creationflags=flags)
            try:
                pause_owned_process(process, request, 'owned-fixture', grace=.05)
                self.assertIsNotNone(process.poll())
                self.assertIn('owned-fixture', request.read_text())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
