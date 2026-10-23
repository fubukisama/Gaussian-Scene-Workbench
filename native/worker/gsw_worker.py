"""Headless compute worker used by the native Qt application."""

import argparse
import contextlib
import hashlib
import json
import math
import os
import re
import shutil
import stat
import sys
import threading
import time
import uuid
from pathlib import Path

try:
    from native.worker.training_summary import normalize_training_summary
    from native.worker.training_step_profile import normalize_performance
    from native.worker.training_resource_policy import normalize_growth
except ModuleNotFoundError:
    # Direct execution uses this file's directory, not the repository root.
    from training_summary import normalize_training_summary
    from training_step_profile import normalize_performance
    from training_resource_policy import normalize_growth


FINAL_STATES = {"done", "failed", "cancelled", "paused"}
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
        "mesh": ("repositoryRoot", "modelDirectory", "outputRoot", "runName", "iteration", "mode"),
        "import": ("repositoryRoot", "projectRoot", "datasetRoot", "scene", "files"),
        "import-recovery": ("projectRoot", "datasetRoot", "scene"),
        "import-project-recovery": ("projectRoot", "datasetRoot"),
    }
    if task not in required_by_task:
        raise ValueError("Unsupported worker task: {}".format(task))
    required = required_by_task[task]
    if task == "import" and "cameraInput" in config:
        required = tuple(name for name in required if name != "files")
    missing = [name for name in required if not config.get(name)]
    if missing:
        raise ValueError("Missing worker configuration fields: " + ", ".join(missing))
    config["task"] = task
    if task == "training" and "trainingSeed" in config:
        try:
            from native.worker.training_seed import validate_training_seed
        except ModuleNotFoundError:
            from training_seed import validate_training_seed
        config["trainingSeed"] = validate_training_seed(config["trainingSeed"])
    if task == "training" and config.get("qualityPlan") is not None:
        if config.get("nativeCheckpoint") is not True or config.get("backend") not in ("3dgs", "2dgs"):
            raise ValueError("quality_requires_native_checkpoint")
        try:
            from native.worker.training_quality_plan import resolve_quality_plan
        except ModuleNotFoundError:
            from training_quality_plan import resolve_quality_plan
        # Fail before backend import, output admission, GPU allocation or process
        # creation. The legacy preset/camera/runtime options remain independent.
        config["qualityPlan"] = resolve_quality_plan(config["qualityPlan"],
                                                    backend=config["backend"])["requested"]
    if task == "training" and ("runtimeOptions" in config or "performanceOptions" in config):
        try:
            from native.worker.training_configuration import normalize_runtime_options
        except ModuleNotFoundError:
            from training_configuration import normalize_runtime_options
        # Validate here but do not fill a partial resume request with fresh
        # defaults: the verified checkpoint supplies its execution defaults.
        normalize_runtime_options(config.get("runtimeOptions"), config.get("performanceOptions"))
    config["_configurationPath"] = str(Path(path).absolute())
    return config


def import_backend(repository_root):
    try:
        from native.worker.import_preflight import load_compute_module
    except ModuleNotFoundError:
        from import_preflight import load_compute_module
    return load_compute_module(repository_root)


class QualityKernelPreflightError(ValueError):
    """Machine diagnostic for CPU admission; the trainer still verifies real ABI."""
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)[:1024]
        super().__init__(code + (": " + self.detail if self.detail else ""))


