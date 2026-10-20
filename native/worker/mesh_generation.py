"""Native staged mesh jobs over the existing, versioned backend commands.

Each invocation owns a new directory. Training inputs and previous meshes are
never used as writable work directories. Completed geometry is retained when a
later texture stage fails; this is an artifact, not an optimizer checkpoint.
"""

import ast
import json
import math
import sys
import threading
import time
from pathlib import Path

from native.worker.training_checkpoint import atomic_json
from native.worker.mesh_material_preview import prepare_material_preview


MODES = {"bounded": "2dgs", "unbounded": "2dgs", "sugar": "3dgs", "gs2mesh": "3dgs"}


class CanonicalMeshCapabilityError(ValueError):
    """Typed release gate, not an environment-installation or SfM failure."""
    code = "camera_dataset_mesh_backend_unavailable"

    def __init__(self, source, iteration, mode, markers):
        self.capability = {
            "canonical_camera_input": False, "mode": mode, "source": str(source), "iteration": iteration,
            "markers": markers,
            "reason": "external_scene_reader_does_not_consume_canonical_full_k_split_and_transform_chain",
            "remaining_acceptance": ["per_view_full_pinhole_projection", "depth_and_normal_camera_conventions",
                                     "training_to_source_scale_chain", "held_out_split_isolation",
                                     "mesh_and_texture_cross_backend_validation"],
        }
        super().__init__(self.code)


def require_mesh_camera_capability(source, iteration, mode):
    """Refuse tracked canonical outputs before external readers or processes.

    Only the selected model root and iteration are checked. These are reserved
    GSW marker names, so corrupt/unreadable markers cannot silently turn a
    canonical job into a legacy one. Other iterations and ordinary .prj files
    do not identify the selected input as a canonical training output.
    """
    source = Path(source)
    selected = source / "point_cloud" / ("iteration_{}".format(iteration)) / "point_cloud.camera-scale.json"
    found = []
    for path in (source / "training-camera-contract.json", selected):
        try:
            path.lstat()
        except FileNotFoundError:
            continue
        except OSError:
            found.append({"path": str(path), "schema": None, "state": "unreadable_marker"})
            continue
        evidence = {"path": str(path), "schema": None, "state": "present"}
        try:
            if path.stat().st_size > 128 * 1024 * 1024:
                raise ValueError("marker_too_large")
            with path.open(encoding="utf-8-sig") as stream:
                metadata = json.load(stream)
            if not isinstance(metadata, dict):
                raise ValueError("marker_not_object")
            schema = metadata.get("schema")
            frame = metadata.get("output_frame", "training")
            evidence["schema"] = schema if isinstance(schema, str) else None
            evidence["coordinate_frame"] = frame if isinstance(frame, str) else "unknown"
        except (OSError, ValueError, TypeError):
            evidence["state"] = "invalid_or_unreadable_marker"
        found.append(evidence)
    if found:
        raise CanonicalMeshCapabilityError(source, iteration, mode, found)


