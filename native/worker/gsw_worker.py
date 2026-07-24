"""Headless compute worker used by the native Qt application."""

import argparse
import contextlib
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import threading
import time
import uuid
from pathlib import Path


FINAL_STATES = {"done", "failed", "cancelled"}
SCENE_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+$")
WINDOWS_DEVICE_NAMES = {
    "CON", "PRN", "AUX", "NUL",
    *("COM{}".format(index) for index in range(1, 10)),
    *("LPT{}".format(index) for index in range(1, 10)),
}


def configure_stdout():
    try:
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8", line_buffering=True)
    except (AttributeError, ValueError):
        pass


def load_configuration(path, requested_task=None):
    with open(path, "r", encoding="utf-8") as stream:
        config = json.load(stream)
    task = requested_task or config.get("task") or "training"
    required_by_task = {
        "training": (
            "repositoryRoot",
            "datasetPath",
            "outputRoot",
            "outputScene",
            "backend",
            "quality",
        ),
        "colmap": ("repositoryRoot", "datasetPath"),
        "import": ("repositoryRoot", "projectRoot", "datasetRoot", "scene", "files"),
        "import-recovery": ("projectRoot", "datasetRoot", "scene"),
        "import-project-recovery": ("projectRoot", "datasetRoot"),
    }
    if task not in required_by_task:
        raise ValueError("Unsupported worker task: {}".format(task))
    required = required_by_task[task]
    missing = [name for name in required if not config.get(name)]
    if missing:
        raise ValueError("Missing worker configuration fields: " + ", ".join(missing))
    config["task"] = task
    return config


def import_backend(repository_root):
    crop_editor = Path(repository_root) / "crop_editor"
    server_path = crop_editor / "server.py"
    if not server_path.exists():
        raise FileNotFoundError("Training backend is missing: {}".format(server_path))
    sys.path.insert(0, str(crop_editor))
    import server  # pylint: disable=import-error,import-outside-toplevel
    return server


def watch_cancel_input(cancel_callback, job_id):
    for line in sys.stdin:
        if line.strip().lower() != "cancel":
            continue
        print("[worker] Cancellation requested by the desktop application.")
        try:
            cancel_callback(job_id)
        except Exception as exc:  # cancellation must still let the main loop finish
            print("[worker] Cancellation error: {}".format(exc))
        return


def emit_status(state, stage, progress_percent=None):
    """Emit one machine-readable status line for the native application."""
    payload = {
        "version": 1,
        "type": "status",
        "state": str(state),
        "stage": str(stage),
    }
    if progress_percent is not None:
        progress = int(round(float(progress_percent)))
        payload["progressPercent"] = max(0, min(100, progress))
    print(
        "[worker-event] "
        + json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
        flush=True,
    )


def job_progress_percent(job):
    for name in ("progressPercent", "progress_percent"):
        value = job.get(name)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return value
    iteration = job.get("iteration")
    total = job.get("total_iterations") or job.get("totalIterations")
    if isinstance(iteration, (int, float)) and isinstance(total, (int, float)) and total > 0:
        return (iteration / total) * 100.0
    return None


def stream_job(lock, jobs, job_id):
    next_log_index = 0
    state = "queued"
    error = None
    previous_stage = None
    while state not in FINAL_STATES:
        with lock:
            job = jobs[job_id]
            lines = list(job.get("log", [])[next_log_index:])
            next_log_index += len(lines)
            state = job.get("status", "failed")
            stage = job.get("stage", state)
            error = job.get("error")
            progress_percent = job_progress_percent(job)
        for line in lines:
            print(line)
        current_stage = (state, stage, progress_percent)
        if current_stage != previous_stage:
            emit_status(state, stage, progress_percent)
            previous_stage = current_stage
        if state not in FINAL_STATES:
            time.sleep(0.5)

    with lock:
        job = jobs[job_id]
        lines = list(job.get("log", [])[next_log_index:])
        error = job.get("error")
    for line in lines:
        print(line)
    return state, error


