"""Public manifest -> decoded payload -> real CPU Torch loss specifications.

Only newly owned D-drive fixtures are written. No renderer, CUDA or training is
started. Torch-specific cases skip in the QA Python that intentionally lacks it.
"""
import copy
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

import numpy as np


class QualitySupervisionTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_SUPERVISION_TEST_ROOT", "D:/Codex/quality-supervision-tests"))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="owned-", dir=str(base))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.geometry = {"width": 3, "height": 2, "K": [[2., 0., 1.5], [0., 2., 1.], [0., 0., 1.]],
                         "pixel_coordinates": "pixel_border", "frame": "camera", "distortion": []}
        self.contract = {"input_data_identity": "a" * 64,
                         "splits": {"train": ["view-開口"], "val": ["held-out"], "test": []},
                         "views": [{"view_id": "view-開口", "split": "train", "sha256": "b" * 64,
                                    "width": 3, "height": 2, "K": self.geometry["K"],
                                    "training_size": [3, 2], "training_K": self.geometry["K"]},
                                   {"view_id": "held-out", "split": "val", "sha256": "c" * 64,
                                    "width": 3, "height": 2, "K": self.geometry["K"],
                                    "training_size": [3, 2], "training_K": self.geometry["K"]}]}

    def asset(self, name, array):
        path = self.root / name
        np.save(str(path), array, allow_pickle=False)
        return {"path": name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def mask_manifest(self, labels=None):
        labels = np.array([[1, 1, 2], [1, 0, 2]], dtype=np.uint8) if labels is None else labels
        return {"schema": "gsw.quality_supervision/1", "dataset_identity": "a" * 64,
                "views": [{"view_id": "view-開口", "split": "train", "image_sha256": "b" * 64,
                           "geometry": copy.deepcopy(self.geometry),
                           "mask": dict(self.asset("regions.npy", labels),
                                        labels={"invalid": 0, "foreground": 1, "background": 2})}]}

    def manifest_path(self, manifest):
        path = self.root / "supervision.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path

    def depth_manifest(self, values=None, definition="camera_z"):
        values = np.full((2, 3), 2., dtype=np.float32) if values is None else values
        depth = dict(self.asset("depth.npy", values), definition=definition, unit="m", frame="camera",
            confidence=self.asset("confidence.npy", np.ones((2, 3), dtype=np.float32)),
            validity=self.asset("validity.npy", np.ones((2, 3), dtype=np.uint8)),
            materials=dict(self.asset("materials.npy", np.ones((2, 3), dtype=np.uint8)),
                labels={"unknown": 0, "opaque": 1, "transparent": 2, "specular": 3, "occluded": 4}),
            provenance={"kind": "measured", "source": "owned analytic fixture", "role": "training",
                        "alignment": {"source": "none", "view_ids": []}})
        return {"schema": "gsw.quality_supervision/1", "dataset_identity": "a" * 64,
                "views": [{"view_id": "view-開口", "split": "train", "image_sha256": "b" * 64,
                           "geometry": copy.deepcopy(self.geometry), "depth": depth}]}

    def load_depth(self, manifest, **options):
        from native.worker.quality_supervision import load_supervision
        return load_supervision(self.manifest_path(manifest), self.contract, kind="depth",
            unit_to_training_scale=2., scale_evidence_identity="e" * 64, **options)

    def registration(self, model="shared_camera_rays"):
        return {"model": model, "source_frame": "camera", "target_frame": "camera",
                "target_view_id": "view-開口", "target_image_sha256": "b" * 64,
                "R": [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]], "t": [0., 0., 0.],
                "translation_unit": "m", "translation_to_training_scale": 2.,
                "scale_evidence_identity": "e" * 64, "evidence_identity": "f" * 64}

    def test_manifest_binds_mask_regions_to_exact_train_view_and_exposes_readonly_weights(self):
        from native.worker.quality_supervision import load_supervision
        provider = load_supervision(self.manifest_path(self.mask_manifest()), self.contract, kind="mask")
        payload = provider.for_view("view-開口")
        np.testing.assert_array_equal(payload["foreground_weights"], [[1., 1., 0.], [1., 0., 0.]])
        np.testing.assert_array_equal(payload["background_weights"], [[0., 0., 1.], [0., 0., 1.]])
        self.assertFalse(payload["foreground_weights"].flags.writeable)
        self.assertEqual(provider.report["counts"]["foreground_pixels"], 3)
        self.assertEqual(provider.report["counts"]["background_pixels"], 2)
        self.assertEqual(len(provider.identity), 64)

    def test_different_intrinsics_resolution_or_distortion_requires_real_reprojection(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        for change in ("K", "width", "distortion", "array_shape"):
            manifest = self.mask_manifest()
            row = manifest["views"][0]
            if change == "K":
                row["geometry"]["K"][0][0] = 4.
            elif change == "width":
                row["geometry"]["width"] = 6
            elif change == "distortion":
                row["geometry"]["distortion"] = [.1]
            else:
                row["mask"].update(self.asset("wrong.npy", np.ones((3, 2), dtype=np.uint8)))
            with self.subTest(change=change), self.assertRaisesRegex(SupervisionError, "reprojection_required"):
                load_supervision(self.manifest_path(manifest), self.contract, kind="mask")

    def test_training_manifest_rejects_heldout_duplicate_missing_and_unbound_views(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        for change in ("heldout", "duplicate", "missing", "unknown", "image_hash", "dataset_hash", "unknown_field"):
            manifest = self.mask_manifest()
            row = manifest["views"][0]
            if change == "heldout":
                row.update(view_id="held-out", split="val", image_sha256="c" * 64)
            elif change == "duplicate":
                manifest["views"].append(copy.deepcopy(row))
            elif change == "missing":
                manifest["views"] = []
            elif change == "unknown":
                row["view_id"] = "not-in-contract"
            elif change == "image_hash":
                row["image_sha256"] = "d" * 64
            elif change == "dataset_hash":
                manifest["dataset_identity"] = "d" * 64
            else:
                manifest["fallback"] = True
            with self.subTest(change=change), self.assertRaises(SupervisionError):
                load_supervision(self.manifest_path(manifest), self.contract, kind="mask")

    def test_mask_rejects_ambiguous_labels_unclassified_pixels_or_all_invalid_weights(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        for change in ("equal_labels", "unclassified", "zero_weights", "all_invalid", "float_labels"):
            manifest = self.mask_manifest()
            mask = manifest["views"][0]["mask"]
            if change == "equal_labels":
                mask["labels"]["background"] = 1
            elif change == "unclassified":
                mask.update(self.asset("unknown.npy", np.full((2, 3), 9, dtype=np.uint8)))
            elif change == "zero_weights":
                mask["weights"] = self.asset("zero.npy", np.zeros((2, 3), dtype=np.float32))
            elif change == "all_invalid":
                mask.update(self.asset("invalid.npy", np.zeros((2, 3), dtype=np.uint8)))
            else:
                mask.update(self.asset("float.npy", np.ones((2, 3), dtype=np.float32)))
            with self.subTest(change=change), self.assertRaises(SupervisionError):
                load_supervision(self.manifest_path(manifest), self.contract, kind="mask")

    def test_real_torch_mask_loss_normalizes_weights_and_background_alpha_is_separate(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run CPU gradient cases in the existing Torch runtime")
        from native.worker.quality_supervision import load_supervision, supervision_losses
        manifest = self.mask_manifest()
        manifest["views"][0]["mask"]["weights"] = self.asset("weights.npy",
            np.array([[1., 3., 1.], [2., 0., 2.]], dtype=np.float32))
        payload = load_supervision(self.manifest_path(manifest), self.contract, kind="mask").for_view("view-開口")
        rgb = torch.tensor([[[1., 2., 100.], [3., 100., 100.]]] * 3, requires_grad=True)
        alpha = torch.tensor([[1., 1., .25], [1., .9, .75]], requires_grad=True)
        result = supervision_losses(rgb, torch.zeros_like(rgb), payload,
            rendered_alpha=alpha, mask_loss_weight=2., background_alpha_weight=.5)
        self.assertAlmostEqual(result["foreground_rgb"].item(), 13. / 6., delta=1e-6)
        self.assertAlmostEqual(result["background_alpha"].item(), 7. / 12., delta=1e-6)
        self.assertAlmostEqual(result["total"].item(), 37. / 8., delta=1e-6)
        result["total"].backward()
        self.assertEqual(rgb.grad[0, 0, 2].item(), 0.)
        self.assertEqual(rgb.grad[0, 1, 1].item(), 0.)
        self.assertGreater(rgb.grad[0, 0, 0].item(), 0.)
        self.assertEqual(alpha.grad[0, 0].item(), 0.)
        self.assertGreater(alpha.grad[0, 2].item(), 0.)

    def test_depth_z_range_and_inverse_z_convert_to_same_explicit_training_frame(self):
        # K has fx=fy=2. The first pixel's ray is (-.5,-.25,1),
        # so a camera-Z=2 plane has range sqrt(5.25) at that pixel.
        for definition in ("camera_z", "ray_range", "inverse_z"):
            values = np.full((2, 3), 2., dtype=np.float32)
            if definition == "ray_range":
                values[:] = [[np.sqrt(5.25), np.sqrt(4.25), np.sqrt(5.25)]] * 2
            elif definition == "inverse_z":
                values[:] = .5
            provider = self.load_depth(self.depth_manifest(values, definition))
            with self.subTest(definition=definition):
                np.testing.assert_allclose(provider.for_view("view-開口")["inverse_z"], .25, rtol=1e-6)
                self.assertEqual(provider.report["scale_evidence_identity"], "e" * 64)
                self.assertEqual(provider.report["counts"]["valid_depth_pixels"], 6)

    def test_relative_depth_uses_only_declared_train_alignment_and_oracle_requires_two_authorizations(self):
        from native.worker.quality_supervision import SupervisionError
        manifest = self.depth_manifest()
        provenance = manifest["views"][0]["depth"]["provenance"]
        provenance.update(kind="relative", alignment={"source": "train_sfm", "view_ids": ["view-開口"],
            "scale": 2., "shift": 0., "evidence_identity": "f" * 64})
        provider = self.load_depth(manifest)
        np.testing.assert_allclose(provider.for_view("view-開口")["inverse_z"], .125)
        provenance["alignment"]["view_ids"] = ["held-out"]
        with self.assertRaisesRegex(SupervisionError, "alignment"):
            self.load_depth(manifest)
        manifest = self.depth_manifest()
        provenance = manifest["views"][0]["depth"]["provenance"]
        provenance.update(kind="oracle", oracle_training_authorized=True)
        with self.assertRaisesRegex(SupervisionError, "oracle"):
            self.load_depth(manifest)
        payload = self.load_depth(manifest, allow_oracle=True).for_view("view-開口")
        self.assertEqual(payload["provenance"]["kind"], "oracle")
        provenance["oracle_training_authorized"] = False
        with self.assertRaisesRegex(SupervisionError, "oracle"):
            self.load_depth(manifest, allow_oracle=True)

    def test_invalid_depth_material_and_confidence_are_excluded_not_zero_ground_truth(self):
        from native.worker.quality_supervision import SupervisionError
        manifest = self.depth_manifest(np.array([[np.nan, 0., -1.], [2., 2., 2.]], dtype=np.float64))
        depth = manifest["views"][0]["depth"]
        depth["materials"].update(self.asset("material-cases.npy", np.array([[1, 1, 1], [2, 3, 1]], dtype=np.uint8)))
        depth["confidence"] = self.asset("confidence-cases.npy", np.array([[1., 1., 1.], [1., 1., .4]], dtype=np.float32))
        provider = self.load_depth(manifest)
        payload = provider.for_view("view-開口")
        np.testing.assert_array_equal(payload["inverse_z"], [[0., 0., 0.], [0., 0., .25]])
        np.testing.assert_allclose(payload["depth_weights"], [[0., 0., 0.], [0., 0., .4]])
        self.assertEqual(provider.report["counts"]["invalid_depth_values"], 3)
        self.assertEqual(provider.report["counts"]["excluded_transparent_pixels"], 1)
        self.assertEqual(provider.report["counts"]["excluded_specular_pixels"], 1)
        # A relative-depth shift must not resurrect invalid raw zero/negative data.
        manifest = self.depth_manifest(np.zeros((2, 3), dtype=np.float32))
        manifest["views"][0]["depth"]["provenance"].update(kind="relative", alignment={
            "source": "train_sfm", "view_ids": ["view-開口"], "scale": 1., "shift": 2., "evidence_identity": "f" * 64})
        with self.assertRaisesRegex(SupervisionError, "all_invalid"):
            self.load_depth(manifest)

    def test_real_torch_depth_uses_declared_inverse_z_or_normalized_moment_and_backpropagates(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run CPU gradient cases in the existing Torch runtime")
        from native.worker.quality_supervision import supervision_losses, SupervisionError
        manifest = self.depth_manifest()
        depth = manifest["views"][0]["depth"]
        depth["validity"] = self.asset("two-valid.npy", np.array([[1, 1, 0], [0, 0, 0]], dtype=np.uint8))
        depth["confidence"] = self.asset("weighted-depth.npy", np.array([[1., .5, 1.], [1., 1., 1.]], dtype=np.float32))
        payload = self.load_depth(manifest).for_view("view-開口")
        prediction = torch.tensor([[.5, .75, 10.], [10., 10., 10.]], requires_grad=True)
        alpha = torch.tensor([[.2, .8, 0.], [0., 0., 0.]], requires_grad=True)
        rgb = torch.zeros((3, 2, 3))
        direct = supervision_losses(rgb, rgb, payload,
            predicted_depth={"definition": "inverse_z", "values": prediction}, depth_loss_weight=2.)
        moment = supervision_losses(rgb, rgb, payload,
            predicted_depth={"definition": "inverse_z_moment", "values": prediction * alpha, "alpha": alpha}, depth_loss_weight=2.)
        self.assertAlmostEqual(direct["depth_inverse_z"].item(), 1. / 3., delta=1e-6)
        self.assertAlmostEqual(moment["total"].item(), 2. / 3., delta=1e-6)
        moment["total"].backward()
        self.assertGreater(prediction.grad[0, 0].item(), 0.)
        self.assertEqual(prediction.grad[1, 0].item(), 0.)
        with self.assertRaisesRegex(SupervisionError, "definition"):
            supervision_losses(rgb, rgb, payload, predicted_depth={"values": prediction}, depth_loss_weight=1.)

    def test_png_l8_regions_and_opencv_integer_k_preserve_explicit_label_semantics(self):
        from PIL import Image
        from native.worker.quality_supervision import load_supervision
        manifest = self.mask_manifest()
        row = manifest["views"][0]
        row["geometry"]["pixel_coordinates"] = "opencv_integer"
        row["geometry"]["K"][0][2] -= .5
        row["geometry"]["K"][1][2] -= .5
        path = self.root / "inverted-regions.png"
        Image.fromarray(np.array([[2, 2, 1], [2, 0, 1]], dtype=np.uint8)).save(str(path))
        row["mask"] = {"path": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                       "labels": {"invalid": 0, "foreground": 2, "background": 1}}
        payload = load_supervision(self.manifest_path(manifest), self.contract, kind="mask").for_view("view-開口")
        np.testing.assert_array_equal(payload["foreground_weights"], [[1., 1., 0.], [1., 0., 0.]])

    def test_hash_changes_missing_assets_path_escape_duplicates_and_midload_cancel_fail_explicitly(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        manifest = self.mask_manifest()
        path = self.manifest_path(manifest)
        (self.root / "regions.npy").write_bytes(b"changed")
        with self.assertRaisesRegex(SupervisionError, "hash_mismatch"):
            load_supervision(path, self.contract, kind="mask")
        manifest = self.mask_manifest()
        (self.root / "regions.npy").unlink()
        with self.assertRaisesRegex(SupervisionError, "asset_unreadable"):
            load_supervision(self.manifest_path(manifest), self.contract, kind="mask")
        manifest = self.mask_manifest()
        manifest["views"][0]["mask"]["path"] = "../outside.npy"
        with self.assertRaisesRegex(SupervisionError, "path_invalid"):
            load_supervision(self.manifest_path(manifest), self.contract, kind="mask")
        path = self.manifest_path(self.mask_manifest())
        path.write_text(path.read_text(encoding="utf-8").replace('"schema":', '"schema": "shadow", "schema":'), encoding="utf-8")
        with self.assertRaisesRegex(SupervisionError, "duplicate"):
            load_supervision(path, self.contract, kind="mask")
        path = self.manifest_path(self.mask_manifest())
        calls = []
        def cancelled():
            calls.append(1)
            return len(calls) >= 5
        with self.assertRaisesRegex(SupervisionError, "cancelled"):
            load_supervision(path, self.contract, kind="mask", cancelled=cancelled)

    def test_provider_snapshots_cannot_be_mutated_and_import_does_not_load_torch(self):
        provider = self.load_depth(self.depth_manifest())
        payload = provider.for_view("view-開口")
        payload["provenance"]["kind"] = "oracle"
        self.assertEqual(provider.for_view("view-開口")["provenance"]["kind"], "measured")
        with self.assertRaises(ValueError):
            payload["depth_weights"].setflags(write=True)
        from native.worker.quality_supervision import SupervisionError
        with self.assertRaisesRegex(SupervisionError, "not_train"):
            provider.for_view("held-out")
        check = subprocess.run([sys.executable, "-c", "import sys; import native.worker.quality_supervision; assert 'torch' not in sys.modules"],
            cwd=str(Path(__file__).resolve().parents[2]), capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stderr)

    def test_unrepresentable_converted_depth_never_becomes_zero_or_infinite_ground_truth(self):
        from native.worker.quality_supervision import SupervisionError
        for value in (1e308, 1e-300):
            with self.subTest(value=value), self.assertRaisesRegex(SupervisionError, "all_invalid"):
                self.load_depth(self.depth_manifest(np.full((2, 3), value, dtype=np.float64)))

    def test_explicit_units_have_equivalent_depth_and_missing_scale_or_evaluation_role_is_rejected(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        for unit, value, factor in (("m", 2., 2.), ("cm", 200., .02), ("mm", 2000., .002)):
            manifest = self.depth_manifest(np.full((2, 3), value, dtype=np.float64))
            manifest["views"][0]["depth"]["unit"] = unit
            provider = load_supervision(self.manifest_path(manifest), self.contract, kind="depth",
                unit_to_training_scale=factor, scale_evidence_identity="e" * 64)
            with self.subTest(unit=unit):
                np.testing.assert_allclose(provider.for_view("view-開口")["inverse_z"], .25)
        path = self.manifest_path(self.depth_manifest())
        with self.assertRaisesRegex(SupervisionError, "scale_evidence"):
            load_supervision(path, self.contract, kind="depth")
        manifest = self.depth_manifest()
        manifest["views"][0]["depth"]["provenance"]["role"] = "evaluation_only"
        with self.assertRaisesRegex(SupervisionError, "provenance"):
            self.load_depth(manifest)

    def test_all_invalid_depth_confidence_validity_and_material_maps_fail_before_training(self):
        from native.worker.quality_supervision import SupervisionError
        for change in ("all_invalid", "confidence_zero", "confidence_nan", "validity_zero", "transparent", "specular", "occluded", "unknown"):
            manifest = self.depth_manifest()
            depth = manifest["views"][0]["depth"]
            if change == "all_invalid":
                depth.update(self.asset("bad-depth.npy", np.full((2, 3), np.inf, dtype=np.float32)))
            elif change.startswith("confidence"):
                depth["confidence"] = self.asset("bad-confidence.npy", np.full((2, 3),
                    np.nan if change.endswith("nan") else 0., dtype=np.float32))
            elif change == "validity_zero":
                depth["validity"] = self.asset("bad-validity.npy", np.zeros((2, 3), dtype=np.uint8))
            else:
                depth["materials"].update(self.asset("bad-materials.npy", np.full((2, 3),
                    depth["materials"]["labels"][change], dtype=np.uint8)))
            with self.subTest(change=change), self.assertRaisesRegex(SupervisionError, "all_invalid"):
                self.load_depth(manifest)

    def test_disabled_supervision_is_zero_and_missing_render_coverage_is_explicit(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run CPU gradient cases in the existing Torch runtime")
        from native.worker.quality_supervision import supervision_losses, SupervisionError
        rgb = torch.ones((3, 2, 3), requires_grad=True)
        untouched = rgb.detach().clone()
        result = supervision_losses(rgb, torch.zeros_like(rgb), None)
        self.assertEqual(result["total"].item(), 0.)
        self.assertTrue(torch.equal(rgb.detach(), untouched))
        self.assertIsNone(rgb.grad)
        self.assertEqual(result["counts"], {})
        payload = self.load_depth(self.depth_manifest()).for_view("view-開口")
        with self.assertRaisesRegex(SupervisionError, "all_invalid"):
            supervision_losses(rgb, torch.zeros_like(rgb), payload, depth_loss_weight=1., predicted_depth={
                "definition": "inverse_z_moment", "values": torch.zeros((2, 3)), "alpha": torch.zeros((2, 3))})

    def test_real_torch_uint8_staged_targets_have_same_rgb_units_as_float_targets(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run CPU gradient cases in the existing Torch runtime")
        from native.worker.quality_supervision import load_supervision, supervision_losses
        payload = load_supervision(self.manifest_path(self.mask_manifest()), self.contract, kind="mask").for_view("view-開口")
        rgb = torch.ones((3, 2, 3), requires_grad=True)
        target = torch.full((3, 2, 3), 255, dtype=torch.uint8)
        result = supervision_losses(rgb, target, payload, mask_loss_weight=1.)
        self.assertEqual(result["foreground_rgb"].item(), 0.)
        result["total"].backward()
        self.assertTrue(torch.equal(rgb.grad, torch.zeros_like(rgb)))

    def test_single_declared_unit_scale_cannot_silently_convert_mixed_unit_views(self):
        from native.worker.quality_supervision import SupervisionError
        manifest = self.depth_manifest()
        other_view = copy.deepcopy(self.contract["views"][0])
        other_view.update(view_id="view-other", sha256="d" * 64)
        self.contract["views"].append(other_view)
        self.contract["splits"]["train"].append("view-other")
        other_row = copy.deepcopy(manifest["views"][0])
        other_row.update(view_id="view-other", image_sha256="d" * 64)
        other_row["depth"]["unit"] = "cm"
        manifest["views"].append(other_row)
        with self.assertRaisesRegex(SupervisionError, "mixed_depth_units"):
            self.load_depth(manifest)

    def test_camera_contract_itself_requires_unique_views_nonempty_train_and_hashed_identity(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        for change in ("duplicate_view", "duplicate_split", "missing_identity", "no_train"):
            contract = copy.deepcopy(self.contract)
            manifest = self.mask_manifest()
            if change == "duplicate_view":
                contract["views"].append(copy.deepcopy(contract["views"][0]))
            elif change == "duplicate_split":
                contract["splits"]["train"].append("view-開口")
            elif change == "missing_identity":
                contract["input_data_identity"] = None
                manifest["dataset_identity"] = None
            else:
                contract["views"] = [contract["views"][1]]
                contract["splits"]["train"] = []
                manifest["views"] = []
            with self.subTest(change=change), self.assertRaisesRegex(SupervisionError, "camera_contract"):
                load_supervision(self.manifest_path(manifest), contract, kind="mask")

    def test_underflowed_mask_weights_or_depth_confidence_are_not_reported_active(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        manifest = self.mask_manifest()
        manifest["views"][0]["mask"]["weights"] = self.asset("tiny-mask.npy", np.full((2, 3), 1e-300, dtype=np.float64))
        with self.assertRaisesRegex(SupervisionError, "all_invalid"):
            load_supervision(self.manifest_path(manifest), self.contract, kind="mask")
        manifest = self.depth_manifest()
        manifest["views"][0]["depth"]["confidence"] = self.asset("tiny-confidence.npy", np.full((2, 3), 1e-300, dtype=np.float64))
        with self.assertRaisesRegex(SupervisionError, "all_invalid"):
            self.load_depth(manifest)

    def test_registered_mask_inverse_rays_respect_offcenter_k_and_unknown_instead_of_resize(self):
        from native.worker.quality_supervision import load_supervision
        manifest = self.mask_manifest(np.array([[1, 2, 0, 1], [2, 1, 1, 0]], dtype=np.uint8))
        row = manifest["views"][0]
        row["geometry"].update(width=4, K=[[2., 0., 2.], [0., 2., 1.], [0., 0., 1.]])
        row["registration"] = self.registration()
        provider = load_supervision(self.manifest_path(manifest), self.contract, kind="mask",
            unit_to_training_scale=2., scale_evidence_identity="e" * 64)
        payload = provider.for_view("view-開口")
        np.testing.assert_array_equal(payload["foreground_weights"], [[0., 0., 1.], [1., 1., 0.]])
        np.testing.assert_array_equal(payload["background_weights"], [[1., 0., 0.], [0., 0., 0.]])
        self.assertEqual(provider.report["views"][0]["geometry_adapter"], "shared_camera_rays")
        self.assertEqual(provider.report["counts"]["invalid_pixels"], 2)

    def test_registered_depth_backprojects_then_translates_and_forward_projects_literal_point(self):
        # Source pixel (0,0), K principal point(.5,.5), Z=2 m -> (0,0,2)m.
        # Explicit t=(.5,0,1)m gives target(.5,0,3)m. At training scale2,
        # target Z=6, target pixel=(1+5/6,1), hence cell(row1,col1).
        values = np.zeros((2, 3), dtype=np.float64)
        values[0, 0] = 2.
        manifest = self.depth_manifest(values)
        row = manifest["views"][0]
        row["geometry"]["K"] = [[1., 0., .5], [0., 1., .5], [0., 0., 1.]]
        row["depth"]["confidence"] = self.asset("point-confidence.npy", np.array([[.6, 1., 1.], [1., 1., 1.]], dtype=np.float32))
        row["registration"] = self.registration("rigid_depth")
        row["registration"]["t"] = [.5, 0., 1.]
        provider = self.load_depth(manifest)
        payload = provider.for_view("view-開口")
        np.testing.assert_allclose(payload["inverse_z"], [[0., 0., 0.], [0., 1. / 6., 0.]], rtol=1e-6)
        np.testing.assert_allclose(payload["depth_weights"], [[0., 0., 0.], [0., .6, 0.]])
        self.assertEqual(provider.report["counts"]["valid_depth_pixels"], 1)
        self.assertEqual(provider.report["counts"]["registration_uncovered_pixels"], 5)

    def test_registered_depth_nearest_transparent_winner_blocks_opaque_background_not_average(self):
        manifest = self.depth_manifest(np.array([[1., 2., 1.]], dtype=np.float64))
        row = manifest["views"][0]
        row["geometry"].update(height=1, K=[[4., 0., .5], [0., 1., .5], [0., 0., 1.]])
        depth = row["depth"]
        depth["confidence"] = self.asset("occlusion-confidence.npy", np.array([[.7, .9, .2]], dtype=np.float32))
        depth["validity"] = self.asset("occlusion-validity.npy", np.ones((1, 3), dtype=np.uint8))
        depth["materials"].update(self.asset("occlusion-material.npy", np.array([[1, 1, 2]], dtype=np.uint8)))
        row["registration"] = self.registration("rigid_depth")
        provider = self.load_depth(manifest)
        payload = provider.for_view("view-開口")
        # Source1 (Z=2) and source2 (Z=1) land in target cell(1,2).
        # Source2 wins geometrically but is transparent: no opaque substitution.
        np.testing.assert_array_equal(payload["winner_source_index"], [[-1, -1, -1], [-1, 0, 2]])
        np.testing.assert_array_equal(payload["exclusion_reason"], [[1, 1, 1], [1, 0, 3]])
        np.testing.assert_allclose(payload["inverse_z"], [[0., 0., 0.], [0., .5, 0.]])
        np.testing.assert_allclose(payload["depth_weights"], [[0., 0., 0.], [0., .7, 0.]])
        self.assertEqual(provider.report["counts"]["registration_occluded_source_pixels"], 1)
        self.assertEqual(provider.report["counts"]["excluded_transparent_pixels"], 1)
        evidence = provider.report["views"][0]["registration_result"]
        self.assertEqual(evidence["winner_source_index"]["shape"], [2, 3])
        self.assertEqual(evidence["exclusion_reason_codes"]["transparent"], 3)
        # If that same winner is explicitly opaque, transport its own .2
        # confidence, not the farther sample's .9 and not an average.
        depth["materials"].update(self.asset("opaque-material.npy", np.ones((1, 3), dtype=np.uint8)))
        opaque = self.load_depth(manifest).for_view("view-開口")
        np.testing.assert_allclose(opaque["depth_weights"], [[0., 0., 0.], [0., .7, .2]])
        np.testing.assert_allclose(opaque["inverse_z"], [[0., 0., 0.], [0., .5, .5]])

    def test_registration_rejects_untrusted_transform_units_scale_binding_and_non_numeric_values(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        for change in ("reflection", "nonorthogonal", "numeric_string", "boolean_translation", "translation",
                       "unit", "scale", "scale_evidence", "target_view", "target_hash", "missing", "distortion"):
            manifest = self.mask_manifest(np.ones((2, 4), dtype=np.uint8))
            row = manifest["views"][0]
            row["geometry"].update(width=4, K=[[2., 0., 2.], [0., 2., 1.], [0., 0., 1.]])
            row["registration"] = self.registration()
            registration = row["registration"]
            if change == "reflection": registration["R"][0][0] = -1.
            elif change == "nonorthogonal": registration["R"][0][0] = 2.
            elif change == "numeric_string": registration["R"][0][0] = "1"
            elif change == "boolean_translation": registration["t"][0] = False
            elif change == "translation": registration["t"][0] = .1
            elif change == "unit": registration["translation_unit"] = "unknown"
            elif change == "scale": registration["translation_to_training_scale"] = 1.
            elif change == "scale_evidence": registration["scale_evidence_identity"] = "0" * 64
            elif change == "target_view": registration["target_view_id"] = "held-out"
            elif change == "target_hash": registration["target_image_sha256"] = "c" * 64
            elif change == "missing": row.pop("registration")
            else: row["geometry"]["distortion"] = [.01]
            with self.subTest(change=change), self.assertRaises(SupervisionError):
                load_supervision(self.manifest_path(manifest), self.contract, kind="mask",
                    unit_to_training_scale=2., scale_evidence_identity="e" * 64)

    def test_registered_depth_full_skew_k_rotation_and_metric_units_have_literal_equivalent_result(self):
        from native.worker.quality_supervision import load_supervision
        original_contract = copy.deepcopy(self.contract)
        for unit, per_metre, factor in (("m", 1., 2.), ("cm", 100., .02), ("mm", 1000., .002)):
            for definition in ("camera_z", "ray_range", "inverse_z"):
                # Source K maps pixel(1,1) to ray(.25,.5,1). Z=2m gives
                # (.5,1,2)m; Rz90+t(1,-.25,1)m gives target(0,.25,3)m.
                values = np.zeros((2, 3), dtype=np.float64)
                values[1, 1] = (2. * per_metre if definition == "camera_z" else
                    np.sqrt(5.25) * per_metre if definition == "ray_range" else 1. / (2. * per_metre))
                manifest = self.depth_manifest(values, definition)
                row = manifest["views"][0]
                row["geometry"]["K"] = [[2., 1., .5], [0., 2., .5], [0., 0., 1.]]
                row["depth"]["unit"] = unit
                row["registration"] = self.registration("rigid_depth")
                row["registration"].update(R=[[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]],
                    t=[1000., -250., 1000.], translation_unit="mm", translation_to_training_scale=.002)
                provider = load_supervision(self.manifest_path(manifest), self.contract, kind="depth",
                    unit_to_training_scale=factor, scale_evidence_identity="e" * 64)
                with self.subTest(unit=unit, definition=definition):
                    np.testing.assert_allclose(provider.for_view("view-開口")["inverse_z"],
                        [[0., 0., 0.], [0., 1. / 6., 0.]], rtol=1e-6)
        self.assertEqual(self.contract, original_contract)

    def test_registered_mask_skew_and_pixel_convention_keep_outside_rays_invalid(self):
        from native.worker.quality_supervision import load_supervision
        manifest = self.mask_manifest(np.array([[1, 2, 0, 0, 0], [2, 1, 0, 0, 0], [0, 0, 0, 0, 0]], dtype=np.uint8))
        row = manifest["views"][0]
        row["geometry"].update(width=5, height=3, K=[[2., 1., 0.], [0., 2., 0.], [0., 0., 1.]],
            pixel_coordinates="opencv_integer")
        row["registration"] = self.registration()
        provider = load_supervision(self.manifest_path(manifest), self.contract, kind="mask",
            unit_to_training_scale=2., scale_evidence_identity="e" * 64)
        payload = provider.for_view("view-開口")
        np.testing.assert_array_equal(payload["foreground_weights"], [[0., 1., 0.], [0., 0., 1.]])
        np.testing.assert_array_equal(payload["background_weights"], [[0., 0., 1.], [0., 1., 0.]])
        self.assertEqual(provider.report["counts"]["registration_uncovered_pixels"], 2)

    def test_provider_preflight_retains_metadata_only_and_lru_rechecks_assets_after_eviction(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        manifest = self.mask_manifest()
        for index in (1, 2):
            view = copy.deepcopy(self.contract["views"][0])
            view.update(view_id="extra-" + str(index), sha256=str(index) * 64)
            self.contract["views"].append(view)
            self.contract["splits"]["train"].append(view["view_id"])
            row = copy.deepcopy(manifest["views"][0])
            row.update(view_id=view["view_id"], image_sha256=view["sha256"])
            row["mask"].update(self.asset("extra-" + str(index) + ".npy", np.ones((2, 3), dtype=np.uint8)))
            manifest["views"].append(row)
        path = self.manifest_path(manifest)
        provider = load_supervision(path, self.contract, kind="mask", max_cached_views=2, max_cached_bytes=1024)
        self.assertEqual(provider.cache_info["resident_views"], 0)
        for view_id in ("view-開口", "extra-1", "extra-2"):
            provider.for_view(view_id)
        self.assertEqual(provider.cache_info["resident_views"], 2)
        self.assertEqual(provider.cache_info["resident_bytes"], 96)
        self.assertEqual(provider.cache_info["evictions"], 1)
        (self.root / "regions.npy").write_bytes(b"changed after preflight")
        with self.assertRaisesRegex(SupervisionError, "hash_mismatch"):
            provider.for_view("view-開口")
        uncached = load_supervision(self.manifest_path(self.mask_manifest()), self.contract_for_one_view(),
            kind="mask", max_cached_views=2, max_cached_bytes=1)
        self.assertEqual(uncached.for_view("view-開口")["foreground_weights"].shape, (2, 3))
        self.assertEqual(uncached.cache_info["resident_views"], 0)

    def contract_for_one_view(self):
        contract = copy.deepcopy(self.contract)
        contract["views"] = [view for view in contract["views"] if view["view_id"] in ("view-開口", "held-out")]
        contract["splits"]["train"] = ["view-開口"]
        return contract

    def test_concurrent_current_and_prefetch_requests_keep_resident_cache_accounting_bounded(self):
        from native.worker.quality_supervision import load_supervision
        provider = load_supervision(self.manifest_path(self.mask_manifest()), self.contract, kind="mask",
            max_cached_views=2, max_cached_bytes=96)
        barrier = threading.Barrier(4)
        def request(unused):
            barrier.wait(timeout=10)
            return provider.for_view("view-開口")["foreground_weights"].sum()
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(request, range(4)))
        self.assertEqual(results, [3., 3., 3., 3.])
        self.assertEqual(provider.cache_info["resident_views"], 1)
        self.assertEqual(provider.cache_info["resident_bytes"], 48)

    def test_cache_hits_recheck_hash_and_byte_budget_evicts_before_view_count_limit(self):
        from native.worker.quality_supervision import load_supervision, SupervisionError
        manifest = self.mask_manifest()
        provider = load_supervision(self.manifest_path(manifest), self.contract, kind="mask", max_cached_bytes=96)
        provider.for_view("view-開口")
        provider.for_view("view-開口")
        self.assertEqual(provider.cache_info["hits"], 1)
        (self.root / "regions.npy").write_bytes(b"changed cached source")
        with self.assertRaisesRegex(SupervisionError, "hash_mismatch"):
            provider.for_view("view-開口")
        manifest = self.mask_manifest()
        row = copy.deepcopy(manifest["views"][0])
        row.update(view_id="extra", image_sha256="d" * 64)
        manifest["views"].append(row)
        view = copy.deepcopy(self.contract["views"][0])
        view.update(view_id="extra", sha256="d" * 64)
        self.contract["views"].append(view)
        self.contract["splits"]["train"].append("extra")
        provider = load_supervision(self.manifest_path(manifest), self.contract, kind="mask",
            max_cached_views=2, max_cached_bytes=95)
        provider.for_view("view-開口")
        provider.for_view("extra")
        self.assertEqual(provider.cache_info["resident_views"], 1)
        self.assertEqual(provider.cache_info["resident_bytes"], 48)

    def test_registered_depth_excludes_nonpositive_target_z_and_rejects_guessed_translation_scale(self):
        from native.worker.quality_supervision import SupervisionError
        manifest = self.depth_manifest()
        manifest["views"][0]["registration"] = self.registration("rigid_depth")
        manifest["views"][0]["registration"]["t"] = [0., 0., -3.]
        with self.assertRaisesRegex(SupervisionError, "all_invalid"):
            self.load_depth(manifest)
        manifest["views"][0]["registration"].update(t=[0., 0., 0.], translation_unit="cm")
        # Caller scale2 is for depth metres. One centimetre must map to.02,
        # not the manifest's old factor2; reject rather than silently guess.
        with self.assertRaisesRegex(SupervisionError, "translation_scale_mismatch"):
            self.load_depth(manifest)

    def test_unrepresentable_source_range_geometry_cannot_turn_into_translation_origin_ground_truth(self):
        from native.worker.quality_supervision import SupervisionError
        values = np.zeros((2, 3), dtype=np.float64)
        values[0, 0] = 1.
        manifest = self.depth_manifest(values, "ray_range")
        row = manifest["views"][0]
        row["geometry"]["K"] = [[1e-200, 0., 1.5], [0., 1e200, 1.], [0., 0., 1.]]
        row["registration"] = self.registration("rigid_depth")
        row["registration"]["t"] = [0., 0., 1.]
        with self.assertRaisesRegex(SupervisionError, "all_invalid"):
            self.load_depth(manifest)


if __name__ == "__main__":
    unittest.main()