def _within(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def validate_mesh(path, cancel=None, textured=False):
    """Check complete triangle payload, finite positions and index ranges."""
    import numpy as np
    from plyfile import PlyData

    import inspect
    arguments = {"mmap": "r"}
    if "known_list_len" in inspect.signature(PlyData.read).parameters:
        arguments["known_list_len"] = {"face": {"vertex_indices": 3}}
        if textured:
            arguments["known_list_len"]["face"]["texcoord"] = 6
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
    values = {}
    for keyword in node.keywords:
        if not keyword.arg or keyword.arg in names:
            raise ValueError("Training cfg_args contains invalid or duplicate options")
        names.add(keyword.arg)
        values[keyword.arg] = ast.literal_eval(keyword.value)
    return values


def run_depth_geometry(config, server, emit):
    """Map the native typed job to a separate correct-backend depth worker.

    Unlike legacy mesh modes this consumes the locked full-K contract directly.
    The result is an un-fused pointcloud, not a triangle mesh or optimizer state.
    """
    import subprocess
    from native.backend.gs_geometry import GsGeometryError, _no_links, _read_json, file_sha256, inspect_geometry_source
    from native.worker.gs_geometry import _config, _write_json
    from native.worker.training_batch import _Commands, _run_child
    source, output_root = Path(config["modelDirectory"]), Path(config["outputRoot"])
    name = config["runName"]
    if (not source.is_absolute() or not output_root.is_absolute() or not isinstance(name, str) or not name
            or Path(name).name != name or name in (".", "..") or any(c in name for c in (":", "\\", "/"))):
        raise GsGeometryError("geometry_invalid_path")
    if config.get("bakeTexture"):
        raise GsGeometryError("geometry_texture_not_available_for_depth_pointcloud")
    dataset = config.get("cameraDatasetPath") or config.get("datasetPath")
    if not dataset:
        raise GsGeometryError("geometry_explicit_camera_dataset_required")
    _no_links(output_root)
    source = source.resolve()
    output_root = output_root.resolve()
    dataset_root = Path(dataset).resolve()
    dataset_root = dataset_root if dataset_root.is_dir() else dataset_root.parent
    if source == output_root or source in output_root.parents or dataset_root == output_root or dataset_root in output_root.parents:
        raise GsGeometryError("geometry_output_overlaps_source")
    # Admission before environment setup, renderer import or subprocess start.
    admitted = inspect_geometry_source(source, config["iteration"], Path(dataset))
    options = dict(config.get("meshOptions") or {})
    options.pop("mode", None)
    request = _config(dict(options, schema="gsw.gs_geometry_request/1", model_directory=str(source), dataset_path=str(dataset),
        iteration=config["iteration"], output_directory=str(output_root / name / "geometry")))
    # Resolve a missing runtime before recording a running job. A setup failure
    # must not strand a result.json permanently marked running.
    backend = admitted["backend"]
    environment = server.training_env(backend)
    environment["GSW_POSE_POLICY"] = "locked"
    environment["GSW_CAMERA_DATASET"] = admitted["prepared_manifest_path"]
    environment["PYTHONUTF8"] = "1"
    environment["PYTHONNOUSERSITE"] = "1"
    if backend == "2dgs":
        environment["GSW_TWO_DGS_SOURCE"] = str(server.TWO_DGS_DIR)
    executable = Path(server.training_python(backend))
    if not executable.is_file():
        raise GsGeometryError("geometry_renderer_python_missing")
    output_root.mkdir(parents=True, exist_ok=True)
    run_directory = output_root / name
    run_directory.mkdir()  # fresh typed job; no previous output overwritten
    configuration = run_directory / "request.json"
    receipt = run_directory / "receipt.json"
    record_path = run_directory / "result.json"
    _write_json(configuration, request)
    record = {"version": 1, "task": "mesh", "mode": "gs_depth", "state": "running", "stage": "geometry_depth",
        "source": str(source), "iteration": config["iteration"], "completedStages": [], "kind": "depth_point_cloud"}
    atomic_json(record_path, record)
    emit("running", "geometry_depth", 0)
    command = [str(executable), "-B", str(Path(__file__).with_name("gs_geometry.py")),
               "--config", str(configuration), "--report", str(receipt)]
    def on_line(channel, line):
        # Child progress is owned by this typed job, never another training.
        if channel == "stdout" and line.startswith("[worker-event] "):
            try:
                event = json.loads(line[len("[worker-event] "):])
                total = event.get("total_views", 0)
                fraction = 100. * event.get("completed_views", 0) / total if total else 0.
                emit("running", "geometry_depth", fraction)
            except (TypeError, ValueError, ZeroDivisionError):
                pass
        else:
            print("[geometry-child] " + line.rstrip(), flush=True)
        return False
    def process_factory(*args, **kwargs):
        return subprocess.Popen(*args, env=environment, **kwargs)
    try:
        code, requested = _run_child(command, configuration, _Commands(), process_factory, on_line)
        result = _read_json(receipt)
        manifest_path = run_directory / "geometry/manifest.json"
        if result.get("manifestPath") != str(manifest_path) or result.get("schema") != "gsw.gs_geometry_report/1":
            raise GsGeometryError("geometry_receipt_mismatch")
        manifest = _read_json(manifest_path)
        record.update(manifestPath=str(manifest_path), geometryReportPath=str(receipt))
        if code or not result.get("success") or manifest.get("status") != "completed":
            error = result.get("error", {})
            raise GsGeometryError(error.get("code", "geometry_extraction_failed"), error.get("detail", ""))
        geometry = manifest.get("geometry", {})
        cloud = run_directory / "geometry/point_cloud.ply"
        if (manifest.get("schema") != "gsw.gs_geometry/1" or geometry.get("path") != "point_cloud.ply"
                or geometry.get("kind") != "depth_point_cloud" or geometry.get("fusion") != "none"
                or geometry.get("sha256") != file_sha256(cloud) or result.get("geometryPath") != str(cloud)):
            raise GsGeometryError("geometry_product_mismatch")
        record.update(state="done", stage="done", geometryPath=str(cloud), geometry=geometry,
            completedStages=["depth", "pointcloud"], geometrySummary=result.get("summary", {}))
        atomic_json(record_path, record)
        emit("done", "done", 100, {"previewPath": str(cloud), "previewKind": "points", "previewIteration": 1})
        return 0
    except Exception as error:
        code = getattr(error, "code", "geometry_extraction_failed")
        state = "cancelled" if code == "geometry_cancelled" else "failed"
        record.update(state=state, stage=state, error=str(error), errorCode=code)
        atomic_json(record_path, record)
        emit(state, state, None, {"generation_issue": code})
        return 130 if state == "cancelled" else 1


def run(config, server, emit):
    if config.get("mode") == "gs_depth":
        return run_depth_geometry(config, server, emit)
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
        require_mesh_camera_capability(source, iteration, mode)
        if not source.is_dir() or not (source / "cfg_args").is_file():
            raise ValueError("Select a training output directory containing cfg_args")
        values = _validate_configuration(source / "cfg_args")
        dataset = Path(values.get("source_path") or "")
        if values.get("source_path") and (dataset / "sparse" / "0").is_dir():
            from native.backend.reconstruction_quality import require_dataset
            report = require_dataset(dataset)
            record["reconstructionQuality"] = report
            stage("environment", None, {"reconstruction_quality": report,
                  "reconstruction_quality_phase": "accepted" if report["preferred"] else "partial"})
            print("[worker] Mesh input reconstruction: {}/{} cameras, {} valid points".format(report["registeredImages"], report["inputImages"], report["validPoints"]), flush=True)
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
        for name in ("cfg_args", "training_backend.json", "cameras.json", "input.ply", "exposure.json", "reconstruction_quality.json"):
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
            record["texture"] = result.get("texture") or {}
        elif result.get("texture"):
            record["texture"] = result["texture"]
        if (texture or mode == "sugar") and not isinstance(record.get("texture"), dict):
            raise ValueError("Texture generation returned no valid asset bundle")
        if (texture or mode == "sugar") and not record.get("texture"):
            raise ValueError("Texture generation returned an empty asset bundle")
        if record.get("texture"):
            files = record["texture"]
            for name in ("obj", "mtl", "png", "zip"):
                asset = Path(files.get(name) or "").resolve()
                if not _within(asset, model.resolve()) or not asset.is_file() or not asset.stat().st_size:
                    raise ValueError("Texture output is incomplete: " + name)
            record["completedStages"].append("texture")
            stage("texture_preview", None, preview)
            material = prepare_material_preview(files, model / "native-material", model, cancel)
            validate_mesh(material["ply"], cancel, textured=True)
            record["materialPreview"] = material
            record["completedStages"].append("texture_preview")
            preview = {"previewPath": material["ply"], "previewKind": "mesh", "previewIteration": 2}
            stage("texture_ready", None, preview)
        if cancel.is_set():
            raise InterruptedError("Mesh job cancelled before publication")
        record.update(state="done", stage="done")
        atomic_json(record_path, record)
        emit("done", "done", 100, preview)
        return 0
    except Exception as exc:
        state = "cancelled" if cancel.is_set() or isinstance(exc, InterruptedError) else "failed"
        record.update(state=state, error=str(exc))
        if isinstance(exc, CanonicalMeshCapabilityError):
            record.update(errorCode=exc.code, capability=exc.capability)
        atomic_json(record_path, record)
        if isinstance(exc, CanonicalMeshCapabilityError):
            emit(state, state, None, {"generation_issue": exc.code, "canonical_camera_input": False,
                                    "capability": exc.capability})
            print("[worker] Mesh job {}: {}".format(state, exc), flush=True)
            return 1
        quality = getattr(exc, "report", None)
        if isinstance(quality, dict):
            record["reconstructionQuality"] = quality
            atomic_json(record_path, record)
            emit(state, state, None, {"reconstruction_quality": quality,
                 "reconstruction_quality_phase": "rejected", "generation_issue": "reconstruction_quality"})
        else:
            emit(state, state)
        print("[worker] Mesh job {}: {}".format(state, exc), flush=True)
        return 130 if state == "cancelled" else 1
