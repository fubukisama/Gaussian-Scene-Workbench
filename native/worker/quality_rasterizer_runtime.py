"""Fail-closed selection of the independent quality kernel, never stock override.

Local hashes establish integrity, not publisher attestation. CPU checks or a
successful build/import never constitute actual GPU numerical acceptance.
"""
import hashlib
import json
from pathlib import Path
import sys

REQUIRED_GPU_CHECKS = (
    'flags_off_outputs', 'flags_off_signed_gradients', 'full_pinhole_pixel_border',
    'absolute_precancellation', 'contributor_pixels', 'alpha_gradients',
    'normalized_inverse_depth_gradients',
)


def file_sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _check_files(entries, root, text_lf=False):
    if not isinstance(entries, dict) or not entries:
        raise ValueError('quality_rasterizer_build_invalid')
    for relative, expected in entries.items():
        path = Path(relative)
        if path.is_absolute() or '..' in path.parts or ':' in relative or '\\' in relative:
            raise ValueError('quality_rasterizer_unsafe_path')
        resolved = (root / path).resolve()
        try:
            resolved.relative_to(root)
        except ValueError:
            raise ValueError('quality_rasterizer_unsafe_path')
        if not resolved.is_file():
            raise ValueError('quality_rasterizer_integrity_mismatch: ' + relative)
        content = resolved.read_bytes()
        if text_lf:
            content = content.replace(b'\r\n', b'\n')
        if hashlib.sha256(content).hexdigest() != expected:
            raise ValueError('quality_rasterizer_integrity_mismatch: ' + relative)


def inspect_quality_rasterizer_build(repository_root, runtime_root):
    """Validate only build integrity; this does not return any capability grant."""
    repository_root, runtime_root = Path(repository_root).resolve(), Path(runtime_root).resolve()
    build_path = runtime_root / 'quality-rasterizer-build.json'
    if not build_path.is_file():
        raise ValueError('quality_rasterizer_build_missing')
    build = json.loads(build_path.read_text(encoding='utf-8'))
    if (build.get('schema') != 'gsw.quality_rasterizer_build/1' or build.get('quality_abi') != 1 or
            build.get('pinhole_covariance_version') != 1 or build.get('source_hash_mode') != 'text-lf-v1'):
        raise ValueError('quality_rasterizer_build_invalid')
    if build.get('python') != '{}.{}'.format(*sys.version_info[:2]):
        raise ValueError('quality_rasterizer_python_mismatch')
    _check_files(build.get('source_files'), repository_root, text_lf=True)
    _check_files(build.get('runtime_files'), runtime_root)
    return {'root': str(runtime_root), 'build': build, 'buildSha256': file_sha256(build_path)}


def select_quality_rasterizer(repository_root, runtime_root):
    inspected = inspect_quality_rasterizer_build(repository_root, runtime_root)
    runtime_root = Path(inspected['root'])
    build = inspected['build']
    build_path = runtime_root / 'quality-rasterizer-build.json'
    acceptance_path = runtime_root / 'quality-rasterizer-acceptance.json'
    if not acceptance_path.is_file():
        raise ValueError('quality_rasterizer_gpu_acceptance_missing')
    acceptance = json.loads(acceptance_path.read_text(encoding='utf-8'))
    checks = acceptance.get('checks', {})
    if (acceptance.get('schema') != 'gsw.quality_rasterizer_acceptance/1' or
            acceptance.get('gpu_executed') is not True or acceptance.get('accepted') is not True or
            acceptance.get('build_manifest_sha256') != file_sha256(build_path) or
            not isinstance(checks, dict) or any(checks.get(key) is not True for key in REQUIRED_GPU_CHECKS)):
        raise ValueError('quality_rasterizer_gpu_acceptance_invalid')
    return {'root': str(runtime_root), 'build': build, 'acceptance': acceptance,
            'buildSha256': file_sha256(build_path), 'acceptanceSha256': file_sha256(acceptance_path),
            'verified_capabilities': ['absolute_screen_gradient_ndc', 'accepted_contributor_pixels',
                                      'alpha_smooth_gradient', 'inverse_depth_moment']}


def configure_quality_rasterizer(repository_root, runtime_root):
    """Explicit opt-in only; never changes the stock package import identity."""
    selected = select_quality_rasterizer(repository_root, runtime_root)
    import torch
    if selected['build'].get('torch') != torch.__version__ or selected['build'].get('cuda') != torch.version.cuda:
        raise ValueError('quality_rasterizer_torch_cuda_mismatch')
    sys.path.insert(0, selected['root'])
    import diff_gaussian_rasterization_quality as quality
    try:
        Path(quality.__file__).resolve().relative_to(Path(selected['root']))
        Path(quality._C.__file__).resolve().relative_to(Path(selected['root']))
    except ValueError:
        raise ValueError('quality_rasterizer_loaded_from_wrong_root')
    if (getattr(quality._C, 'GSW_QUALITY_RASTERIZER_ABI', 0) != 1 or
            getattr(quality._C, 'GSW_PINHOLE_COVARIANCE_VERSION', 0) != 1):
        raise ValueError('quality_rasterizer_abi_mismatch')
    return selected
