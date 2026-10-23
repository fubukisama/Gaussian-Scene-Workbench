# SPDX-License-Identifier: MIT
"""Actual worker delivery of hash-bound nested Full Mip files, CPU only."""
import hashlib
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import numpy as np
from plyfile import PlyData, PlyElement

from native.worker.gsw_worker import publish_training_delivery
from native.backend.camera_scale_export import inspect_coordinate_metadata


class QualityDeliveryTests(unittest.TestCase):
    def setUp(self):
        base = Path('D:/Codex/validation/gsw-quality-upgrade-20261023/p6/quality-delivery-tests')
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        self.iteration = self.output / 'point_cloud/iteration_64'
        self.bundle = self.iteration / 'mip-owned'
        self.bundle.mkdir(parents=True)
        self.raw, self.fused = self.iteration / 'point_cloud.ply', self.bundle / 'model.ply'
        names = ['x', 'y', 'z', 'opacity', 'f_dc_0', 'f_dc_1', 'f_dc_2']
        names += ['scale_{}'.format(i) for i in range(3)] + ['rot_{}'.format(i) for i in range(4)]
        self.data = np.zeros(2, dtype=[(n, 'f4') for n in names] + [('label', 'u2')])
        self.data['x'], self.data['z'], self.data['rot_0'], self.data['label'] = [1., -3.], [2., 4.], 1., [17, 29]
        PlyData([PlyElement.describe(self.data, 'vertex')],
            comments=['GSW_QUALITY_REQUIRES_SIDECAR full_mip_raw_v1']).write(self.raw)
        filtered = self.data.copy()
        filtered['scale_0'], filtered['opacity'] = .125, -.25
        PlyData([PlyElement.describe(filtered, 'vertex')],
            comments=['GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1']).write(self.fused)
        (self.bundle / 'raw.ply').write_bytes(self.raw.read_bytes())
        with (self.bundle / 'f.npy').open('wb') as stream:
            np.save(stream, np.array([[.25], [.5]], dtype='f4'), allow_pickle=False)
        identity = np.eye(3).tolist()
        camera = dict(schema='gsw.training_camera_contract/1', backend='3dgs', pose_policy='locked',
            input_data_identity='c' * 64,
            scale=dict(status='source_declared', source_unit='m', canonical_unit='m', meters_per_unit=1.,
                evidence=[dict(kind='source_declaration', source='owned')]),
            transforms=dict(source_to_canonical=dict(s=1., R=identity, t=[0., 0., 0.],
                input_frame='source', output_frame='canonical', applied=True),
                canonical_to_training=dict(s=2., R=identity, t=[0., 0., 0.],
                    input_frame='canonical', output_frame='training', applied=True)))
        for model in (self.raw, self.bundle / 'raw.ply', self.fused):
            self.write(model.with_suffix('.camera-scale.json'), dict(camera, ply_sha256=self.digest(model)))
        common = dict(schema='gsw.quality_model/1', filter='full_mip', plan_hash='a' * 64,
            kernel_build_sha256='b' * 64, raw_source_sha256=self.digest(self.raw), raw_source_file='raw.ply',
            fused_sha256=self.digest(self.fused), fused_file='model.ply', quality_accepted=False,
            filter_2d=dict(variance=.3, opacity_compensation=True),
            filter_widths=dict(path='f.npy', sha256=self.digest(self.bundle / 'f.npy'), frame='training'))
        self.write(self.fused.with_suffix('.quality.json'), dict(common, representation='fused', filter_3d_applied=True))
        self.write((self.bundle / 'raw.ply').with_suffix('.quality.json'), dict(common, representation='raw', filter_3d_applied=False))
        self.entry = dict(common, representation='raw', filter_3d_applied=False,
            fused_file='mip-owned/model.ply', raw_source_file='mip-owned/raw.ply',
            filter_widths=dict(common['filter_widths'], path='mip-owned/f.npy'))
        self.write(self.raw.with_suffix('.quality.json'), self.entry)

    @staticmethod
    def digest(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    @staticmethod
    def write(path, value):
        Path(path).write_text(json.dumps(value), encoding='utf-8')

    @staticmethod
    def read(path):
        return json.loads(Path(path).read_text(encoding='utf-8'))

    def test_worker_delivers_the_validated_nested_fused_file_and_preserves_raw_and_bundle(self):
        before = {p: self.digest(p) for p in self.iteration.rglob('*') if p.is_file()}
        result = publish_training_delivery(self.output, 64)
        self.assertEqual(result['export_status'], 'ready', result)
        self.assertEqual(result['training_status'], 'done')
        self.assertEqual(result['internal_model_path'], str(self.raw))
        self.assertEqual(result['fused_internal_model_path'], str(self.fused))
        self.assertTrue(Path(result['quality_sidecar_path']).is_file())
        delivered = inspect_coordinate_metadata(result['model_path'])
        self.assertEqual(delivered['quality']['raw_source_sha256'], before[self.raw])
        self.assertEqual(delivered['quality']['derived_from_fused_sha256'], before[self.fused])
        data = PlyData.read(result['model_path'], mmap=False)['vertex'].data
        self.assertAlmostEqual(float(data['scale_0'][0]), .125 + np.log(.5), places=6)
        self.assertEqual(data['label'].tobytes(), self.data['label'].tobytes())
        self.assertEqual({p: self.digest(p) for p in before}, before)
        self.assertEqual(self.read(self.output / 'model-delivery.json'), result)
        repeated = publish_training_delivery(self.output, 64)
        self.assertTrue(repeated['already_available'])

    def assert_failed_receipt(self, reason=None):
        before = self.raw.read_bytes()
        result = publish_training_delivery(self.output, 64)
        self.assertEqual(result['training_status'], 'done')
        self.assertEqual(result['export_status'], 'failed', result)
        if reason:
            self.assertEqual(result['reason'], reason)
        self.assertEqual(self.raw.read_bytes(), before)
        self.assertEqual(self.read(self.output / 'model-delivery.json'), result)
        self.assertEqual(result['internal_model_path'], str(self.raw))
        return result

    def test_absolute_traversal_drive_unc_and_ambiguous_fused_paths_are_receipted_failures(self):
        paths = [str(self.fused), 'C:/Windows/model.ply', 'D:relative.ply', '../model.ply',
            r'..\model.ply', 'mip-owned/../model.ply', r'\\server\share\model.ply',
            '/absolute.ply', 'mip-owned//model.ply', 'mip-owned/./model.ply', '', ['not-a-path']]
        for path in paths:
            with self.subTest(path=path):
                self.write(self.raw.with_suffix('.quality.json'), dict(self.entry, fused_file=path))
                self.assert_failed_receipt('quality_delivery_fused_path_invalid')
                self.assertFalse((self.output / '.gsw-deliveries').exists())

    def test_raw_fused_hash_and_cross_contract_mismatches_fail_without_reclassifying_training(self):
        cases = [
            ('raw_source_sha256', '0' * 64, 'quality_delivery_raw_hash_mismatch'),
            ('fused_sha256', '0' * 64, 'quality_delivery_fused_hash_mismatch'),
            ('plan_hash', 'd' * 64, 'quality_delivery_fused_binding_invalid'),
            ('kernel_build_sha256', 'd' * 64, 'quality_delivery_fused_binding_invalid'),
            ('representation', 'fused', 'quality_delivery_sidecar_invalid'),
            ('filter_3d_applied', True, 'quality_delivery_sidecar_invalid'),
        ]
        for key, value, reason in cases:
            with self.subTest(key=key):
                self.write(self.raw.with_suffix('.quality.json'), dict(self.entry, **{key: value}))
                self.assert_failed_receipt(reason)

    def test_fused_sidecar_and_camera_validation_are_not_bypassed_by_an_entry_hash(self):
        path = self.fused.with_suffix('.quality.json')
        original = path.read_bytes()
        path.unlink()
        self.assert_failed_receipt('quality_mip_sidecar_required')
        path.write_bytes(original)
        camera_path = self.fused.with_suffix('.camera-scale.json')
        camera = self.read(camera_path)
        camera['ply_sha256'] = '0' * 64
        self.write(camera_path, camera)
        self.assert_failed_receipt('camera_scale_export_model_hash_mismatch')

    def test_raw_marker_without_a_sidecar_never_becomes_ordinary_gaussian_delivery(self):
        self.raw.with_suffix('.quality.json').unlink()
        self.assert_failed_receipt('quality_mip_raw_requires_fusion')

    def test_widths_and_raw_snapshot_references_are_also_contained_and_hash_bound(self):
        for kind in ('raw_source_file', 'filter_widths'):
            with self.subTest(kind=kind):
                entry = copy.deepcopy(self.entry)
                if kind == 'raw_source_file':
                    entry[kind] = '../outside-raw.ply'
                else:
                    entry[kind]['path'] = '../outside.npy'
                self.write(self.raw.with_suffix('.quality.json'), entry)
                self.assert_failed_receipt('quality_delivery_fused_path_invalid')
        entry = copy.deepcopy(self.entry)
        entry['filter_widths']['sha256'] = '0' * 64
        self.write(self.raw.with_suffix('.quality.json'), entry)
        self.assert_failed_receipt('quality_delivery_fused_binding_invalid')

    def test_cancel_and_late_raw_entry_changes_never_report_quality_delivery_ready(self):
        before = self.raw.read_bytes()
        cancelled = publish_training_delivery(self.output, 64, cancelled=lambda: True)
        self.assertEqual(cancelled['export_status'], 'cancelled')
        self.assertEqual(cancelled['training_status'], 'done')
        self.assertEqual(self.read(self.output / 'model-delivery.json'), cancelled)
        def progress(value):
            if value == 95:
                changed = dict(self.entry, quality_accepted='changed-after-validation')
                self.write(self.raw.with_suffix('.quality.json'), changed)
        failed = publish_training_delivery(self.output, 64, progress=progress)
        self.assertEqual(failed['export_status'], 'failed')
        self.assertEqual(failed['reason'], 'quality_delivery_source_changed')
        self.assertEqual(failed['training_status'], 'done')
        self.assertEqual(self.raw.read_bytes(), before)

    def make_directory_link(self, link, target):
        if os.name == 'nt':
            child = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(target)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(child.returncode, 0, child.stdout + child.stderr)
        else:
            link.symlink_to(target, target_is_directory=True)
        self.addCleanup(lambda: os.rmdir(str(link)) if link.exists() else None)

    def test_directory_links_in_fused_or_delivery_paths_are_refused_without_external_writes(self):
        alias = self.iteration / 'linked-bundle'
        self.make_directory_link(alias, self.bundle)
        self.write(self.raw.with_suffix('.quality.json'), dict(self.entry, fused_file='linked-bundle/model.ply'))
        self.assert_failed_receipt('quality_delivery_link_refused')
        self.write(self.raw.with_suffix('.quality.json'), self.entry)
        outside = self.output / 'owned-outside-target'
        outside.mkdir()
        self.make_directory_link(self.output / '.gsw-deliveries', outside)
        self.assert_failed_receipt('quality_delivery_link_refused')
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