def preflight_quality_kernel(server, native_control, backend, repository_root):
    """Check the selected training Python's optional component before enqueueing.

    No probe is added to legacy/default, geometry-LR-only, 2DGS or stock MCMC
    paths. This only checks integrity and an existing GPU acceptance declaration;
    it neither imports Torch nor replaces the trainer's binary ABI verification.
    """
    plan = (native_control or {}).get("qualityPlan")
    if plan is None:
        return None
    if (not isinstance(plan, dict) or plan.get("schema") != "gsw.quality_plan_resolution/1"
            or plan.get("backend") != backend or not isinstance(plan.get("effective"), dict)):
        raise QualityKernelPreflightError("quality_plan_resolution_invalid")
    effective = plan["effective"]
    try:
        required = backend == "3dgs" and (
            effective["densification"]["strategy"] in ("abs_gs", "pixel_gs")
            or effective["filter"]["mode"] == "full_mip" or bool(effective.get("supervision")))
    except (KeyError, TypeError):
        raise QualityKernelPreflightError("quality_plan_resolution_invalid")
    if not required:
        return None

    import subprocess
    target = Path(server.training_python(backend)).resolve()
    child_environment = dict(server.training_env(backend))
    child_environment.update(CUDA_VISIBLE_DEVICES="-1", PYTHONDONTWRITEBYTECODE="1",
                             PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    # -I ignores inherited Python path/startup variables. Only the selected
    # repository is inserted explicitly; no quality extension is imported.
    code = r'''
import hashlib, json, os, re, sys
from pathlib import Path
result = {"schema": "gsw.quality_kernel_preflight/1", "success": False}
try:
    if "torch" in sys.modules:
        raise ValueError("quality_kernel_preflight_torch_loaded")
    sys.path.insert(0, str(Path(sys.argv[1]).resolve()))
    from native.worker.quality_training_runtime import quality_kernel_root
    from native.worker.quality_rasterizer_runtime import select_quality_rasterizer
    selected = select_quality_rasterizer(sys.argv[1], quality_kernel_root())
    if "torch" in sys.modules:
        raise ValueError("quality_kernel_preflight_torch_loaded")
    executable = Path(sys.executable).resolve()
    result.update(success=True, target_python=str(executable),
        target_python_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
        python_version="{}.{}.{}".format(*sys.version_info[:3]),
        root=selected["root"], build_sha256=selected["buildSha256"],
        acceptance_sha256=selected["acceptanceSha256"], cpu_only=True,
        torch_imported=False, gpu_executed=False, quality_accepted=False,
        cuda_visible_devices=os.environ.get("CUDA_VISIBLE_DEVICES"),
        isolated=bool(sys.flags.isolated), dont_write_bytecode=bool(sys.dont_write_bytecode))
except Exception as exc:
    detail = str(exc)[:1024]
    name = detail.split(":", 1)[0]
    result.update(code=name if re.fullmatch(r"quality_[a-z0-9_]+", name) else
        "quality_kernel_preflight_failed", detail=detail)
payload = json.dumps(result, ensure_ascii=True, separators=(",", ":"))
if len(payload) > 8192:
    payload = '{"schema":"gsw.quality_kernel_preflight/1","success":false,"code":"quality_kernel_preflight_output_invalid"}'
print(payload, flush=True)
sys.exit(0 if result.get("success") else 1)
'''
    output_limit = 16384
    captured = [bytearray(), bytearray()]
    oversized = threading.Event()
    try:
        process = subprocess.Popen(
            [str(target), "-I", "-B", "-c", code, str(Path(repository_root).resolve())],
            cwd=str(Path(server.GAUSSIAN_DIR).resolve()), env=child_environment,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, ValueError) as exc:
        raise QualityKernelPreflightError("quality_kernel_preflight_launch_failed", exc)

    def capture(stream, index):
        try:
            while True:
                # A short, flushed cap+1 write must not wait for a full next
                # block or EOF before the excess byte is noticed.
                block = stream.read1(4096)
                if not block:
                    break
                remaining = output_limit - len(captured[index])
                captured[index].extend(block[:max(0, remaining)])
                if len(block) > remaining:
                    oversized.set()
                    with contextlib.suppress(OSError):
                        process.kill()
        finally:
            stream.close()

    readers = [threading.Thread(target=capture, args=(stream, index), daemon=True)
               for index, stream in enumerate((process.stdout, process.stderr))]
    for reader in readers:
        reader.start()
    timed_out = False
    try:
        process.wait(timeout=45)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        process.wait()
    for reader in readers:
        reader.join(timeout=2)
    if timed_out:
        raise QualityKernelPreflightError("quality_kernel_preflight_timeout")
    if oversized.is_set() or any(reader.is_alive() for reader in readers):
        raise QualityKernelPreflightError("quality_kernel_preflight_output_invalid")
    try:
        report = json.loads(captured[0].decode("utf-8"))
        if not isinstance(report, dict) or report.get("schema") != "gsw.quality_kernel_preflight/1":
            raise ValueError()
    except (ValueError, UnicodeError):
        raise QualityKernelPreflightError("quality_kernel_preflight_output_invalid",
                                         captured[1].decode("utf-8", errors="replace"))
    if report.get("success") is not True:
        error_code = report.get("code", "")
        if not isinstance(error_code, str) or not re.fullmatch(r"quality_[a-z0-9_]+", error_code):
            error_code = "quality_kernel_preflight_failed"
        raise QualityKernelPreflightError(error_code, report.get("detail", ""))
    try:
        valid = (process.returncode == 0 and Path(report["target_python"]).resolve() == target
                 and report["target_python_sha256"] == hashlib.sha256(target.read_bytes()).hexdigest()
                 and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", report["python_version"])
                 and Path(report["root"]).is_absolute()
                 and all(re.fullmatch(r"[0-9a-f]{64}", report[key])
                         for key in ("build_sha256", "acceptance_sha256"))
                 and report["cpu_only"] is True and report["torch_imported"] is False
                 and report["gpu_executed"] is False and report["quality_accepted"] is False
                 and report["cuda_visible_devices"] == "-1"
                 and report["isolated"] is True and report["dont_write_bytecode"] is True)
    except (KeyError, TypeError, ValueError, OSError):
        valid = False
    if not valid:
        raise QualityKernelPreflightError("quality_kernel_preflight_output_invalid")
    print("[gsw-quality-kernel-preflight] " + json.dumps(report, ensure_ascii=True), flush=True)
    return report


def watch_cancel_input(cancel_callback, job_id, pause_callback=None):
    for line in sys.stdin:
        if line.strip().lower() == "pause" and pause_callback:
            try:
                pause_callback()
                print("[worker] Pause requested; saving at the next iteration boundary.", flush=True)
            except Exception as exc:
                print("[worker] Pause request failed: {}".format(exc), flush=True)
                cancel_callback(job_id)
                return
            continue
        if line.strip().lower() != "cancel":
            continue
        print("[worker] Cancellation requested by the desktop application.")
        try:
            cancel_callback(job_id)
        except Exception as exc:  # cancellation must still let the main loop finish
            print("[worker] Cancellation error: {}".format(exc))
        return


def _finite_number(value):
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def status_telemetry(snapshot):
    """Return the safe, versioned training telemetry exposed to the Qt client."""
    if not isinstance(snapshot, dict):
        return {}

    telemetry = {}
    performance = (normalize_performance(snapshot.get("trainingPerformance"))
                   if snapshot.get("kind") in (None, "training") else None)
    if performance is not None:
        telemetry["trainingPerformance"] = performance
    growth = (normalize_growth(snapshot.get("trainingGrowth"))
              if snapshot.get("kind") in (None, "training") else None)
    if growth is not None:
        telemetry["trainingGrowth"] = growth
    summary = (normalize_training_summary(snapshot.get("trainingSummary"))
               if snapshot.get("kind") in (None, "training") else None)
    if summary is not None:
        telemetry["trainingSummary"] = summary
    runtime = snapshot.get("runtimeConfiguration")
    if isinstance(runtime, dict) and snapshot.get("kind") in (None, "training"):
        try:
            encoded = json.dumps(runtime, allow_nan=False)
            if len(encoded.encode("utf-8")) <= 1024 * 1024:
                telemetry["runtimeConfiguration"] = json.loads(encoded)
        except (TypeError, ValueError):
            pass
    failure = snapshot.get("trainingFailure")
    failure_reason = ((failure.get("kind") if isinstance(failure, dict) else None)
                      or snapshot.get("pause_reason") or snapshot.get("failureReason"))
    if failure_reason in ("cuda_oom", "cuda_oom_reported", "training_exception", "manual_pause",
                          "resource_probe_unavailable", "resource_estimate_unavailable", "vram_pressure"):
        telemetry["failureReason"] = failure_reason
    delivery = snapshot.get('trainingDelivery')
    if type(delivery) is dict and delivery.get('training_status') == 'done' and delivery.get('export_status') in ('running','ready','blocked','failed','cancelled'):
        telemetry['trainingDelivery'] = {key:delivery[key] for key in
            ('training_status','export_status','reason','model_path','internal_model_path','iteration') if key in delivery}
    integer_fields = {
        "iteration": ("iteration",),
        "totalIterations": ("total_iterations", "totalIterations"),
        "gaussianCount": ("gaussian_count", "point_count", "gaussianCount"),
        "previewIteration": ("latest_iteration", "preview_iteration", "previewIteration"),
        "densityGuardIteration": ("density_guard_iteration", "densityGuardIteration"),
        "densityGuardDeferred": ("density_guard_deferred", "densityGuardDeferred"),
        "reconstructionViews": ("reconstructionViews",),
        "reconstructionInputs": ("reconstructionInputs",),
        "reconstructionPoints": ("reconstructionPoints",),
    }
    floating_fields = {
        "loss": ("loss",),
        "psnr": ("psnr",),
        "iterationMilliseconds": ("iteration_milliseconds", "iterationMilliseconds"),
        "elapsedSeconds": ("elapsed_seconds", "elapsedSeconds"),
    }

    for output_name, input_names in integer_fields.items():
        value = next((snapshot.get(name) for name in input_names if snapshot.get(name) is not None), None)
        if _finite_number(value) and float(value) >= 0:
            telemetry[output_name] = int(value)

    for output_name, input_names in floating_fields.items():
        value = next((snapshot.get(name) for name in input_names if snapshot.get(name) is not None), None)
        if _finite_number(value) and float(value) >= 0:
            telemetry[output_name] = float(value)

    preview_path = (
        snapshot.get("partial_point_cloud_path")
        or snapshot.get("point_cloud_path")
        or snapshot.get("previewPath")
    )
    if isinstance(preview_path, str) and preview_path.strip():
        telemetry["previewPath"] = preview_path
    preview_kind = snapshot.get("preview_kind") or snapshot.get("previewKind")
    if isinstance(preview_kind, str) and preview_kind.strip():
        telemetry["previewKind"] = preview_kind.strip()
    quality = snapshot.get("reconstruction_quality")
    if isinstance(quality, dict):
        for output, source in (("reconstructionViews", "registeredImages"), ("reconstructionInputs", "inputImages"), ("reconstructionPoints", "validPoints")):
            value = quality.get(source)
            if _finite_number(value) and value >= 0:
                telemetry[output] = int(value)
    phase = snapshot.get("reconstruction_quality_phase") or snapshot.get("reconstructionQuality")
    if phase in ("accepted", "partial", "repairing", "rejected"):
        telemetry["reconstructionQuality"] = phase
    issue = snapshot.get("generation_issue") or snapshot.get("generationIssue")
    if issue in ("reconstruction_quality", "source_frames_missing", "training_renderer_resume",
                 "training_cuda_oom", "training_cuda_oom_reported", "training_exception", "training_resource_pause"):
        telemetry["generationIssue"] = issue
    return telemetry


def emit_status(state, stage, progress_percent=None, snapshot=None):
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
    payload.update(status_telemetry(snapshot))
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


def stream_job(lock, jobs, job_id, observer=None, defer_success=False):
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
            telemetry = status_telemetry(job)
            observation = dict(job) if observer else None
        if observer is not None:
            observer(observation)
        for line in lines:
            print(line)
        current_stage = (state, stage, progress_percent, tuple(sorted(telemetry.items())))
        if current_stage != previous_stage:
            if state == 'done' and defer_success:
                emit_status('running','coordinate_export',0,dict(telemetry,
                    trainingDelivery={'training_status':'done','export_status':'running'}))
            else:
                emit_status(state, stage, progress_percent, telemetry)
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


def managed_storage_name(display_name):
    """Map display data to an ASCII-only directory, matching ManagedName.h."""
    if not isinstance(display_name, str) or not display_name.strip():
        raise ValueError("Display name must not be blank")
    try:
        legacy = validate_scene_name(display_name)
    except ValueError:
        legacy = None
    if legacy is not None and not legacy.lower().startswith("gsw-name-"):
        return legacy
    return "gsw-name-" + hashlib.sha256(display_name.encode("utf-8")).hexdigest()


def validated_display_name(config, display_key, storage_key):
    storage_name = validate_scene_name(config.get(storage_key))
    if display_key not in config:
        return storage_name  # Previous native job configurations remain readable.
    display_name = config[display_key]
    if managed_storage_name(display_name) != storage_name:
        raise ValueError("Display name does not match its storage identifier")
    return display_name


def write_name_metadata(directory, display_name, storage_name):
    write_import_journal(Path(directory) / ".gsw-name.json", {
        "version": 1, "displayName": display_name, "storageName": storage_name,
    })


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
    validated_display_name(config, "displayName", "scene")
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
    from native.backend.atomic_file import atomic_sibling
    with atomic_sibling(path, mode='w', encoding='utf-8') as (temporary, stream):
        json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
        stream.close()
        # Antivirus/indexing can briefly deny Windows atomic replacement even
        # after our fsynced stream is closed. Retry only this publication step;
        # never rerun the import or remove the existing journal as a fallback.
        for attempt in range(5):
            if _is_reparse_point(path):
                raise RuntimeError("Import journal must not be a link or reparse point: {}".format(path))
            try:
                os.replace(temporary, path)
                break
            except OSError as error:
                if getattr(error, "winerror", None) not in (5, 32) or attempt == 4:
                    raise
                time.sleep(.05)


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
        "files": config.get("files", []),
        "import_job_id": job_id,
    }

    def invoke_import():
        try:
            outcome["result"] = server.save_path_dataset(request)
        except Exception as exc:  # surfaced after status polling completes
            outcome["error"] = exc

    print("[worker] Importing {} media file(s) into staging.".format(len(config.get("files", []))))
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

        # Publish the name with the data, so rollback, migration and recovery
        # cannot leave a successfully imported dataset without its display name.
        write_name_metadata(staging_path, validated_display_name(config, "displayName", "scene"), scene)

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
        if result.get("cameraDatasetInfo"):
            print("[worker-camera-dataset] " + json.dumps(
                {"version": 1, "cameraDatasetInfo": dict(result["cameraDatasetInfo"],
                 metadataPath=str(final_path / "camera_dataset.json"))},
                ensure_ascii=False, separators=(",", ":")), flush=True)
        print("[worker] Media import completed successfully.")
        print("[worker] Dataset: {}".format(final_path))
        print("[worker] Images: {}".format(result.get("image_count", 0)))
        emit_status("done", "done", 100)
        return 0
    except Exception as exc:
        if (last_snapshot or {}).get("status") == "cancelled" or getattr(exc, "code", "") == "camera_import_cancelled":
            emit_status("cancelled", "cancelled", import_progress_percent(last_snapshot or {}))
            print("[worker] Fixed-camera import cancelled; original dataset remains unchanged.")
            return 130
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
    if "cameraInput" in config:
        from native.worker.camera_import import validate_camera_input
        validate_camera_input(config)
        return
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
    if "cameraInput" in config:
        # Explicit camera input is handled before media scanning or compute
        # backend loading. Incidental sparse directories never change policy.
        root = Path(config["repositoryRoot"]).resolve()
        if not (root / "native/backend/camera_dataset.py").is_file():
            raise FileNotFoundError("Selected native camera-dataset backend is unavailable")
        sys.path.insert(0, str(root))
        from native.worker.camera_import import CameraImportService
        server = CameraImportService(config)
    else:
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


