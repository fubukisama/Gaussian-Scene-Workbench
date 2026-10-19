import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

from native.worker.camera_rasterizer_runtime import select_camera_rasterizer


class CameraRasterizerRuntimeTests(unittest.TestCase):
    def test_selected_runtime_binds_python_source_and_binary_bytes(self):
        base = Path('D:/Codex/qa-camera-p0')
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            source, runtime = root / 'source', root / 'runtime'
            source.mkdir(); runtime.mkdir()
            (source / 'kernel.cu').write_bytes(b'owned kernel source')
            (runtime / 'kernel.pyd').write_bytes(b'owned binary fixture, never imported')
            proof = {'schema': 'gsw.camera_rasterizer_runtime/1', 'pinhole_covariance_version': 1,
                     'python': '{}.{}'.format(sys.version_info.major, sys.version_info.minor),
                     'source_files': {'kernel.cu': hashlib.sha256(b'owned kernel source').hexdigest()},
                     'runtime_files': {'kernel.pyd': hashlib.sha256(b'owned binary fixture, never imported').hexdigest()}}
            path = runtime / 'camera-rasterizer-proof.json'
            path.write_text(json.dumps(proof), encoding='utf-8')
            self.assertEqual(select_camera_rasterizer(source, runtime)['root'], str(runtime.resolve()))
            (runtime / 'kernel.pyd').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'integrity_mismatch'):
                select_camera_rasterizer(source, runtime)


if __name__ == '__main__':
    unittest.main()
