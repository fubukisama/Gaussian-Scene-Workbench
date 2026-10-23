"""Public Publisher -> real PLY loader -> offscreen native OpenGL acceptance.

Requires the already-built GSW_NATIVE_MIP_PREVIEW_TEST_EXE; never builds, trains,
installs dependencies, displays a widget, or imports a CUDA rasterizer.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest


class QualityMipPreviewNativeTests(unittest.TestCase):
    def test_real_cpu_publisher_file_contract_and_native_gl(self):
        executable = os.environ.get('GSW_NATIVE_MIP_PREVIEW_TEST_EXE')
        if not executable or not Path(executable).is_file():
            self.skipTest('native preview not_run: supply built GSW_NATIVE_MIP_PREVIEW_TEST_EXE')
        try:
            import torch
        except ImportError:
            self.skipTest('native preview not_run: configured CPU Torch environment required')
        from native.worker.quality_mip import apply_filter
        from native.worker.training_preview import TrainingPreviewPublisher
        from native.worker.training_preview_policy import TrainingPreviewPolicy

        raw = SimpleNamespace(
            get_xyz=torch.tensor([[.1, .2, 2.], [0., 0., 2.5], [-.2, -.1, 3.]]),
            _features_dc=torch.zeros(3, 1, 3),
            _opacity=torch.full((3, 1), torch.logit(torch.tensor(.6)).item()),
            _scaling=torch.tensor([[.02, .03, .04]]).repeat(3, 1).log(),
            _rotation=torch.tensor([[1., 0., 0., 0.]]).repeat(3, 1))
        originals = {name: value.clone() for name, value in vars(raw).items()}
        scales, opacity = apply_filter(torch, raw._scaling.exp(), raw._opacity.sigmoid(), torch.full((3, 1), .01))
        fused = copy.copy(raw)
        fused._scaling, fused._opacity = scales.log(), torch.logit(opacity)
        fused._gsw_quality_display = dict(schema='gsw.quality_model/1', representation='fused',
            filter='full_mip', filter_3d_applied=True, filter_2d=dict(variance=.3, opacity_compensation=True),
            plan_hash='b' * 64, raw_source_kind='preview_snapshot_rows_float32', quality_accepted=False)
        fused._gsw_raw_preview_model = raw
        surfel = copy.copy(raw)
        surfel._scaling = raw._scaling[:, :2]
        root = Path(os.environ.get('GSW_QUALITY_MIP_TEST_ROOT',
            'D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence'))
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='publisher-native-', dir=str(root)) as directory:
            paths = {}
            for kind, model in (('mip', fused), ('plain', raw), ('surfel', surfel)):
                events, published_contracts = [], []

                def receive(prefix, payload):
                    events.append((prefix, payload))
                    if prefix == '[gsw-training-preview]':
                        emitted_file = Path(payload['point_cloud_path'])
                        emitted_sidecar = emitted_file.with_suffix('.quality.json')
                        published_contracts.append((emitted_file.is_file(),
                            json.loads(emitted_sidecar.read_text(encoding='utf-8'))
                            if emitted_sidecar.is_file() else None))

                # CUDA memory is an external observation boundary; a real CPU
                # fixture supplies an explicit no-GPU probe, not a tensor mock.
                policy = TrainingPreviewPolicy(memory_probe=lambda: None)
                publisher = TrainingPreviewPublisher(Path(directory) / kind,
                    receive, max_points=2, policy=policy)
                try:
                    self.assertTrue(publisher.publish_gaussians(model, 12))
                finally:
                    publisher.close()
                frames = [payload for prefix, payload in events if prefix == '[gsw-training-preview]']
                self.assertEqual(len(frames), 1)
                self.assertEqual(frames[0]['gaussian_count'], 3)
                self.assertEqual(frames[0]['preview_count'], 2)
                self.assertEqual(len(published_contracts), 1)
                self.assertTrue(published_contracts[0][0])
                self.assertEqual(published_contracts[0][1] is not None, kind == 'mip')
                paths[kind] = frames[0]['point_cloud_path']
                self.assertTrue(Path(paths[kind]).is_file())
            raw_rows = torch.cat((raw.get_xyz[::2], raw._features_dc[::2, 0, :],
                raw._opacity[::2], raw._scaling[::2], raw._rotation[::2]), 1).numpy().astype('<f4')
            raw_sha = hashlib.sha256(raw_rows.tobytes()).hexdigest()
            fused_file = Path(paths['mip'])
            metadata = json.loads(fused_file.with_suffix('.quality.json').read_text(encoding='utf-8'))
            self.assertEqual(metadata['raw_source_sha256'], raw_sha)
            self.assertEqual(metadata['raw_source_kind'], 'preview_snapshot_rows_float32')
            self.assertEqual(metadata['fused_sha256'], hashlib.sha256(fused_file.read_bytes()).hexdigest())
            self.assertIn(b'comment GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1\n', fused_file.read_bytes())
            for kind in ('plain', 'surfel'):
                self.assertFalse(Path(paths[kind]).with_suffix('.quality.json').exists())
            for name, value in originals.items():
                self.assertTrue(torch.equal(getattr(raw, name), value), name)
            fixture = Path(directory) / 'fixture.json'
            fixture.write_text(json.dumps(dict(paths, raw_sample_sha256=raw_sha)), encoding='utf-8')
            report = root / ('publisher-native-gl-' + Path(directory).name + '.txt')
            environment = dict(os.environ, GSW_QUALITY_MIP_PREVIEW_FIXTURE=str(fixture),
                               QT_QPA_PLATFORM='windows', QT_OPENGL='desktop')
            result = subprocess.run([executable, '-o', str(report) + ',txt'], env=environment,
                                    capture_output=True, text=True, timeout=60,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            content = report.read_text(encoding='utf-8') if report.is_file() else result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, content)
            self.assertNotIn('SKIP', content)
            self.assertIn('Actual native preview GL renderer:', content)
            print('Native preview report:', report)
            print(content)


if __name__ == '__main__':
    unittest.main()
