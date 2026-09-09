"""Regression tests for the build-time localization gate."""
import json
from pathlib import Path
import tempfile
import unittest

import native_i18n as i18n


class CatalogTests(unittest.TestCase):
    def test_production_catalog(self):
        self.assertEqual(i18n.validate(i18n.load_catalog(), i18n.sources()), [])

    def test_missing_locale_and_placeholder(self):
        errors = i18n.validate({'Value %1': {'zh_CN': '值 %1', 'en_US': 'Value %2'}}, {})
        self.assertTrue(any('ja_JP' in error for error in errors))
        self.assertTrue(any('Placeholder mismatch' in error for error in errors))

    def test_missing_source_and_literal(self):
        errors = i18n.validate({}, {'New UI': [('test.cpp', 1, True)]})
        self.assertEqual(len(errors), 2)

    def test_functional_syntax(self):
        for source, bad in [('PLY (*.ply)', 'PLY (*.obj)'), ('<b>Title</b>', 'Title')]:
            entries = {source: dict.fromkeys(('zh_CN', 'en_US', 'ja_JP'), bad)}
            self.assertEqual(len(i18n.validate(entries, {})), 3)

    def test_cpp_extraction(self):
        text = 'QCoreApplication::translate("Workbench", "First " "second %1")'
        self.assertEqual(i18n.decode_literals(i18n.CALL.search(text)[1]), 'First second %1')
        self.assertTrue(i18n.requires_translation('New user-facing message'))
        self.assertFalse(i18n.requires_translation('Gaussian Scene Workbench/temporary'))

    def test_duplicate_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'duplicate.json'
            path.write_text('{"key": {}, "key": {}}', encoding='utf-8')
            with self.assertRaises(ValueError):
                i18n.load_catalog(path)


if __name__ == '__main__':
    unittest.main()
