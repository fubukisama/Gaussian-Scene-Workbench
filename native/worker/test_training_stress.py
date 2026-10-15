"""Stress harness process-boundary checks, without CUDA or training."""
import os
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys


class StressObserverTests(unittest.TestCase):
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
