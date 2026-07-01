import sys
import io
import json
import shutil
import struct
import tempfile
import types
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import numpy as np
from plyfile import PlyData, PlyElement

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import server  # noqa: E402


class FakeUploadItem:
    def __init__(self, filename, data):
        self.filename = filename
        self.file = io.BytesIO(data)


class FakeUploadForm(dict):
    def getfirst(self, key, default=None):
        value = self.get(key, default)
        if isinstance(value, list):
            return value[0] if value else default
        return value


class FakeCgiForm(dict):
    def getlist(self, key):
        value = self.get(key, [])
        values = value if isinstance(value, list) else [value]
        return [getattr(item, "value", item) for item in values]


class FakeProcess:
    def __init__(self):
        self.terminated = False

    def poll(self):
        return None

    def terminate(self):
        self.terminated = True


def write_test_ply(path, xyz):
    dtype = [
        ("x", "f4"),
        ("y", "f4"),
        ("z", "f4"),
        ("scale_0", "f4"),
        ("scale_1", "f4"),
        ("opacity", "f4"),
    ]
    vertices = np.empty(len(xyz), dtype=dtype)
    vertices["x"] = xyz[:, 0]
    vertices["y"] = xyz[:, 1]
    vertices["z"] = xyz[:, 2]
    vertices["scale_0"] = -4.0
    vertices["scale_1"] = -4.0
    vertices["opacity"] = 1.0
    path.parent.mkdir(parents=True, exist_ok=True)
    PlyData([PlyElement.describe(vertices, "vertex")]).write(path)


