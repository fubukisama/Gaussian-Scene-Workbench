#!/usr/bin/env python3
"""Stage original notices for exact deployed PE bytes, without loading DLLs.

Only installed Conda metadata/cache are read. Missing/ambiguous ownership,
changed files, absent original notices and unsafe paths block publication.
This is a provenance/material collector, not a blanket legal compatibility opinion.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import sys
import tempfile


class LicenseStagingError(ValueError):
    def __init__(self, code, report=None):
        super().__init__(code)
        self.code, self.report = code, report


def _sha(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def _safe(path):
    path = Path(path).absolute()
    for parent in (path, *path.parents):
        if parent.exists():
            info = parent.lstat()
            if parent.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
                raise LicenseStagingError('runtime_license_unsafe_path:' + str(parent))
    return path


def _relative(value):
    if not isinstance(value, str) or '\\' in value or ':' in value:
        raise LicenseStagingError('runtime_license_unsafe_relative_path')
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or any(part in ('.', '..') for part in path.parts):
        raise LicenseStagingError('runtime_license_unsafe_relative_path')
    return path


def _json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise LicenseStagingError('runtime_license_duplicate_json_key')
            result[key] = value
        return result
    return json.loads(_safe(path).read_text(encoding='utf-8-sig'), object_pairs_hook=pairs)


def read_pe_imports(path):
    """Return ordinary/delay imports from a file; never LoadLibrary or execute it."""
    data = _safe(path).read_bytes()
    def need(offset, count):
        if offset < 0 or count < 0 or offset + count > len(data):
            raise LicenseStagingError('runtime_license_invalid_pe:' + str(path))
    def unpack(fmt, offset):
        need(offset, struct.calcsize(fmt))
        return struct.unpack_from(fmt, data, offset)
    need(0, 64)
    if data[:2] != b'MZ':
        raise LicenseStagingError('runtime_license_invalid_pe:' + str(path))
    pe = unpack('<I', 60)[0]
    need(pe, 24)
    if data[pe:pe + 4] != b'PE\0\0':
        raise LicenseStagingError('runtime_license_invalid_pe:' + str(path))
    sections, optional_size = unpack('<H', pe + 6)[0], unpack('<H', pe + 20)[0]
    optional = pe + 24
    need(optional, optional_size)
    magic = unpack('<H', optional)[0]
    if magic == 0x20b:
        directories, number_offset, image_base = 112, 108, unpack('<Q', optional + 24)[0]
    elif magic == 0x10b:
        directories, number_offset, image_base = 96, 92, unpack('<I', optional + 28)[0]
    else:
        raise LicenseStagingError('runtime_license_invalid_pe:' + str(path))
    count = unpack('<I', optional + number_offset)[0]
    if not 0 < sections <= 96 or count > 16 or directories + count * 8 > optional_size:
        raise LicenseStagingError('runtime_license_invalid_pe:' + str(path))
    headers = unpack('<I', optional + 60)[0]
    regions = []
    for index in range(sections):
        offset = optional + optional_size + index * 40
        need(offset, 40)
        virtual_size, rva, raw_size, raw_offset = unpack('<IIII', offset + 8)
        need(raw_offset, raw_size)
        regions.append((rva, max(virtual_size, raw_size), raw_offset, raw_size))
    def location(rva, length):
        if 0 <= rva < headers:
            need(rva, length)
            return rva
        matches = [row for row in regions if row[0] <= rva < row[0] + row[1]]
        if len(matches) != 1:
            raise LicenseStagingError('runtime_license_invalid_pe_rva:' + str(path))
        start, unused, raw, size = matches[0]
        if rva - start + length > size:
            raise LicenseStagingError('runtime_license_invalid_pe_rva:' + str(path))
        return raw + rva - start
    def name(rva):
        offset = location(rva, 1)
        end = data.find(b'\0', offset, min(len(data), offset + 261))
        if end == -1:
            raise LicenseStagingError('runtime_license_invalid_pe_import')
        location(rva, end - offset + 1)
        value = data[offset:end].decode('ascii')
        if not re.fullmatch(r'[A-Za-z0-9_.+\-]+\.dll', value, re.IGNORECASE):
            raise LicenseStagingError('runtime_license_invalid_pe_import')
        return value
    result = {'imports': [], 'delay_imports': []}
    for directory, width, label in ((1, 20, 'imports'), (13, 32, 'delay_imports')):
        if count <= directory:
            continue
        rva, size = unpack('<II', optional + directories + directory * 8)
        if rva == size == 0:
            continue
        if not rva or size < width or size > 1024 * 1024:
            raise LicenseStagingError('runtime_license_invalid_pe_import_directory')
        terminated = False
        for offset in range(0, size - width + 1, width):
            row = unpack('<' + 'I' * (width // 4), location(rva + offset, width))
            if not any(row):
                terminated = True
                break
            if directory == 1:
                name_rva = row[3]
            else:
                if row[0] not in (0, 1):
                    raise LicenseStagingError('runtime_license_invalid_pe_delay_attributes')
                name_rva = row[1] if row[0] == 1 else row[1] - image_base
            result[label].append(name(name_rva))
        if not terminated:
            raise LicenseStagingError('runtime_license_unterminated_pe_imports')
        result[label] = sorted(set(result[label]), key=str.lower)
    return result


def _recipe_identity(cache):
    path = cache / 'info/recipe/meta.yaml'
    if not path.is_file():
        return {}, []
    raw = _safe(path).read_text(encoding='utf-8')
    # Read only literal generated YAML scalar fields; never execute/evaluate YAML.
    source = re.search(r'(?ms)^source:\s*\n(.*?)(?=^[A-Za-z_][A-Za-z0-9_]*:|\Z)', raw)
    block = source.group(1) if source else ''
    urls = re.findall(r'(?m)^\s*(?:-\s*)?url:\s*[\"\']?(https?://[^\s\"\']+)', block)
    hashes = re.findall(r'(?m)^\s*sha256:\s*[\"\']?([a-fA-F0-9]{64})', block)
    feedstock = re.search(r'(?m)^\s+remote_url:\s*(https://github\.com/conda-forge/[^\s]+)', raw)
    commit = re.search(r'(?m)^\s+sha:\s*([a-fA-F0-9]{40})\s*$', raw)
    return dict(source_urls=urls, source_sha256=hashes,
                feedstock=feedstock.group(1) if feedstock else None,
                feedstock_commit=commit.group(1) if commit else None), [path]


def _cache(record):
    value = record.get('extracted_package_dir') or record.get('link', {}).get('source')
    if not isinstance(value, str) or not Path(value).is_absolute():
        raise LicenseStagingError('runtime_license_cache_missing:' + record['name'])
    return _safe(value)


def _license_material(record):
    cache = _cache(record)
    about_path = cache / 'info/about.json'
    about = _json(about_path) if about_path.is_file() else {}
    root = cache / 'info/licenses'
    files = sorted(path for path in root.rglob('*') if path.is_file()) if root.is_dir() else []
    declared = about.get('license_file', [])
    declared = [declared] if isinstance(declared, str) else declared
    if not isinstance(declared, list) or any(not isinstance(item, str) for item in declared):
        raise LicenseStagingError('runtime_license_invalid_license_declaration')
    # A different incidental notice cannot hide a missing declared license.
    for item in declared:
        target = root / _relative(item.replace('\\', '/'))
        if not _safe(target).is_file() or target.stat().st_size == 0:
            return cache, about_path, about, []
    if any(_safe(path).stat().st_size == 0 for path in files):
        return cache, about_path, about, []
    return cache, about_path, about, files


def _known_bindings(value):
    if value is None:
        return {}
    if (type(value) is not dict or set(value) != {'schema', 'owners'}
            or value['schema'] != 'gsw.runtime_license_known_owners/1' or type(value['owners']) is not list):
        raise LicenseStagingError('runtime_license_invalid_known_owner_evidence')
    result = {}
    for owner in value['owners']:
        if (type(owner) is not dict or set(owner) != {'binary', 'sha256', 'owner', 'version',
                'license_expression', 'source_urls', 'license_files'}
                or not all(type(owner[key]) is str and owner[key] for key in ('owner', 'version', 'license_expression'))
                or not re.fullmatch('[a-f0-9]{64}', owner.get('sha256', ''))
                or type(owner['source_urls']) is not list or not owner['source_urls']
                or any(type(url) is not str or not url.startswith('https://') for url in owner['source_urls'])
                or type(owner['license_files']) is not list or not owner['license_files']):
            raise LicenseStagingError('runtime_license_invalid_known_owner_evidence')
        key = _relative(owner['binary']).as_posix().lower()
        if key in result:
            raise LicenseStagingError('runtime_license_duplicate_known_owner')
        for file in owner['license_files']:
            if (type(file) is not dict or set(file) != {'source', 'name', 'sha256'}
                    or not Path(file['source']).is_absolute()
                    or not re.fullmatch('[a-f0-9]{64}', file.get('sha256', ''))):
                raise LicenseStagingError('runtime_license_invalid_known_owner_evidence')
            _relative(file['name'])
        result[key] = owner
    return result


def validate_material_paths(entries):
    """Validate compact manifest bindings, rejecting truncated-address collisions.

