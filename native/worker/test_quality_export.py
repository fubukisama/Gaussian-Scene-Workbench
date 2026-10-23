# SPDX-License-Identifier: MIT
"""Public Mip checkpoint export with owned typed PLY and real CPU filtering."""
import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

import numpy as np
from plyfile import PlyData, PlyElement
import torch

from native.worker.quality_export import export_mip_checkpoint
from native.worker.quality_mip import apply_filter


class CPUFilterController:
    def __init__(self, output, data):
        self.scene = SimpleNamespace(model_path=str(output))
        self.mip = SimpleNamespace(widths=torch.tensor([[.25], [.5]], dtype=torch.float32))
        self.plan, self.kernel, self.emit = {"plan_hash": "a" * 64}, {"buildSha256": "b" * 64}, None
        self.data, self.failure, self.omit_labels, self.change_filter = data.copy(), None, False, False

    def preview_model(self):
        if self.change_filter:
            self.mip.widths += .125
        data = self.data.copy()
        scales = torch.tensor(np.column_stack([data["scale_{}".format(i)] for i in range(3)])).exp()
        opacity = torch.tensor(data["opacity"].copy()[:, None]).sigmoid()
        filtered, alpha = apply_filter(torch, scales, opacity, self.mip.widths)
        for index in range(3):
            data["scale_{}".format(index)] = filtered[:, index].log().numpy()
        data["opacity"] = torch.logit(alpha[:, 0]).numpy()
        controller = self
        class Preview:
            def save_ply(self, path):
                selected = data
                if controller.omit_labels:
                    selected = np.empty(len(data), dtype=[(n, data.dtype[n]) for n in data.dtype.names if n != 'owner_label'])
                    for name in selected.dtype.names:
                        selected[name] = data[name]
                PlyData([PlyElement.describe(selected, "vertex")]).write(path)
                if controller.failure:
                    controller.failure(Path(path))
        return Preview()


