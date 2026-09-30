"""Native staged mesh jobs over the existing, versioned backend commands.

Each invocation owns a new directory. Training inputs and previous meshes are
never used as writable work directories. Completed geometry is retained when a
later texture stage fails; this is an artifact, not an optimizer checkpoint.
"""

import ast
import math
import sys
import threading
import time
from pathlib import Path

from native.worker.training_checkpoint import atomic_json


MODES = {"bounded": "2dgs", "unbounded": "2dgs", "sugar": "3dgs", "gs2mesh": "3dgs"}


def _within(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def validate_mesh(path, cancel=None):
    """Check complete triangle payload, finite positions and index ranges."""
    import numpy as np
    from plyfile import PlyData

    import inspect
    arguments = {"mmap": "r"}
    if "known_list_len" in inspect.signature(PlyData.read).parameters:
        arguments["known_list_len"] = {"face": {"vertex_indices": 3}}
    ply = PlyData.read(str(path), **arguments)
    if "vertex" not in ply or "face" not in ply:
        raise ValueError("Mesh output must contain vertices and triangle faces")
    vertices, faces = ply["vertex"].data, ply["face"].data
    if not len(vertices) or not len(faces):
        raise ValueError("Mesh output is empty")
    if not all(name in vertices.dtype.names for name in ("x", "y", "z")):
        raise ValueError("Mesh output has no XYZ coordinates")
    if "vertex_indices" not in faces.dtype.names:
        raise ValueError("Mesh output has no face indices")
    for start in range(0, len(vertices), 65536):
        if cancel and cancel.is_set():
            raise InterruptedError("Mesh validation cancelled")
        chunk = vertices[start:start + 65536]
        if not all(np.isfinite(chunk[name]).all() for name in ("x", "y", "z")):
            raise ValueError("Mesh output contains non-finite positions")
    for start in range(0, len(faces), 65536):
        if cancel and cancel.is_set():
            raise InterruptedError("Mesh validation cancelled")
        indices = np.asarray(list(faces["vertex_indices"][start:start + 65536]))
        if indices.ndim != 2 or indices.shape[1] != 3 or indices.min() < 0 or indices.max() >= len(vertices):
            raise ValueError("Mesh output contains invalid triangle indices")
    return {"vertices": len(vertices), "faces": len(faces)}


def _copy_input(source, target, cancel):
    before = source.stat()
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as reader, target.open("xb") as writer:
        while True:
            if cancel.is_set():
                raise InterruptedError("Input preparation cancelled")
            block = reader.read(8 * 1024 * 1024)
            if not block:
                break
            writer.write(block)
    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError("Training input changed while preparing mesh job")


def _validate_configuration(path):
    # Upstream renderers evaluate cfg_args as Namespace(...). A browsed model
    # directory is data, not permission to execute arbitrary Python expressions.
    node = ast.parse(path.read_text(encoding="utf-8-sig"), mode="eval").body
    if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name) or node.func.id != "Namespace" or node.args:
        raise ValueError("Training cfg_args must be a literal Namespace configuration")
    names = set()
    for keyword in node.keywords:
        if not keyword.arg or keyword.arg in names:
            raise ValueError("Training cfg_args contains invalid or duplicate options")
        names.add(keyword.arg)
        ast.literal_eval(keyword.value)