def publish_training_delivery(output_directory, iteration=None, cancelled=None, progress=None):
    """Deliver coordinates separately from optimizer success; safe to retry."""
    from native.backend.camera_scale_export import deliver_training_result, inspect_coordinate_metadata, CameraScaleExportError
    from native.worker.training_checkpoint import atomic_json, file_digest
    output = _lexical_absolute(output_directory)
    candidates = []
    point_root = output / 'point_cloud'
    if iteration is not None and (type(iteration) is not int or iteration < 1):
        raise ValueError('training_delivery_iteration_invalid')
    if point_root.is_dir() and not _is_reparse_point(point_root):
        for directory in point_root.iterdir():
            match = re.fullmatch(r'iteration_(\d+)', directory.name)
            if (match and directory.is_dir() and not _is_reparse_point(directory) and
                    (iteration is None or int(match.group(1)) == iteration)):
                model = directory / 'point_cloud.ply'
                if model.is_file() and not _is_reparse_point(model):
                    candidates.append((int(match.group(1)), model))
    if not candidates:
        return {'schema':'gsw.model_delivery/1','training_status':'done','export_status':'failed',
                'reason':'training_delivery_model_missing','model_path':None}
    actual_iteration, model = max(candidates)
    raw_model = model
    destination = output / '.gsw-deliveries' / ('i' + str(actual_iteration))
    binding_files = []
    fused_selected = False

    def fail(code, detail=''):
        raise CameraScaleExportError(code, detail)

    def checked(path):
        path = _lexical_absolute(path)
        for part in (path,) + tuple(path.parents):
            if _is_reparse_point(part):
                fail('quality_delivery_link_refused', str(part))
        _ensure_within_root(path, output)
        return path

    def relative_reference(value):
        if not isinstance(value, str) or not value or ':' in value or '\x00' in value or value.startswith(('/', '\\')):
            fail('quality_delivery_fused_path_invalid')
        parts = re.split(r'[/\\]', value)
        if any(part in ('', '.', '..') for part in parts):
            fail('quality_delivery_fused_path_invalid')
        target = checked(raw_model.parent.joinpath(*parts))
        _ensure_within_root(target, raw_model.parent)
        return target

    def read_quality(path):
        checked(path)
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            fail('quality_delivery_sidecar_invalid')
        def pairs(items):
            result = {}
            for key, value in items:
                if key in result:
                    fail('quality_delivery_sidecar_invalid')
                result[key] = value
            return result
        with path.open(encoding='utf-8') as stream:
            result = json.load(stream, object_pairs_hook=pairs,
                parse_constant=lambda unused: fail('quality_delivery_sidecar_invalid'))
        if type(result) is not dict:
            fail('quality_delivery_sidecar_invalid')
        return result

    try:
        checked(model)
        quality_path = model.with_suffix('.quality.json')
        checked(quality_path)
        if _path_exists_no_follow(quality_path):
            quality_hash = file_digest(quality_path)
            quality = read_quality(quality_path)
            hash_string = lambda value: isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None
            filter_2d = quality.get('filter_2d')
            if (quality.get('schema') != 'gsw.quality_model/1' or quality.get('filter') != 'full_mip' or
                    quality.get('representation') != 'raw' or quality.get('filter_3d_applied') is not False or
                    any(not hash_string(quality.get(key)) for key in ('raw_source_sha256', 'fused_sha256', 'plan_hash')) or
                    type(filter_2d) is not dict or type(filter_2d.get('variance')) not in (int, float) or
                    not math.isfinite(filter_2d['variance']) or abs(filter_2d['variance'] - .3) > 1e-12 or
                    filter_2d.get('opacity_compensation') is not True):
                fail('quality_delivery_sidecar_invalid')
            if file_digest(raw_model) != quality['raw_source_sha256']:
                fail('quality_delivery_raw_hash_mismatch')
            fused = relative_reference(quality.get('fused_file'))
            if not fused.is_file() or fused.suffix.lower() != '.ply' or os.path.samefile(raw_model, fused):
                fail('quality_delivery_fused_path_invalid')
            if file_digest(fused) != quality['fused_sha256']:
                fail('quality_delivery_fused_hash_mismatch')
            info = inspect_coordinate_metadata(fused)
            fused_quality = info.get('quality')
            if (fused_quality is None or fused_quality.get('representation') != 'fused' or
                    any(fused_quality.get(key) != quality.get(key) for key in
                        ('raw_source_sha256', 'fused_sha256', 'plan_hash', 'kernel_build_sha256', 'filter_2d'))):
                fail('quality_delivery_fused_binding_invalid')
            binding_files.extend(((raw_model, quality['raw_source_sha256']), (quality_path, quality_hash),
                (fused, quality['fused_sha256']), (Path(info['quality_path']), info['quality_sha256'])))
            if 'raw_source_file' in quality:
                snapshot = relative_reference(quality['raw_source_file'])
                if not snapshot.is_file() or file_digest(snapshot) != quality['raw_source_sha256']:
                    fail('quality_delivery_raw_hash_mismatch')
                binding_files.append((snapshot, quality['raw_source_sha256']))
            if 'filter_widths' in quality:
                widths = quality['filter_widths']
                if (type(widths) is not dict or set(widths) not in ({'path', 'sha256'}, {'path', 'sha256', 'frame'}) or
                        'frame' in widths and widths['frame'] != 'training' or not hash_string(widths.get('sha256'))):
                    fail('quality_delivery_sidecar_invalid')
                width_path = relative_reference(widths['path'])
                source_width_hash = fused_quality.get('filter_widths', {}).get('sha256',
                    fused_quality.get('source_filter_widths_sha256'))
                if not width_path.is_file() or file_digest(width_path) != widths['sha256'] or source_width_hash != widths['sha256']:
                    fail('quality_delivery_fused_binding_invalid')
                binding_files.append((width_path, widths['sha256']))
            model, fused_selected = fused, True
        checked(destination)
        destination.mkdir(parents=True, exist_ok=True)
        result = deliver_training_result(model, destination, cancelled=cancelled, progress=progress)
        if result.get('export_status') == 'ready':
            for path, expected_hash in binding_files:
                checked(path)
                if not path.is_file() or file_digest(path) != expected_hash:
                    fail('quality_delivery_source_changed')
    except Exception as exc:
        # Optimization already completed. An export/plugin allocation or other
        # ordinary exception is an independent failed delivery, not failed
        # training. Preserve KeyboardInterrupt/SystemExit and the raw state.
        result = {'schema':'gsw.model_delivery/1','training_status':'done','export_status':'failed',
                  'reason':getattr(exc,'code','training_delivery_failed'),'detail':str(exc),
                  'model_path':str(raw_model)}
    result.update(iteration=actual_iteration, internal_model_path=str(raw_model))
    result['internal_model_relative_path'] = str(raw_model.relative_to(output)).replace('\\','/')
    if fused_selected:
        result.update(fused_internal_model_path=str(model),
            fused_internal_model_relative_path=str(model.relative_to(output)).replace('\\','/'))
    for key in ('model_path','sidecar_path','camera_path','quality_sidecar_path'):
        if result.get(key):
            result[key.replace('_path','_relative_path')] = str(Path(result[key]).relative_to(output)).replace('\\','/')
    receipt = output / 'model-delivery.json'
    _ensure_within_root(receipt, output)
    atomic_json(receipt, result)
    return result


