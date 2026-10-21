"""Persistent batch observation/control at public file and owned-process seams."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import stat
import socket
import unittest
from unittest.mock import patch


def refuse_native_process(*args, **kwargs):
    raise AssertionError('CPU rejection fixture must never fall through into the production process adapter')


class TrainingBatchConnectionTests(unittest.TestCase):
    def wait_for(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.03)
        self.fail('owned batch did not reach the requested public state')

    def test_opening_legacy_batch_is_read_only_and_never_launches_a_coordinator(self):
        from native.worker.training_batch_connection import inspect_batch, list_batches
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            root = workspace / '.gsw' / 'training-batches' / ('a' * 32)
            root.mkdir(parents=True)
            plan = {'schema': 'gsw.training_batch/1', 'plan_sha256': 'b' * 64,
                    'runs': [{'id': 'run-' + 'c' * 24, 'configuration': {'backend': '3dgs'}}]}
            plan_path = root / 'plan.json'
            plan_path.write_text(json.dumps(plan), encoding='utf-8')
            state = {'schema': 'gsw.training_batch_state/1', 'plan_sha256': 'b' * 64,
                     'plan_file_sha256': hashlib.sha256(plan_path.read_bytes()).hexdigest(),
                     'status': 'ready', 'active_run_id': None,
                     'runs': {'run-' + 'c' * 24: {'status': 'pending', 'attempts': 0, 'artifacts': []}}}
            (root / 'state.json').write_text(json.dumps(state), encoding='utf-8')
            original = {str(p.relative_to(workspace)): p.read_bytes()
                        for p in workspace.rglob('*') if p.is_file()}
            report = inspect_batch(plan_path)
            self.assertEqual(report['schema'], 'gsw.training_batch_connection/1')
            self.assertEqual(report['batchId'], 'a' * 32)
            self.assertEqual(report['queueState'], 'ready')
            self.assertFalse(report['controlAvailable'])
            self.assertEqual(report['liveness'], 'unknown')
            self.assertEqual(report['reason'], 'batch_connection_legacy_metadata_missing')
            self.assertEqual(report['state'], state)
            inventory = list_batches(workspace)
            self.assertEqual(len(inventory['batches']), 1)
            self.assertEqual(inventory['batches'][0]['planPath'], str(plan_path))
            self.assertEqual({str(p.relative_to(workspace)): p.read_bytes()
                              for p in workspace.rglob('*') if p.is_file()}, original)

    def test_cpu_coordinator_is_exclusive_and_safe_pause_survives_observer_close(self):
        from native.worker.training_batch_connection import inspect_batch, request_batch_command
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        _, _, marker = fixture.fake_process_factory('pause-first')
        script = fixture.root / 'owned-coordinator.py'
        script.write_text('''import os,subprocess,sys
from native.worker.training_batch_connection import run_persistent_batch
def factory(command,**options):
 command=list(command);command[2]=sys.argv[2]
 options['env']=dict(os.environ,GSW_BATCH_FIXTURE_MODE='pause-first',GSW_BATCH_FIXTURE_MARKER=sys.argv[3])
 return subprocess.Popen(command,**options)
sys.exit(run_persistent_batch(sys.argv[1],process_factory=factory))
''', encoding='utf-8')
        log = (fixture.root / 'coordinator.log').open('w', encoding='utf-8')
        self.addCleanup(log.close)
        environment = dict(os.environ, PYTHONPATH=str(fixture.repository), PYTHONUTF8='1')
        process = subprocess.Popen([sys.executable, '-B', str(script), str(plan),
            str(fixture.root / 'owned-child.py'), str(marker)], cwd=fixture.root, env=environment,
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        def cleanup():
            if process.poll() is None:
                try:
                    current = inspect_batch(plan)
                    request_batch_command(plan, 'pause', current['session'])
                    process.wait(timeout=10)
                except Exception:
                    process.terminate()
                    process.wait(timeout=5)  # This test owns only this CPU fixture.
        self.addCleanup(cleanup)
        report = self.wait_for(lambda: inspect_batch(plan) if marker.exists() and
            inspect_batch(plan).get('controlAvailable') else None)
        # An invalid read-only inbox file can be renamed but not deleted on
        # Windows. A rejected command must not escape the reader into the old
        # executor's owned-child cleanup, even when quarantine cleanup fails.
        bad_command = Path(report['commandPath'])
        bad_command.write_text('{damaged', encoding='utf-8')
        bad_command.chmod(stat.S_IREAD)
        self.wait_for(lambda: inspect_batch(plan).get('coordinator', {}).get('rejectedCommands'))
        time.sleep(.2)
        self.assertIsNone(process.poll(), 'a damaged observer command must not kill the active child/coordinator')
        # An inspection helper is disposable; observing and exiting must not
        # own, relaunch or stop the independent coordinator/child.
        observed = subprocess.run([sys.executable, '-B', '-m', 'native.worker.training_batch_connection',
            '--operation', 'inspect', '--plan', str(plan)], cwd=fixture.repository, env=environment,
            capture_output=True, text=True, encoding='utf-8', timeout=10,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertEqual(observed.returncode, 0, observed.stdout + observed.stderr)
        self.assertIsNone(process.poll())
        duplicate = subprocess.run([sys.executable, '-B', str(script), str(plan),
            str(fixture.root / 'owned-child.py'), str(marker)], cwd=fixture.root, env=environment,
            stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding='utf-8', timeout=10,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertIsNone(process.poll())
        receipt = request_batch_command(plan, 'pause', report['session'])
        self.assertEqual(receipt['command'], 'pause')
        self.assertEqual(process.wait(timeout=15), 75)
        paused = inspect_batch(plan)
        self.assertEqual(paused['coordinatorOutcome'], 'paused')
        self.assertEqual(paused['queueState'], 'paused')
        self.assertEqual(paused['liveness'], 'ended')
        self.assertEqual(len(paused['pendingRunIds']), 1)
        self.assertTrue(paused['checkpointPreserved'])

    def test_cancel_keeps_checkpoint_and_pending_queue_for_explicit_new_session_resume(self):
        from native.worker.training_batch_connection import inspect_batch, request_batch_command, run_persistent_batch
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        factory, calls, marker = fixture.fake_process_factory('pause-first')
        script = fixture.root / 'cancel-coordinator.py'
        script.write_text('''import os,subprocess,sys
from native.worker.training_batch_connection import run_persistent_batch
def factory(command,**options):
 command=list(command);command[2]=sys.argv[2]
 options['env']=dict(os.environ,GSW_BATCH_FIXTURE_MODE='pause-first',GSW_BATCH_FIXTURE_MARKER=sys.argv[3])
 return subprocess.Popen(command,**options)
sys.exit(run_persistent_batch(sys.argv[1],process_factory=factory))
''', encoding='utf-8')
        log = (fixture.root / 'cancel-coordinator.log').open('w', encoding='utf-8')
        self.addCleanup(log.close)
        environment = dict(os.environ, PYTHONPATH=str(fixture.repository), PYTHONUTF8='1')
        process = subprocess.Popen([sys.executable, '-B', str(script), str(plan),
            str(fixture.root / 'owned-child.py'), str(marker)], cwd=fixture.root, env=environment,
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        def cleanup():
            if process.poll() is None:
                current = inspect_batch(plan)
                request_batch_command(plan, 'pause', current['session'])
                process.wait(timeout=10)
        self.addCleanup(cleanup)
        running = self.wait_for(lambda: inspect_batch(plan) if marker.exists() and
            inspect_batch(plan).get('controlAvailable') else None)
        with self.assertRaisesRegex(ValueError, 'session_changed'):
            request_batch_command(plan, 'cancel', '0' * 32)
        self.assertIsNone(process.poll())
        request_batch_command(plan, 'cancel', running['session'])
        self.assertEqual(process.wait(timeout=15), 130)
        cancelled = inspect_batch(plan)
        self.assertEqual(cancelled['coordinatorOutcome'], 'cancelled')
        self.assertEqual(cancelled['queueState'], 'paused')
        self.assertEqual(len(cancelled['pendingRunIds']), 1)
        self.assertTrue(cancelled['checkpointPreserved'])
        self.assertTrue(cancelled['resumeAvailable'])
        # Merely reading/reopening does not advance the retained queue.
        self.assertEqual(inspect_batch(plan)['state'], cancelled['state'])
        self.assertEqual(calls, [])
        self.assertEqual(run_persistent_batch(plan, resume=True, process_factory=factory), 0)
        completed = inspect_batch(plan)
        self.assertEqual(completed['queueState'], 'completed')
        self.assertEqual(completed['coordinatorOutcome'], 'completed')
        self.assertNotEqual(completed['session'], cancelled['session'])
        self.assertEqual(len(calls), 2)
        self.assertIn('--resume-training', calls[0])
        self.assertNotIn('--resume-training', calls[1])
        self.assertEqual(run_persistent_batch(plan, resume=True, process_factory=factory), 0)
        self.assertEqual(len(calls), 2, 'completed verified results must not run again')

    def test_foreign_closed_owner_is_not_stolen_by_explicit_resume(self):
        from native.worker.training_batch_connection import run_persistent_batch
        from native.worker.training_job_connection import process_identity
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        value = json.loads(plan.read_text(encoding='utf-8'))
        metadata = {'schema': 'gsw.training_batch_connection/1', 'version': 1,
            'batchId': plan.parent.name, 'planPath': str(plan), 'planSha256': value['plan_sha256'],
            'planFileSha256': hashlib.sha256(plan.read_bytes()).hexdigest(),
            'host': socket.gethostname() + '-foreign', 'session': 'd' * 32,
            'coordinatorIdentity': process_identity(os.getpid()), 'closed': True,
            'heartbeat': time.time(), 'outcome': 'paused'}
        (plan.parent / '.coordinator.json').write_text(json.dumps(metadata), encoding='utf-8')
        before = {str(p.relative_to(plan.parent)): p.read_bytes()
                  for p in plan.parent.rglob('*') if p.is_file()}
        with self.assertRaisesRegex(ValueError, 'owner_unverified_or_live'):
            run_persistent_batch(plan, resume=True, process_factory=refuse_native_process)
        self.assertEqual({str(p.relative_to(plan.parent)): p.read_bytes()
                          for p in plan.parent.rglob('*') if p.is_file()}, before)

    @unittest.skipUnless(os.name == 'nt', 'Windows process-query failure boundary')
    def test_legacy_access_denied_pid_is_not_treated_as_dead(self):
        import ctypes
        from native.worker.training_batch_connection import run_persistent_batch
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        target_pid = 0x7ffffff0
        (plan.parent / '.run-lock.json').write_text(json.dumps(
            {'host': socket.gethostname(), 'pid': target_pid}), encoding='utf-8')
        real_loader = ctypes.WinDLL
        class DeniedQuery:
            def __init__(self, real): self.real = real
            def __call__(self, rights, inherit, pid):
                if pid == target_pid:
                    ctypes.set_last_error(5)
                    return 0
                return self.real(rights, inherit, pid)
        def kernel_loader(*args, **kwargs):
            kernel = real_loader(*args, **kwargs)
            kernel.OpenProcess = DeniedQuery(kernel.OpenProcess)
            return kernel
        with patch('ctypes.WinDLL', side_effect=kernel_loader):
            with self.assertRaisesRegex(ValueError, 'legacy_owner_unverified_or_live'):
                run_persistent_batch(plan, process_factory=refuse_native_process)

    def test_post_launch_observation_failure_does_not_lose_owned_child(self):
        from native.worker.training_batch_connection import run_persistent_batch
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        children = []
        def process_adapter(command, **options):
            child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(.2)'], **options)
            children.append(child)
            self.addCleanup(lambda: child.wait(timeout=5))
            # External filesystem race after the process-launch seam. This
            # fixture emits no optimizer/checkpoint/output success evidence.
            Path(command[command.index('--config') + 1]).unlink()
            return child
        with self.assertRaisesRegex(ValueError, 'batch_result_missing'):
            run_persistent_batch(plan, process_factory=process_adapter)
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll(), 'the original executor must retain and reap its owned child')

    def test_between_run_pause_retains_pending_queue_without_claiming_a_checkpoint(self):
        from native.worker.training_batch_connection import inspect_batch, run_persistent_batch
        from native.worker.training_job_connection import process_identity
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp();self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        state_path = plan.parent / 'state.json'
        state = json.loads(state_path.read_text(encoding='utf-8'))
        state.update(status='interrupted', active_run_id=None, reason='pause_between_runs_no_optimizer_checkpoint')
        state_path.write_text(json.dumps(state), encoding='utf-8')
        owner = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(.15)'],
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        identity = self.wait_for(lambda: process_identity(owner.pid))
        owner.wait(timeout=5)
        metadata = {'schema': 'gsw.training_batch_connection/1', 'version': 1,
            'batchId': plan.parent.name, 'planPath': str(plan), 'planSha256': state['plan_sha256'],
            'planFileSha256': hashlib.sha256(plan.read_bytes()).hexdigest(),
            'host': socket.gethostname(), 'session': 'f' * 32, 'coordinatorIdentity': identity,
            'closed': True, 'heartbeat': time.time(), 'outcome': 'paused', 'checkpointPreserved': False}
        (plan.parent / '.coordinator.json').write_text(json.dumps(metadata), encoding='utf-8')
        report = inspect_batch(plan)
        self.assertFalse(report['checkpointPreserved'])
        self.assertEqual(len(report['pendingRunIds']), 2)
        self.assertTrue(report['resumeAvailable'])
        factory, calls, _ = fixture.fake_process_factory('complete')
        self.assertEqual(run_persistent_batch(plan, resume=True, process_factory=factory), 0)
        self.assertEqual(len(calls), 2)
        self.assertTrue(all('--resume-training' not in command for command in calls))

    def test_dead_coordinator_does_not_steal_live_or_unknown_child_or_rewrite_queue(self):
        from native.worker.training_batch_connection import run_persistent_batch
        from native.worker.training_job_connection import process_identity
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        fixture = TrainingBatchWorkerTests(methodName='runTest')
        fixture.setUp();self.addCleanup(fixture.doCleanups)
        plan = fixture.create_public_plan()
        state_path = plan.parent / 'state.json'
        state = json.loads(state_path.read_text(encoding='utf-8'))
        state.update(status='interrupted', active_run_id=None)
        state_path.write_text(json.dumps(state), encoding='utf-8')
        owner = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(.2)'],
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        owner_identity = self.wait_for(lambda: process_identity(owner.pid));owner.wait(timeout=5)
        child = subprocess.Popen([sys.executable, '-c', 'import sys;sys.stdin.readline()'],stdin=subprocess.PIPE,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        child_identity = self.wait_for(lambda: process_identity(child.pid))
        def release():
            if child.poll() is None:
                child.stdin.write(b'release\n');child.stdin.flush();child.wait(timeout=5)
            child.stdin.close()
        self.addCleanup(release)
        metadata = {'schema': 'gsw.training_batch_connection/1', 'version': 1,
            'batchId': plan.parent.name, 'planPath': str(plan), 'planSha256': state['plan_sha256'],
            'planFileSha256': hashlib.sha256(plan.read_bytes()).hexdigest(),
            'host': socket.gethostname(), 'session': '9' * 32, 'coordinatorIdentity': owner_identity,
            'closed': True, 'heartbeat': time.time(), 'outcome': 'interrupted',
            'activeChild': {'identity': child_identity}}
        lease = plan.parent / '.coordinator.json'
        cpu_factory, _, _ = fixture.fake_process_factory('complete')
        for identity in (child_identity, None):
            metadata['activeChild']['identity'] = identity
            lease.write_text(json.dumps(metadata), encoding='utf-8')
            before = {str(p.relative_to(plan.parent)): p.read_bytes() for p in plan.parent.rglob('*') if p.is_file()}
            with self.assertRaisesRegex(ValueError, 'active_child_unverified_or_live'):
                run_persistent_batch(plan, resume=True, process_factory=cpu_factory)
            self.assertIsNone(child.poll())
            self.assertEqual({str(p.relative_to(plan.parent)): p.read_bytes() for p in plan.parent.rglob('*') if p.is_file()}, before)
        release();metadata['activeChild']['identity'] = child_identity
        lease.write_text(json.dumps(metadata), encoding='utf-8')
        factory,calls,_=fixture.fake_process_factory('complete')
        self.assertEqual(run_persistent_batch(plan,resume=True,process_factory=factory),0)
        self.assertEqual(len(calls),2)

    def test_between_run_control_acknowledges_queue_not_optimizer_checkpoint(self):
        from native.worker.training_batch_connection import inspect_batch, request_batch_command, run_persistent_batch
        from native.worker.test_training_batch import TrainingBatchWorkerTests
        for operation, expected_code in (('pause', 76), ('cancel', 130)):
            with self.subTest(operation=operation):
                fixture = TrainingBatchWorkerTests(methodName='runTest')
                fixture.setUp();self.addCleanup(fixture.doCleanups)
                plan = fixture.create_public_plan()
                cpu_factory, calls, _ = fixture.fake_process_factory('complete')
                def process_adapter(command, **options):
                    child = cpu_factory(command, **options)  # Explicit owned CPU entry replacement.
                    report = inspect_batch(plan)
                    request_batch_command(plan, operation, report['session'])
                    return child
                self.assertEqual(run_persistent_batch(plan, process_factory=process_adapter), expected_code)
                report = inspect_batch(plan)
                self.assertEqual(len(calls), 1)
                self.assertEqual(report['queueState'], 'interrupted')
                self.assertEqual(len(report['pendingRunIds']), 1)
                self.assertFalse(report['checkpointPreserved'])
                self.assertEqual(report['coordinator']['lastCommand']['acknowledgement'], 'queue_stopped_between_runs')


if __name__ == '__main__':
    unittest.main()
