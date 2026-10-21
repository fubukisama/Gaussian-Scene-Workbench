# SPDX-License-Identifier: GPL-3.0-or-later
"""Batch-specific durable coordination, separate from trainer ownership.

Observation never imports compute/the batch executor, repairs queue state,
loads a checkpoint, creates a job or starts a coordinator. Only explicit
run/resume cross the execution seam.
"""
import hashlib
import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import uuid

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.worker.training_job_connection import (OutputSessionGate, process_identity,
    _identity_liveness, _valid_process_identity, _read_object, _safe_path)
from native.worker.training_checkpoint import atomic_json
from native.backend.atomic_file import atomic_sibling


SCHEMA = 'gsw.training_batch_connection/1'
_ID = re.compile(r'^[0-9a-f]{32}$')
_SHA = re.compile(r'^[0-9a-f]{64}$')
_RUN = re.compile(r'^run-[0-9a-f]{24}$')


class BatchConnectionError(ValueError):
    pass


def _legacy_pid_ended(pid):
    """A PID-only legacy owner is dead only after an explicit OS exit result."""
    if type(pid) is not int or not 0 < pid <= 0xffffffff:
        return False
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return ctypes.get_last_error() == 87
        try:
            code = wintypes.DWORD()
            return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value != 259)
        finally:
            kernel.CloseHandle(handle)
    try:
        value = Path('/proc/{}/stat'.format(pid)).read_text()
        return value[value.rfind(')') + 2:].split()[0] == 'Z'
    except FileNotFoundError:
        return True
    except (OSError, IndexError):
        return False


def _plan_binding(plan_path):
    value = Path(plan_path)
    if not value.is_absolute():
        raise BatchConnectionError('batch_connection_absolute_plan_required')
    plan_path = _safe_path(value)
    root = plan_path.parent
    if (plan_path.name != 'plan.json' or not _ID.fullmatch(root.name) or
            root.parent.name != 'training-batches' or root.parent.parent.name != '.gsw'):
        raise BatchConnectionError('batch_connection_plan_path_invalid')
    plan = _read_object(plan_path)
    state = _read_object(root / 'state.json')
    digest = hashlib.sha256(plan_path.read_bytes()).hexdigest()
    identity = plan.get('plan_sha256')
    if (plan.get('schema') != 'gsw.training_batch/1' or not isinstance(identity, str) or
            not _SHA.fullmatch(identity) or state.get('schema') != 'gsw.training_batch_state/1' or
            state.get('plan_sha256') != identity or state.get('plan_file_sha256') != digest):
        raise BatchConnectionError('batch_connection_plan_state_mismatch')
    runs, records = plan.get('runs'), state.get('runs')
    if type(runs) is not list or type(records) is not dict or not runs:
        raise BatchConnectionError('batch_connection_queue_invalid')
    identifiers = [run.get('id') if type(run) is dict else None for run in runs]
    if (any(not isinstance(item, str) or not _RUN.fullmatch(item) for item in identifiers) or
            len(set(identifiers)) != len(identifiers) or set(identifiers) != set(records) or
            any(type(record) is not dict for record in records.values())):
        raise BatchConnectionError('batch_connection_queue_invalid')
    return root, plan, state, digest, identifiers


