# SPDX-License-Identifier: MIT
"""Source-preserving quality export metadata and mandatory PLY markers."""
import os
import copy
import hashlib
import json
from pathlib import Path
import shutil


def _poll(cancelled):
    if cancelled and cancelled():
        raise ValueError('quality_mip_export_cancelled')


def _safe(path):
    for item in (path.absolute(),) + tuple(path.absolute().parents):
        if item.is_symlink() or item.exists() and getattr(item.stat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('quality_mip_export_link_refused')


def _header(path):
    result, size = [], 0
    with path.open('rb') as stream:
        while True:
            line = stream.readline(1024 * 1024 + 1)
            size += len(line)
            if not line or size > 1024 * 1024:
                raise ValueError('quality_export_header_invalid')
            result.append(line)
            if line.strip() == b'end_header':
                return result


def _json(path, limit=1024 * 1024):
    _safe(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError('quality_mip_export_metadata_invalid')
    def pairs(rows):
        value = {}
        for key, row in rows:
            if key in value:
                raise ValueError('quality_mip_export_metadata_invalid')
            value[key] = row
        return value
    with path.open(encoding='utf-8') as stream:
        value = json.load(stream, object_pairs_hook=pairs,
            parse_constant=lambda unused: (_ for _ in ()).throw(ValueError('quality_mip_export_metadata_invalid')))
    if type(value) is not dict:
        raise ValueError('quality_mip_export_metadata_invalid')
    return value


def _json_bytes(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')


def _write_json(path, value):
    with path.open('xb') as stream:
        stream.write(_json_bytes(value))
        stream.flush()
        os.fsync(stream.fileno())


def _copy(source, destination, cancelled):
    with source.open('rb') as incoming, destination.open('xb') as outgoing:
        while True:
            _poll(cancelled)
            block = incoming.read(1024 * 1024)
            if not block:
                break
            outgoing.write(block)
        outgoing.flush()
        os.fsync(outgoing.fileno())


def _close_maps(ply):
    seen = set()
    for element in ply.elements:
        value = element.data
        while value is not None and id(value) not in seen:
            seen.add(id(value))
            mapping = getattr(value, '_mmap', None)
            if mapping is not None:
                mapping.close()
            value = getattr(value, 'base', None)


def _merge_filtered_ply(raw, preview, target, count, cancelled):
    """Patch only four filter fields in bounded typed rows; preserve all others."""
    import numpy as np
    from plyfile import PlyData
    opened = []
    try:
        for path in (raw, preview):
            # Actual trainers write binary scalar Gaussian rows. Small ASCII
            # fixtures remain supported without unbounded text materialization.
            if any(b'format ascii' in line for line in _header(path)) and path.stat().st_size > 128 * 1024 * 1024:
                raise ValueError('quality_mip_export_ascii_budget_exceeded')
            opened.append(PlyData.read(path, mmap='r'))
        source, filtered = opened
        if (len(source.elements) != 1 or len(filtered.elements) != 1 or
                source.elements[0].name != 'vertex' or filtered.elements[0].name != 'vertex'):
            raise ValueError('quality_mip_export_ply_layout_unsupported')
        original, active = source['vertex'].data, filtered['vertex'].data
        fields = ('scale_0', 'scale_1', 'scale_2', 'opacity')
        if (len(original) != count or len(active) != count or original.dtype.hasobject or
                any(name not in original.dtype.names or name not in active.dtype.names or
                    original.dtype[name].kind != 'f' or active.dtype[name].kind != 'f' for name in fields)):
            raise ValueError('quality_mip_export_ply_layout_unsupported')
        header = _header(raw)
        header = [b'format binary_little_endian 1.0\n' if line.strip() == b'format ascii 1.0' else line
            for line in header if b'GSW_QUALITY_REQUIRES_SIDECAR' not in line]
        # A marker after properties becomes element.comments in plyfile. Keep
        # one global marker so ordinary readers/writers cannot duplicate it.
        first_element = next(i for i, line in enumerate(header) if line.strip().startswith(b'element '))
        header.insert(first_element, b'comment GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1\n')
        dtype = original.dtype.newbyteorder('<') if source.text else original.dtype
        with target.open('xb') as stream:
            stream.write(b''.join(header))
            for offset in range(0, count, 65536):
                _poll(cancelled)
                block = original[offset:offset + 65536].astype(dtype, copy=True)
                for name in fields:
                    values = active[name][offset:offset + len(block)]
                    if not np.isfinite(values).all():
                        raise ValueError('quality_mip_export_nonfinite_filtered_attributes')
                    block[name] = values
                    if not np.isfinite(block[name]).all():
                        raise ValueError('quality_mip_export_filtered_attribute_overflow')
                stream.write(block.tobytes())
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        for ply in opened:
            _close_maps(ply)


def mark_quality_ply(path, representation):
    if representation not in ('raw', 'fused'):
        raise ValueError('quality_export_representation_invalid')
    path = Path(path)
    _safe(path)
    marker = ('comment GSW_QUALITY_REQUIRES_SIDECAR full_mip_{}_v1\n'.format(representation)).encode('ascii')
    from native.backend.atomic_file import atomic_sibling
    header = _header(path)
    markers = [line for line in header if b'GSW_QUALITY_REQUIRES_SIDECAR' in line]
    if markers:
        if len(markers) != 1 or markers[0].strip() != marker.strip():
            raise ValueError('quality_export_representation_conflict')
        return
    with path.open('rb') as original:
        original.seek(sum(map(len, header)))
        with atomic_sibling(path, mode='wb') as (temporary, stream):
            stream.write(b''.join(header[:-1]))
            stream.write(marker)
            stream.write(header[-1])
            shutil.copyfileobj(original, stream, length=4 * 1024 * 1024)
            stream.flush()
            os.fsync(stream.fileno())
            stream.close()
            original.close()
            os.replace(temporary, path)


def export_mip_checkpoint(controller, iteration, *, cancelled=None):
    """Publish one complete immutable bundle, then rebind the mutable raw entry.

    A failed/cancelled stage is never visible as a complete bundle. Existing
    malformed/tampered bundles are retained and rejected, not overwritten or
    bypassed. The optional cancellation callback does not affect legacy callers.
    """
    import numpy as np
    from native.backend.atomic_file import atomic_sibling_directory
    from native.worker.training_checkpoint import atomic_json, file_digest
    if type(iteration) is not int or iteration < 1:
        raise ValueError('quality_mip_export_iteration_invalid')
    path = Path(controller.scene.model_path) / 'point_cloud' / ('iteration_{}'.format(iteration))
    raw = path / 'point_cloud.ply'
    _safe(raw)
    _poll(cancelled)
    if not raw.is_file():
        raise ValueError('quality_mip_export_source_or_destination_invalid')
    camera_path = raw.with_suffix('.camera-scale.json')
    camera = _json(camera_path, 128 * 1024 * 1024)
    entry_path = raw.with_suffix('.quality.json')
    _safe(entry_path)
    prior_entry = _json(entry_path) if entry_path.exists() else None
    entry_hash = file_digest(entry_path) if prior_entry is not None else None
    original_hash, original_camera_hash = file_digest(raw), file_digest(camera_path)
    # Scene.save writes an unmarked raw hash; an older interrupted/re-entered
    # export may have added only the mandatory raw marker since that save.
    header = _header(raw)
    unmarked = hashlib.sha256(b''.join(line for line in header if b'GSW_QUALITY_REQUIRES_SIDECAR' not in line))
    with raw.open('rb') as stream:
        stream.seek(sum(map(len, header)))
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            _poll(cancelled)
            unmarked.update(block)
    if (camera.get('schema') != 'gsw.training_camera_contract/1' or camera.get('backend') != '3dgs' or
            camera.get('ply_sha256') not in (original_hash, unmarked.hexdigest())):
        raise ValueError('quality_mip_export_camera_binding_invalid')
    camera_base = copy.deepcopy(camera)
    camera_base.pop('ply_sha256', None)
    mark_quality_ply(raw, 'raw')
    raw_hash = file_digest(raw)
    widths = controller.mip.widths.detach().cpu().numpy().copy()
    if widths.ndim != 2 or widths.shape[1] != 1 or not np.isfinite(widths).all() or np.any(widths <= 0):
        raise ValueError('quality_mip_export_widths_invalid')
    widths_hash = hashlib.sha256(widths.tobytes()).hexdigest()
    binding = dict(raw_source_sha256=raw_hash, filter_widths_values_sha256=widths_hash,
        plan_hash=controller.plan['plan_hash'], kernel_build_sha256=controller.kernel['buildSha256'],
        camera_contract_sha256=hashlib.sha256(_json_bytes(camera_base)).hexdigest())
    if any(not isinstance(binding[key], str) or len(binding[key]) != 64 or
           any(char not in '0123456789abcdef' for char in binding[key]) for key in binding):
        raise ValueError('quality_mip_export_identity_invalid')
    identity = hashlib.sha256(_json_bytes(binding)).hexdigest()
    bundle = path / ('mip-' + identity[:20])

    def unchanged():
        _poll(cancelled)
        _safe(raw)
        _safe(camera_path)
        _safe(entry_path)
        current_widths = controller.mip.widths.detach().cpu().numpy()
        if (file_digest(raw) != raw_hash or file_digest(camera_path) != original_camera_hash or
                current_widths.shape != widths.shape or current_widths.dtype != widths.dtype or
                hashlib.sha256(current_widths.tobytes()).hexdigest() != widths_hash or
                controller.plan['plan_hash'] != binding['plan_hash'] or
                controller.kernel['buildSha256'] != binding['kernel_build_sha256'] or
                (entry_path.exists() != (entry_hash is not None)) or
                entry_hash is not None and file_digest(entry_path) != entry_hash):
            raise ValueError('quality_mip_export_source_changed')

    def validate(directory):
        _safe(directory)
        record = _json(directory / 'bundle.json')
        files = {'model.ply', 'raw.ply', 'f.npy', 'model.quality.json', 'raw.quality.json',
                 'model.camera-scale.json', 'raw.camera-scale.json'}
        if (record.get('schema') != 'gsw.quality_mip_bundle/1' or record.get('identity') != identity or
                record.get('binding') != binding or type(record.get('files')) is not dict or
                set(record['files']) != files or {p.name for p in directory.iterdir()} != files | {'bundle.json'}):
            raise ValueError('quality_mip_export_existing_mismatch')
        for name in files:
            _poll(cancelled)
            target = directory / name
            _safe(target)
            if not target.is_file() or file_digest(target) != record['files'][name]:
                raise ValueError('quality_mip_export_existing_mismatch')
        if file_digest(directory / 'raw.ply') != raw_hash:
            raise ValueError('quality_mip_export_existing_mismatch')
        common = _json(directory / 'model.quality.json')
        expected = dict(schema='gsw.quality_model/1', filter='full_mip', representation='fused', filter_3d_applied=True,
            filter_2d=dict(variance=.3, opacity_compensation=True), plan_hash=binding['plan_hash'],
            kernel_build_sha256=binding['kernel_build_sha256'], raw_source_sha256=raw_hash,
            raw_source_file='raw.ply', fused_file='model.ply', fused_sha256=file_digest(directory / 'model.ply'),
            filter_widths=dict(path='f.npy', sha256=file_digest(directory / 'f.npy'), frame='training'), quality_accepted=False)
        if common != expected or _json(directory / 'raw.quality.json') != dict(expected, representation='raw', filter_3d_applied=False):
            raise ValueError('quality_mip_export_existing_mismatch')
        for name, representation in (('model.ply', 'fused'), ('raw.ply', 'raw')):
            marker = ('comment GSW_QUALITY_REQUIRES_SIDECAR full_mip_{}_v1'.format(representation)).encode('ascii')
            markers = [line.strip() for line in _header(directory / name) if b'GSW_QUALITY_REQUIRES_SIDECAR' in line]
            if markers != [marker] or _json((directory / name).with_suffix('.camera-scale.json'), 128 * 1024 * 1024) != dict(
                    camera_base, ply_sha256=file_digest(directory / name)):
                raise ValueError('quality_mip_export_existing_mismatch')
        if (directory / 'f.npy').stat().st_size > widths.nbytes + 4096:
            raise ValueError('quality_mip_export_existing_mismatch')
        loaded = np.load(str(directory / 'f.npy'), allow_pickle=False)
        if not np.array_equal(loaded, widths) or loaded.dtype != widths.dtype:
            raise ValueError('quality_mip_export_existing_mismatch')
        return common

    if bundle.exists() or bundle.is_symlink():
        common = validate(bundle)
        if (prior_entry is not None and prior_entry.get('fused_file') == bundle.name + '/model.ply' and
                prior_entry.get('raw_source_sha256') == raw_hash and
                prior_entry.get('fused_sha256') != common['fused_sha256']):
            raise ValueError('quality_mip_export_existing_mismatch')
    else:
        with atomic_sibling_directory(bundle) as stage:
            _copy(raw, stage / 'raw.ply', cancelled)
            _poll(cancelled)
            controller.preview_model().save_ply(str(stage / 'p.ply'))
            _merge_filtered_ply(stage / 'raw.ply', stage / 'p.ply', stage / 'model.ply', len(widths), cancelled)
            (stage / 'p.ply').unlink()
            with (stage / 'f.npy').open('xb') as stream:
                np.save(stream, widths, allow_pickle=False)
                stream.flush()
                os.fsync(stream.fileno())
            common = dict(schema='gsw.quality_model/1', filter='full_mip', plan_hash=binding['plan_hash'],
                raw_source_sha256=raw_hash, raw_source_file='raw.ply', fused_file='model.ply',
                fused_sha256=file_digest(stage / 'model.ply'), filter_2d=dict(variance=.3, opacity_compensation=True),
                filter_widths=dict(path='f.npy', sha256=file_digest(stage / 'f.npy'), frame='training'),
                kernel_build_sha256=binding['kernel_build_sha256'], quality_accepted=False)
            for name, representation in (('raw.ply', 'raw'), ('model.ply', 'fused')):
                target = stage / name
                _write_json(target.with_suffix('.quality.json'), dict(common, representation=representation,
                    filter_3d_applied=representation == 'fused'))
                _write_json(target.with_suffix('.camera-scale.json'), dict(camera_base, ply_sha256=file_digest(target)))
            _write_json(stage / 'bundle.json', dict(schema='gsw.quality_mip_bundle/1', identity=identity, binding=binding,
                files={p.name: file_digest(p) for p in stage.iterdir()}))
            common = validate(stage)
            unchanged()
            if bundle.exists() or bundle.is_symlink():
                raise ValueError('quality_mip_export_requires_fresh_bundle')
            os.rename(str(stage), str(bundle))
    unchanged()
    # A second Scene.save can rewrite this raw entry and its unmarked camera
    # hash. Rebind on EVERY successful call, including a fully validated reuse.
    entry = dict(common, representation='raw', filter_3d_applied=False,
        raw_source_file=bundle.name + '/raw.ply', fused_file=bundle.name + '/model.ply',
        filter_widths=dict(common['filter_widths'], path=bundle.name + '/f.npy'))
    atomic_json(raw.with_suffix('.quality.json'), entry)
    atomic_json(camera_path, dict(camera_base, ply_sha256=raw_hash))
    fused = bundle / 'model.ply'
    if controller.emit:
        controller.emit('[gsw-quality-export]', dict(common, raw=str(raw), fused=str(fused),
            native_viewer_acceptance='requires_matching_2d_filter'))
    return str(fused)
