"""Transactional staging adapter for explicitly selected fixed-camera inputs.

Only referenced images/calibration/initialization are copied, never flattened,
hard-linked, resampled, solved or inferred from incidental directory contents.
Publication/rollback remain owned by gsw_worker's existing import transaction.
"""

import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import threading


class _InputCancellation:
    """Nonblocking control-pipe polling; no stdin thread delays worker exit."""

    def __init__(self):
        self.cancelled = False
        self.descriptor = None
        self.pending = b""
        try:
            descriptor = sys.stdin.fileno()
            if os.name == "nt":
                import ctypes
                from ctypes import wintypes
                import msvcrt
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.GetFileType.argtypes = [wintypes.HANDLE]
                kernel.GetFileType.restype = wintypes.DWORD
                kernel.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                                wintypes.LPDWORD, wintypes.LPDWORD, wintypes.LPDWORD]
                kernel.PeekNamedPipe.restype = wintypes.BOOL
                handle = wintypes.HANDLE(msvcrt.get_osfhandle(descriptor))
                if kernel.GetFileType(handle) != 3:
                    return

                def available():
                    count = wintypes.DWORD()
                    if not kernel.PeekNamedPipe(handle, None, 0, None, ctypes.byref(count), None):
                        raise ctypes.WinError(ctypes.get_last_error())
                    return count.value
            else:
                import select

                def available():
                    return 256 if select.select([descriptor], [], [], 0)[0] else 0
            self.available = available
            self.descriptor = descriptor
        except (OSError, ValueError, AttributeError):
            pass

    def __call__(self):
        if self.cancelled:
            return True
        if self.descriptor is not None:
            try:
                for _ in range(16):
                    available = self.available()
                    if not available:
                        break
                    chunk = os.read(self.descriptor, min(256, available))
                    if not chunk:
                        self.descriptor = None
                        break
                    self.pending += chunk
                    while b"\n" in self.pending:
                        line, self.pending = self.pending.split(b"\n", 1)
                        if line.strip().lower() == b"cancel":
                            self.cancelled = True
                            return True
                    if len(self.pending) > 4096:
                        self.descriptor = None
                        break
            except (OSError, ValueError):
                self.descriptor = None
        return self.cancelled


