"""Full Mip semantics survive real public CPU coordinate/delivery interfaces."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from plyfile import PlyData, PlyElement

from native.backend import camera_scale_export as exports


class QualityMipExportTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_QUALITY_MIP_EXPORT_TEST_ROOT",
            "D:/Codex/validation/gsw-quality-upgrade-20261023/p6/backend-tests"))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    @staticmethod
    def digest(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    @staticmethod
    def read(path):
        return json.loads(Path(path).read_text(encoding="utf-8"))

    @staticmethod
    def write(path, value):
        Path(path).write_text(json.dumps(value), encoding="utf-8")

    def fixture(self, backend="3dgs", representation="fused", marked=True, quality=True):
        count = 2 if backend == "2dgs" else 3
        names = ["x", "y", "z", "opacity", "f_dc_0", "f_dc_1", "f_dc_2"]
        names += ["scale_{}".format(i) for i in range(count)] + ["rot_{}".format(i) for i in range(4)]
        data = np.zeros(3, dtype=[(name, "f4") for name in names] + [("label", "u2")])
        data["x"], data["y"], data["z"] = [6., 10., 8.], [-4., -2., -6.], [9., 5., 7.]
        data["opacity"], data["rot_0"], data["label"] = .7, 1., [17, 29, 43]
        for i in range(count):
            data["scale_{}".format(i)] = -2. - i
        source = self.root / "owned-fused.ply"
        comments = ["owned asymmetric non-cube Gaussian fixture"]
        if marked:
            comments += ["GSW_QUALITY_REQUIRES_SIDECAR full_mip_{}_v1".format(representation)]
        PlyData([PlyElement.describe(data, "vertex")], comments=comments).write(source)
        identity = np.eye(3).tolist()
        rotation = [[0., -1., 0.], [1., 0., 0.], [0., 0., 1.]]
        contract = dict(schema="gsw.training_camera_contract/1", backend=backend,
            input_data_identity=hashlib.sha256(b"owned mip non-cube geometry").hexdigest(), pose_policy="locked",
            scale=dict(status="source_declared", source_unit="cm", meters_per_unit=.01,
                canonical_unit="m", evidence=[dict(kind="source_declaration", source="generated")]),
            transforms=dict(source_to_canonical=dict(s=.01, R=identity, t=[0., 0., 0.],
                input_frame="source", output_frame="canonical", applied=True),
                canonical_to_training=dict(s=2., R=rotation, t=[10., -6., 3.],
                    input_frame="canonical", output_frame="training", applied=True)),
            views=[dict(view_id="owned-camera", split="train", image_path="not-read.png", width=640, height=480,
                K=[[510., 13., 321.25], [0., 490., 207.75], [0., 0., 1.]],
                c2w_opencv=np.eye(4).tolist(), projection_model="PINHOLE", pixel_coordinates="pixel_border")],
            ply_sha256=self.digest(source))
        self.write(source.with_suffix(".camera-scale.json"), contract)
        widths = self.root / "owned-filter.npy"
        with widths.open("wb") as stream:
            np.save(stream, np.array([.01, .02, .03], dtype="f4"), allow_pickle=False)
        record = dict(schema="gsw.quality_model/1", filter="full_mip", representation=representation,
            filter_3d_applied=representation == "fused", filter_2d=dict(variance=.3, opacity_compensation=True),
            fused_file=source.name, fused_sha256=self.digest(source), raw_source_file="retained-raw.ply",
            raw_source_sha256=(self.digest(source) if representation == "raw" else "a" * 64),
            plan_hash="b" * 64, kernel_build_sha256="c" * 64, quality_accepted=False,
            filter_widths=dict(path=widths.name, sha256=self.digest(widths), frame="training"))
        if quality:
            self.write(source.with_suffix(".quality.json"), record)
        return source, data, contract, record

    def test_fused_metric_crop_rebinds_quality_without_reapplying_3d_filter(self):
        source, original, contract, quality = self.fixture()
        originals = {p: self.digest(p) for p in self.root.iterdir() if p.is_file()}
        target = self.root / "metric-crop.ply"
        report = exports.export_recorded_ply(source, target, mode="metric", representation="auto",
            deleted_indices=[1], deleted_vertex_count=3)
        sidecar = target.with_suffix(".quality.json")
        self.assertTrue(sidecar.is_file(), "successful Full Mip export must publish quality metadata")
        derived = self.read(sidecar)
        self.assertEqual(derived["fused_sha256"], self.digest(target))
        self.assertEqual(derived["fused_file"], target.name)
        self.assertEqual(derived["derived_from_fused_sha256"], originals[source])
        self.assertEqual(derived["raw_source_sha256"], quality["raw_source_sha256"])
        self.assertNotIn("filter_widths", derived)
        self.assertEqual(derived["source_filter_widths_sha256"], quality["filter_widths"]["sha256"])
        self.assertTrue(derived["filter_3d_applied"])
        self.assertEqual(derived["filter_2d"], quality["filter_2d"])
        self.assertFalse(derived["quality_accepted"])
        self.assertEqual(report["quality_reference"]["sha256"], self.digest(sidecar))
        self.assertEqual(derived["coordinate_provenance"]["coordinate_bundle_sha256"], report["bundle_sha256"])
        self.assertEqual(derived["coordinate_provenance"]["deleted_vertices"], 1)
        self.assertEqual(report["training_contract"], contract)
        ply = PlyData.read(target, mmap=False)
        self.assertIn("GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1", ply.comments)
        data = ply["vertex"].data
        np.testing.assert_array_equal(np.column_stack([data[n] for n in ("x", "y", "z")]), [[1., 2., 3.], [0., 1., 2.]])
        for i in range(3):
            np.testing.assert_allclose(data["scale_{}".format(i)], original["scale_{}".format(i)][[0, 2]] + np.log(.5), atol=2e-7)
        self.assertEqual(data["opacity"].tobytes(), original["opacity"][[0, 2]].tobytes())
        self.assertEqual(exports.inspect_coordinate_metadata(target)["quality"]["fused_sha256"], self.digest(target))
        self.assertEqual({p: self.digest(p) for p in originals}, originals)

    def test_raw_and_missing_required_sidecar_fail_at_public_inspection(self):
        source, _, _, _ = self.fixture(representation="raw")
        with self.assertRaisesRegex(exports.CameraScaleExportError, "quality_mip_raw_requires_fusion"):
            exports.inspect_coordinate_metadata(source)
        source, _, _, _ = self.fixture(quality=False)
        source.with_suffix(".quality.json").unlink()
        with self.assertRaisesRegex(exports.CameraScaleExportError, "quality_mip_sidecar_required"):
            exports.inspect_coordinate_metadata(source)

    def test_full_mip_cannot_be_exported_under_a_semantically_unrelated_extension(self):
        source, _, _, _ = self.fixture()
        target = self.root / "would-drop-quality.spz"
        with self.assertRaisesRegex(exports.CameraScaleExportError, "quality_mip_export_format_unsupported"):
            exports.export_recorded_ply(source, target, mode="preserve")
        self.assertFalse(target.exists())

    def test_quality_coordinate_provenance_is_validated_not_just_rehashed(self):
        source, _, _, _ = self.fixture()
        target = self.root / "derived.ply"
        exports.export_recorded_ply(source, target, mode="metric")
        quality_path, metadata_path = target.with_suffix(".quality.json"), target.with_suffix(".camera-scale.json")
        quality, metadata = self.read(quality_path), self.read(metadata_path)
        quality["coordinate_provenance"]["coordinate_bundle_sha256"] = "0" * 64
        self.write(quality_path, quality)
        metadata["quality_reference"]["sha256"] = self.digest(quality_path)
        self.write(metadata_path, metadata)
        with self.assertRaisesRegex(exports.CameraScaleExportError, "quality_mip_model_binding_invalid"):
            exports.inspect_coordinate_metadata(target)

    def test_delivery_and_retry_require_the_complete_quality_bound_bundle(self):
        source, _, _, _ = self.fixture()
        first = exports.deliver_training_result(source, self.root / "delivery")
        self.assertEqual(first["export_status"], "ready")
        self.assertTrue(Path(first["quality_sidecar_path"]).is_file())
        again = exports.deliver_training_result(source, self.root / "delivery")
        self.assertEqual(again["export_status"], "ready")
        self.assertTrue(again["already_available"])
        Path(first["quality_sidecar_path"]).unlink()
        refused = exports.deliver_training_result(source, self.root / "delivery")
        self.assertEqual(refused["export_status"], "failed")
        self.assertEqual(refused["reason"], "quality_mip_sidecar_required")
        self.assertEqual(refused["training_status"], "done")
        self.assertEqual(refused["model_path"], str(source))

    def test_invalid_filter_hash_model_binding_and_widths_are_never_exported(self):
        cases = [
            ("variance", lambda q: q["filter_2d"].update(variance=.09), "quality_mip_sidecar_invalid"),
            ("compensation", lambda q: q["filter_2d"].update(opacity_compensation=False), "quality_mip_sidecar_invalid"),
            ("3d-not-fused", lambda q: q.update(filter_3d_applied=False), "quality_mip_filter_state_invalid"),
            ("model-hash", lambda q: q.update(fused_sha256="0" * 64), "quality_mip_model_hash_mismatch"),
            ("model-name", lambda q: q.update(fused_file="other.ply"), "quality_mip_model_binding_invalid"),
            ("plan-hash", lambda q: q.update(plan_hash="not-a-hash"), "quality_mip_sidecar_invalid"),
            ("width-hash", lambda q: q["filter_widths"].update(sha256="0" * 64), "quality_mip_filter_widths_invalid"),
            ("width-frame", lambda q: q["filter_widths"].update(frame="source"), "quality_mip_filter_widths_invalid"),
            ("width-path", lambda q: q["filter_widths"].update(path="../escape.npy"), "quality_mip_filter_widths_invalid"),
        ]
        for name, mutate, code in cases:
            with self.subTest(name=name):
                source, _, _, quality = self.fixture()
                mutate(quality)
                self.write(source.with_suffix(".quality.json"), quality)
                target = self.root / (name + ".ply")
                with self.assertRaisesRegex(exports.CameraScaleExportError, code):
                    exports.export_recorded_ply(source, target, mode="preserve")
                self.assertFalse(target.exists())
                self.assertFalse(target.with_suffix(".quality.json").exists())

    def test_two_dimensional_gaussians_cannot_claim_full_mip(self):
        source, _, _, _ = self.fixture(backend="2dgs")
        with self.assertRaisesRegex(exports.CameraScaleExportError, "quality_mip_representation_unsupported"):
            exports.inspect_coordinate_metadata(source)

    def test_legacy_backends_do_not_gain_quality_metadata(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                source, _, _, _ = self.fixture(backend=backend, quality=False, marked=False)
                target = self.root / (backend + "-legacy.ply")
                report = exports.export_recorded_ply(source, target, mode="metric")
                self.assertNotIn("quality_reference", report)
                self.assertNotIn("quality", exports.inspect_coordinate_metadata(target))
                self.assertFalse(target.with_suffix(".quality.json").exists())

    def test_sidecar_declared_fused_input_gains_mandatory_marker_and_reexports_once(self):
        source, _, _, quality = self.fixture(marked=False)
        target, again = self.root / "first.ply", self.root / "again.ply"
        exports.export_recorded_ply(source, target, mode="metric")
        exports.export_recorded_ply(target, again, mode="metric")
        first_data, again_data = (PlyData.read(p, mmap=False)["vertex"].data for p in (target, again))
        for name in first_data.dtype.names:
            self.assertEqual(first_data[name].tobytes(), again_data[name].tobytes())
        self.assertIn("GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1", PlyData.read(again, mmap=False).comments)
        repeated = self.read(again.with_suffix(".quality.json"))
        self.assertNotIn("filter_widths", repeated)
        self.assertEqual(repeated["source_filter_widths_sha256"], quality["filter_widths"]["sha256"])
        self.assertEqual(exports.inspect_coordinate_metadata(again)["current_unit"], "m")

    def test_cancel_late_source_changes_and_competing_files_publish_no_partial_bundle(self):
        for scenario in ("cancel", "quality-change", "width-change", "quality-race", "metadata-race"):
            with self.subTest(scenario=scenario):
                source, _, _, quality = self.fixture()
                target = self.root / (scenario + ".ply")
                cancel = []
                def progress(value):
                    if value != 95:
                        return
                    if scenario == "cancel":
                        cancel.append(True)
                    elif scenario == "quality-change":
                        changed = copy.deepcopy(quality)
                        changed["quality_accepted"] = "changed-after-validation"
                        self.write(source.with_suffix(".quality.json"), changed)
                    elif scenario == "width-change":
                        (self.root / quality["filter_widths"]["path"]).write_bytes(b"changed")
                    elif scenario == "quality-race":
                        target.with_suffix(".quality.json").write_bytes(b"foreign-quality")
                    elif scenario == "metadata-race":
                        target.with_suffix(".camera-scale.json").write_bytes(b"foreign-metadata")
                with self.assertRaises((exports.CameraScaleExportError, FileExistsError)) as caught:
                    exports.export_recorded_ply(source, target, mode="metric", progress=progress,
                        cancelled=lambda: bool(cancel))
                if scenario in ("cancel", "quality-change", "width-change"):
                    self.assertEqual(caught.exception.code, "camera_scale_export_cancelled" if scenario == "cancel"
                        else "camera_scale_export_source_changed")
                self.assertFalse(target.exists())
                self.assertFalse(target.with_suffix(".cameras.json").exists())
                for suffix, foreign in ((".quality.json", b"foreign-quality"), (".camera-scale.json", b"foreign-metadata")):
                    path = target.with_suffix(suffix)
                    if scenario == ("quality-race" if suffix == ".quality.json" else "metadata-race"):
                        self.assertEqual(path.read_bytes(), foreign)
                    else:
                        self.assertFalse(path.exists())
                self.assertFalse(list(self.root.glob(".*.tmp")))

    def test_fresh_quality_destination_and_rollback_preserve_competitor_identity(self):
        source, _, _, _ = self.fixture()
        target = self.root / "new.ply"
        quality_path = target.with_suffix(".quality.json")
        quality_path.write_bytes(b"existing-unrelated")
        with self.assertRaisesRegex(exports.CameraScaleExportError, "camera_scale_export_requires_new_output"):
            exports.export_recorded_ply(source, target, mode="metric")
        self.assertEqual(quality_path.read_bytes(), b"existing-unrelated")
        quality_path.unlink()
        def progress(value):
            if value == 100:
                quality_path.unlink()
                quality_path.write_bytes(b"replacement-owned-by-other-writer")
                raise RuntimeError("observer failed after a competitor replaced the quality path")
        with self.assertRaisesRegex(RuntimeError, "observer failed"):
            exports.export_recorded_ply(source, target, mode="metric", progress=progress)
        self.assertEqual(quality_path.read_bytes(), b"replacement-owned-by-other-writer")
        self.assertFalse(target.exists())
        self.assertFalse(target.with_suffix(".camera-scale.json").exists())
        self.assertFalse(target.with_suffix(".cameras.json").exists())

    def test_last_cancellation_callback_cannot_mutate_quality_after_source_recheck(self):
        source, _, _, quality = self.fixture()
        target = self.root / "last-callback.ply"
        reached = []
        def progress(value):
            if value == 95:
                reached.append(True)
        def cancelled():
            if reached:
                changed = copy.deepcopy(quality)
                changed["quality_accepted"] = "late-mutation"
                self.write(source.with_suffix(".quality.json"), changed)
            return False
        with self.assertRaisesRegex(exports.CameraScaleExportError, "camera_scale_export_source_changed"):
            exports.export_recorded_ply(source, target, mode="metric", progress=progress, cancelled=cancelled)
        self.assertFalse(target.exists())
        self.assertFalse(target.with_suffix(".quality.json").exists())
        self.assertFalse(list(self.root.glob(".*.tmp")))

    def test_raw_filter_attributes_and_duplicate_or_unknown_markers_cannot_fall_back(self):
        for scenario in ("raw-property", "duplicate-marker", "unknown-marker"):
            with self.subTest(scenario=scenario):
                source, data, contract, quality = self.fixture()
                comments = ["GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1"]
                if scenario == "raw-property":
                    changed = np.zeros(len(data), dtype=data.dtype.descr + [("filter_3d", "f4")])
                    for name in data.dtype.names:
                        changed[name] = data[name]
                    data = changed
                elif scenario == "duplicate-marker":
                    comments *= 2
                else:
                    comments = ["GSW_QUALITY_REQUIRES_SIDECAR unknown_future_filter"]
                PlyData([PlyElement.describe(data, "vertex")], comments=comments).write(source)
                contract["ply_sha256"] = quality["fused_sha256"] = self.digest(source)
                self.write(source.with_suffix(".camera-scale.json"), contract)
                self.write(source.with_suffix(".quality.json"), quality)
                with self.assertRaisesRegex(exports.CameraScaleExportError,
                        "quality_mip_double_application_risk" if scenario == "raw-property" else "quality_mip_header_state_invalid"):
                    exports.inspect_coordinate_metadata(source)

    def test_public_cli_publishes_a_reopenable_full_mip_bundle(self):
        source, _, _, _ = self.fixture()
        target = self.root / "CLI-品質-米.ply"
        environment = os.environ.copy()
        environment.update(PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1")
        process = subprocess.run([sys.executable, "-B", "-m", "native.backend.camera_scale_export",
            "--input", str(source), "--output", str(target), "--coordinates", "metric", "--representation", "auto"],
            cwd=str(Path(__file__).resolve().parents[3]), env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.assertEqual(process.returncode, 0, process.stderr.decode("utf-8"))
        report = json.loads(process.stdout.decode("utf-8"))
        self.assertTrue(report["success"])
        self.assertEqual(report["quality_reference"]["path"], target.with_suffix(".quality.json").name)
        self.assertEqual(exports.inspect_coordinate_metadata(target)["quality"]["fused_file"], target.name)

    def test_historical_element_comment_marker_exports_as_one_global_marker(self):
        source, data, contract, quality = self.fixture()
        marker = "GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1"
        PlyData([PlyElement.describe(data, "vertex", comments=["keep element note", marker])],
            comments=["keep global note"]).write(source)
        contract["ply_sha256"] = quality["fused_sha256"] = self.digest(source)
        self.write(source.with_suffix(".camera-scale.json"), contract)
        self.write(source.with_suffix(".quality.json"), quality)
        before = source.read_bytes()
        target = self.root / "historical-element-marker.ply"
        report = exports.export_recorded_ply(source, target, mode="metric")
        self.assertTrue(report["success"])
        reopened = exports.inspect_coordinate_metadata(target)
        self.assertEqual(reopened["quality"]["fused_sha256"], self.digest(target))
        result = PlyData.read(target, mmap=False)
        self.assertEqual(result.comments.count(marker), 1)
        self.assertIn("keep global note", result.comments)
        self.assertEqual(result["vertex"].comments, ["keep element note"])
        self.assertEqual(source.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
