import contextlib
import io
import json
import os
import re
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from native.worker import gsw_worker


class FakeBackend:
    def __init__(self, state="done", error=None):
        self.OUTPUT_DIR = None
        self.TRAIN_JOBS_DIR = None
        self.TRAIN_LOCK = threading.Lock()
        self.TRAIN_JOBS = {}
        self.state = state
        self.error = error
        self.call = None

    def start_training(self, *args, **kwargs):
        self.call = (args, kwargs)
        self.TRAIN_JOBS["test-job"] = {
            "status": self.state,
            "stage": self.state,
            "error": self.error,
            "log": ["backend log"],
        }
        return {"id": "test-job"}

    def cancel_training(self, _job_id):
        return None


class FakeColmapBackend:
    def __init__(self, valid_output=True):
        self.MESH_LOCK = threading.Lock()
        self.MESH_JOBS = {}
        self.valid_output = valid_output
        self.call = None

    def start_colmap_alignment(self, scene, options=None, dataset_path=None):
        self.call = {
            "scene": scene,
            "options": options,
            "dataset_path": dataset_path,
        }
        self.MESH_JOBS["colmap-job"] = {
            "status": "done",
            "stage": "done",
            "error": None,
            "log": ["colmap log"],
        }
        return {"id": "colmap-job"}

    def cancel_mesh_job(self, _job_id):
        return None

    def dataset_has_colmap_scene(self, _dataset):
        return self.valid_output


class FakeImportBackend:
    IMAGE_EXTS = {".jpg"}
    VIDEO_EXTS = {".mp4"}

    def __init__(self, imported_bytes=b"imported", error=None):
        self.DATASETS_DIR = None
        self.imported_bytes = imported_bytes
        self.error = error
        self.job_id = None
        self.snapshot = None

    def save_path_dataset(self, request):
        self.job_id = request["import_job_id"]
        staging = Path(self.DATASETS_DIR) / request["scene"]
        images = staging / "images"
        images.mkdir(parents=True)
        (images / "frame.jpg").write_bytes(self.imported_bytes)

        if self.error is not None:
            self.snapshot = {
                "status": "failed",
                "stage": "archiving",
                "error": self.error,
            }
            raise RuntimeError(self.error)

        self.snapshot = {
            "status": "done",
            "stage": "done",
            "error": None,
            "image_count": 1,
        }
        return {"image_count": 1}

    def import_job_snapshot(self, job_id):
        if job_id != self.job_id or self.snapshot is None:
            raise ValueError("Import job not found")
        return dict(self.snapshot)


class SafeNameImportBackend(FakeImportBackend):
    def __init__(self, imported_bytes=b"imported"):
        super().__init__(imported_bytes=imported_bytes)
        self.received_scene = None

    def save_path_dataset(self, request):
        self.received_scene = request["scene"]
        if (
            len(self.received_scene) > 120
            or self.received_scene.startswith(".")
            or self.received_scene.endswith(".")
            or ".." in self.received_scene
            or re.fullmatch(r"[A-Za-z0-9_.-]+", self.received_scene) is None
        ):
            raise ValueError("Invalid scene name")
        return super().save_path_dataset(request)