def _reparse(path):
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(metadata.st_mode) or bool(getattr(metadata, "st_file_attributes", 0)
                                               & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def _reference(root, relative):
    if not isinstance(relative, str) or "\\" in relative or ":" in relative or "\x00" in relative:
        raise ValueError("Invalid fixed-camera relative path")
    path = PurePosixPath(relative)
    if not relative or path.is_absolute() or ".." in path.parts:
        raise ValueError("Fixed-camera path leaves its dataset")
    target = root.joinpath(*path.parts)
    try:
        target.resolve().relative_to(root.resolve())
    except ValueError as error:
        raise ValueError("Fixed-camera path leaves its dataset") from error
    return target


def _output(root, relative):
    path = _reference(root, relative)
    current = root
    for part in path.relative_to(root).parts:
        current = current / part
        if _reparse(current):
            raise ValueError("Fixed-camera managed output cannot traverse a reparse point")
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def _copy_verified(source, destination, expected_digest, cancelled):
    from native.backend.camera_dataset import CameraDatasetError
    if cancelled():
        raise CameraDatasetError("camera_import_cancelled")
    digest = hashlib.sha256()
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        for block in iter(lambda: incoming.read(1024 * 1024), b""):
            if cancelled():
                raise CameraDatasetError("camera_import_cancelled")
            digest.update(block)
            outgoing.write(block)
        outgoing.flush()
        os.fsync(outgoing.fileno())
    if digest.hexdigest() != expected_digest:
        raise ValueError("Source bytes changed during fixed-camera import: " + str(source))
    if os.path.samefile(source, destination):
        raise ValueError("Fixed-camera import requires an independent image copy")
    return destination.stat().st_size


def validate_camera_input(config):
    value = config.get("cameraInput")
    if not isinstance(value, dict) or not value:
        raise ValueError("cameraInput must be a nonempty object")
    if value.get("posePolicy", "locked") != "locked":
        raise ValueError("Fixed-camera import requires posePolicy=locked; no SfM fallback is available")
    if value.get("adapter", "auto") not in ("auto", "blender_transforms", "colmap", "generic_json", "research_json", "trajectory_dataset"):
        raise ValueError("Unsupported fixed-camera input adapter")
    source = Path(str(value.get("sourcePath") or ""))
    if not source.is_absolute() or not source.exists() or not (source.is_file() or source.is_dir()):
        raise ValueError("Fixed-camera sourcePath must be an existing absolute file or directory")
    final = (Path(config["datasetRoot"]) / config["scene"]).resolve()
    try:
        source.resolve().relative_to(final)
    except ValueError:
        pass
    else:
        raise ValueError("An import must not overwrite the selected fixed-camera source dataset")
    for name in ("scale", "normalization", "initialization"):
        if name in value and not isinstance(value[name], dict):
            raise ValueError("cameraInput.{} must be an object".format(name))
    return value


def camera_dataset_info(dataset, metadata_path="camera_dataset.json"):
    """Presentation-only summary; no preview or metric-accuracy claim."""
    from native.backend.camera_admission import camera_dataset_summary
    return camera_dataset_summary(dataset, metadata_path)


class CameraImportService:
    """Fits the existing import staging/status seam without a compute backend."""

    def __init__(self, config):
        self.config = config
        self.camera_input = validate_camera_input(config)
        self.DATASETS_DIR = Path(config["datasetRoot"])
        self._snapshot = None
        self._lock = threading.Lock()
        self._job_id = None
        self._cancel = _InputCancellation()

    def _update(self, **values):
        with self._lock:
            self._snapshot.update(values)

    def import_job_snapshot(self, job_id):
        with self._lock:
            if job_id != self._job_id or self._snapshot is None:
                raise ValueError("Import job not found")
            return dict(self._snapshot)

    def save_path_dataset(self, request):
        from native.backend import camera_dataset
        self._job_id = request["import_job_id"]
        with self._lock:
            self._snapshot = {"status": "running", "stage": "preparing", "total_files": 0,
                              "processed_files": 0, "total_videos": 0, "saved_images": 0,
                              "image_count": 0, "error": ""}
        try:
            selected = self.camera_input
            source = Path(selected["sourcePath"]).resolve()
            adapter = selected.get("adapter", "auto")
            options = {name: copy.deepcopy(selected[name]) for name in ("scale", "normalization", "initialization")
                       if name in selected}
            options["pose_policy"] = "locked"
            dataset = camera_dataset.load_camera_dataset(source, adapter=adapter, options=options,
                                                         cancelled=self._cancel)
            source_root = camera_dataset.camera_dataset_root(source, adapter=adapter)
            destination = self.DATASETS_DIR / request["scene"]
            if destination.exists() or _reparse(destination):
                raise ValueError("Fixed-camera import staging must be a new owned directory")
            if destination.parent.resolve() != self.DATASETS_DIR.resolve():
                raise ValueError("Fixed-camera import staging leaves the managed dataset root")
            destination.mkdir()
            records, copied = [], set()
            self._update(stage="copying_images", total_files=len(dataset["views"]))
            for view in dataset["views"]:
                relative = view["image_path"]
                source_image = _reference(source_root, relative)
                target = _output(destination, relative)
                byte_size = _copy_verified(source_image, target, view["sha256"], self._cancel)
                copied.add(relative)
                records.append({"kind": "image", "viewId": view["view_id"], "split": view["split"],
                                "relativePath": relative, "trainingPath": relative,
                                "sourcePath": str(source_image), "sha256": view["sha256"],
                                "byteSize": byte_size, "archiveMode": "copy", "sourceUnchanged": True})
                self._update(saved_images=len(records), image_count=len(records), processed_files=len(records),
                             current_file=relative)
            initialization = dataset["initialization"]
            if initialization["kind"] != "random":
                relative = initialization["path"]
                if relative in copied:
                    raise ValueError("Initialization path conflicts with an image path")
                _copy_verified(_reference(source_root, relative), _output(destination, relative),
                               initialization["sha256"], self._cancel)
                copied.add(relative)
            # Reimporting a fitted version requires its replayable ancestry,
            # not merely its derived status strings or a cached summary.
            for record in camera_dataset.scale_derivation_archive_records(dataset):
                relative = record["path"]
                if relative in copied:
                    raise ValueError("Scale ancestry path conflicts with an image or initialization")
                _copy_verified(_reference(source_root, relative), _output(destination, relative),
                               record["sha256"], self._cancel)
                copied.add(relative)
            # Archive calibration bytes separately from the canonical manifest.
            # A generic source may itself be named camera_dataset.json.
            source_files = copy.deepcopy(dataset["source"].get("files", []))
            selected_relative = source.relative_to(source_root).as_posix() if source.is_file() else None
            if source.is_file() and not any(record.get("path") == selected_relative for record in source_files):
                source_files.append({"path": selected_relative,
                                     "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
            calibration_records = []
            for record in source_files:
                relative = record["path"]
                source_file = _reference(source_root, relative)
                archived = "source/camera_input/" + relative
                _copy_verified(source_file, _output(destination, archived), record["sha256"], self._cancel)
                calibration_records.append(dict(record, archivePath=archived))
            # Re-read the copied images/point cloud using the same protocol and
            # verify exact semantic identity before publishing the transaction.
            verified = camera_dataset.validate_camera_dataset(dataset, destination, cancelled=self._cancel)
            camera_dataset.write_camera_dataset(verified, destination / camera_dataset.MANIFEST_NAME)
            info = camera_dataset_info(verified)
            camera_dataset.write_camera_dataset(info, destination / "camera_dataset_info.json")
            manifest = {"version": 2, "kind": "fixed_camera_dataset", "files": records,
                        "cameraSources": calibration_records, "dataIdentity": verified["data_identity"],
                        "posePolicy": "locked", "normalizationAppliedToImages": False,
                        "sourcePath": str(source), "sourceRoot": str(source_root), "cameraDatasetInfo": info}
            camera_dataset.write_camera_dataset(manifest, destination / "source/metadata_manifest.json")
            if self._cancel():
                raise camera_dataset.CameraDatasetError("camera_import_cancelled")
            result = {"image_count": len(dataset["views"]), "cameraDatasetInfo": info}
            self._update(status="done", stage="done", current_file="", **result)
            return result
        except Exception as error:
            cancelled = isinstance(error, camera_dataset.CameraDatasetError) and error.code == "camera_import_cancelled"
            self._update(status="cancelled" if cancelled else "failed", stage="cancelled" if cancelled else "failed",
                         error=str(error))
            raise