def run_training(config, resume=False):
    # Direct script/installed-worker execution must resolve its own packaged
    # native modules before claiming an output, independent of caller cwd or
    # a GUI-injected PYTHONPATH. Compute is still not loaded before the gate.
    own_root = str(Path(__file__).resolve().parents[2])
    if own_root not in sys.path:
        sys.path.insert(0,own_root)
    if config.get("qualityPlan") is not None and (
            config.get("nativeCheckpoint") is not True or config.get("backend") not in ("3dgs", "2dgs")):
        raise ValueError("quality_requires_native_checkpoint")
    # Claim output before compute can enqueue/start an optimizer thread, not
    # only when a later GUI observer lease is registered. Closing an observer
    # does not release this worker-owned gate.
    if config.get('nativeCheckpoint') and config.get('backend', '3dgs') in ('3dgs', '2dgs'):
        from native.worker.training_job_connection import OutputSessionGate, ensure_output_not_active
        root = _lexical_absolute(Path(config['outputRoot']))
        output = _lexical_absolute(root / config['outputScene'])
        _ensure_within_root(output, root, direct_child=True)
        session = uuid.uuid4().hex
        with OutputSessionGate(output, session) as gate:
            ensure_output_not_active(config['jobStore'], output)
            return _run_training_owned(config, resume, gate, session)
    return _run_training_owned(config, resume)