class TrainingBackendTests(unittest.TestCase):
    def test_form_items_reads_upload_fields_not_cgi_values(self):
        upload = FakeUploadItem("clip.mov", b"video")
        upload.value = b"video"
        form = FakeCgiForm({"files": [upload]})

        self.assertEqual(server.form_items(form, "files"), [upload])

    def test_video_extract_falls_back_to_conda_when_server_cv2_is_missing(self):
        with (
            mock.patch.object(server, "extract_video_frames_with_ffmpeg", side_effect=RuntimeError("no ffmpeg")),
            mock.patch.object(server, "extract_video_frames_with_cv2", side_effect=RuntimeError("no cv2")),
            mock.patch.object(server, "extract_video_frames_with_conda", return_value=7) as fallback,
        ):
            frames = server.extract_video_frames(Path("clip.mov"), Path("images"), 2)

        self.assertEqual(frames, 7)
        fallback.assert_called_once()

    def test_video_extract_prefers_ffmpeg(self):
        with tempfile.TemporaryDirectory() as tmp:
            images_dir = Path(tmp) / "images"
            images_dir.mkdir()
            video_path = Path(tmp) / "clip.mov"
            video_path.write_bytes(b"video")

            def fake_run(command, **_kwargs):
                output_pattern = command[-1]
                self.assertIn("-vf", command)
                self.assertEqual(command[command.index("-vf") + 1], "fps=2.000000")
                Path(output_pattern.replace("%06d", "000000")).write_bytes(b"frame")
                Path(output_pattern.replace("%06d", "000001")).write_bytes(b"frame")
                return types.SimpleNamespace(returncode=0, stdout="", stderr="")

            with (
                mock.patch.object(server, "ffmpeg_executable", return_value=Path("ffmpeg.exe")),
                mock.patch.object(server.subprocess, "run", side_effect=fake_run),
            ):
                self.assertEqual(server.extract_video_frames_with_ffmpeg(video_path, images_dir, 2), 2)

    def test_import_archives_image_original_and_preserves_browser_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            try:
                image_bytes = b"\xff\xd8\xff\xe1\x00\x10Exif\x00\x00metadata-payload\xff\xd9"
                form = FakeUploadForm({
                    "scene": "metadata_scene",
                    "overwrite": "false",
                    "fps": "2",
                    "files": [FakeUploadItem("nested/IMG_0001.JPG", image_bytes)],
                    "fileMetadata": json.dumps([{
                        "name": "IMG_0001.JPG",
                        "relativePath": "nested/IMG_0001.JPG",
                        "type": "image/jpeg",
                        "size": len(image_bytes),
                        "lastModified": 1710000000123,
                    }]),
                })

                result = server.save_uploaded_dataset(form)

                dataset = Path(result["path"])
                archived = dataset / "source" / "originals" / "IMG_0001.JPG"
                training_copy = dataset / "images" / "IMG_0001.JPG"
                manifest = json.loads((dataset / "source" / "metadata_manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(archived.read_bytes(), image_bytes)
                self.assertEqual(training_copy.read_bytes(), image_bytes)
                self.assertEqual(manifest["files"][0]["relativePath"], "nested/IMG_0001.JPG")
                self.assertEqual(manifest["files"][0]["lastModified"], 1710000000123)
                self.assertEqual(manifest["files"][0]["archivePath"], "source/originals/IMG_0001.JPG")
                self.assertEqual(manifest["files"][0]["trainingPath"], "images/IMG_0001.JPG")
            finally:
                server.DATASETS_DIR = original_datasets

    def test_import_extracts_video_frames_from_archived_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            try:
                video_bytes = b"video-bytes"
                form = FakeUploadForm({
                    "scene": "video_scene",
                    "overwrite": "false",
                    "fps": "2",
                    "files": [FakeUploadItem("clip.mov", video_bytes)],
                    "fileMetadata": json.dumps([{
                        "name": "clip.mov",
                        "type": "video/quicktime",
                        "size": len(video_bytes),
                        "lastModified": 1710000000456,
                    }]),
                })

                def fake_extract(video_path, images_dir, fps):
                    self.assertEqual(video_path, server.DATASETS_DIR / "video_scene" / "source" / "originals" / "clip.mov")
                    (images_dir / "clip_000000.jpg").write_bytes(b"frame")
                    return 1

                with mock.patch.object(server, "extract_video_frames", side_effect=fake_extract):
                    result = server.save_uploaded_dataset(form)

                dataset = Path(result["path"])
                self.assertEqual((dataset / "source" / "originals" / "clip.mov").read_bytes(), video_bytes)
                manifest = json.loads((dataset / "source" / "metadata_manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["files"][0]["kind"], "video")
                self.assertEqual(manifest["files"][0]["archivePath"], "source/originals/clip.mov")
                self.assertEqual(result["extracted_frames"], 1)
            finally:
                server.DATASETS_DIR = original_datasets

    def test_import_local_paths_archives_without_http_uploading_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            source = Path(tmp) / "source" / "clip.mov"
            source.parent.mkdir()
            source.write_bytes(b"video-bytes")
            try:
                def fake_extract(video_path, images_dir, fps):
                    self.assertEqual(video_path, server.DATASETS_DIR / "path_scene" / "source" / "originals" / "clip.mov")
                    (images_dir / "clip_000000.jpg").write_bytes(b"frame")
                    return 1

                with (
                    mock.patch.object(server, "archive_local_source", wraps=server.archive_local_source) as archive,
                    mock.patch.object(server, "extract_video_frames", side_effect=fake_extract),
                ):
                    result = server.save_path_dataset({
                        "scene": "path_scene",
                        "overwrite": True,
                        "fps": 2,
                        "files": [{
                            "path": str(source),
                            "name": "clip.mov",
                            "relativePath": "shoot/clip.mov",
                            "type": "video/quicktime",
                            "size": source.stat().st_size,
                            "lastModified": 1710000000456,
                        }],
                    })

                dataset = Path(result["path"])
                archived = dataset / "source" / "originals" / "clip.mov"
                self.assertEqual(archived.read_bytes(), b"video-bytes")
                self.assertEqual(result["saved_videos"], 1)
                self.assertEqual(result["extracted_frames"], 1)
                self.assertTrue(archive.called)
                manifest = json.loads((dataset / "source" / "metadata_manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["files"][0]["relativePath"], "shoot/clip.mov")
            finally:
                server.DATASETS_DIR = original_datasets

    def test_local_path_video_import_extracts_multiple_videos_with_parallel_workers(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            sources = []
            for name in ("a.mov", "b.mov", "c.mov"):
                source = Path(tmp) / "source" / name
                source.parent.mkdir(exist_ok=True)
                source.write_bytes(b"video")
                sources.append(source)
            try:
                worker_counts = []

                class RecordingExecutor:
                    def __init__(self, max_workers):
                        worker_counts.append(max_workers)

                    def __enter__(self):
                        return self

                    def __exit__(self, exc_type, exc, tb):
                        return False

                    def submit(self, fn, *args):
                        future = server.concurrent.futures.Future()
                        try:
                            future.set_result(fn(*args))
                        except Exception as exc:
                            future.set_exception(exc)
                        return future

                def fake_extract(video_path, images_dir, fps):
                    (images_dir / f"{video_path.stem}_000000.jpg").write_bytes(b"frame")
                    return 1

                with (
                    mock.patch.object(server, "archive_local_source", side_effect=lambda source, target: (shutil.copy2(source, target), "copy")[1]),
                    mock.patch.object(server, "extract_video_frames", side_effect=fake_extract),
                    mock.patch.object(server.concurrent.futures, "ThreadPoolExecutor", RecordingExecutor),
                ):
                    result = server.save_path_dataset({
                        "scene": "parallel_scene",
                        "overwrite": True,
                        "fps": 2,
                        "files": [{"path": str(source), "name": source.name} for source in sources],
                    })

                self.assertEqual(result["saved_videos"], 3)
                self.assertEqual(result["extracted_frames"], 3)
                self.assertEqual(worker_counts, [3])
            finally:
                server.DATASETS_DIR = original_datasets

    def test_local_path_import_progress_reports_extracted_video_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            source = Path(tmp) / "source" / "clip.mov"
            source.parent.mkdir()
            source.write_bytes(b"video")
            job_id = "progress-job"
            try:
                def fake_extract(video_path, images_dir, fps):
                    (images_dir / "clip_000000.jpg").write_bytes(b"frame")
                    (images_dir / "clip_000001.jpg").write_bytes(b"frame")
                    return 2

                with (
                    mock.patch.object(server, "archive_local_source", side_effect=lambda source, target: (shutil.copy2(source, target), "copy")[1]),
                    mock.patch.object(server, "extract_video_frames", side_effect=fake_extract),
                ):
                    server.save_path_dataset({
                        "scene": "progress_scene",
                        "overwrite": True,
                        "fps": 2,
                        "import_job_id": job_id,
                        "files": [{"path": str(source), "name": source.name}],
                    })

                status = server.import_job_snapshot(job_id)
                self.assertEqual(status["status"], "done")
                self.assertEqual(status["stage"], "done")
                self.assertEqual(status["total_files"], 1)
                self.assertEqual(status["processed_files"], 1)
                self.assertEqual(status["total_videos"], 1)
                self.assertEqual(status["processed_videos"], 1)
                self.assertEqual(status["extracted_frames"], 2)
                self.assertEqual(status["image_count"], 2)
            finally:
                server.DATASETS_DIR = original_datasets
                server.IMPORT_JOBS.pop(job_id, None)

    def test_colmap_options_default_match_existing_converter_behavior(self):
        options = server.colmap_options_from_payload({})

        self.assertEqual(options["matching"], "exhaustive")
        self.assertEqual(options["camera_model"], "OPENCV")
        self.assertTrue(options["use_gpu"])
        self.assertEqual(options["mapper_ba_global_function_tolerance"], "0.000001")
        self.assertEqual(options["mapper_ba_global_max_num_iterations"], 50)

    def test_colmap_options_robust_preset_limits_global_ba_and_relaxes_registration(self):
        options = server.colmap_options_from_payload({"preset": "robust"})

        self.assertEqual(options["matching"], "exhaustive")
        self.assertEqual(options["feature_max_image_size"], 2400)
        self.assertEqual(options["mapper_ba_global_max_num_iterations"], 15)
        self.assertEqual(options["mapper_ba_global_frames_freq"], 100)
        self.assertEqual(options["mapper_min_num_matches"], 10)
        self.assertEqual(options["mapper_abs_pose_min_num_inliers"], 20)

    def test_colmap_options_accept_manual_overrides(self):
        options = server.colmap_options_from_payload({
            "preset": "robust",
            "matching": "sequential",
            "camera_model": "PINHOLE",
            "feature_max_image_size": 1600,
            "feature_max_num_features": 6000,
            "sequential_overlap": 25,
            "mapper_ba_global_max_num_iterations": 8,
            "mapper_max_runtime_seconds": 1800,
            "use_gpu": False,
        })

        self.assertEqual(options["matching"], "sequential")
        self.assertEqual(options["camera_model"], "PINHOLE")
        self.assertEqual(options["feature_max_image_size"], 1600)
        self.assertEqual(options["feature_max_num_features"], 6000)
        self.assertEqual(options["sequential_overlap"], 25)
        self.assertEqual(options["mapper_ba_global_max_num_iterations"], 8)
        self.assertEqual(options["mapper_max_runtime_seconds"], 1800)
        self.assertFalse(options["use_gpu"])

    def test_colmap_convert_commands_include_manual_alignment_parameters(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            (dataset / "images").mkdir(parents=True)

            with mock.patch.object(server, "colmap_executable", return_value=Path("colmap.exe")):
                commands = server.colmap_convert_commands(dataset, server.colmap_options_from_payload({
                    "matching": "sequential",
                    "camera_model": "PINHOLE",
                    "feature_max_image_size": 1600,
                    "feature_max_num_features": 6000,
                    "sequential_overlap": 25,
                    "matcher_guided": True,
                    "mapper_ba_global_max_num_iterations": 8,
                    "mapper_ba_global_frames_freq": 120,
                    "mapper_min_num_matches": 12,
                }))

            feature, matcher, mapper, undistorter = commands
            self.assertEqual(feature[1], "feature_extractor")
            self.assertIn("--ImageReader.camera_model", feature)
            self.assertEqual(feature[feature.index("--ImageReader.camera_model") + 1], "PINHOLE")
            self.assertIn("--FeatureExtraction.max_image_size", feature)
            self.assertEqual(feature[feature.index("--FeatureExtraction.max_image_size") + 1], "1600")
            self.assertIn("--SiftExtraction.max_num_features", feature)
            self.assertEqual(feature[feature.index("--SiftExtraction.max_num_features") + 1], "6000")
            self.assertEqual(matcher[1], "sequential_matcher")
            self.assertIn("--SequentialMatching.overlap", matcher)
            self.assertEqual(matcher[matcher.index("--SequentialMatching.overlap") + 1], "25")
            self.assertIn("--FeatureMatching.guided_matching", matcher)
            self.assertEqual(matcher[matcher.index("--FeatureMatching.guided_matching") + 1], "1")
            self.assertIn("--Mapper.ba_global_max_num_iterations", mapper)
            self.assertEqual(mapper[mapper.index("--Mapper.ba_global_max_num_iterations") + 1], "8")
            self.assertIn("--Mapper.min_num_matches", mapper)
            self.assertEqual(mapper[mapper.index("--Mapper.min_num_matches") + 1], "12")
            self.assertEqual(undistorter[1], "image_undistorter")

    def test_colmap_reset_removes_database_cache_for_overwrite_retrain(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            for name in ("distorted", "sparse", "stereo"):
                (dataset / name).mkdir(parents=True)
                (dataset / name / "stale.txt").write_text("stale", encoding="utf-8")
            (dataset / "database.db").write_text("old database", encoding="utf-8")

            job = {"log": []}
            options = server.colmap_options_from_payload({"reset": True})
            with (
                mock.patch.object(server, "colmap_convert_commands", return_value=[]),
                mock.patch.object(server, "normalize_undistorted_sparse"),
            ):
                server.run_colmap_convert(job, dataset, options)

            self.assertFalse((dataset / "database.db").exists())
            self.assertFalse((dataset / "sparse" / "stale.txt").exists())
            self.assertTrue((dataset / "distorted" / "sparse").exists())

    def test_colmap_disables_single_camera_for_mixed_image_dimensions(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            captured_options = {}

            def capture_commands(_dataset, options):
                captured_options.update(options)
                return []

            job = {"log": []}
            options = server.colmap_options_from_payload({"single_camera": True, "reset": False})
            with (
                mock.patch.object(server, "dataset_has_mixed_image_dimensions", return_value=True),
                mock.patch.object(server, "colmap_convert_commands", side_effect=capture_commands),
                mock.patch.object(server, "normalize_undistorted_sparse"),
            ):
                server.run_colmap_convert(job, dataset, options)

            self.assertFalse(captured_options["single_camera"])
            self.assertTrue(any("mixed image dimensions" in line for line in job["log"]))

    def test_list_datasets_marks_cached_alignment_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            dataset = server.DATASETS_DIR / "aligned_scene"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            cache_sparse0 = dataset / ".alignment_cache" / "sparse" / "0"
            cache_sparse0.mkdir(parents=True)
            for name in ("cameras.bin", "images.bin", "points3D.bin"):
                (cache_sparse0 / name).write_bytes(b"cached")

            try:
                datasets = server.list_datasets()
            finally:
                server.DATASETS_DIR = original_datasets

            self.assertEqual(datasets[0]["name"], "aligned_scene")
            self.assertEqual(datasets[0]["image_count"], 1)
            self.assertTrue(datasets[0]["has_alignment"])

    def test_train_existing_dataset_allows_existing_output_without_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = server.DATASETS_DIR
            original_output = server.OUTPUT_DIR
            server.DATASETS_DIR = base / "datasets"
            server.OUTPUT_DIR = base / "output"
            dataset = server.DATASETS_DIR / "scene_existing"
            output = server.OUTPUT_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            output.mkdir(parents=True)
            (output / "keep.txt").write_text("existing result", encoding="utf-8")
            job = {
                "id": "job",
                "scene": "scene_existing",
                "output_scene": "scene_existing",
                "backend": "3dgs",
                "status": "queued",
                "stage": "queued",
                "error": None,
                "cancel_requested": False,
                "allow_existing_output": True,
                "colmap_options": server.colmap_options_from_payload({"reset": False}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(server, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(server, "run_logged"),
                    mock.patch.object(server, "run_colmap_convert"),
                    mock.patch.object(server, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(server, "write_training_metadata"),
                    mock.patch.object(server, "latest_iteration", return_value=1000),
                    mock.patch.object(server, "persist_train_job"),
                ):
                    server.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                server.DATASETS_DIR = original_datasets
                server.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            self.assertIsNone(job["error"])
            self.assertTrue((output / "keep.txt").exists())

    def test_training_rejects_cross_backend_output_reuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = server.DATASETS_DIR
            original_output = server.OUTPUT_DIR
            server.DATASETS_DIR = base / "datasets"
            server.OUTPUT_DIR = base / "output"
            dataset = server.DATASETS_DIR / "scene_existing"
            output = server.OUTPUT_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            output.mkdir(parents=True)
            server.write_training_metadata(output, "3dgs", "full", {"iterations": 30000, "resolution": 4})
            job = {
                "id": "job",
                "scene": "scene_existing",
                "output_scene": "scene_existing",
                "backend": "2dgs",
                "status": "queued",
                "stage": "queued",
                "error": None,
                "cancel_requested": False,
                "allow_existing_output": True,
                "colmap_options": server.colmap_options_from_payload({"reset": False}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(server, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                        "two_dgs_dir": "2dgs",
                    }),
                    mock.patch.object(server, "run_logged") as run_mock,
                    mock.patch.object(server, "persist_train_job"),
                ):
                    server.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                server.DATASETS_DIR = original_datasets
                server.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "failed")
            self.assertIn("belongs to 3DGS", job["error"])
            run_mock.assert_not_called()

    def test_train_existing_dataset_skips_colmap_when_requested(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = server.DATASETS_DIR
            original_output = server.OUTPUT_DIR
            server.DATASETS_DIR = base / "datasets"
            server.OUTPUT_DIR = base / "output"
            dataset = server.DATASETS_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            (dataset / "sparse" / "0").mkdir(parents=True)
            for name in ("cameras.bin", "images.bin", "points3D.bin"):
                (dataset / "sparse" / "0" / name).write_bytes(b"colmap")
            job = {
                "id": "job",
                "scene": "scene_existing",
                "output_scene": "scene_existing",
                "backend": "3dgs",
                "status": "queued",
                "stage": "queued",
                "error": None,
                "cancel_requested": False,
                "allow_existing_output": True,
                "colmap_options": server.colmap_options_from_payload({"reset": True}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(server, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(server, "run_logged"),
                    mock.patch.object(server, "run_colmap_convert") as colmap_mock,
                    mock.patch.object(server, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(server, "write_training_metadata"),
                    mock.patch.object(server, "latest_iteration", return_value=1000),
                    mock.patch.object(server, "persist_train_job"),
                ):
                    server.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                server.DATASETS_DIR = original_datasets
                server.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            colmap_mock.assert_not_called()

    def test_train_existing_dataset_runs_colmap_when_alignment_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = server.DATASETS_DIR
            original_output = server.OUTPUT_DIR
            server.DATASETS_DIR = base / "datasets"
            server.OUTPUT_DIR = base / "output"
            dataset = server.DATASETS_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            job = {
                "id": "job",
                "scene": "scene_existing",
                "output_scene": "scene_existing",
                "backend": "3dgs",
                "status": "queued",
                "stage": "queued",
                "error": None,
                "cancel_requested": False,
                "allow_existing_output": True,
                "colmap_options": server.colmap_options_from_payload({"reset": True}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(server, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(server, "run_logged"),
                    mock.patch.object(server, "run_colmap_convert") as colmap_mock,
                    mock.patch.object(server, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(server, "write_training_metadata"),
                    mock.patch.object(server, "latest_iteration", return_value=1000),
                    mock.patch.object(server, "persist_train_job"),
                ):
                    server.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                server.DATASETS_DIR = original_datasets
                server.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            colmap_mock.assert_called_once()
            self.assertTrue(any("running COLMAP alignment first" in line for line in job["log"]))

    def test_colmap_convert_saves_alignment_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            sparse0 = dataset / "sparse" / "0"
            sparse0.mkdir(parents=True)
            for name in ("cameras.bin", "images.bin", "points3D.bin"):
                (sparse0 / name).write_bytes(name.encode("ascii"))

            job = {"log": []}
            with (
                mock.patch.object(server, "colmap_convert_commands", return_value=[]),
                mock.patch.object(server, "normalize_undistorted_sparse"),
            ):
                server.run_colmap_convert(job, dataset, server.colmap_options_from_payload({"reset": False}))

            cache_sparse0 = dataset / ".alignment_cache" / "sparse" / "0"
            self.assertTrue((cache_sparse0 / "cameras.bin").exists())
            self.assertTrue((cache_sparse0 / "images.bin").exists())
            self.assertTrue((cache_sparse0 / "points3D.bin").exists())
            self.assertTrue((dataset / ".alignment_cache" / "metadata.json").exists())
            self.assertTrue(any("Saved COLMAP alignment cache" in line for line in job["log"]))

    def test_colmap_reset_preserves_alignment_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            for name in ("distorted", "sparse", "stereo"):
                (dataset / name).mkdir(parents=True)
            cache_sparse0 = dataset / ".alignment_cache" / "sparse" / "0"
            cache_sparse0.mkdir(parents=True)
            for name in ("cameras.bin", "images.bin", "points3D.bin"):
                (cache_sparse0 / name).write_bytes(b"cached")
                (dataset / "sparse" / name).write_bytes(b"stale")

            job = {"log": []}
            with (
                mock.patch.object(server, "colmap_convert_commands", return_value=[]),
                mock.patch.object(server, "normalize_undistorted_sparse"),
                mock.patch.object(server, "save_alignment_cache", return_value=False),
            ):
                server.run_colmap_convert(job, dataset, server.colmap_options_from_payload({"reset": True}))

            self.assertTrue((cache_sparse0 / "cameras.bin").exists())
            self.assertTrue((cache_sparse0 / "images.bin").exists())
            self.assertTrue((cache_sparse0 / "points3D.bin").exists())

    def test_train_existing_dataset_restores_alignment_cache_before_training(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = server.DATASETS_DIR
            original_output = server.OUTPUT_DIR
            server.DATASETS_DIR = base / "datasets"
            server.OUTPUT_DIR = base / "output"
            dataset = server.DATASETS_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            cache_sparse0 = dataset / ".alignment_cache" / "sparse" / "0"
            cache_sparse0.mkdir(parents=True)
            for name in ("cameras.bin", "images.bin", "points3D.bin"):
                (cache_sparse0 / name).write_bytes(b"cached")
            job = {
                "id": "job",
                "scene": "scene_existing",
                "output_scene": "scene_existing",
                "backend": "3dgs",
                "status": "queued",
                "stage": "queued",
                "error": None,
                "cancel_requested": False,
                "allow_existing_output": True,
                "colmap_options": server.colmap_options_from_payload({"reset": True}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(server, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(server, "run_logged"),
                    mock.patch.object(server, "run_colmap_convert") as colmap_mock,
                    mock.patch.object(server, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(server, "write_training_metadata"),
                    mock.patch.object(server, "latest_iteration", return_value=1000),
                    mock.patch.object(server, "persist_train_job"),
                ):
                    server.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                server.DATASETS_DIR = original_datasets
                server.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            colmap_mock.assert_not_called()
            self.assertTrue((dataset / "sparse" / "0" / "cameras.bin").exists())
            self.assertTrue(any("Restored cached COLMAP alignment" in line for line in job["log"]))

    def test_2dgs_quality_uses_supported_train_arguments(self):
        options = server.train_args_for_quality("quality", "2dgs")

        self.assertEqual(options["iterations"], 30000)
        self.assertEqual(options["resolution"], 4)
        self.assertEqual(options["depth_ratio"], 0.0)
        self.assertNotIn("antialiasing", options)

    def test_3dgs_max_quality_profile_enables_quality_oriented_training(self):
        options = server.train_args_for_quality("max_quality", "3dgs")

        self.assertEqual(options["iterations"], 30000)
        self.assertEqual(options["resolution"], 2)
        self.assertTrue(options["antialiasing"])
        self.assertEqual(options["optimizer_type"], "sparse_adam")
        self.assertGreater(options["densify_until_iter"], 15000)
        self.assertLess(options["densify_grad_threshold"], 0.0002)

    def test_training_options_accept_safe_overrides_for_3dgs(self):
        options = server.training_options_from_payload("3dgs", "quality", {
            "iterations": 42000,
            "resolution": 1,
            "optimizer_type": "sparse_adam",
            "antialiasing": False,
            "exposure_compensation": True,
            "densify_grad_threshold": 0.00008,
            "densification_interval": 80,
            "densify_until_iter": 24000,
        })

        self.assertEqual(options["iterations"], 42000)
        self.assertEqual(options["resolution"], 1)
        self.assertEqual(options["optimizer_type"], "sparse_adam")
        self.assertFalse(options["antialiasing"])
        self.assertTrue(options["exposure_compensation"])
        self.assertEqual(options["densify_grad_threshold"], 0.00008)
        self.assertEqual(options["densification_interval"], 80)
        self.assertEqual(options["densify_until_iter"], 24000)

    def test_3dgs_training_command_includes_advanced_quality_flags(self):
        options = server.training_options_from_payload("3dgs", "max_quality", {
            "exposure_compensation": True,
        })

        command = server.training_command("3dgs", "dataset", "output", options)

        self.assertIn("--antialiasing", command)
        self.assertIn("--optimizer_type", command)
        self.assertEqual(command[command.index("--optimizer_type") + 1], "sparse_adam")
        self.assertIn("--densify_grad_threshold", command)
        self.assertIn("--densification_interval", command)
        self.assertIn("--densify_until_iter", command)
        self.assertIn("--exposure_lr_init", command)
        self.assertEqual(command[command.index("--exposure_lr_init") + 1], "0.001")

    def test_sparse_adam_falls_back_when_accelerated_rasterizer_is_missing(self):
        options = server.training_options_from_payload("3dgs", "quality", {
            "optimizer_type": "sparse_adam",
        })
        job = {"log": [], "updated_at": 1.0}

        with mock.patch.object(server, "sparse_adam_available", return_value=False):
            resolved = server.resolve_training_options_for_environment("3dgs", options, job)

        command = server.training_command("3dgs", "dataset", "output", resolved)
        self.assertEqual(resolved["optimizer_type"], "default")
        self.assertNotIn("--optimizer_type", command)
        self.assertTrue(any("sparse_adam" in line and "default Adam" in line for line in job["log"]))

    def test_sparse_adam_is_kept_when_accelerated_rasterizer_is_available(self):
        options = server.training_options_from_payload("3dgs", "quality", {
            "optimizer_type": "sparse_adam",
        })
        job = {"log": [], "updated_at": 1.0}

        with mock.patch.object(server, "sparse_adam_available", return_value=True):
            resolved = server.resolve_training_options_for_environment("3dgs", options, job)

        command = server.training_command("3dgs", "dataset", "output", resolved)
        self.assertEqual(resolved["optimizer_type"], "sparse_adam")
        self.assertIn("--optimizer_type", command)
        self.assertEqual(command[command.index("--optimizer_type") + 1], "sparse_adam")

    def test_training_metadata_records_profile_and_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "model"
            options = server.training_options_from_payload("3dgs", "quality", {
                "optimizer_type": "sparse_adam",
            })

            server.write_training_metadata(output, "3dgs", "quality", options)

            backend_metadata = json.loads((output / "training_backend.json").read_text(encoding="utf-8"))
            scene_metadata = json.loads((output / "scene.json").read_text(encoding="utf-8"))
            self.assertEqual(backend_metadata["backend"], "3dgs")
            self.assertEqual(backend_metadata["quality"], "quality")
            self.assertEqual(scene_metadata["training"]["backend"], "3dgs")
            self.assertEqual(scene_metadata["training"]["quality"], "quality")
            self.assertEqual(scene_metadata["training"]["options"]["optimizer_type"], "sparse_adam")

    def test_training_job_persistence_round_trips_without_process_handle(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = server.TRAIN_JOBS_DIR
            server.TRAIN_JOBS_DIR = Path(tmp) / "training"
            try:
                job = {
                    "id": "job-1",
                    "scene": "scene_a",
                    "output_scene": "scene_a",
                    "backend": "3dgs",
                    "status": "queued",
                    "stage": "queued",
                    "created_at": 1.0,
                    "updated_at": 1.0,
                    "returncode": None,
                    "error": None,
                    "cancel_requested": False,
                    "process": object(),
                    "colmap_options": {"preset": "default"},
                    "train_options": {"iterations": 7000},
                    "log": ["created"],
                }

                server.persist_train_job(job)
                loaded = server.load_persisted_train_jobs()

                self.assertIn("job-1", loaded)
                self.assertEqual(loaded["job-1"]["scene"], "scene_a")
                self.assertIsNone(loaded["job-1"]["process"])
                self.assertEqual(loaded["job-1"]["train_options"]["iterations"], 7000)
            finally:
                server.TRAIN_JOBS_DIR = original_jobs_dir

    def test_training_job_persistence_retries_transient_access_denied(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = server.TRAIN_JOBS_DIR
            server.TRAIN_JOBS_DIR = Path(tmp) / "training"
            original_replace = Path.replace
            calls = {"replace": 0}

            def flaky_replace(path, target):
                calls["replace"] += 1
                if calls["replace"] == 1:
                    raise PermissionError("temporarily locked")
                return original_replace(path, target)

            try:
                job = {
                    "id": "job-locked-once",
                    "scene": "scene_a",
                    "output_scene": "scene_a",
                    "backend": "3dgs",
                    "status": "running",
                    "stage": "colmap",
                    "created_at": 1.0,
                    "updated_at": 1.0,
                    "log": ["line"],
                }

                with mock.patch.object(Path, "replace", flaky_replace), mock.patch.object(server.time, "sleep"):
                    self.assertTrue(server.persist_train_job(job))

                loaded = server.load_persisted_train_jobs()
                self.assertIn("job-locked-once", loaded)
                self.assertGreaterEqual(calls["replace"], 2)
                self.assertNotIn("_persist_error", job)
            finally:
                server.TRAIN_JOBS_DIR = original_jobs_dir

    def test_training_job_persistence_failure_does_not_raise(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = server.TRAIN_JOBS_DIR
            server.TRAIN_JOBS_DIR = Path(tmp) / "training"
            try:
                job = {
                    "id": "job-locked",
                    "scene": "scene_a",
                    "output_scene": "scene_a",
                    "backend": "3dgs",
                    "status": "running",
                    "stage": "colmap",
                    "created_at": 1.0,
                    "updated_at": 1.0,
                    "log": ["line"],
                }

                with mock.patch.object(Path, "replace", side_effect=PermissionError("locked")), mock.patch.object(server.time, "sleep"):
                    self.assertFalse(server.persist_train_job(job))

                self.assertIn("_persist_error", job)
            finally:
                server.TRAIN_JOBS_DIR = original_jobs_dir

    def test_run_logged_terminates_child_when_log_write_fails(self):
        class LoggingProcess:
            def __init__(self):
                self.stdout = iter(["line\n"])
                self.terminated = False
                self.killed = False

            def poll(self):
                return None

            def terminate(self):
                self.terminated = True

            def kill(self):
                self.killed = True

            def wait(self, timeout=None):
                return 0

        process = LoggingProcess()
        job = {"backend": "3dgs", "process": None, "cancel_requested": False}

        with (
            mock.patch.object(server.subprocess, "Popen", return_value=process),
            mock.patch.object(server, "training_env", return_value={}),
            mock.patch.object(server, "add_job_log", side_effect=[None, RuntimeError("log write failed")]),
        ):
            with self.assertRaisesRegex(RuntimeError, "log write failed"):
                server.run_logged(job, ["cmd"], Path("."))

        self.assertTrue(process.terminated)
        self.assertIsNone(job["process"])

    def test_shutdown_active_jobs_cancels_training_and_mesh_processes(self):
        class RunningProcess:
            def __init__(self):
                self.terminated = False
                self.killed = False

            def poll(self):
                return None

            def terminate(self):
                self.terminated = True

            def kill(self):
                self.killed = True

            def wait(self, timeout=None):
                return 0

        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = server.TRAIN_JOBS_DIR
            original_train_jobs = server.TRAIN_JOBS
            original_mesh_jobs = server.MESH_JOBS
            train_process = RunningProcess()
            mesh_process = RunningProcess()
            server.TRAIN_JOBS_DIR = Path(tmp) / "training"
            server.TRAIN_JOBS = {
                "train": {
                    "id": "train",
                    "scene": "scene_a",
                    "output_scene": "scene_a",
                    "backend": "3dgs",
                    "status": "running",
                    "stage": "colmap",
                    "created_at": 1.0,
                    "updated_at": 1.0,
                    "process": train_process,
                    "cancel_requested": False,
                    "log": [],
                }
            }
            server.MESH_JOBS = {
                "mesh": {
                    "id": "mesh",
                    "scene": "scene_a",
                    "iteration": 1,
                    "mode": "bounded",
                    "status": "running",
                    "stage": "mesh",
                    "created_at": 1.0,
                    "updated_at": 1.0,
                    "process": mesh_process,
                    "cancel_requested": False,
                    "log": [],
                }
            }
            try:
                stopped = server.shutdown_active_jobs("test shutdown")

                self.assertEqual(stopped, {"training": 1, "mesh": 1})
                self.assertTrue(train_process.terminated)
                self.assertTrue(mesh_process.terminated)
                self.assertEqual(server.TRAIN_JOBS["train"]["status"], "cancelling")
                self.assertEqual(server.MESH_JOBS["mesh"]["status"], "cancelling")
                self.assertTrue(server.TRAIN_JOBS["train"]["cancel_requested"])
                self.assertTrue(server.MESH_JOBS["mesh"]["cancel_requested"])
            finally:
                server.TRAIN_JOBS_DIR = original_jobs_dir
                server.TRAIN_JOBS = original_train_jobs
                server.MESH_JOBS = original_mesh_jobs

    def test_uploaded_masks_are_applied_as_training_image_alpha(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow is required for mask alpha tests")
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = server.DATASETS_DIR
            server.DATASETS_DIR = Path(tmp) / "datasets"
            try:
                image_io = io.BytesIO()
                Image.new("RGB", (2, 2), (10, 20, 30)).save(image_io, format="JPEG")
                mask_io = io.BytesIO()
                mask = Image.new("L", (2, 2), 0)
                mask.putpixel((1, 1), 255)
                mask.save(mask_io, format="PNG")
                form = FakeUploadForm({
                    "scene": "masked_scene",
                    "overwrite": "false",
                    "files": [FakeUploadItem("IMG_0001.JPG", image_io.getvalue())],
                    "maskFiles": [FakeUploadItem("IMG_0001_mask.png", mask_io.getvalue())],
                })

                result = server.save_uploaded_dataset(form)

                self.assertEqual(result["mask_count"], 1)
                self.assertEqual(result["applied_masks"], 1)
                alpha_image = Image.open(server.DATASETS_DIR / "masked_scene" / "images" / "IMG_0001.png")
                self.assertEqual(alpha_image.mode, "RGBA")
                self.assertEqual(alpha_image.getchannel("A").getpixel((0, 0)), 0)
                self.assertEqual(alpha_image.getchannel("A").getpixel((1, 1)), 255)
            finally:
                server.DATASETS_DIR = original_datasets

    def test_splat_export_uses_splat_transform_for_spz_and_sog(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp) / "output"
            try:
                model = server.OUTPUT_DIR / "scene_splat" / "point_cloud" / "iteration_7"
                write_test_ply(model / "point_cloud.ply", np.array([[0.0, 0.0, 0.0]], dtype=np.float32))
                with mock.patch.object(server, "splat_transform_executable", return_value=Path("splat-transform.cmd")):
                    with mock.patch.object(server.subprocess, "run") as run:
                        run.return_value.returncode = 0
                        spz = server.export_splat_format("scene_splat", 7, "spz")
                        sog = server.export_splat_format("scene_splat", 7, "sog")

                self.assertEqual(spz.suffix, ".spz")
                self.assertEqual(sog.suffix, ".sog")
                self.assertEqual(run.call_count, 2)
                first_command = [str(part) for part in run.call_args_list[0].args[0]]
                self.assertIn("-w", first_command)
                self.assertTrue(first_command[-1].endswith("point_cloud.spz"))
            finally:
                server.OUTPUT_DIR = original_output

    def test_import_splat_scene_converts_spz_to_point_cloud_ply(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp) / "output"
            try:
                def fake_run(command, cwd=None, capture_output=False, text=False):
                    Path(command[-1]).parent.mkdir(parents=True, exist_ok=True)
                    write_test_ply(Path(command[-1]), np.array([[1.0, 2.0, 3.0]], dtype=np.float32))
                    return mock.Mock(returncode=0, stdout="", stderr="")

                upload = FakeUploadItem("scan.spz", b"spz-bytes")
                with mock.patch.object(server, "splat_transform_executable", return_value=Path("splat-transform.cmd")):
                    with mock.patch.object(server.subprocess, "run", side_effect=fake_run):
                        result = server.import_splat_scene(upload, "imported_spz", overwrite=False)

                self.assertEqual(result["scene"], "imported_spz")
                self.assertEqual(result["format"], "spz")
                self.assertTrue((server.OUTPUT_DIR / "imported_spz" / "point_cloud" / "iteration_0" / "point_cloud.ply").exists())
            finally:
                server.OUTPUT_DIR = original_output

    def test_mesh_job_cancel_marks_request_and_terminates_process(self):
        process = FakeProcess()
        job = {
            "id": "mesh-job-1",
            "kind": "mesh",
            "scene": "scene_2dgs",
            "iteration": 7,
            "mode": "bounded",
            "status": "running",
            "stage": "mesh",
            "created_at": 1.0,
            "updated_at": 1.0,
            "returncode": None,
            "error": None,
            "cancel_requested": False,
            "process": process,
            "output_mesh": None,
            "download_url": None,
            "log": [],
        }
        with server.MESH_LOCK:
            server.MESH_JOBS[job["id"]] = job
        try:
            snapshot = server.cancel_mesh_job(job["id"])

            self.assertTrue(process.terminated)
            self.assertTrue(snapshot["cancel_requested"])
            self.assertEqual(snapshot["status"], "cancelling")
            self.assertIn("Cancel requested.", snapshot["log"])
        finally:
            with server.MESH_LOCK:
                server.MESH_JOBS.pop(job["id"], None)

    def test_2dgs_environment_report_points_to_local_training_env(self):
        report = server.training_environment_report("2dgs")

        self.assertEqual(report["backend"], "2dgs")
        self.assertTrue(report["two_dgs_dir"].endswith("Documents\\2dgs"))
        self.assertTrue(report["two_dgs_python"].endswith(".venv\\Scripts\\python.exe"))
        self.assertTrue(report["two_dgs_train"].endswith("train.py"))

    def test_point_payload_reports_2dgs_backend_and_robust_view_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                core = np.column_stack([
                    np.linspace(-1.0, 1.0, 200, dtype=np.float32),
                    np.linspace(-0.5, 0.5, 200, dtype=np.float32),
                    np.linspace(-0.25, 0.25, 200, dtype=np.float32),
                ])
                xyz = np.vstack([core, np.array([[1000.0, 0.0, 0.0]], dtype=np.float32)])
                write_test_ply(model / "point_cloud" / "iteration_7" / "point_cloud.ply", xyz)

                payload = server.point_payload("scene_2dgs", 7)
                header_len = int.from_bytes(payload[:4], "little")
                header = json.loads(payload[4:4 + header_len])

                self.assertEqual(header["backend"], "2dgs")
                self.assertGreater(header["bounds"]["max"][0], 900.0)
                self.assertLess(header["view_bounds"]["max"][0], 100.0)
            finally:
                server.OUTPUT_DIR = original_output

    def test_point_payload_filters_non_finite_2dgs_vertices(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                xyz = np.array([
                    [0.0, 0.0, 0.0],
                    [np.nan, 1.0, 0.0],
                    [1.0, np.inf, 0.0],
                    [2.0, 0.0, 0.0],
                ], dtype=np.float32)
                write_test_ply(model / "point_cloud" / "iteration_7" / "point_cloud.ply", xyz)

                payload = server.point_payload("scene_2dgs", 7)
                header_len = int.from_bytes(payload[:4], "little")
                header_bytes = payload[4:4 + header_len]
                header = json.loads(header_bytes)

                self.assertNotIn(b"NaN", header_bytes)
                self.assertNotIn(b"Infinity", header_bytes)
                self.assertEqual(header["count"], 2)
                self.assertEqual(header["bounds"]["min"], [0.0, 0.0, 0.0])
                self.assertEqual(header["bounds"]["max"], [2.0, 0.0, 0.0])
            finally:
                server.OUTPUT_DIR = original_output

    def test_save_cropped_preserves_2dgs_backend_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "source_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                xyz = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [2.0, 0.0, 0.0]], dtype=np.float32)
                write_test_ply(model / "point_cloud" / "iteration_7" / "point_cloud.ply", xyz)

                server.save_cropped("source_2dgs", 7, "cropped_2dgs", [2])

                metadata = json.loads((server.OUTPUT_DIR / "cropped_2dgs" / "training_backend.json").read_text(encoding="utf-8"))
                self.assertEqual(metadata["backend"], "2dgs")
            finally:
                server.OUTPUT_DIR = original_output

    def test_save_trimmed_mesh_registers_mesh_under_derived_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                src_model = server.OUTPUT_DIR / "source_2dgs"
                src_model.mkdir(parents=True, exist_ok=True)
                (src_model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (src_model / "cfg_args").write_text("Namespace(source_path=r'C:\\\\datasets\\\\source_2dgs')", encoding="utf-8")
                point_cloud = src_model / "point_cloud" / "iteration_7" / "point_cloud.ply"
                write_test_ply(point_cloud, np.array([[0.0, 0.0, 0.0]], dtype=np.float32))
                trimmed_ply = "\n".join([
                    "ply",
                    "format ascii 1.0",
                    "element vertex 3",
                    "property float x",
                    "property float y",
                    "property float z",
                    "element face 1",
                    "property list uchar int vertex_indices",
                    "end_header",
                    "0 0 0",
                    "1 0 0",
                    "0 1 0",
                    "3 0 1 2",
                    "",
                ])

                result = server.save_trimmed_mesh(
                    "source_2dgs",
                    7,
                    "source_2dgs_bounded_trimmed",
                    "bounded",
                    trimmed_ply,
                    overwrite=False,
                )

                dst_model = server.OUTPUT_DIR / "source_2dgs_bounded_trimmed"
                self.assertEqual(result["output_scene"], "source_2dgs_bounded_trimmed")
                self.assertEqual(result["mesh_path"], str(dst_model / "train" / "ours_7" / "fuse_post.ply"))
                self.assertTrue((dst_model / "point_cloud" / "iteration_7" / "point_cloud.ply").exists())
                self.assertTrue((dst_model / "train" / "ours_7" / "fuse_post.ply").exists())
                self.assertEqual(json.loads((dst_model / "training_backend.json").read_text(encoding="utf-8"))["backend"], "2dgs")
                self.assertEqual(server.latest_iteration("source_2dgs_bounded_trimmed"), 7)
                self.assertTrue(server.mesh_asset_payload("source_2dgs_bounded_trimmed", 7)["meshes"]["bounded"]["post"]["exists"])
            finally:
                server.OUTPUT_DIR = original_output

    def test_save_trimmed_mesh_removes_unreferenced_vertices(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                src_model = server.OUTPUT_DIR / "source_2dgs"
                src_model.mkdir(parents=True, exist_ok=True)
                (src_model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                write_test_ply(src_model / "point_cloud" / "iteration_7" / "point_cloud.ply", np.array([[0.0, 0.0, 0.0]], dtype=np.float32))
                sparse_ply = "\n".join([
                    "ply",
                    "format ascii 1.0",
                    "element vertex 4",
                    "property float x",
                    "property float y",
                    "property float z",
                    "element face 1",
                    "property list uchar int vertex_indices",
                    "end_header",
                    "0 0 0",
                    "10 0 0",
                    "0 10 0",
                    "9 9 9",
                    "3 1 3 2",
                    "",
                ])

                server.save_trimmed_mesh("source_2dgs", 7, "trimmed", "bounded", sparse_ply, overwrite=False)

                mesh = PlyData.read(server.OUTPUT_DIR / "trimmed" / "train" / "ours_7" / "fuse_post.ply")
                self.assertEqual(len(mesh["vertex"].data), 3)
                self.assertEqual(mesh["face"].data[0][0].tolist(), [0, 1, 2])
            finally:
                server.OUTPUT_DIR = original_output

    def test_2dgs_mesh_command_uses_render_script_and_expected_output_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text("Namespace(source_path=r'C:\\\\datasets\\\\mesh_scene', model_path='ignored', depth_ratio=0.0)", encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, cwd, output_path = server.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {
                        "mode": "unbounded",
                        "mesh_res": 512,
                        "num_cluster": 12,
                        "depth_ratio": 0.0,
                    },
                )

                self.assertEqual(cwd, server.TWO_DGS_DIR)
                self.assertEqual(command[1], "render.py")
                self.assertIn("--unbounded", [str(part) for part in command])
                self.assertIn("--skip_train", [str(part) for part in command])
                self.assertIn("--skip_test", [str(part) for part in command])
                self.assertIn("512", [str(part) for part in command])
                self.assertEqual(output_path, model / "train" / "ours_7000" / "fuse_unbounded_post.ply")
            finally:
                server.OUTPUT_DIR = original_output

    def test_mesh_export_rejects_non_2dgs_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")

                with self.assertRaisesRegex(ValueError, "2DGS"):
                    server.mesh_export_command("mesh_scene", 7000, {"mode": "bounded"})
            finally:
                server.OUTPUT_DIR = original_output

    def test_sugar_mesh_command_uses_existing_3dgs_checkpoint(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            original_datasets = server.DATASETS_DIR
            original_sugar = server.SUGAR_DIR
            server.OUTPUT_DIR = Path(tmp) / "output"
            server.DATASETS_DIR = Path(tmp) / "datasets"
            server.SUGAR_DIR = Path(tmp) / "SuGaR"
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                dataset = server.DATASETS_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (dataset / "images").mkdir(parents=True, exist_ok=True)
                (dataset / "sparse" / "0").mkdir(parents=True, exist_ok=True)
                from PIL import Image
                Image.new("RGB", (101, 50), color=(120, 80, 30)).save(dataset / "images" / "frame.jpg")
                (dataset / "sparse" / "0" / "points3D.ply").write_text("ply\n", encoding="utf-8")
                server.SUGAR_DIR.mkdir(parents=True, exist_ok=True)
                (server.SUGAR_DIR / "train.py").write_text("print('sugar')", encoding="utf-8")
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                (model / "cameras.json").write_text(json.dumps([{
                    "id": 1,
                    "img_name": "frame.jpg",
                    "width": 101,
                    "height": 50,
                    "fx": 80.0,
                    "fy": 40.0,
                }]), encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, cwd, output_path = server.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {"mode": "sugar", "sugar_quality": "high"},
                )

                self.assertEqual(cwd, server.SUGAR_DIR)
                self.assertIn("train.py", [str(part) for part in command])
                self.assertIn("-c", [str(part) for part in command])
                stage = model / "_sugar_input"
                self.assertEqual(command[command.index("-c") + 1], f"{stage.as_posix()}/")
                staged_source = model / "_sugar_source" / "mesh_scene"
                self.assertEqual(command[command.index("-s") + 1], staged_source.as_posix())
                adapted_cameras = json.loads((stage / "cameras.json").read_text(encoding="utf-8"))
                self.assertEqual(adapted_cameras[0]["img_name"], "frame")
                self.assertEqual(adapted_cameras[0]["width"], 101)
                self.assertEqual(adapted_cameras[0]["height"], 50)
                self.assertTrue((staged_source / "images" / "frame.jpg").exists())
                self.assertTrue((staged_source / "sparse" / "0" / "points3D.ply").exists())
                self.assertTrue((stage / "point_cloud" / "iteration_7000" / "point_cloud.ply").exists())
                self.assertIn("-i", [str(part) for part in command])
                self.assertIn("7000", [str(part) for part in command])
                self.assertIn("--high_poly", [str(part) for part in command])
                self.assertIn("--eval", [str(part) for part in command])
                self.assertIn("False", [str(part) for part in command])
                self.assertEqual(output_path, model / "train" / "ours_7000" / "sugar_mesh_post.ply")
            finally:
                server.OUTPUT_DIR = original_output
                server.DATASETS_DIR = original_datasets
                server.SUGAR_DIR = original_sugar

    def test_sugar_mesh_command_honors_quality_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            original_datasets = server.DATASETS_DIR
            original_sugar = server.SUGAR_DIR
            server.OUTPUT_DIR = Path(tmp) / "output"
            server.DATASETS_DIR = Path(tmp) / "datasets"
            server.SUGAR_DIR = Path(tmp) / "SuGaR"
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                dataset = server.DATASETS_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (dataset / "images").mkdir(parents=True, exist_ok=True)
                from PIL import Image
                Image.new("RGB", (64, 64), color=(120, 80, 30)).save(dataset / "images" / "frame.jpg")
                server.SUGAR_DIR.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                (model / "cameras.json").write_text(json.dumps([{
                    "id": 1,
                    "img_name": "frame.jpg",
                    "width": 64,
                    "height": 64,
                    "fx": 50.0,
                    "fy": 50.0,
                }]), encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_30000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, _cwd, _output_path = server.mesh_export_command(
                    "mesh_scene",
                    30000,
                    {
                        "mode": "sugar",
                        "sugar_quality": "low",
                        "sugar_refinement_time": "short",
                        "sugar_regularization": "density",
                        "sugar_surface_level": 0.2,
                        "sugar_square_size": 8,
                        "sugar_postprocess": True,
                    },
                )

                command_text = [str(part) for part in command]
                self.assertIn("--low_poly", command_text)
                self.assertNotIn("--high_poly", command_text)
                self.assertEqual(command_text[command_text.index("-r") + 1], "density")
                self.assertEqual(command_text[command_text.index("-l") + 1], "0.2")
                self.assertEqual(command_text[command_text.index("--square_size") + 1], "8")
                self.assertEqual(command_text[command_text.index("--refinement_time") + 1], "short")
                self.assertIn("--postprocess_mesh", command_text)
            finally:
                server.OUTPUT_DIR = original_output
                server.DATASETS_DIR = original_datasets
                server.SUGAR_DIR = original_sugar

    def test_sugar_staging_normalizes_mixed_image_dimensions(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            model = base / "output" / "mesh_scene"
            dataset = base / "datasets" / "mesh_scene"
            (model / "point_cloud" / "iteration_7").mkdir(parents=True)
            (dataset / "images").mkdir(parents=True)
            from PIL import Image
            Image.new("RGB", (101, 50), color=(255, 0, 0)).save(dataset / "images" / "a.jpg")
            Image.new("RGB", (99, 51), color=(0, 255, 0)).save(dataset / "images" / "b.jpg")
            write_test_ply(model / "point_cloud" / "iteration_7" / "point_cloud.ply", np.array([[0.0, 0.0, 0.0]], dtype=np.float32))
            cameras = [
                {"id": 1, "img_name": "a.jpg", "width": 101, "height": 50, "fx": 80.0, "fy": 40.0},
                {"id": 2, "img_name": "b.jpg", "width": 99, "height": 51, "fx": 77.0, "fy": 39.0},
            ]
            (model / "cameras.json").write_text(json.dumps(cameras), encoding="utf-8")

            stage, staged_source = server.prepare_sugar_checkpoint(model, dataset, 7)

            adapted = json.loads((stage / "cameras.json").read_text(encoding="utf-8"))
            self.assertEqual(adapted[0]["width"], 101)
            self.assertEqual(adapted[0]["height"], 50)
            self.assertEqual(adapted[1]["width"], 101)
            self.assertEqual(adapted[1]["height"], 50)
            self.assertAlmostEqual(adapted[1]["fx"], 77.0 * 101 / 99)
            self.assertAlmostEqual(adapted[1]["fy"], 39.0 * 50 / 51)
            with Image.open(staged_source / "images" / "a.jpg") as image_a:
                self.assertEqual(image_a.size, (101, 50))
            with Image.open(staged_source / "images" / "b.jpg") as image_b:
                self.assertEqual(image_b.size, (101, 50))

    def test_sugar_staging_can_limit_images_and_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            model = base / "output" / "mesh_scene"
            dataset = base / "datasets" / "mesh_scene"
            (model / "point_cloud" / "iteration_7").mkdir(parents=True)
            (dataset / "images").mkdir(parents=True)
            from PIL import Image
            cameras = []
            for index in range(5):
                name = f"frame_{index}.jpg"
                Image.new("RGB", (100, 80), color=(index, 0, 0)).save(dataset / "images" / name)
                cameras.append({
                    "id": index,
                    "img_name": name,
                    "width": 100,
                    "height": 80,
                    "fx": 75.0,
                    "fy": 60.0,
                })
            write_test_ply(model / "point_cloud" / "iteration_7" / "point_cloud.ply", np.array([[0.0, 0.0, 0.0]], dtype=np.float32))
            (model / "cameras.json").write_text(json.dumps(cameras), encoding="utf-8")

            stage, staged_source = server.prepare_sugar_checkpoint(
                model,
                dataset,
                7,
                {"sugar_max_images": 3, "sugar_max_image_size": 50},
            )

            adapted = json.loads((stage / "cameras.json").read_text(encoding="utf-8"))
            self.assertEqual(len(adapted), 3)
            self.assertEqual([camera["img_name"] for camera in adapted], ["frame_0", "frame_2", "frame_4"])
            self.assertEqual(adapted[0]["width"], 50)
            self.assertEqual(adapted[0]["height"], 40)
            self.assertEqual(adapted[0]["fx"], 37.5)
            with Image.open(staged_source / "images" / "frame_0.jpg") as staged:
                self.assertEqual(staged.size, (50, 40))

    def test_sugar_mesh_export_rejects_2dgs_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")

                with self.assertRaisesRegex(ValueError, "3DGS"):
                    server.mesh_export_command("mesh_scene", 7000, {"mode": "sugar"})
            finally:
                server.OUTPUT_DIR = original_output

    def test_gs2mesh_mesh_command_uses_existing_3dgs_checkpoint_and_colmap(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            original_datasets = server.DATASETS_DIR
            original_gs2mesh = getattr(server, "GS2MESH_DIR", None)
            server.OUTPUT_DIR = Path(tmp) / "output"
            server.DATASETS_DIR = Path(tmp) / "datasets"
            server.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                dataset = server.DATASETS_DIR / "mesh_scene"
                sparse = dataset / "sparse" / "0"
                model.mkdir(parents=True, exist_ok=True)
                (dataset / "images").mkdir(parents=True, exist_ok=True)
                sparse.mkdir(parents=True, exist_ok=True)
                (dataset / "images" / "frame.jpg").write_bytes(b"image")
                (sparse / "cameras.txt").write_text(
                    "# colmap text\n1 PINHOLE 4000 2000 3000 3100 2000 1000\n",
                    encoding="utf-8",
                )
                (sparse / "images.txt").write_text("# colmap text\n", encoding="utf-8")
                (sparse / "points3D.txt").write_text("# colmap text\n", encoding="utf-8")
                server.GS2MESH_DIR.mkdir(parents=True, exist_ok=True)
                (server.GS2MESH_DIR / "run_single.py").write_text("print('gs2mesh')", encoding="utf-8")
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                (model / "cameras.json").write_text("[]", encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, cwd, output_path = server.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {
                        "mode": "gs2mesh",
                        "gs2mesh_downsample": 2,
                        "gs2mesh_baseline_percentage": 7,
                        "gs2mesh_tsdf_voxel": 2,
                        "gs2mesh_tsdf_min_depth_baselines": 4,
                        "gs2mesh_tsdf_max_depth_baselines": 20,
                    },
                )

                command_text = [str(part) for part in command]
                self.assertEqual(cwd, server.GS2MESH_DIR)
                self.assertIn("run_single.py", command_text)
                self.assertEqual(command_text[command_text.index("--experiment_folder_name") + 1], "app")
                self.assertEqual(command_text[command_text.index("--renderer_folder_name") + 1], "mesh_scene_d2")
                self.assertIn("--skip_video_extraction", command_text)
                self.assertIn("--skip_colmap", command_text)
                self.assertIn("--skip_GS", command_text)
                self.assertNotIn("--skip_rendering", command_text)
                self.assertEqual(command_text[command_text.index("--GS_iterations") + 1], "7000")
                self.assertEqual(command_text[command_text.index("--colmap_name") + 1], "mesh_scene")
                self.assertNotIn("--renderer_max_images", command_text)
                self.assertEqual(command_text[command_text.index("--downsample") + 1], "2")
                staged_dataset = server.GS2MESH_DIR / "data" / "custom" / "mesh_scene"
                staged_downsampled = server.GS2MESH_DIR / "data" / "custom" / "mesh_scene_downsample2"
                staged_model = server.GS2MESH_DIR / "splatting_output" / "custom_nw_iterations7000" / "mesh_scene"
                staged_downsampled_model = server.GS2MESH_DIR / "splatting_output" / "custom_nw_iterations7000" / "mesh_scene_downsample2"
                self.assertTrue((staged_dataset / "images" / "frame.jpg").exists())
                self.assertTrue((staged_dataset / "sparse" / "0" / "images.txt").exists())
                self.assertTrue((staged_downsampled / "sparse" / "0" / "images.txt").exists())
                self.assertIn(
                    "1 PINHOLE 2000 1000 1500 1550 1000 500",
                    (staged_downsampled / "sparse" / "0" / "cameras.txt").read_text(encoding="utf-8"),
                )
                self.assertTrue((staged_model / "cameras.json").exists())
                self.assertTrue((staged_model / "point_cloud" / "iteration_7000" / "point_cloud.ply").exists())
                self.assertTrue((staged_downsampled_model / "cameras.json").exists())
                self.assertTrue((staged_downsampled_model / "point_cloud" / "iteration_7000" / "point_cloud.ply").exists())
                self.assertEqual(output_path, model / "train" / "ours_7000" / "gs2mesh_post.ply")
            finally:
                server.OUTPUT_DIR = original_output
                server.DATASETS_DIR = original_datasets
                if original_gs2mesh is not None:
                    server.GS2MESH_DIR = original_gs2mesh

    def test_gs2mesh_colmap_conversion_uses_colmap_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            sparse = Path(tmp) / "sparse" / "0"
            sparse.mkdir(parents=True)
            colmap = Path(tmp) / "colmap.exe"
            colmap.write_text("fake", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(Path(args[0]), colmap)
                self.assertIn(str(colmap.parent), kwargs["env"]["PATH"])
                self.assertNotIn("CONDA_PREFIX", kwargs["env"])
                for name in ("cameras.txt", "images.txt", "points3D.txt"):
                    (sparse / name).write_text("# converted\n", encoding="utf-8")
                return server.subprocess.CompletedProcess(args, 0, stdout="", stderr="")

            with mock.patch.object(server, "colmap_executable", return_value=colmap), \
                    mock.patch.object(server, "training_env", side_effect=AssertionError("training_env should not be used")), \
                    mock.patch.object(server.subprocess, "run", side_effect=fake_run):
                server.convert_colmap_model_to_text(sparse)

            self.assertTrue((sparse / "images.txt").exists())

    def test_gs2mesh_mesh_command_reuses_completed_render_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            original_datasets = server.DATASETS_DIR
            original_gs2mesh = getattr(server, "GS2MESH_DIR", None)
            server.OUTPUT_DIR = Path(tmp) / "output"
            server.DATASETS_DIR = Path(tmp) / "datasets"
            server.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                dataset = server.DATASETS_DIR / "mesh_scene"
                sparse = dataset / "sparse" / "0"
                model.mkdir(parents=True, exist_ok=True)
                (dataset / "images").mkdir(parents=True, exist_ok=True)
                sparse.mkdir(parents=True, exist_ok=True)
                (dataset / "images" / "frame.jpg").write_bytes(b"image")
                (sparse / "cameras.txt").write_text(
                    "# colmap text\n1 PINHOLE 4000 2000 3000 3100 2000 1000\n",
                    encoding="utf-8",
                )
                (sparse / "images.txt").write_text(
                    "# colmap text\n1 1 0 0 0 0 0 0 1 frame.jpg\n\n",
                    encoding="utf-8",
                )
                (sparse / "points3D.txt").write_text("# colmap text\n", encoding="utf-8")
                server.GS2MESH_DIR.mkdir(parents=True, exist_ok=True)
                (server.GS2MESH_DIR / "run_single.py").write_text("print('gs2mesh')", encoding="utf-8")
                legacy = (
                    server.GS2MESH_DIR
                    / "output"
                    / "custom_nw_iterations7000_DLNR_Middlebury_baseline7_0p"
                    / "mesh_scene_downsample2"
                    / "000"
                )
                (legacy / "out_DLNR_Middlebury").mkdir(parents=True, exist_ok=True)
                (legacy / "left.png").write_bytes(b"left")
                (legacy / "out_DLNR_Middlebury" / "depth.npy").write_bytes(b"depth")
                (legacy / "out_DLNR_Middlebury" / "occlusion_mask.npy").write_bytes(b"mask")
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                (model / "cameras.json").write_text("[]", encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, _cwd, _output_path = server.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {
                        "mode": "gs2mesh",
                        "gs2mesh_downsample": 2,
                        "gs2mesh_baseline_percentage": 7,
                    },
                )

                command_text = [str(part) for part in command]
                self.assertIn("--skip_rendering", command_text)
                reused = server.GS2MESH_DIR / "output" / "app" / "mesh_scene_d2" / "000"
                self.assertTrue((reused / "left.png").exists())
                self.assertTrue((reused / "out_DLNR_Middlebury" / "depth.npy").exists())
            finally:
                server.OUTPUT_DIR = original_output
                server.DATASETS_DIR = original_datasets
                if original_gs2mesh is not None:
                    server.GS2MESH_DIR = original_gs2mesh

    def test_collect_gs2mesh_output_copies_latest_cleaned_mesh(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            original_gs2mesh = getattr(server, "GS2MESH_DIR", None)
            server.OUTPUT_DIR = Path(tmp) / "output"
            server.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                source = server.GS2MESH_DIR / "output" / "custom_nw_iterations7" / "mesh_scene" / "result_cleaned_mesh.ply"
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("ply\n", encoding="utf-8")

                result = server.collect_gs2mesh_mesh_output("mesh_scene", 7, 0)

                target = server.mesh_output_path("mesh_scene", 7, "gs2mesh", post=True)
                self.assertEqual(result["mesh"], str(target))
                self.assertEqual(target.read_text(encoding="utf-8"), "ply\n")
                self.assertEqual(result["source"], str(source))
            finally:
                server.OUTPUT_DIR = original_output
                if original_gs2mesh is not None:
                    server.GS2MESH_DIR = original_gs2mesh

    def test_collect_sugar_outputs_creates_preview_mesh_and_texture_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            original_sugar = server.SUGAR_DIR
            server.OUTPUT_DIR = Path(tmp) / "output"
            server.SUGAR_DIR = Path(tmp) / "SuGaR"
            try:
                dataset = Path(tmp) / "datasets" / "mesh_scene"
                sugar_dir = server.SUGAR_DIR / "output" / "refined_mesh" / "mesh_scene"
                sugar_dir.mkdir(parents=True, exist_ok=True)
                source_obj = sugar_dir / "sugarfine_scene.obj"
                source_mtl = sugar_dir / "sugarfine_scene.mtl"
                source_png = sugar_dir / "sugarfine_scene.png"
                source_obj.write_text(
                    "\n".join([
                        "mtllib sugarfine_scene.mtl",
                        "v 0 0 0",
                        "v 1 0 0",
                        "v 0 1 0",
                        "vt 0 0",
                        "vt 1 0",
                        "vt 0 1",
                        "usemtl material_0",
                        "f 1/1 2/2 3/3",
                        "",
                    ]),
                    encoding="utf-8",
                )
                source_mtl.write_text("newmtl material_0\nmap_Kd sugarfine_scene.png\n", encoding="utf-8")
                source_png.write_bytes(
                    b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
                    b"\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\xff\xff?"
                    b"\x00\x05\xfe\x02\xfeA\xe2%\xb5\x00\x00\x00\x00IEND\xaeB`\x82"
                )

                result = server.collect_sugar_mesh_outputs("mesh_scene", 7000, dataset, 0)
                texture_paths = server.mesh_texture_paths("mesh_scene", 7000, "sugar", post=True)
                preview_ply = server.mesh_output_path("mesh_scene", 7000, "sugar", post=True)

                self.assertEqual(result["mesh"], str(preview_ply))
                self.assertTrue(preview_ply.exists())
                self.assertTrue(texture_paths["obj"].exists())
                self.assertTrue(texture_paths["mtl"].exists())
                self.assertTrue(texture_paths["png"].exists())
                self.assertTrue(texture_paths["zip"].exists())
                self.assertIn(texture_paths["mtl"].name, texture_paths["obj"].read_text(encoding="utf-8"))
                self.assertIn(texture_paths["png"].name, texture_paths["mtl"].read_text(encoding="utf-8"))
                mesh = PlyData.read(preview_ply)
                self.assertEqual(len(mesh["vertex"].data), 3)
                self.assertEqual(len(mesh["face"].data), 1)
                with zipfile.ZipFile(texture_paths["zip"]) as archive:
                    self.assertIn(texture_paths["obj"].name, archive.namelist())
                    self.assertIn(texture_paths["mtl"].name, archive.namelist())
                    self.assertIn(texture_paths["png"].name, archive.namelist())
            finally:
                server.OUTPUT_DIR = original_output
                server.SUGAR_DIR = original_sugar

    def test_mesh_assets_reports_existing_bounded_and_unbounded_meshes(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                model = server.OUTPUT_DIR / "mesh_scene"
                bounded = model / "train" / "ours_7" / "fuse_post.ply"
                unbounded = model / "train" / "ours_7" / "fuse_unbounded_post.ply"
                bounded.parent.mkdir(parents=True, exist_ok=True)
                bounded.write_bytes(b"ply\n")
                unbounded.write_bytes(b"ply\nmesh\n")

                assets = server.mesh_asset_payload("mesh_scene", 7)

                self.assertTrue(assets["meshes"]["bounded"]["post"]["exists"])
                self.assertTrue(assets["meshes"]["unbounded"]["post"]["exists"])
                self.assertEqual(assets["meshes"]["bounded"]["post"]["size"], 4)
                self.assertIn("/api/mesh/file?", assets["meshes"]["unbounded"]["post"]["url"])
                self.assertIn("texture", assets["meshes"]["bounded"])
                self.assertIn("/api/mesh/texture/file?", assets["meshes"]["bounded"]["texture"]["files"]["zip"]["url"])
                self.assertIn("glb", assets["meshes"]["bounded"]["texture"]["files"])
            finally:
                server.OUTPUT_DIR = original_output

    def test_mesh_preview_downsamples_large_binary_mesh_for_display(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                mesh = server.OUTPUT_DIR / "mesh_scene" / "train" / "ours_7" / "fuse_post.ply"
                mesh.parent.mkdir(parents=True, exist_ok=True)
                header = "\n".join([
                    "ply",
                    "format binary_little_endian 1.0",
                    "element vertex 6",
                    "property float x",
                    "property float y",
                    "property float z",
                    "property uchar red",
                    "property uchar green",
                    "property uchar blue",
                    "element face 4",
                    "property list uchar uint vertex_indices",
                    "end_header",
                    "",
                ]).encode("ascii")
                vertices = b"".join(
                    struct.pack("<fffBBB", float(i), 0.0, 0.0, 255, 255, 255)
                    for i in range(6)
                )
                faces = b"".join(
                    struct.pack("<BIII", 3, i, i + 1, i + 2)
                    for i in range(4)
                )
                mesh.write_bytes(header + vertices + faces)

                preview = server.mesh_preview_path("mesh_scene", 7, "bounded", True, max_faces=2)
                payload = server.mesh_asset_payload("mesh_scene", 7)

                self.assertTrue(preview.exists())
                self.assertIn("/api/mesh/preview_file?", payload["meshes"]["bounded"]["post"]["preview_url"])
                loaded = PlyData.read(preview)
                self.assertEqual(len(loaded["face"].data), 2)
                self.assertLessEqual(len(loaded["vertex"].data), 6)
            finally:
                server.OUTPUT_DIR = original_output

    def test_mesh_chunk_manifest_splits_large_binary_mesh_for_progressive_loading(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                mesh = server.OUTPUT_DIR / "mesh_scene" / "train" / "ours_7" / "fuse_post.ply"
                mesh.parent.mkdir(parents=True, exist_ok=True)
                header = "\n".join([
                    "ply",
                    "format binary_little_endian 1.0",
                    "element vertex 6",
                    "property float x",
                    "property float y",
                    "property float z",
                    "property uchar red",
                    "property uchar green",
                    "property uchar blue",
                    "element face 4",
                    "property list uchar uint vertex_indices",
                    "end_header",
                    "",
                ]).encode("ascii")
                vertices = b"".join(
                    struct.pack("<fffBBB", float(i), 0.0, 0.0, 255, 255, 255)
                    for i in range(6)
                )
                faces = b"".join(
                    struct.pack("<BIII", 3, i, i + 1, i + 2)
                    for i in range(4)
                )
                mesh.write_bytes(header + vertices + faces)

                manifest = server.mesh_chunk_manifest("mesh_scene", 7, "bounded", True, max_faces=2)

                self.assertEqual(manifest["face_count"], 4)
                self.assertEqual(len(manifest["chunks"]), 2)
                self.assertIn("/api/mesh/chunk_file?", manifest["chunks"][0]["url"])
                first_chunk = Path(manifest["chunks"][0]["path"])
                self.assertTrue(first_chunk.exists())
                payload = first_chunk.read_bytes()
                header_len = struct.unpack_from("<I", payload, 0)[0]
                chunk_header = json.loads(payload[4:4 + header_len].decode("utf-8"))
                self.assertEqual(chunk_header["face_count"], 2)
                self.assertEqual(chunk_header["has_vertex_colors"], True)
            finally:
                server.OUTPUT_DIR = original_output

    def test_textured_mesh_chunk_manifest_splits_colmap_textured_ply(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = Path(tmp)
            try:
                texture_dir = server.mesh_texture_dir("mesh_scene", 7, "bounded", True)
                colmap_output = texture_dir / "colmap_texturer" / "output"
                colmap_output.mkdir(parents=True, exist_ok=True)
                source = colmap_output / "mesh.ply"
                (colmap_output / "texture.png").write_bytes(b"png")
                header = "\n".join([
                    "ply",
                    "format binary_little_endian 1.0",
                    "comment TextureFile texture.png",
                    "element vertex 4",
                    "property float x",
                    "property float y",
                    "property float z",
                    "element face 2",
                    "property list uchar int vertex_indices",
                    "property list uchar float texcoord",
                    "end_header",
                    "",
                ]).encode("ascii")
                vertices = b"".join(
                    struct.pack("<fff", float(i), 0.0, 0.0)
                    for i in range(4)
                )
                faces = b"".join([
                    struct.pack("<BiiiBffffff", 3, 0, 1, 2, 6, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0),
                    struct.pack("<BiiiBffffff", 3, 0, 2, 3, 6, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0),
                ])
                source.write_bytes(header + vertices + faces)

                manifest = server.textured_mesh_chunk_manifest("mesh_scene", 7, "bounded", True, max_faces=1)

                self.assertEqual(manifest["face_count"], 2)
                self.assertEqual(len(manifest["chunks"]), 2)
                self.assertIn("/api/mesh/texture/chunk_file?", manifest["chunks"][0]["url"])
                self.assertIn("/api/mesh/texture/file?", manifest["texture_url"])
                first_chunk = Path(manifest["chunks"][0]["path"])
                payload = first_chunk.read_bytes()
                header_len = struct.unpack_from("<I", payload, 0)[0]
                chunk_header = json.loads(payload[4:4 + header_len].decode("utf-8"))
                self.assertEqual(chunk_header["format"], "3dgs-editor-textured-mesh-chunk-v1")
                self.assertEqual(chunk_header["face_count"], 1)
                data_offset = 4 + header_len
                data_offset += (4 - data_offset % 4) % 4
                uv_offset = data_offset + chunk_header["vertex_count"] * 3 * 4
                uvs = struct.unpack_from("<" + "f" * chunk_header["vertex_count"] * 2, payload, uv_offset)
                self.assertEqual(uvs[:6], (0.0, 0.0, 1.0, 0.0, 0.0, 1.0))
            finally:
                server.OUTPUT_DIR = original_output

    def test_texture_options_default_to_openmvs_backend(self):
        options = server.bake_texture_options({"mode": "bounded", "texture_res": 4096})

        self.assertEqual(options["backend"], "openmvs")
        self.assertEqual(options["texture_res"], 4096)
        self.assertEqual(options["bake_source"], "photo")
        self.assertEqual(options["max_faces"], 100000)
        self.assertFalse(options["local_seam_leveling"])
        self.assertEqual(options["cost_smoothness_ratio"], 0.3)
        self.assertEqual(options["texture_size_multiple"], 0)

    def test_texture_options_accept_colmap_backend(self):
        options = server.bake_texture_options({"mode": "bounded", "texture_res": 4096, "backend": "colmap"})

        self.assertEqual(options["backend"], "colmap")
        self.assertEqual(options["max_faces"], 200000)
        self.assertEqual(options["bake_source"], "photo")

    def test_texture_bake_supports_2dgs_and_gs2mesh(self):
        self.assertTrue(server.mesh_mode_supports_texture_bake("bounded", "2dgs"))
        self.assertTrue(server.mesh_mode_supports_texture_bake("unbounded", "2dgs"))
        self.assertTrue(server.mesh_mode_supports_texture_bake("gs2mesh", "3dgs"))
        self.assertFalse(server.mesh_mode_supports_texture_bake("bounded", "3dgs"))
        self.assertFalse(server.mesh_mode_supports_texture_bake("gs2mesh", "2dgs"))
        self.assertFalse(server.mesh_mode_supports_texture_bake("sugar", "3dgs"))

    def test_texture_options_accept_ultra_openmvs_smoothing(self):
        options = server.bake_texture_options({
            "mode": "bounded",
            "texture_res": 8192,
            "max_faces": 0,
            "local_seam_leveling": True,
            "cost_smoothness_ratio": 0.8,
            "texture_size_multiple": 8192,
        })

        self.assertEqual(options["texture_res"], 8192)
        self.assertEqual(options["max_faces"], 0)
        self.assertTrue(options["local_seam_leveling"])
        self.assertEqual(options["cost_smoothness_ratio"], 0.8)
        self.assertEqual(options["texture_size_multiple"], 8192)

    def test_openmvs_texture_commands_stage_colmap_scene_before_texturemesh(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_output = server.OUTPUT_DIR
            original_openmvs = server.OPENMVS_DIR
            server.OUTPUT_DIR = base / "output"
            server.OPENMVS_DIR = base / "openMVS"
            try:
                bin_dir = server.OPENMVS_DIR / "build" / "bin" / "x64" / "Release"
                bin_dir.mkdir(parents=True, exist_ok=True)
                interface_colmap = bin_dir / "InterfaceCOLMAP.exe"
                texture_mesh = bin_dir / "TextureMesh.exe"
                interface_colmap.write_text("", encoding="utf-8")
                texture_mesh.write_text("", encoding="utf-8")

                dataset = base / "dataset"
                (dataset / "images").mkdir(parents=True)
                sparse0 = dataset / "sparse" / "0"
                sparse0.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse0 / name).write_bytes(b"test")

                model = server.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True, exist_ok=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 400000\nend_header\n", encoding="ascii")

                commands, work_dir, output_stem = server.openmvs_texture_commands(
                    "scene_2dgs",
                    7,
                    {
                        "mode": "bounded",
                        "post": True,
                        "texture_res": 4096,
                        "max_faces": 200000,
                        "local_seam_leveling": True,
                        "cost_smoothness_ratio": 0.8,
                        "texture_size_multiple": 8192,
                    },
                )

                self.assertEqual(commands[0][0], interface_colmap)
                self.assertIn(str(work_dir / "dense"), [str(part) for part in commands[0]])
                self.assertIn(str(dataset / "images"), [str(part) for part in commands[0]])
                self.assertEqual(commands[1][0], texture_mesh)
                self.assertIn("-m", [str(part) for part in commands[1]])
                self.assertIn(str(mesh_path), [str(part) for part in commands[1]])
                self.assertIn("--export-type", [str(part) for part in commands[1]])
                self.assertIn("obj", [str(part) for part in commands[1]])
                self.assertIn("--max-texture-size", [str(part) for part in commands[1]])
                self.assertIn("4096", [str(part) for part in commands[1]])
                self.assertIn("--decimate", [str(part) for part in commands[1]])
                self.assertIn("0.500000", [str(part) for part in commands[1]])
                self.assertIn("--virtual-face-images", [str(part) for part in commands[1]])
                self.assertIn("3", [str(part) for part in commands[1]])
                texture_args = [str(part) for part in commands[1]]
                for option in ("--global-seam-leveling", "--local-seam-leveling", "--sharpness-weight"):
                    self.assertIn(option, texture_args)
                self.assertEqual(texture_args[texture_args.index("--global-seam-leveling") + 1], "0")
                self.assertEqual(texture_args[texture_args.index("--local-seam-leveling") + 1], "1")
                self.assertEqual(texture_args[texture_args.index("--sharpness-weight") + 1], "0")
                self.assertEqual(texture_args[texture_args.index("--cost-smoothness-ratio") + 1], "0.8")
                self.assertEqual(texture_args[texture_args.index("--texture-size-multiple") + 1], "8192")
                self.assertTrue((work_dir / "dense" / "sparse" / "cameras.bin").exists())
                self.assertEqual(output_stem.name, "fuse_post_openmvs.obj")
            finally:
                server.OUTPUT_DIR = original_output
                server.OPENMVS_DIR = original_openmvs

    def test_openmvs_texture_commands_reuse_existing_dense_colmap_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_output = server.OUTPUT_DIR
            original_openmvs = server.OPENMVS_DIR
            server.OUTPUT_DIR = base / "output"
            server.OPENMVS_DIR = base / "openMVS"
            try:
                bin_dir = server.OPENMVS_DIR / "build" / "bin" / "x64" / "Release"
                bin_dir.mkdir(parents=True, exist_ok=True)
                (bin_dir / "InterfaceCOLMAP.exe").write_text("", encoding="utf-8")
                (bin_dir / "TextureMesh.exe").write_text("", encoding="utf-8")

                dataset = base / "dataset"
                (dataset / "dense" / "images").mkdir(parents=True)
                (dataset / "dense" / "sparse").mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (dataset / "dense" / "sparse" / name).write_bytes(b"test")

                model = server.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True, exist_ok=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 100\nend_header\n", encoding="ascii")

                commands, _, _ = server.openmvs_texture_commands(
                    "scene_2dgs",
                    7,
                    {"mode": "bounded", "post": True, "texture_res": 2048, "max_faces": 0},
                )

                self.assertEqual(len(commands), 2)
                self.assertEqual(commands[0][0].name, "InterfaceCOLMAP.exe")
                self.assertIn(str(dataset / "dense"), [str(part) for part in commands[0]])
                self.assertEqual(commands[1][0].name, "TextureMesh.exe")
            finally:
                server.OUTPUT_DIR = original_output
                server.OPENMVS_DIR = original_openmvs

    def test_prepare_colmap_texturer_workspace_copies_sparse_and_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            dataset = base / "dataset"
            sparse0 = dataset / "sparse" / "0"
            images = dataset / "images" / "frames"
            sparse0.mkdir(parents=True)
            images.mkdir(parents=True)
            (sparse0 / "cameras.bin").write_bytes(b"camera")
            (sparse0 / "images.bin").write_bytes(b"images")
            (sparse0 / "points3D.bin").write_bytes(b"points")
            (images / "frame_000.jpg").write_bytes(b"jpg")

            workspace = server.prepare_colmap_texturer_workspace(dataset, base / "work")

            self.assertTrue((workspace / "sparse" / "cameras.bin").exists())
            self.assertTrue((workspace / "sparse" / "images.bin").exists())
            self.assertTrue((workspace / "images" / "frames" / "frame_000.jpg").exists())

    def test_colmap_texturer_command_rebuilds_stale_workspace(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = base / "output"
            try:
                dataset = base / "dataset"
                sparse0 = dataset / "sparse" / "0"
                images = dataset / "images"
                sparse0.mkdir(parents=True)
                images.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse0 / name).write_bytes(b"data")
                (images / "frame_000.jpg").write_bytes(b"jpg")

                model = server.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True)
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 0\nend_header\n", encoding="ascii")
                texturer_dir = mesh_path.parent / "fuse_post_texture" / "colmap_texturer"
                stale_workspace = texturer_dir / "workspace" / "images"
                stale_workspace.mkdir(parents=True)
                cache_marker = texturer_dir / "workspace" / "cache.marker"
                cache_marker.write_text("keep", encoding="utf-8")
                old_output = texturer_dir / "output"
                old_output.mkdir(parents=True)
                (old_output / "old.obj").write_text("old", encoding="utf-8")

                with mock.patch.object(server, "colmap_executable", return_value=Path("colmap.exe")):
                    _, work_dir, _ = server.colmap_texturer_command(
                        "scene_2dgs",
                        7,
                        {"mode": "bounded", "post": True, "texture_res": 8192},
                    )

                self.assertTrue((work_dir / "workspace" / "images" / "frame_000.jpg").exists())
                self.assertTrue(cache_marker.exists())
                self.assertFalse((work_dir / "output" / "old.obj").exists())
            finally:
                server.OUTPUT_DIR = original_output

    def test_colmap_texturer_command_uses_texture_scale_factor_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_output = server.OUTPUT_DIR
            server.OUTPUT_DIR = base / "output"
            try:
                dataset = base / "dataset"
                sparse0 = dataset / "sparse" / "0"
                images = dataset / "images"
                sparse0.mkdir(parents=True)
                images.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse0 / name).write_bytes(b"data")
                (images / "frame_000.jpg").write_bytes(b"jpg")

                model = server.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True)
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 0\nend_header\n", encoding="ascii")

                with mock.patch.object(server, "colmap_executable", return_value=Path("colmap.exe")):
                    command, _, _ = server.colmap_texturer_command(
                        "scene_2dgs",
                        7,
                        {
                            "mode": "bounded",
                            "post": True,
                            "texture_res": 8192,
                        },
                    )

                args = [str(part) for part in command]
                self.assertIn("--MeshTextureMapping.texture_scale_factor", args)
                self.assertEqual(args[args.index("--MeshTextureMapping.texture_scale_factor") + 1], "1")
                mesh_texture_mapping_args = [arg for arg in args if arg.startswith("--MeshTextureMapping.")]
                self.assertEqual(mesh_texture_mapping_args, ["--MeshTextureMapping.texture_scale_factor"])
            finally:
                server.OUTPUT_DIR = original_output

    def test_colmap_texturer_caps_scale_for_large_mesh(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            small = base / "small.ply"
            large = base / "large.ply"
            small.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 100\nend_header\n", encoding="ascii")
            large.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 3000000\nend_header\n", encoding="ascii")

            small_scale, small_faces, small_reason = server.colmap_texture_scale_factor(small, 8192)
            large_scale, large_faces, large_reason = server.colmap_texture_scale_factor(large, 8192)

            self.assertEqual(small_faces, 100)
            self.assertEqual(small_scale, 1.0)
            self.assertEqual(small_reason, "requested texture resolution")
            self.assertEqual(large_faces, 3000000)
            self.assertEqual(large_scale, 0.25)
            self.assertEqual(large_reason, "large mesh atlas guard")

    def test_openmvs_texture_bake_retries_without_local_seam_leveling(self):
        job = {
            "scene": "scene_2dgs",
            "iteration": 7,
            "log": [],
        }
        output_paths = {"zip": Path("texture.zip")}
        options = {
            "mode": "bounded",
            "post": True,
            "texture_res": 8192,
            "max_faces": 100000,
            "local_seam_leveling": True,
        }

        def fake_commands(scene, iteration, passed_options, work_subdir="openmvs"):
            return [[work_subdir]], Path(work_subdir), Path(work_subdir) / "mesh.obj"

        with mock.patch.object(server, "openmvs_texture_commands", side_effect=fake_commands) as commands_mock, \
                mock.patch.object(server, "run_logged", side_effect=[RuntimeError("TextureMesh crashed"), None]), \
                mock.patch.object(server, "collect_openmvs_texture_outputs", return_value={"zip": "texture.zip"}):
            collected = server.run_openmvs_texture_bake(job, output_paths, options)

        self.assertFalse(collected["local_seam_leveling_used"])
        self.assertIn("TextureMesh crashed", collected["local_seam_leveling_error"])
        self.assertEqual(commands_mock.call_args_list[0].kwargs, {})
        self.assertEqual(commands_mock.call_args_list[1].kwargs["work_subdir"], "openmvs_retry_noseam")
        self.assertTrue(any("retrying without local seam-leveling" in line for line in job["log"]))

    def test_clear_texture_outputs_removes_stale_preview_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            paths = {
                "dir": base,
                "obj": base / "mesh.obj",
                "mtl": base / "mesh.mtl",
                "png": base / "mesh.png",
                "zip": base / "mesh.zip",
                "glb": base / "mesh.glb",
            }
            for path in paths.values():
                if path != base:
                    path.write_text("old", encoding="utf-8")
            server.clear_texture_outputs(paths)

            self.assertFalse(paths["obj"].exists())
            self.assertFalse(paths["mtl"].exists())
            self.assertFalse(paths["png"].exists())
            self.assertFalse(paths["zip"].exists())
            self.assertFalse(paths["glb"].exists())

    def test_collect_openmvs_texture_outputs_preserves_openmvs_texture_pixels(self):
        try:
            from PIL import Image
        except Exception as exc:
            self.skipTest(f"Pillow unavailable: {exc}")
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            work_dir = base / "openmvs"
            work_dir.mkdir()
            (work_dir / "mesh.obj").write_text(
                "mtllib mesh.mtl\n"
                "v 0 0 0\n"
                "v 1 0 0\n"
                "v 0 1 0\n"
                "vt 0 0\n"
                "vt 1 0\n"
                "vt 0 1\n"
                "usemtl material_00\n"
                "f 1/1 2/2 3/3\n",
                encoding="utf-8",
            )
            (work_dir / "mesh.mtl").write_text(
                "newmtl material_00\nmap_Kd mesh.png\n",
                encoding="utf-8",
            )
            data = np.zeros((2, 2, 3), dtype=np.uint8)
            data[0, 0] = [255, 0, 0]
            Image.fromarray(data).save(work_dir / "mesh.png")
            paths = {
                "dir": base / "texture",
                "obj": base / "texture" / "mesh.obj",
                "mtl": base / "texture" / "mesh.mtl",
                "png": base / "texture" / "mesh.png",
                "zip": base / "texture" / "mesh.zip",
                "glb": base / "texture" / "mesh.glb",
            }

            server.collect_openmvs_texture_outputs(work_dir, paths)

            copied = np.asarray(Image.open(paths["png"]).convert("RGB"))
            self.assertTrue(np.any(np.all(copied == [0, 0, 0], axis=2)))
            self.assertTrue(np.any(np.all(copied == [255, 0, 0], axis=2)))
            self.assertTrue(paths["glb"].exists())
            self.assertEqual(paths["glb"].read_bytes()[:4], b"glTF")

    def test_collect_openmvs_texture_outputs_keeps_texture_when_glb_export_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            work_dir = base / "openmvs"
            work_dir.mkdir()
            (work_dir / "mesh.obj").write_text(
                "mtllib mesh.mtl\n"
                "v 0 0 0\n"
                "v 1 0 0\n"
                "v 0 1 0\n"
                "vt 0 0\n"
                "vt 1 0\n"
                "vt 0 1\n"
                "usemtl material_00\n"
                "f 1/1 2/2 3/3\n",
                encoding="utf-8",
            )
            (work_dir / "mesh.mtl").write_text("newmtl material_00\nmap_Kd mesh.png\n", encoding="utf-8")
            try:
                from PIL import Image
            except Exception as exc:
                self.skipTest(f"Pillow unavailable: {exc}")
            Image.fromarray(np.zeros((2, 2, 3), dtype=np.uint8)).save(work_dir / "mesh.png")
            paths = {
                "dir": base / "texture",
                "obj": base / "texture" / "mesh.obj",
                "mtl": base / "texture" / "mesh.mtl",
                "png": base / "texture" / "mesh.png",
                "zip": base / "texture" / "mesh.zip",
                "glb": base / "texture" / "mesh.glb",
            }

            with mock.patch.object(server, "export_textured_obj_to_glb", side_effect=RuntimeError("missing trimesh")):
                result = server.collect_openmvs_texture_outputs(work_dir, paths)

            self.assertTrue(paths["zip"].exists())
            self.assertTrue(paths["obj"].exists())
            self.assertIn("missing trimesh", result["glb_error"])
            self.assertFalse(paths["glb"].exists())

    def test_bake_texture_from_vertex_colors_exports_obj_mtl_png_zip(self):
        try:
            import open3d as o3d
        except Exception as exc:
            self.skipTest(f"open3d unavailable: {exc}")
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            mesh_path = base / "colored.ply"
            mesh = o3d.geometry.TriangleMesh()
            mesh.vertices = o3d.utility.Vector3dVector(np.array([
                [0.0, 0.0, 0.0],
                [1.0, 0.0, 0.0],
                [0.0, 1.0, 0.0],
            ], dtype=np.float64))
            mesh.triangles = o3d.utility.Vector3iVector(np.array([[0, 1, 2]], dtype=np.int32))
            mesh.vertex_colors = o3d.utility.Vector3dVector(np.array([
                [1.0, 0.0, 0.0],
                [0.0, 1.0, 0.0],
                [0.0, 0.0, 1.0],
            ], dtype=np.float64))
            self.assertTrue(o3d.io.write_triangle_mesh(str(mesh_path), mesh))
            paths = {
                "dir": base / "texture",
                "obj": base / "texture" / "mesh.obj",
                "mtl": base / "texture" / "mesh.mtl",
                "png": base / "texture" / "mesh.png",
                "zip": base / "texture" / "mesh.zip",
                "glb": base / "texture" / "mesh.glb",
            }

            result = server.bake_texture_from_vertex_colors(mesh_path, paths, texture_res=256, padding=2)

            self.assertEqual(result["face_count"], 1)
            self.assertTrue(paths["obj"].exists())
            self.assertTrue(paths["mtl"].exists())
            self.assertTrue(paths["png"].exists())
            self.assertTrue(paths["zip"].exists())
            self.assertTrue(paths["glb"].exists())
            self.assertIn("map_Kd mesh.png", paths["mtl"].read_text(encoding="utf-8"))
            with zipfile.ZipFile(paths["zip"]) as archive:
                self.assertEqual(set(archive.namelist()), {"mesh.obj", "mesh.mtl", "mesh.png"})

    def test_collect_colmap_texture_outputs_converts_textured_ply_to_obj(self):
        try:
            from PIL import Image
        except Exception as exc:
            self.skipTest(f"Pillow unavailable: {exc}")
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            work_dir = base / "colmap" / "output"
            work_dir.mkdir(parents=True)
            vertices = np.array(
                [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
                dtype=[("x", "f4"), ("y", "f4"), ("z", "f4")],
            )
            faces = np.empty(1, dtype=[("vertex_indices", "O"), ("texcoord", "O")])
            faces[0] = (
                np.array([0, 1, 2], dtype=np.int32),
                np.array([0.0, 0.0, 1.0, 0.0, 0.0, 1.0], dtype=np.float32),
            )
            PlyData([PlyElement.describe(vertices, "vertex"), PlyElement.describe(faces, "face")], text=False).write(work_dir / "mesh.ply")
            Image.fromarray(np.zeros((2, 2, 3), dtype=np.uint8)).save(work_dir / "texture.png")
            paths = {
                "dir": base / "out",
                "obj": base / "out" / "mesh.obj",
                "mtl": base / "out" / "mesh.mtl",
                "png": base / "out" / "mesh.png",
                "zip": base / "out" / "mesh.zip",
                "glb": base / "out" / "mesh.glb",
            }

            result = server.collect_colmap_texture_outputs(work_dir, paths)

            self.assertEqual(result["texture_pages"], 1)
            obj_text = paths["obj"].read_text(encoding="utf-8")
            self.assertIn("mtllib mesh.mtl", obj_text)
            self.assertIn("vt 1 0", obj_text)
            self.assertIn("f 1/1 2/2 3/3", obj_text)
            self.assertTrue(paths["zip"].exists())

if __name__ == "__main__":
    unittest.main()