def import_progress_percent(snapshot):
    """Map backend counters to monotonic, stage-aware import progress."""
    stage = snapshot.get("stage", "queued")
    total_files = max(int(snapshot.get("total_files") or 0), 0)
    total_videos = max(int(snapshot.get("total_videos") or 0), 0)
    total_images = max(total_files - total_videos, 0)

    if stage == "queued":
        return 0
    if stage == "preparing":
        return 2
    if stage == "archiving":
        processed = max(int(snapshot.get("processed_files") or 0), 0)
        fraction = min(processed / max(total_files, 1), 1.0)
        return 5 + round(fraction * 40)
    if stage == "copying_images":
        saved = max(int(snapshot.get("saved_images") or 0), 0)
        fraction = min(saved / max(total_images, 1), 1.0)
        return 45 + round(fraction * 20)
    if stage == "extracting_frames":
        processed = max(int(snapshot.get("processed_videos") or 0), 0)
        fraction = min(processed / max(total_videos, 1), 1.0)
        return 65 + round(fraction * 30)
    if stage == "masks":
        return 97
    if stage == "done":
        # Publishing the staging directory is the final two-phase commit step.
        return 99
    return None


def validate_scene_name(name):
    scene = str(name or "")
    device_base = scene.split(".", 1)[0].upper()
    if (
        not scene
        or len(scene) > 120
        or scene.startswith(".")
        or scene.endswith(".")
        or ".." in scene
        or not SCENE_NAME_PATTERN.fullmatch(scene)
        or device_base in WINDOWS_DEVICE_NAMES
    ):
        raise ValueError("Invalid scene name")
    return scene


def resolve_project_dataset_roots(config):
    raw_project_root = Path(str(config.get("projectRoot") or ""))
    raw_dataset_root = Path(str(config.get("datasetRoot") or ""))
    if not raw_project_root.is_absolute() or not raw_dataset_root.is_absolute():
        raise ValueError("projectRoot and datasetRoot must be absolute paths")

    project_root = raw_project_root.resolve()
    dataset_root = raw_dataset_root.resolve()
    expected_dataset_root = (project_root / "datasets").resolve()
    if os.path.normcase(str(dataset_root)) != os.path.normcase(str(expected_dataset_root)):
        raise ValueError("datasetRoot must be the project's datasets directory")
    try:
        dataset_root.relative_to(project_root)
    except ValueError as exc:
        raise ValueError("datasetRoot must remain inside projectRoot") from exc
    return project_root, dataset_root


def resolve_import_location(config):
    project_root, dataset_root = resolve_project_dataset_roots(config)
    return project_root, dataset_root, validate_scene_name(config.get("scene"))


def _path_exists_no_follow(path):
    try:
        os.lstat(path)
        return True
    except FileNotFoundError:
        return False


def _is_reparse_point(path):
    """Return true for links and Windows directory junctions without following them."""
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        return False
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return stat.S_ISLNK(metadata.st_mode) or bool(
        getattr(metadata, "st_file_attributes", 0) & reparse_flag
    )


def _lexical_absolute(path):
    return Path(os.path.abspath(os.fspath(path)))


def _ensure_within_root(path, allowed_root, direct_child=False):
    """Validate containment lexically so a junction target is never resolved."""
    root = _lexical_absolute(allowed_root).resolve()
    target = _lexical_absolute(path)
    if direct_child:
        # Resolve only the parent directories. Resolving the target itself would
        # follow the exact junction/reparse point this guard is meant to contain.
        canonical_parent = target.parent.resolve()
        if os.path.normcase(str(canonical_parent)) != os.path.normcase(str(root)):
            raise ValueError("Path is outside the allowed root: {}".format(target))
        return root / target.name
    try:
        common = os.path.commonpath((str(root), str(target)))
    except ValueError as exc:
        raise ValueError("Path is outside the allowed root: {}".format(target)) from exc
    if os.path.normcase(common) != os.path.normcase(str(root)):
        raise ValueError("Path is outside the allowed root: {}".format(target))
    return target