def _run_training_owned(config, resume=False, output_gate=None, session=None):
    display_name = validated_display_name(config, "outputDisplayName", "outputScene")
    server = import_backend(config["repositoryRoot"])
    server.OUTPUT_DIR = Path(config["outputRoot"]).resolve()
    server.TRAIN_JOBS_DIR = Path(config["jobStore"]).resolve()
    server.TRAIN_JOBS_DIR.mkdir(parents=True, exist_ok=True)

    native_options = {}
    if "trainingSeed" in config:
        from native.worker.training_seed import validate_training_seed
        native_options["training_seed"] = validate_training_seed(config["trainingSeed"])
    pause_callback = None
    request_path = None
    sys.path.insert(0, str(Path(config["repositoryRoot"]).resolve()))
    from native.worker.generation_capabilities import supports_checkpoint
    if config.get("nativeCheckpoint") and supports_checkpoint(config.get("backend")):
        from native.worker.training_checkpoint import atomic_json, read_manifest
        output = _lexical_absolute(server.OUTPUT_DIR / config["outputScene"])
        _ensure_within_root(output, server.OUTPUT_DIR, direct_child=True)
        session = session or uuid.uuid4().hex
        request_path = server.TRAIN_JOBS_DIR / ("pause-" + session + ".json")
        native_options["native_control"] = {
            "output": str(output), "request": str(request_path), "session": session, "resume": resume,
            "backend": config["backend"]}
        pause_callback = lambda: atomic_json(request_path, {"session": session})
        manifest = None
        if resume:
            manifest, _ = read_manifest(output)
            if manifest.get("backend", "3dgs") != config["backend"]:
                raise ValueError("Checkpoint backend does not match the selected training method")
            native_options["resume_checkpoint_iteration"] = manifest["iteration"]
        from native.worker.training_checkpoint import quality_plan_for_control
        quality_plan = quality_plan_for_control(config.get("qualityPlan"), backend=config["backend"],
            checkpoint_plan=(manifest or {}).get("quality_plan"), resume=resume)
        if quality_plan is not None:
            native_options["native_control"]["qualityPlan"] = quality_plan
        from native.worker.training_runtime import runtime_configuration
        runtime_request = {"resume": resume, "backend": config["backend"]}
        if "runtimeOptions" in config:
            runtime_request["runtimeOptions"] = config["runtimeOptions"]
        if "performanceOptions" in config:
            runtime_request["performance"] = config["performanceOptions"]
        runtime_report = runtime_configuration(
            runtime_request, checkpoint_runtime=(manifest or {}).get("runtime_options"),
            checkpoint_performance=(manifest or {}).get("performance"))
        native_options["native_control"]["runtimeOptions"] = runtime_report["requested"]
        native_options["native_control"]["runtimeConfigurationReport"] = runtime_report
    elif resume:
        raise ValueError("This task has no native training checkpoint support")
    elif "runtimeOptions" in config or "performanceOptions" in config:
        raise ValueError("runtime_options_require_native_checkpoint_backend")

    # Keep policy explicit across queueing, retry and complete-state resume.
    # A nearby calibration file must never silently select a different policy.
    if config.get("posePolicy") is not None:
        policy = config["posePolicy"]
        if policy not in ("locked", "solve"):
            raise ValueError("camera_pose_policy_invalid")
        if policy == "locked" and config.get("runColmap", False):
            raise ValueError("camera_locked_colmap_conflict")
        native_options.update(
            pose_policy=policy,
            camera_dataset_path=config.get("cameraDatasetPath"),
            camera_dataset_identity=config.get("cameraDatasetIdentity"),
            resolution_policy=config.get("resolutionPolicy", "preserve" if policy == "locked" else "manual"),
        )

    preflight_quality_kernel(server, native_options.get("native_control"),
                             config.get("backend", "3dgs"), config["repositoryRoot"])
    snapshot = server.start_training(
        config.get("scene") or "native-project",
        config["outputScene"],
        config.get("quality", "quick"),
        False if resume else bool(config.get("runColmap", True)),
        False if resume else bool(config.get("overwrite", False)),
        config.get("backend", "3dgs"),
        config.get("colmapOptions") or {},
        config.get("trainOptions") or {},
        resume,
        dataset_path=config["datasetPath"],
        **native_options
    )
    job_id = snapshot["id"]
    print("[worker] Training job {} started.".format(job_id))
    print("[worker] Output: {}".format(server.OUTPUT_DIR / config["outputScene"]))

    cancel_thread = threading.Thread(
        target=watch_cancel_input,
        args=(server.cancel_training, job_id, pause_callback),
        name="gsw-cancel",
        daemon=True,
    )
    cancel_thread.start()

    connection = None
    if native_options.get("native_control"):
        try:
            from native.worker.training_job_connection import WorkerConnection
            connection = WorkerConnection(server.TRAIN_JOBS_DIR, job_id,
                native_options["native_control"], config.get("_configurationPath"),
                output_gate=output_gate)
        except (OSError, ValueError) as exc:
            print("[worker] Existing-job connection unavailable: {}".format(exc), flush=True)

    def observe_connection(snapshot):
        if connection is None:
            return
        try:
            connection.heartbeat()
            command = connection.take_command()
            if command == "pause" and pause_callback:
                pause_callback()
            elif command == "stop":
                server.cancel_training(job_id)
        except (OSError, ValueError) as exc:
            # Losing a monitor or its filesystem must not cancel optimization.
            print("[worker] Existing-job connection warning: {}".format(exc), flush=True)

    state, error = stream_job(server.TRAIN_LOCK, server.TRAIN_JOBS, job_id,
                              observe_connection if connection is not None else None,
                              defer_success=bool(native_options.get('native_control')))
    if state == 'done' and native_options.get('native_control'):
        delivery_stopped = [False]
        def delivery_cancelled():
            with server.TRAIN_LOCK:
                if server.TRAIN_JOBS[job_id].get('cancel_requested'):
                    delivery_stopped[0] = True
            if connection is not None:
                try:
                    connection.heartbeat()
                    if connection.take_command() == 'stop':
                        delivery_stopped[0] = True
                except (OSError, ValueError) as exc:
                    print('[worker] Delivery observer warning: {}'.format(exc),flush=True)
            return delivery_stopped[0]
        with server.TRAIN_LOCK:
            record = server.TRAIN_JOBS[job_id]
            record.update(status='running',stage='coordinate_export',training_status='done',
                trainingDelivery={'training_status':'done','export_status':'running'})
            server.persist_train_job(record)
        emit_status('running','coordinate_export',0,snapshot=record)
        try:
            delivery = publish_training_delivery(native_options['native_control']['output'],
                cancelled=delivery_cancelled,
                progress=lambda value: emit_status('running','coordinate_export',value,
                    snapshot={'trainingDelivery':{'training_status':'done','export_status':'running'}}))
        except Exception as exc:
            delivery = {'schema':'gsw.model_delivery/1','training_status':'done','export_status':'failed',
                        'reason':getattr(exc,'code','training_delivery_failed'),'detail':str(exc)}
        with server.TRAIN_LOCK:
            record = server.TRAIN_JOBS[job_id]
            summary={key:value for key,value in delivery.items() if key not in ('report',)}
            manifest=Path(native_options['native_control']['output'])/'model-delivery.json'
            if manifest.is_file():
                summary.update(manifest_path=str(manifest),manifest_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest())
            record.update(status='done',stage='done',trainingDelivery=summary,training_status='done')
            server.persist_train_job(record)
        print('[gsw-model-delivery] '+json.dumps(summary,ensure_ascii=True,allow_nan=False),flush=True)
        emit_status('done','done',100,snapshot=record)
    if connection is not None:
        try:
            connection.close(state)
        except OSError as exc:
            print("[worker] Existing-job final connection warning: {}".format(exc), flush=True)
    if request_path and request_path.exists():
        request_path.unlink()

    output_path = _lexical_absolute(server.OUTPUT_DIR / config["outputScene"])
    _ensure_within_root(output_path, server.OUTPUT_DIR, direct_child=True)
    if output_path.is_dir() and not _is_reparse_point(output_path):
        write_name_metadata(output_path, display_name, config["outputScene"])

    if state == "done":
        print("[worker] Training completed successfully.")
        return 0
    if state == "paused":
        print("[worker] Training paused; optimizer state preserved.")
        return 75
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
    parser.add_argument("--resume-training", action="store_true")
    parser.add_argument(
        "--task",
        choices=("training", "colmap", "mesh", "import", "import-recovery", "import-project-recovery"),
    )
    args = parser.parse_args()
    try:
        config = load_configuration(args.config, args.task)
        if config["task"] == "mesh":
            sys.path.insert(0, str(Path(config["repositoryRoot"]).resolve()))
            from native.worker.mesh_generation import run
            return run(config, import_backend(config["repositoryRoot"]), emit_status)
        if config["task"] == "colmap":
            return run_colmap(config)
        if config["task"] == "import":
            return run_import(config)
        if config["task"] == "import-recovery":
            return run_import_recovery(config)
        if config["task"] == "import-project-recovery":
            return run_import_project_recovery(config)
        return run_training(config, resume=args.resume_training)
    except Exception as exc:
        print("[worker] Fatal error: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
