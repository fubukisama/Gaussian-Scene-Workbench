"""Real COLMAP fixtures for the two-view/31-point generation regression."""
import json
import shutil
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def write_model(root, images=9, points=500):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    (root / "cameras.bin").write_bytes(struct.pack("<QiiQQdddd", 1, 1, 1, 1920, 1080, 1600, 1600, 960, 540))
    with (root / "images.bin").open("wb") as stream:
        stream.write(struct.pack("<Q", images))
        for index in range(images):
            stream.write(struct.pack("<idddddddi", index + 1, 1, 0, 0, 0, index * .1, 0, 0, 1))
            stream.write(("frame%02d.jpg" % index).encode() + b"\0")
            stream.write(struct.pack("<Q", 0))
    with (root / "points3D.bin").open("wb") as stream:
        stream.write(struct.pack("<Q", points))
        for index in range(points):
            stream.write(struct.pack("<QdddBBBdQ", index + 1, index * .01, index % 7 * .1, 1 + index % 5, 100, 120, 140, .5, 2))
            stream.write(struct.pack("<iiii", 1, index, 2, index))
    return root


class ReconstructionQualityTests(unittest.TestCase):
    def test_retries_are_bounded_and_explicit_camera_calibration_is_preserved(self):
        from crop_editor import server
        for payload, expected in (({}, 3), ({"camera_model": "PINHOLE"}, 2),
                                  ({"quality_recovery": False}, 1)):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as tmp:
                dataset = Path(tmp)
                (dataset / "images").mkdir()
                for i in range(9):
                    (dataset / "images" / ("frame%02d.jpg" % i)).write_bytes(b"raw")
                mappers, features = [], []
                def run(_job, command, _cwd, _backend):
                    if command[1] == "feature_extractor": features.append(command)
                    if command[1] == "mapper":
                        mappers.append(command)
                        write_model(Path(command[command.index("--output_path") + 1]) / "0", 2, 31)
                    self.assertNotEqual(command[1], "image_undistorter")
                with mock.patch.object(server, "colmap_executable", return_value=Path("colmap.exe")), mock.patch.object(server, "run_logged", side_effect=run):
                    with self.assertRaisesRegex(RuntimeError, "reconstruction_quality"):
                        server.run_colmap_convert({"log": []}, dataset, server.colmap_options_from_payload(payload))
                self.assertEqual(len(mappers), expected)
                for command in mappers[1:]:
                    self.assertEqual(command[command.index("--Mapper.random_seed") + 1], "1")
                    self.assertEqual(command[command.index("--Mapper.init_min_tri_angle") + 1], "4")
                if payload.get("camera_model"):
                    self.assertEqual(len(features), 1)
                    self.assertEqual(features[0][features[0].index("--ImageReader.camera_model") + 1], "PINHOLE")

    def test_viability_covers_both_gaussian_backends_before_overwrite(self):
        from crop_editor import server, reconstruction_quality
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                dataset = root / "dataset"
                (dataset / "images").mkdir(parents=True)
                for i in range(9):
                    (dataset / "images" / ("frame%02d.jpg" % i)).write_bytes(b"raw")
                write_model(dataset / "sparse" / "0", 2, 31)
                output = root / "output" / "scene"
                output.mkdir(parents=True)
                (output / "previous.ply").write_bytes(b"keep")
                (output / "training_backend.json").write_text(json.dumps({"backend": backend}))
                job = {"id": "quality-fixture", "scene": "scene", "output_scene": "scene",
                       "backend": backend, "dataset_path": str(dataset), "log": [], "train_options": {}}
                report = reconstruction_quality.assess_dataset(dataset)
                with mock.patch.object(server, "OUTPUT_DIR", root / "output"), mock.patch.object(server, "ensure_training_environment", return_value={"python": "python", "colmap": "colmap", "two_dgs_dir": "2dgs"}), mock.patch.object(server, "run_logged"), mock.patch.object(server, "persist_train_job"), mock.patch.object(server, "run_colmap_convert", side_effect=reconstruction_quality.ReconstructionQualityError(report)):
                    server.run_training_job(job, False, "quick", True)
                self.assertEqual(job["status"], "failed")
                self.assertEqual(job["generation_issue"], "reconstruction_quality")
                self.assertEqual((output / "previous.ply").read_bytes(), b"keep")

    def test_missing_archived_frames_fail_without_publishing_partial_input(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp)
            originals = dataset / "source" / "originals"
            originals.mkdir(parents=True)
            (dataset / "images").mkdir()
            (dataset / "images" / "frame.jpg").write_bytes(b"undistorted")
            (dataset / "source" / "metadata_manifest.json").write_text(json.dumps({"files": [{"kind": "video", "originalFilename": "capture.mp4", "archivePath": "source/originals/missing.mp4", "extractedFrames": 9, "extractionFps": 2}]}))
            with self.assertRaises(RuntimeError):
                server.preserve_colmap_source_images(dataset)
            self.assertFalse((dataset / "input").exists())
            self.assertEqual((dataset / "images" / "frame.jpg").read_bytes(), b"undistorted")

    def test_existing_output_conflict_rejects_before_colmap_or_source_mutation(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / "dataset"
            (dataset / "images").mkdir(parents=True)
            for i in range(9):
                (dataset / "images" / ("frame%02d.jpg" % i)).write_bytes(b"original")
            sparse = write_model(dataset / "sparse" / "0", 9, 500)
            original_points = (sparse / "points3D.bin").read_bytes()
            output = root / "output" / "scene"
            output.mkdir(parents=True)
            (output / "previous.ply").write_bytes(b"keep")
            (output / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}))
            job = {"id": "output-conflict-fixture", "scene": "scene", "output_scene": "scene",
                   "backend": "2dgs", "dataset_path": str(dataset), "log": [], "train_options": {}}
            with mock.patch.object(server, "OUTPUT_DIR", root / "output"), mock.patch.object(server, "ensure_training_environment", return_value={"python": "python", "colmap": "colmap", "two_dgs_dir": "2dgs"}), mock.patch.object(server, "run_logged") as run_logged, mock.patch.object(server, "persist_train_job"), mock.patch.object(server, "run_colmap_convert") as convert:
                server.run_training_job(job, True, "quick", False)
            self.assertEqual(job["status"], "failed")
            self.assertIn("Output already exists", job["error"])
            convert.assert_not_called()
            run_logged.assert_not_called()
            self.assertEqual((output / "previous.ply").read_bytes(), b"keep")
            self.assertEqual((sparse / "points3D.bin").read_bytes(), original_points)
            self.assertEqual((dataset / "images" / "frame00.jpg").read_bytes(), b"original")
            self.assertFalse((dataset / "input").exists())

    def test_corrupt_binary_and_low_coverage_models_are_not_admitted(self):
        from crop_editor import reconstruction_quality as quality
        with tempfile.TemporaryDirectory() as tmp:
            model = write_model(Path(tmp) / "model", 3, 500)
            self.assertFalse(quality.assess_model(model, 9)["usable"])
            self.assertIn("low_coverage", quality.assess_model(model, 9)["reasons"])
            (model / "images.bin").write_bytes(struct.pack("<Q", 2**63))
            self.assertEqual(quality.assess_model(model, 9)["reasons"], ["invalid_model"])

    def test_quick_2dgs_includes_normal_phase_and_respects_manual_settings(self):
        from crop_editor import server
        preset = server.training_options_from_payload("2dgs", "quick", {})
        self.assertEqual(preset["iterations"], 10000)
        self.assertEqual(preset["resolution"], 2)
        manual = server.training_options_from_payload("2dgs", "quick", {"iterations": 7000, "resolution": 8})
        self.assertEqual(manual["iterations"], 7000)
        self.assertEqual(manual["resolution"], 8)

    def test_legacy_archived_video_uses_matching_import_rate_and_keeps_frames(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / "datasets" / "C0001"
            originals = dataset / "source" / "originals"
            originals.mkdir(parents=True)
            video = originals / "C0001.MP4"
            video.write_bytes(b"original media")
            (dataset / "source" / "metadata_manifest.json").write_text(json.dumps({"files": [{"kind": "video", "originalFilename": video.name, "archivePath": "source/originals/C0001.MP4", "extractedFrames": 9}]}))
            jobs = root / ".gsw" / "jobs"
            jobs.mkdir(parents=True)
            (jobs / "import-test.json").write_text(json.dumps({"scene": "C0001", "fps": 2, "files": [{"path": str(video)}]}))
            def extract(path, staging, fps):
                self.assertEqual(path, video)
                self.assertEqual(fps, 2)
                for i in range(9): (staging / ("frame%02d.jpg" % i)).write_bytes(b"raw")
                return 9
            with mock.patch.object(server, "extract_video_frames", side_effect=extract):
                result = server.preserve_colmap_source_images(dataset)
            self.assertEqual(len(list(result.glob("*.jpg"))), 9)
            self.assertEqual(video.read_bytes(), b"original media")
            self.assertEqual(list(dataset.glob(".colmap-source-*")), [])

    def test_incomplete_existing_input_is_not_silently_reused(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp)
            (dataset / "input").mkdir()
            (dataset / "input" / "frame.jpg").write_bytes(b"keep")
            (dataset / "source").mkdir()
            (dataset / "source" / "metadata_manifest.json").write_text(json.dumps({"files": [{"kind": "video", "extractedFrames": 9}]}))
            with self.assertRaisesRegex(RuntimeError, "source_frames_missing"):
                server.preserve_colmap_source_images(dataset)
            self.assertEqual((dataset / "input" / "frame.jpg").read_bytes(), b"keep")

    def test_text_models_preserve_empty_observations_and_reject_invalid_tracks(self):
        from crop_editor import reconstruction_quality as quality
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cameras.txt").write_text("1 PINHOLE 1920 1080 1600 1600 960 540\n")
            (root / "images.txt").write_text("".join("%d 1 0 0 0 0 0 0 1 frame%d.jpg\n\n" % (i, i) for i in range(1, 4)))
            points = "".join("%d 0 0 1 100 120 140 0.5 1 %d 2 %d\n" % (i, i, i) for i in range(100))
            (root / "points3D.txt").write_text(points)
            self.assertTrue(quality.assess_model(root, 3)["usable"])
            (root / "points3D.txt").write_text(points + "101 0 0 1 100 120 140 0.5 1 -1 2 1\n")
            self.assertFalse(quality.assess_model(root, 3)["usable"])

    def test_does_not_undistort_first_degenerate_component(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "scene"
            images = dataset / "images"
            images.mkdir(parents=True)
            for i in range(9):
                (images / ("frame%02d.jpg" % i)).write_bytes(b"raw")
            selected = []

            def run(_job, command, _cwd, _backend):
                if command[1] == "mapper":
                    output = Path(command[command.index("--output_path") + 1])
                    write_model(output / "0", 2, 31)
                    write_model(output / "1", 5, 2)
                    write_model(output / "2", 9, 500)
                if command[1] == "image_undistorter":
                    source = Path(command[command.index("--input_path") + 1])
                    selected.append(source.name)
                    target = Path(command[command.index("--output_path") + 1])
                    shutil.copytree(images, target / "images")
                    shutil.copytree(source, target / "sparse")
                return 0

            with mock.patch.object(server, "colmap_executable", return_value=Path("colmap.exe")), mock.patch.object(server, "run_logged", side_effect=run):
                server.run_colmap_convert({"log": []}, dataset, server.colmap_options_from_payload({}))
            self.assertEqual(selected, ["2"])
            self.assertEqual((dataset / "input" / "frame00.jpg").read_bytes(), b"raw")

    def test_bad_candidates_never_reach_training_or_replace_old_model(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "scene"
            images = dataset / "images"
            images.mkdir(parents=True)
            (images / "frame00.jpg").write_bytes(b"keep")
            for i in range(1, 9):
                (images / ("frame%02d.jpg" % i)).write_bytes(b"keep")
            original = write_model(dataset / "sparse" / "0")
            before = (original / "points3D.bin").read_bytes()

            def run(_job, command, _cwd, _backend):
                if command[1] == "mapper":
                    root = Path(command[command.index("--output_path") + 1])
                    write_model(root / "0", 2, 31)
                    write_model(root / "1", 5, 2)
                self.assertNotEqual(command[1], "image_undistorter")
                return 0

            with mock.patch.object(server, "colmap_executable", return_value=Path("colmap.exe")), mock.patch.object(server, "run_logged", side_effect=run):
                with self.assertRaisesRegex(RuntimeError, "reconstruction_quality"):
                    server.run_colmap_convert({"log": []}, dataset, server.colmap_options_from_payload({"quality_recovery": False}))
            self.assertEqual((original / "points3D.bin").read_bytes(), before)
            self.assertEqual((images / "frame00.jpg").read_bytes(), b"keep")

    def test_published_subset_does_not_destroy_raw_frames_for_second_run(self):
        from crop_editor import server
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp)
            images = dataset / "images"
            images.mkdir()
            for i in range(9):
                (images / ("frame%02d.jpg" % i)).write_bytes(b"raw")
            staging = dataset / ".colmap-undistorted-test"
            (staging / "images").mkdir(parents=True)
            (staging / "images" / "frame00.jpg").write_bytes(b"undistorted")
            write_model(staging / "sparse" / "0", 5, 500)
            server.publish_undistorted_colmap_output(dataset, staging)
            self.assertEqual(len(list((dataset / "input").glob("*.jpg"))), 9)
            self.assertEqual(server.colmap_image_input_path(dataset), dataset / "input")


if __name__ == "__main__":
    unittest.main()
