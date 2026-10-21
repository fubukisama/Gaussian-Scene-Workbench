"""Bounded CPU-only lease fixture, never a Gaussian trainer/checkpoint."""
import argparse
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from native.worker.training_checkpoint import atomic_json
from native.worker.training_job_connection import OutputSessionGate, WorkerConnection, process_identity

parser=argparse.ArgumentParser()
parser.add_argument('--store',required=True)
parser.add_argument('--job-id',required=True)
parser.add_argument('--mode', choices=('observer', 'mainwindow'), default='observer')
parser.add_argument('--backend', choices=('3dgs', '2dgs'))
parser.add_argument('--output')
parser.add_argument('--configuration')
parser.add_argument('--phase-prefix')
args=parser.parse_args()
store=Path(args.store)
path=store/(args.job_id+'.json')


def mainwindow_fixture():
    """Remote MainWindow acceptance only: real process/lease, no trainer/state.

    Hold before job registration and after the terminal acknowledgement so
    migration protection is tested across the entire owning process lifetime.
    Every release path is bounded and belongs to this isolated fixture.
    """
    if not all((args.backend, args.output, args.configuration, args.phase_prefix)):
        parser.error('mainwindow mode requires backend, output, configuration and phase-prefix')
    prefix = args.phase_prefix
    marker = lambda suffix: Path(prefix + '-' + suffix)
    deadline = time.monotonic() + 30

    def wait_marker(suffix):
        while time.monotonic() < deadline:
            if marker('abort').exists():
                return False
            if marker(suffix).exists():
                return True
            time.sleep(.025)
        return False

    import uuid
    session = uuid.uuid4().hex
    store.mkdir(parents=True, exist_ok=True)
    identity = process_identity(os.getpid())
    if identity is None:
        return 4
    with OutputSessionGate(args.output, session) as gate:
        atomic_json(marker('ready.json'), {'workerIdentity': identity, 'session': session})
        print('OWNED_CPU_PRE_REGISTRATION_READY', flush=True)
        if not wait_marker('register'):
            return 5
        control = {'session': session, 'backend': args.backend, 'output': str(Path(args.output).absolute())}
        job = {'id': args.job_id, 'backend': args.backend, 'status': 'running',
               'stage': 'train', 'iteration': 3, 'total_iterations': 10,
               'native_control': control}
        atomic_json(path, job)
        connection = WorkerConnection(store, args.job_id, control, args.configuration, output_gate=gate)
        try:
            atomic_json(marker('registered.json'), {'workerIdentity': identity})
            print('[worker-event] ' + json.dumps(dict(version=1, type='status', state='running',
                                                     stage='train', iteration=3, totalIterations=10)), flush=True)
            while time.monotonic() < deadline and not marker('abort').exists():
                connection.heartbeat()
                command = connection.take_command()
                if command:
                    job['status'] = 'paused' if command == 'pause' else 'cancelled'
                    job['stage'] = job['status']
                    atomic_json(path, job)
                    connection.close(job['status'])
                    atomic_json(marker('ack.json'), {'command': command, 'workerIdentity': identity,
                                                    'state': job['status']})
                    print('[worker-event] ' + json.dumps(dict(version=1, type='status',
                        state=job['status'], stage=job['status'])), flush=True)
                    # A terminal job row is not proof that its owning process
                    # has stopped writing/released its output ownership yet.
                    return 0 if wait_marker('exit') else 6
                time.sleep(.025)
            return 7
        finally:
            connection.close(job['status'])


if args.mode == 'mainwindow':
    raise SystemExit(mainwindow_fixture())

job=json.loads(path.read_text(encoding='utf-8'))
connection=WorkerConnection(store,args.job_id,job['native_control'],store/'configuration.json')
print('OWNED_CPU_LEASE_READY',flush=True)
deadline=time.perf_counter()+30
try:
    while time.perf_counter()<deadline:
        connection.heartbeat()
        command=connection.take_command()
        if command:
            job['status']='paused' if command=='pause' else 'cancelled'
            job['stage']=job['status']
            atomic_json(path,job)
            connection.close(job['status'])
            print('OWNED_CPU_COMMAND_'+command,flush=True)
            break
        time.sleep(.05)
    else:
        connection.close('fixture_timeout')
finally:
    connection.close(job['status'])
