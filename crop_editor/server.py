import argparse
import cgi
import concurrent.futures
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import uuid
import webbrowser
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PurePosixPath
from urllib.parse import parse_qs, quote, urlparse

import numpy as np
from plyfile import PlyData, PlyElement


ROOT = Path(__file__).resolve().parents[1]
DATASETS_DIR = ROOT / "datasets"
OUTPUT_DIR = ROOT / "output"
PSNR_REPORTS_DIR = ROOT / "reports" / "psnr"
GAUSSIAN_DIR = ROOT / "gaussian-splatting"
TWO_DGS_DIR = Path(os.environ.get("TWO_DGS_DIR", Path.home() / "Documents" / "2dgs"))
SUGAR_DIR = Path(os.environ.get("SUGAR_DIR", ROOT / "SuGaR"))
GS2MESH_DIR = Path(os.environ.get("GS2MESH_DIR", ROOT / "gs2mesh"))
OPENMVS_DIR = Path(os.environ.get("OPENMVS_DIR", ROOT / "openMVS"))
TRAINING_KIT_DIR = ROOT / "training_kit"
STATIC_DIR = Path(__file__).resolve().parent / "static"
PREVIEW_DIR = Path(__file__).resolve().parent / ".preview"
JOBS_DIR = Path(__file__).resolve().parent / ".jobs"
TRAIN_JOBS_DIR = JOBS_DIR / "training"
SPLAT_JOBS_DIR = JOBS_DIR / "splat"
C0 = 0.28209479177387814
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}
VIDEO_EXTS = {".mp4", ".mov", ".avi", ".mkv", ".webm", ".m4v"}
MASK_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp"}
SPLAT_FORMATS = {"ply", "spz", "sog"}
TRAIN_JOBS = {}
TRAIN_LOCK = threading.Lock()
MESH_JOBS = {}
MESH_LOCK = threading.Lock()
SPLAT_JOBS = {}
SPLAT_LOCK = threading.Lock()
IMPORT_JOBS = {}
IMPORT_LOCK = threading.Lock()
TRAINING_BACKENDS = {"3dgs", "2dgs"}
MESH_MODES = {"bounded", "unbounded", "sugar", "gs2mesh"}
SPARSE_ADAM_AVAILABLE_CACHE = None
MESH_PREVIEW_MAX_FACES = 300000
MESH_CHUNK_MAX_FACES = 250000


def mesh_like_job_kind(job):
    return job.get("kind") in {"mesh", "texture", "psnr"}


def safe_name(name):
    if not name or any(part in name for part in ("..", "/", "\\")) or not re.match(r"^[A-Za-z0-9_.-]+$", name):
        raise ValueError("Invalid scene name")
    return name


def safe_filename(name):
    name = Path(name or "file").name
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", name)
    return name or f"file_{uuid.uuid4().hex}"


def unique_path(directory, filename):
    candidate = directory / filename
    if not candidate.exists():
        return candidate
    stem = Path(filename).stem
    suffix = Path(filename).suffix
    return directory / f"{stem}_{uuid.uuid4().hex[:8]}{suffix}"


def unlink_if_exists(path):
    try:
        Path(path).unlink()
        return True
    except FileNotFoundError:
        return False


def form_items(form, key):
    try:
        items = form[key]
    except (KeyError, TypeError):
        items = form.get(key, []) if hasattr(form, "get") else []
    if not isinstance(items, list):
        items = [items]
    return [item for item in items if getattr(item, "filename", None)]


def parse_file_metadata(form):
    raw = form.getfirst("fileMetadata", "[]")
    try:
        metadata = json.loads(raw)
    except Exception:
        return []
    return metadata if isinstance(metadata, list) else []


def maybe_apply_uploaded_mtime(path, metadata):
    last_modified = metadata.get("lastModified") if isinstance(metadata, dict) else None
    if not isinstance(last_modified, (int, float)) or last_modified <= 0:
        return
    timestamp = float(last_modified) / 1000.0
    try:
        os.utime(path, (timestamp, timestamp))
    except OSError:
        pass


def write_import_manifest(source_dir, records):
    manifest_path = source_dir / "metadata_manifest.json"
    if manifest_path.exists():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except Exception:
            manifest = {}
    else:
        manifest = {}
    manifest.setdefault("version", 1)
    manifest.setdefault("files", [])
    manifest["files"].extend(records)
    manifest["updated_at"] = time.time()
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")


def mask_match_key(path):
    stem = Path(path).stem.lower()
    for suffix in ("_mask", "-mask", ".mask", "_alpha", "-alpha", ".alpha"):
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    return re.sub(r"[^a-z0-9]+", "", stem)


def save_uploaded_masks(dataset, form):
    masks = form_items(form, "maskFiles")
    if not masks:
        return 0
    masks_dir = dataset / "source" / "masks"
    masks_dir.mkdir(parents=True, exist_ok=True)
    saved = 0
    for item in masks:
        filename = safe_filename(item.filename)
        if Path(filename).suffix.lower() not in MASK_EXTS:
            continue
        target = unique_path(masks_dir, filename)
        with open(target, "wb") as f:
            shutil.copyfileobj(item.file, f)
        saved += 1
    return saved


def apply_alpha_masks_to_dataset(dataset):
    masks_dir = dataset / "source" / "masks"
    images_dir = dataset / "images"
    if not masks_dir.exists() or not images_dir.exists():
        return 0
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError("Mask training requires Pillow in the Python environment") from exc

    masks_by_key = {}
    for mask_path in masks_dir.iterdir():
        if mask_path.suffix.lower() in MASK_EXTS:
            masks_by_key.setdefault(mask_match_key(mask_path), mask_path)

    applied = 0
    for image_path in list(images_dir.iterdir()):
        if image_path.suffix.lower() not in IMAGE_EXTS:
            continue
        mask_path = masks_by_key.get(mask_match_key(image_path))
        if not mask_path:
            continue
        image = Image.open(image_path).convert("RGBA")
        alpha = Image.open(mask_path).convert("L")
        if alpha.size != image.size:
            alpha = alpha.resize(image.size, Image.Resampling.BILINEAR)
        image.putalpha(alpha)
        target = image_path if image_path.suffix.lower() == ".png" else image_path.with_suffix(".png")
        image.save(target)
        if target != image_path:
            image_path.unlink()
        applied += 1
    return applied


def safe_training_backend(backend):
    backend = (backend or "3dgs").lower()
    if backend not in TRAINING_BACKENDS:
        raise ValueError("Invalid training backend")
    return backend


def model_dir(scene):
    return OUTPUT_DIR / safe_name(scene)


def ply_path(scene, iteration):
    return model_dir(scene) / "point_cloud" / f"iteration_{int(iteration)}" / "point_cloud.ply"


def splat_transform_executable():
    bin_dir = Path(__file__).resolve().parent / "node_modules" / ".bin"
    candidates = [
        bin_dir / ("splat-transform.cmd" if os.name == "nt" else "splat-transform"),
        bin_dir / "splat-transform",
        shutil.which("splat-transform"),
    ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists() or shutil.which(str(candidate)):
            return path
    raise FileNotFoundError("Missing splat-transform. Run npm install in crop_editor.")


def run_splat_transform(input_path, output_path):
    input_path = Path(input_path)
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    command = [splat_transform_executable(), "-w", input_path, output_path]
    result = subprocess.run(command, cwd=Path(__file__).resolve().parent, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"splat-transform failed: {detail}")
    return output_path


def export_splat_format(scene, iteration, fmt):
    fmt = (fmt or "ply").lower()
    if fmt not in SPLAT_FORMATS:
        raise ValueError("Unsupported splat format")
    source = ply_path(scene, iteration)
    if fmt == "ply":
        return source
    output = source.with_name(f"point_cloud.{fmt}")
    if output.exists() and output.stat().st_mtime >= source.stat().st_mtime:
        return output
    return run_splat_transform(source, output)


def import_splat_scene(upload_item, scene, overwrite=False):
    scene = safe_name(scene)
    if not getattr(upload_item, "filename", None):
        raise ValueError("Choose a .ply, .spz, or .sog file")
    filename = safe_filename(upload_item.filename)
    fmt = Path(filename).suffix.lower().lstrip(".")
    if fmt not in SPLAT_FORMATS:
        raise ValueError("Only .ply, .spz, and .sog files can be imported as splat scenes")
    dst_model = model_dir(scene)
    if dst_model.exists() and any(dst_model.iterdir()):
        if overwrite:
            shutil.rmtree(dst_model)
        else:
            raise ValueError(f"Output scene already exists: {scene}")
    iteration_dir = dst_model / "point_cloud" / "iteration_0"
    iteration_dir.mkdir(parents=True, exist_ok=True)
    source_dir = dst_model / "source"
    source_dir.mkdir(parents=True, exist_ok=True)
    uploaded = source_dir / filename
    with open(uploaded, "wb") as f:
        shutil.copyfileobj(upload_item.file, f)
    output_ply = iteration_dir / "point_cloud.ply"
    if fmt == "ply":
        shutil.copy2(uploaded, output_ply)
    else:
        run_splat_transform(uploaded, output_ply)
    write_training_metadata(dst_model, "3dgs", "imported", {"format": fmt, "source": filename})
    return {
        "scene": scene,
        "format": fmt,
        "latest_iteration": 0,
        "backend": "3dgs",
        "path": str(output_ply),
    }


def latest_iteration(scene):
    pc_dir = model_dir(scene) / "point_cloud"
    if not pc_dir.exists():
        return None
    iterations = []
    for child in pc_dir.iterdir():
        if child.is_dir() and child.name.startswith("iteration_"):
            try:
                iterations.append(int(child.name.split("_", 1)[1]))
            except ValueError:
                pass
    return max(iterations) if iterations else None


def detect_ply_backend(scene, iteration=None):
    iteration = latest_iteration(scene) if iteration is None else int(iteration)
    if iteration is None:
        return "3dgs"
    try:
        _, vertices = read_vertices(ply_path(scene, iteration))
    except Exception:
        return "3dgs"
    names = vertices.dtype.names or ()
    if "scale_0" in names and "scale_1" in names and "scale_2" not in names:
        return "2dgs"
    return "3dgs"


def scene_backend(scene, iteration=None):
    metadata_path = model_dir(scene) / "training_backend.json"
    if metadata_path.exists():
        try:
            with open(metadata_path, "r", encoding="utf-8") as f:
                return safe_training_backend(json.load(f).get("backend", "3dgs"))
        except Exception:
            pass
    return detect_ply_backend(scene, iteration)


def ensure_output_backend_compatible(output_scene, requested_backend):
    output = model_dir(output_scene)
    if not output.exists() or not any(output.iterdir()):
        return
    existing_backend = scene_backend(output_scene)
    if existing_backend != requested_backend:
        raise ValueError(
            f"Output scene '{output_scene}' belongs to {existing_backend.upper()}, "
            f"but this job is {requested_backend.upper()}. Choose a different output name "
            "to avoid overwriting trained data."
        )


def list_scenes():
    scenes = []
    if not OUTPUT_DIR.exists():
        return scenes
    for child in sorted(OUTPUT_DIR.iterdir()):
        if not child.is_dir():
            continue
        it = latest_iteration(child.name)
        if it is not None and ply_path(child.name, it).exists():
            scenes.append({"name": child.name, "latest_iteration": it, "backend": scene_backend(child.name, it)})
    return scenes


def list_datasets():
    datasets = []
    if not DATASETS_DIR.exists():
        return datasets
    for child in sorted(DATASETS_DIR.iterdir()):
        if not child.is_dir():
            continue
        images_dir = child / "images"
        image_count = 0
        if images_dir.exists():
            image_count = sum(1 for p in images_dir.iterdir() if p.suffix.lower() in IMAGE_EXTS)
        datasets.append({
            "name": child.name,
            "image_count": image_count,
            "has_alignment": dataset_has_alignment_source(child),
        })
    return datasets


def asset_file(path, label, kind, url=None, extra=None):
    path = Path(path)
    exists = path.exists()
    payload = {
        "label": label,
        "kind": kind,
        "exists": exists,
        "path": str(path),
        "size": path.stat().st_size if exists and path.is_file() else 0,
        "mtime": path.stat().st_mtime if exists else None,
        "url": url if exists else None,
    }
    if extra:
        payload.update(extra)
    return payload


def count_files_with_ext(directory, extensions):
    directory = Path(directory)
    if not directory.exists():
        return 0
    return sum(1 for path in directory.rglob("*") if path.is_file() and path.suffix.lower() in extensions)


def scene_source_path(scene):
    model = model_dir(scene)
    try:
        cfg = load_cfg_args(model)
        source = getattr(cfg, "source_path", "") or ""
        if source:
            return Path(source)
    except Exception:
        pass
    return DATASETS_DIR / scene


def scene_config_assets(scene):
    model = model_dir(scene)
    names = [
        ("scene.json", "Scene metadata", "scene_config"),
        ("training_backend.json", "Training backend", "training_config"),
        ("cfg_args", "Training cfg_args", "training_args"),
        ("cameras.json", "Viewer cameras", "cameras"),
        ("exposure.json", "Exposure data", "exposure"),
        ("input.ply", "Input sparse point cloud", "input_cloud"),
    ]
    return [
        asset_file(model / name, label, kind, f"/api/assets/file?scene={safe_name(scene)}&name={name}")
        for name, label, kind in names
    ]


def scene_splat_assets(scene, iteration):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    source = ply_path(scene, iteration)
    parent = source.parent
    return [
        asset_file(
            source,
            "PLY point cloud",
            "ply",
            f"/api/ply?scene={scene}&iteration={iteration}",
            {"format": "ply"},
        ),
        asset_file(
            parent / "point_cloud.spz",
            "SPZ compressed splat",
            "spz",
            f"/api/splat/export?scene={scene}&iteration={iteration}&format=spz&cached=true",
            {"format": "spz", "exportable": True},
        ),
        asset_file(
            parent / "point_cloud.sog",
            "SOG compressed splat",
            "sog",
            f"/api/splat/export?scene={scene}&iteration={iteration}&format=sog&cached=true",
            {"format": "sog", "exportable": True},
        ),
    ]


def scene_source_assets(scene):
    source = scene_source_path(scene)
    images_dir = source / "images"
    manifest = source / "source" / "metadata_manifest.json"
    alignment_cache = alignment_cache_dir(source)
    return {
        "path": str(source),
        "exists": source.exists(),
        "image_count": count_files_with_ext(images_dir, IMAGE_EXTS),
        "video_count": count_files_with_ext(source / "source" / "originals", VIDEO_EXTS),
        "has_alignment": dataset_has_alignment_source(source) if source.exists() else False,
        "files": [
            asset_file(manifest, "Import manifest", "source_manifest"),
            asset_file(alignment_cache / "metadata.json", "Alignment cache metadata", "alignment_cache"),
        ],
    }


def psnr_report_file_path(scene, run, name):
    scene = safe_name(scene)
    run = safe_name(run)
    name = safe_filename(name)
    if name not in {"psnr_results.json", "psnr_results.csv"}:
        raise ValueError("Unsupported PSNR asset file")
    path = (PSNR_REPORTS_DIR / scene / run / name).resolve()
    root = (PSNR_REPORTS_DIR / scene).resolve()
    if not str(path).startswith(str(root)):
        raise ValueError("Invalid PSNR asset path")
    return path


def open_psnr_report_dir(scene, run):
    directory = psnr_report_file_path(scene, run, "psnr_results.json").parent
    if not directory.exists():
        raise FileNotFoundError(f"PSNR report not found: {directory}")
    if os.name == "nt":
        os.startfile(str(directory))
    else:
        webbrowser.open(directory.as_uri())
    return {"ok": True, "path": str(directory)}


def scene_psnr_assets(scene):
    scene = safe_name(scene)
    base = PSNR_REPORTS_DIR / scene
    if not base.exists():
        return []
    reports = []
    for result_json in base.glob("*/psnr_results.json"):
        run_dir = result_json.parent
        run = run_dir.name
        result = {}
        try:
            result = json.loads(result_json.read_text(encoding="utf-8"))
        except Exception:
            result = {}
        csv_path = run_dir / "psnr_results.csv"
        backend = str(result.get("backend") or run.split("_iter_", 1)[0] or "").upper()
        iteration = result.get("iteration")
        rendered = result.get("rendered_count")
        requested = result.get("requested_count")
        average = result.get("average_psnr")
        if isinstance(average, (int, float)):
            label = f"PSNR {backend} iter {iteration}: {average:.2f} dB"
        else:
            label = f"PSNR {backend} iter {iteration}"
        total_size = result_json.stat().st_size + (csv_path.stat().st_size if csv_path.exists() else 0)
        mtime = max(result_json.stat().st_mtime, csv_path.stat().st_mtime if csv_path.exists() else 0)
        reports.append({
            "label": label,
            "kind": "psnr_report",
            "exists": True,
            "path": str(run_dir),
            "size": total_size,
            "mtime": mtime,
            "url": f"/api/assets/psnr-file?scene={scene}&run={quote(run)}&name=psnr_results.json",
            "csv_url": (
                f"/api/assets/psnr-file?scene={scene}&run={quote(run)}&name=psnr_results.csv"
                if csv_path.exists()
                else None
            ),
            "open_psnr_run": run,
            "backend": backend.lower(),
            "iteration": iteration,
            "average_psnr": average,
            "rendered_count": rendered,
            "requested_count": requested,
            "eval_width": result.get("eval_width"),
            "fallback_reason": result.get("fallback_reason"),
        })
    reports.sort(key=lambda item: item.get("mtime") or 0, reverse=True)
    return reports


def scene_related_jobs(scene):
    scene = safe_name(scene)
    jobs = []
    with TRAIN_LOCK:
        for job in TRAIN_JOBS.values():
            if job.get("scene") == scene or job.get("output_scene") == scene:
                jobs.append(unified_job_snapshot("training", job))
    with MESH_LOCK:
        for job in MESH_JOBS.values():
            if job.get("scene") == scene:
                jobs.append(unified_job_snapshot("mesh", job))
    with SPLAT_LOCK:
        for job in SPLAT_JOBS.values():
            if job.get("scene") == scene:
                jobs.append(unified_job_snapshot("splat", job))
    jobs.sort(key=lambda item: item.get("updated_at") or item.get("created_at") or 0, reverse=True)
    return jobs[:20]


def flatten_mesh_assets(mesh_payload):
    assets = []
    for mode, mode_assets in (mesh_payload.get("meshes") or {}).items():
        for key in ("post", "raw"):
            item = mode_assets.get(key)
            if not item:
                continue
            assets.append({
                **item,
                "label": f"{mode} {'mesh' if key == 'post' else 'raw mesh'}",
                "kind": "mesh",
                "mode": mode,
            })
        texture = mode_assets.get("texture") or {}
        for fmt, item in (texture.get("files") or {}).items():
            assets.append({
                **item,
                "label": f"{mode} texture {fmt.upper()}",
                "kind": "glb" if fmt == "glb" else "texture",
                "mode": mode,
                "format": fmt,
            })
    return assets


def scene_asset_payload(scene, iteration=None):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    backend = scene_backend(scene, iteration)
    source_assets = scene_source_assets(scene)
    mesh_payload = mesh_asset_payload(scene, iteration)
    psnr_assets = scene_psnr_assets(scene)
    return {
        "scene": scene,
        "iteration": iteration,
        "backend": backend,
        "output_dir": str(model_dir(scene)),
        "source": source_assets,
        "psnr_latest": psnr_assets[0] if psnr_assets else None,
        "groups": [
            {"id": "splats", "title": "Gaussian / Splat", "items": scene_splat_assets(scene, iteration)},
            {"id": "mesh", "title": "Mesh / Texture / GLB", "items": flatten_mesh_assets(mesh_payload)},
            {"id": "psnr", "title": "PSNR Reports", "items": psnr_assets},
            {"id": "config", "title": "Training Config", "items": scene_config_assets(scene)},
            {"id": "source", "title": "Source Data", "items": source_assets["files"]},
        ],
        "jobs": scene_related_jobs(scene),
    }


def asset_config_file_path(scene, name):
    name = safe_filename(name)
    allowed = {"scene.json", "training_backend.json", "cfg_args", "cameras.json", "exposure.json", "input.ply"}
    if name not in allowed:
        raise ValueError("Unsupported asset file")
    return model_dir(scene) / name


def open_asset_dir(scene, target):
    scene = safe_name(scene)
    if target == "source":
        directory = scene_source_path(scene)
    elif target == "output":
        directory = model_dir(scene)
    else:
        raise ValueError("Unsupported asset directory")
    directory.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        os.startfile(str(directory))
    else:
        webbrowser.open(directory.as_uri())
    return {"ok": True, "path": str(directory)}


def read_vertices(path):
    ply = PlyData.read(path)
    if "vertex" not in ply:
        raise ValueError(f"No vertex element in {path}")
    vertices = ply["vertex"].data
    for field in ("x", "y", "z"):
        if field not in vertices.dtype.names:
            raise ValueError(f"Missing {field} field in {path}")
    return ply, vertices


def colors_from_vertices(vertices):
    names = vertices.dtype.names
    if all(name in names for name in ("red", "green", "blue")):
        rgb = np.column_stack([vertices["red"], vertices["green"], vertices["blue"]])
        return np.clip(rgb, 0, 255).astype(np.uint8)
    if all(name in names for name in ("f_dc_0", "f_dc_1", "f_dc_2")):
        rgb = np.column_stack([vertices["f_dc_0"], vertices["f_dc_1"], vertices["f_dc_2"]])
        rgb = np.nan_to_num(rgb, nan=0.0, posinf=1.0, neginf=0.0)
        rgb = np.clip(rgb * C0 + 0.5, 0.0, 1.0)
        return (rgb * 255).astype(np.uint8)
    return np.full((len(vertices), 3), 210, dtype=np.uint8)


def opacity_from_vertices(vertices):
    names = vertices.dtype.names
    if "opacity" not in names:
        return np.ones(len(vertices), dtype=np.float32)
    raw = np.asarray(vertices["opacity"], dtype=np.float32)
    raw = np.nan_to_num(raw, nan=0.0, posinf=20.0, neginf=-20.0)
    return (1.0 / (1.0 + np.exp(-raw))).astype(np.float32)


def scale_from_vertices(vertices):
    if len(vertices) == 0:
        return np.empty(0, dtype=np.float32)
    names = vertices.dtype.names
    scale_names = [name for name in ("scale_0", "scale_1", "scale_2") if name in names]
    if not scale_names:
        return np.ones(len(vertices), dtype=np.float32) * 0.02
    raw = np.column_stack([vertices[name] for name in scale_names]).astype(np.float32)
    raw = np.nan_to_num(raw, nan=-4.0, posinf=4.0, neginf=-12.0)
    scale = np.exp(raw).mean(axis=1)
    finite = scale[np.isfinite(scale)]
    if len(finite) == 0:
        return np.ones(len(vertices), dtype=np.float32) * 0.02
    low, high = np.percentile(finite, [2, 98])
    if not np.isfinite(low) or not np.isfinite(high) or high < low:
        low, high = float(finite.min()), float(finite.max())
    scale = np.nan_to_num(scale, nan=float(low), posinf=float(high), neginf=float(low))
    scale = np.clip(scale, low, high)
    return scale.astype(np.float32)


def bounds_from_xyz(xyz):
    if len(xyz) == 0:
        return {"min": [0.0, 0.0, 0.0], "max": [0.0, 0.0, 0.0]}
    return {"min": xyz.min(axis=0).tolist(), "max": xyz.max(axis=0).tolist()}


def view_bounds_from_xyz(xyz):
    if len(xyz) < 100:
        return bounds_from_xyz(xyz)
    low, high = np.percentile(xyz, [1, 99], axis=0)
    if not np.isfinite(low).all() or not np.isfinite(high).all() or np.any(high <= low):
        return bounds_from_xyz(xyz)
    return {"min": low.tolist(), "max": high.tolist()}


def point_payload(scene, iteration):
    path = ply_path(scene, iteration)
    ply, vertices = read_vertices(path)
    xyz = np.column_stack([vertices["x"], vertices["y"], vertices["z"]]).astype(np.float32, copy=False)
    finite_mask = np.isfinite(xyz).all(axis=1)
    dropped_non_finite = int(len(vertices) - int(finite_mask.sum()))
    if dropped_non_finite:
        vertices = vertices[finite_mask]
        xyz = xyz[finite_mask]
    rgb = colors_from_vertices(vertices)
    opacity = opacity_from_vertices(vertices)
    scale = scale_from_vertices(vertices)
    header = {
        "scene": scene,
        "iteration": int(iteration),
        "backend": scene_backend(scene, iteration),
        "count": int(len(vertices)),
        "dropped_non_finite": dropped_non_finite,
        "bounds": bounds_from_xyz(xyz),
        "view_bounds": view_bounds_from_xyz(xyz),
        "ply": str(path),
        "properties": list(vertices.dtype.names),
        "layout": "uint32_header_json + float32_xyz + uint8_rgb + float32_opacity + float32_scale",
    }
    header_bytes = json.dumps(header, allow_nan=False).encode("utf-8")
    return struct.pack("<I", len(header_bytes)) + header_bytes + xyz.tobytes() + rgb.tobytes() + opacity.tobytes() + scale.tobytes()


def camera_payload(scene):
    path = model_dir(scene) / "cameras.json"
    if not path.exists():
        return {"cameras": []}
    with open(path, "r", encoding="utf-8") as f:
        cameras = json.load(f)
    trimmed = []
    for cam in cameras:
        trimmed.append(
            {
                "id": cam.get("id"),
                "img_name": cam.get("img_name"),
                "width": cam.get("width"),
                "height": cam.get("height"),
                "position": cam.get("position"),
                "rotation": cam.get("rotation"),
                "fx": cam.get("fx"),
                "fy": cam.get("fy"),
            }
        )
    return {"cameras": trimmed}


def copy_model_sidecars(src_model, dst_model):
    dst_model.mkdir(parents=True, exist_ok=True)
    for name in ("cfg_args", "cameras.json", "input.ply"):
        src = src_model / name
        if src.exists():
            shutil.copy2(src, dst_model / name)


def filtered_ply(src_ply, dst_ply, delete_indices):
    ply, vertices = read_vertices(src_ply)
    total = len(vertices)
    mask = np.ones(total, dtype=bool)
    delete = np.asarray(delete_indices, dtype=np.int64)
    if delete.size:
        delete = delete[(delete >= 0) & (delete < total)]
        mask[np.unique(delete)] = False
    kept = vertices[mask]
    if len(kept) == 0:
        raise ValueError("Crop would remove all Gaussians")

    dst_ply.parent.mkdir(parents=True, exist_ok=True)
    elements = []
    for element in ply.elements:
        if element.name == "vertex":
            elements.append(PlyElement.describe(kept, "vertex"))
        else:
            elements.append(element)
    PlyData(
        elements,
        text=ply.text,
        byte_order=ply.byte_order,
        comments=ply.comments,
        obj_info=ply.obj_info,
    ).write(dst_ply)
    return int(len(kept)), int(total - len(kept))


def compact_mesh_ply(path):
    path = Path(path)
    ply = PlyData.read(path)
    if "vertex" not in ply or "face" not in ply:
        raise ValueError(f"Mesh PLY must contain vertex and face elements: {path}")
    vertices = ply["vertex"].data
    faces = ply["face"].data
    if "vertex_indices" not in faces.dtype.names:
        raise ValueError(f"Mesh PLY faces must contain vertex_indices: {path}")

    remap = {}
    kept_faces = []
    for row in faces:
        face = [int(index) for index in row["vertex_indices"]]
        if len(face) < 3:
            continue
        mapped = []
        for index in face:
            if index < 0 or index >= len(vertices):
                raise ValueError(f"Mesh face references invalid vertex index {index}")
            if index not in remap:
                remap[index] = len(remap)
            mapped.append(remap[index])
        kept_faces.append(mapped)
    if not kept_faces:
        raise ValueError("Trimmed mesh has no faces")

    ordered_old_indices = np.empty(len(remap), dtype=np.int64)
    for old_index, new_index in remap.items():
        ordered_old_indices[new_index] = old_index
    compact_vertices = vertices[ordered_old_indices]
    face_dtype = [("vertex_indices", "O")]
    compact_faces = np.empty(len(kept_faces), dtype=face_dtype)
    compact_faces["vertex_indices"] = [np.asarray(face, dtype=np.int32) for face in kept_faces]

    elements = []
    for element in ply.elements:
        if element.name == "vertex":
            elements.append(PlyElement.describe(compact_vertices, "vertex"))
        elif element.name == "face":
            elements.append(PlyElement.describe(compact_faces, "face"))
        else:
            elements.append(element)
    PlyData(
        elements,
        text=ply.text,
        byte_order=ply.byte_order,
        comments=ply.comments,
        obj_info=ply.obj_info,
    ).write(path)
    return int(len(compact_vertices)), int(len(compact_faces))


def save_cropped(scene, iteration, output_scene, delete_indices):
    src_model = model_dir(scene)
    dst_model = model_dir(output_scene)
    src_ply = ply_path(scene, iteration)
    dst_ply = ply_path(output_scene, iteration)

    copy_model_sidecars(src_model, dst_model)
    kept, removed = filtered_ply(src_ply, dst_ply, delete_indices)
    write_training_metadata(dst_model, scene_backend(scene, iteration))
    return {"output_scene": output_scene, "kept": kept, "removed": removed, "path": str(dst_model)}


def save_trimmed_mesh(scene, iteration, output_scene, mode, ply_text, overwrite=False):
    scene = safe_name(scene)
    output_scene = safe_name(output_scene)
    iteration = int(iteration)
    mode = safe_mesh_mode(mode)
    if output_scene == scene:
        raise ValueError("Output scene must be different from the source scene")
    if not ply_text or not ply_text.lstrip().startswith("ply"):
        raise ValueError("Trimmed mesh must be a PLY file")

    src_model = model_dir(scene)
    dst_model = model_dir(output_scene)
    if dst_model.exists() and not overwrite:
        raise ValueError(f"Output scene already exists: {output_scene}")

    src_point_cloud = ply_path(scene, iteration)
    if not src_point_cloud.exists():
        raise FileNotFoundError(f"Source Gaussian model not found: {src_point_cloud}")

    if dst_model.exists() and overwrite:
        shutil.rmtree(dst_model)
    copy_model_sidecars(src_model, dst_model)
    dst_point_cloud = ply_path(output_scene, iteration)
    dst_point_cloud.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src_point_cloud, dst_point_cloud)

    dst_mesh = mesh_output_path(output_scene, iteration, mode, post=True)
    dst_mesh.parent.mkdir(parents=True, exist_ok=True)
    dst_mesh.write_text(ply_text, encoding="utf-8")
    try:
        vertex_count, face_count = compact_mesh_ply(dst_mesh)
    except Exception as exc:
        unlink_if_exists(dst_mesh)
        raise ValueError(f"Invalid trimmed mesh PLY: {exc}") from exc

    write_training_metadata(dst_model, scene_backend(scene, iteration))
    return {
        "output_scene": output_scene,
        "iteration": iteration,
        "mode": mode,
        "path": str(dst_model),
        "mesh_path": str(dst_mesh),
        "mesh_url": mesh_file_url(output_scene, iteration, mode, post=True),
        "mesh_vertices": vertex_count,
        "mesh_faces": face_count,
    }