def _is_normal_directory(path):
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        return False
    return stat.S_ISDIR(metadata.st_mode) and not _is_reparse_point(path)


@contextlib.contextmanager
def import_transaction_lock(dataset_root, scene):
    """Hold an OS-released lock for one managed scene import."""
    dataset_root = _lexical_absolute(dataset_root)
    lock_path = _ensure_within_root(
        dataset_root / ".{}.import.lock".format(scene),
        dataset_root,
        direct_child=True,
    )
    if _is_reparse_point(lock_path):
        raise RuntimeError("Import lock must not be a link or reparse point: {}".format(lock_path))
    if _path_exists_no_follow(lock_path) and not stat.S_ISREG(os.lstat(lock_path).st_mode):
        raise RuntimeError("Import lock must be a regular file: {}".format(lock_path))
    lock_file = open(lock_path, "a+b")
    lock_file.seek(0, os.SEEK_END)
    if lock_file.tell() == 0:
        lock_file.write(b"\0")
        lock_file.flush()

    try:
        if os.name == "nt":
            import msvcrt

            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        lock_file.close()
        raise RuntimeError("Another import is already active for scene: {}".format(scene)) from exc

    try:
        yield
    finally:
        try:
            if os.name == "nt":
                import msvcrt

                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
        finally:
            lock_file.close()


def _unlink_reparse_point(path):
    try:
        path.unlink()
    except (IsADirectoryError, PermissionError):
        # Windows directory junctions are removed with rmdir, never rmtree.
        os.rmdir(path)


def _remove_tree_safely(path, allowed_root):
    with os.scandir(path) as entries:
        for entry in entries:
            child = _ensure_within_root(entry.path, allowed_root)
            if _is_reparse_point(child):
                _unlink_reparse_point(child)
                continue
            metadata = os.lstat(child)
            if stat.S_ISDIR(metadata.st_mode):
                _remove_tree_safely(child, allowed_root)
                os.rmdir(child)
            else:
                child.unlink()


def remove_path(path, allowed_root):
    """Remove one direct child without ever traversing a reparse point."""
    path = _ensure_within_root(path, allowed_root, direct_child=True)
    if not _path_exists_no_follow(path):
        return False
    if _is_reparse_point(path):
        _unlink_reparse_point(path)
        return True
    metadata = os.lstat(path)
    if stat.S_ISDIR(metadata.st_mode):
        _remove_tree_safely(path, allowed_root)
        os.rmdir(path)
    else:
        path.unlink()
    return True


def import_journal_path(dataset_root, scene):
    scene = validate_scene_name(scene)
    dataset_root = _lexical_absolute(dataset_root)
    return _ensure_within_root(
        dataset_root / ".{}.import-transaction.json".format(scene),
        dataset_root,
        direct_child=True,
    )


