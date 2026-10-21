# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Read-only existing-job observation, separate from the owning trainer.

Importing this module never imports compute.py, repairs job states, loads a
checkpoint, starts training or acquires ownership of an observed process.
"""
import json
import os
import argparse
import hashlib
from pathlib import Path
import re
import stat
import time
import uuid

from native.worker.training_checkpoint import atomic_json
from native.backend.atomic_file import atomic_sibling

_ID = re.compile(r'^[0-9a-f]{32}$')


class JobConnectionError(ValueError):
    pass


def _safe_path(path):
    path = Path(path).absolute()
    if os.name == 'nt' and len(str(path)) >= 260 and not str(path).startswith('\\\\?\\'):
        # Path.exists() can hide MAX_PATH failures as a missing record. Never
        # interpret that as evidence that an output has no active owner.
        raise JobConnectionError('job_connection_path_too_long')
    if '..' in path.parts:
        raise JobConnectionError('job_connection_path_invalid')
    for node in (path,) + tuple(path.parents):
        metadata = node.lstat() if node.exists() else None
        if (node.is_symlink() or (metadata is not None and
                getattr(metadata, 'st_file_attributes', 0) & 0x400)):
            raise JobConnectionError('job_connection_link_refused')
    return path


def _read_object(path, limit=16 * 1024 * 1024):
    path = _safe_path(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise JobConnectionError('job_connection_record_unavailable')
    with path.open('r', encoding='utf-8') as stream:
        result = json.load(stream)
    if type(result) is not dict:
        raise JobConnectionError('job_connection_record_invalid')
    return result


def process_identity(pid):
    """PID plus OS creation identity; never infer ownership from PID alone."""
    if type(pid) is not int or pid < 1:
        return None
    if os.name == 'nt':
        if pid > 0xffffffff:
            return None  # ctypes DWORD conversion must never wrap to another PID.
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        kernel.GetProcessTimes.restype = wintypes.BOOL
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        try:
            status = wintypes.DWORD()
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(status)) or status.value != 259:
                return None
            times = [wintypes.FILETIME() for _ in range(4)]
            if not kernel.GetProcessTimes(handle, *[ctypes.byref(value) for value in times]):
                return None
            token = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
            return {'pid': pid, 'startToken': 'win-filetime:' + str(token)}
        finally:
            kernel.CloseHandle(handle)
    try:
        # Linux verification workers: field22 is starttime after the final ')'.
        stat = Path('/proc/{}/stat'.format(pid)).read_text()
        fields = stat[stat.rfind(')') + 2:].split()
        if fields[0] == 'Z':
            return None
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        return {'pid': pid, 'startToken': 'linux:' + boot + ':' + fields[19]}
    except (OSError, IndexError):
        return None


def _valid_process_identity(identity):
    if (type(identity) is not dict or set(identity) != {'pid', 'startToken'} or
            type(identity.get('pid')) is not int or
            identity['pid'] < 1 or not isinstance(identity.get('startToken'), str)):
        return False
    if os.name == 'nt':
        return (identity['pid'] <= 0xffffffff and
                re.fullmatch(r'win-filetime:[1-9][0-9]{0,19}', identity['startToken']) is not None and
                int(identity['startToken'].split(':',1)[1]) <= 2**64-1)
    return re.fullmatch(r'linux:[0-9a-f-]+:[0-9]+', identity['startToken']) is not None


def _identity_liveness(identity):
    """Distinguish proved exit from failed observation before reusing a gate."""
    if not _valid_process_identity(identity):
        return 'unknown'
    current = process_identity(identity['pid'])
    if current is not None:
        return 'live' if current == identity else 'ended'
    if os.name == 'nt':
        if identity['pid'] > 0xffffffff or not identity['startToken'].startswith('win-filetime:'):
            return 'unknown'
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        handle = kernel.OpenProcess(0x1000, False, identity['pid'])
        if not handle:
            return 'ended' if ctypes.get_last_error() == 87 else 'unknown'
        try:
            status = wintypes.DWORD()
            if kernel.GetExitCodeProcess(handle, ctypes.byref(status)) and status.value != 259:
                return 'ended'
            return 'unknown'
        finally:
            kernel.CloseHandle(handle)
    try:
        path = Path('/proc/{}/stat'.format(identity['pid']))
        value = path.read_text()
        fields = value[value.rfind(')') + 2:].split()
        return 'ended' if fields[0] == 'Z' else 'unknown'
    except FileNotFoundError:
        return 'ended' if identity['startToken'].startswith('linux:') else 'unknown'
    except (OSError, IndexError):
        return 'unknown'


def _canonical_output(output):
    return _safe_path(_safe_path(output).resolve())


class OutputSessionGate:
    """Nonblocking, process-held exclusive writer gate for one output directory.

    Acquire before starting a trainer, not merely when attaching its observer.
    The durable record is never unlinked or replaced while its OS lock is held.
    An abandoned record is reusable only after an explicit close or proved exit;
    an unreadable, malformed or otherwise unverified old owner is not displaced.
    """
    def __init__(self, output, session):
        if not isinstance(session, str) or not _ID.fullmatch(session):
            raise JobConnectionError('job_connection_session_invalid')
        self.output = _canonical_output(output)
        self.session = session
        key = hashlib.sha256(os.path.normcase(str(self.output)).encode('utf-8')).hexdigest()[:32]
        self.path = self.output.parent / '.gsw-locks' / (key + '.json')
        self._fd = None
        self._record = None

    @property
    def active(self):
        return self._fd is not None

    def matches(self, output, session):
        return (self.active and session == self.session and
                os.path.normcase(str(_canonical_output(output))) == os.path.normcase(str(self.output)) and
                self._record['workerIdentity'] == process_identity(os.getpid()))

    @staticmethod
    def _lock(fd, acquire):
        if os.name == 'nt':
            import msvcrt
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK if acquire else msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(fd, (fcntl.LOCK_EX | fcntl.LOCK_NB) if acquire else fcntl.LOCK_UN)

    def _verify_file(self):
        _safe_path(self.path)
        actual, opened = self.path.stat(), os.fstat(self._fd)
        if (not stat.S_ISREG(opened.st_mode) or
                (actual.st_dev, actual.st_ino) != (opened.st_dev, opened.st_ino)):
            raise JobConnectionError('output_session_path_changed')

    def _write(self):
        self._verify_file()
        data = b'\n' + json.dumps(self._record, ensure_ascii=True, allow_nan=False).encode('utf-8')
        os.lseek(self._fd, 0, os.SEEK_SET)
        offset = 0
        while offset < len(data):
            written = os.write(self._fd, data[offset:])
            if written < 1:
                raise OSError('output_session_write_failed')
            offset += written
        os.ftruncate(self._fd, len(data))
        os.fsync(self._fd)

    def acquire(self):
        if self.active:
            raise JobConnectionError('output_session_already_acquired')
        identity = process_identity(os.getpid())
        if identity is None:
            raise JobConnectionError('worker_identity_unavailable_or_ended')
        _safe_path(self.path.parent).mkdir(parents=True, exist_ok=True)
        _safe_path(self.path)
        flags = os.O_RDWR | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
        try:
            fd = os.open(str(self.path), flags | os.O_CREAT | os.O_EXCL, 0o600)
            created = True
        except FileExistsError:
            fd, created = os.open(str(self.path), flags), False
        locked = False
        try:
            try:
                self._lock(fd, True)
            except OSError:
                raise JobConnectionError('output_session_busy')
            locked = True
            self._fd = fd
            self._verify_file()
            if not created:
                os.lseek(fd, 0, os.SEEK_SET)
                data = os.read(fd, 64 * 1024 + 1)
                try:
                    previous = json.loads(data[1:].decode('utf-8')) if data.startswith(b'\n') else None
                except (ValueError, UnicodeError):
                    previous = None
                if (len(data) > 64 * 1024 or type(previous) is not dict or
                        previous.get('version') != 1 or not isinstance(previous.get('output'), str) or
                        os.path.normcase(previous['output']) != os.path.normcase(str(self.output)) or
                        not isinstance(previous.get('session'), str) or not _ID.fullmatch(previous['session']) or
                        type(previous.get('closed')) is not bool or
                        not _valid_process_identity(previous.get('workerIdentity'))):
                    raise JobConnectionError('output_session_owner_unverified')
                if not previous['closed']:
                    liveness = _identity_liveness(previous['workerIdentity'])
                    if liveness != 'ended':
                        raise JobConnectionError('output_session_busy' if liveness == 'live'
                                                 else 'output_session_owner_unverified')
            self._record = {'version': 1, 'output': str(self.output), 'session': self.session,
                            'workerIdentity': identity, 'closed': False, 'acquiredAt': time.time()}
            self._write()
            return self
        except BaseException:
            self._fd = None
            try:
                if locked:
                    self._lock(fd, False)
            finally:
                os.close(fd)
            raise

    def release(self):
        if self._fd is None:
            return
        fd = self._fd
        try:
            self._record.update(closed=True, releasedAt=time.time())
            self._write()
        finally:
            self._fd = None
            try:
                self._lock(fd, False)
            finally:
                os.close(fd)

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *_):
        self.release()


def _connection_paths(store, session):
    if not isinstance(session, str) or not _ID.fullmatch(session):
        raise JobConnectionError('job_connection_session_invalid')
    root = _safe_path(store / '.c')
    return root / (session + '.json'), root / (session + '-cmd.json')


def ensure_output_not_active(job_store, output):
    """Read-only legacy preflight, called under the new output gate.

    A running row is not repaired to failed, nor assumed dead from a stale
    heartbeat. Only a matching closed lease or proved OS identity exit permits
    reuse. Gate-less legacy workers in unknown job stores cannot be discovered.
    """
    store, target = _safe_path(job_store), _canonical_output(output)
    try:
        metadata = store.stat()
    except FileNotFoundError:
        return {'version': 1, 'checkedJobs': 0, 'endedJobs': 0}
    if not stat.S_ISDIR(metadata.st_mode):
        raise JobConnectionError('job_connection_store_unavailable')
    paths = [path for path in store.iterdir() if path.suffix == '.json' and _ID.fullmatch(path.stem)]
    if len(paths) > 4096:
        raise JobConnectionError('job_connection_inventory_limit')
    checked, ended = 0, 0
    for path in sorted(paths):
        job = _read_object(path)
        if job.get('status') in ('done', 'failed', 'cancelled', 'paused'):
            continue
        control = job.get('native_control')
        recorded_output = control.get('output') if type(control) is dict else None
        if isinstance(recorded_output, str):
            if os.path.normcase(str(_canonical_output(recorded_output))) != os.path.normcase(str(target)):
                continue
        elif job.get('output_scene') != target.name:
            continue
        checked += 1
        if job.get('id') != path.stem or type(control) is not dict:
            raise JobConnectionError('output_session_existing_job_unverified')
        try:
            lease_path, _ = _connection_paths(store, control.get('session'))
            lease = _read_object(lease_path, 64 * 1024)
        except (OSError, ValueError):
            raise JobConnectionError('output_session_existing_job_unverified')
        if (lease.get('version') != 1 or lease.get('jobId') != job['id'] or
                lease.get('session') != control['session'] or lease.get('backend') != job.get('backend') or
                lease.get('output') != recorded_output or type(lease.get('closed')) is not bool or
                not _valid_process_identity(lease.get('workerIdentity'))):
            raise JobConnectionError('output_session_existing_job_unverified')
        if lease['closed']:
            ended += 1
            continue
        liveness = _identity_liveness(lease['workerIdentity'])
        if liveness == 'ended':
            ended += 1
            continue
        raise JobConnectionError('output_session_existing_job_active' if liveness == 'live'
                                 else 'output_session_existing_job_unverified')
    return {'version': 1, 'checkedJobs': checked, 'endedJobs': ended}


def inspect_job(job_store, job_id):
    """Return recorded state without claiming a stale running row is live."""
    if not isinstance(job_id, str) or not _ID.fullmatch(job_id):
        raise JobConnectionError('job_connection_id_invalid')
    store = _safe_path(job_store)
    job = _read_object(store / (job_id + '.json'))
    if job.get('id') != job_id or job.get('backend') not in ('3dgs', '2dgs'):
        raise JobConnectionError('job_connection_identity_mismatch')
    report = {'version': 1, 'jobId': job_id, 'jobStore': str(store),
            'backend': job['backend'], 'recordedState': job.get('status', 'unknown'),
            'stage': job.get('stage', 'unknown'), 'iteration': job.get('iteration'),
            'totalIterations': job.get('total_iterations'),
            'liveness': 'unverified', 'controlAvailable': False,
            'reason': 'legacy_connection_metadata_missing', 'job': job}
    control = job.get('native_control')
    runtime = job.get('runtimeConfiguration')
    report['runtimeConfigurationObserved'] = type(runtime) is dict
    report['runtimeConfiguration'] = runtime if type(runtime) is dict else (
        control.get('runtimeConfigurationReport') if type(control) is dict else None)
    report['configurationPath'] = None
    report['outputDirectory'] = None
    report['checkpointState'] = 'no_checkpoint'
    from native.worker.gsw_worker import status_telemetry
    observed_state = (job.get('status') if job.get('status') in ('done', 'failed', 'cancelled', 'paused')
                      else 'unknown')
    report['statusEvent'] = dict(version=1, type='status', state=observed_state,
                                 stage=job.get('stage', 'unknown'), **status_telemetry(job))
    if type(control) is dict and isinstance(control.get('output'), str):
        output = _safe_path(control['output'])
        report['outputDirectory'] = str(output)
        if _safe_path(output / '.gsw-resume' / 'ready.json').is_file():
            # Presence is not checksum/optimizer verification. Resume performs
            # those checks after the native explicit trust confirmation.
            report['checkpointState'] = 'available_unverified'
    if report['recordedState'] in ('done', 'failed', 'cancelled', 'paused'):
        report['liveness'] = 'ended'
    if type(control) is not dict or not control.get('session'):
        return report
    lease_path, _ = _connection_paths(store, control['session'])
    if not lease_path.is_file():
        return report
    lease = _read_object(lease_path, 64 * 1024)
    if (lease.get('version') != 1 or lease.get('jobId') != job_id or
            lease.get('session') != control['session'] or
            lease.get('backend') != job['backend'] or
            lease.get('output') != control.get('output')):
        raise JobConnectionError('job_connection_identity_mismatch')
    report.update(connectionRecord=str(lease_path),
                  configurationPath=lease.get('configurationPath'),
                  outputDirectory=lease.get('output'), workerIdentity=lease.get('workerIdentity'))
    if lease.get('closed') is True:
        report.update(liveness='ended', reason='worker_session_ended')
        return report
    timestamp = lease.get('heartbeat')
    if (type(timestamp) not in (int, float) or not -2 <= time.time() - timestamp <= 15):
        report['reason'] = 'worker_heartbeat_stale'
        return report
    identity = lease.get('workerIdentity')
    if (type(identity) is not dict or process_identity(identity.get('pid')) != identity):
        report['reason'] = 'worker_identity_unavailable_or_ended'
        return report
    live_state = job.get('status') in ('queued', 'running', 'cancelling')
    report.update(liveness='live', controlAvailable=live_state,
                  reason='verified_live_session' if live_state else 'job_is_terminal')
    report['statusEvent']['state'] = job.get('status', 'unknown')
    return report


class WorkerConnection:
    """Worker-owned lease and command inbox; observers never own this object."""
    def __init__(self, job_store, job_id, control, configuration_path=None, output_gate=None):
        current = inspect_job(job_store, job_id)
        self.store, self.job_id = _safe_path(job_store), job_id
        self.session, self.backend = control['session'], control['backend']
        self.path, self.command_path = _connection_paths(self.store, self.session)
        if current['job'].get('native_control') != control or current['backend'] != self.backend:
            raise JobConnectionError('job_connection_identity_mismatch')
        identity = process_identity(os.getpid())
        if identity is None:
            raise JobConnectionError('worker_identity_unavailable_or_ended')
        self._owns_gate = output_gate is None
        self.output_gate = output_gate or OutputSessionGate(control['output'], self.session).acquire()
        try:
            if not self.output_gate.matches(control['output'], self.session):
                raise JobConnectionError('output_session_gate_mismatch')
            self.path.parent.mkdir(exist_ok=True)
            _safe_path(self.path.parent)
            self.record = {'version': 1, 'jobId': job_id, 'session': self.session,
                           'backend': self.backend, 'output': control['output'],
                           'configurationPath': str(_safe_path(configuration_path)) if configuration_path else None,
                           'workerIdentity': identity, 'closed': False}
            self.heartbeat()
        except BaseException:
            if self._owns_gate:
                self.output_gate.release()
            raise

    def heartbeat(self):
        self.record['heartbeat'] = time.time()
        atomic_json(_safe_path(self.path), self.record)

    def take_command(self):
        if not self.command_path.exists():
            return None
        _safe_path(self.command_path)
        owned = self.command_path.with_name(uuid.uuid4().hex)
        os.rename(str(self.command_path), str(owned))
        try:
            request = _read_object(owned, 4096)
            if (request.get('session') != self.session or request.get('jobId') != self.job_id or
                    request.get('workerIdentity') != self.record['workerIdentity'] or
                    request.get('command') not in ('pause', 'stop') or
                    not isinstance(request.get('nonce'), str) or not _ID.fullmatch(request['nonce'])):
                raise JobConnectionError('job_connection_command_invalid')
            return request['command']
        finally:
            owned.unlink()

    def close(self, state):
        try:
            self.record.update(closed=True, finalState=str(state))
            self.heartbeat()
        finally:
            if self._owns_gate:
                self.output_gate.release()


def request_command(job_store, job_id, command):
    """Publish a session-bound request, never kill a PID or start training."""
    if command not in ('pause', 'stop'):
        raise JobConnectionError('job_connection_command_invalid')
    report = inspect_job(job_store, job_id)
    if not report['controlAvailable']:
        raise JobConnectionError('job_connection_control_unavailable')
    if command == 'pause' and report.get('stage') == 'coordinate_export':
        raise JobConnectionError('job_connection_training_already_completed')
    session = report['job']['native_control']['session']
    _, target = _connection_paths(_safe_path(job_store), session)
    receipt = {'version': 1, 'jobId': job_id, 'session': session,
               'workerIdentity': report['workerIdentity'], 'command': command,
               'nonce': uuid.uuid4().hex, 'requestedAt': time.time()}
    with atomic_sibling(target) as (temporary, stream):
        json.dump(receipt, stream)
        stream.flush()
        os.fsync(stream.fileno())
        stream.close()
        # An inbox slot is exclusive. Never overwrite another pending action.
        try:
            os.link(str(temporary), str(_safe_path(target)))
        except FileExistsError:
            raise JobConnectionError('job_connection_command_pending')
    return receipt


def list_jobs(job_store):
    """Bounded identity list; listing never repairs or mutates recorded jobs."""
    store = _safe_path(job_store)
    if not store.is_dir():
        raise JobConnectionError('job_connection_store_unavailable')
    paths = [path for path in store.iterdir() if path.suffix == '.json' and _ID.fullmatch(path.stem)]
    if len(paths) > 4096:
        raise JobConnectionError('job_connection_inventory_limit')
    jobs = []
    for path in sorted(paths):
        report = inspect_job(store, path.stem)
        jobs.append({key: report[key] for key in (
            'jobId', 'backend', 'recordedState', 'iteration', 'totalIterations',
            'liveness', 'controlAvailable', 'reason')})
        jobs[-1]['outputName'] = report['job'].get('output_scene', path.stem)
    return {'version': 1, 'jobStore': str(store), 'jobs': jobs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--job-store', required=True)
    parser.add_argument('--job-id')
    parser.add_argument('--command', choices=('pause', 'stop'))
    args = parser.parse_args()
    try:
        if args.command and not args.job_id:
            raise JobConnectionError('job_connection_id_invalid')
        result = (request_command(args.job_store, args.job_id, args.command) if args.command else
                  inspect_job(args.job_store, args.job_id) if args.job_id else list_jobs(args.job_store))
        print(json.dumps(result, ensure_ascii=True, allow_nan=False), flush=True)
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({'version': 1, 'error': str(error)}, ensure_ascii=True), flush=True)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
