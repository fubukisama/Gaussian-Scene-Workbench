import json
import sys
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


class TrainingTelemetryTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
