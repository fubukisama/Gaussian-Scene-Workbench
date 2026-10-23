"""CPU real-file tests at the public dependency-license staging seam."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stage_native_dependency_licenses import (
    LicenseStagingError, inspect_dependency_licenses, stage_dependency_licenses,
    validate_material_paths,
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pe_bytes(imports=(), delay_imports=()):
    """Literal PE32+ with one .rdata section and independent import descriptors."""
    data = bytearray(2048)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x2022)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<Q', data, 0x98 + 24, 0x140000000)
    struct.pack_into('<I', data, 0x98 + 60, 512)
    struct.pack_into('<I', data, 0x98 + 108, 16)
    data[0x188:0x190] = b'.rdata\0\0'
    struct.pack_into('<IIII', data, 0x190, 1536, 0x1000, 1536, 512)
    cursor = 1024
    for directory, values, size, offset in ((1, imports, 20, 512), (13, delay_imports, 32, 768)):
        if not values:
            continue
        struct.pack_into('<II', data, 0x98 + 112 + directory * 8,
                         0x1000 + offset - 512, (len(values) + 1) * size)
        for index, name in enumerate(values):
            raw = name.encode('ascii') + b'\0'
            data[cursor:cursor + len(raw)] = raw
            rva = 0x1000 + cursor - 512
            if directory == 1:
                struct.pack_into('<IIIII', data, offset + index * size, 0, 0, 0, rva, 0)
            else:
                struct.pack_into('<IIIIIIII', data, offset + index * size, 1, rva, 0, 0, 0, 0, 0, 0)
            cursor += len(raw)
    return bytes(data)


class DependencyLicenseTests(unittest.TestCase):
    def setUp(self):
        selected = os.environ.get('GSW_SMOKE_OUTPUT_DIR')
        if selected:
            Path(selected).mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='gsw-license-test-', dir=selected)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.qt = self.root / 'qt'
        self.bin = self.root / 'package/bin'
        self.out = self.root / 'licenses'
        (self.qt / 'conda-meta').mkdir(parents=True)
        self.bin.mkdir(parents=True)

    def package(self, name='example', filename='example.dll', binary=None, license_bytes=b'Original copyright\r\nAll terms retained.\r\n'):
        binary = pe_bytes() if binary is None else binary
        cache = self.root / 'cache' / (name + '-1.2-build0')
        (cache / 'info/licenses').mkdir(parents=True)
        (cache / 'info/recipe').mkdir()
        (cache / 'info/licenses/LICENSE.txt').write_bytes(license_bytes)
        (cache / 'info/about.json').write_text(json.dumps(dict(license='BSD-3-Clause', license_file='LICENSE.txt', home='https://example.org/project')), encoding='utf-8')
        (cache / 'info/recipe/meta.yaml').write_text('package:\n  name: ' + name + '\n  version: 1.2\nsource:\n  url: https://example.org/source-1.2.tar.gz\n  sha256: ' + '1' * 64 + '\nextra:\n  remote_url: https://github.com/conda-forge/example-feedstock\n  sha: ' + '2' * 40 + '\n', encoding='utf-8')
        relative = 'Library/bin/' + filename
        installed = self.qt / relative
        installed.parent.mkdir(parents=True, exist_ok=True)
        installed.write_bytes(binary)
        (self.bin / filename).write_bytes(binary)
        meta = dict(name=name, version='1.2', build='build0', license='BSD-3-Clause',
            url='https://conda.anaconda.org/conda-forge/win-64/' + name + '-1.2-build0.conda',
            sha256='3' * 64, extracted_package_dir=str(cache), files=[relative],
            paths_data=dict(paths=[dict(_path=relative, sha256=digest(binary), sha256_in_prefix=digest(binary))]))
        (self.qt / 'conda-meta' / (name + '.json')).write_text(json.dumps(meta), encoding='utf-8')
        return cache

    def test_stages_original_license_with_deployed_binary_and_metadata_hashes(self):
        self.package()
        report = stage_dependency_licenses(self.qt, self.bin, self.out)
        self.assertEqual(report['status'], 'complete')
        self.assertEqual(report['binaries'][0]['sha256'], digest(pe_bytes()))
        entry = report['packages'][0]['license_files'][0]
        self.assertEqual((self.out / entry['path']).read_bytes(), b'Original copyright\r\nAll terms retained.\r\n')
        self.assertEqual(entry['sha256'], digest(b'Original copyright\r\nAll terms retained.\r\n'))
        self.assertEqual(json.loads((self.out / 'index.json').read_text())['schema'], 'gsw.runtime_dependency_licenses/1')

    def test_split_package_uses_only_same_source_feedstock_commit_original_notice(self):
        cache = self.package(name='runtime')
        (cache / 'info/licenses/LICENSE.txt').unlink()
        self.package(name='development', filename='unused.dll')
        (self.bin / 'unused.dll').unlink()
        report = stage_dependency_licenses(self.qt, self.bin, self.out)
        runtime = next(row for row in report['packages'] if row['name'] == 'runtime')
        self.assertEqual(runtime['license_source_package']['name'], 'development')
        self.assertEqual(runtime['recipe_identity']['source_sha256'], ['1' * 64])
        self.assertEqual((self.out / runtime['license_files'][0]['path']).read_bytes(),
                         b'Original copyright\r\nAll terms retained.\r\n')

    def test_LGPL3_package_also_ships_verified_unmodified_GPL3_text(self):
        cache = self.package()
        path = self.qt / 'conda-meta/example.json'
        meta = json.loads(path.read_text())
        meta['license'] = 'LGPL-3.0-only'
        path.write_text(json.dumps(meta), encoding='utf-8')
        about = json.loads((cache / 'info/about.json').read_text())
        about['license'] = 'LGPL-3.0-only'
        (cache / 'info/about.json').write_text(json.dumps(about), encoding='utf-8')
        report = stage_dependency_licenses(self.qt, self.bin, self.out)
        gpl = [entry for entry in report['packages'][0]['license_files']
               if entry.get('role') == 'supplemental_GPL3_text_for_LGPL3']
        self.assertEqual(len(gpl), 1)
        self.assertEqual(gpl[0]['sha256'], '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986')
        self.assertEqual((self.out / gpl[0]['path']).read_bytes(),
                         (Path(__file__).resolve().parents[1] / 'COPYING').read_bytes())

    def test_unknown_original_application_requires_explicit_exact_hash_owner(self):
        app = self.bin / 'Workbench.exe'
        app.write_bytes(pe_bytes())
        notice = self.root / 'COPYING-original'
        notice.write_bytes(b'Original application license\n')
        known = dict(schema='gsw.runtime_license_known_owners/1', owners=[dict(
            binary='Workbench.exe', sha256=digest(pe_bytes()), owner='Test Workbench',
            version='1.0', license_expression='GPL-3.0-or-later',
            source_urls=['https://example.org/source/1.0'],
            license_files=[dict(source=str(notice), name='COPYING', sha256=digest(notice.read_bytes()))])])
        report = stage_dependency_licenses(self.qt, self.bin, self.out, known_owners=known)
        self.assertEqual(report['binaries'][0]['owner_kind'], 'explicit_exact_hash')
        self.assertEqual(report['packages'][0]['name'], 'Test Workbench')

    def test_missing_declared_license_blocks_without_publishing_partial_output(self):
        cache = self.package()
        (cache / 'info/licenses/LICENSE.txt').unlink()
        (cache / 'info/licenses/unrelated.txt').write_text('Not the declared license', encoding='utf-8')
        with self.assertRaises(LicenseStagingError) as raised:
            stage_dependency_licenses(self.qt, self.bin, self.out)
        self.assertEqual(raised.exception.code, 'runtime_license_materials_incomplete')
        self.assertFalse(self.out.exists())

    def test_deployed_binary_change_cannot_be_assigned_by_filename(self):
        self.package()
        (self.bin / 'example.dll').write_bytes(pe_bytes(imports=['KERNEL32.dll']))
        report = inspect_dependency_licenses(self.qt, self.bin)
        self.assertEqual(report['status'], 'blocked')
        self.assertEqual(report['blockers'][0]['code'], 'runtime_license_owner_missing')

    def test_split_same_version_without_same_source_hash_is_rejected(self):
        cache = self.package(name='runtime')
        (cache / 'info/licenses/LICENSE.txt').unlink()
        other = self.package(name='development', filename='unused.dll')
        (self.bin / 'unused.dll').unlink()
        path = other / 'info/recipe/meta.yaml'
        path.write_text(path.read_text().replace('1' * 64, '9' * 64), encoding='utf-8')
        report = inspect_dependency_licenses(self.qt, self.bin)
        self.assertEqual(report['status'], 'blocked')
        self.assertNotIn('license_source_package', report['packages'][0])

    def test_existing_output_is_never_overwritten(self):
        self.package()
        self.out.mkdir()
        (self.out / 'user.txt').write_bytes(b'original')
        with self.assertRaises(LicenseStagingError):
            stage_dependency_licenses(self.qt, self.bin, self.out)
        self.assertEqual((self.out / 'user.txt').read_bytes(), b'original')

    def test_ordinary_and_delay_PE_imports_are_read_without_execution(self):
        self.package(binary=pe_bytes(imports=['KERNEL32.dll'], delay_imports=['optional.dll']))
        report = inspect_dependency_licenses(self.qt, self.bin)
        self.assertEqual(report['binaries'][0]['imports'], ['KERNEL32.dll'])
        self.assertEqual(report['binaries'][0]['delay_imports'], ['optional.dll'])

    def test_nonconda_Qt_SDK_is_explicitly_out_of_scope(self):
        (self.qt / 'conda-meta').rmdir()
        with self.assertRaises(LicenseStagingError) as raised:
            inspect_dependency_licenses(self.qt, self.bin)
        self.assertEqual(raised.exception.code, 'runtime_license_conda_environment_required')

    def test_metadata_path_traversal_is_rejected(self):
        self.package()
        file = self.qt / 'conda-meta/example.json'
        meta = json.loads(file.read_text())
        meta['files'] = ['../escape.dll']
        file.write_text(json.dumps(meta), encoding='utf-8')
        with self.assertRaises(LicenseStagingError):
            inspect_dependency_licenses(self.qt, self.bin)

    def test_actual_CLI_stages_or_returns_nonzero_JSON_blocker(self):
        self.package()
        cli = Path(__file__).with_name('stage_native_dependency_licenses.py')
        result = subprocess.run([sys.executable, '-B', str(cli), '--qt-root', str(self.qt),
            '--package-bin', str(self.bin), '--output-root', str(self.out)],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['status'], 'complete')
        (self.bin / 'unknown.dll').write_bytes(pe_bytes())
        blocked = subprocess.run([sys.executable, '-B', str(cli), '--qt-root', str(self.qt),
            '--package-bin', str(self.bin), '--inspect-only'], capture_output=True, text=True, timeout=10)
        self.assertEqual(blocked.returncode, 2, blocked.stderr)
        self.assertEqual(json.loads(blocked.stdout)['status'], 'blocked')

    def test_output_inside_installed_Qt_root_is_rejected(self):
        self.package()
        with self.assertRaises(LicenseStagingError) as raised:
            stage_dependency_licenses(self.qt, self.bin, self.qt / 'license-output')
        self.assertEqual(raised.exception.code, 'runtime_license_output_inside_input')

    def test_empty_declared_notice_is_not_complete_material(self):
        self.package(license_bytes=b'')
        with self.assertRaises(LicenseStagingError):
            stage_dependency_licenses(self.qt, self.bin, self.out)
        self.assertFalse(self.out.exists())

    def test_malformed_PE_is_rejected_without_loading_it(self):
        self.package(binary=b'MZ' + bytes(126))
        with self.assertRaises(LicenseStagingError):
            inspect_dependency_licenses(self.qt, self.bin)

    def test_known_owner_notice_hash_mismatch_does_not_publish(self):
        (self.bin / 'app.exe').write_bytes(pe_bytes())
        notice = self.root / 'notice'
        notice.write_bytes(b'changed')
        known = dict(schema='gsw.runtime_license_known_owners/1', owners=[dict(
            binary='app.exe', sha256=digest(pe_bytes()), owner='App', version='1',
            license_expression='MIT', source_urls=['https://example.org/app'],
            license_files=[dict(source=str(notice), name='LICENSE', sha256='0' * 64)])])
        with self.assertRaises(LicenseStagingError) as raised:
            stage_dependency_licenses(self.qt, self.bin, self.out, known_owners=known)
        self.assertEqual(raised.exception.report['blockers'][0]['code'], 'runtime_license_known_owner_notice_changed')
        self.assertFalse(self.out.exists())

    def test_real_long_package_root_uses_compact_material_paths(self):
        cache = self.package(name='runtime-package-with-original-long-name')
        (cache / 'info/licenses/LICENSE.txt').unlink()
        self.package(name='development-package-with-original-long-name', filename='unused.dll')
        (self.bin / 'unused.dll').unlink()
        parent = self.root / ('package-' + 'p' * 44) / ('version-' + 'v' * 44) / 'licenses'
        parent.mkdir(parents=True)
        output = parent / 'runtime-dependencies'
        report = stage_dependency_licenses(self.qt, self.bin, output)
        for package in report['packages']:
            for entry in package['license_files'] + package.get('source_metadata', []):
                self.assertLess(len(entry['path']), 45)
                self.assertTrue((output / entry['path']).is_file())
                self.assertIn('source_filename', entry)
        self.assertTrue((output / 'index.json').is_file())

    def test_identical_notice_bytes_deduplicate_but_original_names_and_owners_remain(self):
        self.package(name='first')
        cache = self.package(name='second', filename='second.dll')
        original = cache / 'info/licenses/LICENSE.txt'
        original.rename(cache / 'info/licenses/COPYING-second.txt')
        about_path = cache / 'info/about.json'
        about = json.loads(about_path.read_text())
        about['license_file'] = 'COPYING-second.txt'
        about_path.write_text(json.dumps(about), encoding='utf-8')
        report = stage_dependency_licenses(self.qt, self.bin, self.out)
        first, second = [package['license_files'][0] for package in report['packages']]
        self.assertEqual(first['path'], second['path'])
        self.assertEqual(first['sha256'], second['sha256'])
        self.assertEqual(first['source_filename'], 'LICENSE.txt')
        self.assertEqual(second['source_filename'], 'COPYING-second.txt')
        self.assertNotEqual(report['packages'][0]['id'], report['packages'][1]['id'])
        self.assertEqual((self.out / first['path']).read_bytes(),
                         b'Original copyright\r\nAll terms retained.\r\n')

    def test_compact_relative_hash_collision_is_rejected_at_manifest_seam(self):
        # Two literal declared hashes sharing the 128-bit address; no fake
        # SHA computation or mocking of the collector's own functions.
        first = 'a' * 32 + '0' * 32
        second = 'a' * 32 + '1' * 32
        with self.assertRaises(LicenseStagingError) as raised:
            validate_material_paths([
                dict(path='m/' + 'a' * 32, sha256=first),
                dict(path='m/' + 'a' * 32, sha256=second)])
        self.assertEqual(raised.exception.code, 'runtime_license_material_address_collision')

    def test_material_manifest_cannot_escape_content_addressed_directory(self):
        with self.assertRaises(LicenseStagingError):
            validate_material_paths([dict(path='../outside', sha256='a' * 64)])


if __name__ == '__main__':
    unittest.main()
