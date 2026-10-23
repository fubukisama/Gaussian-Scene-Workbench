# SPDX-License-Identifier: MIT
"""Isolated rebuild of unchanged full-K stock kernel; never overwrite a runtime.

The proof binds the actual compiled kernel/build/verification input closure,
not unrelated UI/test files. It does not attest a training program, numeric GPU
acceptance or checkpoint compatibility. Preserve old runtime/source archives.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from training_kit.quality_rasterizer.build import d_path, glm_source_files, read_windows_batch_environment, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', required=True)
    parser.add_argument('--compile', action='store_true')
    args = parser.parse_args()
    output = d_path(args.output_root)
    if output.exists():
        raise ValueError('pinhole_build_destination_exists')
    source = ROOT / 'gaussian-splatting/submodules/diff-gaussian-rasterization'
    files = [p for p in source.rglob('*') if p.is_file() and not any(
        part in ('.git', '__pycache__', 'third_party') for part in p.relative_to(source).parts)]
    files += glm_source_files(source / 'third_party/glm')
    files += [Path(__file__).resolve(), Path(__file__).resolve().with_name('setup.py'),
              ROOT / 'training_kit/quality_rasterizer/build.py',
              ROOT / 'native/worker/camera_rasterizer_runtime.py', ROOT / 'scripts/verify_pinhole_rasterizer_gpu.py']
    hashes = {str(p.relative_to(ROOT)).replace('\\', '/'): sha256(p, True) for p in sorted(files)}
    if not args.compile:
        print(json.dumps(dict(output=str(output), source_files=hashes, gpu_verified=False)))
        return 0
    environment = read_windows_batch_environment('D:/Apps/PortableMSVC-2022/msvc/setup_x64.bat')
    output.mkdir(parents=True)
    for directory in ('temp', 'build', 'torch-extensions', 'pip-cache'):
        (output / directory).mkdir()
    environment.update(DISTUTILS_USE_SDK='1', CUDA_HOME='D:/Apps/CUDA/11.8', CUDA_PATH='D:/Apps/CUDA/11.8',
        TORCH_CUDA_ARCH_LIST='8.9', MAX_JOBS='2', TEMP=str(output / 'temp'), TMP=str(output / 'temp'),
        TORCH_EXTENSIONS_DIR=str(output / 'torch-extensions'), PIP_CACHE_DIR=str(output / 'pip-cache'))
    environment['PATH'] = 'D:/Apps/CUDA/11.8/bin' + os.pathsep + environment.get('PATH', '')
    stage, runtime = output / 'source', output / 'runtime'
    shutil.copytree(source, stage, ignore=shutil.ignore_patterns('.git', '__pycache__', '*.pyc'))
    subprocess.run([sys.executable, str(Path(__file__).resolve().with_name('setup.py')), 'build_ext',
                    '--build-lib', str(runtime), '--build-temp', str(output / 'build')],
                   cwd=str(stage), env=environment, check=True)
    package = runtime / 'diff_gaussian_rasterization'
    shutil.copy2(source / 'diff_gaussian_rasterization/__init__.py', package / '__init__.py')
    shutil.copy2(source / 'LICENSE.md', runtime / 'LICENSE-Graphdeco.md')
    import torch
    binaries = list(package.glob('_C*.pyd'))
    if len(binaries) != 1:
        raise ValueError('pinhole_binary_missing')
    proof = dict(schema='gsw.camera_rasterizer_runtime/1', pinhole_covariance_version=1,
        python='{}.{}'.format(*sys.version_info[:2]), torch=torch.__version__, cuda=torch.version.cuda,
        source_hash_mode='text-lf-v1', source_files=hashes,
        runtime_files={str(p.relative_to(runtime)).replace('\\', '/'): sha256(p)
                       for p in (package / '__init__.py', binaries[0], runtime / 'LICENSE-Graphdeco.md')},
        build_kind='compiled_from_unchanged_stock_kernel', proof_scope='compiled_kernel_input_closure',
        attests_training_program=False, gpu_verified=False,
        built_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (runtime / 'camera-rasterizer-proof.json').write_text(json.dumps(proof, indent=2, sort_keys=True), encoding='utf-8')
    print(json.dumps(dict(runtime=str(runtime), gpu_verified=False)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
