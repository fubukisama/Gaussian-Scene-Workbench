import sys
import io
import json
import os
import shutil
import struct
import subprocess
import tempfile
import types
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import numpy as np
from PIL import Image
from plyfile import PlyData, PlyElement

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from native.backend import compute  # noqa: E402
from native.worker.test_reconstruction_quality import write_model  # noqa: E402


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
    def test_conda_root_candidates_accepts_base_prefix(self):
        prefix = Path("/opt/miniforge3")
        with mock.patch.dict(compute.os.environ, {"CONDA_PREFIX": str(prefix)}, clear=False):
            candidates = compute.conda_root_candidates()

        self.assertIn(prefix, candidates)

    def test_conda_root_candidates_accepts_named_environment_prefix(self):
        root = Path("/opt/miniforge3")
        with mock.patch.dict(
            compute.os.environ,
            {"CONDA_PREFIX": str(root / "envs" / "gaussian_splatting")},
            clear=False,
        ):
            candidates = compute.conda_root_candidates()

        self.assertIn(root, candidates)


    def test_video_extract_falls_back_to_conda_when_worker_cv2_is_missing(self):
        with (
            mock.patch.object(compute, "extract_video_frames_with_ffmpeg", side_effect=RuntimeError("no ffmpeg")),
            mock.patch.object(compute, "extract_video_frames_with_cv2", side_effect=RuntimeError("no cv2")),
            mock.patch.object(compute, "extract_video_frames_with_conda", return_value=7) as fallback,
        ):
            frames = compute.extract_video_frames(Path("clip.mov"), Path("images"), 2)

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
                mock.patch.object(compute, "ffmpeg_executable", return_value=Path("ffmpeg.exe")),
                mock.patch.object(compute.subprocess, "run", side_effect=fake_run),
            ):
                self.assertEqual(compute.extract_video_frames_with_ffmpeg(video_path, images_dir, 2), 2)


    def test_import_local_paths_archives_from_selected_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp).resolve()
            original_datasets = compute.DATASETS_DIR
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            source = Path(tmp) / "source" / "clip.mov"
            source.parent.mkdir()
            source.write_bytes(b"video-bytes")
            try:
                def fake_extract(video_path, images_dir, fps):
                    self.assertEqual(video_path, compute.DATASETS_DIR / "path_scene" / "source" / "originals" / "clip.mov")
                    (images_dir / "clip_000000.jpg").write_bytes(b"frame")
                    return 1

                with (
                    mock.patch.object(compute, "archive_local_source", wraps=compute.archive_local_source) as archive,
                    mock.patch.object(compute, "extract_video_frames", side_effect=fake_extract),
                ):
                    result = compute.save_path_dataset({
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
                compute.DATASETS_DIR = original_datasets

    def test_local_path_video_import_extracts_multiple_videos_with_parallel_workers(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = compute.DATASETS_DIR
            compute.DATASETS_DIR = Path(tmp) / "datasets"
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
                        future = compute.concurrent.futures.Future()
                        try:
                            future.set_result(fn(*args))
                        except Exception as exc:
                            future.set_exception(exc)
                        return future

                def fake_extract(video_path, images_dir, fps):
                    (images_dir / f"{video_path.stem}_000000.jpg").write_bytes(b"frame")
                    return 1

                with (
                    mock.patch.object(compute, "archive_local_source", side_effect=lambda source, target: (shutil.copy2(source, target), "copy")[1]),
                    mock.patch.object(compute, "extract_video_frames", side_effect=fake_extract),
                    mock.patch.object(compute.concurrent.futures, "ThreadPoolExecutor", RecordingExecutor),
                ):
                    result = compute.save_path_dataset({
                        "scene": "parallel_scene",
                        "overwrite": True,
                        "fps": 2,
                        "files": [{"path": str(source), "name": source.name} for source in sources],
                    })

                self.assertEqual(result["saved_videos"], 3)
                self.assertEqual(result["extracted_frames"], 3)
                self.assertEqual(worker_counts, [3])
            finally:
                compute.DATASETS_DIR = original_datasets

    def test_local_path_import_progress_reports_extracted_video_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = compute.DATASETS_DIR
            compute.DATASETS_DIR = Path(tmp) / "datasets"
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
                    mock.patch.object(compute, "archive_local_source", side_effect=lambda source, target: (shutil.copy2(source, target), "copy")[1]),
                    mock.patch.object(compute, "extract_video_frames", side_effect=fake_extract),
                ):
                    compute.save_path_dataset({
                        "scene": "progress_scene",
                        "overwrite": True,
                        "fps": 2,
                        "import_job_id": job_id,
                        "files": [{"path": str(source), "name": source.name}],
                    })

                status = compute.import_job_snapshot(job_id)
                self.assertEqual(status["status"], "done")
                self.assertEqual(status["stage"], "done")
                self.assertEqual(status["total_files"], 1)
                self.assertEqual(status["processed_files"], 1)
                self.assertEqual(status["total_videos"], 1)
                self.assertEqual(status["processed_videos"], 1)
                self.assertEqual(status["extracted_frames"], 2)
                self.assertEqual(status["image_count"], 2)
            finally:
                compute.DATASETS_DIR = original_datasets
                compute.IMPORT_JOBS.pop(job_id, None)

    def test_colmap_options_default_match_existing_converter_behavior(self):
        options = compute.colmap_options_from_payload({})

        self.assertEqual(options["matching"], "exhaustive")
        self.assertEqual(options["camera_model"], "OPENCV")
        self.assertTrue(options["use_gpu"])
        self.assertEqual(options["mapper_ba_global_function_tolerance"], "0.000001")
        self.assertEqual(options["mapper_ba_global_max_num_iterations"], 50)

    def test_colmap_executable_honors_colmap_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "colmap.exe"
            executable.write_bytes(b"fixture")
            with mock.patch.dict(
                compute.os.environ,
                {"COLMAP_EXE": "", "COLMAP_PATH": str(executable)},
                clear=False,
            ):
                self.assertEqual(compute.colmap_executable(), executable)

    def test_versioned_colmap_candidates_select_newest_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            install_root = Path(tmp) / "COLMAP"
            for version in ("3.13.0", "4.1.0"):
                executable = install_root / version / "bin" / "colmap.exe"
                executable.parent.mkdir(parents=True)
                executable.write_bytes(b"fixture")

            candidates = compute.versioned_colmap_candidates([install_root])

            self.assertEqual(candidates[0], install_root / "4.1.0" / "bin" / "colmap.exe")

    def test_colmap_options_robust_preset_limits_global_ba_and_relaxes_registration(self):
        options = compute.colmap_options_from_payload({"preset": "robust"})

        self.assertEqual(options["matching"], "exhaustive")
        self.assertEqual(options["feature_max_image_size"], 2400)
        self.assertEqual(options["mapper_ba_global_max_num_iterations"], 15)
        self.assertEqual(options["mapper_ba_global_frames_freq"], 100)
        self.assertEqual(options["mapper_min_num_matches"], 10)
        self.assertEqual(options["mapper_abs_pose_min_num_inliers"], 20)

    def test_colmap_options_accept_manual_overrides(self):
        options = compute.colmap_options_from_payload({
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

            with mock.patch.object(compute, "colmap_executable", return_value=Path("colmap.exe")):
                commands = compute.colmap_convert_commands(dataset, compute.colmap_options_from_payload({
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
            undistort_output = Path(undistorter[undistorter.index("--output_path") + 1])
            self.assertEqual(undistort_output, dataset / ".colmap-undistorted")
            self.assertNotEqual(undistort_output, dataset)
            self.assertNotEqual(undistort_output / "images", dataset / "images")

    def test_colmap_convert_stages_undistortion_before_replacing_training_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            images = dataset / "images"
            images.mkdir(parents=True)
            (images / "frame.jpg").write_bytes(b"original")
            for i in range(9):
                (images / ("frame%02d.jpg" % i)).write_bytes(b"original")
            observed_original = []

            def fake_run(_job, command, _cwd, _backend):
                if command[1] == "mapper":
                    write_model(Path(command[command.index("--output_path") + 1]) / "0")
                if command[1] != "image_undistorter":
                    return 0
                observed_original.append((images / "frame.jpg").read_bytes())
                staging = Path(command[command.index("--output_path") + 1])
                (staging / "images").mkdir(parents=True)
                (staging / "images" / "frame.jpg").write_bytes(b"undistorted")
                write_model(staging / "sparse")
                return 0

            with (
                mock.patch.object(compute, "colmap_executable", return_value=Path("colmap.exe")),
                mock.patch.object(compute, "run_logged", side_effect=fake_run),
            ):
                compute.run_colmap_convert(
                    {"log": []},
                    dataset,
                    compute.colmap_options_from_payload({"reset": False}),
                )

            self.assertEqual(observed_original, [b"original"])
            self.assertEqual((images / "frame.jpg").read_bytes(), b"undistorted")
            self.assertTrue(compute.dataset_has_colmap_scene(dataset))
            self.assertTrue(compute.dataset_has_colmap_scene(dataset / ".alignment_cache"))
            self.assertEqual(list(dataset.glob(".colmap-undistorted-*")), [])
            self.assertEqual(list(dataset.glob(".colmap-backup-*")), [])

    def test_colmap_convert_failure_preserves_imported_training_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            images = dataset / "images"
            images.mkdir(parents=True)
            (images / "frame.jpg").write_bytes(b"original")
            for i in range(9):
                (images / ("frame%02d.jpg" % i)).write_bytes(b"original")
            sparse = dataset / "sparse" / "0"
            sparse.mkdir(parents=True)
            for name in ("cameras.bin", "images.bin", "points3D.bin"):
                (sparse / name).write_bytes(b"original")
            stereo = dataset / "stereo"
            stereo.mkdir()
            (stereo / "original.txt").write_bytes(b"original")

            def fake_run(_job, command, _cwd, _backend):
                if command[1] == "mapper":
                    write_model(Path(command[command.index("--output_path") + 1]) / "0")
                if command[1] == "image_undistorter":
                    staging = Path(command[command.index("--output_path") + 1])
                    (staging / "images").mkdir(parents=True)
                    (staging / "images" / "partial.jpg").write_bytes(b"partial")
                    raise RuntimeError("undistorter failed")
                return 0

            with (
                mock.patch.object(compute, "colmap_executable", return_value=Path("colmap.exe")),
                mock.patch.object(compute, "run_logged", side_effect=fake_run),
            ):
                with self.assertRaisesRegex(RuntimeError, "undistorter failed"):
                    compute.run_colmap_convert(
                        {"log": []},
                        dataset,
                        compute.colmap_options_from_payload({"reset": False}),
                    )

            self.assertEqual((images / "frame.jpg").read_bytes(), b"original")
            self.assertEqual((sparse / "cameras.bin").read_bytes(), b"original")
            self.assertEqual((stereo / "original.txt").read_bytes(), b"original")
            self.assertEqual(list(dataset.glob(".colmap-undistorted-*")), [])
            self.assertEqual(list(dataset.glob(".colmap-backup-*")), [])

    def test_colmap_publish_rolls_back_all_training_folders_on_rename_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = (Path(tmp) / "dataset").resolve()
            staging = dataset / ".colmap-undistorted-test"
            for root, marker in ((dataset, b"original"), (staging, b"new")):
                (root / "images").mkdir(parents=True)
                (root / "images" / "frame.jpg").write_bytes(marker)
                sparse = root / "sparse" / "0"
                sparse.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse / name).write_bytes(marker)

            original_rename = Path.rename

            def fail_new_sparse(path, target):
                if path == staging / "sparse":
                    raise OSError("publish failed")
                return original_rename(path, target)

            with mock.patch.object(Path, "rename", autospec=True, side_effect=fail_new_sparse):
                with self.assertRaisesRegex(OSError, "publish failed"):
                    compute.publish_undistorted_colmap_output(dataset, staging)

            self.assertEqual((dataset / "images" / "frame.jpg").read_bytes(), b"original")
            self.assertEqual(
                (dataset / "sparse" / "0" / "cameras.bin").read_bytes(),
                b"original",
            )
            self.assertEqual(list(dataset.glob(".colmap-backup-*")), [])

    def test_colmap_reset_clears_work_cache_but_preserves_published_training_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            for name in ("distorted", "sparse", "stereo"):
                (dataset / name).mkdir(parents=True)
                (dataset / name / "stale.txt").write_text("stale", encoding="utf-8")
            (dataset / "database.db").write_text("old database", encoding="utf-8")

            job = {"log": []}
            options = compute.colmap_options_from_payload({"reset": True})
            with (
                mock.patch.object(compute, "colmap_convert_commands", return_value=[]),
                mock.patch.object(compute, "normalize_undistorted_sparse"),
            ):
                compute.run_colmap_convert(job, dataset, options)

            self.assertFalse((dataset / "database.db").exists())
            self.assertFalse((dataset / "distorted" / "stale.txt").exists())
            self.assertTrue((dataset / "sparse" / "stale.txt").exists())
            self.assertTrue((dataset / "stereo" / "stale.txt").exists())
            self.assertTrue((dataset / "distorted" / "sparse").exists())

    def test_colmap_disables_single_camera_for_mixed_image_dimensions(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            captured_options = {}

            def capture_commands(_dataset, options, _undistort_output=None):
                captured_options.update(options)
                return []

            job = {"log": []}
            options = compute.colmap_options_from_payload({"single_camera": True, "reset": False})
            with (
                mock.patch.object(compute, "dataset_has_mixed_image_dimensions", return_value=True),
                mock.patch.object(compute, "colmap_convert_commands", side_effect=capture_commands),
                mock.patch.object(compute, "normalize_undistorted_sparse"),
            ):
                compute.run_colmap_convert(job, dataset, options)

            self.assertFalse(captured_options["single_camera"])
            self.assertTrue(any("mixed image dimensions" in line for line in job["log"]))


    def test_train_existing_dataset_allows_existing_output_without_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = compute.DATASETS_DIR
            original_output = compute.OUTPUT_DIR
            compute.DATASETS_DIR = base / "datasets"
            compute.OUTPUT_DIR = base / "output"
            dataset = compute.DATASETS_DIR / "scene_existing"
            output = compute.OUTPUT_DIR / "scene_existing"
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
                "colmap_options": compute.colmap_options_from_payload({"reset": False}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(compute, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(compute, "run_logged"),
                    mock.patch.object(compute, "run_colmap_convert"),
                    mock.patch.object(compute, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(compute, "write_training_metadata"),
                    mock.patch.object(compute, "training_point_cloud", return_value={
                        "iteration": 1000,
                        "path": "point_cloud.ply",
                        "vertex_count": 42,
                    }),
                    mock.patch.object(compute, "persist_train_job"),
                ):
                    compute.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                compute.DATASETS_DIR = original_datasets
                compute.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            self.assertIsNone(job["error"])
            self.assertTrue((output / "keep.txt").exists())

    def test_training_rejects_cross_backend_output_reuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = compute.DATASETS_DIR
            original_output = compute.OUTPUT_DIR
            compute.DATASETS_DIR = base / "datasets"
            compute.OUTPUT_DIR = base / "output"
            dataset = compute.DATASETS_DIR / "scene_existing"
            output = compute.OUTPUT_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"fake")
            output.mkdir(parents=True)
            compute.write_training_metadata(output, "3dgs", "full", {"iterations": 30000, "resolution": 4})
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
                "colmap_options": compute.colmap_options_from_payload({"reset": False}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(compute, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                        "two_dgs_dir": "2dgs",
                    }),
                    mock.patch.object(compute, "run_logged") as run_mock,
                    mock.patch.object(compute, "persist_train_job"),
                ):
                    compute.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                compute.DATASETS_DIR = original_datasets
                compute.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "failed")
            self.assertIn("belongs to 3DGS", job["error"])
            run_mock.assert_not_called()

    def test_train_existing_dataset_skips_colmap_when_requested(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = compute.DATASETS_DIR
            original_output = compute.OUTPUT_DIR
            compute.DATASETS_DIR = base / "datasets"
            compute.OUTPUT_DIR = base / "output"
            dataset = compute.DATASETS_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            for i in range(9):
                (dataset / "images" / ("frame%02d.jpg" % i)).write_bytes(b"fake")
            write_model(dataset / "sparse" / "0")
            (compute.OUTPUT_DIR / "scene_existing").mkdir(parents=True)
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
                "colmap_options": compute.colmap_options_from_payload({"reset": True}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(compute, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(compute, "run_logged"),
                    mock.patch.object(compute, "run_colmap_convert") as colmap_mock,
                    mock.patch.object(compute, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(compute, "write_training_metadata"),
                    mock.patch.object(compute, "training_point_cloud", return_value={
                        "iteration": 1000,
                        "path": "point_cloud.ply",
                        "vertex_count": 42,
                    }),
                    mock.patch.object(compute, "persist_train_job"),
                ):
                    compute.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                compute.DATASETS_DIR = original_datasets
                compute.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            colmap_mock.assert_not_called()

    def test_train_existing_dataset_runs_colmap_when_alignment_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = compute.DATASETS_DIR
            original_output = compute.OUTPUT_DIR
            compute.DATASETS_DIR = base / "datasets"
            compute.OUTPUT_DIR = base / "output"
            dataset = compute.DATASETS_DIR / "scene_existing"
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
                "colmap_options": compute.colmap_options_from_payload({"reset": True}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(compute, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(compute, "run_logged"),
                    mock.patch.object(compute, "run_colmap_convert") as colmap_mock,
                    mock.patch.object(compute, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(compute, "write_training_metadata"),
                    mock.patch.object(compute, "training_point_cloud", return_value={
                        "iteration": 1000,
                        "path": "point_cloud.ply",
                        "vertex_count": 42,
                    }),
                    mock.patch.object(compute, "persist_train_job"),
                ):
                    compute.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                compute.DATASETS_DIR = original_datasets
                compute.OUTPUT_DIR = original_output

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
                mock.patch.object(compute, "colmap_convert_commands", return_value=[]),
                mock.patch.object(compute, "normalize_undistorted_sparse"),
            ):
                compute.run_colmap_convert(job, dataset, compute.colmap_options_from_payload({"reset": False}))

            cache_sparse0 = dataset / ".alignment_cache" / "sparse" / "0"
            self.assertTrue((cache_sparse0 / "cameras.bin").exists())
            self.assertTrue((cache_sparse0 / "images.bin").exists())
            self.assertTrue((cache_sparse0 / "points3D.bin").exists())
            self.assertTrue((dataset / ".alignment_cache" / "metadata.json").exists())
            self.assertTrue(any("Saved COLMAP alignment cache" in line for line in job["log"]))

    def test_colmap_alignment_job_runs_converter_and_marks_done(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = compute.DATASETS_DIR
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            dataset = compute.DATASETS_DIR / "align_scene"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images" / "frame.jpg").write_bytes(b"jpg")
            try:
                def fake_convert(job, dataset_path, options):
                    self.assertEqual(dataset_path, dataset.resolve())
                    self.assertEqual(options["preset"], "robust")
                    sparse0 = dataset_path / "sparse" / "0"
                    sparse0.mkdir(parents=True)
                    for name in ("cameras.bin", "images.bin", "points3D.bin"):
                        (sparse0 / name).write_bytes(b"colmap")

                job = {
                    "id": "colmap-job",
                    "kind": "colmap",
                    "scene": "align_scene",
                    "iteration": 0,
                    "mode": "colmap",
                    "options": compute.colmap_options_from_payload({"preset": "robust"}),
                    "status": "queued",
                    "stage": "queued",
                    "created_at": 1,
                    "updated_at": 1,
                    "returncode": None,
                    "error": None,
                    "cancel_requested": False,
                    "process": None,
                    "output_dir": str(dataset),
                    "output_mesh": None,
                    "texture": None,
                    "log": [],
                }
                with mock.patch.object(compute, "run_colmap_convert", side_effect=fake_convert):
                    compute.run_colmap_alignment_job(job)

                self.assertEqual(job["status"], "done")
                self.assertEqual(job["stage"], "done")
                self.assertTrue(job["alignment"]["has_alignment"])
                self.assertEqual(job["output_dir"], str(dataset.resolve()))
                self.assertTrue(any("COLMAP alignment complete" in line for line in job["log"]))
            finally:
                compute.DATASETS_DIR = original_datasets

    def test_colmap_alignment_job_fails_when_dataset_images_are_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_datasets = compute.DATASETS_DIR
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            try:
                job = {
                    "id": "colmap-missing",
                    "kind": "colmap",
                    "scene": "missing_scene",
                    "iteration": 0,
                    "mode": "colmap",
                    "options": compute.colmap_options_from_payload({}),
                    "status": "queued",
                    "stage": "queued",
                    "created_at": 1,
                    "updated_at": 1,
                    "returncode": None,
                    "error": None,
                    "cancel_requested": False,
                    "process": None,
                    "output_dir": str(compute.DATASETS_DIR / "missing_scene"),
                    "output_mesh": None,
                    "texture": None,
                    "log": [],
                }
                compute.run_colmap_alignment_job(job)

                self.assertEqual(job["status"], "failed")
                self.assertIn("No training images found", job["error"])
            finally:
                compute.DATASETS_DIR = original_datasets


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
                mock.patch.object(compute, "colmap_convert_commands", return_value=[]),
                mock.patch.object(compute, "normalize_undistorted_sparse"),
                mock.patch.object(compute, "save_alignment_cache", return_value=False),
            ):
                compute.run_colmap_convert(job, dataset, compute.colmap_options_from_payload({"reset": True}))

            self.assertTrue((cache_sparse0 / "cameras.bin").exists())
            self.assertTrue((cache_sparse0 / "images.bin").exists())
            self.assertTrue((cache_sparse0 / "points3D.bin").exists())

    def test_train_existing_dataset_restores_alignment_cache_before_training(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_datasets = compute.DATASETS_DIR
            original_output = compute.OUTPUT_DIR
            compute.DATASETS_DIR = base / "datasets"
            compute.OUTPUT_DIR = base / "output"
            dataset = compute.DATASETS_DIR / "scene_existing"
            (dataset / "images").mkdir(parents=True)
            for i in range(9):
                (dataset / "images" / ("frame%02d.jpg" % i)).write_bytes(b"fake")
            cache_sparse0 = dataset / ".alignment_cache" / "sparse" / "0"
            write_model(cache_sparse0)
            (compute.OUTPUT_DIR / "scene_existing").mkdir(parents=True)
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
                "colmap_options": compute.colmap_options_from_payload({"reset": True}),
                "train_options": {"iterations": 1000},
                "log": [],
            }

            try:
                with (
                    mock.patch.object(compute, "ensure_training_environment", return_value={
                        "python": "python",
                        "colmap": "colmap",
                    }),
                    mock.patch.object(compute, "run_logged"),
                    mock.patch.object(compute, "run_colmap_convert") as colmap_mock,
                    mock.patch.object(compute, "resolve_training_options_for_environment", side_effect=lambda backend, options, job: options),
                    mock.patch.object(compute, "write_training_metadata"),
                    mock.patch.object(compute, "training_point_cloud", return_value={
                        "iteration": 1000,
                        "path": "point_cloud.ply",
                        "vertex_count": 42,
                    }),
                    mock.patch.object(compute, "persist_train_job"),
                ):
                    compute.run_training_job(job, run_convert=False, quality="quick", overwrite=False)
            finally:
                compute.DATASETS_DIR = original_datasets
                compute.OUTPUT_DIR = original_output

            self.assertEqual(job["status"], "done")
            colmap_mock.assert_not_called()
            self.assertTrue((dataset / "sparse" / "0" / "cameras.bin").exists())
            self.assertTrue(any("Restored cached COLMAP alignment" in line for line in job["log"]))

    def test_2dgs_quality_uses_supported_train_arguments(self):
        options = compute.train_args_for_quality("quality", "2dgs")

        self.assertEqual(options["iterations"], 30000)
        self.assertEqual(options["resolution"], 4)
        self.assertEqual(options["depth_ratio"], 0.0)
        self.assertEqual(options["densify_until_iter"], 10000)
        self.assertEqual(options["densification_interval"], 150)
        self.assertGreater(options["densify_grad_threshold"], 0.0002)
        self.assertNotIn("antialiasing", options)

    def test_2dgs_training_command_saves_milestones_and_caps_densification(self):
        options = compute.training_options_from_payload("2dgs", "max_quality", {})

        command = compute.training_command("2dgs", "dataset", "output", options)

        self.assertEqual(command[command.index("--save_iterations") + 1:command.index("--checkpoint_iterations")], ["7000", "15000", "30000"])
        self.assertEqual(command[command.index("--checkpoint_iterations") + 1:command.index("--depth_ratio")], ["7000", "15000", "30000"])
        self.assertIn("--densify_grad_threshold", command)
        self.assertEqual(command[command.index("--densify_until_iter") + 1], "10000")
        self.assertEqual(command[command.index("--densification_interval") + 1], "200")

    def test_3dgs_max_quality_profile_enables_quality_oriented_training(self):
        options = compute.train_args_for_quality("max_quality", "3dgs")

        self.assertEqual(options["iterations"], 30000)
        self.assertEqual(options["resolution"], 2)
        self.assertTrue(options["antialiasing"])
        self.assertEqual(options["optimizer_type"], "sparse_adam")
        self.assertGreater(options["densify_until_iter"], 15000)
        self.assertLess(options["densify_grad_threshold"], 0.0002)

    def test_training_options_accept_safe_overrides_for_3dgs(self):
        options = compute.training_options_from_payload("3dgs", "quality", {
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
        options = compute.training_options_from_payload("3dgs", "max_quality", {
            "exposure_compensation": True,
        })

        command = compute.training_command("3dgs", "dataset", "output", options)

        self.assertIn("--antialiasing", command)
        self.assertIn("--optimizer_type", command)
        self.assertEqual(command[command.index("--optimizer_type") + 1], "sparse_adam")
        self.assertIn("--densify_grad_threshold", command)
        self.assertIn("--densification_interval", command)
        self.assertIn("--densify_until_iter", command)
        self.assertIn("--exposure_lr_init", command)
        self.assertEqual(command[command.index("--exposure_lr_init") + 1], "0.001")
        self.assertIn("--enable_gpu_preview", command)

    def test_sparse_adam_falls_back_when_accelerated_rasterizer_is_missing(self):
        options = compute.training_options_from_payload("3dgs", "quality", {
            "optimizer_type": "sparse_adam",
        })
        job = {"log": [], "updated_at": 1.0}

        with mock.patch.object(compute, "sparse_adam_available", return_value=False):
            resolved = compute.resolve_training_options_for_environment("3dgs", options, job)

        command = compute.training_command("3dgs", "dataset", "output", resolved)
        self.assertEqual(resolved["optimizer_type"], "default")
        self.assertNotIn("--optimizer_type", command)
        self.assertTrue(any("sparse_adam" in line and "default Adam" in line for line in job["log"]))

    def test_sparse_adam_is_kept_when_accelerated_rasterizer_is_available(self):
        options = compute.training_options_from_payload("3dgs", "quality", {
            "optimizer_type": "sparse_adam",
        })
        job = {"log": [], "updated_at": 1.0}

        with mock.patch.object(compute, "sparse_adam_available", return_value=True):
            resolved = compute.resolve_training_options_for_environment("3dgs", options, job)

        command = compute.training_command("3dgs", "dataset", "output", resolved)
        self.assertEqual(resolved["optimizer_type"], "sparse_adam")
        self.assertIn("--optimizer_type", command)
        self.assertEqual(command[command.index("--optimizer_type") + 1], "sparse_adam")

    def test_training_metadata_records_profile_and_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "model"
            options = compute.training_options_from_payload("3dgs", "quality", {
                "optimizer_type": "sparse_adam",
            })

            compute.write_training_metadata(output, "3dgs", "quality", options)

            backend_metadata = json.loads((output / "training_backend.json").read_text(encoding="utf-8"))
            scene_metadata = json.loads((output / "scene.json").read_text(encoding="utf-8"))
            self.assertEqual(backend_metadata["backend"], "3dgs")
            self.assertEqual(backend_metadata["quality"], "quality")
            self.assertEqual(scene_metadata["training"]["backend"], "3dgs")
            self.assertEqual(scene_metadata["training"]["quality"], "quality")
            self.assertEqual(scene_metadata["training"]["options"]["optimizer_type"], "sparse_adam")

    def test_training_metadata_retains_loaded_summary_for_both_backends(self):
        from native.worker.training_summary import configured_training_summary, loaded_training_summary
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "model"
                options = compute.training_options_from_payload(backend, "original_quality", {"iterations": 12345})
                summary = loaded_training_summary(configured_training_summary(backend, "original_quality", options),
                                                  [types.SimpleNamespace(image_width=1928, image_height=1084)])
                compute.write_training_metadata(output, backend, "original_quality", options, summary)
                metadata = json.loads((output / "scene.json").read_text(encoding="utf-8"))
                self.assertEqual(metadata["training"]["summary"], summary)
                self.assertEqual(metadata["training"]["options"], options)
                self.assertEqual(summary["iterations"], 12345)

    def test_configured_effective_summary_is_persisted_before_training_for_both_backends(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                dataset = root / "dataset"
                (dataset / "images").mkdir(parents=True)
                (dataset / "images" / "frame.jpg").write_bytes(b"fixture")
                (dataset / "transforms_train.json").write_text("{}", encoding="utf-8")
                job = {"id": "configured", "scene": "scene", "output_scene": "out", "backend": backend,
                       "status": "queued", "stage": "queued", "error": None, "cancel_requested": False,
                       "dataset_path": str(dataset), "train_options": {"iterations": 12345}, "log": []}
                observed = []
                def run(_job, _command, _directory, _backend):
                    if _command[1] != "train.py":
                        return
                    persisted = json.loads((root / "jobs" / "configured.json").read_text(encoding="utf-8"))
                    observed.append(persisted["trainingSummary"])
                    self.assertEqual(persisted["effective_train_options"]["iterations"], 12345)
                with mock.patch.object(compute, "OUTPUT_DIR", root / "output"), \
                     mock.patch.object(compute, "TRAIN_JOBS_DIR", root / "jobs"), \
                     mock.patch.object(compute, "ensure_training_environment", return_value={"python": "python", "colmap": "colmap", "two_dgs_dir": "2dgs"}), \
                     mock.patch.object(compute, "sparse_adam_available", return_value=False), \
                     mock.patch.object(compute, "run_logged", side_effect=run), \
                     mock.patch.object(compute, "training_point_cloud", return_value={"iteration": 12345, "path": "model.ply", "vertex_count": 42}):
                    compute.run_training_job(job, False, "original_quality", False)
                self.assertEqual(job["status"], "done", job.get("error"))
                self.assertEqual(len(observed), 1)
                self.assertEqual(observed[0]["phase"], "configured")
                self.assertEqual(observed[0]["optimizer"], "default" if backend == "3dgs" else "adam")
                self.assertEqual(observed[0]["iterations"], 12345)
                self.assertEqual(job["train_options"], {"iterations": 12345})

    def test_two_dgs_vram_warning_reports_selected_guard_and_density_schedule(self):
        from native.worker.training_configuration import normalize_runtime_options

        for enabled, mode in ((False, "pause"), (True, "pause"), (True, "defer")):
            with self.subTest(enabled=enabled, mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                dataset = root / "dataset"
                (dataset / "images").mkdir(parents=True)
                (dataset / "images/frame.jpg").write_bytes(b"owned CPU fixture")
                (dataset / "transforms_train.json").write_text("{}", encoding="utf-8")
                runtime = normalize_runtime_options({"version": 2, "resourceGuard": {
                    "enabled": enabled, "mode": mode, "max_consecutive_denials": 7}})["requested"]
                options = {"iterations": 1043, "resolution": 1, "densify_until_iter": 31,
                           "densification_interval": 13, "densify_grad_threshold": 0.00037}
                job = {"id": "warning", "scene": "capture", "output_scene": "result", "backend": "2dgs",
                       "status": "queued", "stage": "queued", "error": None, "cancel_requested": False,
                       "dataset_path": str(dataset), "train_options": options, "log": [],
                       "native_control": {"runtimeOptions": runtime}}
                commands = []
                with mock.patch.object(compute, "OUTPUT_DIR", root / "output"), \
                     mock.patch.object(compute, "TRAIN_JOBS_DIR", root / "jobs"), \
                     mock.patch.object(compute, "ensure_training_environment", return_value={"python": "python", "colmap": "colmap", "two_dgs_dir": "2dgs"}), \
                     mock.patch.object(compute, "run_logged", side_effect=lambda *args: commands.append(args[1])), \
                     mock.patch.object(compute, "training_point_cloud", return_value={"iteration": 1043, "path": "owned.ply", "vertex_count": 4}):
                    compute.run_training_job(job, False, "original_quality", False)
                self.assertEqual(job["status"], "done", job.get("error"))
                warning = next(line for line in job["log"] if "WARNING: 2DGS full/half-resolution" in line)
                self.assertIn("resource_guard=" + ("ON" if enabled else "OFF"), warning)
                self.assertIn("mode=" + mode, warning)
                self.assertIn("densify_until=31, densify_interval=13, densify_grad=0.00037", warning)
                self.assertNotIn("4GB", warning)
                self.assertNotIn("guard caps", warning)
                self.assertNotIn("7000/15000", warning)
                self.assertIn("CUDA out-of-memory remains possible", warning)
                if enabled and mode == "pause":
                    self.assertIn("pause after 7 consecutive denials", warning)
                elif enabled:
                    self.assertIn("defer growth without automatic resource pause", warning)
                else:
                    self.assertIn("software memory thresholds will not defer growth or pause", warning)
                self.assertEqual(job["effective_train_options"], compute.training_options_from_payload("2dgs", "original_quality", options))
                self.assertTrue(any("--iterations" in command and "1043" in command for command in commands))


    def test_training_job_persistence_round_trips_without_process_handle(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = compute.TRAIN_JOBS_DIR
            compute.TRAIN_JOBS_DIR = Path(tmp) / "training"
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

                compute.persist_train_job(job)
                loaded = compute.load_persisted_train_jobs()

                self.assertIn("job-1", loaded)
                self.assertEqual(loaded["job-1"]["scene"], "scene_a")
                self.assertIsNone(loaded["job-1"]["process"])
                self.assertEqual(loaded["job-1"]["train_options"]["iterations"], 7000)
            finally:
                compute.TRAIN_JOBS_DIR = original_jobs_dir

    def test_training_job_persistence_retries_transient_access_denied(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = compute.TRAIN_JOBS_DIR
            compute.TRAIN_JOBS_DIR = Path(tmp) / "training"
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

                with mock.patch.object(Path, "replace", flaky_replace), mock.patch.object(compute.time, "sleep"):
                    self.assertTrue(compute.persist_train_job(job))

                loaded = compute.load_persisted_train_jobs()
                self.assertIn("job-locked-once", loaded)
                self.assertGreaterEqual(calls["replace"], 2)
                self.assertNotIn("_persist_error", job)
            finally:
                compute.TRAIN_JOBS_DIR = original_jobs_dir

    def test_training_job_persistence_failure_does_not_raise(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_jobs_dir = compute.TRAIN_JOBS_DIR
            compute.TRAIN_JOBS_DIR = Path(tmp) / "training"
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

                with mock.patch.object(Path, "replace", side_effect=PermissionError("locked")), mock.patch.object(compute.time, "sleep"):
                    self.assertFalse(compute.persist_train_job(job))

                self.assertIn("_persist_error", job)
            finally:
                compute.TRAIN_JOBS_DIR = original_jobs_dir

    def test_run_logged_terminates_child_when_log_write_fails(self):
        class LoggingProcess:
            def __init__(self):
                self.stdout = io.StringIO("line\n")
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
            mock.patch.object(compute.subprocess, "Popen", return_value=process),
            mock.patch.object(compute, "training_env", return_value={}),
            mock.patch.object(compute, "add_job_log", side_effect=[None, RuntimeError("log write failed")]),
        ):
            with self.assertRaisesRegex(RuntimeError, "log write failed"):
                compute.run_logged(job, ["cmd"], Path("."))

        self.assertTrue(process.terminated)
        self.assertIsNone(job["process"])
        self.assertTrue(process.stdout.closed)


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
            "log": [],
        }
        with compute.MESH_LOCK:
            compute.MESH_JOBS[job["id"]] = job
        try:
            snapshot = compute.cancel_mesh_job(job["id"])

            self.assertTrue(process.terminated)
            self.assertTrue(snapshot["cancel_requested"])
            self.assertEqual(snapshot["status"], "cancelling")
            self.assertIn("Cancel requested.", snapshot["log"])
        finally:
            with compute.MESH_LOCK:
                compute.MESH_JOBS.pop(job["id"], None)


    def test_2dgs_environment_report_points_to_local_training_env(self):
        report = compute.training_environment_report("2dgs")

        self.assertEqual(report["backend"], "2dgs")
        self.assertEqual(Path(report["two_dgs_dir"]), compute.TWO_DGS_DIR)
        self.assertEqual(Path(report["two_dgs_python"]), compute.training_python("2dgs"))
        self.assertTrue(report["two_dgs_train"].endswith("train.py"))


    def test_2dgs_mesh_command_uses_render_script_and_expected_output_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp).resolve()
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp)
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text("Namespace(source_path=r'C:\\\\datasets\\\\mesh_scene', model_path='ignored', depth_ratio=0.0)", encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, cwd, output_path = compute.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {
                        "mode": "unbounded",
                        "mesh_res": 512,
                        "num_cluster": 12,
                        "depth_ratio": 0.0,
                    },
                )

                self.assertEqual(cwd, compute.TWO_DGS_DIR)
                self.assertEqual(command[1], "render.py")
                self.assertIn("--unbounded", [str(part) for part in command])
                self.assertIn("--skip_train", [str(part) for part in command])
                self.assertIn("--skip_test", [str(part) for part in command])
                self.assertIn("512", [str(part) for part in command])
                self.assertEqual(output_path, model / "train" / "ours_7000" / "fuse_unbounded_post.ply")
            finally:
                compute.OUTPUT_DIR = original_output

    def test_mesh_export_rejects_non_2dgs_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp)
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")

                with self.assertRaisesRegex(ValueError, "2DGS"):
                    compute.mesh_export_command("mesh_scene", 7000, {"mode": "bounded"})
            finally:
                compute.OUTPUT_DIR = original_output

    def test_sugar_mesh_command_uses_existing_3dgs_checkpoint(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp).resolve()
            original_output = compute.OUTPUT_DIR
            original_datasets = compute.DATASETS_DIR
            original_sugar = compute.SUGAR_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            compute.SUGAR_DIR = Path(tmp) / "SuGaR"
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                dataset = compute.DATASETS_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (dataset / "images").mkdir(parents=True, exist_ok=True)
                (dataset / "sparse" / "0").mkdir(parents=True, exist_ok=True)
                from PIL import Image
                Image.new("RGB", (101, 50), color=(120, 80, 30)).save(dataset / "images" / "frame.jpg")
                (dataset / "sparse" / "0" / "points3D.ply").write_text("ply\n", encoding="utf-8")
                compute.SUGAR_DIR.mkdir(parents=True, exist_ok=True)
                (compute.SUGAR_DIR / "train.py").write_text("print('sugar')", encoding="utf-8")
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

                command, cwd, output_path = compute.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {"mode": "sugar", "sugar_quality": "high"},
                )

                self.assertEqual(cwd, compute.SUGAR_DIR)
                self.assertEqual(command[1], compute.SUGAR_RUNNER)
                self.assertEqual(command[2], compute.SUGAR_DIR / "train.py")
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
                compute.OUTPUT_DIR = original_output
                compute.DATASETS_DIR = original_datasets
                compute.SUGAR_DIR = original_sugar

    def test_sugar_mesh_command_honors_quality_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_datasets = compute.DATASETS_DIR
            original_sugar = compute.SUGAR_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            compute.SUGAR_DIR = Path(tmp) / "SuGaR"
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                dataset = compute.DATASETS_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (dataset / "images").mkdir(parents=True, exist_ok=True)
                from PIL import Image
                Image.new("RGB", (64, 64), color=(120, 80, 30)).save(dataset / "images" / "frame.jpg")
                compute.SUGAR_DIR.mkdir(parents=True, exist_ok=True)
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

                command, _cwd, _output_path = compute.mesh_export_command(
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
                compute.OUTPUT_DIR = original_output
                compute.DATASETS_DIR = original_datasets
                compute.SUGAR_DIR = original_sugar

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

            stage, staged_source = compute.prepare_sugar_checkpoint(model, dataset, 7)

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

            stage, staged_source = compute.prepare_sugar_checkpoint(
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
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp)
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")

                with self.assertRaisesRegex(ValueError, "3DGS"):
                    compute.mesh_export_command("mesh_scene", 7000, {"mode": "sugar"})
            finally:
                compute.OUTPUT_DIR = original_output

    def test_gs2mesh_mesh_command_uses_existing_3dgs_checkpoint_and_colmap(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp).resolve()
            original_output = compute.OUTPUT_DIR
            original_datasets = compute.DATASETS_DIR
            original_gs2mesh = getattr(compute, "GS2MESH_DIR", None)
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                dataset = compute.DATASETS_DIR / "mesh_scene"
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
                compute.GS2MESH_DIR.mkdir(parents=True, exist_ok=True)
                (compute.GS2MESH_DIR / "run_single.py").write_text("print('gs2mesh')", encoding="utf-8")
                (model / "training_backend.json").write_text(json.dumps({"backend": "3dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                (model / "cameras.json").write_text("[]", encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, cwd, output_path = compute.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {
                        "mode": "gs2mesh",
                        "task_port": 6113,
                        "gs2mesh_downsample": 2,
                        "gs2mesh_baseline_percentage": 7,
                        "gs2mesh_tsdf_voxel": 2,
                        "gs2mesh_tsdf_min_depth_baselines": 4,
                        "gs2mesh_tsdf_max_depth_baselines": 20,
                    },
                )

                command_text = [str(part) for part in command]
                self.assertEqual(cwd, compute.GS2MESH_DIR)
                self.assertIn("run_single.py", command_text)
                self.assertEqual(command_text[command_text.index("--experiment_folder_name") + 1], "app")
                self.assertEqual(command_text[command_text.index("--renderer_folder_name") + 1], "mesh_scene_d2")
                self.assertIn("--skip_video_extraction", command_text)
                self.assertIn("--skip_colmap", command_text)
                self.assertIn("--skip_GS", command_text)
                self.assertNotIn("--skip_rendering", command_text)
                self.assertEqual(command_text[command_text.index("--GS_iterations") + 1], "7000")
                self.assertEqual(command_text[command_text.index("--GS_port") + 1], "6113")
                self.assertEqual(command_text[command_text.index("--colmap_name") + 1], "mesh_scene")
                self.assertNotIn("--renderer_max_images", command_text)
                self.assertEqual(command_text[command_text.index("--downsample") + 1], "2")
                staged_dataset = compute.GS2MESH_DIR / "data" / "custom" / "mesh_scene"
                staged_downsampled = compute.GS2MESH_DIR / "data" / "custom" / "mesh_scene_downsample2"
                staged_model = compute.GS2MESH_DIR / "splatting_output" / "custom_nw_iterations7000" / "mesh_scene"
                staged_downsampled_model = compute.GS2MESH_DIR / "splatting_output" / "custom_nw_iterations7000" / "mesh_scene_downsample2"
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
                compute.OUTPUT_DIR = original_output
                compute.DATASETS_DIR = original_datasets
                if original_gs2mesh is not None:
                    compute.GS2MESH_DIR = original_gs2mesh

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
                return compute.subprocess.CompletedProcess(args, 0, stdout="", stderr="")

            with mock.patch.object(compute, "colmap_executable", return_value=colmap), \
                    mock.patch.object(compute, "training_env", side_effect=AssertionError("training_env should not be used")), \
                    mock.patch.object(compute.subprocess, "run", side_effect=fake_run):
                compute.convert_colmap_model_to_text(sparse)

            self.assertTrue((sparse / "images.txt").exists())

    def test_gs2mesh_mesh_command_reuses_completed_render_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_datasets = compute.DATASETS_DIR
            original_gs2mesh = getattr(compute, "GS2MESH_DIR", None)
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.DATASETS_DIR = Path(tmp) / "datasets"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                dataset = compute.DATASETS_DIR / "mesh_scene"
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
                compute.GS2MESH_DIR.mkdir(parents=True, exist_ok=True)
                (compute.GS2MESH_DIR / "run_single.py").write_text("print('gs2mesh')", encoding="utf-8")
                legacy = (
                    compute.GS2MESH_DIR
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

                command, _cwd, _output_path = compute.mesh_export_command(
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
                reused = compute.GS2MESH_DIR / "output" / "app" / "mesh_scene_d2" / "000"
                self.assertTrue((reused / "left.png").exists())
                self.assertTrue((reused / "out_DLNR_Middlebury" / "depth.npy").exists())
            finally:
                compute.OUTPUT_DIR = original_output
                compute.DATASETS_DIR = original_datasets
                if original_gs2mesh is not None:
                    compute.GS2MESH_DIR = original_gs2mesh


    def test_collect_sugar_outputs_creates_preview_mesh_and_texture_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_sugar = compute.SUGAR_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.SUGAR_DIR = Path(tmp) / "SuGaR"
            try:
                dataset = Path(tmp) / "datasets" / "mesh_scene"
                sugar_dir = compute.SUGAR_DIR / "output" / "refined_mesh" / "mesh_scene"
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

                result = compute.collect_sugar_mesh_outputs("mesh_scene", 7000, dataset, 0)
                texture_paths = compute.mesh_texture_paths("mesh_scene", 7000, "sugar", post=True)
                preview_ply = compute.mesh_output_path("mesh_scene", 7000, "sugar", post=True)

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
                compute.OUTPUT_DIR = original_output
                compute.SUGAR_DIR = original_sugar


    def test_texture_options_default_to_openmvs_backend(self):
        options = compute.bake_texture_options({"mode": "bounded", "texture_res": 4096})

        self.assertEqual(options["backend"], "openmvs")
        self.assertEqual(options["texture_res"], 4096)
        self.assertEqual(options["bake_source"], "photo")
        self.assertEqual(options["max_faces"], 100000)
        self.assertFalse(options["local_seam_leveling"])
        self.assertEqual(options["cost_smoothness_ratio"], 0.3)
        self.assertEqual(options["texture_size_multiple"], 0)

    def test_texture_options_accept_colmap_backend(self):
        options = compute.bake_texture_options({"mode": "bounded", "texture_res": 4096, "backend": "colmap"})

        self.assertEqual(options["backend"], "colmap")
        self.assertEqual(options["max_faces"], 200000)
        self.assertEqual(options["bake_source"], "photo")

    def test_texture_bake_supports_2dgs_and_gs2mesh(self):
        self.assertTrue(compute.mesh_mode_supports_texture_bake("bounded", "2dgs"))
        self.assertTrue(compute.mesh_mode_supports_texture_bake("unbounded", "2dgs"))
        self.assertTrue(compute.mesh_mode_supports_texture_bake("gs2mesh", "3dgs"))
        self.assertFalse(compute.mesh_mode_supports_texture_bake("bounded", "3dgs"))
        self.assertFalse(compute.mesh_mode_supports_texture_bake("gs2mesh", "2dgs"))
        self.assertFalse(compute.mesh_mode_supports_texture_bake("sugar", "3dgs"))

    def test_texture_options_accept_ultra_openmvs_smoothing(self):
        options = compute.bake_texture_options({
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
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(compute.os.environ, {"OPENMVS_BIN": ""}):
            tmp = Path(tmp).resolve()
            base = Path(tmp)
            original_output = compute.OUTPUT_DIR
            original_openmvs = compute.OPENMVS_DIR
            compute.OUTPUT_DIR = base / "output"
            compute.OPENMVS_DIR = base / "openMVS"
            try:
                bin_dir = compute.OPENMVS_DIR / "build" / "bin" / "x64" / "Release"
                bin_dir.mkdir(parents=True, exist_ok=True)
                suffix = ".exe" if sys.platform == "win32" else ""
                interface_colmap = bin_dir / f"InterfaceCOLMAP{suffix}"
                texture_mesh = bin_dir / f"TextureMesh{suffix}"
                for tool in (interface_colmap, texture_mesh):
                    tool.write_text("" if suffix else "#!/bin/sh\nexit 99\n", encoding="utf-8")
                    if not suffix:
                        tool.chmod(0o755)

                dataset = base / "dataset"
                (dataset / "images").mkdir(parents=True)
                sparse0 = dataset / "sparse" / "0"
                sparse0.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse0 / name).write_bytes(b"test")

                model = compute.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True, exist_ok=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 400000\nend_header\n", encoding="ascii")

                commands, work_dir, output_stem = compute.openmvs_texture_commands(
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
                compute.OUTPUT_DIR = original_output
                compute.OPENMVS_DIR = original_openmvs

    def test_openmvs_texture_commands_reuse_existing_dense_colmap_export(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(compute.os.environ, {"OPENMVS_BIN": ""}):
            base = Path(tmp)
            original_output = compute.OUTPUT_DIR
            original_openmvs = compute.OPENMVS_DIR
            compute.OUTPUT_DIR = base / "output"
            compute.OPENMVS_DIR = base / "openMVS"
            try:
                bin_dir = compute.OPENMVS_DIR / "build" / "bin" / "x64" / "Release"
                bin_dir.mkdir(parents=True, exist_ok=True)
                suffix = ".exe" if sys.platform == "win32" else ""
                interface_colmap = bin_dir / f"InterfaceCOLMAP{suffix}"
                texture_mesh = bin_dir / f"TextureMesh{suffix}"
                for tool in (interface_colmap, texture_mesh):
                    tool.write_text("" if suffix else "#!/bin/sh\nexit 99\n", encoding="utf-8")
                    if not suffix:
                        tool.chmod(0o755)

                dataset = base / "dataset"
                (dataset / "dense" / "images").mkdir(parents=True)
                (dataset / "dense" / "sparse").mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (dataset / "dense" / "sparse" / name).write_bytes(b"test")

                model = compute.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}', model_path='ignored')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True, exist_ok=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 100\nend_header\n", encoding="ascii")

                commands, _, _ = compute.openmvs_texture_commands(
                    "scene_2dgs",
                    7,
                    {"mode": "bounded", "post": True, "texture_res": 2048, "max_faces": 0},
                )

                self.assertEqual(len(commands), 2)
                self.assertEqual(commands[0][0], interface_colmap)
                self.assertIn(str(dataset / "dense"), [str(part) for part in commands[0]])
                self.assertEqual(commands[1][0], texture_mesh)
            finally:
                compute.OUTPUT_DIR = original_output
                compute.OPENMVS_DIR = original_openmvs

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

            workspace = compute.prepare_colmap_texturer_workspace(dataset, base / "work")

            self.assertTrue((workspace / "sparse" / "cameras.bin").exists())
            self.assertTrue((workspace / "sparse" / "images.bin").exists())
            self.assertTrue((workspace / "images" / "frames" / "frame_000.jpg").exists())

    def test_colmap_texturer_command_rebuilds_stale_workspace(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = base / "output"
            try:
                dataset = base / "dataset"
                sparse0 = dataset / "sparse" / "0"
                images = dataset / "images"
                sparse0.mkdir(parents=True)
                images.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse0 / name).write_bytes(b"data")
                (images / "frame_000.jpg").write_bytes(b"jpg")

                model = compute.OUTPUT_DIR / "scene_2dgs"
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

                with mock.patch.object(compute, "colmap_executable", return_value=Path("colmap.exe")):
                    _, work_dir, _ = compute.colmap_texturer_command(
                        "scene_2dgs",
                        7,
                        {"mode": "bounded", "post": True, "texture_res": 8192},
                    )

                self.assertTrue((work_dir / "workspace" / "images" / "frame_000.jpg").exists())
                self.assertTrue(cache_marker.exists())
                self.assertFalse((work_dir / "output" / "old.obj").exists())
            finally:
                compute.OUTPUT_DIR = original_output

    def test_colmap_texturer_command_uses_texture_scale_factor_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = base / "output"
            try:
                dataset = base / "dataset"
                sparse0 = dataset / "sparse" / "0"
                images = dataset / "images"
                sparse0.mkdir(parents=True)
                images.mkdir(parents=True)
                for name in ("cameras.bin", "images.bin", "points3D.bin"):
                    (sparse0 / name).write_bytes(b"data")
                (images / "frame_000.jpg").write_bytes(b"jpg")

                model = compute.OUTPUT_DIR / "scene_2dgs"
                model.mkdir(parents=True)
                (model / "cfg_args").write_text(f"Namespace(source_path=r'{dataset}')", encoding="utf-8")
                mesh_path = model / "train" / "ours_7" / "fuse_post.ply"
                mesh_path.parent.mkdir(parents=True)
                mesh_path.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 0\nend_header\n", encoding="ascii")

                with mock.patch.object(compute, "colmap_executable", return_value=Path("colmap.exe")):
                    command, _, _ = compute.colmap_texturer_command(
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
                compute.OUTPUT_DIR = original_output

    def test_colmap_texturer_caps_scale_for_large_mesh(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            small = base / "small.ply"
            large = base / "large.ply"
            small.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 100\nend_header\n", encoding="ascii")
            large.write_text("ply\nformat ascii 1.0\nelement vertex 0\nelement face 3000000\nend_header\n", encoding="ascii")

            small_scale, small_faces, small_reason = compute.colmap_texture_scale_factor(small, 8192)
            large_scale, large_faces, large_reason = compute.colmap_texture_scale_factor(large, 8192)

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

        with mock.patch.object(compute, "openmvs_texture_commands", side_effect=fake_commands) as commands_mock, \
                mock.patch.object(compute, "run_logged", side_effect=[RuntimeError("TextureMesh crashed"), None]), \
                mock.patch.object(compute, "collect_openmvs_texture_outputs", return_value={"zip": "texture.zip"}):
            collected = compute.run_openmvs_texture_bake(job, output_paths, options)

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
            compute.clear_texture_outputs(paths)

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

            compute.collect_openmvs_texture_outputs(work_dir, paths)

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

            with mock.patch.object(compute, "export_textured_obj_to_glb", side_effect=RuntimeError("missing trimesh")):
                result = compute.collect_openmvs_texture_outputs(work_dir, paths)

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

            result = compute.bake_texture_from_vertex_colors(mesh_path, paths, texture_res=256, padding=2)

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

            result = compute.collect_colmap_texture_outputs(work_dir, paths)

            self.assertEqual(result["texture_pages"], 1)
            obj_text = paths["obj"].read_text(encoding="utf-8")
            self.assertIn("mtllib mesh.mtl", obj_text)
            self.assertIn("vt 1 0", obj_text)
            self.assertIn("f 1/1 2/2 3/3", obj_text)
            self.assertTrue(paths["zip"].exists())


    def test_gs2mesh_environment_rejects_shared_sugar_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_gs2mesh_dir = compute.GS2MESH_DIR
            original_sugar_dir = compute.SUGAR_DIR
            gs2mesh_dir = Path(tmp) / "gs2mesh"
            sugar_dir = Path(tmp) / "sugar"
            shared_python = Path(tmp) / "envs" / "shared" / "python.exe"
            gs2mesh_dir.mkdir(parents=True)
            sugar_dir.mkdir(parents=True)
            shared_python.parent.mkdir(parents=True)
            shared_python.write_bytes(b"")
            (gs2mesh_dir / "run_single.py").write_text("print('gs2mesh')", encoding="utf-8")
            compute.GS2MESH_DIR = gs2mesh_dir
            compute.SUGAR_DIR = sugar_dir
            try:
                with mock.patch.object(compute, "gs2mesh_python", return_value=shared_python), \
                        mock.patch.object(compute, "sugar_python", return_value=shared_python):
                    with self.assertRaisesRegex(RuntimeError, "must use different Python environments"):
                        compute.ensure_gs2mesh_environment()
            finally:
                compute.GS2MESH_DIR = original_gs2mesh_dir
                compute.SUGAR_DIR = original_sugar_dir

    def test_recover_2dgs_post_mesh_uses_fresh_raw_mesh_and_runtime_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            try:
                raw_mesh = compute.mesh_output_path("mesh_scene", 7, "bounded", post=False)
                post_mesh = compute.mesh_output_path("mesh_scene", 7, "bounded", post=True)
                raw_mesh.parent.mkdir(parents=True, exist_ok=True)
                raw_mesh.write_bytes(b"fresh raw mesh")
                started_at = raw_mesh.stat().st_mtime

                def fake_run_logged(job, command, cwd, backend):
                    self.assertEqual(command[0], Path("runtime-python.exe"))
                    self.assertEqual(command[1], compute.ROOT / "scripts" / "postprocess_2dgs_mesh.py")
                    self.assertEqual(Path(command[command.index("--input") + 1]), raw_mesh)
                    self.assertEqual(Path(command[command.index("--output") + 1]), post_mesh)
                    self.assertEqual(command[command.index("--num-cluster") + 1], "50")
                    self.assertEqual(cwd, compute.ROOT)
                    self.assertEqual(backend, "2dgs")
                    post_mesh.write_bytes(b"processed mesh")

                job = {"backend": "2dgs", "log": []}
                with mock.patch.object(compute, "training_python", return_value=Path("runtime-python.exe")), \
                        mock.patch.object(compute, "run_logged", side_effect=fake_run_logged):
                    recovered = compute.recover_2dgs_post_mesh(
                        job, "mesh_scene", 7, "bounded", 50, started_at
                    )

                self.assertTrue(recovered)
                self.assertTrue(post_mesh.exists())
            finally:
                compute.OUTPUT_DIR = original_output

    def test_gs2mesh_environment_does_not_inherit_other_conda_or_sugar_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            env_root = base / "miniforge3" / "envs" / "gs2mesh"
            other_env = base / "miniforge3" / "envs" / "gaussian_splatting"
            sugar_source = base / "SuGaR" / "gaussian_splatting"
            system_bin = base / "system-bin"
            inherited = compute.os.pathsep.join([str(other_env), str(system_bin)])

            with mock.patch.dict(
                compute.os.environ,
                {"PATH": inherited, "PYTHONPATH": str(sugar_source)},
                clear=True,
            ), mock.patch.object(compute, "gs2mesh_env_root", return_value=env_root):
                environment = compute.gs2mesh_env()

            self.assertNotIn(str(other_env), environment["PATH"])
            self.assertIn(str(system_bin), environment["PATH"])
            self.assertNotIn("PYTHONPATH", environment)
            self.assertEqual(environment["CONDA_PREFIX"], str(env_root))
            self.assertEqual(environment["PYTHONNOUSERSITE"], "1")

    def test_2dgs_bounded_mesh_command_derives_voxel_from_gaussian_extent(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp)
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text("Namespace(source_path=r'C:\\\\datasets\\\\mesh_scene')", encoding="utf-8")
                core = np.column_stack(
                    [
                        np.linspace(-10.0, 10.0, 100, dtype=np.float32),
                        np.zeros(100, dtype=np.float32),
                        np.zeros(100, dtype=np.float32),
                    ]
                )
                xyz = np.vstack([core, np.array([[10000.0, 0.0, 0.0]], dtype=np.float32)])
                write_test_ply(model / "point_cloud" / "iteration_7000" / "point_cloud.ply", xyz)

                command, _cwd, _output_path = compute.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {"mode": "bounded", "mesh_res": 512},
                )

                command_text = [str(part) for part in command]
                voxel_size = float(command_text[command_text.index("--voxel_size") + 1])
                sdf_trunc = float(command_text[command_text.index("--sdf_trunc") + 1])
                expected_extent = float(np.max(np.diff(np.percentile(xyz, [1.0, 99.0], axis=0), axis=0)))
                self.assertAlmostEqual(voxel_size, expected_extent / 512, places=7)
                self.assertAlmostEqual(sdf_trunc, voxel_size * 5.0, places=7)
                self.assertLess(voxel_size, 0.05)
                self.assertNotIn("--depth_trunc", command_text)
            finally:
                compute.OUTPUT_DIR = original_output

    def test_gs2mesh_env_root_uses_dedicated_conda_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            conda_root = Path(tmp) / "miniforge3"
            gs2mesh_python = conda_root / "envs" / "gs2mesh" / "python.exe"
            gs2mesh_python.parent.mkdir(parents=True)
            gs2mesh_python.write_bytes(b"")

            with mock.patch.dict(compute.os.environ, {}, clear=True), \
                    mock.patch.object(compute, "conda_root_candidates", return_value=[conda_root]), \
                    mock.patch.object(compute, "install_drive_root", return_value=None), \
                    mock.patch.object(compute.Path, "home", return_value=Path(tmp) / "home"):
                resolved = compute.gs2mesh_env_root()

            self.assertEqual(resolved, gs2mesh_python.parent.resolve())

    def test_gs2mesh_two_stage_commands_keep_source_scale(self):
        options = compute.mesh_export_options(
            {"mode": "gs2mesh", "task_port": 6113, "gs2mesh_auto_scale": True}
        )
        render = [str(value) for value in compute.build_gs2mesh_command("mesh_scene", 7000, options, skip_tsdf=True)]
        fusion = [str(value) for value in compute.build_gs2mesh_command("mesh_scene", 7000, options, skip_rendering=True)]

        self.assertIn("--skip_TSDF", render)
        self.assertNotIn("--skip_rendering", render)
        self.assertIn("--skip_rendering", fusion)
        self.assertNotIn("--skip_TSDF", fusion)
        self.assertEqual(fusion[fusion.index("--TSDF_scale") + 1], "1.0")

    def test_2dgs_mesh_job_recovers_when_upstream_post_process_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            try:
                raw_mesh = compute.mesh_output_path("mesh_scene", 7, "bounded", post=False)
                post_mesh = compute.mesh_output_path("mesh_scene", 7, "bounded", post=True)
                calls = []

                def fake_run_logged(job, command, cwd, backend):
                    calls.append([str(part) for part in command])
                    if len(calls) == 1:
                        raw_mesh.parent.mkdir(parents=True, exist_ok=True)
                        raw_mesh.write_bytes(b"fresh raw mesh")
                        raise RuntimeError("upstream connected-component post-process failed")
                    post_mesh.write_bytes(b"recovered mesh")

                job = {
                    "id": "mesh-job",
                    "kind": "mesh",
                    "scene": "mesh_scene",
                    "iteration": 7,
                    "mode": "bounded",
                    "options": {"mode": "bounded", "num_cluster": 50},
                    "status": "queued",
                    "stage": "queued",
                    "log": [],
                    "cancel_requested": False,
                }
                with mock.patch.object(compute, "ensure_mesh_environment"), \
                        mock.patch.object(
                            compute,
                            "mesh_export_command",
                            return_value=([Path("runtime-python.exe"), "render.py"], Path(tmp), post_mesh),
                        ), \
                        mock.patch.object(compute, "training_python", return_value=Path("runtime-python.exe")), \
                        mock.patch.object(compute, "run_logged", side_effect=fake_run_logged):
                    compute.run_mesh_export_job(job)

                self.assertEqual(job["status"], "done")
                self.assertEqual(job["stage"], "done")
                self.assertEqual(Path(job["output_mesh"]), post_mesh)
                self.assertTrue(post_mesh.exists())
                self.assertEqual(len(calls), 2)
                self.assertIn("postprocess_2dgs_mesh.py", calls[1][1])
            finally:
                compute.OUTPUT_DIR = original_output

    def test_collect_gs2mesh_output_copies_fresh_mesh_from_current_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_gs2mesh = getattr(compute, "GS2MESH_DIR", None)
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                source = (
                    compute.GS2MESH_DIR
                    / "output"
                    / "app"
                    / "mesh_scene_d2"
                    / "mesh_scene_downsample2_custom_nw_iterations7_DLNR_Middlebury_baseline7_0p_"
                    "mask0_occ1_scale1_0_voxel2_512_trunc4_20_cleaned_mesh.ply"
                )
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(
                    "\n".join([
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
                    ]),
                    encoding="utf-8",
                )

                result = compute.collect_gs2mesh_mesh_output(
                    "mesh_scene",
                    7,
                    {"mode": "gs2mesh", "gs2mesh_downsample": 2},
                    0,
                )

                target = compute.mesh_output_path("mesh_scene", 7, "gs2mesh", post=True)
                self.assertEqual(result["mesh"], str(target))
                self.assertEqual(target.read_text(encoding="utf-8"), source.read_text(encoding="utf-8"))
                self.assertEqual(result["source"], str(source))
            finally:
                compute.OUTPUT_DIR = original_output
                if original_gs2mesh is not None:
                    compute.GS2MESH_DIR = original_gs2mesh

    def test_task_port_allocator_skips_external_and_app_managed_conflicts(self):
        job_a = {"id": "job-a"}
        job_b = {"id": "job-b"}
        compute.TASK_PORT_OWNERS.clear()
        try:
            with mock.patch.object(compute, "task_port_available", side_effect=lambda port: port != 6009):
                port_a = compute.reserve_task_port(job_a, "3DGS training", preferred=6009, start=6110, end=6111)
                port_b = compute.reserve_task_port(job_b, "2DGS training", preferred=6009, start=6110, end=6111)

            self.assertEqual(port_a, 6110)
            self.assertEqual(port_b, 6111)
            self.assertEqual(job_a["task_port"], 6110)
            self.assertEqual(job_b["task_port"], 6111)
        finally:
            compute.release_task_port(job_a)
            compute.release_task_port(job_b)
            compute.TASK_PORT_OWNERS.clear()

    def test_gs2mesh_auto_scale_expands_depth_window_and_voxel_without_transform(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "cache"
            model = Path(tmp) / "output" / "mesh_scene"
            camera_data = []
            for index, depth_value in enumerate((10.0, 15.0)):
                stereo = cache / f"{index:03d}" / "out_DLNR_Middlebury"
                stereo.mkdir(parents=True)
                np.save(stereo / "depth.npy", np.full((32, 32), depth_value, dtype=np.float32))
                np.save(stereo / "occlusion_mask.npy", np.ones((32, 32), dtype=np.uint8))
                camera_data.append({"left": {"baseline": 0.25, "pos": [float(index), 0.0, 0.0]}})
            (cache / "camera_data.json").write_text(json.dumps(camera_data), encoding="utf-8")
            write_test_ply(
                model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                np.array([[-12.0, 0.0, 0.0], [12.0, 0.0, 0.0]], dtype=np.float32),
            )
            requested = compute.mesh_export_options(
                {
                    "mode": "gs2mesh",
                    "gs2mesh_tsdf_voxel": 2,
                    "gs2mesh_tsdf_min_depth_baselines": 4,
                    "gs2mesh_tsdf_max_depth_baselines": 20,
                    "gs2mesh_auto_scale": True,
                }
            )

            profile = compute.profile_gs2mesh_render_cache(cache, requested)
            effective, report = compute.resolve_gs2mesh_scale_options(requested, profile, model, 7000)

            self.assertEqual(profile["requested_coverage"], 0.0)
            self.assertEqual(effective["gs2mesh_tsdf_voxel"], 8)
            self.assertEqual(effective["gs2mesh_tsdf_min_depth_baselines"], 4)
            self.assertEqual(effective["gs2mesh_tsdf_max_depth_baselines"], 80)
            self.assertEqual(report["source_coordinates"], "preserved")
            self.assertEqual(report["tsdf_scale"], 1.0)

    def test_collect_gs2mesh_output_rejects_stale_mesh_from_same_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_gs2mesh = getattr(compute, "GS2MESH_DIR", None)
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                source = (
                    compute.GS2MESH_DIR
                    / "output"
                    / "app"
                    / "mesh_scene_d2"
                    / "mesh_scene_downsample2_custom_nw_iterations7_DLNR_Middlebury_baseline7_0p_"
                    "mask0_occ1_scale1_0_voxel2_512_trunc4_20_cleaned_mesh.ply"
                )
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(
                    "ply\nformat ascii 1.0\nelement vertex 3\nelement face 1\nend_header\n",
                    encoding="utf-8",
                )
                os.utime(source, (10, 10))

                with self.assertRaisesRegex(FileNotFoundError, "fresh cleaned mesh"):
                    compute.collect_gs2mesh_mesh_output(
                        "mesh_scene",
                        7,
                        {"mode": "gs2mesh", "gs2mesh_downsample": 2},
                        100,
                    )
            finally:
                compute.OUTPUT_DIR = original_output
                if original_gs2mesh is not None:
                    compute.GS2MESH_DIR = original_gs2mesh

    def test_2dgs_bounded_mesh_command_preserves_manual_tsdf_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp)
            try:
                model = compute.OUTPUT_DIR / "mesh_scene"
                model.mkdir(parents=True, exist_ok=True)
                (model / "training_backend.json").write_text(json.dumps({"backend": "2dgs"}), encoding="utf-8")
                (model / "cfg_args").write_text("Namespace(source_path=r'C:\\\\datasets\\\\mesh_scene')", encoding="utf-8")
                write_test_ply(
                    model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                    np.array([[0.0, 0.0, 0.0], [20.0, 0.0, 0.0]], dtype=np.float32),
                )

                command, _cwd, _output_path = compute.mesh_export_command(
                    "mesh_scene",
                    7000,
                    {
                        "mode": "bounded",
                        "mesh_res": 512,
                        "voxel_size": 0.25,
                        "depth_trunc": 80.0,
                        "sdf_trunc": 1.5,
                    },
                )

                command_text = [str(part) for part in command]
                self.assertEqual(command_text[command_text.index("--voxel_size") + 1], "0.25")
                self.assertEqual(command_text[command_text.index("--depth_trunc") + 1], "80.0")
                self.assertEqual(command_text[command_text.index("--sdf_trunc") + 1], "1.5")
            finally:
                compute.OUTPUT_DIR = original_output

    def test_2dgs_mesh_options_validate_manual_quality_controls(self):
        options = compute.mesh_export_options(
            {
                "mode": "bounded",
                "mesh_res": 1024,
                "num_cluster": 12,
                "depth_ratio": 0.25,
                "voxel_size": 0.01,
                "sdf_trunc": 0.05,
                "depth_trunc": 40.0,
            }
        )
        self.assertEqual(options["mesh_res"], 1024)
        self.assertEqual(options["num_cluster"], 12)
        self.assertEqual(options["depth_ratio"], 0.25)
        self.assertEqual(options["voxel_size"], 0.01)
        self.assertEqual(options["sdf_trunc"], 0.05)
        self.assertEqual(options["depth_trunc"], 40.0)

        for key in ("voxel_size", "sdf_trunc", "depth_trunc"):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, key):
                compute.mesh_export_options({"mode": "bounded", key: 0})
        with self.assertRaisesRegex(ValueError, "depth_ratio"):
            compute.mesh_export_options({"mode": "bounded", "depth_ratio": 1.1})
        with self.assertRaisesRegex(ValueError, "finite"):
            compute.mesh_export_options({"mode": "bounded", "voxel_size": float("nan")})

    def test_sugar_environment_does_not_inherit_gs2mesh_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            env_root = base / "miniforge3" / "envs" / "sugar"
            gs2mesh_env = base / "miniforge3" / "envs" / "gs2mesh"
            system_bin = base / "system-bin"
            inherited = compute.os.pathsep.join([str(gs2mesh_env), str(system_bin)])

            with mock.patch.dict(
                compute.os.environ,
                {"PATH": inherited, "PYTHONPATH": str(base / "gs2mesh")},
                clear=True,
            ), mock.patch.object(compute, "sugar_env_root", return_value=env_root):
                environment = compute.sugar_env()

            self.assertNotIn(str(gs2mesh_env), environment["PATH"])
            self.assertIn(str(system_bin), environment["PATH"])
            self.assertNotIn("PYTHONPATH", environment)
            self.assertEqual(environment["CONDA_PREFIX"], str(env_root))

    def test_gs2mesh_legacy_cache_rejects_changed_orientation_and_maps_camera_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            colmap = Path(tmp) / "colmap"
            sparse = colmap / "sparse" / "0"
            sparse.mkdir(parents=True)
            (sparse / "cameras.txt").write_text(
                "7 PINHOLE 200 100 150 151 100 50\n"
                "9 PINHOLE 400 300 320 321 200 150\n",
                encoding="utf-8",
            )
            (sparse / "images.txt").write_text(
                "2 1 0 0 0 -1 0 0 9 second.jpg\n\n"
                "1 1 0 0 0 0 0 0 7 first.jpg\n\n",
                encoding="utf-8",
            )
            cache = Path(tmp) / "cache"
            cache.mkdir()
            camera_data = [
                {
                    "left": {
                        "baseline": 0.035,
                        "pos": [0.0, 0.0, 0.0],
                        "width": 200,
                        "height": 100,
                        "fx": 150,
                        "fy": 151,
                        "cx": 100,
                        "cy": 50,
                        "extrinsic": np.eye(4).tolist(),
                    }
                },
                {
                    "left": {
                        "baseline": 0.035,
                        "pos": [1.0, 0.0, 0.0],
                        "width": 400,
                        "height": 300,
                        "fx": 320,
                        "fy": 321,
                        "cx": 200,
                        "cy": 150,
                        "extrinsic": np.asarray(
                            [[1, 0, 0, 1], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                            dtype=float,
                        ).tolist(),
                    }
                },
            ]
            camera_data_path = cache / "camera_data.json"
            camera_data_path.write_text(json.dumps(camera_data), encoding="utf-8")
            signature = {
                "scene_360": True,
                "baseline_percentage": 7.0,
                "point_cloud": {"mtime_ns": 0},
                "runner": {"mtime_ns": 0},
            }

            self.assertTrue(compute.gs2mesh_legacy_cache_matches(cache, colmap, signature))

            camera_data[0]["left"]["extrinsic"][0][0] = -1.0
            camera_data_path.write_text(json.dumps(camera_data), encoding="utf-8")
            self.assertFalse(compute.gs2mesh_legacy_cache_matches(cache, colmap, signature))

    def test_gs2mesh_environment_check_rejects_missing_dedicated_runtime_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_dir = compute.GS2MESH_DIR
            gs2mesh_dir = Path(tmp) / "gs2mesh"
            python_path = Path(tmp) / "envs" / "gs2mesh" / "python.exe"
            gs2mesh_dir.mkdir(parents=True)
            python_path.parent.mkdir(parents=True)
            (gs2mesh_dir / "run_single.py").write_text("print('gs2mesh')", encoding="utf-8")
            python_path.write_bytes(b"")
            compute.GS2MESH_DIR = gs2mesh_dir
            try:
                with mock.patch.object(compute, "gs2mesh_python", return_value=python_path), \
                        mock.patch.object(compute, "python_probe", return_value=(False, "No module named simple_knn")):
                    with self.assertRaisesRegex(RuntimeError, "GS2Mesh runtime imports failed"):
                        compute.ensure_gs2mesh_environment()
            finally:
                compute.GS2MESH_DIR = original_dir

    def test_recover_2dgs_post_mesh_rejects_stale_raw_mesh(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            try:
                raw_mesh = compute.mesh_output_path("mesh_scene", 7, "bounded", post=False)
                raw_mesh.parent.mkdir(parents=True, exist_ok=True)
                raw_mesh.write_bytes(b"stale raw mesh")
                started_at = raw_mesh.stat().st_mtime + 60.0

                job = {"backend": "2dgs", "log": []}
                with mock.patch.object(compute, "run_logged") as run_mock:
                    recovered = compute.recover_2dgs_post_mesh(
                        job, "mesh_scene", 7, "bounded", 50, started_at
                    )

                self.assertFalse(recovered)
                run_mock.assert_not_called()
            finally:
                compute.OUTPUT_DIR = original_output

    def test_sugar_environment_check_imports_native_runtime_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_sugar_dir = compute.SUGAR_DIR
            original_gs2mesh_dir = compute.GS2MESH_DIR
            sugar_dir = Path(tmp) / "SuGaR"
            gs2mesh_dir = Path(tmp) / "GS2Mesh"
            sugar_python = Path(tmp) / "envs" / "sugar" / "python.exe"
            gs2mesh_python = Path(tmp) / "envs" / "gs2mesh" / "python.exe"
            sugar_dir.mkdir(parents=True)
            gs2mesh_dir.mkdir(parents=True)
            sugar_python.parent.mkdir(parents=True)
            gs2mesh_python.parent.mkdir(parents=True)
            sugar_python.write_bytes(b"")
            gs2mesh_python.write_bytes(b"")
            (sugar_dir / "train.py").write_text("print('sugar')", encoding="utf-8")
            compute.SUGAR_DIR = sugar_dir
            compute.GS2MESH_DIR = gs2mesh_dir
            try:
                with mock.patch.object(compute, "sugar_python", return_value=sugar_python), \
                        mock.patch.object(compute, "gs2mesh_python", return_value=gs2mesh_python), \
                        mock.patch.object(compute, "python_probe", return_value=(True, "runtime ok")) as probe:
                    report = compute.ensure_sugar_environment()

                self.assertEqual(report["python"], str(sugar_python))
                probe_code = probe.call_args.args[2]
                self.assertIn("import torch", probe_code)
                self.assertIn("import pytorch3d", probe_code)
                self.assertIn("from pytorch3d import _C", probe_code)
                self.assertIn("import open3d", probe_code)
            finally:
                compute.SUGAR_DIR = original_sugar_dir
                compute.GS2MESH_DIR = original_gs2mesh_dir

    def test_gs2mesh_manual_scale_options_remain_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "cache"
            model = Path(tmp) / "output" / "mesh_scene"
            stereo = cache / "000" / "out_DLNR_Middlebury"
            stereo.mkdir(parents=True)
            np.save(stereo / "depth.npy", np.full((8, 8), 10.0, dtype=np.float32))
            np.save(stereo / "occlusion_mask.npy", np.ones((8, 8), dtype=np.uint8))
            (cache / "camera_data.json").write_text(
                json.dumps([{"left": {"baseline": 0.25, "pos": [0.0, 0.0, 0.0]}}]),
                encoding="utf-8",
            )
            write_test_ply(
                model / "point_cloud" / "iteration_7000" / "point_cloud.ply",
                np.array([[0.0, 0.0, 0.0], [24.0, 0.0, 0.0]], dtype=np.float32),
            )
            requested = compute.mesh_export_options(
                {
                    "mode": "gs2mesh",
                    "gs2mesh_tsdf_voxel": 3,
                    "gs2mesh_tsdf_min_depth_baselines": 5,
                    "gs2mesh_tsdf_max_depth_baselines": 30,
                    "gs2mesh_auto_scale": False,
                }
            )

            profile = compute.profile_gs2mesh_render_cache(cache, requested)
            effective, _report = compute.resolve_gs2mesh_scale_options(requested, profile, model, 7000)

            self.assertEqual(effective["gs2mesh_tsdf_voxel"], 3)
            self.assertEqual(effective["gs2mesh_tsdf_min_depth_baselines"], 5)
            self.assertEqual(effective["gs2mesh_tsdf_max_depth_baselines"], 30)

    def test_gs2mesh_depth_profile_honors_disabled_occlusion_mask(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "cache"
            stereo = cache / "000" / "out_DLNR_Middlebury"
            stereo.mkdir(parents=True)
            np.save(stereo / "depth.npy", np.full((8, 8), 10.0, dtype=np.float32))
            np.save(stereo / "occlusion_mask.npy", np.zeros((8, 8), dtype=np.uint8))
            (cache / "camera_data.json").write_text(
                json.dumps([{"left": {"baseline": 0.25, "pos": [0.0, 0.0, 0.0]}}]),
                encoding="utf-8",
            )

            profile = compute.profile_gs2mesh_render_cache(
                cache,
                {
                    "mode": "gs2mesh",
                    "gs2mesh_use_occlusion_mask": False,
                    "gs2mesh_tsdf_min_depth_baselines": 4,
                    "gs2mesh_tsdf_max_depth_baselines": 80,
                },
            )

            self.assertEqual(profile["sampled_after_mask"], 64)
            self.assertEqual(profile["depth"]["p50"], 10.0)
            self.assertTrue(profile["occlusion_mask_usable"])

    def test_collect_gs2mesh_output_rejects_empty_mesh(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_gs2mesh = getattr(compute, "GS2MESH_DIR", None)
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                source = (
                    compute.GS2MESH_DIR
                    / "output"
                    / "app"
                    / "mesh_scene_d2"
                    / "mesh_scene_downsample2_custom_nw_iterations7_DLNR_Middlebury_baseline7_0p_"
                    "mask0_occ1_scale1_0_voxel2_512_trunc4_20_cleaned_mesh.ply"
                )
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(
                    "ply\nformat ascii 1.0\nelement vertex 0\nelement face 0\nend_header\n",
                    encoding="utf-8",
                )

                with self.assertRaisesRegex(RuntimeError, "no vertices or faces"):
                    compute.collect_gs2mesh_mesh_output(
                        "mesh_scene",
                        7,
                        {"mode": "gs2mesh", "gs2mesh_downsample": 2},
                        0,
                    )
            finally:
                compute.OUTPUT_DIR = original_output
                if original_gs2mesh is not None:
                    compute.GS2MESH_DIR = original_gs2mesh

    def test_sugar_runner_imports_modules_next_to_upstream_entry_point(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake_torch = Path(tmp) / "torch"
            fake_torch.mkdir()
            (fake_torch / "__init__.py").write_text("", encoding="utf-8")
            upstream = Path(tmp) / "SuGaR"
            upstream.mkdir()
            (upstream / "sugar_sibling.py").write_text("VALUE = 'sugar import ok'\n", encoding="utf-8")
            entry = upstream / "train.py"
            entry.write_text("from sugar_sibling import VALUE\nprint(VALUE)\n", encoding="utf-8")
            environment = os.environ.copy()
            environment["PYTHONPATH"] = tmp

            result = subprocess.run(
                [
                    sys.executable,
                    str(compute.SUGAR_RUNNER),
                    str(entry),
                ],
                cwd=compute.ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("sugar import ok", result.stdout)

    def test_gs2mesh_invalid_signed_cache_is_quarantined(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_gs2mesh = compute.GS2MESH_DIR
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                options = compute.mesh_export_options(
                    {"mode": "gs2mesh", "gs2mesh_downsample": 2, "gs2mesh_baseline_percentage": 7}
                )
                runtime = compute.GS2MESH_DIR / "data" / "custom" / "mesh_scene_downsample2"
                sparse = runtime / "sparse" / "0"
                sparse.mkdir(parents=True)
                (sparse / "images.txt").write_text(
                    "1 1 0 0 0 0 0 0 1 frame.jpg\n\n", encoding="utf-8"
                )
                cache = compute.gs2mesh_output_dirs("mesh_scene", 7000, options)[1]
                stereo = cache / "000" / "out_DLNR_Middlebury"
                stereo.mkdir(parents=True)
                (cache / "000" / "left.png").write_bytes(b"left")
                np.save(stereo / "depth.npy", np.ones((2, 2), dtype=np.float32))
                np.save(stereo / "occlusion_mask.npy", np.ones((2, 2), dtype=np.uint8))
                compute.write_gs2mesh_cache_manifest(cache, {"version": 1, "scene": "wrong"})
                signature = {"version": 1, "scene": "mesh_scene", "scene_360": True, "baseline_percentage": 7.0}

                reused = compute.prepare_gs2mesh_render_cache_dir(
                    "mesh_scene",
                    7000,
                    options,
                    signature=signature,
                    quarantine_invalid=True,
                )

                self.assertIsNone(reused)
                self.assertFalse(cache.exists())
                self.assertTrue(any((cache.parent / ".stale").iterdir()))
            finally:
                compute.GS2MESH_DIR = original_gs2mesh

    def test_collect_gs2mesh_output_preserves_nonempty_raw_mesh_when_cleaning_removes_all(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_gs2mesh = compute.GS2MESH_DIR
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                root = compute.GS2MESH_DIR / "output" / "app" / "mesh_scene_d2"
                stem = (
                    "mesh_scene_downsample2_custom_nw_iterations7_DLNR_Middlebury_baseline7_0p_"
                    "mask0_occ1_scale1_0_voxel2_512_trunc4_20"
                )
                root.mkdir(parents=True)
                cleaned = root / f"{stem}_cleaned_mesh.ply"
                raw = root / f"{stem}_mesh.ply"
                cleaned.write_text(
                    "ply\nformat ascii 1.0\nelement vertex 0\nelement face 0\nend_header\n",
                    encoding="utf-8",
                )
                raw.write_text(
                    "ply\nformat ascii 1.0\nelement vertex 3\nelement face 1\nend_header\n",
                    encoding="utf-8",
                )

                result = compute.collect_gs2mesh_mesh_output(
                    "mesh_scene",
                    7,
                    {"mode": "gs2mesh", "gs2mesh_downsample": 2},
                    0,
                )

                self.assertFalse(result["cleaned"])
                self.assertEqual(result["source"], str(raw))
                self.assertIn("raw TSDF mesh", result["warning"])
            finally:
                compute.OUTPUT_DIR = original_output
                compute.GS2MESH_DIR = original_gs2mesh

    def test_gs2mesh_staging_keeps_aligned_camera_source_byte_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_gs2mesh = compute.GS2MESH_DIR
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                source = Path(tmp) / "source"
                images = source / "images"
                sparse = source / "sparse" / "0"
                images.mkdir(parents=True)
                sparse.mkdir(parents=True)
                (images / "frame.jpg").write_bytes(b"image")
                originals = {
                    "cameras.txt": b"# cameras\n1 PINHOLE 100 50 80 80 50 25\n",
                    "images.txt": b"# images\n1 1 0 0 0 0 0 0 1 frame.jpg\n\n",
                    "points3D.txt": b"# points\n1 0 0 0 255 255 255 0\n",
                }
                for name, value in originals.items():
                    (sparse / name).write_bytes(value)

                compute.prepare_gs2mesh_dataset("mesh_scene", source, downsample=2)

                for name, value in originals.items():
                    self.assertEqual((sparse / name).read_bytes(), value)
            finally:
                compute.GS2MESH_DIR = original_gs2mesh

    def test_sugar_environment_reports_deterministic_pytorch3d_policy_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_sugar_dir = compute.SUGAR_DIR
            original_gs2mesh_dir = compute.GS2MESH_DIR
            sugar_dir = Path(tmp) / "SuGaR"
            gs2mesh_dir = Path(tmp) / "GS2Mesh"
            sugar_python = Path(tmp) / "envs" / "sugar" / "python.exe"
            gs2mesh_python = Path(tmp) / "envs" / "gs2mesh" / "python.exe"
            sugar_dir.mkdir(parents=True)
            gs2mesh_dir.mkdir(parents=True)
            sugar_python.parent.mkdir(parents=True)
            gs2mesh_python.parent.mkdir(parents=True)
            sugar_python.write_bytes(b"")
            gs2mesh_python.write_bytes(b"")
            (sugar_dir / "train.py").write_text("print('sugar')", encoding="utf-8")
            compute.SUGAR_DIR = sugar_dir
            compute.GS2MESH_DIR = gs2mesh_dir
            blocked = "OSError: [WinError 4551] application control policy blocked pytorch3d\\_C.pyd"
            try:
                with mock.patch.object(compute, "sugar_python", return_value=sugar_python), \
                        mock.patch.object(compute, "gs2mesh_python", return_value=gs2mesh_python), \
                        mock.patch.object(compute, "smart_app_control_state", return_value="on"), \
                        mock.patch.object(compute, "python_probe", return_value=(False, blocked)) as probe:
                    with self.assertRaisesRegex(RuntimeError, "unsigned PyTorch3D native module") as raised:
                        compute.ensure_sugar_environment()

                self.assertEqual(probe.call_count, 1)
                self.assertIn("no per-file allow rule", str(raised.exception))
                self.assertIn("Linux/WSL", str(raised.exception))
                self.assertNotIn("replace the conda-forge Open3D", str(raised.exception))
            finally:
                compute.SUGAR_DIR = original_sugar_dir
                compute.GS2MESH_DIR = original_gs2mesh_dir

    def test_collect_gs2mesh_output_never_falls_back_to_another_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_output = compute.OUTPUT_DIR
            original_gs2mesh = getattr(compute, "GS2MESH_DIR", None)
            compute.OUTPUT_DIR = Path(tmp) / "output"
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                wrong = (
                    compute.GS2MESH_DIR
                    / "output"
                    / "app"
                    / "other_scene_d2"
                    / "mesh_scene_downsample2_custom_nw_iterations7_DLNR_Middlebury_baseline7_0p_"
                    "mask0_occ1_scale1_0_voxel2_512_trunc4_20_cleaned_mesh.ply"
                )
                wrong.parent.mkdir(parents=True, exist_ok=True)
                wrong.write_text(
                    "ply\nformat ascii 1.0\nelement vertex 3\nelement face 1\nend_header\n",
                    encoding="utf-8",
                )

                with self.assertRaisesRegex(FileNotFoundError, "mesh_scene"):
                    compute.collect_gs2mesh_mesh_output(
                        "mesh_scene",
                        7,
                        {"mode": "gs2mesh", "gs2mesh_downsample": 2},
                        0,
                    )
                self.assertFalse(compute.mesh_output_path("mesh_scene", 7, "gs2mesh", post=True).exists())
            finally:
                compute.OUTPUT_DIR = original_output
                if original_gs2mesh is not None:
                    compute.GS2MESH_DIR = original_gs2mesh

    def test_gs2mesh_staging_expands_multi_camera_mapping_without_touching_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_gs2mesh = compute.GS2MESH_DIR
            compute.GS2MESH_DIR = Path(tmp) / "gs2mesh"
            try:
                source = Path(tmp) / "source"
                (source / "images").mkdir(parents=True)
                sparse = source / "sparse" / "0"
                sparse.mkdir(parents=True)
                (source / "images" / "first.jpg").write_bytes(b"first")
                (source / "images" / "second.jpg").write_bytes(b"second")
                cameras = (
                    "# Camera list\n"
                    "# Number of cameras: 2\n"
                    "7 PINHOLE 200 100 150 151 100 50\n"
                    "9 PINHOLE 400 300 320 321 200 150\n"
                )
                images = (
                    "# Image list\n"
                    "2 1 0 0 0 -1 0 0 9 second.jpg\n\n"
                    "1 1 0 0 0 0 0 0 7 first.jpg\n\n"
                )
                (sparse / "cameras.txt").write_text(cameras, encoding="utf-8")
                (sparse / "images.txt").write_text(images, encoding="utf-8")
                (sparse / "points3D.txt").write_text("# points\n", encoding="utf-8")

                staged = compute.prepare_gs2mesh_dataset("mesh_scene", source, downsample=1)

                self.assertEqual((sparse / "cameras.txt").read_text(encoding="utf-8"), cameras)
                self.assertEqual((sparse / "images.txt").read_text(encoding="utf-8"), images)
                staged_sparse = staged / "sparse" / "0"
                staged_cameras = compute.gs2mesh_colmap_renderer_intrinsics(
                    staged_sparse / "cameras.txt",
                    compute.gs2mesh_colmap_image_records(staged_sparse / "images.txt"),
                )
                self.assertEqual(staged_cameras[0][:2], (200, 100))
                self.assertEqual(staged_cameras[1][:2], (400, 300))
                staged_records = compute.gs2mesh_colmap_image_records(staged_sparse / "images.txt")
                self.assertEqual(
                    [record["camera_id"] for record in staged_records],
                    [record["image_id"] for record in staged_records],
                )
            finally:
                compute.GS2MESH_DIR = original_gs2mesh

    def test_sugar_env_root_prefers_machine_runtime_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            local_app_data = Path(tmp) / "LocalAppData"
            runtime_root = Path(tmp) / "runtime"
            env_root = runtime_root / "envs" / "sugar"
            (env_root / "Scripts").mkdir(parents=True)
            (env_root / "Scripts" / "python.exe").write_bytes(b"")
            legacy_env_root = Path(tmp) / "legacy" / "sugar"
            legacy_env_root.mkdir(parents=True)
            (legacy_env_root / "python.exe").write_bytes(b"")
            config_path = local_app_data / "Gaussian Scene Workbench" / "runtime.json"
            config_path.parent.mkdir(parents=True)
            config_path.write_text(
                json.dumps({"schema_version": 1, "runtime_root": str(runtime_root), "sugar_env_root": str(env_root)}),
                encoding="utf-8",
            )

            resolved = compute.sugar_env_root(
                environ={"LOCALAPPDATA": str(local_app_data), "SUGAR_CONDA_PREFIX": str(legacy_env_root)},
                home=Path(tmp) / "home",
            )

            self.assertEqual(resolved, env_root)
            self.assertEqual(compute.environment_python_path(resolved), env_root / "Scripts" / "python.exe")


    def test_cancelled_2dgs_mesh_does_not_start_or_recover(self):
        job = {
            "id": "cancelled-mesh", "scene": "mesh_scene", "iteration": 7,
            "mode": "bounded", "options": {"mode": "bounded"},
            "status": "queued", "stage": "queued", "log": [],
            "cancel_requested": True,
        }
        with mock.patch.object(compute, "ensure_mesh_environment") as environment, \
                mock.patch.object(compute, "run_logged") as run, \
                mock.patch.object(compute, "recover_2dgs_post_mesh") as recover:
            compute.run_mesh_export_job(job)

        self.assertEqual(job["status"], "cancelled")
        environment.assert_not_called()
        run.assert_not_called()
        recover.assert_not_called()

    def test_gs2mesh_failure_releases_its_reserved_renderer_port(self):
        job = {
            "id": "owned-gs2mesh", "scene": "mesh_scene", "iteration": 7,
            "mode": "gs2mesh", "options": {"mode": "gs2mesh"},
            "status": "queued", "stage": "queued", "log": [],
            "cancel_requested": False,
        }
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(compute, "OUTPUT_DIR", Path(tmp) / "output"), \
                mock.patch.object(compute, "ensure_gs2mesh_environment"), \
                mock.patch.object(compute, "task_port_available", return_value=True), \
                mock.patch.object(compute, "run_gs2mesh_mesh_job", side_effect=RuntimeError("owned stage failure")):
            compute.run_mesh_export_job(job)

        self.assertEqual(job["status"], "failed")
        self.assertIn("owned stage failure", job["error"])
        self.assertTrue(job["task_port_released"])
        self.assertNotIn(job["task_port"], compute.TASK_PORT_OWNERS)


if __name__ == "__main__":
    unittest.main()
