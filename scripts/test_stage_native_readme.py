"""Package README links must resolve inside the actual distribution."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stage_native_readme import stage_readme


class PackageReadmeTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get('GSW_SMOKE_OUTPUT_DIR',
                      'D:/t' if Path('D:/t').is_dir() else tempfile.gettempdir()))
        parent.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=str(parent), prefix='gsw-readme-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'source 日本語'
        (self.root / 'native').mkdir(parents=True)
        (self.root / 'docs').mkdir()
        self.package = Path(self.tmp.name) / 'package'
        self.package.mkdir()

    def readme(self, text):
        (self.root / 'native/README.md').write_text(text, encoding='utf-8')

    def test_real_files_rebased_with_anchor_and_original_preserved(self):
        self.readme('[Quality](../docs/QUALITY_PLAN.md) [组](../docs/MULTI.md#同时选中) [GPL](../COPYING)')
        (self.root / 'docs/QUALITY_PLAN.md').write_bytes(b'# Quality\n')
        (self.root / 'docs/MULTI.md').write_bytes(b'# Groups\n')
        (self.root / 'COPYING').write_bytes(b'license\n')
        before = hashlib.sha256((self.root / 'native/README.md').read_bytes()).hexdigest()
        result = stage_readme(self.root, self.package)
        actual = (self.package / 'README.md').read_text(encoding='utf-8')
        self.assertIn('(docs/QUALITY_PLAN.md)', actual)
        self.assertIn('(docs/MULTI.md#同时选中)', actual)
        self.assertIn('(COPYING)', actual)
        self.assertEqual(b'# Quality\n', (self.package / 'docs/QUALITY_PLAN.md').read_bytes())
        self.assertEqual(b'license\n', (self.package / 'COPYING').read_bytes())
        self.assertEqual(before, hashlib.sha256((self.root / 'native/README.md').read_bytes()).hexdigest())
        self.assertEqual(3, len(result['copied_links']))

    def test_external_and_anchor_links_unchanged(self):
        self.readme('[web](https://example.org/file) [section](#scope)')
        stage_readme(self.root, self.package)
        self.assertEqual((self.root / 'native/README.md').read_bytes(), (self.package / 'README.md').read_bytes())

    def test_native_local_license_is_rebased_and_actual_readme_links_all_resolve(self):
        self.readme('[native license](LICENSE.md)')
        (self.root / 'native/LICENSE.md').write_bytes(b'Native GPL scope\n')
        stage_readme(self.root, self.package)
        self.assertEqual('[native license](native/LICENSE.md)', (self.package / 'README.md').read_text(encoding='utf-8'))
        self.assertEqual(b'Native GPL scope\n', (self.package / 'native/LICENSE.md').read_bytes())
        real_root = Path(__file__).resolve().parents[1]
        real_package = self.package / 'actual'
        stage_readme(real_root, real_package)
        import re
        for link in re.findall(r'\]\(([^)]+)\)', (real_package / 'README.md').read_text(encoding='utf-8')):
            if link.startswith(('https://', 'http://', '#')):
                continue
            self.assertTrue((real_package / link.split('#', 1)[0]).is_file(), link)

    def test_missing_link_is_error_before_writes(self):
        self.readme('[Missing](../docs/missing.md)')
        with self.assertRaisesRegex(ValueError, 'missing'):
            stage_readme(self.root, self.package)
        self.assertEqual([], list(self.package.iterdir()))

    def test_traversal_and_unknown_parent_target_rejected(self):
        for link in ('../docs/../../secret.md', '../private.json', '../docs/nested/file.md'):
            self.readme('[bad](' + link + ')')
            with self.assertRaisesRegex(ValueError, 'unsupported'):
                stage_readme(self.root, self.package)
            self.assertEqual([], list(self.package.iterdir()))


if __name__ == '__main__':
    unittest.main()
