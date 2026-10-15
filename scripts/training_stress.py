"""Bounded, opt-in native training stress test with independent input copies.

This is a PLY warm-start experiment, NOT optimizer-state resume. The original
photos, reconstruction, model and checkpoints are never passed as writable
training paths. Only this harness's child process can be stopped.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 ** 2), b''):
            value.update(block)
    return value.hexdigest()


def memory_observer_command(repository, trainer_pid, windows_root=None):
    """Use the Windows component's absolute location, not a Conda PATH lookup."""
    system = Path(windows_root or os.environ.get('SystemRoot', 'C:/Windows'))
    executable = system / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    if not executable.is_file():
        raise FileNotFoundError('Windows PowerShell is unavailable: ' + str(executable))
    return [str(executable), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(Path(repository) / 'scripts/training_memory_monitor.ps1'),
            '-TrainerPid', str(trainer_pid)]


def pause_owned_process(process, request, session, grace=45):
    """A failed observer must not leave this harness's trainer unsupervised."""
    request.write_text(json.dumps({'session': session}), encoding='utf-8')
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def child(arguments):
    import runpy
    import torch
    # A hard allocator cap prevents these experiments from intentionally
    # consuming all physical VRAM or relying on Windows shared GPU memory.
    torch.cuda.set_per_process_memory_fraction(.75)
    sys.path.insert(0, str(Path(arguments[0]).resolve().parent))
    sys.argv = [arguments[0]] + arguments[1:]
    runpy.run_path(arguments[0], run_name='__main__')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--child', nargs=argparse.REMAINDER)
    parser.add_argument('--dataset', type=Path)
    parser.add_argument('--model-ply', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--python', type=Path)
    parser.add_argument('--backend', choices=('3dgs', '2dgs'), default='3dgs')
    parser.add_argument('--case', choices=('legacy', 'automatic', 'pressure', 'growth'), default='automatic')
    parser.add_argument('--points', type=int, default=1000000)
    parser.add_argument('--iterations', type=int, default=60)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--memory-observer', action='store_true')
    parser.add_argument('--baseline-runtime', action='store_true',
                        help='Keep the previous bounded runtime but disable the new exact-storage/prefetch/auxiliary features.')
    args = parser.parse_args()
    if args.child is not None:
        child(args.child)
        return 0
    if any(value is None for value in (args.dataset, args.output, args.python)):
        parser.error('--dataset, --output and --python are required')
    source = args.dataset.resolve(strict=True)
    root = args.output.resolve()
    if root.exists() or source == root or source in root.parents or root in source.parents:
        raise ValueError('Fresh independent output outside source data is required')
    if not 1 <= args.points <= 3000000 or not 2 <= args.iterations <= 200 or not 10 <= args.timeout <= 600:
        raise ValueError('Stress limits exceed bounded acceptance scope')
    repository = Path(__file__).resolve().parents[1]
    if args.memory_observer and os.name == 'nt':
        memory_observer_command(repository, 0)  # Resolve before any trainer is launched.
    files = [path for folder in ('images', 'sparse') for path in (source / folder).rglob('*') if path.is_file()]
    if args.model_ply:
        files.append(args.model_ply.resolve(strict=True))
    before = {str(path): digest(path) for path in files}
    root.mkdir(parents=True)
    dataset = root / 'input-copy'
    for folder in ('images', 'sparse'):
        shutil.copytree(source / folder, dataset / folder)
    output = root / 'model'
    output.mkdir()
    warm_iteration = None
    if args.model_ply:
        if args.backend != '3dgs':
            raise ValueError('A 3DGS warm-start PLY is not a 2DGS optimizer/model state')
        import numpy as np
        from plyfile import PlyData, PlyElement
        original = PlyData.read(str(args.model_ply))
        rows = original['vertex'].data
        indices = np.linspace(0, len(rows) - 1, min(args.points, len(rows)), dtype=np.int64)
        destination = output / 'point_cloud/iteration_1/point_cloud.ply'
        destination.parent.mkdir(parents=True)
        PlyData([PlyElement.describe(rows[indices].copy(), 'vertex')], text=False).write(str(destination))
        warm_iteration = 1
    session = uuid.uuid4().hex
    request = root / 'pause-request.json'
    performance = {'enabled': args.case != 'legacy', 'profile_interval': 1,
                   'usage_fraction': .5 if args.case == 'pressure' else .9,
                   'minimum_spare_mib': 8192 if args.case == 'pressure' else 512}
    if args.baseline_runtime:
        performance.update(camera_prefetch=False, capacity_storage=False, surfel_auxiliary=False)
    control = {'backend': args.backend, 'output': str(output), 'request': str(request),
               'session': session, 'resume': False, 'performance': performance}
    env = os.environ.copy()
    env.update(PYTHONUTF8='1', MPLBACKEND='Agg', GSW_NATIVE_TRAINING_CONTROL=json.dumps(control))
    loss_path = env.get('GSW_TRAINING_LOSS_MODULE_PATH_' + args.backend.upper())
    if loss_path:
        env['GSW_TRAINING_LOSS_MODULE_PATH'] = loss_path
    source_backend = (repository / 'gaussian-splatting' if args.backend == '3dgs'
                      else Path(env['TWO_DGS_DIR']))
    env['GSW_TWO_DGS_SOURCE'] = str(source_backend)
    entry = source_backend / 'train.py' if args.backend == '3dgs' else repository / 'native/worker/two_dgs_train.py'
    total = args.iterations + (warm_iteration or 0)
    command = [str(args.python), '-B', str(Path(__file__).resolve()), '--child', str(entry),
               '-s', str(dataset), '-m', str(output), '-r', '1', '--data_device', 'cpu',
               '--iterations', str(total), '--test_iterations', str(total + 1), '--save_iterations', str(total),
               '--densify_from_iter', '0',
               '--densify_until_iter', str(total) if args.case in ('pressure', 'growth') else '0',
               '--densification_interval', '2', '--densify_grad_threshold', '0.00000001']
    if args.backend == '3dgs':
        command += ['--disable_viewer', '--optimizer_type', 'default', '--antialiasing']
        if warm_iteration:
            command += ['--load_iteration', str(warm_iteration)]
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    started = time.monotonic()
    interrupted = False
    with (root / 'training.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, cwd=str(source_backend), env=env,
                                   stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
        monitor = None
        monitor_log = None
        if args.memory_observer and os.name == 'nt':
            monitor_log = (root / 'memory-observations.jsonl').open('w', encoding='utf-8')
            try:
                monitor = subprocess.Popen(memory_observer_command(repository, process.pid),
                    stdout=monitor_log, stderr=subprocess.STDOUT, creationflags=flags)
            except OSError:
                monitor_log.close()
                pause_owned_process(process, request, session)
                raise
        while process.poll() is None:
            elapsed = time.monotonic() - started
            if elapsed > args.timeout and not interrupted:
                request.write_text(json.dumps({'session': session}), encoding='utf-8')
                interrupted = True
            if elapsed > args.timeout + 45:
                process.terminate()  # Only the test-owned trainer, never a user job.
                break
            time.sleep(.25)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
        if monitor is not None:
            try:
                monitor.wait(timeout=10)
            except subprocess.TimeoutExpired:
                monitor.terminate()
                monitor.wait(timeout=10)
            monitor_log.close()
    events = []
    for line in (root / 'training.log').read_text(encoding='utf-8', errors='replace').splitlines():
        for name in ('performance', 'growth', 'runtime', 'image-cache', 'image-staging',
                     'capacity-storage', 'metrics', 'quality-profile'):
            prefix = '[gsw-training-' + name + '] '
            if prefix in line:
                encoded = line.split(prefix, 1)[1]
                try:
                    payload, _ = json.JSONDecoder().raw_decode(encoded)
                    events.append({'type': name, 'payload': payload})
                except ValueError:
                    pass
    unchanged = all(path.is_file() and digest(path) == before[str(path)] for path in files)
    report = {'version': 1, 'backend': args.backend, 'case': args.case,
              'baseline_runtime': args.baseline_runtime,
              'source_unchanged': unchanged, 'source_hashes': before,
              'photos': len(list((dataset / 'images').glob('*'))),
              'warm_start_only': bool(warm_iteration), 'optimizer_resume': False,
              'allocator_fraction_cap': .75, 'returncode': process.returncode,
              'timeout_pause_requested': interrupted, 'duration_seconds': time.monotonic() - started,
              'events': events}
    (root / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: report[key] for key in report if key not in ('source_hashes', 'events')}, ensure_ascii=False))
    return 0 if unchanged and process.returncode in (0, 75) and events else 1


if __name__ == '__main__':
    raise SystemExit(main())
