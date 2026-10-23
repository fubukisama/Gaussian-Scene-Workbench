"""Real Windows batch system-boundary regression; never compiles code."""
import os
from pathlib import Path
import tempfile
import unittest

from training_kit.quality_rasterizer.build import read_windows_batch_environment


@unittest.skipUnless(os.name == 'nt', 'Windows batch environment seam')
class BuildEnvironmentTests(unittest.TestCase):
    def test_real_batch_with_spaces_preserves_unicode_environment(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            batch = Path(folder) / 'setup with spaces.bat'
            batch.write_bytes(b'@echo off\r\nset GSW_BATCH_VISITED=yes\r\nset GSW_BATCH_ECHO=%GSW_BATCH_INPUT%\r\n')
            inherited = dict(os.environ)
            inherited['GSW_BATCH_INPUT'] = '\u65e5\u672c\u8a9e-\u4e2d\u6587-en'
            result = read_windows_batch_environment(batch, inherited)
            self.assertEqual(result['GSW_BATCH_VISITED'], 'yes')
            self.assertEqual(result['GSW_BATCH_ECHO'], inherited['GSW_BATCH_INPUT'])

    def test_existing_batch_with_command_metacharacter_path_is_rejected(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            batch = Path(folder) / 'unsafe&setup.bat'
            batch.write_bytes(b'@echo off\r\nset GSW_UNSAFE_BATCH_VISITED=yes\r\n')
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_msvc_setup_unsafe_path'):
                read_windows_batch_environment(batch)

    def test_nonzero_real_batch_exit_is_rejected(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            batch = Path(folder) / 'failing setup.cmd'
            batch.write_bytes(b'@echo off\r\nexit /b 7\r\n')
            with self.assertRaisesRegex(RuntimeError, 'quality_rasterizer_msvc_setup_failed: exit=7'):
                read_windows_batch_environment(batch)


if __name__ == '__main__':
    unittest.main()