This pure manifest seam permits callers/tests to reject a collision using
declared full hashes without needing to manufacture a SHA-256 preimage.
"""
    addresses = {}
    for entry in entries:
        digest = entry.get('sha256')
        if type(digest) is not str or not re.fullmatch('[a-f0-9]{64}', digest):
            raise LicenseStagingError('runtime_license_invalid_material_hash')
        expected = 'm/' + digest[:32]
        if entry.get('path') != expected:
            raise LicenseStagingError('runtime_license_material_address_invalid')
        previous = addresses.setdefault(expected, digest)
        if previous != digest:
            raise LicenseStagingError('runtime_license_material_address_collision')


def inspect_dependency_licenses(qt_root, package_bin, known_owners=None):
    """Inspect exact PE ownership/notices. The returned report contains no DLL loads."""
    qt_root, package_bin = _safe(qt_root), _safe(package_bin)
    if not (qt_root / 'conda-meta').is_dir():
        raise LicenseStagingError('runtime_license_conda_environment_required')
    known = _known_bindings(known_owners)
    metadata = []
    owners = {}
    for path in sorted((qt_root / 'conda-meta').glob('*.json')):
        record = _json(path)
        if not all(isinstance(record.get(key), str) and record[key] for key in ('name', 'version', 'build')):
            raise LicenseStagingError('runtime_license_invalid_conda_metadata')
        record['_metadata_path'] = path
        metadata.append(record)
        paths = {row['_path']: row for row in record.get('paths_data', {}).get('paths', [])}
        for relative in record.get('files', []):
            safe = _relative(relative)
            if safe.suffix.lower() in ('.dll', '.exe'):
                owners.setdefault(safe.name.lower(), []).append((record, safe, paths.get(relative, {})))
    binaries, selected, blockers, explicit = [], {}, [], {}
    paths = sorted(path for path in package_bin.rglob('*') if path.is_file() and path.suffix.lower() in ('.dll', '.exe'))
    if not paths:
        raise LicenseStagingError('runtime_license_no_pe_files')
    for binary in paths:
        relative = binary.relative_to(package_bin).as_posix()
        actual = _sha(_safe(binary))
        imports = read_pe_imports(binary)
        matches = {}
        for record, source_relative, info in owners.get(binary.name.lower(), []):
            source = _safe(qt_root / source_relative)
            expected = info.get('sha256_in_prefix', info.get('sha256'))
            if source.is_file() and _sha(source) == actual and (expected is None or expected == actual):
                matches[record['name']] = (record, source_relative)
        row = dict(path=relative, sha256=actual, bytes=binary.stat().st_size, **imports)
        supplied = known.get(relative.lower())
        if len(matches) == 0 and supplied is not None:
            if supplied['sha256'] != actual:
                row.update(status='blocked', code='runtime_license_known_owner_hash_mismatch')
                blockers.append(dict(code=row['code'], binary=relative, sha256=actual))
            else:
                key = 'explicit-' + hashlib.sha256(json.dumps(supplied, sort_keys=True).encode()).hexdigest()[:24]
                row.update(status='bound', owner_kind='explicit_exact_hash', package=key)
                explicit[key] = supplied
        elif len(matches) != 1:
            code = 'runtime_license_owner_missing' if not matches else 'runtime_license_owner_ambiguous'
            row.update(status='blocked', code=code)
            blockers.append(dict(code=code, binary=relative, sha256=actual))
        else:
            record, source_relative = next(iter(matches.values()))
            key = record['name'] + '-' + record['version'] + '-' + record['build']
            row.update(status='bound', owner_kind='conda_exact_hash', package=key, installed_path=source_relative.as_posix())
            selected[key] = record
        binaries.append(row)
    packages, copies = [], []
    for key, owner in sorted(explicit.items()):
        package = dict(id=key, name=owner['owner'], version=owner['version'], build=None,
            owner_kind='explicit_exact_hash', license_expression=owner['license_expression'],
            source_urls=owner['source_urls'], license_files=[],
            evidence_sha256=hashlib.sha256(json.dumps(owner, sort_keys=True).encode()).hexdigest())
        for file in owner['license_files']:
            source = _safe(file['source'])
            if not source.is_file() or source.stat().st_size == 0 or _sha(source) != file['sha256']:
                blockers.append(dict(code='runtime_license_known_owner_notice_changed', package=key))
                continue
            destination = key + '/licenses/' + _relative(file['name']).as_posix()
            entry = dict(path=destination, sha256=file['sha256'], bytes=source.stat().st_size,
                         source_path=str(source))
            package['license_files'].append(entry)
            copies.append((source, destination, entry['sha256']))
        packages.append(package)
    for key, record in sorted(selected.items()):
        cache, about_path, about, licenses = _license_material(record)
        package = dict(id=key, name=record['name'], version=record['version'], build=record['build'],
            license_expression=record.get('license'), package_url=record.get('url'),
            package_sha256=record.get('sha256'), metadata_sha256=_sha(record['_metadata_path']),
            license_files=[], source_metadata=[])
        package['upstream'] = {name: about[name] for name in ('home', 'dev_url', 'doc_url') if name in about}
        identity, recipe_paths = _recipe_identity(cache)
        package['recipe_identity'] = identity
        license_cache = cache
        license_metadata = []
        if not licenses and identity.get('source_sha256') and identity.get('feedstock') and identity.get('feedstock_commit'):
            for candidate in metadata:
                if (candidate['name'] == record['name'] or candidate['version'] != record['version']
                        or candidate.get('license') != record.get('license')):
                    continue
                other_cache, other_about_path, other_about, other_files = _license_material(candidate)
                other_identity, other_recipe = _recipe_identity(other_cache)
                if (other_files and all(other_identity.get(field) == identity.get(field)
                        for field in ('source_urls', 'source_sha256', 'feedstock', 'feedstock_commit'))):
                    licenses, license_cache = other_files, other_cache
                    package['license_source_package'] = dict(name=candidate['name'], version=candidate['version'],
                        build=candidate['build'], metadata_sha256=_sha(candidate['_metadata_path']),
                        binding='exact_source_URL_SHA256_feedstock_commit_version_license_expression',
                        recipe_identity=other_identity)
                    license_metadata = [candidate['_metadata_path'], other_about_path] + other_recipe
                    package['upstream'] = {name: other_about[name] for name in ('home', 'dev_url', 'doc_url') if name in other_about}
                    break
        license_root = license_cache / 'info/licenses'
        if not licenses:
            blockers.append(dict(code='runtime_license_original_notice_missing', package=key))
        for source in licenses:
            source = _safe(source)
            relative = source.relative_to(license_root).as_posix()
            destination = key + '/licenses/' + relative
            entry = dict(path=destination, sha256=_sha(source), bytes=source.stat().st_size,
                         source_path=str(source))
            package['license_files'].append(entry)
            copies.append((source, destination, entry['sha256']))
        if 'LGPL-3.0' in (record.get('license') or ''):
            # LGPLv3 incorporates GPLv3. This is the repository's preserved
            # upstream GPL text, not invented Qt license/attribution material.
            source = _safe(Path(__file__).resolve().parents[1] / 'COPYING')
            expected = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
            if not source.is_file() or _sha(source) != expected:
                blockers.append(dict(code='runtime_license_verified_GPL3_text_missing', package=key))
            else:
                destination = key + '/licenses/supplemental/GPL-3.0.txt'
                entry = dict(path=destination, sha256=expected, bytes=source.stat().st_size,
                    source_path=str(source), source_url='https://www.gnu.org/licenses/gpl-3.0.txt',
                    role='supplemental_GPL3_text_for_LGPL3')
                package['license_files'].append(entry)
                copies.append((source, destination, expected))
        for source in [record['_metadata_path']] + ([about_path] if about_path.is_file() else []) + recipe_paths:
            relative = key + '/metadata/' + ('conda-meta.json' if source == record['_metadata_path'] else source.name)
            entry = dict(path=relative, sha256=_sha(source), source_path=str(source))
            package['source_metadata'].append(entry)
            copies.append((source, relative, entry['sha256']))
        for source in license_metadata:
            relative = key + '/metadata/license-source-' + source.name
            entry = dict(path=relative, sha256=_sha(source), source_path=str(source))
            package['source_metadata'].append(entry)
            copies.append((source, relative, entry['sha256']))
        packages.append(package)
    report = dict(schema='gsw.runtime_dependency_licenses/1',
        status='blocked' if blockers else 'complete',
        scope='Conda-based native distributions only. Exact deployed PE bytes and original notices; not an official Qt SDK solution, legal compatibility opinion or proof all system DLLs are bundled.',
        qt_root=str(qt_root), package_bin=str(package_bin), binaries=binaries, packages=packages,
        blockers=blockers, binary_count=len(binaries), package_count=len(packages),
        copies=[dict(source=str(src), path=dst, sha256=digest) for src, dst, digest in copies])
    # Material names must not inherit long package/build/source names. Their
    # original names remain explicit provenance, not filesystem path segments.
    for package in report['packages']:
        for entry in package['license_files'] + package.get('source_metadata', []):
            entry['original_relative_path'] = entry['path']
            entry['source_filename'] = Path(entry['source_path']).name
            entry['path'] = 'm/' + entry['sha256'][:32]
    for entry in report['copies']:
        entry['path'] = 'm/' + entry['sha256'][:32]
    validate_material_paths(report['copies'])
    report['material_path_scheme'] = 'm/SHA256_first_128_bits_with_full_SHA256_and_byte_collision_checks'
    return report


def stage_dependency_licenses(qt_root, package_bin, output_root, known_owners=None):
    """Publish a new notice directory only after the complete inspection passes."""
    output_root = _safe(output_root)
    for input_root in (_safe(qt_root), _safe(package_bin)):
        try:
            output_root.resolve().relative_to(input_root.resolve())
        except ValueError:
            continue
        raise LicenseStagingError('runtime_license_output_inside_input')
    if output_root.exists():
        raise LicenseStagingError('runtime_license_output_exists')
    if not output_root.parent.is_dir():
        raise LicenseStagingError('runtime_license_output_parent_missing')
    report = inspect_dependency_licenses(qt_root, package_bin, known_owners)
    if report['blockers']:
        raise LicenseStagingError('runtime_license_materials_incomplete', report)
    stage = Path(tempfile.mkdtemp(prefix='.' + output_root.name + '-', dir=str(output_root.parent)))
    try:
        for entry in report.pop('copies'):
            source = _safe(entry['source'])
            if _sha(source) != entry['sha256']:
                raise LicenseStagingError('runtime_license_source_changed')
            target = stage / _relative(entry['path'])
            target.parent.mkdir(parents=True, exist_ok=True)
            content = source.read_bytes()
            if target.exists():
                if _sha(target) != entry['sha256'] or target.read_bytes() != content:
                    raise LicenseStagingError('runtime_license_material_byte_collision')
            else:
                with target.open('xb') as stream:
                    stream.write(content)
            if _sha(target) != entry['sha256']:
                raise LicenseStagingError('runtime_license_copy_changed')
        for binary in report['binaries']:
            if _sha(_safe(Path(package_bin) / _relative(binary['path']))) != binary['sha256']:
                raise LicenseStagingError('runtime_license_deployed_binary_changed')
        with (stage / 'index.json').open('x', encoding='utf-8') as stream:
            json.dump(report, stream, sort_keys=True, indent=2, allow_nan=False)
        if output_root.exists():
            raise LicenseStagingError('runtime_license_output_exists')
        stage.rename(output_root)
    finally:
        if stage.exists():
            safe_stage = _safe(stage)
            if (safe_stage.resolve().parent != output_root.parent.resolve()
                    or not safe_stage.name.startswith('.' + output_root.name + '-')):
                raise LicenseStagingError('runtime_license_cleanup_target_changed')
            shutil.rmtree(safe_stage)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', required=True)
    parser.add_argument('--package-bin', required=True)
    parser.add_argument('--output-root')
    parser.add_argument('--inspect-only', action='store_true')
    parser.add_argument('--known-owners', help='Explicit source/notices manifest with exact deployed binary SHA256 bindings')
    args = parser.parse_args(argv)
    try:
        known = _json(args.known_owners) if args.known_owners else None
        if args.inspect_only:
            report = inspect_dependency_licenses(args.qt_root, args.package_bin, known)
        else:
            if not args.output_root:
                parser.error('--output-root is required unless --inspect-only')
            report = stage_dependency_licenses(args.qt_root, args.package_bin, args.output_root, known)
        print(json.dumps(report, sort_keys=True, indent=2, allow_nan=False))
        return 2 if report['blockers'] else 0
    except LicenseStagingError as error:
        print(json.dumps(error.report or dict(schema='gsw.runtime_dependency_licenses/1',
            status='blocked', blockers=[dict(code=error.code)]), sort_keys=True, indent=2))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