def load_cfg_args(model_path):
    cfg_path = Path(model_path) / "cfg_args"
    if not cfg_path.exists():
        return argparse.Namespace()
    text = cfg_path.read_text(encoding="utf-8")
    return eval(text, {"Namespace": argparse.Namespace})


def safe_mesh_mode(mode):
    mode = (mode or "bounded").lower()
    if mode not in MESH_MODES:
        raise ValueError("Invalid mesh export mode")
    return mode


def mesh_mode_supports_texture_bake(mode, backend):
    mode = safe_mesh_mode(mode)
    backend = safe_training_backend(backend)
    if mode == "gs2mesh":
        return backend == "3dgs"
    if mode == "sugar":
        return False
    return backend == "2dgs"


def mesh_output_path(scene, iteration, mode="bounded", post=True):
    mode = safe_mesh_mode(mode)
    if mode == "sugar":
        name = "sugar_mesh.ply"
    elif mode == "gs2mesh":
        name = "gs2mesh.ply"
    else:
        name = "fuse_unbounded.ply" if mode == "unbounded" else "fuse.ply"
    if post:
        name = name.replace(".ply", "_post.ply")
    return model_dir(scene) / "train" / f"ours_{int(iteration)}" / name


def mesh_file_url(scene, iteration, mode="bounded", post=True):
    return f"/api/mesh/file?scene={safe_name(scene)}&iteration={int(iteration)}&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}"


def mesh_preview_file_url(scene, iteration, mode="bounded", post=True, max_faces=MESH_PREVIEW_MAX_FACES):
    return (
        f"/api/mesh/preview_file?scene={safe_name(scene)}&iteration={int(iteration)}"
        f"&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}&max_faces={int(max_faces)}"
    )


def mesh_chunk_manifest_url(scene, iteration, mode="bounded", post=True, max_faces=MESH_CHUNK_MAX_FACES):
    return (
        f"/api/mesh/chunks?scene={safe_name(scene)}&iteration={int(iteration)}"
        f"&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}&max_faces={int(max_faces)}"
    )


def mesh_chunk_file_url(scene, iteration, mode, post, key, index):
    return (
        f"/api/mesh/chunk_file?scene={safe_name(scene)}&iteration={int(iteration)}"
        f"&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}"
        f"&key={safe_filename(key)}&index={int(index)}"
    )


def mesh_texture_dir(scene, iteration, mode="bounded", post=True):
    source = mesh_output_path(scene, iteration, mode, post)
    return source.parent / f"{source.stem}_texture"


def mesh_texture_paths(scene, iteration, mode="bounded", post=True):
    scene = safe_name(scene)
    iteration = int(iteration)
    mode = safe_mesh_mode(mode)
    out_dir = mesh_texture_dir(scene, iteration, mode, post)
    stem = mesh_output_path(scene, iteration, mode, post).stem
    return {
        "dir": out_dir,
        "obj": out_dir / f"{stem}_textured.obj",
        "mtl": out_dir / f"{stem}_textured.mtl",
        "png": out_dir / f"{stem}_texture.png",
        "zip": out_dir / f"{stem}_textured.zip",
        "glb": out_dir / f"{stem}_textured.glb",
    }


def mesh_texture_file_url(scene, iteration, mode="bounded", kind="zip", post=True):
    return (
        f"/api/mesh/texture/file?scene={safe_name(scene)}&iteration={int(iteration)}"
        f"&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}&kind={safe_texture_kind(kind)}"
    )


def mesh_texture_chunk_manifest_url(scene, iteration, mode="bounded", post=True, max_faces=MESH_CHUNK_MAX_FACES):
    return (
        f"/api/mesh/texture/chunks?scene={safe_name(scene)}&iteration={int(iteration)}"
        f"&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}&max_faces={int(max_faces)}"
    )


def mesh_texture_chunk_file_url(scene, iteration, mode, post, key, index):
    return (
        f"/api/mesh/texture/chunk_file?scene={safe_name(scene)}&iteration={int(iteration)}"
        f"&mode={safe_mesh_mode(mode)}&post={'true' if post else 'false'}"
        f"&key={safe_filename(key)}&index={int(index)}"
    )


def safe_texture_kind(kind):
    kind = (kind or "zip").lower()
    if kind not in {"obj", "mtl", "png", "zip", "glb"}:
        raise ValueError("Invalid texture file kind")
    return kind


def mesh_texture_asset_payload(scene, iteration, mode="bounded", post=True):
    paths = mesh_texture_paths(scene, iteration, mode, post)
    textured_ply = paths["dir"] / "colmap_texturer" / "output" / "mesh.ply"
    textured_texture = paths["dir"] / "colmap_texturer" / "output" / "texture.png"
    files = {}
    for kind in ("obj", "mtl", "png", "zip", "glb"):
        path = paths[kind]
        files[kind] = {
            "exists": path.exists(),
            "size": path.stat().st_size if path.exists() else 0,
            "path": str(path),
            "url": mesh_texture_file_url(scene, iteration, mode, kind, post),
        }
    return {
        "exists": files["zip"]["exists"],
        "dir": str(paths["dir"]),
        "files": files,
        "chunks_url": mesh_texture_chunk_manifest_url(scene, iteration, mode, post),
        "chunk_max_faces": MESH_CHUNK_MAX_FACES,
        "chunk_source_exists": textured_ply.exists() and textured_texture.exists(),
        "chunk_source": str(textured_ply),
    }


def ply_binary_header_info(path):
    path = Path(path)
    with path.open("rb") as f:
        header_bytes = bytearray()
        while True:
            line = f.readline()
            if not line:
                raise ValueError(f"Invalid PLY: missing end_header in {path}")
            header_bytes.extend(line)
            if line.strip() == b"end_header":
                break
        data_offset = f.tell()
    header = header_bytes.decode("ascii", errors="replace")
    lines = header.splitlines()
    format_line = next((line for line in lines if line.startswith("format ")), "")
    if "binary_little_endian" not in format_line:
        raise ValueError("Mesh preview currently supports binary_little_endian PLY files")
    elements = {}
    vertex_properties = []
    face_list = None
    element = None
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "element":
            element = parts[1]
            elements[element] = int(parts[2])
        elif parts[0] == "property" and element == "vertex" and len(parts) >= 3:
            vertex_properties.append((parts[1], parts[2]))
        elif parts[0] == "property" and element == "face" and len(parts) >= 5 and parts[1] == "list":
            face_list = (parts[2], parts[3], parts[4])
    if "vertex" not in elements or "face" not in elements:
        raise ValueError("Mesh preview requires vertex and face elements")
    if face_list != ("uchar", "uint", "vertex_indices"):
        raise ValueError("Mesh preview requires face property list uchar uint vertex_indices")
    return {
        "data_offset": data_offset,
        "vertex_count": elements["vertex"],
        "face_count": elements["face"],
        "vertex_properties": vertex_properties,
    }


def scalar_struct_format(ply_type):
    return {
        "char": "b",
        "int8": "b",
        "uchar": "B",
        "uint8": "B",
        "short": "h",
        "int16": "h",
        "ushort": "H",
        "uint16": "H",
        "int": "i",
        "int32": "i",
        "uint": "I",
        "uint32": "I",
        "float": "f",
        "float32": "f",
        "double": "d",
        "float64": "d",
    }.get(ply_type, "f")


def scalar_byte_size(ply_type):
    return struct.calcsize("<" + scalar_struct_format(ply_type))


def mesh_preview_path(scene, iteration, mode="bounded", post=True, max_faces=MESH_PREVIEW_MAX_FACES):
    source = mesh_output_path(scene, iteration, mode, post)
    if not source.exists():
        raise FileNotFoundError(source)
    max_faces = max(1, int(max_faces or MESH_PREVIEW_MAX_FACES))
    cache_dir = source.parent / ".mesh_preview"
    cache_dir.mkdir(parents=True, exist_ok=True)
    stat = source.stat()
    preview = cache_dir / f"{source.stem}_{stat.st_mtime_ns}_{stat.st_size}_{max_faces}.ply"
    if preview.exists():
        return preview
    return write_mesh_preview(source, preview, max_faces)


def write_mesh_preview(source, preview, max_faces):
    info = ply_binary_header_info(source)
    vertex_count = info["vertex_count"]
    face_count = info["face_count"]
    vertex_properties = info["vertex_properties"]
    vertex_stride = sum(scalar_byte_size(prop_type) for prop_type, _ in vertex_properties)
    offsets = {}
    cursor = 0
    for prop_type, name in vertex_properties:
        offsets[name] = (cursor, prop_type)
        cursor += scalar_byte_size(prop_type)
    for name in ("x", "y", "z"):
        if name not in offsets:
            raise ValueError("Mesh preview requires x/y/z vertex properties")
    has_rgb = all(name in offsets for name in ("red", "green", "blue"))
    stride = max(1, int(np.ceil(face_count / max_faces)))
    sampled_faces = []
    used_vertices = set()
    face_offset = info["data_offset"] + vertex_count * vertex_stride
    with source.open("rb") as f:
        f.seek(face_offset)
        for face_index in range(face_count):
            raw_n = f.read(1)
            if not raw_n:
                break
            n = raw_n[0]
            indices_raw = f.read(4 * n)
            if face_index % stride != 0 or n < 3:
                continue
            indices = list(struct.unpack("<" + "I" * n, indices_raw))
            for tri_index in range(1, n - 1):
                tri = (indices[0], indices[tri_index], indices[tri_index + 1])
                sampled_faces.append(tri)
                used_vertices.update(tri)
                if len(sampled_faces) >= max_faces:
                    break
            if len(sampled_faces) >= max_faces:
                break
    ordered_vertices = sorted(index for index in used_vertices if 0 <= index < vertex_count)
    remap = {old: new for new, old in enumerate(ordered_vertices)}
    vertex_rows = []

    def read_vertex_value(record, name):
        offset, prop_type = offsets[name]
        return struct.unpack_from("<" + scalar_struct_format(prop_type), record, offset)[0]

    used_lookup = set(ordered_vertices)
    next_needed = 0
    with source.open("rb") as f:
        f.seek(info["data_offset"])
        for vertex_index in range(vertex_count):
            record = f.read(vertex_stride)
            if vertex_index not in used_lookup:
                continue
            x = read_vertex_value(record, "x")
            y = read_vertex_value(record, "y")
            z = read_vertex_value(record, "z")
            if has_rgb:
                r = int(read_vertex_value(record, "red"))
                g = int(read_vertex_value(record, "green"))
                b = int(read_vertex_value(record, "blue"))
                vertex_rows.append((x, y, z, r, g, b))
            else:
                vertex_rows.append((x, y, z))
            next_needed += 1
            if next_needed >= len(ordered_vertices):
                break
    remapped_faces = [
        tuple(remap[index] for index in tri)
        for tri in sampled_faces
        if all(index in remap for index in tri)
    ]
    lines = [
        "ply",
        "format ascii 1.0",
        f"comment preview of {source.name}; full-resolution mesh is used for download and trim",
        f"element vertex {len(vertex_rows)}",
        "property float x",
        "property float y",
        "property float z",
    ]
    if has_rgb:
        lines.extend(["property uchar red", "property uchar green", "property uchar blue"])
    lines.extend([
        f"element face {len(remapped_faces)}",
        "property list uchar int vertex_indices",
        "end_header",
    ])
    for row in vertex_rows:
        lines.append(" ".join(str(value) for value in row))
    for a, b, c in remapped_faces:
        lines.append(f"3 {a} {b} {c}")
    preview.write_text("\n".join(lines) + "\n", encoding="ascii")
    return preview


def mesh_chunk_cache_paths(source, max_faces):
    source = Path(source)
    stat = source.stat()
    key = f"{source.stem}_{stat.st_mtime_ns}_{stat.st_size}_{int(max_faces)}"
    directory = source.parent / ".mesh_chunks" / key
    return key, directory, directory / "manifest.json"


def write_mesh_chunk(chunk_path, positions, colors, triangles, remap, source_has_colors):
    vertex_count = len(remap)
    face_count = len(triangles)
    out_positions = np.empty(vertex_count * 3, dtype=np.float32)
    out_colors = np.empty(vertex_count * 3, dtype=np.uint8) if source_has_colors else None
    for old_index, new_index in remap.items():
        out_positions[new_index * 3:new_index * 3 + 3] = positions[old_index * 3:old_index * 3 + 3]
        if out_colors is not None:
            out_colors[new_index * 3:new_index * 3 + 3] = colors[old_index * 3:old_index * 3 + 3]
    indices = np.asarray(triangles, dtype=np.uint32).reshape(face_count * 3)
    header = {
        "format": "3dgs-editor-mesh-chunk-v1",
        "vertex_count": vertex_count,
        "face_count": face_count,
        "has_vertex_colors": source_has_colors,
    }
    header_bytes = json.dumps(header, separators=(",", ":")).encode("utf-8")
    header_padding = (-4 - len(header_bytes)) % 4
    with chunk_path.open("wb") as f:
        f.write(struct.pack("<I", len(header_bytes)))
        f.write(header_bytes)
        if header_padding:
            f.write(b"\0" * header_padding)
        f.write(out_positions.tobytes())
        if out_colors is not None:
            f.write(out_colors.tobytes())
        f.write(indices.tobytes())
    return {
        "file": chunk_path.name,
        "path": str(chunk_path),
        "vertex_count": vertex_count,
        "face_count": face_count,
        "size": chunk_path.stat().st_size,
    }


