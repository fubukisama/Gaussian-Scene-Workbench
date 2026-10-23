"""D-only isolated quality-kernel build; dry-run unless --compile is explicit.

Never installs into an environment, overwrites stock sources or generates GPU
acceptance. Build success is not an advertised capability.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def sha256(path, text=False):
    content = Path(path).read_bytes()
    return hashlib.sha256(content.replace(b'\r\n', b'\n') if text else content).hexdigest()


def d_path(path):
    path = Path(path).resolve()
    if path.drive.upper() != 'D:':
        raise ValueError('quality_rasterizer_requires_d_drive')
    return path


def read_windows_batch_environment(setup_path, inherited=None):
    """Run one real setup batch hidden and decode `cmd /u` environment output.

    Windows subprocess sequences pass through list2cmdline, whose backslash-quote
    escaping is not cmd.exe command-language escaping. Pass a complete command
    line string instead; /s strips only the outer /c quote pair. Setup paths
    may contain spaces, but command/expansion metacharacters are rejected.
    """
    if os.name != 'nt':
        raise OSError('quality_rasterizer_windows_batch_required')
    unsafe = '"%!?&|<>^()\r\n\x00'
    raw = os.fspath(setup_path)
    if not isinstance(raw, str) or any(character in raw for character in unsafe):
        raise ValueError('quality_rasterizer_msvc_setup_unsafe_path')
    setup = d_path(raw)
    if any(character in str(setup) for character in unsafe) or setup.suffix.lower() not in ('.bat', '.cmd'):
        raise ValueError('quality_rasterizer_msvc_setup_unsafe_path')
    if not setup.is_file():
        raise ValueError('quality_rasterizer_msvc_setup_missing')
    environment = dict(os.environ if inherited is None else inherited)
    command = Path(environment.get('COMSPEC', r'C:\Windows\System32\cmd.exe')).resolve()
    if (not command.is_file() or command.name.lower() != 'cmd.exe' or
            any(character in str(command) for character in unsafe)):
        raise ValueError('quality_rasterizer_cmd_path_invalid')
    command_line = '"{}" /d /v:off /u /s /c "call "{}" >nul && set"'.format(command, setup)
    result = subprocess.run(command_line, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=environment, cwd=str(setup.parent),
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode != 0:
        # External batch diagnostics need not use cmd's Unicode mode; never
        # decode unknown byte output as cp932 and present corrupted text.
        raise RuntimeError('quality_rasterizer_msvc_setup_failed: exit={}'.format(result.returncode))
    try:
        output = result.stdout.decode('utf-16-le').lstrip('\ufeff')
    except UnicodeDecodeError as error:
        raise RuntimeError('quality_rasterizer_msvc_environment_encoding') from error
    selected = {}
    for line in output.splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            selected[key.upper()] = value
    if not selected:
        raise RuntimeError('quality_rasterizer_msvc_environment_empty')
    return selected


def glm_source_files(root):
    """Actual header include tree and its license, not GLM docs/test programs.

    Native packages intentionally omit HTML/JS documentation. Binding those
    unrelated files made an otherwise identical installed kernel unverifiable.
    Keep all transitive GLM headers under glm/ rather than guessing includes.
    """
    root = Path(root)
    headers = [p for p in (root / 'glm').rglob('*')
               if p.is_file() and p.suffix.lower() in ('.h', '.hpp', '.inl')
               and not any(part.startswith('.') for part in p.relative_to(root).parts)]
    return sorted(headers + [root / 'copying.txt'])


def source_files(repository):
    own = repository / 'training_kit' / 'quality_rasterizer'
    files = [p for p in (own / 'src').rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    files += [repository / 'native/worker/quality_rasterizer_runtime.py',
              repository / 'scripts/verify_quality_rasterizer.py',
              repository / 'scripts/verify_pinhole_rasterizer_gpu.py',
              repository / 'scripts/build_quality_rasterizer.ps1', own / 'build.py',
              own / 'reference.py', own / 'README.md', own / 'PROVENANCE.json']
    glm = repository / 'gaussian-splatting/submodules/diff-gaussian-rasterization/third_party/glm'
    files += glm_source_files(glm)
    return {str(p.relative_to(repository)).replace('\\', '/'): sha256(p, True) for p in sorted(files)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', required=True)
    parser.add_argument('--compile', action='store_true')
    parser.add_argument('--cuda-root', default='D:/Apps/CUDA/11.8')
    parser.add_argument('--cuda-arch', default='8.9')
    parser.add_argument('--msvc-setup', default='D:/Apps/PortableMSVC-2022/msvc/setup_x64.bat')
    args = parser.parse_args(argv)
    repository = Path(__file__).resolve().parents[2]
    output = d_path(args.output_root)
    if output.exists():
        raise ValueError('quality_rasterizer_output_exists')
    plan = {'schema': 'gsw.quality_rasterizer_build_plan/1', 'compile_requested': args.compile,
            'output': str(output), 'python_executable': sys.executable, 'quality_abi': 1,
            'cuda_arch': args.cuda_arch, 'gpu_verified': False,
            'source_files': source_files(repository)}
    if not args.compile:
        print(json.dumps(plan, sort_keys=True))
        return 0
    environment = dict(os.environ)
    if os.name == 'nt':
        environment = read_windows_batch_environment(args.msvc_setup, environment)
        # The selected setup has already activated the verified compiler and
        # matching headers. Torch 2.0 distutils must not activate another SDK.
        environment['DISTUTILS_USE_SDK'] = '1'
    cuda = d_path(args.cuda_root)
    output.mkdir(parents=True)
    environment.update(CUDA_HOME=str(cuda), CUDA_PATH=str(cuda), TORCH_CUDA_ARCH_LIST=args.cuda_arch,
                       MAX_JOBS='2', TEMP=str(output / 'temp'), TMP=str(output / 'temp'),
                       TORCH_EXTENSIONS_DIR=str(output / 'torch-extensions'),
                       PIP_CACHE_DIR=str(output / 'pip-cache'))
    environment['PATH'] = str(cuda / 'bin') + os.pathsep + environment.get('PATH', '')
    Path(environment['TEMP']).mkdir()
    stage = output / 'source'
    shutil.copytree(repository / 'training_kit/quality_rasterizer/src', stage,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    glm = repository / 'gaussian-splatting/submodules/diff-gaussian-rasterization/third_party/glm'
    shutil.copytree(glm, stage / 'third_party/glm', ignore=shutil.ignore_patterns('.git'))
    environment['GSW_QUALITY_GLM_ROOT'] = str(stage / 'third_party/glm')
    runtime = output / 'runtime'
    subprocess.run([sys.executable, 'setup.py', 'build_ext', '--build-lib', str(runtime),
                    '--build-temp', str(output / 'build')], cwd=str(stage), env=environment, check=True)
    package = runtime / 'diff_gaussian_rasterization_quality'
    shutil.copy2(stage / 'diff_gaussian_rasterization_quality/__init__.py', package / '__init__.py')
    shutil.copy2(stage / 'LICENSE.md', runtime / 'LICENSE-Graphdeco.md')
    shutil.copy2(stage / 'third_party/glm/copying.txt', runtime / 'LICENSE-GLM.txt')
    identity = subprocess.check_output([sys.executable, '-c',
        'import json,torch;print(json.dumps(dict(torch=torch.__version__,cuda=torch.version.cuda)))'],
        env=environment, text=True)
    identity = json.loads(identity)
    binaries = list(package.glob('_C*.pyd')) + list(package.glob('_C*.so'))
    if len(binaries) != 1:
        raise RuntimeError('quality_rasterizer_binary_missing_or_ambiguous')
    proof = dict(schema='gsw.quality_rasterizer_build/1', quality_abi=1,
                 pinhole_covariance_version=1, python='{}.{}'.format(*sys.version_info[:2]),
                 torch=identity['torch'], cuda=identity['cuda'], cuda_arch=args.cuda_arch,
                 built_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 source_hash_mode='text-lf-v1', source_files=plan['source_files'],
                 runtime_files={str(p.relative_to(runtime)).replace('\\', '/'): sha256(p)
                                for p in [package / '__init__.py', binaries[0], runtime / 'LICENSE-Graphdeco.md',
                                          runtime / 'LICENSE-GLM.txt']},
                 gpu_verified=False)
    (runtime / 'quality-rasterizer-build.json').write_text(json.dumps(proof, indent=2, sort_keys=True), encoding='utf-8')
    print(json.dumps({'runtime_root': str(runtime), 'gpu_verified': False}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