class QualityExportTests(unittest.TestCase):
    def setUp(self):
        base = Path("D:/Codex/validation/gsw-quality-upgrade-20261023/p6/checkpoint-export-tests")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.iteration = self.root / "point_cloud/iteration_64"
        self.iteration.mkdir(parents=True)
        names = ["x", "y", "z", "opacity", "f_dc_0", "f_dc_1", "f_dc_2"]
        names += ["scale_{}".format(i) for i in range(3)] + ["rot_{}".format(i) for i in range(4)]
        self.data = np.zeros(2, dtype=[(name, "f4") for name in names] + [("owner_label", "u2")])
        self.data["x"], self.data["z"], self.data["rot_0"] = [1., -3.], [2., 4.], 1.
        self.data["owner_label"] = [17, 29]
        self.data["scale_0"], self.data["scale_1"], self.data["scale_2"] = np.log(1.), np.log(2.), np.log(3.)
        self.controller = CPUFilterController(self.root, self.data)
        self.raw = self.iteration / "point_cloud.ply"
        self.camera = {"schema": "gsw.training_camera_contract/1", "backend": "3dgs", "pose_policy": "locked",
            "input_data_identity": "c" * 64, "scale": {"status": "source_declared", "meters_per_unit": 1.,
                "source_unit": "m", "canonical_unit": "m", "evidence": [{"kind": "source_declaration", "source": "owned-fixture"}]},
            "transforms": {"source_to_canonical": {"s": 1., "R": np.eye(3).tolist(), "t": [0., 0., 0.],
                "input_frame": "source", "output_frame": "canonical", "applied": True},
                "canonical_to_training": {"s": 1., "R": np.eye(3).tolist(), "t": [0., 0., 0.],
                    "input_frame": "canonical", "output_frame": "training", "applied": True}}}
        self.scene_save()

    @staticmethod
    def digest(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    @staticmethod
    def read(path):
        return json.loads(Path(path).read_text(encoding="utf-8"))

    def scene_save(self):
        PlyData([PlyElement.describe(self.data, "vertex")], comments=["owned fixture"]).write(self.raw)
        self.raw.with_suffix(".camera-scale.json").write_text(
            json.dumps(dict(self.camera, ply_sha256=self.digest(self.raw))), encoding="utf-8")

    def assert_pair(self, ply):
        quality = self.read(ply.with_suffix(".quality.json"))
        camera = self.read(ply.with_suffix(".camera-scale.json"))
        digest = self.digest(ply)
        self.assertEqual(camera["ply_sha256"], digest)
        self.assertEqual(quality["fused_sha256"] if quality["representation"] == "fused" else quality["raw_source_sha256"], digest)
        self.assertEqual(quality["filter_2d"], {"variance": .3, "opacity_compensation": True})
        self.assertFalse(quality["quality_accepted"])
        return quality

    def test_scene_save_reentry_reuses_only_a_complete_bundle_and_repairs_raw_bindings(self):
        fused = Path(export_mip_checkpoint(self.controller, 64))
        self.assertRegex(fused.parent.name, r"^mip-[0-9a-f]{20}$")
        self.assertEqual(fused.name, "model.ply")
        first = {p: p.read_bytes() for p in fused.parent.iterdir()}
        self.assert_pair(fused)
        self.assert_pair(fused.parent / "raw.ply")
        self.assert_pair(self.raw)
        self.scene_save()  # Actual save+scheduled branch overwrites raw without its marker.
        repeated = Path(export_mip_checkpoint(self.controller, 64))
        self.assertEqual(repeated, fused)
        raw_quality = self.assert_pair(self.raw)
        self.assertEqual((self.raw.parent / raw_quality["fused_file"]).resolve(), fused.resolve())
        self.assertEqual({p: p.read_bytes() for p in fused.parent.iterdir()}, first)
        self.assertEqual((fused.parent / "raw.ply").read_bytes(), self.raw.read_bytes())
        data = PlyData.read(fused, mmap=False)["vertex"].data
        self.assertEqual(data["owner_label"].tobytes(), self.data["owner_label"].tobytes())
        # Independent analytic check: sigma_x=1, filter=.25 gives sqrt(1.0625).
        self.assertAlmostEqual(float(np.exp(data["scale_0"][0])), 1.0307764064, places=6)

    def test_fusion_preserves_all_source_properties_even_when_model_writer_omits_unknown_fields(self):
        self.controller.omit_labels = True
        fused = Path(export_mip_checkpoint(self.controller, 64))
        data = PlyData.read(fused, mmap=False)["vertex"].data
        self.assertEqual(data.dtype, self.data.dtype)
        for name in self.data.dtype.names:
            if name not in ("scale_0", "scale_1", "scale_2", "opacity"):
                self.assertEqual(data[name].tobytes(), self.data[name].tobytes(), name)

    def test_stage_failure_and_cancellation_leave_no_partial_bundle_or_locked_maps(self):
        for scenario in ('writer-failure', 'cancel-after-preview'):
            with self.subTest(scenario=scenario):
                self.scene_save()
                stopped = []
                def failure(path):
                    if scenario == 'writer-failure':
                        raise RuntimeError('owned simulated writer failure')
                    stopped.append(True)
                self.controller.failure = failure
                with self.assertRaisesRegex((RuntimeError, ValueError),
                        'owned simulated writer failure' if scenario == 'writer-failure' else 'quality_mip_export_cancelled'):
                    export_mip_checkpoint(self.controller, 64, cancelled=lambda: bool(stopped))
                self.assertEqual({p.name for p in self.iteration.iterdir()}, {'point_cloud.ply', 'point_cloud.camera-scale.json'})
                # Windows cleanup must succeed even with a traceback retaining locals.
                self.raw.rename(self.iteration / 'can-rename.ply')
                (self.iteration / 'can-rename.ply').rename(self.raw)
        self.controller.failure = None
        fused = Path(export_mip_checkpoint(self.controller, 64))
        self.assert_pair(fused)

    def test_existing_tampered_or_incomplete_bundle_is_preserved_and_never_bypassed(self):
        fused = Path(export_mip_checkpoint(self.controller, 64))
        quality_path, manifest_path = fused.with_suffix('.quality.json'), fused.parent / 'bundle.json'
        quality = self.read(quality_path)
        quality['filter_2d']['variance'] = .09
        quality_path.write_text(json.dumps(quality), encoding='utf-8')
        # Even a consistent file hash does not make incorrect semantics valid.
        manifest = self.read(manifest_path)
        manifest['files'][quality_path.name] = self.digest(quality_path)
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        tampered = {p: p.read_bytes() for p in fused.parent.iterdir()}
        self.scene_save()
        with self.assertRaisesRegex(ValueError, 'quality_mip_export_existing_mismatch'):
            export_mip_checkpoint(self.controller, 64)
        self.assertEqual({p: p.read_bytes() for p in fused.parent.iterdir()}, tampered)
        self.assertEqual(len(list(self.iteration.glob('mip-*'))), 1)
        quality_path.unlink()
        incomplete = {p: p.read_bytes() for p in fused.parent.iterdir()}
        with self.assertRaisesRegex(ValueError, 'quality_mip_export_existing_mismatch'):
            export_mip_checkpoint(self.controller, 64)
        self.assertEqual({p: p.read_bytes() for p in fused.parent.iterdir()}, incomplete)

    def test_a_competing_fresh_bundle_destination_is_not_replaced_or_removed(self):
        fused = Path(export_mip_checkpoint(self.controller, 64))
        destination, saved = fused.parent, self.iteration / 'preserved-first-bundle'
        destination.rename(saved)
        first = {p.name: p.read_bytes() for p in saved.iterdir()}
        self.scene_save()
        def competitor(path):
            destination.mkdir()
            (destination / 'foreign').write_bytes(b'do not remove')
        self.controller.failure = competitor
        with self.assertRaisesRegex(ValueError, 'quality_mip_export_requires_fresh_bundle'):
            export_mip_checkpoint(self.controller, 64)
        self.assertEqual((destination / 'foreign').read_bytes(), b'do not remove')
        self.assertEqual({p.name: p.read_bytes() for p in saved.iterdir()}, first)
        self.assertEqual({p.name for p in self.iteration.iterdir()}, {
            'point_cloud.ply', 'point_cloud.camera-scale.json', 'point_cloud.quality.json', destination.name, saved.name})

    def test_source_changes_during_staging_are_rejected_without_hiding_the_changed_input(self):
        def change(path):
            with self.raw.open('ab') as stream:
                stream.write(b'owned late input mutation')
        self.controller.failure = change
        with self.assertRaisesRegex(ValueError, 'quality_mip_export_source_changed'):
            export_mip_checkpoint(self.controller, 64)
        self.assertTrue(self.raw.read_bytes().endswith(b'owned late input mutation'))
        self.assertFalse(list(self.iteration.glob('mip-*')))

    def test_live_filter_change_cannot_mislabel_a_fused_snapshot_with_old_widths(self):
        self.controller.change_filter = True
        with self.assertRaisesRegex(ValueError, 'quality_mip_export_source_changed'):
            export_mip_checkpoint(self.controller, 64)
        self.assertFalse(list(self.iteration.glob('mip-*')))

    def test_rehashed_fused_tampering_cannot_override_the_existing_raw_entry_binding(self):
        fused = Path(export_mip_checkpoint(self.controller, 64))
        with fused.open('ab') as stream:
            stream.write(b'changed output despite rehashed bundle')
        quality = self.read(fused.with_suffix('.quality.json'))
        quality['fused_sha256'] = self.digest(fused)
        for name in ('model.quality.json', 'raw.quality.json'):
            row = self.read(fused.parent / name)
            row['fused_sha256'] = quality['fused_sha256']
            (fused.parent / name).write_text(json.dumps(row), encoding='utf-8')
        camera_path = fused.with_suffix('.camera-scale.json')
        camera = self.read(camera_path)
        camera['ply_sha256'] = quality['fused_sha256']
        camera_path.write_text(json.dumps(camera), encoding='utf-8')
        manifest_path = fused.parent / 'bundle.json'
        manifest = self.read(manifest_path)
        manifest['files'] = {name: self.digest(fused.parent / name) for name in manifest['files']}
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        self.scene_save()
        with self.assertRaisesRegex(ValueError, 'quality_mip_export_existing_mismatch'):
            export_mip_checkpoint(self.controller, 64)

    def test_complete_checkpoint_bundle_reopens_and_delivers_through_public_coordinate_interfaces(self):
        from native.backend.camera_scale_export import inspect_coordinate_metadata, deliver_training_result
        fused = Path(export_mip_checkpoint(self.controller, 64))
        info = inspect_coordinate_metadata(fused)
        self.assertEqual(info['quality']['fused_sha256'], self.digest(fused))
        self.assertEqual(info['quality']['raw_source_file'], 'raw.ply')
        before = {p: p.read_bytes() for p in fused.parent.iterdir()}
        delivered = deliver_training_result(fused, self.root / 'delivered')
        self.assertEqual(delivered['export_status'], 'ready', delivered)
        self.assertTrue(Path(delivered['quality_sidecar_path']).is_file())
        self.assertEqual(inspect_coordinate_metadata(delivered['model_path'])['quality']['filter_2d'],
            {'variance': .3, 'opacity_compensation': True})
        self.assertEqual({p: p.read_bytes() for p in fused.parent.iterdir()}, before)


if __name__ == "__main__":
    unittest.main()