def build_mesh_chunk_cache(source, cache_dir, manifest_path, max_faces):
    info = ply_binary_header_info(source)
    vertex_count = info["vertex_count"]
    face_count = info["face_count"]
    vertex_properties = info["vertex_properties"]
    vertex_stride = sum(scalar_byte_size(prop_type) for prop_type, _ in vertex_properties)
    offsets = {}
    cursor = 0
    for prop_type, name in vertex_properties:
        offsets[name] = (cursor, prop_type)
        cursor += scalar_byte_size(prop_type)
    for name in ("x", "y", "z"):
        if name not in offsets:
            raise ValueError("Mesh chunks require x/y/z vertex properties")
    has_rgb = all(name in offsets for name in ("red", "green", "blue"))
    positions = np.empty(vertex_count * 3, dtype=np.float32)
    colors = np.empty(vertex_count * 3, dtype=np.uint8) if has_rgb else None

    def read_vertex_value(record, name):
        offset, prop_type = offsets[name]
        return struct.unpack_from("<" + scalar_struct_format(prop_type), record, offset)[0]

    with source.open("rb") as f:
        f.seek(info["data_offset"])
        for vertex_index in range(vertex_count):
            record = f.read(vertex_stride)
            base = vertex_index * 3
            positions[base] = read_vertex_value(record, "x")
            positions[base + 1] = read_vertex_value(record, "y")
            positions[base + 2] = read_vertex_value(record, "z")
            if colors is not None:
                colors[base] = int(read_vertex_value(record, "red"))
                colors[base + 1] = int(read_vertex_value(record, "green"))
                colors[base + 2] = int(read_vertex_value(record, "blue"))

    cache_dir.mkdir(parents=True, exist_ok=True)
    chunks = []
    triangles = []
    remap = {}

    def mapped(index):
        index = int(index)
        if index not in remap:
            remap[index] = len(remap)
        return remap[index]

    def flush_chunk():
        nonlocal triangles, remap
        if not triangles:
            return
        chunk_path = cache_dir / f"chunk_{len(chunks):05d}.bin"
        chunks.append(write_mesh_chunk(chunk_path, positions, colors, triangles, remap, has_rgb))
        triangles = []
        remap = {}

    face_offset = info["data_offset"] + vertex_count * vertex_stride
    with source.open("rb") as f:
        f.seek(face_offset)
        for _ in range(face_count):
            raw_n = f.read(1)
            if not raw_n:
                break
            n = raw_n[0]
            raw_indices = f.read(4 * n)
            if n < 3:
                continue
            indices = struct.unpack("<" + "I" * n, raw_indices)
            for tri_index in range(1, n - 1):
                tri = (
                    mapped(indices[0]),
                    mapped(indices[tri_index]),
                    mapped(indices[tri_index + 1]),
                )
                triangles.append(tri)
                if len(triangles) >= max_faces:
                    flush_chunk()
        flush_chunk()

    manifest = {
        "format": "3dgs-editor-mesh-chunks-v1",
        "source": str(source),
        "vertex_count": vertex_count,
        "face_count": face_count,
        "has_vertex_colors": has_rgb,
        "max_faces": max_faces,
        "chunks": chunks,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def mesh_chunk_manifest(scene, iteration, mode="bounded", post=True, max_faces=MESH_CHUNK_MAX_FACES):
    scene = safe_name(scene)
    iteration = int(iteration)
    mode = safe_mesh_mode(mode)
    max_faces = max(1, int(max_faces or MESH_CHUNK_MAX_FACES))
    source = mesh_output_path(scene, iteration, mode, post)
    if not source.exists():
        raise FileNotFoundError(source)
    key, cache_dir, manifest_path = mesh_chunk_cache_paths(source, max_faces)
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        manifest = build_mesh_chunk_cache(source, cache_dir, manifest_path, max_faces)
    for index, chunk in enumerate(manifest["chunks"]):
        chunk["path"] = str(cache_dir / chunk["file"])
        chunk["url"] = mesh_chunk_file_url(scene, iteration, mode, post, key, index)
        chunk["index"] = index
    manifest["key"] = key
    manifest["chunk_count"] = len(manifest["chunks"])
    return manifest


def textured_mesh_source_paths(scene, iteration, mode="bounded", post=True):
    paths = mesh_texture_paths(scene, iteration, mode, post)
    texturer_output = paths["dir"] / "colmap_texturer" / "output"
    return texturer_output / "mesh.ply", texturer_output / "texture.png"


def textured_mesh_chunk_cache_paths(source, max_faces):
    source = Path(source)
    stat = source.stat()
    key = f"{source.stem}_textured_{stat.st_mtime_ns}_{stat.st_size}_{int(max_faces)}"
    directory = source.parent / ".textured_mesh_chunks" / key
    return key, directory, directory / "manifest.json"


def ply_binary_mesh_layout(path):
    path = Path(path)
    with path.open("rb") as f:
        header_bytes = bytearray()
        while True:
            line = f.readline()
            if not line:
                raise ValueError(f"Invalid PLY: missing end_header in {path}")
            header_bytes.extend(line)
            if line.strip() == b"end_header":
                break
        data_offset = f.tell()
    header = header_bytes.decode("ascii", errors="replace")
    lines = header.splitlines()
    format_line = next((line for line in lines if line.startswith("format ")), "")
    if "binary_little_endian" not in format_line:
        raise ValueError("Textured mesh chunks require binary_little_endian PLY files")
    elements = {}
    vertex_properties = []
    face_properties = []
    element = None
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "element":
            element = parts[1]
            elements[element] = int(parts[2])
        elif parts[0] == "property" and element == "vertex" and len(parts) >= 3:
            vertex_properties.append(("scalar", parts[1], parts[2]))
        elif parts[0] == "property" and element == "face":
            if len(parts) >= 5 and parts[1] == "list":
                face_properties.append(("list", parts[2], parts[3], parts[4]))
            elif len(parts) >= 3:
                face_properties.append(("scalar", parts[1], parts[2]))
    if "vertex" not in elements or "face" not in elements:
        raise ValueError("Textured mesh chunks require vertex and face elements")
    return {
        "data_offset": data_offset,
        "vertex_count": elements["vertex"],
        "face_count": elements["face"],
        "vertex_properties": vertex_properties,
        "face_properties": face_properties,
    }


def write_textured_mesh_chunk(chunk_path, positions, triangles, remap):
    vertex_count = len(remap)
    face_count = len(triangles)
    out_positions = np.empty(vertex_count * 3, dtype=np.float32)
    out_uvs = np.empty(vertex_count * 2, dtype=np.float32)
    for key, new_index in remap.items():
        old_index, u, v = key
        out_positions[new_index * 3:new_index * 3 + 3] = positions[old_index * 3:old_index * 3 + 3]
        out_uvs[new_index * 2:new_index * 2 + 2] = (u, v)
    indices = np.asarray(triangles, dtype=np.uint32).reshape(face_count * 3)
    header = {
        "format": "3dgs-editor-textured-mesh-chunk-v1",
        "vertex_count": vertex_count,
        "face_count": face_count,
        "has_uvs": True,
    }
    header_bytes = json.dumps(header, separators=(",", ":")).encode("utf-8")
    header_padding = (-4 - len(header_bytes)) % 4
    with chunk_path.open("wb") as f:
        f.write(struct.pack("<I", len(header_bytes)))
        f.write(header_bytes)
        if header_padding:
            f.write(b"\0" * header_padding)
        f.write(out_positions.tobytes())
        f.write(out_uvs.tobytes())
        f.write(indices.tobytes())
    return {
        "file": chunk_path.name,
        "path": str(chunk_path),
        "vertex_count": vertex_count,
        "face_count": face_count,
        "size": chunk_path.stat().st_size,
    }


def build_textured_mesh_chunk_cache(source, texture_path, cache_dir, manifest_path, max_faces):
    info = ply_binary_mesh_layout(source)
    vertex_count = info["vertex_count"]
    face_count = info["face_count"]
    vertex_properties = info["vertex_properties"]
    face_properties = info["face_properties"]
    vertex_stride = sum(scalar_byte_size(prop_type) for _, prop_type, _ in vertex_properties)
    offsets = {}
    cursor = 0
    for _, prop_type, name in vertex_properties:
        offsets[name] = (cursor, prop_type)
        cursor += scalar_byte_size(prop_type)
    for name in ("x", "y", "z"):
        if name not in offsets:
            raise ValueError("Textured mesh chunks require x/y/z vertex properties")
    if not any(prop[0] == "list" and prop[3] == "vertex_indices" for prop in face_properties):
        raise ValueError("Textured mesh chunks require face vertex_indices")
    if not any(prop[0] == "list" and prop[3] == "texcoord" for prop in face_properties):
        raise ValueError("Textured mesh chunks require face texcoord")
    positions = np.empty(vertex_count * 3, dtype=np.float32)

    def read_vertex_value(record, name):
        offset, prop_type = offsets[name]
        return struct.unpack_from("<" + scalar_struct_format(prop_type), record, offset)[0]

    with Path(source).open("rb") as f:
        f.seek(info["data_offset"])
        for vertex_index in range(vertex_count):
            record = f.read(vertex_stride)
            base = vertex_index * 3
            positions[base] = read_vertex_value(record, "x")
            positions[base + 1] = read_vertex_value(record, "y")
            positions[base + 2] = read_vertex_value(record, "z")

    cache_dir.mkdir(parents=True, exist_ok=True)
    chunks = []
    triangles = []
    remap = {}

    def mapped(vertex_index, u, v):
        key = (int(vertex_index), float(u), float(v))
        if key not in remap:
            remap[key] = len(remap)
        return remap[key]

    def flush_chunk():
        nonlocal triangles, remap
        if not triangles:
            return
        chunk_path = cache_dir / f"chunk_{len(chunks):05d}.bin"
        chunks.append(write_textured_mesh_chunk(chunk_path, positions, triangles, remap))
        triangles = []
        remap = {}

    face_offset = info["data_offset"] + vertex_count * vertex_stride
    with Path(source).open("rb") as f:
        f.seek(face_offset)
        for _ in range(face_count):
            vertex_indices = None
            texcoords = None
            for prop in face_properties:
                if prop[0] == "scalar":
                    _, prop_type, _ = prop
                    f.read(scalar_byte_size(prop_type))
                    continue
                _, count_type, item_type, name = prop
                raw_count = f.read(scalar_byte_size(count_type))
                if not raw_count:
                    break
                count = struct.unpack("<" + scalar_struct_format(count_type), raw_count)[0]
                raw_values = f.read(scalar_byte_size(item_type) * count)
                values = struct.unpack("<" + scalar_struct_format(item_type) * count, raw_values) if count else ()
                if name == "vertex_indices":
                    vertex_indices = values
                elif name == "texcoord":
                    texcoords = values
            if not vertex_indices or len(vertex_indices) < 3 or not texcoords or len(texcoords) < len(vertex_indices) * 2:
                continue
            for tri_index in range(1, len(vertex_indices) - 1):
                corners = (0, tri_index, tri_index + 1)
                tri = tuple(
                    mapped(vertex_indices[corner], texcoords[corner * 2], texcoords[corner * 2 + 1])
                    for corner in corners
                )
                triangles.append(tri)
                if len(triangles) >= max_faces:
                    flush_chunk()
        flush_chunk()

    manifest = {
        "format": "3dgs-editor-textured-mesh-chunks-v1",
        "source": str(source),
        "texture": str(texture_path),
        "vertex_count": vertex_count,
        "face_count": face_count,
        "max_faces": max_faces,
        "chunks": chunks,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def textured_mesh_chunk_manifest(scene, iteration, mode="bounded", post=True, max_faces=MESH_CHUNK_MAX_FACES):
    scene = safe_name(scene)
    iteration = int(iteration)
    mode = safe_mesh_mode(mode)
    max_faces = max(1, int(max_faces or MESH_CHUNK_MAX_FACES))
    source, texture = textured_mesh_source_paths(scene, iteration, mode, post)
    if not source.exists():
        raise FileNotFoundError(f"Textured COLMAP PLY not found: {source}")
    if not texture.exists():
        raise FileNotFoundError(f"Textured COLMAP texture not found: {texture}")
    key, cache_dir, manifest_path = textured_mesh_chunk_cache_paths(source, max_faces)
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        manifest = build_textured_mesh_chunk_cache(source, texture, cache_dir, manifest_path, max_faces)
    for index, chunk in enumerate(manifest["chunks"]):
        chunk["path"] = str(cache_dir / chunk["file"])
        chunk["url"] = mesh_texture_chunk_file_url(scene, iteration, mode, post, key, index)
        chunk["index"] = index
    manifest["key"] = key
    manifest["chunk_count"] = len(manifest["chunks"])
    manifest["texture_url"] = mesh_texture_file_url(scene, iteration, mode, "png", post)
    return manifest


def mesh_asset_payload(scene, iteration):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    meshes = {}
    for mode in ("bounded", "unbounded", "sugar", "gs2mesh"):
        meshes[mode] = {}
        for post in (False, True):
            path = mesh_output_path(scene, iteration, mode, post)
            key = "post" if post else "raw"
            meshes[mode][key] = {
                "exists": path.exists(),
                "size": path.stat().st_size if path.exists() else 0,
                "path": str(path),
                "url": mesh_file_url(scene, iteration, mode, post),
                "preview_url": mesh_preview_file_url(scene, iteration, mode, post),
                "preview_max_faces": MESH_PREVIEW_MAX_FACES,
                "chunks_url": mesh_chunk_manifest_url(scene, iteration, mode, post),
                "chunk_max_faces": MESH_CHUNK_MAX_FACES,
            }
        meshes[mode]["texture"] = mesh_texture_asset_payload(scene, iteration, mode, post=True)
    return {"scene": scene, "iteration": iteration, "meshes": meshes}


def mesh_export_options(options):
    options = options or {}
    mode = safe_mesh_mode(options.get("mode", "bounded"))
    mesh_res = int(options.get("mesh_res", 512 if mode == "bounded" else 1024))
    num_cluster = int(options.get("num_cluster", 50))
    depth_ratio = float(options.get("depth_ratio", 0.0))
    if mode not in {"sugar", "gs2mesh"} and (mesh_res < 64 or mesh_res > 4096):
        raise ValueError("mesh_res must be between 64 and 4096")
    if num_cluster < 1 or num_cluster > 500:
        raise ValueError("num_cluster must be between 1 and 500")
    sugar_quality = (options.get("sugar_quality") or "high").lower()
    if sugar_quality not in {"low", "high"}:
        sugar_quality = "high"
    sugar_regularization = (options.get("sugar_regularization") or "dn_consistency").lower()
    if sugar_regularization not in {"dn_consistency", "density", "sdf"}:
        sugar_regularization = "dn_consistency"
    sugar_refinement_time = (options.get("sugar_refinement_time") or "medium").lower()
    if sugar_refinement_time not in {"short", "medium", "long"}:
        sugar_refinement_time = "medium"
    sugar_surface_level = float(options.get("sugar_surface_level", 0.3))
    if sugar_surface_level < 0.05 or sugar_surface_level > 1.0:
        raise ValueError("sugar_surface_level must be between 0.05 and 1.0")
    sugar_square_size = int(options.get("sugar_square_size", 10))
    if sugar_square_size < 4 or sugar_square_size > 32:
        raise ValueError("sugar_square_size must be between 4 and 32")
    sugar_max_images = int(options.get("sugar_max_images", 0))
    if sugar_max_images < 0 or sugar_max_images > 2000:
        raise ValueError("sugar_max_images must be between 0 and 2000")
    sugar_max_image_size = int(options.get("sugar_max_image_size", 1920))
    if sugar_max_image_size < 256 or sugar_max_image_size > 4096:
        raise ValueError("sugar_max_image_size must be between 256 and 4096")
    gs2mesh_downsample = int(options.get("gs2mesh_downsample", 2))
    if gs2mesh_downsample < 1 or gs2mesh_downsample > 8:
        raise ValueError("gs2mesh_downsample must be between 1 and 8")
    gs2mesh_baseline_percentage = float(options.get("gs2mesh_baseline_percentage", 7.0))
    if gs2mesh_baseline_percentage <= 0 or gs2mesh_baseline_percentage > 50:
        raise ValueError("gs2mesh_baseline_percentage must be between 0 and 50")
    gs2mesh_tsdf_voxel = int(options.get("gs2mesh_tsdf_voxel", 2))
    if gs2mesh_tsdf_voxel < 1 or gs2mesh_tsdf_voxel > 8:
        raise ValueError("gs2mesh_tsdf_voxel must be between 1 and 8")
    gs2mesh_tsdf_min_depth_baselines = int(options.get("gs2mesh_tsdf_min_depth_baselines", 4))
    if gs2mesh_tsdf_min_depth_baselines < 1 or gs2mesh_tsdf_min_depth_baselines > 100:
        raise ValueError("gs2mesh_tsdf_min_depth_baselines must be between 1 and 100")
    gs2mesh_tsdf_max_depth_baselines = int(options.get("gs2mesh_tsdf_max_depth_baselines", 20))
    if gs2mesh_tsdf_max_depth_baselines < gs2mesh_tsdf_min_depth_baselines or gs2mesh_tsdf_max_depth_baselines > 200:
        raise ValueError("gs2mesh_tsdf_max_depth_baselines must be greater than min and no more than 200")
    gs2mesh_tsdf_cleaning_threshold = int(options.get("gs2mesh_tsdf_cleaning_threshold", 100000))
    if gs2mesh_tsdf_cleaning_threshold < 0 or gs2mesh_tsdf_cleaning_threshold > 10000000:
        raise ValueError("gs2mesh_tsdf_cleaning_threshold must be between 0 and 10000000")
    return {
        "mode": mode,
        "mesh_res": mesh_res,
        "num_cluster": num_cluster,
        "depth_ratio": depth_ratio,
        "voxel_size": float(options.get("voxel_size", -1.0)),
        "depth_trunc": float(options.get("depth_trunc", -1.0)),
        "sdf_trunc": float(options.get("sdf_trunc", -1.0)),
        "sugar_quality": sugar_quality,
        "sugar_regularization": sugar_regularization,
        "sugar_refinement_time": sugar_refinement_time,
        "sugar_surface_level": sugar_surface_level,
        "sugar_square_size": sugar_square_size,
        "sugar_max_images": sugar_max_images,
        "sugar_max_image_size": sugar_max_image_size,
        "sugar_postprocess": safe_bool(options.get("sugar_postprocess"), False),
        "gs2mesh_downsample": gs2mesh_downsample,
        "gs2mesh_baseline_percentage": gs2mesh_baseline_percentage,
        "gs2mesh_scene_360": safe_bool(options.get("gs2mesh_scene_360"), True),
        "gs2mesh_tsdf_voxel": gs2mesh_tsdf_voxel,
        "gs2mesh_tsdf_min_depth_baselines": gs2mesh_tsdf_min_depth_baselines,
        "gs2mesh_tsdf_max_depth_baselines": gs2mesh_tsdf_max_depth_baselines,
        "gs2mesh_tsdf_cleaning_threshold": gs2mesh_tsdf_cleaning_threshold,
    }


def posix_dir_with_trailing_slash(path):
    text = Path(path).as_posix()
    return text if text.endswith("/") else f"{text}/"


def link_or_copy_file(source, target):
    source = Path(source)
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() or target.is_symlink():
        target.unlink()
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def copytree_replace(source, target):
    source = Path(source)
    target = Path(target)
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)


def remove_tree_inside(root, target):
    root = Path(root).resolve()
    target = Path(target).resolve()
    if not str(target).lower().startswith(str(root).lower()):
        raise RuntimeError(f"Refusing to remove path outside staging root: {target}")
    if target.exists():
        shutil.rmtree(target)


def link_or_copy_tree(source, target):
    source = Path(source)
    target = Path(target)
    remove_tree_inside(target.parent, target)
    target.mkdir(parents=True, exist_ok=True)
    for item in source.rglob("*"):
        relative = item.relative_to(source)
        dst = target / relative
        if item.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
        elif item.is_file():
            link_or_copy_file(item, dst)


CAMERA_PARAM_SCALE_INDICES = {
    "SIMPLE_PINHOLE": (0, 1, 2),
    "PINHOLE": (0, 1, 2, 3),
    "SIMPLE_RADIAL": (0, 1, 2),
    "RADIAL": (0, 1, 2),
    "OPENCV": (0, 1, 2, 3),
    "OPENCV_FISHEYE": (0, 1, 2, 3),
    "FULL_OPENCV": (0, 1, 2, 3),
    "FOV": (0, 1, 2, 3),
    "SIMPLE_RADIAL_FISHEYE": (0, 1, 2),
    "RADIAL_FISHEYE": (0, 1, 2),
    "THIN_PRISM_FISHEYE": (0, 1, 2, 3),
}


def scaled_colmap_cameras_text(cameras_text, downsample):
    factor = float(downsample)
    if factor <= 1:
        return cameras_text
    lines = []
    for line in cameras_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            lines.append(line)
            continue
        parts = stripped.split()
        if len(parts) < 5:
            lines.append(line)
            continue
        try:
            model = parts[1].upper()
            width = max(1, round(float(parts[2]) / factor))
            height = max(1, round(float(parts[3]) / factor))
            params = [float(value) for value in parts[4:]]
        except ValueError:
            lines.append(line)
            continue
        for index in CAMERA_PARAM_SCALE_INDICES.get(model, (0, 1, 2, 3)):
            if index < len(params):
                params[index] /= factor
        params_text = " ".join(f"{value:.17g}" for value in params)
        lines.append(f"{parts[0]} {parts[1]} {width} {height} {params_text}")
    return "\n".join(lines) + ("\n" if cameras_text.endswith("\n") else "")


def prepare_gs2mesh_downsampled_sparse(dataset_dir, downsample):
    downsample = int(downsample or 1)
    if downsample <= 1:
        return None
    source_sparse = Path(dataset_dir) / "sparse" / "0"
    downsampled_dir = Path(f"{os.path.normpath(str(dataset_dir))}_downsample{downsample}")
    target_sparse = downsampled_dir / "sparse" / "0"
    target_sparse.mkdir(parents=True, exist_ok=True)
    for name in ("images.txt", "points3D.txt"):
        source = source_sparse / name
        if source.exists():
            shutil.copy2(source, target_sparse / name)
    cameras = source_sparse / "cameras.txt"
    if cameras.exists():
        scaled = scaled_colmap_cameras_text(cameras.read_text(encoding="utf-8"), downsample)
        (target_sparse / "cameras.txt").write_text(scaled, encoding="utf-8")
    for source in source_sparse.iterdir():
        if source.is_file() and source.suffix.lower() == ".bin":
            shutil.copy2(source, target_sparse / source.name)
    return downsampled_dir


def gs2mesh_float_token(value):
    return str(float(value)).replace(".", "_")


def gs2mesh_runtime_colmap_name(scene, downsample):
    downsample = int(downsample or 1)
    return f"{scene}_downsample{downsample}" if downsample > 1 else scene


def gs2mesh_dataset_string(iteration, baseline_percentage):
    return f"custom_nw_iterations{int(iteration)}_DLNR_Middlebury_baseline{gs2mesh_float_token(baseline_percentage)}p"


def gs2mesh_renderer_folder(scene, downsample):
    downsample = int(downsample or 1)
    return f"{scene}_d{downsample}" if downsample > 1 else scene


def gs2mesh_output_dirs(scene, iteration, options):
    runtime_scene = gs2mesh_runtime_colmap_name(scene, options["gs2mesh_downsample"])
    dataset_name = gs2mesh_dataset_string(iteration, options["gs2mesh_baseline_percentage"])
    legacy = GS2MESH_DIR / "output" / dataset_name / runtime_scene
    short = GS2MESH_DIR / "output" / "app" / gs2mesh_renderer_folder(scene, options["gs2mesh_downsample"])
    return legacy, short


def count_colmap_images(images_txt):
    count = 0
    data_line_index = 0
    with open(images_txt, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if data_line_index % 2 == 0:
                count += 1
            data_line_index += 1
    return count


def gs2mesh_cache_complete(output_dir, colmap_dir):
    output_dir = Path(output_dir)
    images_txt = Path(colmap_dir) / "sparse" / "0" / "images.txt"
    if not output_dir.exists() or not images_txt.exists():
        return False
    expected = count_colmap_images(images_txt)
    if expected <= 0:
        return False
    for index in range(expected):
        frame_dir = output_dir / f"{index:03d}"
        if not (frame_dir / "left.png").exists():
            return False
        stereo_dir = frame_dir / "out_DLNR_Middlebury"
        if not (stereo_dir / "depth.npy").exists() or not (stereo_dir / "occlusion_mask.npy").exists():
            return False
    return True


def prepare_gs2mesh_render_cache(scene, iteration, options):
    legacy, short = gs2mesh_output_dirs(scene, iteration, options)
    runtime_scene = gs2mesh_runtime_colmap_name(scene, options["gs2mesh_downsample"])
    runtime_colmap_dir = GS2MESH_DIR / "data" / "custom" / runtime_scene
    if gs2mesh_cache_complete(short, runtime_colmap_dir):
        return True
    if gs2mesh_cache_complete(legacy, runtime_colmap_dir):
        link_or_copy_tree(legacy, short)
        return True
    return False


def sugar_camera_name(name):
    path = PurePosixPath(str(name).replace("\\", "/"))
    if path.suffix.lower() in IMAGE_EXTS:
        return str(path.with_suffix(""))
    return str(name)


def source_image_path(images_dir, img_name):
    images_dir = Path(images_dir)
    raw = PurePosixPath(str(img_name).replace("\\", "/")).name
    raw_path = images_dir / raw
    if raw_path.exists():
        return raw_path
    stem = PurePosixPath(raw).stem
    for ext in IMAGE_EXTS:
        candidate = images_dir / f"{stem}{ext}"
        if candidate.exists():
            return candidate
        candidate = images_dir / f"{stem}{ext.upper()}"
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"Missing source image for SuGaR: {images_dir / raw}")


def sugar_target_image_size(source_path, cameras, max_img_size=1920):
    from PIL import Image

    images_dir = Path(source_path) / "images"
    first = next((camera for camera in cameras if isinstance(camera, dict) and camera.get("img_name")), None)
    if first is None:
        raise ValueError("Missing img_name entries in cameras.json for SuGaR")
    image_path = source_image_path(images_dir, first["img_name"])
    with Image.open(image_path) as image:
        width, height = image.size
    scale = max(width, height) / float(max_img_size) if max(width, height) > max_img_size else 1.0
    return max(1, round(width / scale)), max(1, round(height / scale))


def sampled_sugar_cameras(cameras, max_images):
    valid = [camera for camera in cameras if isinstance(camera, dict) and camera.get("img_name")]
    if max_images <= 0 or len(valid) <= max_images:
        return list(cameras)
    if max_images == 1:
        keep_ids = {id(valid[len(valid) // 2])}
    else:
        keep_ids = {
            id(valid[round(index * (len(valid) - 1) / (max_images - 1))])
            for index in range(max_images)
        }
    return [
        camera for camera in cameras
        if not isinstance(camera, dict) or not camera.get("img_name") or id(camera) in keep_ids
    ]


def prepare_sugar_source_dataset(source_path, model_path, cameras, max_images=0, max_image_size=1920):
    from PIL import Image

    source_path = Path(source_path)
    model_path = Path(model_path)
    cameras = sampled_sugar_cameras(cameras, int(max_images or 0))
    target_w, target_h = sugar_target_image_size(source_path, cameras, int(max_image_size or 1920))
    staged_source = model_path / "_sugar_source" / model_path.name
    staged_images = staged_source / "images"
    staged_images.mkdir(parents=True, exist_ok=True)
    source_images = source_path / "images"
    adapted = []
    for camera in cameras:
        if not isinstance(camera, dict):
            adapted.append(camera)
            continue
        item = dict(camera)
        original_name = item.get("img_name", "")
        if original_name:
            item["img_name"] = sugar_camera_name(original_name)
            src = source_image_path(source_images, original_name)
            dst = staged_images / f"{item['img_name']}.jpg"
            needs_resize = True
            if dst.exists():
                try:
                    with Image.open(dst) as existing:
                        needs_resize = existing.size != (target_w, target_h)
                except Exception:
                    needs_resize = True
            if needs_resize:
                with Image.open(src) as image:
                    resampling = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
                    resized = image.convert("RGB").resize((target_w, target_h), resampling)
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    resized.save(dst, quality=95, subsampling=0)
        orig_w = float(item.get("width") or target_w)
        orig_h = float(item.get("height") or target_h)
        if orig_w > 0 and orig_h > 0:
            item["width"] = target_w
            item["height"] = target_h
            if "fx" in item:
                item["fx"] = float(item["fx"]) * target_w / orig_w
            if "fy" in item:
                item["fy"] = float(item["fy"]) * target_h / orig_h
        adapted.append(item)
    sparse = source_path / "sparse"
    if sparse.exists():
        copytree_replace(sparse, staged_source / "sparse")
    return staged_source, adapted


def prepare_sugar_checkpoint(model_path, source_path, iteration, options=None):
    options = options or {}
    model_path = Path(model_path)
    source_path = Path(source_path)
    iteration = int(iteration)
    stage = model_path / "_sugar_input"
    stage.mkdir(parents=True, exist_ok=True)
    cameras_path = model_path / "cameras.json"
    if not cameras_path.exists():
        raise FileNotFoundError(f"Missing cameras.json for SuGaR: {cameras_path}")
    cameras = json.loads(cameras_path.read_text(encoding="utf-8"))
    if not isinstance(cameras, list):
        raise ValueError(f"Invalid cameras.json for SuGaR: {cameras_path}")
    staged_source, adapted = prepare_sugar_source_dataset(
        source_path,
        model_path,
        cameras,
        options.get("sugar_max_images", 0),
        options.get("sugar_max_image_size", 1920),
    )
    (stage / "cameras.json").write_text(json.dumps(adapted, ensure_ascii=False), encoding="utf-8")
    for name in ("cfg_args", "scene.json", "training_backend.json", "exposure.json", "input.ply"):
        src = model_path / name
        if src.exists() and src.is_file():
            shutil.copy2(src, stage / name)
    source_ply = model_path / "point_cloud" / f"iteration_{iteration}" / "point_cloud.ply"
    target_ply = stage / "point_cloud" / f"iteration_{iteration}" / "point_cloud.ply"
    link_or_copy_file(source_ply, target_ply)
    return stage, staged_source


def colmap_executable():
    configured = os.environ.get("COLMAP_PATH")
    if configured:
        return Path(configured)
    local_candidates = [
        ROOT / "third_party" / "colmap" / "bin" / "colmap.exe",
        ROOT / "tools" / "colmap" / "bin" / "colmap.exe",
        ROOT / "colmap" / "bin" / "colmap.exe",
    ]
    for candidate in local_candidates:
        if candidate.exists():
            return candidate
    userprofile = os.environ.get("USERPROFILE", str(Path.home()))
    candidate = Path(userprofile) / "Downloads" / "colmap-x64-windows-cuda" / "bin" / "colmap.exe"
    if candidate.exists():
        return candidate
    found = shutil.which("colmap")
    return Path(found) if found else candidate


def colmap_process_env():
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    colmap = colmap_executable()
    path_parts = [str(colmap.parent), env.get("PATH", "")]
    env["PATH"] = os.pathsep.join(part for part in path_parts if part)
    return env


def convert_colmap_model_to_text(sparse_dir):
    sparse_dir = Path(sparse_dir)
    required = [sparse_dir / name for name in ("cameras.txt", "images.txt", "points3D.txt")]
    if all(path.exists() for path in required):
        return
    colmap = colmap_executable()
    if not colmap.exists():
        raise FileNotFoundError(f"Missing COLMAP executable for GS2Mesh model conversion: {colmap}")
    result = subprocess.run(
        [
            str(colmap),
            "model_converter",
            "--input_path",
            str(sparse_dir),
            "--output_path",
            str(sparse_dir),
            "--output_type",
            "TXT",
        ],
        cwd=str(sparse_dir),
        env=colmap_process_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        details = "\n".join(
            part
            for part in (
                f"return code: {result.returncode}",
                f"stdout:\n{result.stdout.strip()}" if result.stdout.strip() else "",
                f"stderr:\n{result.stderr.strip()}" if result.stderr.strip() else "",
            )
            if part
        )
        raise RuntimeError(f"COLMAP model_converter failed for GS2Mesh:\n{details}")
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise RuntimeError(f"COLMAP model_converter finished but did not create: {', '.join(missing)}")


def prepare_gs2mesh_dataset(scene, source_path, downsample=1):
    source_path = Path(source_path)
    dataset_dir = GS2MESH_DIR / "data" / "custom" / scene
    remove_tree_inside(GS2MESH_DIR / "data" / "custom", dataset_dir)
    dataset_dir.mkdir(parents=True, exist_ok=True)
    images_dir = source_path / "images"
    sparse_dir = source_path / "sparse"
    if not images_dir.exists():
        raise FileNotFoundError(f"Missing source images for GS2Mesh: {images_dir}")
    if not (sparse_dir / "0").exists():
        raise FileNotFoundError(f"Missing COLMAP sparse/0 for GS2Mesh: {sparse_dir / '0'}")
    link_or_copy_tree(images_dir, dataset_dir / "images")
    copytree_replace(sparse_dir, dataset_dir / "sparse")
    convert_colmap_model_to_text(dataset_dir / "sparse" / "0")
    prepare_gs2mesh_downsampled_sparse(dataset_dir, downsample)
    return dataset_dir


def stage_gs2mesh_checkpoint_scene(scene_name, model_path, iteration):
    model_path = Path(model_path)
    iteration = int(iteration)
    stage = GS2MESH_DIR / "splatting_output" / f"custom_nw_iterations{iteration}" / scene_name
    remove_tree_inside(GS2MESH_DIR / "splatting_output", stage)
    stage.mkdir(parents=True, exist_ok=True)
    for name in ("cfg_args", "cameras.json", "scene.json", "training_backend.json", "exposure.json", "input.ply"):
        src = model_path / name
        if src.exists() and src.is_file():
            shutil.copy2(src, stage / name)
    source_ply = model_path / "point_cloud" / f"iteration_{iteration}" / "point_cloud.ply"
    if not source_ply.exists():
        raise FileNotFoundError(f"Missing point cloud for GS2Mesh: {source_ply}")
    target_ply = stage / "point_cloud" / f"iteration_{iteration}" / "point_cloud.ply"
    link_or_copy_file(source_ply, target_ply)
    return stage


def prepare_gs2mesh_checkpoint(scene, model_path, iteration, downsample=1):
    stage = stage_gs2mesh_checkpoint_scene(scene, model_path, iteration)
    downsample = int(downsample or 1)
    if downsample > 1:
        stage_gs2mesh_checkpoint_scene(f"{scene}_downsample{downsample}", model_path, iteration)
    return stage


def prepare_gs2mesh_inputs(scene, model_path, source_path, iteration, downsample=1):
    dataset_dir = prepare_gs2mesh_dataset(scene, source_path, downsample)
    checkpoint_dir = prepare_gs2mesh_checkpoint(scene, model_path, iteration, downsample)
    return dataset_dir, checkpoint_dir


def find_latest_gs2mesh_cleaned_mesh(started_at):
    output_root = GS2MESH_DIR / "output"
    if not output_root.exists():
        raise FileNotFoundError(f"GS2Mesh output directory was not created: {output_root}")
    threshold = float(started_at) - 5.0
    candidates = [
        path for path in output_root.rglob("*cleaned_mesh.ply")
        if path.is_file() and path.stat().st_mtime >= threshold
    ]
    if not candidates:
        candidates = [path for path in output_root.rglob("*cleaned_mesh.ply") if path.is_file()]
    if not candidates:
        raise FileNotFoundError(f"GS2Mesh completed but no *_cleaned_mesh.ply was found under {output_root}")
    return max(candidates, key=lambda path: path.stat().st_mtime)


def collect_gs2mesh_mesh_output(scene, iteration, started_at):
    source = find_latest_gs2mesh_cleaned_mesh(started_at)
    target = mesh_output_path(scene, iteration, "gs2mesh", post=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return {"source": str(source), "mesh": str(target)}


def mesh_export_command(scene, iteration, options=None):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    backend = scene_backend(scene, iteration)
    model_path = model_dir(scene)
    opts = mesh_export_options(options)
    if opts["mode"] == "sugar":
        if backend != "3dgs":
            raise ValueError("SuGaR mesh export is only available for 3DGS scenes")
        if not ply_path(scene, iteration).exists():
            raise FileNotFoundError(f"Missing point cloud for mesh export: {ply_path(scene, iteration)}")
        cfg = load_cfg_args(model_path)
        source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / scene)
        sugar_checkpoint_path, sugar_source_dir = prepare_sugar_checkpoint(model_path, source_path, iteration, opts)
        sugar_source_path = sugar_source_dir.as_posix()
        sugar_model_path = posix_dir_with_trailing_slash(sugar_checkpoint_path)
        command = [
            sugar_python(),
            "train.py",
            "-s",
            sugar_source_path,
            "-c",
            sugar_model_path,
            "-i",
            str(iteration),
            "-r",
            opts["sugar_regularization"],
            "-l",
            str(opts["sugar_surface_level"]),
            "--square_size",
            str(opts["sugar_square_size"]),
            "-t",
            "True",
            "--export_ply",
            "True",
            "--eval",
            "False",
        ]
        if opts["sugar_quality"] == "low":
            command.extend(["--low_poly", "True"])
        else:
            command.extend(["--high_poly", "True"])
        if opts["sugar_refinement_time"] in {"short", "medium", "long"}:
            command.extend(["--refinement_time", opts["sugar_refinement_time"]])
        if opts["sugar_postprocess"]:
            command.extend(["--postprocess_mesh", "True"])
        return command, SUGAR_DIR, mesh_output_path(scene, iteration, opts["mode"], post=True)
    if opts["mode"] == "gs2mesh":
        if backend != "3dgs":
            raise ValueError("GS2Mesh mesh export is only available for 3DGS scenes")
        cfg = load_cfg_args(model_path)
        source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / scene)
        prepare_gs2mesh_inputs(scene, model_path, source_path, iteration, opts["gs2mesh_downsample"])
        can_reuse_render_cache = prepare_gs2mesh_render_cache(scene, iteration, opts)
        command = [
            gs2mesh_python(),
            "run_single.py",
            "--colmap_name",
            scene,
            "--dataset_name",
            "custom",
            "--experiment_folder_name",
            "app",
            "--renderer_folder_name",
            gs2mesh_renderer_folder(scene, opts["gs2mesh_downsample"]),
            "--skip_video_extraction",
            "--skip_colmap",
            "--skip_GS",
            "--skip_masking",
            "--GS_iterations",
            str(iteration),
            "--downsample",
            str(opts["gs2mesh_downsample"]),
            "--renderer_baseline_percentage",
            str(opts["gs2mesh_baseline_percentage"]),
            "--TSDF_voxel",
            str(opts["gs2mesh_tsdf_voxel"]),
            "--TSDF_min_depth_baselines",
            str(opts["gs2mesh_tsdf_min_depth_baselines"]),
            "--TSDF_max_depth_baselines",
            str(opts["gs2mesh_tsdf_max_depth_baselines"]),
            "--TSDF_cleaning_threshold",
            str(opts["gs2mesh_tsdf_cleaning_threshold"]),
        ]
        if not opts["gs2mesh_scene_360"]:
            command.append("--no-renderer_scene_360")
        if can_reuse_render_cache:
            command.append("--skip_rendering")
        return command, GS2MESH_DIR, mesh_output_path(scene, iteration, opts["mode"], post=True)
    if backend != "2dgs":
        raise ValueError("Mesh export is only available for 2DGS scenes")
    if not ply_path(scene, iteration).exists():
        raise FileNotFoundError(f"Missing point cloud for mesh export: {ply_path(scene, iteration)}")
    cfg = load_cfg_args(model_path)
    source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / scene)
    command = [
        training_python("2dgs"),
        "render.py",
        "-m",
        model_path,
        "-s",
        source_path,
        "--iteration",
        str(iteration),
        "--skip_train",
        "--skip_test",
        "--mesh_res",
        str(opts["mesh_res"]),
        "--num_cluster",
        str(opts["num_cluster"]),
        "--depth_ratio",
        str(opts["depth_ratio"]),
    ]
    if opts["mode"] == "unbounded":
        command.append("--unbounded")
    else:
        for arg_name in ("voxel_size", "depth_trunc", "sdf_trunc"):
            if opts[arg_name] > 0:
                command.extend([f"--{arg_name}", str(opts[arg_name])])
    return command, TWO_DGS_DIR, mesh_output_path(scene, iteration, opts["mode"], post=True)


def ensure_mesh_environment():
    report = training_environment_report("2dgs")
    problems = []
    python_path = Path(report["two_dgs_python"])
    render_path = TWO_DGS_DIR / "render.py"
    if not report["two_dgs_dir_exists"]:
        problems.append(f"Missing 2DGS directory: {report['two_dgs_dir']}")
    if not python_path.exists():
        problems.append(f"Missing 2DGS venv Python: {python_path}")
    if not render_path.exists():
        problems.append(f"Missing 2DGS render.py: {render_path}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return report


def sugar_python():
    configured = os.environ.get("SUGAR_PYTHON")
    if configured:
        return Path(configured)
    env_python = sugar_env_root() / "python.exe"
    if env_python.exists():
        return env_python
    return training_python("3dgs")


def ensure_sugar_environment():
    problems = []
    python_path = sugar_python()
    script_path = SUGAR_DIR / "train.py"
    if not SUGAR_DIR.exists():
        problems.append(f"Missing SuGaR directory: {SUGAR_DIR}")
    if not python_path.exists():
        problems.append(f"Missing SuGaR Python executable: {python_path}")
    if not script_path.exists():
        problems.append(f"Missing SuGaR train.py: {script_path}")
    if not problems:
        probe = (
            "import importlib.util\n"
            "missing=[m for m in ('torch','pytorch3d','open3d') if importlib.util.find_spec(m) is None]\n"
            "print(','.join(missing))\n"
        )
        try:
            result = subprocess.run(
                [str(python_path), "-c", probe],
                cwd=str(SUGAR_DIR),
                env=sugar_env(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=20,
                check=False,
            )
            missing = (result.stdout or "").strip()
            if result.returncode != 0:
                problems.append(f"SuGaR dependency probe failed: {(result.stderr or result.stdout or '').strip()}")
            elif missing:
                problems.append(f"Missing SuGaR Python modules in {python_path}: {missing}")
        except Exception as exc:
            problems.append(f"SuGaR dependency probe failed: {exc}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return {"sugar_dir": str(SUGAR_DIR), "python": str(python_path), "script": str(script_path)}


def gs2mesh_python():
    configured = os.environ.get("GS2MESH_PYTHON")
    if configured:
        return Path(configured)
    return gs2mesh_env_root() / "python.exe"


def ensure_gs2mesh_environment():
    problems = []
    python_path = gs2mesh_python()
    script_path = GS2MESH_DIR / "run_single.py"
    if not GS2MESH_DIR.exists():
        problems.append(f"Missing GS2Mesh directory: {GS2MESH_DIR}")
    if not python_path.exists():
        problems.append(f"Missing GS2Mesh Python executable: {python_path}")
    if not script_path.exists():
        problems.append(f"Missing GS2Mesh run_single.py: {script_path}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return {"gs2mesh_dir": str(GS2MESH_DIR), "python": str(python_path), "script": str(script_path)}


def parse_obj_material_path(obj_path):
    obj_path = Path(obj_path)
    try:
        for line in obj_path.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.strip()
            if stripped.startswith("mtllib "):
                return obj_path.parent / stripped.split(None, 1)[1].strip()
    except OSError:
        pass
    candidate = obj_path.with_suffix(".mtl")
    return candidate if candidate.exists() else None


def parse_mtl_texture_path(mtl_path):
    if not mtl_path:
        return None
    mtl_path = Path(mtl_path)
    try:
        for line in mtl_path.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.strip()
            if stripped.startswith("map_Kd "):
                return mtl_path.parent / stripped.split(None, 1)[1].strip()
    except OSError:
        return None
    for suffix in (".png", ".jpg", ".jpeg"):
        candidate = mtl_path.with_suffix(suffix)
        if candidate.exists():
            return candidate
    return None


def obj_to_ascii_ply(obj_path, ply_path):
    vertices = []
    faces = []
    with Path(obj_path).open("r", encoding="utf-8", errors="replace") as obj:
        for line in obj:
            if line.startswith("v "):
                parts = line.split()
                if len(parts) >= 4:
                    vertices.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif line.startswith("f "):
                indices = []
                for token in line.split()[1:]:
                    raw = token.split("/", 1)[0]
                    if not raw:
                        continue
                    index = int(raw)
                    if index < 0:
                        index = len(vertices) + index
                    else:
                        index -= 1
                    indices.append(index)
                if len(indices) >= 3:
                    for i in range(1, len(indices) - 1):
                        faces.append((indices[0], indices[i], indices[i + 1]))
    if not vertices or not faces:
        raise RuntimeError(f"SuGaR OBJ has no loadable mesh geometry: {obj_path}")
    ply_path = Path(ply_path)
    ply_path.parent.mkdir(parents=True, exist_ok=True)
    with ply_path.open("w", encoding="utf-8", newline="\n") as ply:
        ply.write("ply\nformat ascii 1.0\n")
        ply.write(f"element vertex {len(vertices)}\n")
        ply.write("property float x\nproperty float y\nproperty float z\n")
        ply.write(f"element face {len(faces)}\n")
        ply.write("property list uchar int vertex_indices\nend_header\n")
        for x, y, z in vertices:
            ply.write(f"{x:.9g} {y:.9g} {z:.9g}\n")
        for a, b, c in faces:
            ply.write(f"3 {a} {b} {c}\n")
    return ply_path


def find_latest_sugar_obj(source_path, started_at):
    scene_name = Path(source_path).name
    search_roots = [SUGAR_DIR / "output" / "refined_mesh" / scene_name, SUGAR_DIR / "output" / "refined_mesh"]
    candidates = []
    for root in search_roots:
        if root.exists():
            candidates.extend(root.rglob("*.obj"))
    candidates = [
        path for path in candidates
        if path.is_file() and path.stat().st_mtime >= started_at - 5
    ]
    if not candidates:
        raise RuntimeError(f"SuGaR finished but no textured OBJ was found in {SUGAR_DIR / 'output' / 'refined_mesh'}")
    return max(candidates, key=lambda path: path.stat().st_mtime)


def collect_sugar_mesh_outputs(scene, iteration, source_path, started_at):
    output_paths = mesh_texture_paths(scene, iteration, "sugar", post=True)
    output_paths["dir"].mkdir(parents=True, exist_ok=True)
    source_obj = find_latest_sugar_obj(source_path, started_at)
    source_mtl = parse_obj_material_path(source_obj)
    source_texture = parse_mtl_texture_path(source_mtl)
    if not source_mtl or not source_mtl.exists():
        raise RuntimeError(f"SuGaR OBJ has no material file: {source_obj}")
    if not source_texture or not source_texture.exists():
        raise RuntimeError(f"SuGaR material has no texture image: {source_mtl}")

    obj_text = source_obj.read_text(encoding="utf-8", errors="replace")
    obj_lines = []
    for line in obj_text.splitlines():
        if line.strip().startswith("mtllib "):
            obj_lines.append(f"mtllib {output_paths['mtl'].name}")
        else:
            obj_lines.append(line)
    output_paths["obj"].write_text("\n".join(obj_lines) + "\n", encoding="utf-8", newline="\n")

    mtl_text = source_mtl.read_text(encoding="utf-8", errors="replace")
    mtl_lines = []
    for line in mtl_text.splitlines():
        if line.strip().startswith("map_Kd "):
            mtl_lines.append(f"map_Kd {output_paths['png'].name}")
        else:
            mtl_lines.append(line)
    output_paths["mtl"].write_text("\n".join(mtl_lines) + "\n", encoding="utf-8", newline="\n")
    convert_texture_to_png(source_texture, output_paths["png"])

    geometry_ply = obj_to_ascii_ply(output_paths["obj"], mesh_output_path(scene, iteration, "sugar", post=True))
    with zipfile.ZipFile(output_paths["zip"], "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(output_paths["obj"], output_paths["obj"].name)
        archive.write(output_paths["mtl"], output_paths["mtl"].name)
        archive.write(output_paths["png"], output_paths["png"].name)
    glb_error = None
    try:
        export_textured_obj_to_glb(output_paths)
    except Exception as exc:
        glb_error = str(exc)
    return {
        "source_obj": str(source_obj),
        "mesh": str(geometry_ply),
        "obj": str(output_paths["obj"]),
        "mtl": str(output_paths["mtl"]),
        "png": str(output_paths["png"]),
        "zip": str(output_paths["zip"]),
        "glb": str(output_paths["glb"]) if output_paths["glb"].exists() else None,
        "glb_error": glb_error,
    }


def bool_option(value, default=False):
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


def bake_texture_options(options):
    options = options or {}
    mode = safe_mesh_mode(options.get("mode", "bounded"))
    resolution = int(options.get("texture_res", 2048))
    padding = int(options.get("padding", 8))
    max_images = int(options.get("max_images", 96))
    bake_source = (options.get("bake_source", "photo") or "photo").lower()
    backend = (options.get("backend") or options.get("texture_backend") or "openmvs").lower()
    max_faces = int(options.get("max_faces", 100000 if backend == "openmvs" else 200000))
    local_seam_leveling = bool_option(options.get("local_seam_leveling"), False)
    cost_smoothness_ratio = float(options.get("cost_smoothness_ratio", 0.3))
    texture_size_multiple = int(options.get("texture_size_multiple", 0))
    post = bool_option(options.get("post"), True)
    if resolution < 256 or resolution > 8192:
        raise ValueError("texture_res must be between 256 and 8192")
    if padding < 0 or padding > 64:
        raise ValueError("padding must be between 0 and 64")
    if max_faces < 0 or max_faces > 5000000:
        raise ValueError("max_faces must be between 0 and 5000000")
    if max_images < 1 or max_images > 1000:
        raise ValueError("max_images must be between 1 and 1000")
    if cost_smoothness_ratio < 0.0 or cost_smoothness_ratio > 1.0:
        raise ValueError("cost_smoothness_ratio must be between 0 and 1")
    if texture_size_multiple < 0 or texture_size_multiple > 8192:
        raise ValueError("texture_size_multiple must be between 0 and 8192")
    if bake_source not in {"photo", "vertex"}:
        raise ValueError("bake_source must be photo or vertex")
    if backend not in {"openmvs", "python", "colmap"}:
        raise ValueError("texture backend must be openmvs, colmap, or python")
    return {
        "mode": mode,
        "texture_res": resolution,
        "padding": padding,
        "max_faces": max_faces,
        "max_images": max_images,
        "bake_source": bake_source,
        "backend": backend,
        "local_seam_leveling": local_seam_leveling,
        "cost_smoothness_ratio": cost_smoothness_ratio,
        "texture_size_multiple": texture_size_multiple,
        "post": post,
    }


def texture_bake_command(mesh_path, output_dir, texture_res=2048, padding=8, max_faces=200000, source_dir=None, bake_source="photo", max_images=96):
    script = TWO_DGS_DIR / "tools" / "bake_mesh_texture.py"
    if not script.exists():
        raise FileNotFoundError(f"Missing texture bake script: {script}")
    command = [
        training_python("2dgs"),
        script,
        "--mesh",
        mesh_path,
        "--output-dir",
        output_dir,
        "--texture-res",
        int(texture_res),
        "--padding",
        int(padding),
        "--max-faces",
        int(max_faces),
        "--bake-source",
        bake_source,
        "--max-images",
        int(max_images),
    ]
    if source_dir:
        command.extend(["--source-dir", source_dir])
    return command


def openmvs_bin_dirs():
    dirs = []
    env_bin = os.environ.get("OPENMVS_BIN", "")
    for part in env_bin.split(os.pathsep):
        if part:
            dirs.append(Path(part))
    dirs.extend([
        OPENMVS_DIR / "build" / "bin" / "x64" / "Release",
        OPENMVS_DIR / "build" / "bin" / "Release",
        OPENMVS_DIR / "build" / "bin",
        OPENMVS_DIR / "install" / "bin",
        OPENMVS_DIR / "bin" / "x64" / "Release",
        OPENMVS_DIR / "bin" / "Release",
        OPENMVS_DIR / "bin",
    ])
    return dirs


def openmvs_executable(name):
    exe_name = f"{name}.exe" if os.name == "nt" and not name.lower().endswith(".exe") else name
    for directory in openmvs_bin_dirs():
        candidate = directory / exe_name
        if candidate.exists():
            return candidate
    found = shutil.which(exe_name)
    return Path(found) if found else None


def colmap_executable():
    env_colmap = os.environ.get("COLMAP_EXE")
    if env_colmap and Path(env_colmap).exists():
        return Path(env_colmap)
    fallback = ROOT / "third_party" / "colmap" / "bin" / "colmap.exe"
    for candidate in (
        fallback,
        ROOT / "tools" / "colmap" / "bin" / "colmap.exe",
        ROOT / "colmap" / "bin" / "colmap.exe",
    ):
        if candidate.exists():
            return candidate
    userprofile = os.environ.get("USERPROFILE", str(Path.home()))
    bundled = Path(userprofile) / "Downloads" / "colmap-x64-windows-cuda" / "bin" / "colmap.exe"
    if bundled.exists():
        return bundled
    found = shutil.which("colmap.exe" if os.name == "nt" else "colmap")
    return Path(found) if found else fallback


def openmvs_environment_report():
    interface_colmap = openmvs_executable("InterfaceCOLMAP")
    texture_mesh = openmvs_executable("TextureMesh")
    colmap = colmap_executable()
    return {
        "openmvs_dir": str(OPENMVS_DIR),
        "openmvs_dir_exists": OPENMVS_DIR.exists(),
        "openmvs_bin_dirs": [str(path) for path in openmvs_bin_dirs()],
        "interface_colmap": str(interface_colmap) if interface_colmap else None,
        "interface_colmap_exists": bool(interface_colmap),
        "texture_mesh": str(texture_mesh) if texture_mesh else None,
        "texture_mesh_exists": bool(texture_mesh),
        "colmap_undistorter": str(colmap) if colmap else None,
        "colmap_undistorter_exists": bool(colmap),
    }


def ensure_openmvs_environment():
    report = openmvs_environment_report()
    problems = []
    if not report["interface_colmap_exists"]:
        problems.append("Missing OpenMVS InterfaceCOLMAP.exe")
    if not report["texture_mesh_exists"]:
        problems.append("Missing OpenMVS TextureMesh.exe")
    if problems:
        hint = (
            f"Build OpenMVS from source into {OPENMVS_DIR} or set OPENMVS_BIN to the folder "
            "containing InterfaceCOLMAP.exe and TextureMesh.exe."
        )
        raise RuntimeError("; ".join(problems) + ". " + hint)
    return report


def ply_face_count(path):
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("ascii", errors="ignore").strip()
            if line.startswith("element face "):
                try:
                    return int(line.split()[-1])
                except ValueError:
                    return 0
            if line == "end_header":
                break
    return 0


def colmap_texture_scale_factor(source_mesh, texture_res=8192):
    requested_scale = max(0.03125, min(float(texture_res) / 8192.0, 1.0))
    face_count = ply_face_count(source_mesh)
    scale = requested_scale
    reason = "requested texture resolution"
    if face_count > 2500000:
        scale = min(scale, 0.25)
        reason = "large mesh atlas guard"
    elif face_count > 750000:
        scale = min(scale, 0.5)
        reason = "medium mesh atlas guard"
    return scale, face_count, reason


def prepare_openmvs_colmap_input(dataset_path, work_dir):
    dataset_path = Path(dataset_path)
    dense_dir = dataset_path / "dense"
    dense_images = dense_dir / "images"
    dense_sparse = dense_dir / "sparse"
    if dense_images.exists() and ((dense_sparse / "cameras.bin").exists() or (dense_sparse / "cameras.txt").exists()):
        return dense_dir, dense_images, None

    images_dir = dataset_path / "images"
    if not images_dir.exists():
        raise FileNotFoundError(f"Missing source images folder for OpenMVS: {images_dir}")

    sparse_root = dataset_path / "sparse"
    sparse_candidates = [sparse_root / "0", sparse_root]
    source_sparse = next((path for path in sparse_candidates if (path / "cameras.bin").exists() or (path / "cameras.txt").exists()), None)
    if source_sparse is None:
        raise FileNotFoundError(f"Missing COLMAP sparse model for OpenMVS: {sparse_root}")

    staged_dir = work_dir / "dense"
    staged_sparse = staged_dir / "sparse"
    staged_sparse.mkdir(parents=True, exist_ok=True)
    for name in ("cameras.bin", "images.bin", "points3D.bin", "rigs.bin", "frames.bin", "cameras.txt", "images.txt", "points3D.txt"):
        src = source_sparse / name
        if src.exists():
            shutil.copy2(src, staged_sparse / name)
    return staged_dir, images_dir, None


def prepare_colmap_texturer_workspace(dataset_path, work_dir):
    dataset_path = Path(dataset_path)
    images_dir = dataset_path / "images"
    if not images_dir.exists():
        raise FileNotFoundError(f"Missing source images folder for COLMAP mesh_texturer: {images_dir}")

    sparse_root = dataset_path / "sparse"
    sparse_candidates = [sparse_root, sparse_root / "0"]
    source_sparse = next((path for path in sparse_candidates if (path / "cameras.bin").exists() or (path / "cameras.txt").exists()), None)
    if source_sparse is None:
        raise FileNotFoundError(f"Missing COLMAP sparse model for mesh_texturer: {sparse_root}")

    workspace = Path(work_dir) / "workspace"
    staged_sparse = workspace / "sparse"
    staged_images = workspace / "images"
    staged_sparse.mkdir(parents=True, exist_ok=True)
    staged_images.mkdir(parents=True, exist_ok=True)

    for name in ("cameras.bin", "images.bin", "points3D.bin", "rigs.bin", "frames.bin", "cameras.txt", "images.txt", "points3D.txt"):
        src = source_sparse / name
        if src.exists():
            dst = staged_sparse / name
            if not dst.exists() or src.stat().st_mtime > dst.stat().st_mtime:
                shutil.copy2(src, dst)

    for image in images_dir.rglob("*"):
        if image.is_file() and image.suffix.lower() in IMAGE_EXTS:
            dst = staged_images / image.relative_to(images_dir)
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists() or image.stat().st_mtime > dst.stat().st_mtime:
                shutil.copy2(image, dst)
    return workspace


def latest_existing_mtime(paths):
    latest = 0.0
    for path in paths:
        path = Path(path)
        if path.exists():
            latest = max(latest, path.stat().st_mtime)
    return latest


def openmvs_texture_commands(scene, iteration, options, work_subdir="openmvs"):
    source_mesh = mesh_output_path(scene, iteration, options["mode"], options["post"])
    if not source_mesh.exists():
        raise FileNotFoundError(f"Mesh not found: {source_mesh}. Export Mesh first.")
    report = ensure_openmvs_environment()
    cfg = load_cfg_args(model_dir(scene))
    dataset_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / scene)
    output_paths = mesh_texture_paths(scene, iteration, options["mode"], options["post"])
    work_dir = output_paths["dir"] / work_subdir
    work_dir.mkdir(parents=True, exist_ok=True)
    colmap_input, images_dir, undistort_command = prepare_openmvs_colmap_input(dataset_path, work_dir)
    scene_mvs = work_dir / "scene.mvs"
    output_stem = work_dir / f"{source_mesh.stem}_openmvs.obj"

    face_count = ply_face_count(source_mesh)
    max_faces = int(options.get("max_faces") or 0)
    decimate = 1.0
    if max_faces > 0 and face_count > max_faces:
        decimate = max(max_faces / float(face_count), 0.001)

    commands = []
    if undistort_command:
        commands.append(undistort_command)
    sparse_dir = colmap_input / "sparse"
    sparse_mtime = latest_existing_mtime(sparse_dir.glob("*")) if sparse_dir.exists() else 0.0
    scene_mvs_reusable = scene_mvs.exists() and (not sparse_mtime or scene_mvs.stat().st_mtime >= sparse_mtime)
    if not scene_mvs_reusable:
        commands.append([
            Path(report["interface_colmap"]),
            "-i",
            colmap_input,
            "-o",
            scene_mvs,
            "--image-folder",
            images_dir,
            "-w",
            work_dir,
        ])
    commands.extend([
        [
            Path(report["texture_mesh"]),
            "-i",
            scene_mvs,
            "-m",
            source_mesh,
            "-o",
            output_stem,
            "-w",
            work_dir,
            "--export-type",
            "obj",
            "--resolution-level",
            "0",
            "--max-texture-size",
            str(int(options["texture_res"])),
            "--texture-size-multiple",
            str(int(options.get("texture_size_multiple", 0))),
            "--decimate",
            f"{decimate:.6f}",
            "--empty-color",
            "12632256",
            "--virtual-face-images",
            "3",
            "--close-holes",
            "0",
            "--cost-smoothness-ratio",
            f"{float(options.get('cost_smoothness_ratio', 0.3)):g}",
            "--global-seam-leveling",
            "0",
            "--local-seam-leveling",
            "1" if options.get("local_seam_leveling") else "0",
            "--sharpness-weight",
            "0",
            "--patch-packing-heuristic",
            "3",
        ],
    ])
    return commands, work_dir, output_stem


def colmap_texturer_command(scene, iteration, options):
    source_mesh = mesh_output_path(scene, iteration, options["mode"], options["post"])
    if not source_mesh.exists():
        raise FileNotFoundError(f"Mesh not found: {source_mesh}. Export Mesh first.")
    colmap = colmap_executable()
    if not colmap:
        raise RuntimeError("Missing COLMAP executable for mesh_texturer")
    cfg = load_cfg_args(model_dir(scene))
    dataset_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / scene)
    output_paths = mesh_texture_paths(scene, iteration, options["mode"], options["post"])
    work_dir = output_paths["dir"] / "colmap_texturer"
    work_dir.mkdir(parents=True, exist_ok=True)
    workspace = prepare_colmap_texturer_workspace(dataset_path, work_dir)
    texturer_output = work_dir / "output"
    if texturer_output.exists():
        shutil.rmtree(texturer_output)
    texturer_output.mkdir(parents=True, exist_ok=True)
    scale, _, _ = colmap_texture_scale_factor(source_mesh, options.get("texture_res", 8192))
    return [
        colmap,
        "mesh_texturer",
        "--workspace_path",
        workspace,
        "--input_path",
        source_mesh,
        "--output_path",
        texturer_output,
        "--MeshTextureMapping.texture_scale_factor",
        f"{scale:g}",
    ], work_dir, texturer_output


def clear_texture_outputs(output_paths):
    for key in ("obj", "mtl", "png", "zip", "glb"):
        try:
            Path(output_paths[key]).unlink()
        except FileNotFoundError:
            pass


def run_openmvs_texture_bake(job, output_paths, options):
    try:
        commands, work_dir, _ = openmvs_texture_commands(job["scene"], job["iteration"], options)
        for command in commands:
            run_logged(job, command, work_dir, "2dgs")
        collected = collect_openmvs_texture_outputs(work_dir, output_paths)
        collected["local_seam_leveling_used"] = bool(options.get("local_seam_leveling"))
        return collected
    except Exception as exc:
        if not options.get("local_seam_leveling"):
            raise
        add_job_log(job, f"WARNING: OpenMVS local seam-leveling failed; retrying without local seam-leveling. Existing texture files were preserved. Error: {exc}")
        retry_options = {**options, "local_seam_leveling": False}
        commands, work_dir, _ = openmvs_texture_commands(
            job["scene"],
            job["iteration"],
            retry_options,
            work_subdir="openmvs_retry_noseam",
        )
        for command in commands:
            run_logged(job, command, work_dir, "2dgs")
        collected = collect_openmvs_texture_outputs(work_dir, output_paths)
        collected["local_seam_leveling_used"] = False
        collected["local_seam_leveling_error"] = str(exc)
        return collected


def convert_texture_to_png(source, target):
    source = Path(source)
    target = Path(target)
    if source.suffix.lower() == ".png":
        shutil.copy2(source, target)
        return
    try:
        from PIL import Image
    except Exception as exc:
        raise RuntimeError(f"OpenMVS wrote a non-PNG texture ({source.name}); install Pillow to convert it: {exc}")
    with Image.open(source) as image:
        image.save(target)


def export_textured_obj_to_glb(output_paths):
    try:
        import trimesh
    except Exception as exc:
        raise RuntimeError(f"GLB export requires trimesh in the 2DGS environment: {exc}") from exc
    obj_path = Path(output_paths["obj"])
    glb_path = Path(output_paths["glb"])
    if not obj_path.exists():
        raise FileNotFoundError(f"Textured OBJ not found for GLB export: {obj_path}")
    loaded = trimesh.load(obj_path, process=False)
    glb_path.parent.mkdir(parents=True, exist_ok=True)
    loaded.export(glb_path)
    if not glb_path.exists() or glb_path.read_bytes()[:4] != b"glTF":
        raise RuntimeError(f"GLB export failed: {glb_path}")
    return glb_path


def collect_openmvs_texture_outputs(work_dir, output_paths):
    work_dir = Path(work_dir)
    obj_files = sorted(work_dir.rglob("*.obj"), key=lambda p: (0 if "text" in p.stem.lower() else 1, -p.stat().st_mtime))
    if not obj_files:
        raise RuntimeError(f"OpenMVS finished but no OBJ was written in {work_dir}")
    source_obj = obj_files[0]
    source_mtl = source_obj.with_suffix(".mtl")
    if not source_mtl.exists():
        mtl_files = sorted(source_obj.parent.glob("*.mtl"))
        if not mtl_files:
            raise RuntimeError(f"OpenMVS OBJ has no MTL next to it: {source_obj}")
        source_mtl = mtl_files[0]

    output_paths["dir"].mkdir(parents=True, exist_ok=True)
    texture_names = []
    for raw_line in source_mtl.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = raw_line.strip()
        if stripped.lower().startswith("map_kd "):
            texture_names.append(stripped.split(maxsplit=1)[1])
    texture_sources = []
    for name in texture_names:
        texture = (source_mtl.parent / name).resolve()
        if texture.exists() and texture.suffix.lower() in IMAGE_EXTS:
            texture_sources.append(texture)
    if not texture_sources:
        images = [p for p in source_mtl.parent.iterdir() if p.suffix.lower() in IMAGE_EXTS]
        texture_sources = sorted(images)
    if not texture_sources:
        raise RuntimeError(f"OpenMVS OBJ has no texture image next to it: {source_obj}")

    obj_text = source_obj.read_text(encoding="utf-8", errors="replace")
    obj_text = re.sub(r"(?m)^mtllib\s+.*$", f"mtllib {output_paths['mtl'].name}", obj_text)
    output_paths["obj"].write_text(obj_text, encoding="utf-8")

    convert_texture_to_png(texture_sources[0], output_paths["png"])
    mtl_text = source_mtl.read_text(encoding="utf-8", errors="replace")
    mtl_text = re.sub(r"(?mi)^map_Kd\s+.*$", f"map_Kd {output_paths['png'].name}", mtl_text)
    output_paths["mtl"].write_text(mtl_text, encoding="utf-8")

    with zipfile.ZipFile(output_paths["zip"], "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(output_paths["obj"], output_paths["obj"].name)
        archive.write(output_paths["mtl"], output_paths["mtl"].name)
        archive.write(output_paths["png"], output_paths["png"].name)
        for texture in texture_sources[1:]:
            archive.write(texture, texture.name)
    glb_error = None
    try:
        export_textured_obj_to_glb(output_paths)
    except Exception as exc:
        glb_error = str(exc)
    return {
        "obj": str(output_paths["obj"]),
        "mtl": str(output_paths["mtl"]),
        "png": str(output_paths["png"]),
        "zip": str(output_paths["zip"]),
        "glb": str(output_paths["glb"]) if output_paths["glb"].exists() else None,
        "glb_error": glb_error,
        "openmvs_obj": str(source_obj),
        "texture_pages": len(texture_sources),
    }


def write_colmap_textured_ply_as_obj(ply_path, texture_name, obj_path, mtl_name):
    from plyfile import PlyData

    ply = PlyData.read(str(ply_path))
    vertices = ply["vertex"].data
    faces = ply["face"].data
    vt_lines = []
    face_lines = []
    vt_index = 1
    for face in faces:
        vertex_indices = list(face["vertex_indices"])
        texcoords = list(face["texcoord"])
        if len(vertex_indices) < 3 or len(texcoords) < len(vertex_indices) * 2:
            continue
        face_refs = []
        for corner, vertex_index in enumerate(vertex_indices):
            u = float(texcoords[corner * 2])
            v = float(texcoords[corner * 2 + 1])
            vt_lines.append(f"vt {u:.9g} {v:.9g}\n")
            face_refs.append(f"{int(vertex_index) + 1}/{vt_index}")
            vt_index += 1
        for i in range(1, len(face_refs) - 1):
            face_lines.append(f"f {face_refs[0]} {face_refs[i]} {face_refs[i + 1]}\n")
    if not face_lines:
        raise RuntimeError(f"COLMAP textured PLY has no textured faces: {ply_path}")

    with Path(obj_path).open("w", encoding="utf-8", newline="\n") as obj:
        obj.write(f"mtllib {mtl_name}\n")
        obj.write("usemtl material_0\n")
        for vertex in vertices:
            obj.write(f"v {float(vertex['x']):.9g} {float(vertex['y']):.9g} {float(vertex['z']):.9g}\n")
        obj.writelines(vt_lines)
        obj.writelines(face_lines)


def collect_colmap_texture_outputs(texturer_output, output_paths):
    texturer_output = Path(texturer_output)
    source_ply = texturer_output / "mesh.ply"
    source_texture = texturer_output / "texture.png"
    if not source_ply.exists():
        raise RuntimeError(f"COLMAP mesh_texturer finished but no mesh.ply was written in {texturer_output}")
    if not source_texture.exists():
        raise RuntimeError(f"COLMAP mesh_texturer finished but no texture.png was written in {texturer_output}")

    output_paths["dir"].mkdir(parents=True, exist_ok=True)
    convert_texture_to_png(source_texture, output_paths["png"])
    output_paths["mtl"].write_text(
        "\n".join([
            "newmtl material_0",
            "Ka 1.000000 1.000000 1.000000",
            "Kd 1.000000 1.000000 1.000000",
            "Ks 0.000000 0.000000 0.000000",
            "d 1.000000",
            f"map_Kd {output_paths['png'].name}",
            "",
        ]),
        encoding="utf-8",
    )
    write_colmap_textured_ply_as_obj(source_ply, output_paths["png"].name, output_paths["obj"], output_paths["mtl"].name)

    with zipfile.ZipFile(output_paths["zip"], "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(output_paths["obj"], output_paths["obj"].name)
        archive.write(output_paths["mtl"], output_paths["mtl"].name)
        archive.write(output_paths["png"], output_paths["png"].name)
    glb_error = None
    try:
        export_textured_obj_to_glb(output_paths)
    except Exception as exc:
        glb_error = str(exc)
    return {
        "obj": str(output_paths["obj"]),
        "mtl": str(output_paths["mtl"]),
        "png": str(output_paths["png"]),
        "zip": str(output_paths["zip"]),
        "glb": str(output_paths["glb"]) if output_paths["glb"].exists() else None,
        "glb_error": glb_error,
        "colmap_ply": str(source_ply),
        "texture_pages": 1,
    }


def bake_texture_from_vertex_colors(mesh_path, output_paths, texture_res=2048, padding=8):
    try:
        import cv2
        import open3d as o3d
        import xatlas
    except Exception as exc:
        raise RuntimeError(f"Texture baking requires open3d, opencv-python, and xatlas in the 2DGS environment: {exc}")

    mesh = o3d.io.read_triangle_mesh(str(mesh_path))
    vertices = np.asarray(mesh.vertices, dtype=np.float32)
    triangles = np.asarray(mesh.triangles, dtype=np.uint32)
    vertex_colors = np.asarray(mesh.vertex_colors, dtype=np.float32)
    if not len(vertices) or not len(triangles):
        raise ValueError(f"Mesh has no triangles: {mesh_path}")
    if len(vertex_colors) != len(vertices):
        raise ValueError("Mesh has no vertex colors to bake. Export/load a 2DGS colored mesh first.")

    if not mesh.has_vertex_normals():
        mesh.compute_vertex_normals()
    normals = np.asarray(mesh.vertex_normals, dtype=np.float32)

    atlas = xatlas.Atlas()
    atlas.add_mesh(vertices, triangles, normals)
    pack_options = xatlas.PackOptions()
    pack_options.resolution = int(texture_res)
    pack_options.padding = int(padding)
    pack_options.create_image = False
    atlas.generate(pack_options=pack_options)
    vmapping, indices, uvs = atlas[0]
    indices = np.asarray(indices, dtype=np.uint32).reshape(-1, 3)
    uvs = np.asarray(uvs, dtype=np.float32)
    remapped_vertices = vertices[np.asarray(vmapping, dtype=np.uint32)]
    remapped_colors = np.clip(vertex_colors[np.asarray(vmapping, dtype=np.uint32)] * 255.0, 0, 255).astype(np.uint8)

    resolution = int(texture_res)
    texture = np.zeros((resolution, resolution, 3), dtype=np.uint8)
    mask = np.zeros((resolution, resolution), dtype=np.uint8)
    uv_pixels = np.column_stack([
        np.clip(uvs[:, 0] * (resolution - 1), 0, resolution - 1),
        np.clip((1.0 - uvs[:, 1]) * (resolution - 1), 0, resolution - 1),
    ]).astype(np.float32)
    for tri in indices:
        pts = np.rint(uv_pixels[tri]).astype(np.int32)
        color = remapped_colors[tri].mean(axis=0).astype(np.uint8)
        cv2.fillConvexPoly(texture, pts, color.tolist(), lineType=cv2.LINE_AA)
        cv2.fillConvexPoly(mask, pts, 255, lineType=cv2.LINE_AA)

    if padding:
        kernel = np.ones((3, 3), dtype=np.uint8)
        for _ in range(int(padding)):
            dilated_texture = cv2.dilate(texture, kernel)
            dilated_mask = cv2.dilate(mask, kernel)
            fill = (mask == 0) & (dilated_mask > 0)
            texture[fill] = dilated_texture[fill]
            mask = dilated_mask

    output_paths["dir"].mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(output_paths["png"]), cv2.cvtColor(texture, cv2.COLOR_RGB2BGR))
    output_paths["mtl"].write_text(
        "\n".join([
            "newmtl material_0",
            "Ka 1.000000 1.000000 1.000000",
            "Kd 1.000000 1.000000 1.000000",
            "Ks 0.000000 0.000000 0.000000",
            "d 1.000000",
            f"map_Kd {output_paths['png'].name}",
            "",
        ]),
        encoding="utf-8",
    )
    with output_paths["obj"].open("w", encoding="utf-8", newline="\n") as obj:
        obj.write(f"mtllib {output_paths['mtl'].name}\n")
        obj.write("usemtl material_0\n")
        for vertex in remapped_vertices:
            obj.write(f"v {vertex[0]:.9g} {vertex[1]:.9g} {vertex[2]:.9g}\n")
        for uv in uvs:
            obj.write(f"vt {uv[0]:.9g} {uv[1]:.9g}\n")
        for tri in indices:
            a, b, c = (int(tri[0]) + 1, int(tri[1]) + 1, int(tri[2]) + 1)
            obj.write(f"f {a}/{a} {b}/{b} {c}/{c}\n")

    with zipfile.ZipFile(output_paths["zip"], "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for kind in ("obj", "mtl", "png"):
            archive.write(output_paths[kind], output_paths[kind].name)
    glb_error = None
    try:
        export_textured_obj_to_glb(output_paths)
    except Exception as exc:
        glb_error = str(exc)

    return {
        "vertex_count": int(len(remapped_vertices)),
        "face_count": int(len(indices)),
        "texture_res": resolution,
        "obj": str(output_paths["obj"]),
        "mtl": str(output_paths["mtl"]),
        "png": str(output_paths["png"]),
        "zip": str(output_paths["zip"]),
        "glb": str(output_paths["glb"]) if output_paths["glb"].exists() else None,
        "glb_error": glb_error,
    }


def cleanup_previews(max_age_seconds=3600):
    if not PREVIEW_DIR.exists():
        return
    cutoff = time.time() - max_age_seconds
    for child in PREVIEW_DIR.glob("*.ply"):
        try:
            if child.stat().st_mtime < cutoff:
                child.unlink()
        except OSError:
            pass


def make_preview(scene, iteration, delete_indices):
    cleanup_previews()
    token = uuid.uuid4().hex
    src_ply = ply_path(scene, iteration)
    dst_ply = PREVIEW_DIR / f"{token}.ply"
    kept, removed = filtered_ply(src_ply, dst_ply, delete_indices)
    return {
        "token": token,
        "url": f"/api/preview_ply?token={token}",
        "kept": kept,
        "removed": removed,
        "path": str(dst_ply),
    }


def serializable_train_job(job):
    data = {}
    for key, value in job.items():
        if key == "process":
            data[key] = None
        elif isinstance(value, Path):
            data[key] = str(value)
        else:
            data[key] = value
    data.setdefault("process", None)
    return data


def serializable_job(job):
    data = {}
    for key, value in job.items():
        if key in {"process", "thread"}:
            data[key] = None
        elif isinstance(value, Path):
            data[key] = str(value)
        else:
            data[key] = value
    data.setdefault("process", None)
    return data


def atomic_write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(data, indent=2, ensure_ascii=False)
    last_error = None
    for attempt in range(6):
        tmp = path.with_name(f"{path.name}.{uuid.uuid4().hex}.tmp")
        try:
            tmp.write_text(payload, encoding="utf-8")
            tmp.replace(path)
            return True
        except OSError as exc:
            if isinstance(exc, PermissionError) or getattr(exc, "winerror", None) in {5, 32}:
                last_error = exc
                try:
                    unlink_if_exists(tmp)
                except OSError:
                    pass
                time.sleep(0.025 * (attempt + 1))
                continue
            raise
    return str(last_error) if last_error else "Unknown persistence failure"


def persist_train_job(job):
    if not job.get("id") or "output_scene" not in job:
        return True
    data = serializable_train_job(job)
    path = TRAIN_JOBS_DIR / f"{safe_filename(data['id'])}.json"
    result = atomic_write_json(path, data)
    if result is True:
        job.pop("_persist_error", None)
        return True
    job["_persist_error"] = result
    return False


def persist_splat_job(job):
    if not job.get("id"):
        return True
    data = serializable_job(job)
    path = SPLAT_JOBS_DIR / f"{safe_filename(data['id'])}.json"
    result = atomic_write_json(path, data)
    if result is True:
        job.pop("_persist_error", None)
        return True
    job["_persist_error"] = result
    return False


def load_persisted_train_jobs():
    jobs = {}
    if not TRAIN_JOBS_DIR.exists():
        return jobs
    for path in TRAIN_JOBS_DIR.glob("*.json"):
        try:
            job = json.loads(path.read_text(encoding="utf-8"))
            job["process"] = None
            job.setdefault("log", [])
            job.setdefault("cancel_requested", False)
            jobs[job["id"]] = job
        except Exception:
            continue
    return jobs


def load_persisted_splat_jobs():
    jobs = {}
    if not SPLAT_JOBS_DIR.exists():
        return jobs
    for path in SPLAT_JOBS_DIR.glob("*.json"):
        try:
            job = json.loads(path.read_text(encoding="utf-8"))
            job["process"] = None
            job.setdefault("kind", "splat_export")
            job.setdefault("log", [])
            job.setdefault("cancel_requested", False)
            if job.get("status") in {"queued", "running", "cancelling"}:
                job["status"] = "cancelled"
                job["stage"] = "cancelled"
                job["error"] = "Server restarted before this export finished."
                job["updated_at"] = time.time()
                persist_splat_job(job)
            jobs[job["id"]] = job
        except Exception:
            continue
    return jobs


def job_snapshot(job):
    return {
        "id": job["id"],
        "kind": "training",
        "scene": job["scene"],
        "output_scene": job["output_scene"],
        "backend": job.get("backend", "3dgs"),
        "status": job["status"],
        "stage": job["stage"],
        "created_at": job["created_at"],
        "updated_at": job["updated_at"],
        "returncode": job.get("returncode"),
        "error": job.get("error"),
        "log": job["log"][-240:],
        "cancel_requested": bool(job.get("cancel_requested")),
    }


def splat_job_snapshot(job):
    return {
        "id": job["id"],
        "kind": "splat_export",
        "scene": job["scene"],
        "iteration": job["iteration"],
        "format": job["format"],
        "status": job["status"],
        "stage": job["stage"],
        "created_at": job["created_at"],
        "updated_at": job["updated_at"],
        "returncode": job.get("returncode"),
        "error": job.get("error"),
        "output_path": job.get("output_path"),
        "download_url": job.get("download_url"),
        "log": job["log"][-240:],
        "cancel_requested": bool(job.get("cancel_requested")),
    }


def add_job_log(job, message):
    line = str(message).rstrip()
    if not line:
        return
    lock = MESH_LOCK if mesh_like_job_kind(job) else SPLAT_LOCK if job.get("kind") == "splat_export" else TRAIN_LOCK
    with lock:
        job["log"].append(line)
        job["updated_at"] = time.time()
        if job.get("kind") == "splat_export":
            persist_splat_job(job)
        elif not mesh_like_job_kind(job):
            persist_train_job(job)


def set_job_stage(job, status, stage):
    lock = MESH_LOCK if mesh_like_job_kind(job) else SPLAT_LOCK if job.get("kind") == "splat_export" else TRAIN_LOCK
    with lock:
        job["status"] = status
        job["stage"] = stage
        job["updated_at"] = time.time()
        if job.get("kind") == "splat_export":
            persist_splat_job(job)
        elif not mesh_like_job_kind(job):
            persist_train_job(job)


def training_env(backend="3dgs"):
    backend = safe_training_backend(backend)
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    userprofile = env.get("USERPROFILE", str(Path.home()))
    env_root = Path(userprofile) / "miniforge3" / "envs" / "gaussian_splatting"
    colmap_bin = colmap_executable().parent
    path_parts = [
        str(colmap_bin),
        str(env_root),
        str(env_root / "Library" / "bin"),
        str(env_root / "Library" / "usr" / "bin"),
        str(env_root / "Scripts"),
        env.get("PATH", ""),
    ]
    if backend == "2dgs":
        two_dgs_venv = TWO_DGS_DIR / ".venv"
        cuda_path = Path(os.environ.get("CUDA_PATH", r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2"))
        path_parts = [
            str(two_dgs_venv / "Scripts"),
            str(cuda_path / "bin"),
            *path_parts,
        ]
        env["CUDA_PATH"] = str(cuda_path)
        env["CUDA_HOME"] = str(cuda_path)
        env["TORCH_CUDA_ARCH_LIST"] = env.get("TORCH_CUDA_ARCH_LIST", "8.6")
    env["PATH"] = os.pathsep.join(path_parts)
    env["CONDA_PREFIX"] = str(env_root)
    return env


def sugar_env_root():
    configured = os.environ.get("SUGAR_CONDA_PREFIX")
    if configured:
        return Path(configured)
    return Path(os.environ.get("USERPROFILE", str(Path.home()))) / "miniforge3" / "envs" / "sugar"


def sugar_env():
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env_root = sugar_env_root()
    path_parts = [
        str(env_root),
        str(env_root / "Library" / "bin"),
        str(env_root / "Library" / "usr" / "bin"),
        str(env_root / "Scripts"),
        env.get("PATH", ""),
    ]
    env["PATH"] = os.pathsep.join(path_parts)
    env["CONDA_PREFIX"] = str(env_root)
    return env


def gs2mesh_env_root():
    configured = os.environ.get("GS2MESH_CONDA_PREFIX")
    if configured:
        return Path(configured)
    # GS2Mesh reuses the SuGaR environment by default because that environment
    # already carries the Windows-compatible 3DGS rasterizer extensions.
    return sugar_env_root()


def gs2mesh_env():
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env_root = gs2mesh_env_root()
    path_parts = [
        str(env_root),
        str(env_root / "Library" / "bin"),
        str(env_root / "Library" / "usr" / "bin"),
        str(env_root / "Scripts"),
        env.get("PATH", ""),
    ]
    env["PATH"] = os.pathsep.join(path_parts)
    env["CONDA_PREFIX"] = str(env_root)
    sugar_extension_paths = [
        str(SUGAR_DIR / "gaussian_splatting" / "submodules" / "diff-gaussian-rasterization"),
        str(SUGAR_DIR / "gaussian_splatting" / "submodules" / "simple-knn"),
    ]
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = os.pathsep.join([*sugar_extension_paths, existing_pythonpath])
    return env


def training_python(backend="3dgs"):
    backend = safe_training_backend(backend)
    if backend == "2dgs":
        return TWO_DGS_DIR / ".venv" / "Scripts" / "python.exe"
    gaussian_python = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "miniforge3" / "envs" / "gaussian_splatting" / "python.exe"
    if gaussian_python.exists():
        return gaussian_python
    return Path(sys.executable)


def command_path(name):
    found = shutil.which(name)
    return Path(found) if found else None


def vs_build_tools_installation():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    try:
        result = subprocess.run(
            [
                str(vswhere),
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Workload.VCTools",
                "-property",
                "installationPath",
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except Exception:
        return None
    path = result.stdout.strip().splitlines()[0] if result.stdout.strip() else ""
    return Path(path) if path else None


def python_probe(python_path, env, code, timeout=20):
    if not python_path or not Path(python_path).exists():
        return False, "python missing"
    try:
        result = subprocess.run(
            [str(python_path), "-c", code],
            cwd=str(ROOT),
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except Exception as exc:
        return False, str(exc)
    if result.returncode == 0:
        return True, (result.stdout or "").strip()
    return False, ((result.stderr or result.stdout or "").strip() or f"exit {result.returncode}")


def training_environment_report(backend="3dgs"):
    backend = safe_training_backend(backend)
    env = training_env(backend)
    userprofile = env.get("USERPROFILE", str(Path.home()))
    miniforge_root = Path(userprofile) / "miniforge3"
    conda_bat = miniforge_root / "condabin" / "conda.bat"
    env_root = Path(userprofile) / "miniforge3" / "envs" / "gaussian_splatting"
    python_path = training_python(backend)
    colmap_path = colmap_executable()
    if colmap_path is None:
        colmap_path = ROOT / "third_party" / "colmap" / "bin" / "colmap.exe"
    ffmpeg_path = Path(userprofile) / "miniforge3" / "envs" / "gaussian_splatting" / "Library" / "bin" / "ffmpeg.exe"
    git_path = command_path("git")
    npm_path = command_path("npm")
    vs_install = vs_build_tools_installation()
    crop_helper = ROOT / "crop_editor" / "node_modules" / ".bin" / ("splat-transform.cmd" if os.name == "nt" else "splat-transform")
    opencv_ok, opencv_detail = python_probe(
        python_path,
        env,
        "import cv2; print(cv2.__version__)",
    )
    video_ok, video_detail = python_probe(
        python_path,
        env,
        "import cv2, imageio, imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())",
    )
    runtime_ok, runtime_detail = python_probe(
        python_path,
        env,
        "import torch, cv2; from PIL import Image; import diff_gaussian_rasterization, simple_knn; print('cuda', torch.cuda.is_available())",
    )
    report = {
        "backend": backend,
        "root": str(ROOT),
        "miniforge": str(miniforge_root),
        "miniforge_exists": miniforge_root.exists(),
        "conda": str(conda_bat),
        "conda_exists": conda_bat.exists(),
        "python": str(python_path),
        "python_exists": python_path.exists(),
        "env_root": str(env_root),
        "env_root_exists": env_root.exists(),
        "colmap": str(colmap_path),
        "colmap_exists": colmap_path.exists(),
        "git": str(git_path) if git_path else "",
        "git_exists": bool(git_path),
        "npm": str(npm_path) if npm_path else "",
        "npm_exists": bool(npm_path),
        "vs_build_tools": str(vs_install) if vs_install else "",
        "vs_build_tools_exists": bool(vs_install),
        "ffmpeg": str(ffmpeg_path),
        "ffmpeg_exists": ffmpeg_path.exists(),
        "crop_node_helper": str(crop_helper),
        "crop_node_helper_exists": crop_helper.exists(),
        "gaussian_dir": str(GAUSSIAN_DIR),
        "gaussian_dir_exists": GAUSSIAN_DIR.exists(),
        "two_dgs_dir": str(TWO_DGS_DIR),
        "two_dgs_dir_exists": TWO_DGS_DIR.exists(),
        "two_dgs_python": str(TWO_DGS_DIR / ".venv" / "Scripts" / "python.exe"),
        "two_dgs_python_exists": (TWO_DGS_DIR / ".venv" / "Scripts" / "python.exe").exists(),
        "two_dgs_train": str(TWO_DGS_DIR / "train.py"),
        "two_dgs_train_exists": (TWO_DGS_DIR / "train.py").exists(),
        "opencv_ok": opencv_ok,
        "opencv_error": None if opencv_ok else opencv_detail,
        "opencv_detail": opencv_detail if opencv_ok else None,
        "video_packages_ok": video_ok,
        "video_packages_error": None if video_ok else video_detail,
        "video_packages_detail": video_detail if video_ok else None,
        "runtime_imports_ok": runtime_ok,
        "runtime_imports_error": None if runtime_ok else runtime_detail,
        "runtime_imports_detail": runtime_detail if runtime_ok else None,
    }
    report.update(openmvs_environment_report())
    return report


def ensure_training_environment(backend="3dgs"):
    backend = safe_training_backend(backend)
    report = training_environment_report(backend)
    problems = []
    if not report["python_exists"]:
        problems.append(f"Missing Python executable: {report['python']}")
    if not report["gaussian_dir_exists"]:
        problems.append(f"Missing gaussian-splatting directory: {report['gaussian_dir']}")
    if backend == "2dgs":
        if not report["two_dgs_dir_exists"]:
            problems.append(f"Missing 2DGS directory: {report['two_dgs_dir']}")
        if not report["two_dgs_python_exists"]:
            problems.append(f"Missing 2DGS venv Python: {report['two_dgs_python']}")
        if not report["two_dgs_train_exists"]:
            problems.append(f"Missing 2DGS train.py: {report['two_dgs_train']}")
    if not report["colmap_exists"]:
        problems.append(f"Missing COLMAP executable: {report['colmap']}")
    if not report["opencv_ok"]:
        problems.append(f"OpenCV unavailable for video import: {report['opencv_error']}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return report


def sparse_adam_available():
    global SPARSE_ADAM_AVAILABLE_CACHE
    if SPARSE_ADAM_AVAILABLE_CACHE is not None:
        return SPARSE_ADAM_AVAILABLE_CACHE
    python_path = training_python("3dgs")
    if not python_path.exists() or not GAUSSIAN_DIR.exists():
        SPARSE_ADAM_AVAILABLE_CACHE = False
        return SPARSE_ADAM_AVAILABLE_CACHE
    probe = (
        "try:\n"
        "    from diff_gaussian_rasterization import SparseGaussianAdam\n"
        "    print('1')\n"
        "except Exception:\n"
        "    print('0')\n"
    )
    try:
        result = subprocess.run(
            [str(python_path), "-c", probe],
            cwd=str(GAUSSIAN_DIR),
            env=training_env("3dgs"),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
            check=False,
        )
        SPARSE_ADAM_AVAILABLE_CACHE = result.returncode == 0 and result.stdout.strip().splitlines()[-1:] == ["1"]
    except Exception:
        SPARSE_ADAM_AVAILABLE_CACHE = False
    return SPARSE_ADAM_AVAILABLE_CACHE


def resolve_training_options_for_environment(backend, options, job=None):
    backend = safe_training_backend(backend)
    resolved = dict(options)
    if backend == "3dgs" and resolved.get("optimizer_type") == "sparse_adam" and not sparse_adam_available():
        resolved["optimizer_type"] = "default"
        message = (
            "WARNING: sparse_adam requested but SparseGaussianAdam is unavailable in the 3DGS environment; "
            "falling back to default Adam. Install the gaussian-splatting 3dgs_accel rasterizer to enable sparse_adam."
        )
        if job is not None:
            add_job_log(job, message)
    return resolved


def command_failure_message(args, returncode, output_tail):
    command = " ".join(str(a) for a in args)
    output_text = "\n".join(output_tail)
    executable = Path(str(args[0])).name.lower() if args else ""
    subcommand = str(args[1]).lower() if len(args) > 1 else ""
    if executable == "colmap.exe" and subcommand == "mapper" and "No good initial image pair found" in output_text:
        return (
            "COLMAP could not initialize a sparse 3D reconstruction: no good initial image pair was found. "
            "The images matched, but they likely have too little camera translation/parallax, are near-duplicates, "
            "or were captured from almost the same viewpoint. Capture more photos while moving around the object "
            "with strong overlap, then import and train again."
        )
    return f"Command failed with exit code {returncode}: {command}"


def run_logged(job, args, cwd, backend=None):
    add_job_log(job, f"> {' '.join(str(a) for a in args)}")
    if backend == "sugar":
        process_env = sugar_env()
    elif backend == "gs2mesh":
        process_env = gs2mesh_env()
    else:
        process_env = training_env(backend or job.get("backend", "3dgs"))
    process = subprocess.Popen(
        [str(a) for a in args],
        cwd=str(cwd),
        env=process_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    lock = MESH_LOCK if mesh_like_job_kind(job) else TRAIN_LOCK
    with lock:
        job["process"] = process
    output_tail = []
    try:
        try:
            for line in process.stdout:
                if job.get("cancel_requested"):
                    process.terminate()
                    add_job_log(job, "Cancel requested. Terminating current command...")
                    break
                add_job_log(job, line)
                output_tail.append(line.rstrip())
                if len(output_tail) > 200:
                    output_tail = output_tail[-200:]
            rc = process.wait()
        except Exception:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except Exception:
                    process.kill()
                    process.wait()
            raise
    finally:
        with lock:
            if job.get("process") is process:
                job["process"] = None
    if job.get("cancel_requested"):
        raise RuntimeError("Training cancelled by user")
    if rc != 0:
        raise RuntimeError(command_failure_message(args, rc, output_tail))
    return rc


COLMAP_CAMERA_MODELS = {
    "SIMPLE_PINHOLE",
    "PINHOLE",
    "SIMPLE_RADIAL",
    "RADIAL",
    "OPENCV",
    "OPENCV_FISHEYE",
    "FULL_OPENCV",
}


def safe_bool(value, default=False):
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, str):
        return value.lower() in {"1", "true", "yes", "on"}
    return bool(value)


def safe_int_range(value, default, min_value=None, max_value=None):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        parsed = default
    if min_value is not None:
        parsed = max(min_value, parsed)
    if max_value is not None:
        parsed = min(max_value, parsed)
    return parsed


def safe_float_string(value, default, min_value=None, max_value=None):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        parsed = float(default)
    if min_value is not None:
        parsed = max(float(min_value), parsed)
    if max_value is not None:
        parsed = min(float(max_value), parsed)
    return f"{parsed:.12g}"


def safe_float_range(value, default, min_value=None, max_value=None):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        parsed = float(default)
    if min_value is not None:
        parsed = max(float(min_value), parsed)
    if max_value is not None:
        parsed = min(float(max_value), parsed)
    return parsed


def colmap_options_from_payload(payload):
    payload = payload if isinstance(payload, dict) else {}
    preset = payload.get("preset", "default")
    options = {
        "preset": preset if preset in {"default", "robust", "sequential"} else "default",
        "matching": "exhaustive",
        "camera_model": "OPENCV",
        "single_camera": True,
        "use_gpu": True,
        "reset": True,
        "feature_max_image_size": -1,
        "feature_max_num_features": 8192,
        "matcher_guided": False,
        "matcher_max_num_matches": 32768,
        "sequential_overlap": 10,
        "mapper_min_num_matches": 15,
        "mapper_multiple_models": True,
        "mapper_init_min_num_inliers": 100,
        "mapper_abs_pose_min_num_inliers": 30,
        "mapper_ba_global_function_tolerance": "0.000001",
        "mapper_ba_global_max_num_iterations": 50,
        "mapper_ba_global_frames_freq": 500,
        "mapper_ba_global_points_freq": 250000,
        "mapper_ba_refine_focal_length": True,
        "mapper_ba_refine_principal_point": False,
        "mapper_ba_refine_extra_params": True,
        "mapper_max_runtime_seconds": -1,
    }
    if options["preset"] == "robust":
        options.update({
            "feature_max_image_size": 2400,
            "feature_max_num_features": 12000,
            "matcher_guided": True,
            "mapper_min_num_matches": 10,
            "mapper_init_min_num_inliers": 50,
            "mapper_abs_pose_min_num_inliers": 20,
            "mapper_ba_global_function_tolerance": "0.00001",
            "mapper_ba_global_max_num_iterations": 15,
            "mapper_ba_global_frames_freq": 100,
            "mapper_ba_global_points_freq": 50000,
        })
    elif options["preset"] == "sequential":
        options.update({
            "matching": "sequential",
            "feature_max_image_size": 2400,
            "feature_max_num_features": 12000,
            "matcher_guided": True,
            "sequential_overlap": 20,
            "mapper_min_num_matches": 10,
            "mapper_init_min_num_inliers": 50,
            "mapper_abs_pose_min_num_inliers": 20,
            "mapper_ba_global_function_tolerance": "0.00001",
            "mapper_ba_global_max_num_iterations": 15,
            "mapper_ba_global_frames_freq": 100,
            "mapper_ba_global_points_freq": 50000,
        })

    if payload.get("matching") in {"exhaustive", "sequential"}:
        options["matching"] = payload["matching"]
    camera_model = str(payload.get("camera_model", options["camera_model"])).upper()
    if camera_model in COLMAP_CAMERA_MODELS:
        options["camera_model"] = camera_model

    options["single_camera"] = safe_bool(payload.get("single_camera"), options["single_camera"])
    options["use_gpu"] = safe_bool(payload.get("use_gpu"), options["use_gpu"])
    options["reset"] = safe_bool(payload.get("reset"), options["reset"])
    options["feature_max_image_size"] = safe_int_range(payload.get("feature_max_image_size"), options["feature_max_image_size"], -1, 12000)
    options["feature_max_num_features"] = safe_int_range(payload.get("feature_max_num_features"), options["feature_max_num_features"], 512, 50000)
    options["matcher_guided"] = safe_bool(payload.get("matcher_guided"), options["matcher_guided"])
    options["matcher_max_num_matches"] = safe_int_range(payload.get("matcher_max_num_matches"), options["matcher_max_num_matches"], 1024, 262144)
    options["sequential_overlap"] = safe_int_range(payload.get("sequential_overlap"), options["sequential_overlap"], 2, 100)
    options["mapper_min_num_matches"] = safe_int_range(payload.get("mapper_min_num_matches"), options["mapper_min_num_matches"], 4, 100)
    options["mapper_multiple_models"] = safe_bool(payload.get("mapper_multiple_models"), options["mapper_multiple_models"])
    options["mapper_init_min_num_inliers"] = safe_int_range(payload.get("mapper_init_min_num_inliers"), options["mapper_init_min_num_inliers"], 10, 1000)
    options["mapper_abs_pose_min_num_inliers"] = safe_int_range(payload.get("mapper_abs_pose_min_num_inliers"), options["mapper_abs_pose_min_num_inliers"], 10, 1000)
    if "mapper_ba_global_function_tolerance" in payload:
        options["mapper_ba_global_function_tolerance"] = safe_float_string(payload.get("mapper_ba_global_function_tolerance"), options["mapper_ba_global_function_tolerance"], 0, 1)
    options["mapper_ba_global_max_num_iterations"] = safe_int_range(payload.get("mapper_ba_global_max_num_iterations"), options["mapper_ba_global_max_num_iterations"], 1, 200)
    options["mapper_ba_global_frames_freq"] = safe_int_range(payload.get("mapper_ba_global_frames_freq"), options["mapper_ba_global_frames_freq"], 10, 10000)
    options["mapper_ba_global_points_freq"] = safe_int_range(payload.get("mapper_ba_global_points_freq"), options["mapper_ba_global_points_freq"], 1000, 10000000)
    options["mapper_ba_refine_focal_length"] = safe_bool(payload.get("mapper_ba_refine_focal_length"), options["mapper_ba_refine_focal_length"])
    options["mapper_ba_refine_principal_point"] = safe_bool(payload.get("mapper_ba_refine_principal_point"), options["mapper_ba_refine_principal_point"])
    options["mapper_ba_refine_extra_params"] = safe_bool(payload.get("mapper_ba_refine_extra_params"), options["mapper_ba_refine_extra_params"])
    options["mapper_max_runtime_seconds"] = safe_int_range(payload.get("mapper_max_runtime_seconds"), options["mapper_max_runtime_seconds"], -1, 86400)
    return options


def colmap_image_input_path(dataset):
    dataset = Path(dataset)
    input_dir = dataset / "input"
    return input_dir if input_dir.exists() else dataset / "images"


def dataset_has_mixed_image_dimensions(dataset):
    image_path = colmap_image_input_path(dataset)
    if not image_path.exists():
        return False
    try:
        from PIL import Image
    except Exception:
        return False
    dimensions = set()
    for image_file in image_path.iterdir():
        if not image_file.is_file() or image_file.suffix.lower() not in IMAGE_EXTS:
            continue
        try:
            with Image.open(image_file) as image:
                dimensions.add(image.size)
        except Exception:
            continue
        if len(dimensions) > 1:
            return True
    return False


def dataset_has_colmap_scene(dataset):
    sparse0 = Path(dataset) / "sparse" / "0"
    has_binary = all((sparse0 / name).exists() for name in ("cameras.bin", "images.bin", "points3D.bin"))
    has_text = all((sparse0 / name).exists() for name in ("cameras.txt", "images.txt", "points3D.txt"))
    has_ply = (sparse0 / "points3D.ply").exists() and (
        ((sparse0 / "cameras.bin").exists() and (sparse0 / "images.bin").exists())
        or ((sparse0 / "cameras.txt").exists() and (sparse0 / "images.txt").exists())
    )
    return has_binary or has_text or has_ply


def dataset_has_recognized_training_scene(dataset):
    dataset = Path(dataset)
    return dataset_has_colmap_scene(dataset) or (dataset / "transforms_train.json").exists()


def alignment_cache_dir(dataset):
    return Path(dataset) / ".alignment_cache"


def dataset_has_alignment_source(dataset):
    dataset = Path(dataset)
    return dataset_has_recognized_training_scene(dataset) or dataset_has_colmap_scene(alignment_cache_dir(dataset))


def save_alignment_cache(dataset, job=None):
    dataset = Path(dataset)
    if not dataset_has_colmap_scene(dataset):
        return False
    cache = alignment_cache_dir(dataset)
    sparse_cache = cache / "sparse"
    tmp_sparse = cache / f"sparse_tmp_{uuid.uuid4().hex[:8]}"
    tmp_sparse.parent.mkdir(parents=True, exist_ok=True)
    if tmp_sparse.exists():
        shutil.rmtree(tmp_sparse)
    shutil.copytree(dataset / "sparse", tmp_sparse)
    if sparse_cache.exists():
        shutil.rmtree(sparse_cache)
    shutil.move(str(tmp_sparse), str(sparse_cache))
    metadata = {
        "version": 1,
        "saved_at": time.time(),
        "source": "colmap",
        "sparse": "sparse",
    }
    (cache / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    if job is not None:
        add_job_log(job, f"Saved COLMAP alignment cache: {cache}")
    return True


def restore_alignment_cache(dataset, job=None):
    dataset = Path(dataset)
    cache = alignment_cache_dir(dataset)
    if not dataset_has_colmap_scene(cache):
        return False
    sparse = dataset / "sparse"
    if sparse.exists():
        shutil.rmtree(sparse)
    shutil.copytree(cache / "sparse", sparse)
    if job is not None:
        add_job_log(job, f"Restored cached COLMAP alignment: {cache}")
    return True


def append_option(command, flag, value, include=True):
    if include:
        command.extend([flag, str(value)])


def colmap_convert_commands(dataset, options):
    dataset = Path(dataset)
    colmap = colmap_executable()
    if not colmap:
        raise RuntimeError("Missing COLMAP executable")
    image_path = colmap_image_input_path(dataset)
    database_path = dataset / "distorted" / "database.db"
    sparse_path = dataset / "distorted" / "sparse"
    use_gpu = "1" if options["use_gpu"] else "0"
    feature = [
        colmap,
        "feature_extractor",
        "--database_path",
        database_path,
        "--image_path",
        image_path,
        "--ImageReader.single_camera",
        "1" if options["single_camera"] else "0",
        "--ImageReader.camera_model",
        options["camera_model"],
        "--FeatureExtraction.use_gpu",
        use_gpu,
        "--SiftExtraction.max_num_features",
        str(options["feature_max_num_features"]),
    ]
    append_option(feature, "--FeatureExtraction.max_image_size", options["feature_max_image_size"], options["feature_max_image_size"] > 0)

    matcher = [
        colmap,
        "sequential_matcher" if options["matching"] == "sequential" else "exhaustive_matcher",
        "--database_path",
        database_path,
        "--FeatureMatching.use_gpu",
        use_gpu,
        "--FeatureMatching.guided_matching",
        "1" if options["matcher_guided"] else "0",
        "--FeatureMatching.max_num_matches",
        str(options["matcher_max_num_matches"]),
    ]
    append_option(matcher, "--SequentialMatching.overlap", options["sequential_overlap"], options["matching"] == "sequential")

    mapper = [
        colmap,
        "mapper",
        "--database_path",
        database_path,
        "--image_path",
        image_path,
        "--output_path",
        sparse_path,
        "--Mapper.min_num_matches",
        str(options["mapper_min_num_matches"]),
        "--Mapper.multiple_models",
        "1" if options["mapper_multiple_models"] else "0",
        "--Mapper.init_min_num_inliers",
        str(options["mapper_init_min_num_inliers"]),
        "--Mapper.abs_pose_min_num_inliers",
        str(options["mapper_abs_pose_min_num_inliers"]),
        "--Mapper.ba_global_function_tolerance",
        str(options["mapper_ba_global_function_tolerance"]),
        "--Mapper.ba_global_max_num_iterations",
        str(options["mapper_ba_global_max_num_iterations"]),
        "--Mapper.ba_global_frames_freq",
        str(options["mapper_ba_global_frames_freq"]),
        "--Mapper.ba_global_points_freq",
        str(options["mapper_ba_global_points_freq"]),
        "--Mapper.ba_refine_focal_length",
        "1" if options["mapper_ba_refine_focal_length"] else "0",
        "--Mapper.ba_refine_principal_point",
        "1" if options["mapper_ba_refine_principal_point"] else "0",
        "--Mapper.ba_refine_extra_params",
        "1" if options["mapper_ba_refine_extra_params"] else "0",
    ]
    append_option(mapper, "--Mapper.max_runtime_seconds", options["mapper_max_runtime_seconds"], options["mapper_max_runtime_seconds"] > 0)

    undistorter = [
        colmap,
        "image_undistorter",
        "--image_path",
        image_path,
        "--input_path",
        sparse_path / "0",
        "--output_path",
        dataset,
        "--output_type",
        "COLMAP",
    ]
    return [feature, matcher, mapper, undistorter]


def normalize_undistorted_sparse(dataset):
    sparse = Path(dataset) / "sparse"
    sparse0 = sparse / "0"
    sparse0.mkdir(parents=True, exist_ok=True)
    for child in list(sparse.iterdir()):
        if child.name == "0":
            continue
        target = sparse0 / child.name
        if target.exists():
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()
        shutil.move(str(child), str(target))


def run_colmap_convert(job, dataset, options):
    dataset = Path(dataset)
    options = dict(options)
    if options.get("reset", True):
        database = dataset / "database.db"
        if database.exists():
            add_job_log(job, f"Removing stale COLMAP cache: {database}")
            database.unlink()
        for child in ("distorted", "sparse", "stereo"):
            path = dataset / child
            if path.exists():
                add_job_log(job, f"Removing stale COLMAP cache: {path}")
                shutil.rmtree(path)
    (dataset / "distorted" / "sparse").mkdir(parents=True, exist_ok=True)
    if options.get("single_camera") and dataset_has_mixed_image_dimensions(dataset):
        options["single_camera"] = False
        add_job_log(job, "WARNING: Detected mixed image dimensions; disabling COLMAP single_camera to avoid CAMERA_SINGLE_DIM_ERROR.")
    add_job_log(job, f"COLMAP preset: {options['preset']}, matching: {options['matching']}, camera: {options['camera_model']}")
    for command in colmap_convert_commands(dataset, options):
        run_logged(job, command, ROOT, "3dgs")
    normalize_undistorted_sparse(dataset)
    save_alignment_cache(dataset, job)


def train_args_for_quality(quality, backend="3dgs"):
    backend = safe_training_backend(backend)
    quality = quality or "quick"
    if backend == "2dgs":
        if quality == "quick":
            return {"iterations": 7000, "resolution": 8, "depth_ratio": 0.0}
        if quality == "full":
            return {"iterations": 30000, "resolution": 8, "depth_ratio": 0.0}
        if quality == "quality":
            return {"iterations": 30000, "resolution": 4, "depth_ratio": 0.0}
        if quality == "max_quality":
            return {"iterations": 30000, "resolution": 2, "depth_ratio": 0.0}
    else:
        if quality == "quick":
            return {
                "iterations": 7000,
                "resolution": 8,
                "antialiasing": False,
                "optimizer_type": "default",
                "exposure_compensation": False,
                "densify_grad_threshold": 0.0002,
                "densification_interval": 100,
                "densify_until_iter": 7000,
            }
        if quality == "full":
            return {
                "iterations": 30000,
                "resolution": 8,
                "antialiasing": False,
                "optimizer_type": "default",
                "exposure_compensation": False,
                "densify_grad_threshold": 0.0002,
                "densification_interval": 100,
                "densify_until_iter": 15000,
            }
        if quality == "quality":
            return {
                "iterations": 30000,
                "resolution": 4,
                "antialiasing": True,
                "optimizer_type": "sparse_adam",
                "exposure_compensation": False,
                "densify_grad_threshold": 0.00016,
                "densification_interval": 100,
                "densify_until_iter": 18000,
            }
        if quality == "max_quality":
            return {
                "iterations": 30000,
                "resolution": 2,
                "antialiasing": True,
                "optimizer_type": "sparse_adam",
                "exposure_compensation": True,
                "densify_grad_threshold": 0.00012,
                "densification_interval": 80,
                "densify_until_iter": 22000,
            }
    raise ValueError("Invalid training quality")


def training_options_from_payload(backend, quality, payload=None):
    backend = safe_training_backend(backend)
    payload = payload if isinstance(payload, dict) else {}
    options = train_args_for_quality(quality, backend)
    options["iterations"] = safe_int_range(payload.get("iterations"), options["iterations"], 1000, 200000)
    options["resolution"] = safe_int_range(payload.get("resolution"), options["resolution"], 1, 16)
    if backend == "2dgs":
        options["depth_ratio"] = safe_float_range(payload.get("depth_ratio"), options["depth_ratio"], 0.0, 1.0)
        return options

    optimizer_type = str(payload.get("optimizer_type", options["optimizer_type"]))
    options["optimizer_type"] = optimizer_type if optimizer_type in {"default", "sparse_adam"} else options["optimizer_type"]
    options["antialiasing"] = safe_bool(payload.get("antialiasing"), options["antialiasing"])
    options["exposure_compensation"] = safe_bool(payload.get("exposure_compensation"), options["exposure_compensation"])
    options["densify_grad_threshold"] = safe_float_range(
        payload.get("densify_grad_threshold"),
        options["densify_grad_threshold"],
        0.00001,
        0.01,
    )
    options["densification_interval"] = safe_int_range(
        payload.get("densification_interval"),
        options["densification_interval"],
        10,
        1000,
    )
    options["densify_until_iter"] = safe_int_range(
        payload.get("densify_until_iter"),
        min(options["densify_until_iter"], options["iterations"]),
        0,
        options["iterations"],
    )
    return options


def write_training_metadata(output, backend, quality=None, options=None):
    metadata = {
        "backend": backend,
        "created_at": time.time(),
        "source": "3DGS Editor",
    }
    if quality:
        metadata["quality"] = quality
    output.mkdir(parents=True, exist_ok=True)
    with open(output / "training_backend.json", "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2)
    if options:
        scene_metadata = {
            "name": output.name,
            "created_at": metadata["created_at"],
            "source": "3DGS Editor",
            "training": {
                "backend": backend,
                "quality": quality,
                "options": options,
            },
        }
        with open(output / "scene.json", "w", encoding="utf-8") as f:
            json.dump(scene_metadata, f, indent=2)


def training_command(backend, dataset, output, options):
    backend = safe_training_backend(backend)
    iterations = options["iterations"]
    if backend == "2dgs":
        return [
            training_python("2dgs"),
            "train.py",
            "-s",
            dataset,
            "-m",
            output,
            "-r",
            str(options["resolution"]),
            "--data_device",
            "cpu",
            "--iterations",
            str(iterations),
            "--test_iterations",
            str(iterations),
            "--save_iterations",
            str(iterations),
            "--checkpoint_iterations",
            str(iterations),
            "--depth_ratio",
            str(options["depth_ratio"]),
        ]

    command = [
        training_python("3dgs"),
        "train.py",
        "-s",
        dataset,
        "-m",
        output,
        "-r",
        str(options["resolution"]),
        "--data_device",
        "cpu",
        "--iterations",
        str(iterations),
        "--test_iterations",
        str(iterations),
        "--save_iterations",
        str(iterations),
        "--checkpoint_iterations",
        str(iterations),
    ]
    if options["antialiasing"]:
        command.append("--antialiasing")
    if options.get("optimizer_type") and options["optimizer_type"] != "default":
        command.extend(["--optimizer_type", options["optimizer_type"]])
    if "densify_grad_threshold" in options:
        command.extend(["--densify_grad_threshold", f"{options['densify_grad_threshold']:.12g}"])
    if "densification_interval" in options:
        command.extend(["--densification_interval", str(options["densification_interval"])])
    if "densify_until_iter" in options:
        command.extend(["--densify_until_iter", str(options["densify_until_iter"])])
    if options.get("exposure_compensation"):
        command.extend([
            "--exposure_lr_init",
            "0.001",
            "--exposure_lr_final",
            "0.0001",
            "--exposure_lr_delay_steps",
            "5000",
            "--exposure_lr_delay_mult",
            "0.001",
        ])
    return command


def run_training_job(job, run_convert, quality, overwrite):
    try:
        backend = safe_training_backend(job.get("backend", "3dgs"))
        set_job_stage(job, "running", "environment")
        report = ensure_training_environment(backend)
        add_job_log(job, f"Backend: {backend.upper()}")
        add_job_log(job, f"Python: {report['python']}")
        if backend == "2dgs":
            add_job_log(job, f"2DGS: {report['two_dgs_dir']}")
        add_job_log(job, f"COLMAP: {report['colmap']}")
        add_job_log(job, "OpenCV: OK")
        dataset = DATASETS_DIR / job["scene"]
        output = OUTPUT_DIR / job["output_scene"]
        images_dir = dataset / "images"
        if not images_dir.exists() or not any(p.suffix.lower() in IMAGE_EXTS for p in images_dir.iterdir()):
            raise ValueError(f"No training images found: {images_dir}")
        if output.exists() and any(output.iterdir()):
            ensure_output_backend_compatible(job["output_scene"], backend)
            if overwrite:
                add_job_log(job, f"Removing existing output: {output}")
                shutil.rmtree(output)
            elif job.get("allow_existing_output"):
                add_job_log(job, f"Using existing output directory: {output}")
            else:
                raise ValueError(f"Output already exists: {output}. Enable overwrite or choose another output name.")

        set_job_stage(job, "running", "prepare")
        run_logged(job, ["cmd.exe", "/d", "/s", "/c", TRAINING_KIT_DIR / "apply_local_fixes.bat"], ROOT, "3dgs")

        if run_convert:
            set_job_stage(job, "running", "colmap")
            run_colmap_convert(job, dataset, job.get("colmap_options") or colmap_options_from_payload({}))
        elif not dataset_has_recognized_training_scene(dataset):
            if not restore_alignment_cache(dataset, job):
                add_job_log(job, "Existing dataset has no recognized sparse/0 or transforms_train.json; running COLMAP alignment first.")
                set_job_stage(job, "running", "colmap")
                run_colmap_convert(job, dataset, job.get("colmap_options") or colmap_options_from_payload({}))

        options = training_options_from_payload(backend, quality, job.get("train_options"))
        options = resolve_training_options_for_environment(backend, options, job)
        add_job_log(job, f"Training preset: {quality}, iterations={options['iterations']}, resolution={options['resolution']}")
        if backend == "3dgs":
            add_job_log(
                job,
                "3DGS quality options: "
                f"optimizer={options['optimizer_type']}, "
                f"aa={options['antialiasing']}, "
                f"exposure={options['exposure_compensation']}, "
                f"densify_until={options['densify_until_iter']}, "
                f"densify_interval={options['densification_interval']}, "
                f"densify_grad={options['densify_grad_threshold']:.12g}",
            )
        command = training_command(backend, dataset, output, options)
        set_job_stage(job, "running", "train")
        run_logged(job, command, TWO_DGS_DIR if backend == "2dgs" else GAUSSIAN_DIR, backend)
        write_training_metadata(output, backend, quality, options)
        with TRAIN_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["latest_iteration"] = latest_iteration(job["output_scene"])
            persist_train_job(job)
        add_job_log(job, f"{backend.upper()} training complete: output/{job['output_scene']}")
    except Exception as exc:
        with TRAIN_LOCK:
            if job.get("cancel_requested"):
                job["status"] = "cancelled"
                job["stage"] = "cancelled"
            else:
                job["status"] = "failed"
                job["stage"] = "failed"
            job["error"] = str(exc)
            job["returncode"] = 1
            job["updated_at"] = time.time()
            persist_train_job(job)
        add_job_log(job, f"ERROR: {exc}")


def start_training(scene, output_scene, quality="quick", run_convert=True, overwrite=False, backend="3dgs", colmap_options=None, train_options=None, allow_existing_output=False):
    scene = safe_name(scene)
    output_scene = safe_name(output_scene or scene)
    backend = safe_training_backend(backend)
    job_id = uuid.uuid4().hex
    job = {
        "id": job_id,
        "scene": scene,
        "output_scene": output_scene,
        "backend": backend,
        "quality": quality,
        "status": "queued",
        "stage": "queued",
        "created_at": time.time(),
        "updated_at": time.time(),
        "returncode": None,
        "error": None,
        "cancel_requested": False,
        "allow_existing_output": bool(allow_existing_output),
        "process": None,
        "colmap_options": colmap_options_from_payload(colmap_options or {}),
        "train_options": train_options if isinstance(train_options, dict) else {},
        "log": [],
    }
    with TRAIN_LOCK:
        TRAIN_JOBS[job_id] = job
        persist_train_job(job)
    thread = threading.Thread(target=run_training_job, args=(job, run_convert, quality, overwrite), daemon=True)
    thread.start()
    return job_snapshot(job)


def terminate_process(process, timeout=5):
    if not process or process.poll() is not None:
        return False
    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=timeout,
                check=False,
            )
            try:
                process.wait(timeout=timeout)
            except Exception:
                pass
            return True
        except Exception:
            pass
    try:
        process.terminate()
        process.wait(timeout=timeout)
        return True
    except Exception:
        try:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=timeout)
            return True
        except Exception:
            return False


def cancel_training(job_id):
    with TRAIN_LOCK:
        job = TRAIN_JOBS.get(job_id)
        if not job:
            raise ValueError("Training job not found")
        job["cancel_requested"] = True
        job["status"] = "cancelling"
        job["updated_at"] = time.time()
        process = job.get("process")
        persist_train_job(job)
    terminate_process(process)
    add_job_log(job, "Cancel requested.")
    return job_snapshot(job)


def run_splat_export_job(job):
    try:
        set_job_stage(job, "running", "export")
        add_job_log(job, f"Scene: output/{job['scene']}")
        add_job_log(job, f"Iteration: {job['iteration']}")
        add_job_log(job, f"Format: {job['format'].upper()}")
        path = export_splat_format(job["scene"], job["iteration"], job["format"])
        if job.get("cancel_requested"):
            raise RuntimeError("Splat export cancelled by user")
        with SPLAT_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["output_path"] = str(path)
            job["download_url"] = (
                f"/api/splat/export?scene={job['scene']}&iteration={job['iteration']}"
                f"&format={job['format']}&cached=true"
            )
            persist_splat_job(job)
        add_job_log(job, f"Export complete: {path}")
    except Exception as exc:
        with SPLAT_LOCK:
            if job.get("cancel_requested"):
                job["status"] = "cancelled"
                job["stage"] = "cancelled"
            else:
                job["status"] = "failed"
                job["stage"] = "failed"
            job["error"] = str(exc)
            job["returncode"] = 1
            job["updated_at"] = time.time()
            persist_splat_job(job)
        add_job_log(job, f"ERROR: {exc}")


def start_splat_export(scene, iteration, fmt):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    fmt = (fmt or "spz").lower()
    if fmt not in SPLAT_FORMATS:
        raise ValueError("Unsupported splat format")
    job_id = uuid.uuid4().hex
    job = {
        "id": job_id,
        "kind": "splat_export",
        "scene": scene,
        "iteration": iteration,
        "format": fmt,
        "status": "queued",
        "stage": "queued",
        "created_at": time.time(),
        "updated_at": time.time(),
        "returncode": None,
        "error": None,
        "cancel_requested": False,
        "process": None,
        "output_path": None,
        "download_url": None,
        "log": [],
    }
    with SPLAT_LOCK:
        SPLAT_JOBS[job_id] = job
        persist_splat_job(job)
    thread = threading.Thread(target=run_splat_export_job, args=(job,), daemon=True)
    thread.start()
    return splat_job_snapshot(job)


def cancel_splat_job(job_id):
    with SPLAT_LOCK:
        job = SPLAT_JOBS.get(job_id)
        if not job:
            raise ValueError("Splat export job not found")
        if job.get("status") in {"done", "failed", "cancelled"}:
            return splat_job_snapshot(job)
        job["cancel_requested"] = True
        job["status"] = "cancelling"
        job["updated_at"] = time.time()
        persist_splat_job(job)
    add_job_log(job, "Cancel requested. Export will stop before download is published.")
    return splat_job_snapshot(job)


def unified_job_snapshot(kind, job):
    if kind == "training":
        snapshot = job_snapshot(job)
        snapshot["title"] = f"{snapshot.get('backend', '3dgs').upper()} training"
        snapshot["output_dir"] = str(OUTPUT_DIR / snapshot["output_scene"])
        snapshot["can_retry"] = snapshot["status"] in {"failed", "cancelled"}
        return snapshot
    if kind == "mesh":
        snapshot = psnr_job_snapshot(job) if job.get("kind") == "psnr" else mesh_job_snapshot(job)
        if snapshot.get("kind") == "psnr":
            snapshot["title"] = "PSNR analysis"
            snapshot["output_dir"] = job.get("output_dir") or str(PSNR_REPORTS_DIR)
        else:
            snapshot["title"] = "Texture bake" if snapshot.get("kind") == "texture" else "Mesh export"
            snapshot["output_dir"] = str(model_dir(snapshot["scene"]) / "train" / f"ours_{int(snapshot['iteration'])}")
        snapshot["can_retry"] = snapshot["status"] in {"failed", "cancelled"}
        return snapshot
    if kind == "splat":
        snapshot = splat_job_snapshot(job)
        snapshot["title"] = f"{snapshot['format'].upper()} export"
        snapshot["output_dir"] = str(ply_path(snapshot["scene"], snapshot["iteration"]).parent)
        snapshot["can_retry"] = snapshot["status"] in {"failed", "cancelled"}
        return snapshot
    raise ValueError("Invalid job kind")


def list_all_jobs(limit=50):
    jobs = []
    with TRAIN_LOCK:
        jobs.extend(unified_job_snapshot("training", job) for job in TRAIN_JOBS.values())
    with MESH_LOCK:
        jobs.extend(unified_job_snapshot("mesh", job) for job in MESH_JOBS.values())
    with SPLAT_LOCK:
        jobs.extend(unified_job_snapshot("splat", job) for job in SPLAT_JOBS.values())
    jobs.sort(key=lambda item: item.get("updated_at") or item.get("created_at") or 0, reverse=True)
    return jobs[: max(1, int(limit or 50))]


def find_unified_job(job_id):
    with TRAIN_LOCK:
        if job_id in TRAIN_JOBS:
            return "training", TRAIN_JOBS[job_id], job_snapshot(TRAIN_JOBS[job_id])
    with MESH_LOCK:
        if job_id in MESH_JOBS:
            return "mesh", MESH_JOBS[job_id], mesh_job_snapshot(MESH_JOBS[job_id])
    with SPLAT_LOCK:
        if job_id in SPLAT_JOBS:
            return "splat", SPLAT_JOBS[job_id], splat_job_snapshot(SPLAT_JOBS[job_id])
    raise ValueError("Job not found")


def cancel_unified_job(job_id):
    kind, _, _ = find_unified_job(job_id)
    if kind == "training":
        return unified_job_snapshot("training", cancel_training(job_id))
    if kind == "mesh":
        return unified_job_snapshot("mesh", cancel_mesh_job(job_id))
    return unified_job_snapshot("splat", cancel_splat_job(job_id))


def retry_unified_job(job_id):
    kind, job, _ = find_unified_job(job_id)
    status = job.get("status")
    if status not in {"failed", "cancelled"}:
        raise ValueError("Only failed or cancelled jobs can be retried")
    if kind == "splat":
        return unified_job_snapshot("splat", start_splat_export(job["scene"], job["iteration"], job["format"]))
    if kind == "mesh":
        if job.get("kind") == "texture":
            return unified_job_snapshot("mesh", start_texture_bake(job["scene"], job["iteration"], job.get("options", {})))
        return unified_job_snapshot("mesh", start_mesh_export(job["scene"], job["iteration"], job.get("options", {})))
    return unified_job_snapshot(
        "training",
        start_training(
            job["scene"],
            job.get("output_scene") or job["scene"],
            job.get("quality", "quick"),
            False,
            False,
            job.get("backend", "3dgs"),
            job.get("colmap_options", {}),
            job.get("train_options", {}),
            True,
        ),
    )


def open_job_output_dir(job_id):
    kind, job, _ = find_unified_job(job_id)
    snapshot = unified_job_snapshot("splat" if kind == "splat" else kind, job)
    output_dir = Path(snapshot.get("output_dir") or ROOT)
    output_dir.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        os.startfile(str(output_dir))
    else:
        webbrowser.open(output_dir.as_uri())
    return {"ok": True, "path": str(output_dir)}


def mesh_job_snapshot(job):
    return {
        "id": job["id"],
        "kind": job.get("kind", "mesh"),
        "scene": job["scene"],
        "iteration": job["iteration"],
        "mode": job["mode"],
        "status": job["status"],
        "stage": job["stage"],
        "created_at": job["created_at"],
        "updated_at": job["updated_at"],
        "returncode": job.get("returncode"),
        "error": job.get("error"),
        "output_mesh": job.get("output_mesh"),
        "download_url": job.get("download_url"),
        "texture": job.get("texture"),
        "texture_download_url": job.get("texture_download_url"),
        "log": job["log"][-240:],
        "cancel_requested": bool(job.get("cancel_requested")),
    }


def cancel_mesh_job(job_id):
    with MESH_LOCK:
        job = MESH_JOBS.get(job_id)
        if not job:
            raise ValueError("Mesh job not found")
        job["cancel_requested"] = True
        job["status"] = "cancelling"
        job["updated_at"] = time.time()
        process = job.get("process")
    terminate_process(process)
    add_job_log(job, "Cancel requested.")
    return psnr_job_snapshot(job) if job.get("kind") == "psnr" else mesh_job_snapshot(job)


def shutdown_active_jobs(reason="server shutdown"):
    stopped = {"training": 0, "mesh": 0}
    with TRAIN_LOCK:
        training_jobs = list(TRAIN_JOBS.values())
        for job in training_jobs:
            if job.get("status") in {"done", "failed", "cancelled"}:
                continue
            job["cancel_requested"] = True
            job["status"] = "cancelling"
            job["updated_at"] = time.time()
            persist_train_job(job)
    for job in training_jobs:
        if job.get("cancel_requested"):
            process = job.get("process")
            if terminate_process(process):
                stopped["training"] += 1
            add_job_log(job, f"Cancel requested: {reason}")

    with MESH_LOCK:
        mesh_jobs = list(MESH_JOBS.values())
        for job in mesh_jobs:
            if job.get("status") in {"done", "failed", "cancelled"}:
                continue
            job["cancel_requested"] = True
            job["status"] = "cancelling"
            job["updated_at"] = time.time()
    for job in mesh_jobs:
        if job.get("cancel_requested"):
            process = job.get("process")
            if terminate_process(process):
                stopped["mesh"] += 1
            add_job_log(job, f"Cancel requested: {reason}")

    with SPLAT_LOCK:
        splat_jobs = list(SPLAT_JOBS.values())
        for job in splat_jobs:
            if job.get("status") in {"done", "failed", "cancelled"}:
                continue
            job["cancel_requested"] = True
            job["status"] = "cancelled"
            job["stage"] = "cancelled"
            job["updated_at"] = time.time()
            persist_splat_job(job)
            stopped["splat"] = stopped.get("splat", 0) + 1
    for job in splat_jobs:
        if job.get("cancel_requested"):
            add_job_log(job, f"Cancel requested: {reason}")
    return stopped


def run_mesh_export_job(job):
    try:
        set_job_stage(job, "running", "environment")
        options = job.get("options", {})
        if options.get("mode") == "sugar":
            ensure_sugar_environment()
        elif options.get("mode") == "gs2mesh":
            ensure_gs2mesh_environment()
        else:
            ensure_mesh_environment()
        command, cwd, output_path = mesh_export_command(job["scene"], job["iteration"], options)
        if options.get("mode") == "sugar":
            backend_label = "SuGaR"
            run_backend = "sugar"
        elif options.get("mode") == "gs2mesh":
            backend_label = "GS2Mesh"
            run_backend = "gs2mesh"
        else:
            backend_label = "2DGS"
            run_backend = "2dgs"
        add_job_log(job, f"Backend: {backend_label}")
        add_job_log(job, f"Scene: output/{job['scene']}")
        add_job_log(job, f"Mode: {job['mode']}")
        add_job_log(job, f"Output mesh: {output_path}")
        set_job_stage(job, "running", "mesh")
        started_at = time.time()
        run_logged(job, command, cwd, run_backend)
        texture_result = None
        if options.get("mode") == "sugar":
            cfg = load_cfg_args(model_dir(job["scene"]))
            source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / job["scene"])
            texture_result = collect_sugar_mesh_outputs(job["scene"], job["iteration"], source_path, started_at)
            if texture_result.get("glb_error"):
                add_job_log(job, f"WARNING: GLB export skipped: {texture_result['glb_error']}")
        elif options.get("mode") == "gs2mesh":
            gs2mesh_result = collect_gs2mesh_mesh_output(job["scene"], job["iteration"], started_at)
            add_job_log(job, f"GS2Mesh cleaned mesh: {gs2mesh_result['source']}")
        if not output_path.exists():
            raise RuntimeError(f"Mesh export finished but output file was not found: {output_path}")
        with MESH_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["output_mesh"] = str(output_path)
            job["download_url"] = f"/api/mesh/file?scene={job['scene']}&iteration={job['iteration']}&mode={job['mode']}&post=true"
            if texture_result:
                job["texture"] = texture_result
                job["texture_download_url"] = mesh_texture_file_url(
                    job["scene"], job["iteration"], job["mode"], "zip", True
                )
        add_job_log(job, f"Mesh export complete: {output_path}")
    except Exception as exc:
        with MESH_LOCK:
            if job.get("cancel_requested"):
                job["status"] = "cancelled"
                job["stage"] = "cancelled"
            else:
                job["status"] = "failed"
                job["stage"] = "failed"
            job["error"] = str(exc)
            job["returncode"] = 1
            job["updated_at"] = time.time()
        add_job_log(job, f"ERROR: {exc}")


def start_mesh_export(scene, iteration, options=None):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    options = mesh_export_options(options)
    job_id = uuid.uuid4().hex
    job = {
        "id": job_id,
        "kind": "mesh",
        "scene": scene,
        "iteration": iteration,
        "mode": options["mode"],
        "options": options,
        "status": "queued",
        "stage": "queued",
        "created_at": time.time(),
        "updated_at": time.time(),
        "returncode": None,
        "error": None,
        "cancel_requested": False,
        "process": None,
        "output_mesh": None,
        "download_url": None,
        "texture": None,
        "texture_download_url": None,
        "log": [],
    }
    with MESH_LOCK:
        MESH_JOBS[job_id] = job
    thread = threading.Thread(target=run_mesh_export_job, args=(job,), daemon=True)
    thread.start()
    return mesh_job_snapshot(job)


def psnr_job_snapshot(job):
    snapshot = mesh_job_snapshot(job)
    snapshot["title"] = "PSNR analysis"
    snapshot["backend"] = job.get("backend")
    snapshot["count"] = job.get("count")
    snapshot["eval_width"] = job.get("eval_width")
    snapshot["fallback_reason"] = job.get("fallback_reason")
    snapshot["analysis_filter_profile"] = job.get("analysis_filter_profile")
    snapshot["source_path"] = job.get("source_path")
    snapshot["output_dir"] = job.get("output_dir")
    snapshot["result"] = job.get("result")
    return snapshot


def psnr_output_dir(scene, iteration, backend, count):
    stamp = time.strftime("%Y%m%d_%H%M%S")
    return PSNR_REPORTS_DIR / safe_name(scene) / f"{backend}_iter_{int(iteration)}_n{int(count)}_{stamp}"


def psnr_width_attempts(requested_width):
    requested = max(0, int(requested_width or 0))
    attempts = [requested]
    for fallback in (1600, 1200, 800, 512):
        if requested == 0 or fallback < requested:
            attempts.append(fallback)
    deduped = []
    for width in attempts:
        if width not in deduped:
            deduped.append(width)
    return deduped


def is_retryable_3dgs_rasterizer_error(log_lines):
    text = "\n".join(str(line) for line in log_lines).lower()
    retryable_markers = (
        "storage size calculation overflowed",
        "all psnr safe-mode views failed",
        "cuda error: an illegal memory access",
        "illegal memory access was encountered",
        "cuda out of memory",
        "out of memory",
        "cublas_status_alloc_failed",
        "cudnn_status_alloc_failed",
    )
    return any(marker in text for marker in retryable_markers)


def psnr_filter_profiles(backend):
    if backend != "3dgs":
        return [{"name": "raw", "prune_opacity": 0.0, "max_scale": 0.0, "max_gaussians": 0}]
    return [
        {"name": "raw", "prune_opacity": 0.0, "max_scale": 0.0, "max_gaussians": 0},
        {"name": "filtered", "prune_opacity": 0.01, "max_scale": 0.5, "max_gaussians": 500000},
        {"name": "filtered_strong", "prune_opacity": 0.02, "max_scale": 0.25, "max_gaussians": 250000},
    ]


def raise_if_cancelled(job, label="Job"):
    if job.get("cancel_requested"):
        raise RuntimeError(f"{label} cancelled by user")


def run_psnr_job(job):
    try:
        set_job_stage(job, "running", "environment")
        raise_if_cancelled(job, "PSNR analysis")
        backend = safe_training_backend(job.get("backend") or scene_backend(job["scene"], job["iteration"]))
        model_path = model_dir(job["scene"])
        if not model_path.exists():
            raise FileNotFoundError(f"Model output not found: {model_path}")
        cfg = load_cfg_args(model_path)
        source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / job["scene"])
        if not source_path.exists():
            raise FileNotFoundError(f"Source dataset not found: {source_path}")
        output_dir = Path(job["output_dir"])
        output_dir.mkdir(parents=True, exist_ok=True)

        if scene_backend(job["scene"], job["iteration"]) != backend:
            raise RuntimeError(
                f"Scene backend is {scene_backend(job['scene'], job['iteration']).upper()}, "
                f"but PSNR backend is {backend.upper()}."
            )

        add_job_log(job, f"Backend: {backend.upper()}")
        add_job_log(job, f"Scene: output/{job['scene']}")
        add_job_log(job, f"Iteration: {job['iteration']}")
        add_job_log(job, f"Source images: {source_path}")
        add_job_log(job, f"Fixed camera count: {job['count']}")
        add_job_log(job, f"Evaluation width: {job['eval_width'] or 'model cfg'}")
        add_job_log(job, f"PSNR output: {output_dir}")
        set_job_stage(job, "running", "render")
        raise_if_cancelled(job, "PSNR analysis")
        def psnr_command(width, target_dir, profile):
            command = [
                training_python(backend),
                ROOT / "scripts" / "render_psnr_views.py",
                "--backend",
                backend,
                "--root",
                ROOT,
                "--source",
                source_path,
                "--model",
                model_path,
                "--iteration",
                int(job["iteration"]),
                "--count",
                int(job["count"]),
                "--eval-width",
                int(width),
                "--output-dir",
                target_dir,
                "--prune-opacity",
                profile["prune_opacity"],
                "--max-scale",
                profile["max_scale"],
                "--max-gaussians",
                profile["max_gaussians"],
            ]
            if backend == "2dgs":
                command.extend(["--two-dgs-dir", TWO_DGS_DIR])
            if backend == "3dgs":
                command.append("--safe-mode")
            return command

        last_error = None
        attempts = psnr_width_attempts(job["eval_width"]) if backend == "3dgs" else [int(job["eval_width"])]
        profiles = psnr_filter_profiles(backend)
        completed = False
        for profile_index, profile in enumerate(profiles):
            raise_if_cancelled(job, "PSNR analysis")
            if profile_index > 0:
                with MESH_LOCK:
                    job["fallback_reason"] = "Original 3DGS could not be rendered by the rasterizer; PSNR is using analysis-only Gaussian filtering"
                    job["stage"] = "render_filtered"
                    job["updated_at"] = time.time()
                add_job_log(job, (
                    "WARNING: Retrying PSNR with analysis-only Gaussian filtering "
                    f"({profile['name']}: opacity>={profile['prune_opacity']}, "
                    f"scale<={profile['max_scale']}, max={profile['max_gaussians']})."
                ))
            for index, width in enumerate(attempts):
                raise_if_cancelled(job, "PSNR analysis")
                if index > 0:
                    with MESH_LOCK:
                        job["eval_width"] = width
                        if not job.get("fallback_reason"):
                            job["fallback_reason"] = "3DGS rasterizer CUDA memory error; PSNR was retried at a lower evaluation width"
                        job["stage"] = "render_fallback"
                        job["updated_at"] = time.time()
                    add_job_log(job, f"WARNING: Retrying PSNR at width {width} after 3DGS rasterizer CUDA memory error.")
                if output_dir.exists():
                    shutil.rmtree(output_dir)
                output_dir.mkdir(parents=True, exist_ok=True)
                before_log_count = len(job.get("log", []))
                try:
                    run_logged(job, psnr_command(width, output_dir, profile), ROOT, backend)
                    raise_if_cancelled(job, "PSNR analysis")
                    last_error = None
                    completed = True
                    with MESH_LOCK:
                        job["eval_width"] = width
                        job["analysis_filter_profile"] = profile
                    break
                except RuntimeError as exc:
                    if job.get("cancel_requested"):
                        raise
                    last_error = exc
                    new_lines = job.get("log", [])[before_log_count:]
                    retryable = is_retryable_3dgs_rasterizer_error(new_lines)
                    last_width = index == len(attempts) - 1
                    last_profile = profile_index == len(profiles) - 1
                    if backend != "3dgs" or not retryable or (last_width and last_profile):
                        raise
                    if last_width:
                        add_job_log(job, f"3DGS rasterizer failed for all widths using profile {profile['name']}; trying next filter profile.")
                    else:
                        add_job_log(job, f"3DGS rasterizer CUDA memory error at width {width or 'model cfg'}; trying a lower width.")
            if completed:
                break
        if last_error is not None:
            raise last_error
        result_path = output_dir / "psnr_results.json"
        if not result_path.exists():
            raise RuntimeError(f"PSNR finished but result was not found: {result_path}")
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if job.get("fallback_reason"):
            result["fallback_reason"] = job.get("fallback_reason")
            result["analysis_filter_profile"] = job.get("analysis_filter_profile")
            result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        with MESH_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["source_path"] = str(source_path)
            job["output_dir"] = str(output_dir)
            job["result"] = result
        add_job_log(job, f"Average PSNR: {result.get('average_psnr'):.4f} dB")
        add_job_log(job, f"Rendered: {result.get('rendered_count')} / requested {result.get('requested_count')}")
    except Exception as exc:
        with MESH_LOCK:
            if job.get("cancel_requested"):
                job["status"] = "cancelled"
                job["stage"] = "cancelled"
            else:
                job["status"] = "failed"
                job["stage"] = "failed"
            job["error"] = str(exc)
            job["returncode"] = 1
            job["updated_at"] = time.time()
        add_job_log(job, f"ERROR: {exc}")


def start_psnr_analysis(scene, iteration, backend=None, count=20, eval_width=0):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    if iteration is None:
        raise ValueError("No iteration available for PSNR analysis")
    backend = safe_training_backend(backend or scene_backend(scene, iteration))
    count = max(1, min(500, int(count or 20)))
    eval_width = max(0, min(12000, int(eval_width or 0)))
    job_id = uuid.uuid4().hex
    output_dir = psnr_output_dir(scene, iteration, backend, count)
    job = {
        "id": job_id,
        "kind": "psnr",
        "scene": scene,
        "iteration": iteration,
        "mode": "psnr",
        "backend": backend,
        "count": count,
        "eval_width": eval_width,
        "status": "queued",
        "stage": "queued",
        "created_at": time.time(),
        "updated_at": time.time(),
        "returncode": None,
        "error": None,
        "cancel_requested": False,
        "process": None,
        "output_mesh": None,
        "download_url": None,
        "texture": None,
        "texture_download_url": None,
        "source_path": None,
        "output_dir": str(output_dir),
        "result": None,
        "fallback_reason": None,
        "analysis_filter_profile": None,
        "log": [],
    }
    with MESH_LOCK:
        MESH_JOBS[job_id] = job
    thread = threading.Thread(target=run_psnr_job, args=(job,), daemon=True)
    thread.start()
    return psnr_job_snapshot(job)


def run_texture_bake_job(job):
    try:
        set_job_stage(job, "running", "texture")
        options = job.get("options", {})
        source_path = mesh_output_path(job["scene"], job["iteration"], options["mode"], options["post"])
        if not source_path.exists():
            raise FileNotFoundError(f"Mesh not found: {source_path}. Export Mesh first.")
        output_paths = mesh_texture_paths(job["scene"], job["iteration"], options["mode"], options["post"])
        cfg = load_cfg_args(model_dir(job["scene"]))
        dataset_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / job["scene"])
        add_job_log(job, f"Source mesh: {source_path}")
        add_job_log(job, f"Source photos: {dataset_path}")
        add_job_log(job, f"Texture resolution: {options['texture_res']}")
        add_job_log(job, f"Max bake faces: {options['max_faces'] or 'no limit'}")
        if options["mode"] == "gs2mesh":
            add_job_log(job, "GS2Mesh texture bake uses the exported mesh geometry plus COLMAP/OpenMVS photo cameras; vertex colors are not used as texture.")
        if options.get("backend") == "openmvs":
            add_job_log(job, "Texture bake: OpenMVS InterfaceCOLMAP + TextureMesh")
            add_job_log(job, "OpenMVS cache: reusing scene.mvs when camera alignment has not changed.")
            if options.get("local_seam_leveling"):
                add_job_log(job, "Existing texture files will be kept until OpenMVS finishes successfully.")
            collected = run_openmvs_texture_bake(job, output_paths, options)
            if collected.get("glb_error"):
                add_job_log(job, f"WARNING: GLB export skipped: {collected['glb_error']}")
            if collected.get("local_seam_leveling_error"):
                add_job_log(job, "WARNING: Texture bake completed without local seam-leveling because OpenMVS crashed with it enabled.")
        elif options.get("backend") == "colmap":
            add_job_log(job, "Texture bake: COLMAP mesh_texturer")
            add_job_log(job, "COLMAP cache: reusing staged sparse camera model and images when unchanged.")
            scale, face_count, scale_reason = colmap_texture_scale_factor(source_path, options["texture_res"])
            add_job_log(job, f"Source mesh faces: {face_count}")
            add_job_log(job, f"COLMAP texture scale factor: {scale:g} ({scale_reason})")
            command, work_dir, texturer_output = colmap_texturer_command(job["scene"], job["iteration"], options)
            run_logged(job, command, work_dir, "2dgs")
            collected = collect_colmap_texture_outputs(texturer_output, output_paths)
            if collected.get("glb_error"):
                add_job_log(job, f"WARNING: GLB export skipped: {collected['glb_error']}")
        else:
            add_job_log(job, "Texture bake: Python camera projection + xatlas UV unwrap")
            add_job_log(job, f"Max source images: {options['max_images']}")
            command = texture_bake_command(
                source_path,
                output_paths["dir"],
                options["texture_res"],
                options["padding"],
                options["max_faces"],
                dataset_path,
                options["bake_source"],
                options["max_images"],
            )
            run_logged(job, command, TWO_DGS_DIR, "2dgs")
            collected = {
                "obj": str(output_paths["obj"]),
                "mtl": str(output_paths["mtl"]),
                "png": str(output_paths["png"]),
                "zip": str(output_paths["zip"]),
                "glb": str(output_paths["glb"]) if output_paths["glb"].exists() else None,
            }
            glb_error = None
            try:
                export_textured_obj_to_glb(output_paths)
            except Exception as exc:
                glb_error = str(exc)
                add_job_log(job, f"WARNING: GLB export skipped: {glb_error}")
            collected["glb"] = str(output_paths["glb"]) if output_paths["glb"].exists() else None
            collected["glb_error"] = glb_error
        if not output_paths["zip"].exists():
            raise RuntimeError(f"Texture bake finished but zip was not found: {output_paths['zip']}")
        result = {
            "backend": options["backend"],
            "texture_res": options["texture_res"],
            "max_faces": options["max_faces"],
            "bake_source": options["bake_source"],
            "source_path": str(dataset_path),
            **collected,
        }
        with MESH_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["texture"] = result
            job["texture_download_url"] = mesh_texture_file_url(
                job["scene"], job["iteration"], options["mode"], "zip", options["post"]
            )
        add_job_log(job, f"Texture bake complete: {result['zip']}")
    except Exception as exc:
        with MESH_LOCK:
            job["status"] = "failed"
            job["stage"] = "failed"
            job["error"] = str(exc)
            job["returncode"] = 1
            job["updated_at"] = time.time()
        add_job_log(job, f"ERROR: {exc}")


def start_texture_bake(scene, iteration, options=None):
    scene = safe_name(scene)
    iteration = int(iteration or latest_iteration(scene))
    options = bake_texture_options(options)
    backend = scene_backend(scene, iteration)
    if not mesh_mode_supports_texture_bake(options["mode"], backend):
        raise ValueError("Texture baking is only available for 2DGS meshes or GS2Mesh meshes from 3DGS scenes")
    job_id = uuid.uuid4().hex
    job = {
        "id": job_id,
        "kind": "texture",
        "scene": scene,
        "iteration": iteration,
        "mode": options["mode"],
        "options": options,
        "status": "queued",
        "stage": "queued",
        "created_at": time.time(),
        "updated_at": time.time(),
        "returncode": None,
        "error": None,
        "cancel_requested": False,
        "process": None,
        "output_mesh": str(mesh_output_path(scene, iteration, options["mode"], options["post"])),
        "download_url": None,
        "texture": None,
        "texture_download_url": None,
        "log": [],
    }
    with MESH_LOCK:
        MESH_JOBS[job_id] = job
    thread = threading.Thread(target=run_texture_bake_job, args=(job,), daemon=True)
    thread.start()
    return mesh_job_snapshot(job)


def extract_video_frames_with_cv2(video_path, images_dir, fps):
    try:
        import cv2
    except Exception as exc:
        raise RuntimeError(f"OpenCV is not available in the server Python: {exc}")

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"Could not open video: {video_path.name}")
    source_fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    step = max(1, int(round(source_fps / max(float(fps), 0.1))))
    stem = safe_filename(video_path.stem)
    written = 0
    frame_index = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if frame_index % step == 0:
            target = images_dir / f"{stem}_{written:06d}.jpg"
            cv2.imwrite(str(target), frame)
            written += 1
        frame_index += 1
    cap.release()
    return written


def conda_executable():
    bundled = Path.home() / "miniforge3" / "Scripts" / "conda.exe"
    if bundled.exists():
        return bundled
    found = shutil.which("conda")
    return Path(found) if found else None


def ffmpeg_executable():
    candidates = [
        os.environ.get("FFMPEG_PATH"),
        shutil.which("ffmpeg"),
        Path.home() / "miniforge3" / "envs" / "gaussian_splatting" / "Library" / "bin" / "ffmpeg.exe",
        Path.home() / "miniforge3" / "Library" / "bin" / "ffmpeg.exe",
    ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists():
            return path
    return None


def extract_video_frames_with_ffmpeg(video_path, images_dir, fps):
    ffmpeg = ffmpeg_executable()
    if not ffmpeg:
        raise RuntimeError("ffmpeg.exe was not found")
    stem = safe_filename(video_path.stem)
    pattern = images_dir / f"{stem}_%06d.jpg"
    before = {p.name for p in images_dir.glob(f"{stem}_*.jpg")}
    result = subprocess.run(
        [
            str(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(video_path),
            "-vf",
            f"fps={max(float(fps), 0.1):.6f}",
            "-q:v",
            "2",
            "-start_number",
            "0",
            str(pattern),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"ffmpeg extraction failed: {detail}")
    after = [p for p in images_dir.glob(f"{stem}_*.jpg") if p.name not in before or p.stat().st_size > 0]
    if not after:
        raise RuntimeError("ffmpeg extraction produced no frames")
    return len(after)


def extract_video_frames_with_conda(video_path, images_dir, fps):
    conda = conda_executable()
    if not conda:
        raise RuntimeError("conda.exe was not found")
    script = Path(__file__).resolve().parent / "video_extract.py"
    result = subprocess.run(
        [str(conda), "run", "-n", "gaussian_splatting", "python", str(script), str(video_path), str(images_dir), str(fps)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"conda video extraction failed: {detail}")
    for line in reversed((result.stdout or "").splitlines()):
        text = line.strip()
        if text.isdigit():
            return int(text)
    raise RuntimeError(f"conda video extraction did not report frame count: {(result.stdout or '').strip()}")


def extract_video_frames(video_path, images_dir, fps):
    errors = []
    try:
        return extract_video_frames_with_ffmpeg(video_path, images_dir, fps)
    except Exception as exc:
        errors.append(str(exc))
    try:
        return extract_video_frames_with_cv2(video_path, images_dir, fps)
    except Exception as exc:
        errors.append(str(exc))
    try:
        return extract_video_frames_with_conda(video_path, images_dir, fps)
    except Exception as exc:
        errors.append(str(exc))
    raise RuntimeError(f"Could not extract frames from {video_path.name}: {'; '.join(errors)}")


def prepare_dataset_import(scene, overwrite):
    dataset = DATASETS_DIR / scene
    images_dir = dataset / "images"
    source_dir = dataset / "source"
    originals_dir = source_dir / "originals"
    if dataset.exists() and overwrite:
        shutil.rmtree(dataset)
    images_dir.mkdir(parents=True, exist_ok=True)
    originals_dir.mkdir(parents=True, exist_ok=True)
    return dataset, images_dir, source_dir, originals_dir


def import_archived_media(archive_target, original_filename, relative_path, content_type, size, last_modified, dataset, images_dir, fps):
    ext = archive_target.suffix.lower()
    record = {
        "kind": "image" if ext in IMAGE_EXTS else "video",
        "originalFilename": original_filename,
        "filename": archive_target.name,
        "relativePath": relative_path or original_filename,
        "contentType": content_type or "",
        "size": size,
        "lastModified": last_modified,
        "byteSize": archive_target.stat().st_size,
        "archivePath": archive_target.relative_to(dataset).as_posix(),
        "trainingPath": None,
        "extractedFrames": 0,
    }
    saved_images = 0
    saved_videos = 0
    extracted_frames = 0
    if ext in IMAGE_EXTS:
        target = unique_path(images_dir, archive_target.name)
        shutil.copy2(archive_target, target)
        record["trainingPath"] = target.relative_to(dataset).as_posix()
        saved_images += 1
    else:
        saved_videos += 1
        frames = extract_video_frames(archive_target, images_dir, fps)
        record["extractedFrames"] = frames
        extracted_frames += frames
    return record, saved_images, saved_videos, extracted_frames


def video_extract_worker_count(video_count):
    if video_count <= 1:
        return 1
    raw = os.environ.get("GS_VIDEO_EXTRACT_WORKERS", "").strip()
    if raw:
        try:
            configured = int(raw)
            if configured > 0:
                return min(configured, video_count)
        except ValueError:
            pass
    return min(3, video_count)


def init_import_job(job_id, scene, total_files=0, total_videos=0):
    if not job_id:
        return
    now = time.time()
    with IMPORT_LOCK:
        IMPORT_JOBS[job_id] = {
            "id": job_id,
            "scene": scene,
            "status": "running",
            "stage": "queued",
            "created_at": now,
            "updated_at": now,
            "total_files": int(total_files or 0),
            "processed_files": 0,
            "total_videos": int(total_videos or 0),
            "processed_videos": 0,
            "saved_images": 0,
            "saved_videos": 0,
            "extracted_frames": 0,
            "image_count": 0,
            "video_workers": 0,
            "current_file": "",
            "recent": [],
            "error": "",
        }


def update_import_job(job_id, **updates):
    if not job_id:
        return None
    with IMPORT_LOCK:
        job = IMPORT_JOBS.setdefault(job_id, {
            "id": job_id,
            "status": "running",
            "stage": "queued",
            "created_at": time.time(),
            "updated_at": time.time(),
            "recent": [],
        })
        job.update(updates)
        job["updated_at"] = time.time()
        return dict(job)


def append_import_recent(job_id, item):
    if not job_id:
        return
    with IMPORT_LOCK:
        job = IMPORT_JOBS.get(job_id)
        if not job:
            return
        recent = list(job.get("recent", []))
        recent.append(item)
        job["recent"] = recent[-12:]
        job["updated_at"] = time.time()


def finish_import_job(job_id, status, **updates):
    if not job_id:
        return
    update_import_job(job_id, status=status, **updates)


def import_job_snapshot(job_id):
    with IMPORT_LOCK:
        job = IMPORT_JOBS.get(job_id)
        if not job:
            raise ValueError("Import job not found")
        return dict(job)


def import_archived_media_batch(entries, dataset, images_dir, fps, import_job_id=None):
    records = [None] * len(entries)
    saved_images = 0
    saved_videos = 0
    extracted_frames = 0
    video_entries = []
    update_import_job(import_job_id, stage="copying_images", current_file="")
    for index, entry in enumerate(entries):
        archive_target = entry["archive_target"]
        ext = archive_target.suffix.lower()
        record = {
            "kind": "image" if ext in IMAGE_EXTS else "video",
            "originalFilename": entry["original_filename"],
            "filename": archive_target.name,
            "relativePath": entry.get("relative_path") or entry["original_filename"],
            "contentType": entry.get("content_type") or "",
            "size": entry.get("size"),
            "lastModified": entry.get("last_modified"),
            "byteSize": archive_target.stat().st_size,
            "archivePath": archive_target.relative_to(dataset).as_posix(),
            "trainingPath": None,
            "extractedFrames": 0,
        }
        record.update(entry.get("extra_record", {}))
        records[index] = record
        if ext in IMAGE_EXTS:
            update_import_job(import_job_id, current_file=archive_target.name)
            target = unique_path(images_dir, archive_target.name)
            shutil.copy2(archive_target, target)
            record["trainingPath"] = target.relative_to(dataset).as_posix()
            saved_images += 1
            update_import_job(import_job_id, saved_images=saved_images)
        else:
            saved_videos += 1
            video_entries.append((index, archive_target))
    if video_entries:
        workers = video_extract_worker_count(len(video_entries))
        processed_videos = 0
        update_import_job(
            import_job_id,
            stage="extracting_frames",
            total_videos=len(video_entries),
            saved_videos=saved_videos,
            video_workers=workers,
            current_file="",
        )
        if workers <= 1:
            for index, archive_target in video_entries:
                update_import_job(import_job_id, current_file=archive_target.name)
                frames = extract_video_frames(archive_target, images_dir, fps)
                records[index]["extractedFrames"] = frames
                extracted_frames += frames
                processed_videos += 1
                update_import_job(import_job_id, extracted_frames=extracted_frames, processed_videos=processed_videos, current_file="")
                append_import_recent(import_job_id, {"name": archive_target.name, "frames": frames})
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
                pending_names = [path.name for _, path in video_entries[:workers]]
                update_import_job(import_job_id, current_file=", ".join(pending_names))
                futures = {
                    executor.submit(extract_video_frames, archive_target, images_dir, fps): (index, archive_target)
                    for index, archive_target in video_entries
                }
                for future in concurrent.futures.as_completed(futures):
                    index, archive_target = futures[future]
                    frames = future.result()
                    records[index]["extractedFrames"] = frames
                    extracted_frames += frames
                    processed_videos += 1
                    update_import_job(import_job_id, extracted_frames=extracted_frames, processed_videos=processed_videos, current_file="")
                    append_import_recent(import_job_id, {"name": archive_target.name, "frames": frames})
    return records, saved_images, saved_videos, extracted_frames


def archive_local_source(source, target):
    try:
        os.link(source, target)
        return "hardlink"
    except OSError:
        shutil.copy2(source, target)
        return "copy"


def save_uploaded_dataset(form):
    scene = safe_name(form.getfirst("scene", ""))
    overwrite = form.getfirst("overwrite", "false").lower() == "true"
    fps = float(form.getfirst("fps", "2"))
    import_job_id = form.getfirst("import_job_id", "")

    file_items = form_items(form, "files")
    total_videos = sum(1 for item in file_items if Path(getattr(item, "filename", "")).suffix.lower() in VIDEO_EXTS)
    init_import_job(import_job_id, scene, len(file_items), total_videos)
    try:
        update_import_job(import_job_id, stage="preparing")
        dataset, images_dir, source_dir, originals_dir = prepare_dataset_import(scene, overwrite)
        update_import_job(import_job_id, stage="archiving")
    except Exception as exc:
        finish_import_job(import_job_id, "failed", error=str(exc))
        raise

    file_metadata = parse_file_metadata(form)
    import_entries = []
    try:
        processed_files = 0
        for index, item in enumerate(file_items):
            if not getattr(item, "filename", None):
                continue
            browser_metadata = file_metadata[index] if index < len(file_metadata) and isinstance(file_metadata[index], dict) else {}
            filename = safe_filename(item.filename)
            ext = Path(filename).suffix.lower()
            if ext not in IMAGE_EXTS and ext not in VIDEO_EXTS:
                continue
            update_import_job(import_job_id, current_file=filename)
            archive_target = unique_path(originals_dir, filename)
            with open(archive_target, "wb") as f:
                shutil.copyfileobj(item.file, f)
            maybe_apply_uploaded_mtime(archive_target, browser_metadata)
            import_entries.append({
                "archive_target": archive_target,
                "original_filename": item.filename,
                "relative_path": browser_metadata.get("relativePath") or item.filename,
                "content_type": browser_metadata.get("type") or "",
                "size": browser_metadata.get("size"),
                "last_modified": browser_metadata.get("lastModified"),
            })
            processed_files += 1
            update_import_job(import_job_id, processed_files=processed_files)
        manifest_records, saved_images, saved_videos, extracted_frames = import_archived_media_batch(import_entries, dataset, images_dir, fps, import_job_id)
        if manifest_records:
            write_import_manifest(source_dir, manifest_records)
        update_import_job(import_job_id, stage="masks", current_file="")
        mask_count = save_uploaded_masks(dataset, form)
        applied_masks = apply_alpha_masks_to_dataset(dataset) if mask_count else 0
        total_images = sum(1 for p in images_dir.iterdir() if p.suffix.lower() in IMAGE_EXTS)
        if total_images == 0:
            raise ValueError("No images were imported. Choose photos, or a readable video.")
        result = {
            "scene": scene,
            "path": str(dataset),
            "saved_images": saved_images,
            "saved_videos": saved_videos,
            "extracted_frames": extracted_frames,
            "image_count": total_images,
            "mask_count": mask_count,
            "applied_masks": applied_masks,
            "video_workers": video_extract_worker_count(saved_videos) if saved_videos else 0,
        }
        finish_import_job(import_job_id, "done", stage="done", current_file="", **result)
        return result
    except Exception as exc:
        finish_import_job(import_job_id, "failed", error=str(exc))
        raise


def save_path_dataset(data):
    scene = safe_name(data.get("scene", ""))
    overwrite = bool(data.get("overwrite", False))
    fps = float(data.get("fps", 2))
    import_job_id = data.get("import_job_id") or ""
    source_items = [item for item in data.get("files", []) if isinstance(item, dict)]
    total_videos = sum(1 for item in source_items if Path(item.get("name") or item.get("path", "")).suffix.lower() in VIDEO_EXTS)
    init_import_job(import_job_id, scene, len(source_items), total_videos)
    try:
        update_import_job(import_job_id, stage="preparing")
        dataset, images_dir, source_dir, originals_dir = prepare_dataset_import(scene, overwrite)
        update_import_job(import_job_id, stage="archiving")
    except Exception as exc:
        finish_import_job(import_job_id, "failed", error=str(exc))
        raise

    import_entries = []
    try:
        processed_files = 0
        for item in source_items:
            source = Path(item.get("path", ""))
            if not source.is_absolute() or not source.is_file():
                continue
            filename = safe_filename(item.get("name") or source.name)
            ext = Path(filename).suffix.lower()
            if ext not in IMAGE_EXTS and ext not in VIDEO_EXTS:
                continue
            update_import_job(import_job_id, current_file=filename)
            archive_target = unique_path(originals_dir, filename)
            archive_mode = archive_local_source(source, archive_target)
            import_entries.append({
                "archive_target": archive_target,
                "original_filename": item.get("name") or source.name,
                "relative_path": item.get("relativePath") or source.name,
                "content_type": item.get("type") or "",
                "size": item.get("size"),
                "last_modified": item.get("lastModified"),
                "extra_record": {
                    "sourcePath": str(source),
                    "archiveMode": archive_mode,
                },
            })
            processed_files += 1
            update_import_job(import_job_id, processed_files=processed_files)
        manifest_records, saved_images, saved_videos, extracted_frames = import_archived_media_batch(import_entries, dataset, images_dir, fps, import_job_id)
        if manifest_records:
            write_import_manifest(source_dir, manifest_records)
        total_images = sum(1 for p in images_dir.iterdir() if p.suffix.lower() in IMAGE_EXTS)
        if total_images == 0:
            raise ValueError("No images were imported. Choose photos, or a readable video.")
        result = {
            "scene": scene,
            "path": str(dataset),
            "saved_images": saved_images,
            "saved_videos": saved_videos,
            "extracted_frames": extracted_frames,
            "image_count": total_images,
            "mask_count": 0,
            "applied_masks": 0,
            "path_import": True,
            "video_workers": video_extract_worker_count(saved_videos) if saved_videos else 0,
        }
        finish_import_job(import_job_id, "done", stage="done", current_file="", **result)
        return result
    except Exception as exc:
        finish_import_job(import_job_id, "failed", error=str(exc))
        raise


class Handler(BaseHTTPRequestHandler):
    def send_json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, exc, status=400):
        self.send_json({"error": str(exc)}, status)

    def do_GET(self):
        try:
            parsed = urlparse(self.path)
            if parsed.path == "/api/app/health":
                return self.send_json({
                    "app": "3DGS Editor",
                    "api_version": 2,
                    "capabilities": {
                        "job_center": True,
                        "asset_manager": True,
                        "splat_export_jobs": True,
                        "psnr_analysis": True,
                    },
                })
            if parsed.path == "/api/scenes":
                return self.send_json({"scenes": list_scenes()})
            if parsed.path == "/api/datasets":
                return self.send_json({"datasets": list_datasets()})
            if parsed.path == "/api/assets":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                return self.send_json(scene_asset_payload(scene, iteration))
            if parsed.path == "/api/assets/file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                name = qs.get("name", [""])[0]
                path = asset_config_file_path(scene, name)
                mime = "application/octet-stream" if path.suffix.lower() == ".ply" else "text/plain; charset=utf-8"
                return self.serve_file(path, mime, path.name)
            if parsed.path == "/api/assets/psnr-file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                run = qs.get("run", [""])[0]
                name = qs.get("name", [""])[0]
                path = psnr_report_file_path(scene, run, name)
                mime = "application/json; charset=utf-8" if path.suffix.lower() == ".json" else "text/csv; charset=utf-8"
                return self.serve_file(path, mime, path.name)
            if parsed.path == "/api/import/status":
                qs = parse_qs(parsed.query)
                job_id = qs.get("id", [""])[0]
                return self.send_json(import_job_snapshot(job_id))
            if parsed.path == "/api/jobs":
                qs = parse_qs(parsed.query)
                limit = int(qs.get("limit", ["50"])[0])
                return self.send_json({"jobs": list_all_jobs(limit)})
            if parsed.path == "/api/jobs/status":
                qs = parse_qs(parsed.query)
                job_id = qs.get("id", [""])[0]
                kind, job, _ = find_unified_job(job_id)
                api_kind = "splat" if kind == "splat" else kind
                return self.send_json(unified_job_snapshot(api_kind, job))
            if parsed.path == "/api/train/check":
                qs = parse_qs(parsed.query)
                backend = safe_training_backend(qs.get("backend", ["3dgs"])[0])
                return self.send_json(training_environment_report(backend))
            if parsed.path == "/api/train/status":
                qs = parse_qs(parsed.query)
                job_id = qs.get("id", [""])[0]
                with TRAIN_LOCK:
                    job = TRAIN_JOBS.get(job_id)
                    if not job:
                        raise ValueError("Training job not found")
                    return self.send_json(job_snapshot(job))
            if parsed.path == "/api/train/jobs":
                with TRAIN_LOCK:
                    jobs = [job_snapshot(job) for job in TRAIN_JOBS.values()]
                jobs.sort(key=lambda item: item["created_at"], reverse=True)
                return self.send_json({"jobs": jobs[:20]})
            if parsed.path == "/api/mesh/status":
                qs = parse_qs(parsed.query)
                job_id = qs.get("id", [""])[0]
                with MESH_LOCK:
                    job = MESH_JOBS.get(job_id)
                    if not job:
                        raise ValueError("Mesh export job not found")
                    return self.send_json(psnr_job_snapshot(job) if job.get("kind") == "psnr" else mesh_job_snapshot(job))
            if parsed.path == "/api/splat/export/status":
                qs = parse_qs(parsed.query)
                job_id = qs.get("id", [""])[0]
                with SPLAT_LOCK:
                    job = SPLAT_JOBS.get(job_id)
                    if not job:
                        raise ValueError("Splat export job not found")
                    return self.send_json(splat_job_snapshot(job))
            if parsed.path == "/api/mesh/assets":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                return self.send_json(mesh_asset_payload(scene, iteration))
            if parsed.path == "/api/mesh/chunks":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                max_faces = int(qs.get("max_faces", [MESH_CHUNK_MAX_FACES])[0])
                return self.send_json(mesh_chunk_manifest(scene, iteration, mode, post, max_faces))
            if parsed.path == "/api/mesh/texture/chunks":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                max_faces = int(qs.get("max_faces", [MESH_CHUNK_MAX_FACES])[0])
                return self.send_json(textured_mesh_chunk_manifest(scene, iteration, mode, post, max_faces))
            if parsed.path == "/api/points":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                payload = point_payload(scene, iteration)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            if parsed.path == "/api/cameras":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                return self.send_json(camera_payload(scene))
            if parsed.path == "/api/ply":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                return self.serve_file(ply_path(scene, iteration), "application/octet-stream")
            if parsed.path == "/api/splat/export":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                fmt = qs.get("format", ["ply"])[0].lower()
                path = export_splat_format(scene, iteration, fmt)
                return self.serve_file(path, "application/octet-stream", f"{scene}_iteration_{iteration}.{fmt}")
            if parsed.path == "/api/preview_ply":
                qs = parse_qs(parsed.query)
                token = qs.get("token", [""])[0]
                if not token or not all(c in "0123456789abcdef" for c in token) or len(token) != 32:
                    raise ValueError("Invalid preview token")
                return self.serve_file(PREVIEW_DIR / f"{token}.ply", "application/octet-stream")
            if parsed.path == "/api/mesh/file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                path = mesh_output_path(scene, iteration, mode, post)
                return self.serve_file(path, "application/octet-stream", path.name)
            if parsed.path == "/api/mesh/preview_file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                max_faces = int(qs.get("max_faces", [MESH_PREVIEW_MAX_FACES])[0])
                path = mesh_preview_path(scene, iteration, mode, post, max_faces)
                return self.serve_file(path, "application/octet-stream", path.name)
            if parsed.path == "/api/mesh/chunk_file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                key = safe_filename(qs.get("key", [""])[0])
                index = int(qs.get("index", ["0"])[0])
                source = mesh_output_path(scene, iteration, mode, post)
                _, cache_dir, _ = mesh_chunk_cache_paths(source, MESH_CHUNK_MAX_FACES)
                path = cache_dir.parent / key / f"chunk_{index:05d}.bin"
                return self.serve_file(path, "application/octet-stream", path.name)
            if parsed.path == "/api/mesh/texture/chunk_file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                key = safe_filename(qs.get("key", [""])[0])
                index = int(qs.get("index", ["0"])[0])
                source, _ = textured_mesh_source_paths(scene, iteration, mode, post)
                _, cache_dir, _ = textured_mesh_chunk_cache_paths(source, MESH_CHUNK_MAX_FACES)
                path = cache_dir.parent / key / f"chunk_{index:05d}.bin"
                return self.serve_file(path, "application/octet-stream", path.name)
            if parsed.path == "/api/mesh/texture/file":
                qs = parse_qs(parsed.query)
                scene = safe_name(qs.get("scene", [""])[0])
                iteration = int(qs.get("iteration", [latest_iteration(scene)])[0])
                mode = safe_mesh_mode(qs.get("mode", ["bounded"])[0])
                post = qs.get("post", ["true"])[0].lower() != "false"
                kind = safe_texture_kind(qs.get("kind", ["zip"])[0])
                path = mesh_texture_paths(scene, iteration, mode, post)[kind]
                mime = {
                    "obj": "text/plain",
                    "mtl": "text/plain",
                    "png": "image/png",
                    "zip": "application/zip",
                    "glb": "model/gltf-binary",
                }[kind]
                return self.serve_file(path, mime, path.name)
            self.serve_static(parsed.path)
        except Exception as exc:
            self.send_error_json(exc, 500)

    def do_POST(self):
        try:
            parsed_path = urlparse(self.path).path
            if parsed_path == "/api/shutdown":
                stopped = shutdown_active_jobs("desktop app shutdown")
                self.send_json({"ok": True, "stopped": stopped})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            if parsed_path == "/api/import":
                form = cgi.FieldStorage(
                    fp=self.rfile,
                    headers=self.headers,
                    environ={
                        "REQUEST_METHOD": "POST",
                        "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                    },
                )
                return self.send_json(save_uploaded_dataset(form))
            if parsed_path == "/api/splat/import":
                form = cgi.FieldStorage(
                    fp=self.rfile,
                    headers=self.headers,
                    environ={
                        "REQUEST_METHOD": "POST",
                        "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                    },
                )
                upload = form_items(form, "file")
                return self.send_json(import_splat_scene(
                    upload[0] if upload else None,
                    form.getfirst("scene", ""),
                    form.getfirst("overwrite", "false").lower() == "true",
                ))
            if parsed_path not in (
                "/api/import-paths",
                "/api/save",
                "/api/preview",
                "/api/train/start",
                "/api/train/cancel",
                "/api/mesh/start",
                "/api/mesh/cancel",
                "/api/mesh/trimmed/save",
                "/api/mesh/texture/start",
                "/api/psnr/start",
                "/api/splat/export/start",
                "/api/splat/export/cancel",
                "/api/jobs/cancel",
                "/api/jobs/retry",
                "/api/jobs/open-output",
                "/api/assets/open-dir",
                "/api/assets/open-psnr",
            ):
                return self.send_json({"error": "Not found"}, 404)
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(length).decode("utf-8"))
            if parsed_path == "/api/import-paths":
                return self.send_json(save_path_dataset(data))
            if parsed_path == "/api/jobs/cancel":
                return self.send_json(cancel_unified_job(data["id"]))
            if parsed_path == "/api/jobs/retry":
                return self.send_json(retry_unified_job(data["id"]))
            if parsed_path == "/api/jobs/open-output":
                return self.send_json(open_job_output_dir(data["id"]))
            if parsed_path == "/api/assets/open-dir":
                return self.send_json(open_asset_dir(data["scene"], data.get("target", "output")))
            if parsed_path == "/api/assets/open-psnr":
                return self.send_json(open_psnr_report_dir(data["scene"], data["run"]))
            if parsed_path == "/api/train/cancel":
                return self.send_json(cancel_training(data["id"]))
            if parsed_path == "/api/mesh/cancel":
                return self.send_json(cancel_mesh_job(data["id"]))
            if parsed_path == "/api/splat/export/cancel":
                return self.send_json(cancel_splat_job(data["id"]))
            if parsed_path == "/api/splat/export/start":
                return self.send_json(start_splat_export(
                    data["scene"],
                    int(data.get("iteration") or latest_iteration(data["scene"])),
                    data.get("format", "spz"),
                ))
            if parsed_path == "/api/mesh/start":
                job = start_mesh_export(
                    data["scene"],
                    int(data.get("iteration") or latest_iteration(data["scene"])),
                    data.get("options", {}),
                )
                return self.send_json(job)
            if parsed_path == "/api/mesh/trimmed/save":
                result = save_trimmed_mesh(
                    data["scene"],
                    int(data.get("iteration") or latest_iteration(data["scene"])),
                    data["output_scene"],
                    data.get("mode", "bounded"),
                    data.get("ply", ""),
                    bool(data.get("overwrite", False)),
                )
                return self.send_json(result)
            if parsed_path == "/api/mesh/texture/start":
                job = start_texture_bake(
                    data["scene"],
                    int(data.get("iteration") or latest_iteration(data["scene"])),
                    data.get("options", {}),
                )
                return self.send_json(job)
            if parsed_path == "/api/psnr/start":
                return self.send_json(start_psnr_analysis(
                    data["scene"],
                    int(data.get("iteration") or latest_iteration(data["scene"])),
                    data.get("backend") or scene_backend(data["scene"], data.get("iteration")),
                    data.get("count", 20),
                    data.get("eval_width", 0),
                ))
            if parsed_path == "/api/train/start":
                job = start_training(
                    data["scene"],
                    data.get("output_scene") or data["scene"],
                    data.get("quality", "quick"),
                    bool(data.get("run_convert", True)),
                    bool(data.get("overwrite", False)),
                    data.get("backend", "3dgs"),
                    data.get("colmap", {}),
                    data.get("train_options", {}),
                    bool(data.get("allow_existing_output", False)),
                )
                return self.send_json(job)
            scene = safe_name(data["scene"])
            iteration = int(data["iteration"])
            delete_indices = data.get("delete_indices", [])
            if parsed_path == "/api/preview":
                return self.send_json(make_preview(scene, iteration, delete_indices))
            output_scene = safe_name(data["output_scene"])
            overwrite = bool(data.get("overwrite", False))
            if output_scene == scene:
                raise ValueError("Output scene must be different from the source scene")
            if model_dir(output_scene).exists() and not overwrite:
                raise ValueError(f"Output scene already exists: {output_scene}")
            return self.send_json(save_cropped(scene, iteration, output_scene, delete_indices))
        except Exception as exc:
            self.send_error_json(exc, 400)

    def serve_static(self, path):
        if path in ("", "/"):
            path = "/index.html"
        rel = Path(path.lstrip("/"))
        target = (STATIC_DIR / rel).resolve()
        if not str(target).startswith(str(STATIC_DIR.resolve())) or not target.exists():
            self.send_json({"error": "Not found"}, 404)
            return
        content_type = "text/html; charset=utf-8"
        if target.suffix in (".js", ".mjs"):
            content_type = "text/javascript; charset=utf-8"
        elif target.suffix == ".css":
            content_type = "text/css; charset=utf-8"
        body = target.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_file(self, path, content_type, download_name=None):
        path = Path(path)
        if not path.exists():
            self.send_json({"error": "Not found"}, 404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        if download_name:
            self.send_header("Content-Disposition", f'attachment; filename="{safe_filename(download_name)}"')
        self.send_header("Content-Length", str(path.stat().st_size))
        self.end_headers()
        with open(path, "rb") as f:
            shutil.copyfileobj(f, self.wfile)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7860)
    parser.add_argument("--no-open", action="store_true")
    args = parser.parse_args()

    with TRAIN_LOCK:
        TRAIN_JOBS.update(load_persisted_train_jobs())
    with SPLAT_LOCK:
        SPLAT_JOBS.update(load_persisted_splat_jobs())
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}"
    print(f"3DGS Crop Editor: {url}")
    print(f"Output root: {OUTPUT_DIR}")
    if not args.no_open:
        webbrowser.open(url)
    server.serve_forever()


if __name__ == "__main__":
    main()
