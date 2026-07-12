"""Headless compute worker used by the native Qt application."""

import argparse
import json
import os
import sys
import threading
import time
from pathlib import Path


FINAL_STATES = {"done", "failed", "cancelled"}


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
        for line in lines:
            print(line)
        current_stage = (state, stage)
        if current_stage != previous_stage:
            print("[worker-status] {} / {}".format(state, stage))
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
    parser.add_argument("--task", choices=("training", "colmap"))
    args = parser.parse_args()
    try:
        config = load_configuration(args.config, args.task)
        if config["task"] == "colmap":
            return run_colmap(config)
        return run_training(config)
    except Exception as exc:
        print("[worker] Fatal error: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
