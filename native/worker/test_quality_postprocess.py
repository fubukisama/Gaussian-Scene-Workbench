"""Real-file CPU tests at the source-preserving cleanup public interface."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image
from plyfile import PlyData, PlyElement

from native.backend import camera_scale_export
from native.worker import quality_postprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, sort_keys=True), encoding="utf-8")


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")).hexdigest()


class QualityPostprocessTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get("GSW_POSTPROCESS_TEST_ROOT", "D:/Codex/validation/gsw-quality-upgrade-20261023/postprocess-cpu-tests"))
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=str(parent))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = self.root / "inputs"
        self.inputs.mkdir()

    def fixture(self, dimensions=3):
        names = [(name, "f4") for name in ("x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity")]
        names += [("scale_%d" % index, "f4") for index in range(dimensions)]
        names += [("rot_%d" % index, "f4") for index in range(4)]
        names += [("f_rest_%d" % index, "f4") for index in range(9)]
        names += [("confidence", "f8"), ("label", "u2"), ("gaussian_id", "u4")]
        rows = np.zeros(4, dtype=names)
        rows["x"] = [.65, 0., .25, -.6]
        rows["y"] = [0., 0., 0., .5]
        rows["z"] = 3.
        rows["opacity"] = 2.
        rows["rot_0"] = 1.
        rows["gaussian_id"] = [101, 103, 107, 109]
        rows["confidence"] = [.1, .2, .3, .4]
        rows["label"] = [7, 8, 9, 10]
        rows["f_dc_0"] = .2
        for index in range(dimensions):
            rows["scale_%d" % index] = np.log(.025)
        for index in range(9):
            rows["f_rest_%d" % index] = index * .03
        source = self.inputs / "asymmetric.ply"
        PlyData([PlyElement.describe(rows, "vertex")], text=False,
            comments=["owned asymmetric generated Gaussian fixture"], obj_info=["preserve all attributes"]).write(source)
        identity = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        views = []
        for index, split in enumerate(("train", "train", "test")):
            pose = np.eye(4); pose[0, 3] = index * .1
            views.append({"view_id": "camera-%d" % index, "split": split, "image_path": "RGB_NOT_READ.png",
                "width": 64, "height": 48, "K": [[40., 3., 32.], [0., 43., 24.], [0., 0., 1.]],
                "c2w_opencv": pose.tolist(), "projection_model": "PINHOLE", "pixel_coordinates": "pixel_border"})
        contract = {"schema": "gsw.training_camera_contract/1", "backend": "3dgs" if dimensions == 3 else "2dgs",
            "input_data_identity": hashlib.sha256(b"owned noncube cleanup scene").hexdigest(), "pose_policy": "locked",
            "scale": {"status": "source_declared", "source_unit": "m", "meters_per_unit": 1., "canonical_unit": "m",
                "evidence": [{"kind": "generated_coordinate_definition"}]},
            "transforms": {"source_to_canonical": {"s": 1., "R": identity, "t": [0., 0., 0.],
                "input_frame": "source", "output_frame": "canonical", "applied": True},
                "canonical_to_training": {"s": 1., "R": identity, "t": [0., 0., 0.],
                "input_frame": "canonical", "output_frame": "training", "applied": True}},
            "views": views, "clipping": {"frame": "training", "znear": .2, "zfar": 20.}, "ply_sha256": sha(source)}
        sidecar = source.with_suffix(".camera-scale.json")
        write_json(sidecar, contract)
        entries = []
        for view in views[:2]:
            mask = np.full((48, 64), 2, dtype="uint8")
            mask[20:29, 29:35] = 1
            mask[28:36, 19:29] = 0
            path = self.inputs / (view["view_id"] + "-mask.png")
            Image.fromarray(mask).save(str(path))
            entries.append({"view_id": view["view_id"], "path": path.name, "sha256": sha(path)})
        masks = self.inputs / "masks.json"
        write_json(masks, {"schema": "gsw.quality_cleanup_masks/1", "input_data_identity": contract["input_data_identity"],
            "labels": {"unknown": 0, "foreground": 1, "background": 2}, "views": entries})
        request = {"schema": "gsw.quality_mask_cleanup/1", "input_data_identity": contract["input_data_identity"],
            "source": {"ply": str(source), "sha256": sha(source), "sidecar": str(sidecar), "sidecar_sha256": sha(sidecar)},
            "masks": {"manifest": str(masks), "sha256": sha(masks)}, "visibility": None, "oracle_cleanup": False,
            "policy": {"min_views": 2, "alpha_support_cutoff": .001, "pixel_guard": 1,
                "depth_tolerance": {"value": 0., "unit": "m"}},
            "budget": {"max_vertices": 100000, "max_input_bytes": 67108864, "max_cached_views": 2,
                "max_cache_bytes": 268435456, "chunk_vertices": 64, "max_support_pixels": 16384,
                "max_report_bytes": 67108864}}
        return request, source, rows

    def add_visibility(self, request, role="cleanup_visibility"):
        source = Path(request["source"]["ply"])
        info = camera_scale_export.inspect_coordinate_metadata(source, request["source"]["sidecar"])
        if info["camera_path"]:
            cameras = json.loads(Path(info["camera_path"]).read_text(encoding="utf-8"))["views"]
        else:
            cameras = info["training_contract"]["views"]
        rows = []
        for view in cameras:
            if view["split"] != "train":
                continue
            depth = self.inputs / (view["view_id"] + "-depth.npy")
            np.save(str(depth), np.full((view["height"], view["width"]), 10., dtype="float64"), allow_pickle=False)
            validity = self.inputs / (view["view_id"] + "-opaque.png")
            Image.fromarray(np.ones((view["height"], view["width"]), dtype="uint8")).save(str(validity))
            rows.append({"view_id": view["view_id"], "camera_sha256": digest(view),
                "definition": "camera_z", "unit": "m", "coordinate_frame": info["current_frame"],
                "depth": {"path": depth.name, "sha256": sha(depth)},
                "opaque_validity": {"path": validity.name, "sha256": sha(validity)}})
        manifest = self.inputs / "visibility.json"
        write_json(manifest, {"schema": "gsw.quality_cleanup_visibility/1", "input_data_identity": request["input_data_identity"],
            "role": role, "provenance": {"kind": "independent_observed_depth", "independent_of_source_model": True,
                "source_id": "owned analytic free-space visibility", "source_sha256": hashlib.sha256(b"independent generated visibility").hexdigest()},
            "opaque_validity_labels": {"unknown": 0, "reliable_opaque": 1}, "views": rows})
        request["visibility"] = {"manifest": str(manifest), "sha256": sha(manifest)}
        return manifest

    def test_without_independent_visibility_every_row_and_dtype_is_retained_unverified(self):
        request, source, original = self.fixture()
        source_hash = sha(source)
        report = quality_postprocess.postprocess(request, self.root / "derived")
        self.assertEqual("unverified", report["cleanup_status"])
        self.assertEqual(0, report["deleted_count"])
        self.assertEqual(4, report["retained_count"])
        self.assertFalse(report["quality_accepted"])
        output = Path(report["model_path"])
        retained = PlyData.read(str(output), mmap=False)["vertex"].data
        for name in original.dtype.names:
            self.assertEqual(original.dtype[name], retained.dtype[name])
            self.assertEqual(original[name].tobytes(), retained[name].tobytes())
        self.assertEqual(source_hash, sha(source))
        metadata = camera_scale_export.inspect_coordinate_metadata(output)
        self.assertEqual("training", metadata["current_frame"])
        self.assertEqual(request["input_data_identity"], metadata["input_data_identity"])
        self.assertEqual("not_run", report["raw_quality_evaluation"])
        self.assertEqual("not_run", report["postprocess_quality_evaluation"])

    def test_only_whole_footprint_reliably_visible_background_is_removed(self):
        request, source, original = self.fixture()
        self.add_visibility(request)
        report = quality_postprocess.postprocess(request, self.root / "derived")
        self.assertEqual(1, report["deleted_count"])
        self.assertEqual("applied_unvalidated", report["cleanup_status"])
        actual = PlyData.read(report["model_path"], mmap=False)["vertex"].data
        self.assertEqual([103, 107, 109], actual["gaussian_id"].tolist())
        for name in original.dtype.names:
            self.assertEqual(original.dtype[name], actual.dtype[name])
            self.assertEqual(original[name][1:].tobytes(), actual[name].tobytes())
        decisions = [json.loads(line) for line in (Path(report["manifest_path"]).parent / "decisions.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual("delete", decisions[0]["action"])
        self.assertEqual(2, decisions[0]["verified_background_views"])
        self.assertEqual("foreground_overlap", decisions[2]["reason"])
        self.assertEqual("unknown_mask_overlap", decisions[3]["reason"])
        self.assertFalse(report["quality_accepted"])

    def test_cancelled_operation_releases_source_file_even_with_retained_traceback(self):
        request, source, original = self.fixture()
        calls = [0]
        def cancelled():
            calls[0] += 1
            return calls[0] >= 6
        captured = None
        try:
            quality_postprocess.postprocess(request, self.root / "cancelled", cancelled=cancelled)
        except quality_postprocess.QualityPostprocessError as error:
            captured = error
        self.assertIsNotNone(captured)
        self.assertEqual("cleanup_cancelled", captured.code)
        self.assertFalse((self.root / "cancelled").exists())
        renamed = source.with_name("source-still-owned.ply")
        source.rename(renamed)
        self.assertTrue(renamed.is_file())

    def test_evaluation_depth_requires_oracle_cleanup_and_loses_independent_geometry_eligibility(self):
        request, source, original = self.fixture()
        self.add_visibility(request, role="evaluation_only")
        with self.assertRaisesRegex(quality_postprocess.QualityPostprocessError, "explicit_oracle_cleanup"):
            quality_postprocess.postprocess(request, self.root / "forbidden")
        self.assertFalse((self.root / "forbidden").exists())
        request["oracle_cleanup"] = True
        report = quality_postprocess.postprocess(request, self.root / "oracle")
        self.assertTrue(report["oracle_cleanup"])
        self.assertFalse(report["independent_geometry_evaluation_eligible"])
        self.assertFalse(report["quality_accepted"])

    def test_an_occluded_view_vetoes_deletion_even_when_other_view_is_background(self):
        request, source, original = self.fixture()
        manifest = self.add_visibility(request)
        changed = json.loads(manifest.read_text(encoding="utf-8"))
        depth = self.inputs / changed["views"][0]["depth"]["path"]
        np.save(str(depth), np.full((48, 64), 2., dtype="float64"), allow_pickle=False)
        changed["views"][0]["depth"]["sha256"] = sha(depth)
        write_json(manifest, changed)
        request["visibility"]["sha256"] = sha(manifest)
        report = quality_postprocess.postprocess(request, self.root / "occluded")
        self.assertEqual(0, report["deleted_count"])
        self.assertEqual("unverified", report["cleanup_status"])
        decisions = [json.loads(line) for line in (self.root / "occluded/decisions.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual("occluded_or_depth_overlap", decisions[0]["reason"])

    def test_missing_visibility_does_not_turn_into_background_and_heldout_masks_are_rejected(self):
        request, source, original = self.fixture()
        manifest = self.add_visibility(request)
        changed = json.loads(manifest.read_text(encoding="utf-8"))
        changed["views"] = changed["views"][:1]
        write_json(manifest, changed); request["visibility"]["sha256"] = sha(manifest)
        report = quality_postprocess.postprocess(request, self.root / "few-views")
        self.assertEqual(0, report["deleted_count"])
        masks = Path(request["masks"]["manifest"])
        single = json.loads(masks.read_text(encoding="utf-8")); single["views"] = single["views"][:1]
        write_json(masks, single); request["masks"]["sha256"] = sha(masks)
        report = quality_postprocess.postprocess(request, self.root / "one-mask")
        self.assertEqual(0, report["deleted_count"])
        decisions = [json.loads(line) for line in (self.root / "one-mask/decisions.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual("insufficient_verified_views", decisions[0]["reason"])
        wrong = json.loads(masks.read_text(encoding="utf-8")); wrong["views"][0]["view_id"] = "camera-2"
        write_json(masks, wrong); request["masks"]["sha256"] = sha(masks)
        with self.assertRaisesRegex(quality_postprocess.QualityPostprocessError, "heldout_or_unknown_mask_view"):
            quality_postprocess.postprocess(request, self.root / "heldout")
        self.assertFalse((self.root / "heldout").exists())

    def test_two_dgs_surfel_subset_preserves_two_scales_and_every_other_property(self):
        request, source, original = self.fixture(dimensions=2)
        self.add_visibility(request)
        report = quality_postprocess.postprocess(request, self.root / "surfel")
        actual = PlyData.read(report["model_path"], mmap=False)["vertex"].data
        self.assertEqual([103, 107, 109], actual["gaussian_id"].tolist())
        self.assertNotIn("scale_2", actual.dtype.names)
        for name in original.dtype.names:
            self.assertEqual(original.dtype[name], actual.dtype[name])
            self.assertEqual(original[name][1:].tobytes(), actual[name].tobytes())

    def test_rigid_scene_transform_and_millimeter_export_have_same_cleanup_decisions(self):
        request, source, original = self.fixture()
        transform = {"schema": "gsw.scene_transform/1", "frame": "training", "unit": "training",
            "transform": {"s": 2.5, "R": [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]], "t": [10., -5., 2.]}}
        moved = self.inputs / "moved.ply"
        camera_scale_export.export_recorded_ply(source, moved, mode="preserve", representation="gaussian", scene_transform=transform)
        millimeters = self.inputs / "millimeters.ply"
        camera_scale_export.export_recorded_ply(moved, millimeters, mode="metric", output_unit="mm", representation="gaussian")
        metadata = millimeters.with_suffix(".camera-scale.json")
        request["source"] = {"ply": str(millimeters), "sha256": sha(millimeters), "sidecar": str(metadata), "sidecar_sha256": sha(metadata)}
        self.add_visibility(request)
        before = PlyData.read(str(millimeters), mmap=False)["vertex"].data.copy()
        report = quality_postprocess.postprocess(request, self.root / "millimeter-cleanup")
        actual = PlyData.read(report["model_path"], mmap=False)["vertex"].data
        self.assertEqual([103, 107, 109], actual["gaussian_id"].tolist())
        for name in before.dtype.names:
            self.assertEqual(before.dtype[name], actual.dtype[name])
            self.assertEqual(before[name][1:].tobytes(), actual[name].tobytes())
        info = camera_scale_export.inspect_coordinate_metadata(report["model_path"])
        self.assertEqual("mm", info["current_unit"])
        self.assertEqual("canonical", info["current_frame"])

    def test_public_cli_writes_fresh_derived_version_without_reading_rgb_or_starting_gpu(self):
        request, source, original = self.fixture()
        self.add_visibility(request)
        request_path = self.root / "cleanup-request.json"
        write_json(request_path, request)
        script = Path(__file__).resolve().parents[2] / "scripts/cleanup_gaussians.py"
        process = subprocess.run([sys.executable, "-B", str(script), "--request", str(request_path),
            "--output", str(self.root / "cli-derived")], capture_output=True, text=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=30)
        self.assertEqual(0, process.returncode, process.stderr)
        report = json.loads(process.stdout)
        self.assertEqual(1, report["deleted_count"])
        self.assertFalse(report["quality_accepted"])
        self.assertFalse((self.inputs / "RGB_NOT_READ.png").exists())
        self.assertEqual(sha(source), request["source"]["sha256"])

    def test_explicit_covariance_larger_than_log_scales_is_not_pruned_by_small_center_footprint(self):
        request, source, original = self.fixture()
        fields = ["cov_xx", "cov_xy", "cov_xz", "cov_yy", "cov_yz", "cov_zz"]
        enlarged = np.zeros(len(original), dtype=original.dtype.descr + [(name, "f4") for name in fields])
        for name in original.dtype.names:
            enlarged[name] = original[name]
        enlarged["cov_xx"] = 1.
        enlarged["cov_yy"] = 1.
        enlarged["cov_zz"] = 1.
        PlyData([PlyElement.describe(enlarged, "vertex")]).write(source)
        metadata = Path(request["source"]["sidecar"])
        record = json.loads(metadata.read_text(encoding="utf-8")); record["ply_sha256"] = sha(source)
        write_json(metadata, record)
        request["source"].update(sha256=sha(source), sidecar_sha256=sha(metadata))
        self.add_visibility(request)
        report = quality_postprocess.postprocess(request, self.root / "wide-support")
        self.assertEqual(0, report["deleted_count"])
        self.assertEqual(4, report["retained_count"])

    def test_input_mutation_at_last_cancel_checkpoint_prevents_complete_publication(self):
        request, source, original = self.fixture()
        self.add_visibility(request)
        mask = self.inputs / "camera-0-mask.png"
        changed = [False]
        def checkpoint():
            if not changed[0] and any(self.root.glob("*/manifest.json")):
                with mask.open("ab") as stream:
                    stream.write(b"late owned fixture mutation")
                changed[0] = True
            return False
        with self.assertRaisesRegex(quality_postprocess.QualityPostprocessError, "input_changed_before_publication"):
            quality_postprocess.postprocess(request, self.root / "must-not-publish", cancelled=checkpoint)
        self.assertTrue(changed[0])
        self.assertFalse((self.root / "must-not-publish").exists())

    def test_full_intrinsic_skew_keeps_foreground_that_diagonal_intrinsics_would_miss(self):
        request, source, original = self.fixture()
        original["x"][0], original["y"][0] = 0., 1.
        PlyData([PlyElement.describe(original, "vertex")]).write(source)
        metadata = Path(request["source"]["sidecar"])
        record = json.loads(metadata.read_text(encoding="utf-8"))
        for view in record["views"]:
            view["K"][0][1] = 18.
        record["ply_sha256"] = sha(source); write_json(metadata, record)
        request["source"].update(sha256=sha(source), sidecar_sha256=sha(metadata))
        masks = Path(request["masks"]["manifest"])
        changed = json.loads(masks.read_text(encoding="utf-8"))
        for row in changed["views"]:
            mask = np.full((48, 64), 2, dtype="uint8"); mask[36:42, 36:42] = 1
            path = self.inputs / row["path"]; Image.fromarray(mask).save(str(path)); row["sha256"] = sha(path)
        write_json(masks, changed); request["masks"]["sha256"] = sha(masks)
        self.add_visibility(request)
        report = quality_postprocess.postprocess(request, self.root / "full-k")
        ids = PlyData.read(report["model_path"], mmap=False)["vertex"]["gaussian_id"].tolist()
        self.assertIn(101, ids)  # Worked projection: u=40*0/3 + 18*1/3 + 32 = 38, not 32.

    def test_three_views_are_processed_with_bounded_lru_and_chunked_row_evidence(self):
        request, source, original = self.fixture()
        metadata = Path(request["source"]["sidecar"])
        record = json.loads(metadata.read_text(encoding="utf-8"))
        third = json.loads(json.dumps(record["views"][0])); third["view_id"] = "camera-third"; third["c2w_opencv"][0][3] = .2
        record["views"].append(third); write_json(metadata, record); request["source"]["sidecar_sha256"] = sha(metadata)
        masks = Path(request["masks"]["manifest"])
        changed = json.loads(masks.read_text(encoding="utf-8"))
        path = self.inputs / "camera-third-mask.png"
        Image.open(str(self.inputs / changed["views"][0]["path"])).save(str(path))
        changed["views"].append({"view_id": "camera-third", "path": path.name, "sha256": sha(path)})
        write_json(masks, changed); request["masks"]["sha256"] = sha(masks)
        self.add_visibility(request)
        request["budget"]["chunk_vertices"] = 1
        report = quality_postprocess.postprocess(request, self.root / "cached")
        self.assertLessEqual(report["cache_stats"]["peak_views"], 2)
        self.assertLessEqual(report["cache_stats"]["peak_bytes"], request["budget"]["max_cache_bytes"])
        self.assertGreater(report["cache_stats"]["evictions"], 0)
        self.assertEqual(1, report["deleted_count"])

    def test_existing_output_is_not_replaced_and_insufficient_cache_is_explicit(self):
        request, source, original = self.fixture()
        self.add_visibility(request)
        output = self.root / "foreign"; output.mkdir()
        sentinel = output / "user-data.txt"; sentinel.write_text("preserve", encoding="utf-8")
        with self.assertRaisesRegex(quality_postprocess.QualityPostprocessError, "fresh_output_required"):
            quality_postprocess.postprocess(request, output)
        self.assertEqual("preserve", sentinel.read_text(encoding="utf-8"))
        request["budget"]["max_cache_bytes"] = 10000
        with self.assertRaisesRegex(quality_postprocess.QualityPostprocessError, "single_view_exceeds_cache_budget"):
            quality_postprocess.postprocess(request, self.root / "under-budget")
        self.assertFalse((self.root / "under-budget").exists())

    def test_unreliable_opaque_visibility_preserves_background_candidates(self):
        request, source, original = self.fixture()
        manifest = self.add_visibility(request)
        changed = json.loads(manifest.read_text(encoding="utf-8"))
        path = self.inputs / changed["views"][0]["opaque_validity"]["path"]
        Image.fromarray(np.zeros((48, 64), dtype="uint8")).save(str(path))
        changed["views"][0]["opaque_validity"]["sha256"] = sha(path)
        write_json(manifest, changed); request["visibility"]["sha256"] = sha(manifest)
        report = quality_postprocess.postprocess(request, self.root / "uncertain-material")
        self.assertEqual(0, report["deleted_count"])
        decisions = [json.loads(line) for line in (self.root / "uncertain-material/decisions.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual("visibility_unknown", decisions[0]["reason"])

    def test_unadapted_material_reference_in_element_comment_is_rejected(self):
        request, source, original = self.fixture()
        element = PlyElement.describe(original, "vertex", comments=["TextureFile external-texture.png"])
        PlyData([element]).write(source)
        metadata = Path(request["source"]["sidecar"])
        record = json.loads(metadata.read_text(encoding="utf-8")); record["ply_sha256"] = sha(source)
        write_json(metadata, record); request["source"].update(sha256=sha(source), sidecar_sha256=sha(metadata))
        with self.assertRaisesRegex(quality_postprocess.QualityPostprocessError, "unsupported_material_assets"):
            quality_postprocess.postprocess(request, self.root / "material-unsupported")
        self.assertFalse((self.root / "material-unsupported").exists())


if __name__ == "__main__":
    unittest.main()
