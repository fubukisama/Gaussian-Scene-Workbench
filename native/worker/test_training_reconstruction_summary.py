"""Read-only reconstruction reporting through the public probe interface."""
import struct
import json
import tempfile
import unittest
from pathlib import Path


def binary_model(root, views=3, points=100):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    (root / "cameras.bin").write_bytes(struct.pack("<QiiQQdddd", 1, 1, 1, 24, 16, 20, 20, 12, 8))
    with (root / "images.bin").open("wb") as stream:
        stream.write(struct.pack("<Q", views))
        for index in range(views):
            stream.write(struct.pack("<idddddddi", index + 1, 1, 0, 0, 0, index * .1, 0, 0, 1))
            stream.write(("frame%d.png" % index).encode() + b"\0" + struct.pack("<Q", 0))
    with (root / "points3D.bin").open("wb") as stream:
        stream.write(struct.pack("<Q", points))
        for index in range(points):
            stream.write(struct.pack("<QdddBBBdQiiii", index + 1, index * .01, 0, 1, 100, 120, 140, .5, 2, 1, index, 2, index))


class ReconstructionSummaryTests(unittest.TestCase):
    def test_reports_registered_views_and_intrinsics_not_directory_photo_count(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(6):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"header-fixture")
            binary_model(dataset / "sparse" / "0")
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["registeredImages"], 3)
            self.assertEqual(report["cameraCount"], 1)
            self.assertEqual(report["inputImages"], 6)
            self.assertEqual(report["format"], "colmap_binary")
            self.assertTrue(report["usable"])
            self.assertEqual(report["decision"], "reuse")
            self.assertFalse(report["effectiveRunColmap"])

    def test_raw_input_without_undistorted_images_forces_conversion(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "input").mkdir()
            for index in range(3):
                (dataset / "input" / ("frame%d.png" % index)).write_bytes(b"raw")
            binary_model(dataset / "sparse" / "0")
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["decision"], "undistortion_required")
            self.assertTrue(report["effectiveRunColmap"])

    def test_missing_model_rebuilds_but_existing_cache_is_reused_without_mutation(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"raw")
            missing = summarize_reconstruction(dataset)
            self.assertEqual(missing["decision"], "missing_reconstruction")
            self.assertTrue(missing["effectiveRunColmap"])
            cache = dataset / ".alignment_cache" / "sparse" / "0"
            binary_model(cache)
            before = {path.name: path.read_bytes() for path in cache.iterdir()}
            reused = summarize_reconstruction(dataset)
            self.assertEqual(reused["sourceKind"], "alignment_cache")
            self.assertEqual(reused["decision"], "reuse_cache")
            self.assertFalse(reused["effectiveRunColmap"])
            self.assertFalse((dataset / "sparse").exists())
            self.assertEqual(before, {path.name: path.read_bytes() for path in cache.iterdir()})

    def test_rejects_corrupt_and_degenerate_sources_without_claiming_reuse(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"raw")
            model = dataset / "sparse" / "0"
            binary_model(model, views=2, points=31)
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["decision"], "repair_required")
            self.assertFalse(report["usable"])
            (model / "cameras.bin").write_bytes(b"x")
            corrupt = summarize_reconstruction(dataset)
            self.assertTrue(corrupt["ready"])
            self.assertNotEqual(corrupt["decision"], "reuse")
            self.assertIsNone(corrupt["cameraCount"])

    def test_camera_header_and_pose_references_are_checked_not_just_point_quality(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"raw")
            model = dataset / "sparse" / "0"
            binary_model(model)
            (model / "cameras.bin").write_bytes(struct.pack("<Q", 5000000000))
            report = summarize_reconstruction(dataset)
            self.assertTrue(report["blocked"])
            self.assertEqual(report["decision"], "invalid_source")
            self.assertIsNone(report["cameraCount"])
            self.assertFalse(report["usable"])
            rerun = summarize_reconstruction(dataset, True)
            self.assertTrue(rerun["effectiveRunColmap"])
            self.assertFalse(rerun["blocked"])

    def test_transforms_files_are_reported_as_camera_frames_not_colmap_registration(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            frames = []
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"header")
                frames.append({"file_path": "images/frame%d" % index,
                               "transform_matrix": [[1, 0, 0, index * .1], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]})
            (dataset / "transforms_train.json").write_text(json.dumps({"camera_angle_x": .8, "frames": frames}))
            (dataset / "transforms_test.json").write_text(json.dumps({"camera_angle_x": .8, "frames": []}))
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["sourceKind"], "transforms")
            self.assertEqual(report["format"], "transforms_json")
            self.assertIsNone(report["registeredImages"])
            self.assertEqual(report["frameCount"], 3)
            self.assertEqual(report["decision"], "reuse_transforms")
            self.assertFalse(report["effectiveRunColmap"])
            (dataset / "transforms_test.json").unlink()
            invalid = summarize_reconstruction(dataset)
            self.assertTrue(invalid["blocked"])
            self.assertEqual(invalid["decision"], "invalid_source")

    def test_ply_only_sparse_points_do_not_fake_an_assessable_model_or_wrong_source_files(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"raw")
            model = dataset / "sparse" / "0"
            binary_model(model)
            (model / "points3D.bin").unlink()
            (model / "points3D.ply").write_text("ply\nformat ascii 1.0\nend_header\n")
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["format"], "colmap_binary")
            self.assertTrue(report["cameraFile"].endswith("cameras.bin"))
            self.assertTrue(report["pointFile"].endswith("points3D.ply"))
            self.assertEqual(report["registeredImages"], 3)
            self.assertFalse(report["usable"])
            self.assertEqual(report["decision"], "repair_required")

    def test_text_models_and_user_requested_rerun_report_the_selected_camera_source(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"raw")
            model = dataset / "sparse" / "0"
            model.mkdir(parents=True)
            (model / "cameras.txt").write_text("1 PINHOLE 24 16 20 20 12 8\n")
            (model / "images.txt").write_text("# poses\n" + "".join(
                "%d 1 0 0 0 %f 0 0 1 frame%d.png\n\n" % (index + 1, index * .1, index)
                for index in range(3)))
            (model / "points3D.txt").write_text("".join(
                "%d %f 0 1 100 120 140 .5 1 %d 2 %d\n" % (index + 1, index * .01, index, index)
                for index in range(100)))
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["format"], "colmap_text")
            self.assertEqual(report["registeredImages"], 3)
            self.assertEqual(report["cameraCount"], 1)
            self.assertEqual(report["decision"], "reuse")
            requested = summarize_reconstruction(dataset, True)
            self.assertEqual(requested["decision"], "user_rerun")
            self.assertTrue(requested["effectiveRunColmap"])
            self.assertEqual(requested["cameraFile"], str(model / "cameras.txt"))

    def test_invalid_alignment_cache_is_not_reported_as_automatic_repair(self):
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary)
            (dataset / "images").mkdir()
            for index in range(3):
                (dataset / "images" / ("frame%d.png" % index)).write_bytes(b"raw")
            binary_model(dataset / ".alignment_cache" / "sparse" / "0", views=2, points=31)
            report = summarize_reconstruction(dataset)
            self.assertEqual(report["decision"], "invalid_cache")
            self.assertFalse(report["effectiveRunColmap"])
            self.assertTrue(report["blocked"])
            requested = summarize_reconstruction(dataset, True)
            self.assertTrue(requested["effectiveRunColmap"])
            self.assertFalse(requested["blocked"])


if __name__ == "__main__":
    unittest.main()
