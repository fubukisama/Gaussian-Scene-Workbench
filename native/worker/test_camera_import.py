"""Known-camera import through the owned worker CLI and real transactions."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

from PIL import Image

from native.backend.camera_dataset import load_camera_dataset


class CameraImportTests(unittest.TestCase):
    def setUp(self):
        base = Path("D:/Codex/qa-camera-import-p0")
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repository = Path(__file__).resolve().parents[2]
        self.source = self.root / "source"
        self.source.mkdir()
        self.project = self.root / "project"
        self.project.mkdir()
        self.final = self.project / "datasets" / "capture"
        for index, split in enumerate(("train", "val", "test")):
            path = self.source / split / "same.png"
            path.parent.mkdir()
            Image.new("RGB", (12 + index, 8 + index), (10 + index, 20, 30)).save(path)
            pose = [[1., 0., 0., float(index)], [0., 1., 0., 0.],
                    [0., 0., 1., 0.], [0., 0., 0., 1.]]
            (self.source / ("transforms_" + split + ".json")).write_text(json.dumps({
                "camera_angle_x": .7, "frames": [{"file_path": split + "/same", "transform_matrix": pose}]}),
                encoding="utf-8")
        # Incidental/broken SfM must never supersede explicit fixed cameras.
        (self.source / "sparse/0").mkdir(parents=True)
        (self.source / "sparse/0/cameras.bin").write_bytes(b"not a reconstruction")

    def config(self, **extra):
        config = {"task": "import", "repositoryRoot": str(self.repository),
                  "projectRoot": str(self.project), "datasetRoot": str(self.project / "datasets"),
                  "scene": "capture", "cameraInput": {"adapter": "blender_transforms",
                  "sourcePath": str(self.source), "posePolicy": "locked"}}
        config.update(extra)
        path = self.root / "job.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        return path

    def worker(self, path, task="import", cancel=False):
        process = subprocess.Popen([sys.executable, "-B", str(self.repository / "native/worker/gsw_worker.py"),
                                    "--task", task, "--config", str(path)], cwd=str(self.root),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace",
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        if cancel:
            process.stdin.write("cancel\n")
            process.stdin.flush()
        # Keep stdin open. A finished import must not wait for the desktop to
        # close its control pipe before the worker exits.
        process.wait(timeout=30)
        process.stdin.close()
        stdout, stderr = process.stdout.read(), process.stderr.read()
        process.stdout.close()
        process.stderr.close()
        return process.returncode, stdout + stderr

    def test_locked_import_copies_nested_holdouts_independently_and_preserves_source_bytes(self):
        source_hashes = {path.relative_to(self.source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in self.source.rglob("*") if path.is_file()}
        code, log = self.worker(self.config())
        self.assertEqual(code, 0, log)
        dataset = load_camera_dataset(self.final)
        self.assertEqual({key: len(value) for key, value in dataset["splits"].items()},
                         {"train": 1, "val": 1, "test": 1})
        self.assertEqual(dataset["pose_policy"], "locked")
        for view in dataset["views"]:
            destination = self.final / view["image_path"]
            source = self.source / view["image_path"]
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            self.assertFalse(os.path.samefile(source, destination))
        info = json.loads((self.final / "camera_dataset_info.json").read_text(encoding="utf-8"))
        self.assertEqual(info["counts"], {"train": 1, "val": 1, "test": 1})
        self.assertEqual(info["dataIdentity"], dataset["data_identity"])
        self.assertEqual(info["posePolicy"], "locked")
        self.assertIn("[worker-camera-dataset]", log)
        self.assertEqual(source_hashes, {path.relative_to(self.source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in self.source.rglob("*") if path.is_file()})
        self.assertFalse((self.final / "sparse").exists())

    def test_managed_copy_can_move_roots_without_changing_camera_identity(self):
        code, log = self.worker(self.config())
        self.assertEqual(code, 0, log)
        before = load_camera_dataset(self.final)
        moved = self.root / "relocated/project/datasets/capture"
        shutil.copytree(self.final, moved)
        # The archived source calibration can remain traceable to the original
        # selection, but normalized input identity is independent of its root.
        reopened = load_camera_dataset(moved)
        self.assertEqual(reopened["data_identity"], before["data_identity"])
        mapping = json.loads((moved / "source/metadata_manifest.json").read_text(encoding="utf-8"))
        self.assertEqual([entry["viewId"] for entry in mapping["files"]],
                         [view["view_id"] for view in reopened["views"]])
        self.assertEqual(mapping["dataIdentity"], before["data_identity"])
        for key in ("pose_policy", "splits", "transforms"):
            changed = json.loads(json.dumps(reopened))
            if key == "pose_policy":
                changed[key] = "solve"
            elif key == "splits":
                changed[key]["train"].append(changed[key]["test"][0])
            else:
                changed[key]["canonical_to_training"]["s"] = 2.
            (moved / "camera_dataset.json").write_text(json.dumps(changed), encoding="utf-8")
            with self.subTest(key=key), self.assertRaises(ValueError):
                load_camera_dataset(moved)

    def test_cancel_keeps_old_dataset_and_worker_exits_with_control_pipe_open(self):
        self.final.mkdir(parents=True)
        marker = self.final / "old-project-data.txt"
        marker.write_bytes(b"do not replace")
        code, log = self.worker(self.config(overwrite=True), cancel=True)
        self.assertEqual(code, 130, log)
        self.assertEqual(marker.read_bytes(), b"do not replace")
        self.assertIn('"state":"cancelled"', log)
        code, log = self.worker(self.root / "job.json", task="import-recovery")
        self.assertEqual(code, 0, log)
        self.assertEqual(marker.read_bytes(), b"do not replace")
        self.assertFalse(any(path.name.startswith("gsw-import-") for path in self.final.parent.iterdir()))

    def test_failed_locked_input_does_not_publish_or_replace_existing_dataset(self):
        self.final.mkdir(parents=True)
        marker = self.final / "old-project-data.txt"
        marker.write_bytes(b"keep")
        (self.source / "test/same.png").unlink()
        code, log = self.worker(self.config(overwrite=True))
        self.assertEqual(code, 1, log)
        self.assertIn("camera_missing_image", log)
        self.assertEqual(marker.read_bytes(), b"keep")
        self.assertFalse((self.final / "camera_dataset.json").exists())

    def test_malformed_explicit_camera_mode_cannot_fall_back_to_media_import(self):
        path = self.config(cameraInput={}, files=[{"path": str(self.source / "train/same.png")}])
        code, log = self.worker(path)
        self.assertEqual(code, 1, log)
        self.assertIn("cameraInput must be a nonempty object", log)
        self.assertFalse(self.final.exists())


if __name__ == "__main__":
    unittest.main()
