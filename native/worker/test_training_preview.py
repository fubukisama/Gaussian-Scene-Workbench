import struct
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from native.worker.training_preview import TrainingPreviewPublisher


class Rows:
    def __len__(self):
        return 2

    def tobytes(self):
        return struct.pack("<28f", *range(28))


class TrainingPreviewTests(unittest.TestCase):
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