def write_import_journal(path, payload):
    """Atomically persist an import transaction state before filesystem renames."""
    path = _lexical_absolute(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if _is_reparse_point(path):
        raise RuntimeError("Import journal must not be a link or reparse point: {}".format(path))
    temporary = path.with_name("{}.tmp-{}".format(path.name, uuid.uuid4().hex))
    try:
        with open(temporary, "x", encoding="utf-8") as stream:
            json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if _path_exists_no_follow(temporary):
            temporary.unlink()


def _read_import_journal(path):
    if _is_reparse_point(path):
        raise RuntimeError("Import journal must not be a link or reparse point: {}".format(path))
    with open(path, "r", encoding="utf-8") as stream:
        payload = json.load(stream)
    if not isinstance(payload, dict) or payload.get("version") != 1:
        raise RuntimeError("Unsupported import transaction journal: {}".format(path))
    return payload


def _journal_artifact_path(payload, field, dataset_root, expected_prefix):
    raw_value = payload.get(field)
    if not isinstance(raw_value, str) or not raw_value:
        raise RuntimeError("Import transaction journal is missing {}".format(field))
    path = _ensure_within_root(Path(raw_value), dataset_root, direct_child=True)
    if not path.name.startswith(expected_prefix):
        raise RuntimeError("Import transaction journal has an invalid {}".format(field))
    return path


def _staging_scene_name(scene, token):
    scene_digest = hashlib.sha256(scene.encode("utf-8")).hexdigest()[:16]
    return "gsw-import-{}-{}".format(scene_digest, token)


def _is_staging_artifact_name(name, scene):
    legacy_prefix = ".{}.import-".format(scene)
    if name.startswith(legacy_prefix):
        return True
    scene_digest = hashlib.sha256(scene.encode("utf-8")).hexdigest()[:16]
    return re.fullmatch(
        r"gsw-import-{}-[0-9a-f]{{32}}".format(scene_digest), name
    ) is not None


def _journal_staging_path(payload, dataset_root, scene):
    path = _journal_artifact_path(payload, "stagingPath", dataset_root, "")
    if not _is_staging_artifact_name(path.name, scene):
        raise RuntimeError("Import transaction journal has an invalid stagingPath")
    return path


def _scene_artifacts(dataset_root, scene):
    backup_prefix = ".{}.backup-".format(scene)
    legacy_staging_prefix = ".{}.import-".format(scene)
    journal_name = ".{}.import-transaction.json".format(scene)
    backups = []
    staging_paths = []
    journal_temps = []
    if not dataset_root.exists():
        return backups, staging_paths, journal_temps
    for entry in dataset_root.iterdir():
        name = entry.name
        if name.startswith(backup_prefix):
            backups.append(entry)
        elif name.startswith("{}.tmp-".format(journal_name)):
            journal_temps.append(entry)
        elif name.startswith(legacy_staging_prefix) and name != journal_name:
            staging_paths.append(entry)
    return backups, staging_paths, journal_temps


def _recover_import_artifacts(dataset_root, scene):
    dataset_root = _lexical_absolute(dataset_root)
    final_path = dataset_root / scene
    final_path = _ensure_within_root(final_path, dataset_root, direct_child=True)
    journal_path = import_journal_path(dataset_root, scene)
    backups, staging_paths, journal_temps = _scene_artifacts(dataset_root, scene)
    restored = False
    committed = False
    outcome = "cleaned"
    removed_artifacts = 0

    if _is_reparse_point(final_path):
        raise RuntimeError("Managed dataset target must not be a link or reparse point: {}".format(final_path))
    if _path_exists_no_follow(final_path) and not _is_normal_directory(final_path):
        raise RuntimeError("Managed dataset target must be a normal directory: {}".format(final_path))

    journal = None
    if _path_exists_no_follow(journal_path):
        journal = _read_import_journal(journal_path)
        if journal.get("scene") != scene:
            raise RuntimeError("Import transaction journal scene does not match its filename")
        state = journal.get("state")
        if state not in {"staging", "publishing", "committed"}:
            raise RuntimeError("Import transaction journal has an invalid state")
        expected_final = _journal_artifact_path(journal, "finalPath", dataset_root, scene)
        if os.path.normcase(str(expected_final)) != os.path.normcase(str(final_path)):
            raise RuntimeError("Import transaction journal has an invalid finalPath")
        staging_path = _journal_staging_path(journal, dataset_root, scene)
        backup_path = _journal_artifact_path(
            journal, "backupPath", dataset_root, ".{}.backup-".format(scene)
        )
        for artifact, collection in ((staging_path, staging_paths), (backup_path, backups)):
            if artifact not in collection and _path_exists_no_follow(artifact):
                collection.append(artifact)

        final_exists = _path_exists_no_follow(final_path)
        backup_exists = _path_exists_no_follow(backup_path)
        had_final = bool(journal.get("hadFinal", backup_exists))

        if state == "committed" and final_exists:
            committed = True
            outcome = "committed"
        elif state == "committed" and backup_exists:
            if not _is_normal_directory(backup_path):
                raise RuntimeError("Import backup cannot be restored safely: {}".format(backup_path))
            backup_path.rename(final_path)
            backups.remove(backup_path)
            restored = True
            outcome = "rolled_back"
        elif state == "committed":
            raise RuntimeError(
                "The committed dataset is missing and no rollback backup remains: {}".format(
                    final_path
                )
            )
        elif state != "committed":
            outcome = "rolled_back"
            if backup_exists:
                if not _is_normal_directory(backup_path):
                    raise RuntimeError("Import backup cannot be restored safely: {}".format(backup_path))
                if final_exists:
                    removed_artifacts += int(remove_path(final_path, dataset_root))
                backup_path.rename(final_path)
                backups.remove(backup_path)
                restored = True
            elif not had_final and state == "publishing" and final_exists:
                removed_artifacts += int(remove_path(final_path, dataset_root))
            elif had_final and not final_exists:
                raise RuntimeError(
                    "The original dataset is missing and its rollback backup is unavailable: {}".format(
                        final_path
                    )
                )
    elif not _path_exists_no_follow(final_path) and backups:
        restorable = [path for path in backups if _is_normal_directory(path)]
        if restorable:
            newest_backup = max(restorable, key=lambda path: os.lstat(path).st_mtime_ns)
            newest_backup.rename(final_path)
            backups.remove(newest_backup)
            restored = True
            outcome = "rolled_back"

    for artifact in backups + staging_paths + journal_temps:
        removed_artifacts += int(remove_path(artifact, dataset_root))
    if journal is not None:
        removed_artifacts += int(remove_path(journal_path, dataset_root))

    return {
        "restored": restored,
        "committed": committed,
        "outcome": outcome,
        "finalPath": str(final_path),
        "removedArtifacts": removed_artifacts,
    }


def recover_import_artifacts(dataset_root, scene):
    dataset_root = Path(dataset_root).resolve()
    dataset_root.mkdir(parents=True, exist_ok=True)
    scene = validate_scene_name(scene)
    with import_transaction_lock(dataset_root, scene):
        return _recover_import_artifacts(dataset_root, scene)


def publish_import(staging_path, final_path, backup_path, overwrite, commit_callback=None):
    """Atomically publish staging, rolling back an overwritten dataset on error."""
    final_path = _lexical_absolute(final_path)
    dataset_root = final_path.parent
    staging_path = _ensure_within_root(staging_path, dataset_root, direct_child=True)
    backup_path = _ensure_within_root(backup_path, dataset_root, direct_child=True)
    if _is_reparse_point(final_path):
        raise RuntimeError("Managed dataset target must not be a link or reparse point: {}".format(final_path))
    if _path_exists_no_follow(final_path) and not _is_normal_directory(final_path):
        raise RuntimeError("Managed dataset target must be a normal directory: {}".format(final_path))
    if not _path_exists_no_follow(staging_path):
        raise FileNotFoundError("Import staging is missing: {}".format(staging_path))
    if not _is_normal_directory(staging_path):
        raise RuntimeError("Import staging must be a normal directory: {}".format(staging_path))
    if _path_exists_no_follow(backup_path):
        raise RuntimeError("Import backup path already exists: {}".format(backup_path))

    backup_created = False
    published = False
    if _path_exists_no_follow(final_path):
        if not overwrite:
            raise FileExistsError("Dataset already exists: {}".format(final_path))
        final_path.rename(backup_path)
        backup_created = True

    try:
        staging_path.rename(final_path)
        published = True
        if commit_callback is not None:
            commit_callback()
    except Exception as publish_error:
        rollback_error = None
        if published and _path_exists_no_follow(final_path):
            try:
                remove_path(final_path, dataset_root)
            except Exception as exc:  # preserve the backup and report both failures
                rollback_error = exc
        if backup_created:
            try:
                if rollback_error is None:
                    backup_path.rename(final_path)
            except Exception as exc:
                rollback_error = exc
        if rollback_error is not None:
            raise RuntimeError(
                "Could not publish imported dataset ({}); rollback is preserved at {} ({})".format(
                    publish_error, backup_path, rollback_error
                )
            ) from publish_error
        raise

    if backup_created:
        try:
            remove_path(backup_path, dataset_root)
        except OSError as exc:
            print("[worker] Warning: could not remove import backup {}: {}".format(backup_path, exc))


def _run_import_locked(config, server, dataset_root, scene):
    server.DATASETS_DIR = dataset_root
    final_path = dataset_root / scene
    overwrite = bool(config.get("overwrite", False))
    if _is_reparse_point(final_path):
        raise RuntimeError("Managed dataset target must not be a link or reparse point: {}".format(final_path))
    if _path_exists_no_follow(final_path) and not _is_normal_directory(final_path):
        raise RuntimeError("Managed dataset target must be a normal directory: {}".format(final_path))
    had_final = _path_exists_no_follow(final_path)
    if had_final and not overwrite:
        raise FileExistsError("Dataset already exists: {}".format(final_path))

    token = uuid.uuid4().hex
    staging_scene = _staging_scene_name(scene, token)
    staging_path = dataset_root / staging_scene
    backup_path = dataset_root / ".{}.backup-{}".format(scene, token)
    journal_path = import_journal_path(dataset_root, scene)
    job_id = "native-import-{}".format(token)
    outcome = {}
    journal = {
        "version": 1,
        "scene": scene,
        "state": "staging",
        "hadFinal": had_final,
        "finalPath": str(final_path),
        "stagingPath": str(staging_path),
        "backupPath": str(backup_path),
    }
    write_import_journal(journal_path, journal)

    request = {
        "scene": staging_scene,
        "overwrite": False,
        "fps": config.get("fps", 2),
        "files": config["files"],
        "import_job_id": job_id,
    }

    def invoke_import():
        try:
            outcome["result"] = server.save_path_dataset(request)
        except Exception as exc:  # surfaced after status polling completes
            outcome["error"] = exc

    print("[worker] Importing {} media file(s) into staging.".format(len(config["files"])))
    emit_status("running", "queued", 0)
    import_thread = threading.Thread(
        target=invoke_import,
        name="gsw-media-import",
        daemon=True,
    )
    import_thread.start()

    previous_status = None
    last_snapshot = None
    try:
        while import_thread.is_alive():
            try:
                snapshot = server.import_job_snapshot(job_id)
            except ValueError:
                import_thread.join(0.05)
                continue
            last_snapshot = snapshot
            state = snapshot.get("status", "running")
            stage = snapshot.get("stage", state)
            progress = import_progress_percent(snapshot)
            # Backend completion still needs an atomic publish before it is final.
            event = (
                "running" if state == "done" else state,
                "finalizing" if state == "done" else stage,
                99 if state == "done" else progress,
            )
            if event != previous_status:
                emit_status(*event)
                previous_status = event
            import_thread.join(0.1)

        import_thread.join()
        try:
            last_snapshot = server.import_job_snapshot(job_id)
        except ValueError:
            pass

        if "error" in outcome:
            raise outcome["error"]
        if not last_snapshot or last_snapshot.get("status") != "done":
            error = (last_snapshot or {}).get("error") or "backend import did not complete"
            raise RuntimeError(error)
        if not staging_path.is_dir():
            raise RuntimeError("Backend import did not create staging dataset: {}".format(staging_path))

        finalizing = ("running", "finalizing", 99)
        if previous_status != finalizing:
            emit_status(*finalizing)
        journal["state"] = "publishing"
        write_import_journal(journal_path, journal)

        def mark_committed():
            journal["state"] = "committed"
            write_import_journal(journal_path, journal)

        publish_import(
            staging_path,
            final_path,
            backup_path,
            overwrite,
            commit_callback=mark_committed,
        )
        result = outcome.get("result") or {}
        print("[worker] Media import completed successfully.")
        print("[worker] Dataset: {}".format(final_path))
        print("[worker] Images: {}".format(result.get("image_count", 0)))
        emit_status("done", "done", 100)
        return 0
    except Exception as exc:
        stage = (last_snapshot or {}).get("stage") or "failed"
        progress = import_progress_percent(last_snapshot or {})
        emit_status("failed", stage, progress)
        print("[worker] Media import failed: {}".format(exc))
        return 1
    finally:
        try:
            remove_path(staging_path, dataset_root)
        except OSError as exc:
            print("[worker] Warning: could not clean import staging {}: {}".format(staging_path, exc))


def validate_import_files(config, server):
    files = config.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("Import configuration must contain at least one media file")
    supported_extensions = set(server.IMAGE_EXTS) | set(server.VIDEO_EXTS)
    for item in files:
        if not isinstance(item, dict):
            raise ValueError("Import file entries must be objects")
        source = Path(str(item.get("path") or ""))
        if not source.is_absolute() or not source.is_file():
            raise FileNotFoundError("Import source is missing or invalid: {}".format(source))
        if source.suffix.lower() not in supported_extensions:
            raise ValueError("Unsupported import source: {}".format(source))


def run_import(config):
    _project_root, dataset_root, scene = resolve_import_location(config)
    dataset_root.mkdir(parents=True, exist_ok=True)
    server = import_backend(config["repositoryRoot"])
    validate_import_files(config, server)
    with import_transaction_lock(dataset_root, scene):
        recovery = _recover_import_artifacts(dataset_root, scene)
        if recovery["restored"]:
            print("[worker] Restored interrupted import backup: {}".format(recovery["finalPath"]))
        return _run_import_locked(config, server, dataset_root, scene)


def run_import_recovery(config):
    _project_root, dataset_root, scene = resolve_import_location(config)
    recovery = recover_import_artifacts(dataset_root, scene)
    if recovery["restored"]:
        print("[worker] Restored interrupted import backup: {}".format(recovery["finalPath"]))
    if recovery["removedArtifacts"]:
        print("[worker] Removed {} incomplete import artifact(s).".format(
            recovery["removedArtifacts"]
        ))
    print(
        "[worker-recovery] "
        + json.dumps(
            {
                "version": 1,
                "scene": scene,
                "committed": recovery["committed"],
                "restored": recovery["restored"],
                "outcome": recovery["outcome"],
                "finalPath": recovery["finalPath"],
                "removedArtifacts": recovery["removedArtifacts"],
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        flush=True,
    )
    return 0


def _scene_from_artifact_name(name):
    patterns = (
        r"^\.(.+)\.import-transaction\.json(?:\.tmp-[A-Za-z0-9]+)?$",
        r"^\.(.+)\.backup-.+$",
        r"^\.(.+)\.import-(?!lock$).+$",
    )
    for pattern in patterns:
        match = re.match(pattern, name)
        if not match:
            continue
        try:
            return validate_scene_name(match.group(1))
        except ValueError:
            return None
    return None


def run_import_project_recovery(config):
    _project_root, dataset_root = resolve_project_dataset_roots(config)
    dataset_root.mkdir(parents=True, exist_ok=True)
    scenes = set()
    for artifact in dataset_root.iterdir():
        scene = _scene_from_artifact_name(artifact.name)
        if scene:
            scenes.add(scene)

    recoveries = []
    for scene in sorted(scenes):
        recovery = recover_import_artifacts(dataset_root, scene)
        recoveries.append({"scene": scene, **recovery})

    print(
        "[worker-recovery] "
        + json.dumps(
            {
                "version": 1,
                "projectRecovery": True,
                "committed": any(item["committed"] for item in recoveries),
                "restored": any(item["restored"] for item in recoveries),
                "recoveredScenes": len(recoveries),
                "committedPaths": [
                    item["finalPath"] for item in recoveries if item["committed"]
                ],
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        flush=True,
    )
    return 0


def run_training(config):
    server = import_backend(config["repositoryRoot"])
    server.OUTPUT_DIR = Path(config["outputRoot"]).resolve()
    server.TRAIN_JOBS_DIR = Path(config["jobStore"]).resolve()
    server.TRAIN_JOBS_DIR.mkdir(parents=True, exist_ok=True)

    snapshot = server.start_training(
        config.get("scene") or "native-project",
        config["outputScene"],
        config.get("quality", "quick"),
        bool(config.get("runColmap", True)),
        bool(config.get("overwrite", False)),
        config.get("backend", "3dgs"),
        config.get("colmapOptions") or {},
        config.get("trainOptions") or {},
        False,
        dataset_path=config["datasetPath"],
    )
    job_id = snapshot["id"]
    print("[worker] Training job {} started.".format(job_id))
    print("[worker] Output: {}".format(server.OUTPUT_DIR / config["outputScene"]))

    cancel_thread = threading.Thread(
        target=watch_cancel_input,
        args=(server.cancel_training, job_id),
        name="gsw-cancel",
        daemon=True,
    )
    cancel_thread.start()

    state, error = stream_job(server.TRAIN_LOCK, server.TRAIN_JOBS, job_id)

    if state == "done":
        print("[worker] Training completed successfully.")
        return 0
    if state == "cancelled":
        print("[worker] Training cancelled.")
        return 130
    print("[worker] Training failed: {}".format(error or "unknown error"))
    return 1


def run_colmap(config):
    server = import_backend(config["repositoryRoot"])
    dataset = Path(config["datasetPath"]).resolve()
    if not dataset.is_dir():
        raise FileNotFoundError("Dataset directory is missing: {}".format(dataset))

    configured_colmap = config.get("colmapExecutable")
    if configured_colmap:
        colmap = Path(configured_colmap).resolve()
        if not colmap.is_file():
            raise FileNotFoundError("COLMAP executable is missing: {}".format(colmap))
        os.environ["COLMAP_PATH"] = str(colmap)
        os.environ["COLMAP_EXE"] = str(colmap)
    else:
        colmap = Path(server.colmap_executable())
        if not colmap.is_file():
            raise FileNotFoundError("COLMAP executable is missing: {}".format(colmap))

    options = config.get("colmapOptions") or {}
    snapshot = server.start_colmap_alignment(
        config.get("scene") or "native-project",
        options,
        dataset_path=str(dataset),
    )
    job_id = snapshot["id"]
    print("[worker] COLMAP job {} started.".format(job_id))
    print("[worker] Dataset: {}".format(dataset))
    print("[worker] Executable: {}".format(colmap))

    cancel_thread = threading.Thread(
        target=watch_cancel_input,
        args=(server.cancel_mesh_job, job_id),
        name="gsw-cancel",
        daemon=True,
    )
    cancel_thread.start()

    state, error = stream_job(server.MESH_LOCK, server.MESH_JOBS, job_id)
    if state == "done":
        if not server.dataset_has_colmap_scene(dataset):
            print("[worker] COLMAP finished without a complete sparse/0 camera model.")
            return 1
        print("[worker] COLMAP reconstruction completed successfully.")
        print("[worker] Sparse model: {}".format(dataset / "sparse" / "0"))
        return 0
    if state == "cancelled":
        print("[worker] COLMAP reconstruction cancelled.")
        return 130
    print("[worker] COLMAP reconstruction failed: {}".format(error or "unknown error"))
    return 1


def main():
    configure_stdout()
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument(
        "--task",
        choices=("training", "colmap", "import", "import-recovery", "import-project-recovery"),
    )
    args = parser.parse_args()
    try:
        config = load_configuration(args.config, args.task)
        if config["task"] == "colmap":
            return run_colmap(config)
        if config["task"] == "import":
            return run_import(config)
        if config["task"] == "import-recovery":
            return run_import_recovery(config)
        if config["task"] == "import-project-recovery":
            return run_import_project_recovery(config)
        return run_training(config)
    except Exception as exc:
        print("[worker] Fatal error: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
