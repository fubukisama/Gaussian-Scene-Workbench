import json
import struct
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


# These focused contract tests do not exercise the PLY renderer. Stubbing the
# optional heavy modules keeps them runnable in the desktop build environment.
if "numpy" not in sys.modules:
    sys.modules["numpy"] = types.ModuleType("numpy")
if "cgi" not in sys.modules:
    sys.modules["cgi"] = types.ModuleType("cgi")
if "plyfile" not in sys.modules:
    plyfile = types.ModuleType("plyfile")
    plyfile.PlyData = object
    plyfile.PlyElement = object
    sys.modules["plyfile"] = plyfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import server  # noqa: E402


def write_colmap_points(path, points):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(struct.pack("<Q", len(points)))
        for index, (x, y, z, red, green, blue) in enumerate(points, start=1):
            stream.write(
                struct.pack(
                    "<QdddBBBdQ", index, x, y, z, red, green, blue, 0.25, 1
                )
            )
            stream.write(struct.pack("<II", index, index - 1))


class TrainingTelemetryTests(unittest.TestCase):
    def test_pause_exit_requires_current_session_checkpoint(self):
        from native.worker.training_checkpoint import TrainingCheckpoint
        import pickle
        with tempfile.TemporaryDirectory() as temporary:
            control = {"output": temporary, "session": "current"}
            TrainingCheckpoint(temporary, control).save(types.SimpleNamespace(save=pickle.dump),
                {"iteration": 3, "total": 10, "identity": "test"})
            job = {"id": "pause-test", "backend": "3dgs", "native_control": control, "log": []}
            process = mock.Mock(stdout=[], wait=mock.Mock(return_value=75))
            with mock.patch.object(server, "persist_train_job"), \
                 mock.patch.object(server, "training_env", return_value={}), \
                 mock.patch.object(server.subprocess, "Popen", return_value=process) as launch:
                with self.assertRaises(server.NativeTrainingPaused) as paused:
                    server.run_logged(job, ["python", "train.py"], temporary, "3dgs")
                self.assertEqual(paused.exception.manifest["iteration"], 3)
                self.assertEqual(json.loads(launch.call_args.kwargs["env"]["GSW_NATIVE_TRAINING_CONTROL"]), control)
                control["session"] = "next-session"
                with self.assertRaisesRegex(RuntimeError, "this training session"):
                    server.run_logged(job, ["python", "train.py"], temporary, "3dgs")
                self.assertIsNone(job["process"])

    def test_native_resume_never_reconstructs_missing_alignment(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset = Path(temporary) / "dataset"
            (dataset / "images").mkdir(parents=True)
            (dataset / "images/a.jpg").write_bytes(b"fixture")
            job = {"id": "resume-test", "scene": "test", "output_scene": "test", "backend": "3dgs",
                   "dataset_path": str(dataset), "native_control": {"resume": True}, "log": []}
            with mock.patch.object(server, "OUTPUT_DIR", Path(temporary) / "output"), \
                 mock.patch.object(server, "ensure_training_environment", return_value={"python": "test", "colmap": "test"}), \
                 mock.patch.object(server, "persist_train_job"), mock.patch.object(server, "run_logged"), \
                 mock.patch.object(server, "restore_alignment_cache", return_value=False) as restore, \
                 mock.patch.object(server, "run_colmap_convert", side_effect=RuntimeError("COLMAP must not run")) as convert, \
                 mock.patch.object(server, "training_point_cloud", return_value=None):
                server.run_training_job(job, False, "quick", False)
            convert.assert_not_called()
            restore.assert_not_called()
            self.assertEqual(job["status"], "failed")
            self.assertIn("resume", job["error"].lower())

    def test_initial_gaussians_follow_sparse_generation_and_ignore_late_frames(self):
        job = {"preview_kind": "colmap_sparse", "preview_iteration": 40}
        server.apply_training_preview(job, {"iteration": 0, "preview_kind": "gaussian_initial",
            "point_cloud_path": "E:/initial.ply", "gaussian_count": 12})
        self.assertEqual(job["preview_iteration"], 0)
        self.assertEqual(job["preview_kind"], "gaussian_initial")
        server.apply_training_preview(job, {"iteration": 30, "preview_kind": "gaussian_live",
            "point_cloud_path": "E:/live.ply"})
        server.apply_training_preview(job, {"iteration": 20, "preview_kind": "gaussian_live",
            "point_cloud_path": "E:/old.ply"})
        self.assertEqual(job["partial_point_cloud_path"], "E:/live.ply")
        self.assertEqual(job["gaussian_count"], 12)
        server.apply_training_preview(job, {"iteration": 30, "point_cloud_path": "E:/checkpoint.ply"})
        server.apply_training_preview(job, {"iteration": 30, "preview_kind": "gaussian_live",
            "point_cloud_path": "E:/late.ply"})
        self.assertEqual(job["partial_point_cloud_path"], "E:/checkpoint.ply")

    def test_training_log_updates_live_visualization_metrics(self):
        job = {
            "id": "train-telemetry",
            "scene": "scene",
            "output_scene": "scene-output",
            "backend": "3dgs",
            "status": "running",
            "stage": "train",
            "created_at": 1,
            "updated_at": 1,
            "log": [],
        }
        metrics = {
            "iteration": 11100,
            "total_iterations": 30000,
            "loss": 0.0234,
            "psnr": 27.5,
            "gaussian_count": 123456,
            "iteration_milliseconds": 12.5,
            "elapsed_seconds": 144.0,
        }

        with mock.patch.object(server, "persist_train_job"):
            server.add_job_log(
                job,
                "[gsw-training-metrics] "
                + json.dumps(metrics)
                + " [30/07 02:25:25]",
            )
            server.add_job_log(
                job,
                "[gsw-training-preview] " + json.dumps({
                    "iteration": 10000,
                    "point_cloud_path": "E:/model/point_cloud.ply",
                }) + " [30/07 02:25:25]",
            )

        snapshot = server.job_snapshot(job)
        self.assertEqual(snapshot["iteration"], 11100)
        self.assertEqual(snapshot["total_iterations"], 30000)
        self.assertEqual(snapshot["progressPercent"], 37)
        self.assertEqual(snapshot["loss"], 0.0234)
        self.assertEqual(snapshot["psnr"], 27.5)
        self.assertEqual(snapshot["gaussian_count"], 123456)
        self.assertEqual(snapshot["iteration_milliseconds"], 12.5)
        self.assertEqual(snapshot["elapsed_seconds"], 144.0)
        self.assertEqual(snapshot["latest_iteration"], 10000)
        self.assertEqual(snapshot["partial_point_cloud_path"], "E:/model/point_cloud.ply")

    def test_colmap_binary_preview_is_bounded_and_preserves_color(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "points3D.bin"
            write_colmap_points(
                source,
                [
                    (1.0, 2.0, 3.0, 10, 20, 30),
                    (4.0, 5.0, 6.0, 40, 50, 60),
                    (7.0, 8.0, 9.0, 70, 80, 90),
                ],
            )

            source_count, points = server.read_colmap_points3d_binary(
                source, max_points=2
            )

            self.assertEqual(source_count, 3)
            self.assertEqual(points, [(1.0, 2.0, 3.0, 10, 20, 30),
                                      (7.0, 8.0, 9.0, 70, 80, 90)])

    def test_colmap_preview_publisher_updates_live_job_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            snapshot = dataset / "distorted" / "snapshots" / "2"
            write_colmap_points(
                snapshot / "points3D.bin",
                [(1.25, -2.5, 3.75, 120, 121, 122)],
            )
            job = {
                "id": "colmap-preview",
                "kind": "colmap",
                "scene": "scene",
                "status": "running",
                "stage": "colmap",
                "created_at": 1,
                "updated_at": 1,
                "log": [],
            }
            publisher = server.ColmapPointCloudPreviewPublisher(
                job, dataset, [snapshot.parent], interval=60
            )

            self.assertTrue(publisher.publish_latest(force=True))

            preview = Path(job["partial_point_cloud_path"])
            self.assertTrue(preview.is_file())
            self.assertEqual(job["preview_kind"], "colmap_sparse")
            self.assertEqual(job["latest_iteration"], 1)
            self.assertEqual(job["point_count"], 1)
            header = preview.read_bytes().split(b"end_header\n", 1)[0]
            self.assertIn(b"element vertex 1", header)

    def test_colmap_snapshot_cadence_limits_preview_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            images = dataset / "images"
            images.mkdir(parents=True)
            for index in range(97):
                (images / f"{index:04d}.jpg").write_bytes(b"image")

            self.assertEqual(
                server.colmap_snapshot_frames_frequency(dataset, target_count=48),
                3,
            )
            command = ["colmap.exe", "mapper"]
            server.configure_colmap_mapper_snapshots(
                command, dataset / "snapshots", 3
            )
            self.assertEqual(
                command[command.index("--Mapper.snapshot_frames_freq") + 1],
                "3",
            )
            self.assertEqual(
                Path(command[command.index("--Mapper.snapshot_path") + 1]),
                dataset / "snapshots",
            )


if __name__ == "__main__":
    unittest.main()