def inspect_batch(plan_path):
    """Read bound queue metadata only; legacy/unknown sessions stay read-only."""
    root, plan, state, digest, identifiers = _plan_binding(plan_path)
    report = {
        'schema': SCHEMA, 'version': 1, 'kind': 'training_batch',
        'batchId': root.name, 'planPath': str(root / 'plan.json'),
        'planSha256': plan['plan_sha256'], 'planFileSha256': digest,
        'queueState': state.get('status', 'unknown'), 'state': state,
        'activeRunId': state.get('active_run_id'),
        'pendingRunIds': [key for key in identifiers if state['runs'][key].get('status') == 'pending'],
        'coordinatorOutcome': None, 'liveness': 'unknown', 'controlAvailable': False,
        'resumeAvailable': False,
        'reason': 'batch_connection_legacy_metadata_missing',
    }
    path = root / '.coordinator.json'
    if not path.exists():
        return report
    try:
        connection = _read_object(path, 256 * 1024)
        valid = (connection.get('schema') == SCHEMA and connection.get('version') == 1 and
                 connection.get('batchId') == root.name and connection.get('planSha256') == plan['plan_sha256'] and
                 connection.get('planFileSha256') == digest and connection.get('planPath') == str(root / 'plan.json') and
                 isinstance(connection.get('session'), str) and _ID.fullmatch(connection['session']) and
                 type(connection.get('closed')) is bool and
                 _valid_process_identity(connection.get('coordinatorIdentity')) and
                 type(connection.get('heartbeat')) in (int, float) and math.isfinite(connection['heartbeat']))
        if not valid:
            raise BatchConnectionError('batch_connection_identity_mismatch')
        same_host = connection.get('host') == socket.gethostname()
        liveness = _identity_liveness(connection['coordinatorIdentity']) if same_host else 'unknown'
        age = time.time() - connection['heartbeat']
        fresh = -2 <= age <= 15
        report.update(session=connection['session'], coordinator=connection,
            coordinatorOutcome=connection.get('outcome'), liveness=liveness,
            controlAvailable=same_host and liveness == 'live' and fresh and not connection['closed'],
            commandPath=str(root / '.coordinators' / (connection['session'] + '-cmd.json')),
            checkpointPreserved=bool(connection.get('checkpointPreserved')))
        # This is only an observation of retained queue metadata. Explicit
        # resume still invokes all original source/artifact/full-state checks.
        between_runs = (state.get('active_run_id') is None and bool(report['pendingRunIds']) and
            all(row.get('status') in ('pending', 'completed') for row in state['runs'].values()))
        report['resumeAvailable'] = (same_host and liveness == 'ended' and connection['closed'] and
            state.get('status') in ('paused', 'interrupted') and (report['checkpointPreserved'] or between_runs))
        report['reason'] = ('batch_connection_foreign_host' if not same_host else
                            'batch_connection_session_ended' if liveness == 'ended' else
                            'batch_connection_owner_unverified' if liveness != 'live' else
                            'batch_connection_session_closed' if connection['closed'] else
                            'batch_connection_heartbeat_stale' if not fresh else 'verified_live_batch_session')
    except (OSError, ValueError, TypeError, KeyError):
        report['reason'] = 'batch_connection_identity_mismatch'
    return report


def list_batches(workspace):
    """List existing plans without creating storage or replaying inputs."""
    value = Path(workspace)
    if not value.is_absolute():
        raise BatchConnectionError('batch_connection_absolute_workspace_required')
    root = _safe_path(value) / '.gsw' / 'training-batches'
    _safe_path(root)
    batches = []
    if root.is_dir():
        entries = list(root.iterdir())
        if len(entries) > 10000:
            raise BatchConnectionError('batch_connection_inventory_limit')
        for entry in sorted(entries):
            if _ID.fullmatch(entry.name):
                batches.append(inspect_batch(entry / 'plan.json'))
    return {'schema': SCHEMA, 'version': 1, 'kind': 'training_batch_inventory', 'batches': batches}


def request_batch_command(plan_path, command, expected_session):
    """Write a verified batch inbox request, never start/kill a coordinator."""
    if command not in ('pause', 'cancel') or not isinstance(expected_session, str) or not _ID.fullmatch(expected_session):
        raise BatchConnectionError('batch_connection_command_invalid')
    report = inspect_batch(plan_path)
    if not report['controlAvailable'] or report.get('session') != expected_session:
        raise BatchConnectionError('batch_connection_control_unavailable_or_session_changed')
    target = _safe_path(report['commandPath'])
    receipt = {'schema': SCHEMA, 'version': 1, 'batchId': report['batchId'],
        'planSha256': report['planSha256'], 'planFileSha256': report['planFileSha256'],
        'session': expected_session, 'coordinatorIdentity': report['coordinator']['coordinatorIdentity'],
        'command': command, 'nonce': uuid.uuid4().hex, 'requestedAt': time.time()}
    with atomic_sibling(target) as (temporary, stream):
        json.dump(receipt, stream, allow_nan=False)
        stream.flush(); os.fsync(stream.fileno()); stream.close()
        try:
            os.link(str(temporary), str(_safe_path(target)))
        except FileExistsError:
            raise BatchConnectionError('batch_connection_command_pending')
    return receipt


