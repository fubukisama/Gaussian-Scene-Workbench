import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from native.worker.training_preview import TrainingPreviewPublisher
from native.worker.training_preview_policy import TrainingPreviewPolicy


class Rows:
    def __len__(self):
        return 2

    def tobytes(self):
        return struct.pack("<28f", *range(28))


class TrainingPreviewTests(unittest.TestCase):
    def test_observation_query_failure_skips_tensors_without_raising(self):
        def unavailable():
            raise RuntimeError("Owned CUDA query fixture unavailable")
        policy = TrainingPreviewPolicy(memory_probe=unavailable)
        class Model:
            reads = 0
            @property
            def get_xyz(self):
                self.reads += 1
                raise AssertionError("Unavailable observation query must not read training tensors")
        with tempfile.TemporaryDirectory() as directory:
            publisher = TrainingPreviewPublisher(directory, lambda *event: None, policy=policy)
            model = Model()
            try:
                self.assertFalse(publisher.publish_gaussians(model, 3860))
                self.assertEqual(model.reads, 0)
            finally:
                publisher.close()

    def test_invalid_memory_probe_cannot_authorize_tensor_snapshots(self):
        class Model:
            @property
            def get_xyz(self):
                raise AssertionError("Invalid budgets must not inspect training tensors")
        for memory in ((float("nan"), 12 * 1024**3), (float("inf"), 12 * 1024**3),
                       (1, 0), (True, 12 * 1024**3), (13 * 1024**3, 12 * 1024**3)):
            with self.subTest(memory=memory), tempfile.TemporaryDirectory() as directory:
                policy = TrainingPreviewPolicy(memory_probe=lambda: memory)
                publisher = TrainingPreviewPublisher(directory, lambda *event: None, policy=policy)
                try:
                    self.assertFalse(publisher.publish_gaussians(Model(), 3860))
                finally:
                    publisher.close()

    def test_healthy_shared_gpu_limits_redundant_cpu_observations(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run with the configured training Python for tensor observation tests")
        now, emitted = [0.], threading.Event()
        policy = TrainingPreviewPolicy(memory_probe=lambda: (8 * 1024**3, 12 * 1024**3),
                                       clock=lambda: now[0])
        model = SimpleNamespace(get_xyz=torch.zeros(2, 3), _scaling=torch.zeros(2, 3),
                                _features_dc=torch.zeros(2, 1, 3), _opacity=torch.zeros(2, 1),
                                _rotation=torch.zeros(2, 4))
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch("native.worker.training_preview.time.monotonic", side_effect=lambda: now[0]):
            publisher = TrainingPreviewPublisher(directory, lambda *event: emitted.set(), policy=policy)
            try:
                self.assertTrue(publisher.publish_gaussians(model, 1, shared_gpu_healthy=True))
                self.assertTrue(emitted.wait(2))
                now[0] = 4.
                self.assertFalse(publisher.publish_gaussians(model, 2, shared_gpu_healthy=True))
                now[0] = 30.
                deadline = time.perf_counter() + 2.
                while not publisher.due() and time.perf_counter() < deadline:
                    threading.Event().wait(.01)
                self.assertTrue(publisher.publish_gaussians(model, 3, shared_gpu_healthy=True))
            finally:
                publisher.close()

    def test_vram_recovery_requires_sustained_headroom_before_tensor_snapshot(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run with the configured training Python for tensor observation tests")
        now, free = [0.], [128 * 1024**2]
        policy = TrainingPreviewPolicy(memory_probe=lambda: (free[0], 12 * 1024**3),
                                       clock=lambda: now[0])
        model = SimpleNamespace()
        model.get_xyz = torch.zeros(2, 3)
        model._scaling = torch.zeros(2, 3)
        model._features_dc = torch.zeros(2, 1, 3)
        model._opacity = torch.zeros(2, 1)
        model._rotation = torch.zeros(2, 4)
        with tempfile.TemporaryDirectory() as directory:
            publisher = TrainingPreviewPublisher(directory, lambda *event: None, policy=policy)
            try:
                self.assertFalse(publisher.publish_gaussians(model, 3860))
                free[0], now[0] = 2 * 1024**3, 1.
                self.assertFalse(publisher.publish_gaussians(model, 3861))
                free[0], now[0] = 700 * 1024**2, 5.
                self.assertFalse(publisher.publish_gaussians(model, 3862))
                free[0], now[0] = 2 * 1024**3, 6.
                self.assertFalse(publisher.publish_gaussians(model, 3863))
                now[0] = 15.
                self.assertFalse(publisher.publish_gaussians(model, 3864))
                now[0] = 16.
                self.assertTrue(publisher.publish_gaussians(model, 3865))
            finally:
                publisher.close()

    def test_low_vram_skips_snapshot_before_reading_training_tensors(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run with the configured training Python for tensor observation tests")

        class Model:
            def __init__(self):
                self.tensor_reads = 0
                self._scaling = torch.zeros(2, 3)
                self._features_dc = torch.zeros(2, 1, 3)
                self._opacity = torch.zeros(2, 1)
                self._rotation = torch.zeros(2, 4)

            @property
            def get_xyz(self):
                self.tensor_reads += 1
                return torch.zeros(2, 3)

        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(torch.cuda, "is_available", return_value=True), \
             mock.patch.object(torch.cuda, "mem_get_info", return_value=(128 * 1024**2, 12 * 1024**3)):
            model, events = Model(), []
            publisher = TrainingPreviewPublisher(directory, lambda *event: events.append(event))
            try:
                self.assertFalse(publisher.publish_gaussians(model, 3860))
                self.assertEqual(model.tensor_reads, 0)
                self.assertFalse(any(prefix == "[gsw-training-preview]" for prefix, _ in events))
            finally:
                publisher.close()

    def test_two_scale_preview_preserves_surfel_contract(self):
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            events = []
            publisher = TrainingPreviewPublisher(directory, lambda *event: events.append(event))
            rows = np.arange(26, dtype=np.float32).reshape(2, 13)
            self.assertTrue(publisher.publish_rows(rows, 12, source_count=30))
            publisher.close()
            data = Path(events[0][1]["point_cloud_path"]).read_bytes()
            header, body = data.split(b"end_header\n", 1)
            self.assertIn(b"property float scale_1", header)
            self.assertNotIn(b"property float scale_2", header)
            self.assertIn(b"property float rot_3", header)
            self.assertEqual(body, rows.astype("<f4").tobytes())

    def test_cleanup_supports_legacy_training_python(self):
        original = Path.unlink
        def legacy_unlink(path):
            return original(path)
        with mock.patch.object(Path, "unlink", legacy_unlink):
            self.test_atomic_attributes_initial_frame_and_retention()

    def test_atomic_attributes_initial_frame_and_retention(self):
        with tempfile.TemporaryDirectory() as directory:
            events = []
            publisher = TrainingPreviewPublisher(directory, lambda *event: events.append(event))
            for index in range(7):
                path = publisher.directory / f"preview_{index:06d}.ply"
                publisher._write(path, Rows(), index, 100, index == 0)
                self.assertTrue(path.is_file())
                self.assertFalse(path.with_suffix(".tmp").exists())
            self.assertEqual(len(list(publisher.directory.glob("*.ply"))), 4)
            self.assertEqual(events[0][1]["preview_kind"], "gaussian_initial")
            self.assertEqual(events[-1][1]["preview_kind"], "gaussian_live")
            self.assertEqual(events[-1][1]["gaussian_count"], 100)
            self.assertEqual(events[-1][1]["preview_count"], 2)
            data = path.read_bytes()
            header, body = data.split(b"end_header\n", 1)
            self.assertIn(b"element vertex 2", header)
            self.assertIn(b"property float rot_3", header)
            self.assertEqual(body, Rows().tobytes())
            self.assertNotIn("point_cloud/iteration_", str(path))
            publisher.close()

    def test_slow_writer_does_not_queue_and_interval_is_time_based(self):
        with tempfile.TemporaryDirectory() as directory:
            publisher = TrainingPreviewPublisher(directory, lambda *event: None, interval=3)
            gate = threading.Event()
            publisher.future = publisher.executor.submit(gate.wait, 2)
            try:
                self.assertFalse(publisher.due())
            finally:
                gate.set()
            publisher.future.result()
            self.assertTrue(publisher.due())
            publisher.last_time = 20
            with mock.patch("native.worker.training_preview.time.monotonic", return_value=22):
                self.assertFalse(publisher.due())
            with mock.patch("native.worker.training_preview.time.monotonic", return_value=23):
                self.assertTrue(publisher.due())
            publisher.close()

    def test_failed_write_emits_no_preview_and_keeps_existing(self):
        with tempfile.TemporaryDirectory() as directory:
            events = []
            publisher = TrainingPreviewPublisher(directory, lambda *event: events.append(event))
            path = publisher.directory / "preview.ply"
            publisher._write(path, Rows(), 0, 2, True)
            before = path.read_bytes()
            with mock.patch("native.worker.training_preview.os.replace", side_effect=OSError("disk failure")):
                publisher._write(path, Rows(), 1, 2, False)
            self.assertEqual(before, path.read_bytes())
            self.assertEqual(events[-1][0], "[gsw-preview-warning]")
            self.assertFalse(path.with_suffix(".tmp").exists())
            publisher.close()


if __name__ == "__main__":
    unittest.main()
