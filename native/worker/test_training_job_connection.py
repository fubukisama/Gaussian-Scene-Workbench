"""Existing-job observation uses files, never the training-start/recovery path."""
import json
import contextlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from native.worker.training_job_connection import inspect_job


class TrainingJobConnectionTests(unittest.TestCase):
    def test_preflight_refuses_parent_traversal_and_preserves_long_root_records(self):
        from native.worker.training_job_connection import ensure_output_not_active, JobConnectionError, process_identity
        parent = '\\\\?\\' + str(Path(tempfile.gettempdir()).resolve()) if os.name == 'nt' else None
        with tempfile.TemporaryDirectory(dir=parent) as temporary:
            root = Path(temporary)
            for store, output in ((root / 'absent' / '..', root / 'result'),
                                  (root, root / 'absent' / '..' / 'result')):
                with self.subTest(store=store, output=output):
                    with self.assertRaisesRegex(JobConnectionError, 'path_invalid'):
                        ensure_output_not_active(store, output)
                    self.assertEqual(list(root.iterdir()), [])
            store = root
            while len(str(store)) < 340:
                store /= 'long-native-job-root'
            store.mkdir(parents=True)
            output, job_id, session = store / 'output', '1' * 32, '2' * 32
            input_store = Path(str(store)[4:]) if os.name == 'nt' else store
            input_output = Path(str(output)[4:]) if os.name == 'nt' else output
            job_path, lease_path = store / (job_id + '.json'), store / '.c' / (session + '.json')
            lease_path.parent.mkdir()
            job_path.write_text(json.dumps({'id': job_id, 'backend': '2dgs', 'status': 'running',
                'native_control': {'session': session, 'backend': '2dgs', 'output': str(input_output)}}), encoding='utf-8')
            lease = {'version': 1, 'jobId': job_id, 'session': session, 'backend': '2dgs',
                     'output': str(input_output), 'closed': False, 'heartbeat': 1,
                     'workerIdentity': process_identity(os.getpid())}
            lease_path.write_text(json.dumps(lease), encoding='utf-8')
            before_job, before_lease = job_path.read_bytes(), lease_path.read_bytes()
            refusal = 'path_too_long' if os.name == 'nt' else 'output_session_existing_job_active'
            with self.assertRaisesRegex(JobConnectionError, refusal):
                ensure_output_not_active(input_store, input_output)
            self.assertEqual(job_path.read_bytes(), before_job)
            self.assertEqual(lease_path.read_bytes(), before_lease)

    def test_start_preflight_refuses_legacy_active_output_without_mutating_history(self):
        from native.worker.training_job_connection import ensure_output_not_active, JobConnectionError, process_identity
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output, job_id, session = root / 'result', 'a' * 32, 'b' * 32
            control = {'session': session, 'backend': '3dgs', 'output': str(output)}
            path = root / (job_id + '.json')
            path.write_text(json.dumps({'id': job_id, 'backend': '3dgs', 'status': 'running',
                'output_scene': output.name, 'native_control': control}), encoding='utf-8')
            original = path.read_bytes()
            with self.assertRaisesRegex(JobConnectionError, 'output_session_existing_job_unverified'):
                ensure_output_not_active(root, output)
            lease_path = root / '.c' / (session + '.json')
            lease_path.parent.mkdir()
            lease = {'version': 1, 'jobId': job_id, 'session': session, 'backend': '3dgs',
                     'output': str(output), 'closed': False, 'workerIdentity': process_identity(os.getpid())}
            lease_path.write_text(json.dumps(lease), encoding='utf-8')
            with self.assertRaisesRegex(JobConnectionError, 'output_session_existing_job_active'):
                ensure_output_not_active(root, output)
            lease['closed'] = True
            lease_path.write_text(json.dumps(lease), encoding='utf-8')
            lease_before = lease_path.read_bytes()
            ensure_output_not_active(root, output)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(lease_path.read_bytes(), lease_before)
            ensure_output_not_active(root, root / 'different-output')

    def test_worker_connections_in_different_gui_stores_share_one_output_gate(self):
        from native.worker.training_job_connection import OutputSessionGate, WorkerConnection, JobConnectionError
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / 'arbitrary-output'
            output.mkdir()
            original = output / 'existing-result.ply'
            original.write_bytes(b'original result must be retained')
            controls, stores, identifiers = [], [], []
            for index in range(2):
                store = root / ('gui-' + str(index))
                store.mkdir()
                job_id, session = str(index + 1) * 32, str(index + 3) * 32
                control = {'session': session, 'backend': '3dgs', 'output': str(output)}
                (store / (job_id + '.json')).write_text(json.dumps({
                    'id': job_id, 'backend': '3dgs', 'status': 'running', 'native_control': control}),
                    encoding='utf-8')
                stores.append(store)
                identifiers.append(job_id)
                controls.append(control)
            with OutputSessionGate(output, controls[0]['session']) as gate:
                worker = WorkerConnection(stores[0], identifiers[0], controls[0], output_gate=gate)
                worker.close('done')
                self.assertTrue(gate.active, 'borrowed gate must cover final metadata writes')
                with self.assertRaisesRegex(JobConnectionError, 'output_session_busy'):
                    WorkerConnection(stores[1], identifiers[1], controls[1])
            retry = WorkerConnection(stores[1], identifiers[1], controls[1])
            retry.close('done')
            self.assertEqual(original.read_bytes(), b'original result must be retained')

    def test_output_gate_does_not_displace_an_unverified_or_still_live_old_owner(self):
        from native.worker.training_job_connection import OutputSessionGate, JobConnectionError, process_identity
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'result'
            with OutputSessionGate(output, 'a' * 32) as previous:
                path = previous.path
            original = path.read_bytes()
            record = json.loads(original[1:].decode('utf-8'))
            for invalid in ('malformed', 'unknown', 'extra-identity-key', 'impossible-time', 'live'):
                with self.subTest(record=invalid):
                    if invalid == 'malformed':
                        data = b'not-json'
                    else:
                        identity = process_identity(os.getpid())
                        if invalid == 'extra-identity-key':
                            identity['unrecognized'] = 'must-not-prove-process-exit'
                        elif invalid == 'unknown':
                            identity = {'pid': os.getpid(), 'startToken': 'unknown'}
                        elif invalid == 'impossible-time':
                            identity = {'pid':os.getpid(),'startToken':'win-filetime:'+str(2**64)}
                        record.update(closed=False, workerIdentity=identity)
                        data = b'\n' + json.dumps(record).encode('utf-8')
                    path.write_bytes(data)
                    with self.assertRaisesRegex(JobConnectionError, 'output_session_owner_unverified|output_session_busy'):
                        OutputSessionGate(output, 'b' * 32).acquire()
                    self.assertEqual(path.read_bytes(), data)
            path.write_bytes(original)
            with OutputSessionGate(output, 'c' * 32):
                pass

    def test_output_gate_rejects_second_live_owner_and_allows_retry_after_exit(self):
        from native.worker.training_job_connection import OutputSessionGate, JobConnectionError, ensure_output_not_active
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output, ready = root / 'not-created-output', root / 'ready.json'
            script = '''import os, sys
from pathlib import Path
from native.worker.training_job_connection import OutputSessionGate
gate = OutputSessionGate(sys.argv[1], 'd' * 32).acquire()
Path(sys.argv[2]).write_text('ready', encoding='utf-8')
command = sys.stdin.readline().strip()
if command == 'crash':
    os._exit(23)
gate.release()
'''
            child = subprocess.Popen([sys.executable, '-c', script, str(output), str(ready)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            try:
                deadline = time.monotonic() + 10
                while not ready.exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists(), 'owned CPU gate fixture did not acquire')
                self.assertFalse(output.exists(), 'gate must not create the training output')
                with self.assertRaisesRegex(JobConnectionError, 'output_session_busy'):
                    OutputSessionGate(output, 'e' * 32).acquire()
                _, errors = child.communicate(b'crash\n', timeout=10)
                self.assertEqual(child.returncode, 23, errors.decode('utf-8', errors='replace'))
                abandoned = json.loads(OutputSessionGate(output, 'f' * 32).path.read_bytes()[1:])
                legacy = root / 'legacy-store'
                (legacy / '.c').mkdir(parents=True)
                job_id, session = '1' * 32, 'd' * 32
                (legacy / (job_id + '.json')).write_text(json.dumps({
                    'id': job_id, 'backend': '3dgs', 'status': 'running', 'native_control': {
                        'session': session, 'backend': '3dgs', 'output': str(output)}}), encoding='utf-8')
                (legacy / '.c' / (session + '.json')).write_text(json.dumps({
                    'version': 1, 'jobId': job_id, 'session': session, 'backend': '3dgs',
                    'output': str(output), 'closed': False, 'workerIdentity': abandoned['workerIdentity']}),
                    encoding='utf-8')
                self.assertEqual(ensure_output_not_active(legacy, output)['endedJobs'], 1)
                with OutputSessionGate(output, 'f' * 32) as next_owner:
                    self.assertTrue(next_owner.active)
                self.assertFalse(next_owner.active)
                with OutputSessionGate(output, 'a' * 32):
                    pass
            finally:
                if child.poll() is None:
                    child.kill()  # Only the subprocess created by this fixture.
                child.communicate(timeout=10)

    @unittest.skipUnless(os.name == 'nt', 'Windows DWORD process identity boundary')
    def test_process_identity_does_not_wrap_an_oversized_pid_to_an_existing_process(self):
        from native.worker.training_job_connection import process_identity
        identity = process_identity(os.getpid())
        self.assertIsNotNone(identity)
        self.assertEqual(identity['pid'], os.getpid())
        self.assertIsNone(process_identity(os.getpid() + 2 ** 32))

    def test_live_owned_session_accepts_only_its_requested_command(self):
        from native.worker.training_job_connection import WorkerConnection, request_command, JobConnectionError
        with tempfile.TemporaryDirectory() as temporary, contextlib.ExitStack() as cleanup:
            store = Path(temporary)
            job_id, session = 'b' * 32, 'c' * 32
            output = store / 'output'
            output.mkdir()
            control = {'session': session, 'backend': '2dgs', 'output': str(output),
                       'request': str(store / ('pause-' + session + '.json'))}
            job = {'id': job_id, 'backend': '2dgs', 'status': 'running', 'stage': 'train',
                   'iteration': 8, 'total_iterations': 31, 'output_scene': 'output',
                   'native_control': control}
            path = store / (job_id + '.json')
            path.write_text(json.dumps(job), encoding='utf-8')
            before = path.read_bytes()
            worker = WorkerConnection(store, job_id, control)
            cleanup.callback(worker.close,'test_cleanup')
            worker.heartbeat()
            report = inspect_job(store, job_id)
            self.assertEqual(report['liveness'], 'live')
            self.assertTrue(report['controlAvailable'])
            receipt = request_command(store, job_id, 'pause')
            self.assertEqual(receipt['command'], 'pause')
            self.assertEqual(worker.take_command(), 'pause')
            self.assertIsNone(worker.take_command())
            exporting=dict(job,stage='coordinate_export')
            path.write_text(json.dumps(exporting),encoding='utf-8')
            with self.assertRaisesRegex(JobConnectionError,'training_already_completed'):
                request_command(store,job_id,'pause')
            self.assertEqual(request_command(store,job_id,'stop')['command'],'stop')
            self.assertEqual(worker.take_command(),'stop')
            path.write_bytes(before)
            worker.close('paused')
            self.assertFalse(inspect_job(store, job_id)['controlAvailable'])
            self.assertEqual(path.read_bytes(), before)

    def test_legacy_running_record_is_read_only_and_not_claimed_live(self):
        with tempfile.TemporaryDirectory() as temporary:
            store = Path(temporary)
            job_id = 'a' * 32
            job = {'id': job_id, 'backend': '3dgs', 'status': 'running',
                   'stage': 'train', 'iteration': 37, 'total_iterations': 93,
                   'output_scene': 'arbitrary-model', 'process': None}
            path = store / (job_id + '.json')
            path.write_text(json.dumps(job), encoding='utf-8')
            before = path.read_bytes()
            report = inspect_job(store, job_id)
            self.assertEqual(report['recordedState'], 'running')
            self.assertEqual(report['iteration'], 37)
            self.assertEqual(report['liveness'], 'unverified')
            self.assertFalse(report['controlAvailable'])
            self.assertEqual(report['reason'], 'legacy_connection_metadata_missing')
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(list(store.iterdir()), [path])


if __name__ == '__main__':
    unittest.main()