def run(config, server, emit):
    source = Path(config["modelDirectory"]).resolve()
    output_root = Path(config["outputRoot"]).resolve()
    run_name = config["runName"]
    if not run_name or Path(run_name).name != run_name or run_name in {".", ".."} or ":" in run_name:
        raise ValueError("Invalid mesh run directory name")
    iteration = config["iteration"]
    if isinstance(iteration, bool) or not isinstance(iteration, int) or iteration <= 0:
        raise ValueError("Mesh source iteration must be a positive integer")
    mode = config["mode"]
    if mode not in MODES:
        raise ValueError("Unsupported mesh generation method")
    options = server.mesh_export_options({**config.get("meshOptions", {}), "mode": mode})
    # Reject NaN/Inf before launching an external process.
    if any(isinstance(value, float) and not math.isfinite(value) for value in options.values()):
        raise ValueError("Mesh options must contain finite numbers")
    texture = bool(config.get("bakeTexture", False))
    if mode == "sugar" and texture:
        raise ValueError("SuGaR owns its texture stage; separate OpenMVS baking is unavailable")
    cancel = threading.Event()
    current = [None]

    def watch():
        for line in sys.stdin:
            if line.strip().lower() == "cancel":
                cancel.set()
                if current[0]:
                    server.cancel_mesh_job(current[0])
                return

    threading.Thread(target=watch, name="gsw-mesh-cancel", daemon=True).start()
    output_root.mkdir(parents=True, exist_ok=True)
    run_dir = output_root / run_name
    run_dir.mkdir()  # exclusive: no restart or overwrite of an earlier run
    record_path = run_dir / "result.json"
    record = {"version": 1, "task": "mesh", "state": "running", "stage": "environment",
              "source": str(source), "iteration": iteration, "mode": mode, "completedStages": []}

    def stage(name, progress=None, preview=None):
        record["stage"] = name
        atomic_json(record_path, record)
        emit("running", name, progress, preview)

    def stream(job_id, label):
        current[0] = job_id
        if cancel.is_set():
            server.cancel_mesh_job(job_id)
        index = 0
        while True:
            with server.MESH_LOCK:
                job = server.MESH_JOBS[job_id]
                lines = list(job.get("log", [])[index:])
                index += len(lines)
                state = job.get("status", "failed")
                error = job.get("error")
                result = dict(job)
            for line in lines:
                print(line, flush=True)
            if cancel.is_set() or state == "cancelled":
                raise InterruptedError(label + " cancelled")
            if state == "failed":
                raise RuntimeError(error or label + " failed")
            if state == "done":
                current[0] = None
                return result
            time.sleep(0.1)

    try:
        stage("environment")
        if not source.is_dir() or not (source / "cfg_args").is_file():
            raise ValueError("Select a training output directory containing cfg_args")
        _validate_configuration(source / "cfg_args")
        input_ply = source / "point_cloud" / ("iteration_" + str(iteration)) / "point_cloud.ply"
        if not input_ply.is_file():
            raise FileNotFoundError("Selected training iteration is missing: " + str(input_ply))
        server.OUTPUT_DIR = source.parent
        if server.scene_backend(source.name, iteration) != MODES[mode]:
            raise ValueError("Mesh method is incompatible with the source training backend")
        if mode == "sugar":
            server.ensure_sugar_environment()
        elif mode == "gs2mesh":
            server.ensure_gs2mesh_environment()
        else:
            server.ensure_mesh_environment()
        if texture:
            server.ensure_openmvs_environment()
        if cancel.is_set():
            raise InterruptedError("Mesh job cancelled")
        stage("mesh_inputs")
        model = run_dir / run_name
        _copy_input(input_ply, model / "point_cloud" / input_ply.parent.name / input_ply.name, cancel)
        for name in ("cfg_args", "training_backend.json", "cameras.json", "input.ply", "exposure.json"):
            if (source / name).is_file():
                _copy_input(source / name, model / name, cancel)
        _validate_configuration(model / "cfg_args")
        server.OUTPUT_DIR = run_dir
        stage("mesh")
        result = stream(server.start_mesh_export(run_name, iteration, options)["id"], "Mesh generation")
        mesh = Path(result.get("output_mesh") or "").resolve()
        if not _within(mesh, model.resolve()) or not mesh.is_file():
            raise ValueError("Mesh result is outside the current job directory")
        stage("mesh_validation")
        geometry = validate_mesh(mesh, cancel)
        record.update(meshPath=str(mesh), geometry=geometry)
        record["completedStages"].append("mesh")
        preview = {"previewPath": str(mesh), "previewKind": "mesh", "previewIteration": 1}
        stage("mesh_ready", None, preview)
        if texture:
            stage("texture", None, preview)
            texture_options = {"mode": mode, "backend": "openmvs", "post": True,
                               "texture_res": config.get("textureResolution", 2048), "max_faces": 0}
            result = stream(server.start_texture_bake(run_name, iteration, texture_options)["id"], "Texture baking")
            files = result.get("texture") or {}
            for name in ("obj", "mtl", "png", "zip"):
                asset = Path(files.get(name) or "").resolve()
                if not _within(asset, model.resolve()) or not asset.is_file() or not asset.stat().st_size:
                    raise ValueError("Texture output is incomplete: " + name)
            record["texture"] = files
            record["completedStages"].append("texture")
        elif result.get("texture"):
            record["texture"] = result["texture"]
        if cancel.is_set():
            raise InterruptedError("Mesh job cancelled before publication")
        record.update(state="done", stage="done")
        atomic_json(record_path, record)
        emit("done", "done", 100, preview)
        return 0
    except Exception as exc:
        state = "cancelled" if cancel.is_set() or isinstance(exc, InterruptedError) else "failed"
        record.update(state=state, error=str(exc))
        atomic_json(record_path, record)
        emit(state, state)
        print("[worker] Mesh job {}: {}".format(state, exc), flush=True)
        return 130 if state == "cancelled" else 1