class WorkerTests(unittest.TestCase):
    def configuration(self, root):
        return {
            "repositoryRoot": str(root),
            "datasetPath": str(root / "dataset"),
            "outputRoot": str(root / "output"),
            "outputScene": "test-scene",
            "backend": "3dgs",
            "quality": "quick",
            "jobStore": str(root / "jobs"),
            "runColmap": True,
            "overwrite": False,
            "trainOptions": {"iterations": 7000, "resolution": 8},
        }

    def import_configuration(self, root, overwrite=False):
        project_root = root / "project"
        dataset_root = project_root / "datasets"
        dataset_root.mkdir(parents=True)
        source = root / "source.jpg"
        source.write_bytes(b"source")
        return {
            "task": "import",
            "repositoryRoot": str(root / "repository"),
            "projectRoot": str(project_root),
            "datasetRoot": str(dataset_root),
            "scene": "capture",
            "files": [{"path": str(source)}],
            "overwrite": overwrite,
        }

    def write_import_transaction(
        self, dataset_root, scene, state, final_path, staging_path, backup_path
    ):
        journal_path = gsw_worker.import_journal_path(dataset_root, scene)
        gsw_worker.write_import_journal(
            journal_path,
            {
                "version": 1,
                "scene": scene,
                "state": state,
                "finalPath": str(final_path),
                "stagingPath": str(staging_path),
                "backupPath": str(backup_path),
            },
        )
        return journal_path

    def test_load_configuration_rejects_missing_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "job.json"
            path.write_text('{"backend": "3dgs"}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Missing worker configuration fields"):
                gsw_worker.load_configuration(path)

    def test_load_configuration_accepts_colmap_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "job.json"
            path.write_text(
                '{"task":"colmap","repositoryRoot":"repo","datasetPath":"dataset"}',
                encoding="utf-8",
            )
            config = gsw_worker.load_configuration(path)
            self.assertEqual(config["task"], "colmap")

    def test_load_configuration_requires_project_root_for_import(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "import.json"
            path.write_text(
                '{"task":"import","repositoryRoot":"repo","datasetRoot":"datasets",'
                '"scene":"capture","files":[{"path":"frame.jpg"}]}',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "projectRoot"):
                gsw_worker.load_configuration(path)

    def test_import_location_must_be_the_project_datasets_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = {
                "projectRoot": str(root / "project"),
                "datasetRoot": str(root / "outside"),
                "scene": "capture",
            }
            with self.assertRaisesRegex(ValueError, "datasetRoot"):
                gsw_worker.resolve_import_location(config)

    def test_run_import_publishes_new_managed_dataset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.import_configuration(root)
            backend = FakeImportBackend(imported_bytes=b"first import")

            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_import(config)

            imported = root / "project" / "datasets" / "capture" / "images" / "frame.jpg"
            self.assertEqual(exit_code, 0)
            self.assertEqual(imported.read_bytes(), b"first import")
            journal = gsw_worker.import_journal_path(
                root / "project" / "datasets", "capture"
            )
            self.assertEqual(
                json.loads(journal.read_text(encoding="utf-8"))["state"],
                "committed",
            )

    def test_run_import_uses_backend_safe_staging_scene(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.import_configuration(root)
            config["scene"] = "C0001"
            backend = SafeNameImportBackend(imported_bytes=b"video frame")

            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_import(config)

            self.assertEqual(exit_code, 0)
            self.assertFalse(backend.received_scene.startswith("."))
            imported = (
                root / "project" / "datasets" / "C0001" / "images" / "frame.jpg"
            )
            self.assertEqual(imported.read_bytes(), b"video frame")

    def test_run_import_bounds_staging_name_for_maximum_scene_length(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.import_configuration(root)
            config["scene"] = "a" * 120
            backend = SafeNameImportBackend()

            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_import(config)

            self.assertEqual(exit_code, 0)
            self.assertLessEqual(len(backend.received_scene), 120)
            self.assertTrue(
                (root / "project" / "datasets" / ("a" * 120)).is_dir()
            )

    def test_run_import_overwrites_existing_dataset_with_new_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.import_configuration(root, overwrite=True)
            final = root / "project" / "datasets" / "capture"
            final.mkdir()
            (final / "old.txt").write_text("old", encoding="utf-8")
            backend = FakeImportBackend(imported_bytes=b"replacement")

            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_import(config)

            self.assertEqual(exit_code, 0)
            self.assertFalse((final / "old.txt").exists())
            self.assertEqual((final / "images" / "frame.jpg").read_bytes(), b"replacement")

    def test_run_import_backend_failure_preserves_existing_dataset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.import_configuration(root, overwrite=True)
            final = root / "project" / "datasets" / "capture"
            final.mkdir()
            original = final / "old.txt"
            original.write_text("keep me", encoding="utf-8")
            backend = FakeImportBackend(error="backend import failed")

            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_import(config)

            self.assertEqual(exit_code, 1)
            self.assertEqual(original.read_text(encoding="utf-8"), "keep me")
            self.assertFalse((final / "images" / "frame.jpg").exists())

    def test_run_training_forwards_native_configuration_and_succeeds(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backend = FakeBackend()
            idle_thread = mock.Mock()
            idle_thread.start = mock.Mock()
            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                mock.patch.object(gsw_worker.threading, "Thread", return_value=idle_thread),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_training(self.configuration(root))

            self.assertEqual(exit_code, 0)
            self.assertEqual(backend.OUTPUT_DIR, (root / "output").resolve())
            self.assertEqual(backend.TRAIN_JOBS_DIR, (root / "jobs").resolve())
            self.assertEqual(backend.call[1]["dataset_path"], str(root / "dataset"))
            self.assertEqual(backend.call[0][1], "test-scene")

    def test_run_training_returns_failure_for_failed_backend_job(self):
        with tempfile.TemporaryDirectory() as temporary:
            backend = FakeBackend(state="failed", error="training failed")
            idle_thread = mock.Mock()
            idle_thread.start = mock.Mock()
            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                mock.patch.object(gsw_worker.threading, "Thread", return_value=idle_thread),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_training(self.configuration(Path(temporary)))

            self.assertEqual(exit_code, 1)

    def test_run_colmap_forwards_absolute_dataset_and_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dataset = root / "dataset with spaces"
            dataset.mkdir()
            colmap = root / "colmap.exe"
            colmap.write_bytes(b"test")
            backend = FakeColmapBackend()
            idle_thread = mock.Mock()
            idle_thread.start = mock.Mock()
            config = {
                "task": "colmap",
                "repositoryRoot": str(root),
                "datasetPath": str(dataset),
                "scene": "qa-scene",
                "colmapExecutable": str(colmap),
                "colmapOptions": {"preset": "robust", "use_gpu": False},
            }
            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                mock.patch.object(gsw_worker.threading, "Thread", return_value=idle_thread),
                mock.patch.dict(os.environ, {}, clear=False),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_colmap(config)
                self.assertEqual(os.environ["COLMAP_PATH"], str(colmap.resolve()))

            self.assertEqual(exit_code, 0)
            self.assertEqual(backend.call["scene"], "qa-scene")
            self.assertEqual(backend.call["dataset_path"], str(dataset.resolve()))
            self.assertEqual(backend.call["options"]["preset"], "robust")

    def test_run_colmap_rejects_incomplete_sparse_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dataset = root / "dataset"
            dataset.mkdir()
            colmap = root / "colmap.exe"
            colmap.write_bytes(b"test")
            backend = FakeColmapBackend(valid_output=False)
            idle_thread = mock.Mock()
            idle_thread.start = mock.Mock()
            config = {
                "task": "colmap",
                "repositoryRoot": str(root),
                "datasetPath": str(dataset),
                "colmapExecutable": str(colmap),
            }
            with (
                mock.patch.object(gsw_worker, "import_backend", return_value=backend),
                mock.patch.object(gsw_worker.threading, "Thread", return_value=idle_thread),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                exit_code = gsw_worker.run_colmap(config)

            self.assertEqual(exit_code, 1)

    def test_recover_interrupted_import_restores_backup_and_removes_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            dataset_root.mkdir()
            backup = dataset_root / ".capture.backup-test"
            staging = dataset_root / ".capture.import-test"
            backup.mkdir()
            staging.mkdir()
            (backup / "old.txt").write_text("old", encoding="utf-8")
            (staging / "partial.txt").write_text("partial", encoding="utf-8")

            result = gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertTrue(result["restored"])
            self.assertEqual((dataset_root / "capture" / "old.txt").read_text(encoding="utf-8"), "old")
            self.assertFalse(backup.exists())
            self.assertFalse(staging.exists())

    def test_recover_interrupted_import_removes_backend_safe_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            dataset_root.mkdir()
            final = dataset_root / "capture"
            staging = dataset_root / gsw_worker._staging_scene_name(
                "capture", "a" * 32
            )
            backup = dataset_root / ".capture.backup-test"
            staging.mkdir()
            (staging / "partial.txt").write_text("partial", encoding="utf-8")
            journal = self.write_import_transaction(
                dataset_root, "capture", "staging", final, staging, backup
            )

            result = gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertFalse(result["restored"])
            self.assertFalse(staging.exists())
            self.assertFalse(journal.exists())

    def test_recover_import_preserves_final_and_removes_obsolete_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            final = dataset_root / "capture"
            backup = dataset_root / ".capture.backup-test"
            staging = dataset_root / ".capture.import-test"
            final.mkdir(parents=True)
            backup.mkdir()
            staging.mkdir()
            (final / "current.txt").write_text("current", encoding="utf-8")

            result = gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertFalse(result["restored"])
            self.assertEqual((final / "current.txt").read_text(encoding="utf-8"), "current")
            self.assertFalse(backup.exists())
            self.assertFalse(staging.exists())

    def test_recovery_rejects_regular_file_target_without_deleting_backup(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            final = dataset_root / "capture"
            backup = dataset_root / ".capture.backup-test"
            dataset_root.mkdir()
            final.write_text("not a dataset directory", encoding="utf-8")
            backup.mkdir()
            (backup / "old.txt").write_text("old", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "normal directory"):
                gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertTrue(final.is_file())
            self.assertEqual(
                (backup / "old.txt").read_text(encoding="utf-8"), "old"
            )

    def test_recover_publishing_transaction_restores_backup_over_new_final(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            final = dataset_root / "capture"
            staging = dataset_root / ".capture.import-test"
            backup = dataset_root / ".capture.backup-test"
            final.mkdir(parents=True)
            backup.mkdir()
            (final / "version.txt").write_text("new", encoding="utf-8")
            (backup / "version.txt").write_text("old", encoding="utf-8")
            journal = self.write_import_transaction(
                dataset_root, "capture", "publishing", final, staging, backup
            )

            result = gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertEqual(result["outcome"], "rolled_back")
            self.assertEqual((final / "version.txt").read_text(encoding="utf-8"), "old")
            self.assertFalse(backup.exists())
            self.assertFalse(journal.exists())

    def test_recover_committed_transaction_keeps_new_final_and_reports_committed(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            final = dataset_root / "capture"
            staging = dataset_root / ".capture.import-test"
            backup = dataset_root / ".capture.backup-test"
            final.mkdir(parents=True)
            backup.mkdir()
            (final / "version.txt").write_text("new", encoding="utf-8")
            (backup / "version.txt").write_text("old", encoding="utf-8")
            journal = self.write_import_transaction(
                dataset_root, "capture", "committed", final, staging, backup
            )

            result = gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertEqual(result["outcome"], "committed")
            self.assertEqual((final / "version.txt").read_text(encoding="utf-8"), "new")
            self.assertFalse(backup.exists())
            self.assertFalse(journal.exists())

    def test_recover_committed_transaction_fails_if_both_versions_are_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            dataset_root = Path(temporary) / "datasets"
            dataset_root.mkdir()
            final = dataset_root / "capture"
            staging = dataset_root / ".capture.import-test"
            backup = dataset_root / ".capture.backup-test"
            journal = self.write_import_transaction(
                dataset_root, "capture", "committed", final, staging, backup
            )

            with self.assertRaisesRegex(RuntimeError, "committed dataset is missing"):
                gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertTrue(journal.exists())

    def test_remove_path_unlinks_reparse_directory_without_recursive_delete(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / ".capture.import-test"
            artifact.mkdir()

            with mock.patch.object(gsw_worker, "_is_reparse_point", return_value=True), \
                    mock.patch.object(
                        gsw_worker.shutil,
                        "rmtree",
                        side_effect=AssertionError("must not traverse a reparse point"),
                    ):
                gsw_worker.remove_path(artifact, allowed_root=root)

            self.assertFalse(artifact.exists())

    def test_remove_path_rejects_target_outside_allowed_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            allowed_root = root / "datasets"
            outside = root / "outside"
            allowed_root.mkdir()
            outside.mkdir()

            with self.assertRaisesRegex(ValueError, "outside the allowed root"):
                gsw_worker.remove_path(outside, allowed_root=allowed_root)

            self.assertTrue(outside.exists())

    def test_recovery_rejects_artifact_path_outside_dataset_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dataset_root = root / "datasets"
            final = dataset_root / "capture"
            backup = dataset_root / ".capture.backup-test"
            outside = root / ".capture.import-outside"
            final.mkdir(parents=True)
            backup.mkdir()
            outside.mkdir()
            journal = gsw_worker.import_journal_path(dataset_root, "capture")
            gsw_worker.write_import_journal(
                journal,
                {
                    "version": 1,
                    "scene": "capture",
                    "state": "publishing",
                    "finalPath": str(final),
                    "stagingPath": str(outside),
                    "backupPath": str(backup),
                },
            )

            with self.assertRaisesRegex(ValueError, "outside the allowed root"):
                gsw_worker.recover_import_artifacts(dataset_root, "capture")

            self.assertTrue(outside.exists())
            self.assertTrue(journal.exists())

    def test_publish_import_rolls_back_when_commit_callback_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            final = root / "capture"
            staging = root / ".capture.import-test"
            backup = root / ".capture.backup-test"
            final.mkdir()
            staging.mkdir()
            (final / "version.txt").write_text("old", encoding="utf-8")
            (staging / "version.txt").write_text("new", encoding="utf-8")

            def fail_commit():
                raise RuntimeError("journal commit failed")

            with self.assertRaisesRegex(RuntimeError, "journal commit failed"):
                gsw_worker.publish_import(
                    staging,
                    final,
                    backup,
                    True,
                    commit_callback=fail_commit,
                )

            self.assertEqual((final / "version.txt").read_text(encoding="utf-8"), "old")
            self.assertFalse(backup.exists())

    def test_project_recovery_scans_journals_without_a_scene_argument(self):
        with tempfile.TemporaryDirectory() as temporary:
            project_root = Path(temporary) / "project"
            dataset_root = project_root / "datasets"
            final = dataset_root / "capture"
            staging = dataset_root / ".capture.import-test"
            backup = dataset_root / ".capture.backup-test"
            final.mkdir(parents=True)
            backup.mkdir()
            (final / "version.txt").write_text("new", encoding="utf-8")
            (backup / "version.txt").write_text("old", encoding="utf-8")
            journal = self.write_import_transaction(
                dataset_root, "capture", "publishing", final, staging, backup
            )
            config = {
                "projectRoot": str(project_root),
                "datasetRoot": str(dataset_root),
            }

            with contextlib.redirect_stdout(io.StringIO()):
                exit_code = gsw_worker.run_import_project_recovery(config)

            self.assertEqual(exit_code, 0)
            self.assertEqual((final / "version.txt").read_text(encoding="utf-8"), "old")
            self.assertFalse(journal.exists())

    def test_project_recovery_reports_committed_dataset_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            project_root = Path(temporary) / "project"
            dataset_root = project_root / "datasets"
            final = dataset_root / "capture"
            staging = dataset_root / ".capture.import-test"
            backup = dataset_root / ".capture.backup-test"
            final.mkdir(parents=True)
            backup.mkdir()
            (final / "version.txt").write_text("new", encoding="utf-8")
            self.write_import_transaction(
                dataset_root, "capture", "committed", final, staging, backup
            )
            output = io.StringIO()

            with contextlib.redirect_stdout(output):
                exit_code = gsw_worker.run_import_project_recovery(
                    {
                        "projectRoot": str(project_root),
                        "datasetRoot": str(dataset_root),
                    }
                )

            event_line = next(
                line for line in output.getvalue().splitlines()
                if line.startswith("[worker-recovery] ")
            )
            event = json.loads(event_line.split(" ", 1)[1])
            self.assertEqual(exit_code, 0)
            self.assertEqual(event["committedPaths"], [str(final.resolve())])
            self.assertEqual(
                (final / "version.txt").read_text(encoding="utf-8"), "new"
            )

    def test_publish_import_rolls_back_when_staging_publish_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            final = root / "capture"
            missing_staging = root / ".capture.import-missing"
            backup = root / ".capture.backup-test"
            final.mkdir()
            (final / "old.txt").write_text("old", encoding="utf-8")

            with self.assertRaises(FileNotFoundError):
                gsw_worker.publish_import(missing_staging, final, backup, True)

            self.assertEqual((final / "old.txt").read_text(encoding="utf-8"), "old")
            self.assertFalse(backup.exists())


if __name__ == "__main__":
    unittest.main()
