"""Public runtime selection tests using synthetic filesystem evidence only."""
import tempfile
import hashlib
import json
import sys
import unittest
from pathlib import Path

from native.worker.quality_rasterizer_runtime import select_quality_rasterizer, REQUIRED_GPU_CHECKS


class QualityRuntimeTests(unittest.TestCase):
    def build_fixture(self, folder):
        root = Path(folder)
        (root / 'source.py').write_text('synthetic source\n', encoding='utf-8')
        (root / 'binary.pyd').write_bytes(b'synthetic-not-a-real-cuda-binary')
        self.build = {'schema': 'gsw.quality_rasterizer_build/1', 'quality_abi': 1,
                      'pinhole_covariance_version': 1, 'python': '{}.{}'.format(*sys.version_info[:2]),
                      'torch': 'synthetic-test', 'cuda': 'synthetic-test', 'cuda_arch': '8.9',
                      'source_hash_mode': 'text-lf-v1',
                      'source_files': {'source.py': hashlib.sha256(b'synthetic source\n').hexdigest()},
                      'runtime_files': {'binary.pyd': hashlib.sha256((root / 'binary.pyd').read_bytes()).hexdigest()}}
        self.build_path = root / 'quality-rasterizer-build.json'
        self.build_path.write_text(json.dumps(self.build), encoding='utf-8')
        return root

    def test_missing_quality_build_is_rejected(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_build_missing'):
                select_quality_rasterizer(Path(folder), Path(folder))

    def test_build_without_actual_gpu_acceptance_is_rejected(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            root = self.build_fixture(folder)
            self.build['gpu_verified'] = True  # An unsupported build claim cannot bypass acceptance.
            self.build_path.write_text(json.dumps(self.build), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_gpu_acceptance_missing'):
                select_quality_rasterizer(root, root)

    def test_cpu_only_acceptance_is_rejected(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            root = self.build_fixture(folder)
            (root / 'quality-rasterizer-acceptance.json').write_text(json.dumps({
                'schema': 'gsw.quality_rasterizer_acceptance/1', 'gpu_executed': False,
                'accepted': True}), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_gpu_acceptance_invalid'):
                select_quality_rasterizer(root, root)

    def write_synthetic_acceptance(self, root):
        # Parser fixture ONLY: these bytes are not a numerical GPU test result.
        acceptance = {'schema': 'gsw.quality_rasterizer_acceptance/1', 'gpu_executed': True,
                      'accepted': True, 'build_manifest_sha256': hashlib.sha256(self.build_path.read_bytes()).hexdigest(),
                      'checks': {key: True for key in REQUIRED_GPU_CHECKS}, 'synthetic_test_fixture': True}
        (root / 'quality-rasterizer-acceptance.json').write_text(json.dumps(acceptance), encoding='utf-8')

    def test_synthetic_complete_parser_fixture_can_be_selected(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            root = self.build_fixture(folder)
            self.write_synthetic_acceptance(root)
            result = select_quality_rasterizer(root, root)
            self.assertIn('absolute_screen_gradient_ndc', result['verified_capabilities'])

    def test_changed_binary_cannot_reuse_prior_acceptance(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            root = self.build_fixture(folder)
            self.write_synthetic_acceptance(root)
            (root / 'binary.pyd').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_integrity_mismatch'):
                select_quality_rasterizer(root, root)

    def test_wrong_abi_is_rejected_before_import(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            root = self.build_fixture(folder)
            self.build['quality_abi'] = 2
            self.build_path.write_text(json.dumps(self.build), encoding='utf-8')
            self.write_synthetic_acceptance(root)
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_build_invalid'):
                select_quality_rasterizer(root, root)

    def test_incomplete_gpu_check_list_cannot_advertise_capabilities(self):
        with tempfile.TemporaryDirectory(dir='D:/CodexData/temp') as folder:
            root = self.build_fixture(folder)
            self.write_synthetic_acceptance(root)
            path = root / 'quality-rasterizer-acceptance.json'
            acceptance = json.loads(path.read_text(encoding='utf-8'))
            acceptance['checks'].pop('absolute_precancellation')
            path.write_text(json.dumps(acceptance), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'quality_rasterizer_gpu_acceptance_invalid'):
                select_quality_rasterizer(root, root)


if __name__ == '__main__':
    unittest.main()