class _Coordinator:
    def __init__(self, plan_path):
        self.root, self.plan, self.state, digest, _ = _plan_binding(plan_path)
        self.session = uuid.uuid4().hex
        self.path = self.root / '.coordinator.json'
        self.history = self.root / '.coordinators'
        self.inbox = self.history / (self.session + '-cmd.json')
        self.gate = OutputSessionGate(self.root, self.session)
        self.lock = threading.RLock()
        self.stopping = threading.Event()
        self.requested = set()
        self.record = {'schema': SCHEMA, 'version': 1, 'batchId': self.root.name,
            'planPath': str(self.root / 'plan.json'), 'planSha256': self.plan['plan_sha256'],
            'planFileSha256': digest, 'host': socket.gethostname(), 'session': self.session,
            'coordinatorIdentity': process_identity(os.getpid()), 'closed': False,
            'outcome': 'running', 'activeRunId': None, 'activeChild': None,
            'coordinatorSource': {'path': str(Path(__file__).resolve()),
                'sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},
            'rejectedCommands': [], 'checkpointPreserved': False}
        if not self.record['coordinatorIdentity']:
            raise BatchConnectionError('batch_connection_owner_identity_unavailable')

    def acquire(self):
        previous = inspect_batch(self.root / 'plan.json')
        child = previous.get('coordinator', {}).get('activeChild')
        if child is not None and (type(child) is not dict or _identity_liveness(child.get('identity')) != 'ended'):
            raise BatchConnectionError('batch_connection_active_child_unverified_or_live')
        if (self.path.exists() and previous.get('liveness') != 'ended' and not
                (previous.get('liveness') == 'live' and
                 previous.get('coordinator', {}).get('closed') is True)):
            # Closed is a durable control result, not proof that an unknown,
            # foreign-host owner has stopped writing. A local, fully verified
            # closed session can only be reused after the OS gate is released.
            raise BatchConnectionError('batch_connection_existing_owner_unverified_or_live')
        legacy = self.root / '.run-lock.json'
        if legacy.exists():
            value = _read_object(legacy)
            if value.get('host') != socket.gethostname() or not _legacy_pid_ended(value.get('pid')):
                raise BatchConnectionError('batch_connection_legacy_owner_unverified_or_live')
        self.gate.acquire()
        try:
            _safe_path(self.history).mkdir(exist_ok=True)
            self.write()
            self.thread = threading.Thread(target=self.heartbeat, daemon=True)
            self.thread.start()
            return self
        except BaseException:
            self.gate.release()
            raise

    def write(self):
        with self.lock:
            self.record['heartbeat'] = time.time()
            try:
                state = _read_object(self.root / 'state.json')
                self.record['activeRunId'] = state.get('active_run_id')
                self.record['queueState'] = state.get('status')
                self.record['checkpointPreserved'] = any(
                    type(row) is dict and bool(row.get('checkpoint')) for row in state.get('runs', {}).values())
            except (OSError, ValueError, TypeError):
                pass
            atomic_json(_safe_path(self.history / (self.session + '.json')), self.record)
            atomic_json(_safe_path(self.path), self.record)

    def heartbeat(self):
        while not self.stopping.wait(.5):
            try:
                self.write()
            except (OSError, ValueError):
                # Broken observation storage must not kill the owned trainer.
                pass

    def commands(self):
        if not self.inbox.exists():
            return []
        owned = self.history / (self.session + '-consumed-' + uuid.uuid4().hex + '.json')
        try:
            _safe_path(self.inbox)
            os.rename(str(self.inbox), str(owned))
            value = _read_object(owned, 4096)
            stamp = value.get('requestedAt')
            if (value.get('schema') != SCHEMA or value.get('version') != 1 or
                    value.get('batchId') != self.record['batchId'] or
                    value.get('planSha256') != self.record['planSha256'] or
                    value.get('planFileSha256') != self.record['planFileSha256'] or
                    value.get('session') != self.session or
                    value.get('coordinatorIdentity') != self.record['coordinatorIdentity'] or
                    value.get('command') not in ('pause', 'cancel') or
                    not isinstance(value.get('nonce'), str) or not _ID.fullmatch(value['nonce']) or
                    type(stamp) not in (int, float) or not math.isfinite(stamp) or not -2 <= time.time() - stamp <= 300):
                raise BatchConnectionError('batch_connection_command_identity_mismatch')
            operation = value['command']
            with self.lock:
                self.requested.add(operation)
                self.record['lastCommand'] = dict(value, acknowledgement='accepted_waiting_for_checkpoint')
                self.record['outcome'] = 'cancelling_checkpoint' if operation == 'cancel' else 'pausing'
                try:
                    self.write()
                except (OSError, ValueError) as error:
                    # A verified request was received. Observation-storage
                    # failure must neither kill the child nor silently lose
                    # its safe checkpoint request.
                    self.record['observationError'] = str(error)
            return ['pause']  # Both operations preserve the complete child state.
        except (OSError, ValueError, TypeError, KeyError) as error:
            with self.lock:
                self.record['rejectedCommands'] = (self.record['rejectedCommands'] +
                    [{'reason': str(error), 'at': time.time()}])[-32:]
            return []
        finally:
            try:
                if owned.exists():
                    owned.unlink()
            except OSError:
                # Preserve an undeletable, already consumed command as
                # quarantine evidence. Observer input must never escape into
                # the executor's finally block and terminate its owned child.
                with self.lock:
                    self.record['quarantinedCommandPath'] = str(owned)

    def factory(self, external):
        def start(command, **options):
            configuration = Path(command[command.index('--config') + 1])
            value = _read_object(configuration)
            metadata = {'configurationPath': str(configuration),
                'configurationSha256': hashlib.sha256(configuration.read_bytes()).hexdigest(),
                'backend': value.get('backend'), 'output': str(Path(value['outputRoot']) / value['outputScene'])}
            with self.lock:
                # Publish BEFORE CreateProcess. If the coordinator dies in the
                # spawn/registration gap, an unknown child marker blocks queue
                # reuse and project relocation instead of claiming no writer.
                self.record['activeChild'] = dict(metadata, identity=None, launchState='starting')
                self.write()
            child = external(command, **options)
            try:
                with self.lock:
                    self.record['activeChild'] = dict(metadata, identity=process_identity(child.pid), launchState='started')
                    self.write()
            except (OSError, ValueError, TypeError) as error:
                # Launch has succeeded. Never throw before the original
                # executor receives its Popen and can reap/control that child.
                with self.lock:
                    self.record['observationError'] = str(error)
            return child
        return start

    def close(self, outcome, returncode=None):
        self.stopping.set()
        self.thread.join(timeout=2)
        try:
            with self.lock:
                self.record.update(closed=True, outcome=outcome, activeChild=None)
                self.record['executorReturncode'] = returncode
                if self.record.get('lastCommand'):
                    # Only exit75 means the executor verified this active
                    # child's complete checkpoint. Exit76 is a safe queue
                    # boundary, not newly saved optimizer state; historical
                    # checkpoints in some other row cannot change that.
                    acknowledgement = ('checkpointed_and_stopped' if returncode == 75 else
                        'queue_stopped_between_runs' if returncode == 76 else 'coordinator_ended')
                    self.record['lastCommand']['acknowledgement'] = acknowledgement
                self.write()
        finally:
            self.gate.release()


def run_persistent_batch(plan_path, resume=False, process_factory=None):
    """Explicit new coordinator session; optional external process adapter."""
    coordinator = _Coordinator(plan_path).acquire()
    outcome = 'interrupted'
    code = None
    try:
        # Preserve the old executor, plan fingerprints and full-state checks.
        # Observation does not import this module or any training backend.
        from native.worker.training_batch import run_training_batch
        code = run_training_batch(plan_path, resume=resume,
            process_factory=coordinator.factory(process_factory or subprocess.Popen),
            control_reader=coordinator.commands)
        outcome = ('completed' if code == 0 else 'cancelled' if 'cancel' in coordinator.requested and code in (75, 76)
                   else 'paused' if code in (75, 76) else 'failed')
        return 130 if outcome == 'cancelled' else code
    finally:
        coordinator.close(outcome, code)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--operation', required=True, choices=('inspect', 'list', 'command', 'run', 'resume'))
    parser.add_argument('--plan'); parser.add_argument('--workspace')
    parser.add_argument('--command', choices=('pause', 'cancel')); parser.add_argument('--session')
    args = parser.parse_args(argv)
    try:
        if args.operation in ('run', 'resume'):
            return run_persistent_batch(args.plan, resume=args.operation == 'resume')
        value = (list_batches(args.workspace) if args.operation == 'list' else
                 request_batch_command(args.plan, args.command, args.session) if args.operation == 'command' else
                 inspect_batch(args.plan))
        print(json.dumps(value, ensure_ascii=False, allow_nan=False), flush=True)
        return 0
    except (OSError, ValueError, TypeError) as error:
        print(json.dumps({'schema': SCHEMA, 'success': False, 'error': str(error)}, ensure_ascii=False), flush=True)
        return 2


if __name__ == '__main__':
    sys.exit(main())
