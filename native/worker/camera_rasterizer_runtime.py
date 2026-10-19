"""Select the independently built known-camera rasterizer, never replace Torch.

Runtime hashes provide local integrity, not publisher/signature attestation.
The legacy solve path keeps its configured upstream runtime unchanged.
"""
import hashlib
import json
import os
from pathlib import Path
import sys


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def select_camera_rasterizer(repository_root, runtime_root):
    repository_root = Path(repository_root).resolve()
    runtime_root = Path(runtime_root).resolve()
    proof_path = runtime_root / 'camera-rasterizer-proof.json'
    if not proof_path.is_file():
        raise ValueError('camera_pinhole_rasterizer_runtime_missing: ' + str(proof_path))
    with proof_path.open(encoding='utf-8') as stream:
        proof = json.load(stream)
    if proof.get('schema') != 'gsw.camera_rasterizer_runtime/1' or proof.get('pinhole_covariance_version') != 1:
        raise ValueError('camera_pinhole_rasterizer_proof_invalid')
    expected_python = '{}.{}'.format(sys.version_info.major, sys.version_info.minor)
    if proof.get('python') != expected_python:
        raise ValueError('camera_pinhole_rasterizer_python_mismatch')
    for key, base in (('runtime_files', runtime_root), ('source_files', repository_root)):
        entries = proof.get(key)
        if not isinstance(entries, dict) or not entries:
            raise ValueError('camera_pinhole_rasterizer_proof_invalid')
        for relative, expected in entries.items():
            path = base / relative
            if Path(relative).is_absolute() or '..' in Path(relative).parts:
                raise ValueError('camera_pinhole_rasterizer_unsafe_path')
            resolved = path.resolve()
            resolved.relative_to(base)
            if not resolved.is_file():
                raise ValueError('camera_pinhole_rasterizer_integrity_mismatch: ' + relative)
            if key == 'source_files' and proof.get('source_hash_mode') == 'text-lf-v1':
                actual = hashlib.sha256(resolved.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
            else:
                actual = file_sha256(resolved)
            if actual != expected:
                raise ValueError('camera_pinhole_rasterizer_integrity_mismatch: ' + relative)
    return {'root': str(runtime_root), 'proof': proof, 'proofSha256': file_sha256(proof_path)}


def configure_camera_rasterizer(repository_root, runtime_root=None):
    if os.environ.get('GSW_POSE_POLICY') != 'locked' or not os.environ.get('GSW_CAMERA_DATASET'):
        return None
    if runtime_root is None:
        runtime_root = os.environ.get('GSW_CAMERA_RASTERIZER_ROOT')
    if not runtime_root:
        prefix = Path(sys.executable).resolve().parent
        if prefix.name.lower() == 'scripts':
            prefix = prefix.parent
        runtime_root = prefix.parent.parent / 'kernels' / 'camera-pinhole3d-v1'
    selected = select_camera_rasterizer(repository_root, runtime_root)
    if 'diff_gaussian_rasterization' in sys.modules:
        raise ValueError('camera_pinhole_rasterizer_loaded_too_late')
    sys.path.insert(0, selected['root'])
    import torch
    import diff_gaussian_rasterization
    if (getattr(diff_gaussian_rasterization._C, 'GSW_PINHOLE_COVARIANCE_VERSION', 0) != 1 or
            selected['proof'].get('torch') != torch.__version__ or
            selected['proof'].get('cuda') != torch.version.cuda):
        raise ValueError('camera_pinhole_rasterizer_abi_mismatch')
    os.environ['GSW_CAMERA_RASTERIZER_PROOF_SHA256'] = selected['proofSha256']
    return selected
