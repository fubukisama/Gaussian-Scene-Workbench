"""Native worker compute services; no listener or web application dependencies."""

import argparse
import ast
import concurrent.futures
from contextlib import ExitStack
import hashlib
import json
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
import zipfile
from pathlib import Path, PurePosixPath

import numpy as np
from plyfile import PlyData

try:
    import winreg
except ImportError:
    winreg = None

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from native.backend import reconstruction_quality
from native.backend.atomic_file import owned_directory
from native.worker.training_summary import (
    configured_training_summary, normalize_training_summary, PARAMETER_FIELDS,
)
from native.worker.training_step_profile import normalize_performance
from native.worker.training_resource_policy import normalize_growth
WORKSPACE_ROOT = Path(os.environ.get("GS_EDITOR_WORKSPACE_ROOT", ROOT)).resolve()
DATASETS_DIR = WORKSPACE_ROOT / "datasets"
OUTPUT_DIR = WORKSPACE_ROOT / "output"
GAUSSIAN_DIR = ROOT / "gaussian-splatting"
def two_dgs_source_root():
    configured = os.environ.get("TWO_DGS_DIR")
    if configured:
        return Path(configured)
    candidates = [ROOT / "2dgs"]
    if ROOT.anchor:
        candidates.append(Path(ROOT.anchor) / "Gaussian-Scene-Workbench-Runtime" / "2dgs")
    candidates.append(Path.home() / "Documents" / "2dgs")
    return next((path for path in candidates if (path / "train.py").is_file()), candidates[-1])


TWO_DGS_DIR = two_dgs_source_root()
SUGAR_DIR = Path(os.environ.get("SUGAR_DIR", ROOT / "SuGaR"))
GS2MESH_DIR = Path(os.environ.get("GS2MESH_DIR", ROOT / "gs2mesh"))
OPENMVS_DIR = Path(os.environ.get("OPENMVS_DIR", ROOT / "openMVS"))
JOBS_DIR = WORKSPACE_ROOT / ".gsw" / "jobs"
TRAIN_JOBS_DIR = JOBS_DIR / "training"
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}
VIDEO_EXTS = {".mp4", ".mov", ".avi", ".mkv", ".webm", ".m4v"}
MASK_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp"}
TRAIN_JOBS = {}
TRAIN_LOCK = threading.Lock()
MESH_JOBS = {}
MESH_LOCK = threading.Lock()
TASK_PORT_LOCK = threading.Lock()
TASK_PORT_OWNERS = {}
TASK_PORT_START = 6100
TASK_PORT_END = 6999
RUNTIME_PROBE_TIMEOUT_SECONDS = 90
IMPORT_JOBS = {}
IMPORT_LOCK = threading.Lock()
TRAINING_BACKENDS = {"3dgs", "2dgs"}
MESH_MODES = {"bounded", "unbounded", "sugar", "gs2mesh"}
SPARSE_ADAM_AVAILABLE_CACHE = None
COLMAP_POINT_PREVIEW_MAX_POINTS = 500000
COLMAP_POINT_PREVIEW_INTERVAL_SECONDS = 0.5
COLMAP_SNAPSHOT_TARGET_COUNT = 48
TRAINING_PROGRESS_PATTERN = re.compile(r"\[gsw-training-progress\]\s+(\d+)\s*/\s*(\d+)")
TRAINING_METRICS_PREFIX = "[gsw-training-metrics]"
TRAINING_PERFORMANCE_PREFIX = "[gsw-training-performance]"
TRAINING_GROWTH_PREFIX = "[gsw-training-growth]"
TRAINING_INPUT_PREFIX = "[gsw-training-input]"
TRAINING_DENSITY_PREFIX = "[gsw-density-control]"
TRAINING_PREVIEW_PREFIX = "[gsw-training-preview]"
WINDOWS_DEVICE_NAMES = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}


def canonicalize_gs2mesh_camera_mapping(sparse_dir):
    """Expand multi-camera COLMAP intrinsics in the staged copy only.

    GS2Mesh indexes sorted camera rows by sorted image position. COLMAP instead
    maps each image to an arbitrary CAMERA_ID, so mixed-camera datasets need a
    one-camera-row-per-image staging representation to preserve that mapping.
    """
    sparse_dir = Path(sparse_dir)
    cameras_path = sparse_dir / "cameras.txt"
    images_path = sparse_dir / "images.txt"
    cameras_text = cameras_path.read_text(encoding="utf-8")
    images_text = images_path.read_text(encoding="utf-8")
    cameras = {}
    camera_comments = []
    for line in cameras_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            camera_comments.append(line)
            continue
        fields = stripped.split()
        if len(fields) < 5:
            raise ValueError(f"Invalid COLMAP camera record: {line}")
        cameras[int(fields[0])] = fields[1:]
    if not cameras:
        raise ValueError(f"No COLMAP cameras found: {cameras_path}")

    image_lines = images_text.splitlines()
    image_records = []
    expecting_image = True
    for line_index, line in enumerate(image_lines):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if expecting_image:
            if not stripped:
                continue
            fields = stripped.split()
            if len(fields) < 10:
                raise ValueError(f"Invalid COLMAP image record: {line}")
            image_id = int(fields[0])
            camera_id = int(fields[8])
            if camera_id not in cameras:
                raise ValueError(
                    f"COLMAP image {image_id} references missing camera {camera_id}: {images_path}"
                )
            image_records.append((image_id, camera_id, line_index, fields))
            expecting_image = False
        else:
            expecting_image = True

    if len(cameras) == 1:
        return {"expanded": False, "camera_count": 1, "image_count": len(image_records)}

    expanded_rows = []
    for image_id, camera_id, line_index, fields in image_records:
        fields[8] = str(image_id)
        image_lines[line_index] = " ".join(fields)
        expanded_rows.append((image_id, cameras[camera_id]))
    expanded_rows.sort(key=lambda item: item[0])
    header = [
        line
        for line in camera_comments
        if not line.lstrip().startswith("# Number of cameras:")
    ]
    header.append(f"# Number of cameras: {len(expanded_rows)}")
    camera_lines = header + [
        f"{image_id} {' '.join(fields)}" for image_id, fields in expanded_rows
    ]
    trailing_cameras = "\n" if cameras_text.endswith("\n") else ""
    trailing_images = "\n" if images_text.endswith("\n") else ""
    cameras_path.write_text("\n".join(camera_lines) + trailing_cameras, encoding="utf-8")
    images_path.write_text("\n".join(image_lines) + trailing_images, encoding="utf-8")
    return {
        "expanded": True,
        "camera_count": len(expanded_rows),
        "image_count": len(image_records),
    }


def gs2mesh_file_identity(path, *, digest=False, include_mtime=True):
    path = Path(path)
    if not path.exists() or not path.is_file():
        return {"exists": False}
    stat = path.stat()
    identity = {"exists": True, "size": int(stat.st_size)}
    if include_mtime:
        identity["mtime_ns"] = int(stat.st_mtime_ns)
    if digest:
        hasher = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                hasher.update(chunk)
        identity["sha256"] = hasher.hexdigest()
    return identity


def gs2mesh_cache_signature(scene, iteration, options, model_path):
    opts = mesh_export_options(options)
    runtime_scene = gs2mesh_runtime_colmap_name(scene, opts["gs2mesh_downsample"])
    sparse = GS2MESH_DIR / "data" / "custom" / runtime_scene / "sparse" / "0"
    point_cloud = (
        Path(model_path)
        / "point_cloud"
        / f"iteration_{int(iteration)}"
        / "point_cloud.ply"
    )
    return {
        "version": GS2MESH_CACHE_SIGNATURE_VERSION,
        "scene": safe_name(scene),
        "iteration": int(iteration),
        "downsample": opts["gs2mesh_downsample"],
        "baseline_percentage": opts["gs2mesh_baseline_percentage"],
        "scene_360": opts["gs2mesh_scene_360"],
        "point_cloud": gs2mesh_file_identity(point_cloud),
        "cameras": gs2mesh_file_identity(
            sparse / "cameras.txt", digest=True, include_mtime=False
        ),
        "images": gs2mesh_file_identity(
            sparse / "images.txt", digest=True, include_mtime=False
        ),
        "points3d": gs2mesh_file_identity(
            sparse / "points3D.txt", digest=True, include_mtime=False
        ),
        "runner": gs2mesh_file_identity(
            GS2MESH_DIR / "run_single.py", digest=True, include_mtime=False
        ),
    }


def gs2mesh_source_camera_identity(source_path):
    sparse = Path(source_path) / "sparse" / "0"
    return {
        name: gs2mesh_file_identity(sparse / name, digest=True, include_mtime=False)
        for name in ("cameras.txt", "images.txt", "cameras.bin", "images.bin")
    }


def gs2mesh_cache_manifest_path(output_dir):
    return Path(output_dir) / GS2MESH_CACHE_MANIFEST


def read_gs2mesh_cache_manifest(output_dir):
    path = gs2mesh_cache_manifest_path(output_dir)
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def write_gs2mesh_cache_manifest(output_dir, signature):
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    target = gs2mesh_cache_manifest_path(output_dir)
    temporary = target.with_suffix(f".tmp-{uuid.uuid4().hex}")
    payload = dict(signature)
    payload["written_at"] = time.time()
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(temporary, target)
    return target


def gs2mesh_cache_manifest_matches(output_dir, signature):
    manifest = read_gs2mesh_cache_manifest(output_dir)
    if not manifest:
        return False
    return all(manifest.get(key) == value for key, value in signature.items())


def gs2mesh_colmap_image_records(images_txt):
    records = []
    expecting_image = True
    with open(images_txt, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            if expecting_image:
                if not stripped:
                    continue
                fields = stripped.split()
                if len(fields) < 10:
                    raise ValueError(f"Invalid COLMAP image record: {line.rstrip()}")
                image_id = int(fields[0])
                w, x, y, z = (float(value) for value in fields[1:5])
                translation = np.asarray([float(value) for value in fields[5:8]], dtype=np.float64)
                camera_id = int(fields[8])
                rotation = np.asarray(
                    [
                        [1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * w * z, 2 * x * z + 2 * w * y],
                        [2 * x * y + 2 * w * z, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * w * x],
                        [2 * x * z - 2 * w * y, 2 * y * z + 2 * w * x, 1 - 2 * x * x - 2 * y * y],
                    ],
                    dtype=np.float64,
                )
                records.append(
                    {
                        "image_id": image_id,
                        "camera_id": camera_id,
                        "name": fields[9],
                        "center": -(rotation.T @ translation),
                        "camera_to_world_rotation": rotation.T,
                    }
                )
                expecting_image = False
            else:
                expecting_image = True
    return sorted(records, key=lambda record: record["image_id"])


def gs2mesh_colmap_camera_centers(images_txt):
    records = gs2mesh_colmap_image_records(images_txt)
    return np.asarray([record["center"] for record in records], dtype=np.float64)


def gs2mesh_colmap_renderer_intrinsics(cameras_txt, image_records):
    cameras = {}
    with open(cameras_txt, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            fields = stripped.split()
            if len(fields) < 7:
                raise ValueError(f"Invalid COLMAP camera record: {line.rstrip()}")
            camera_id = int(fields[0])
            model = fields[1].upper()
            width, height = int(fields[2]), int(fields[3])
            params = [float(value) for value in fields[4:]]
            if model in {"SIMPLE_PINHOLE", "SIMPLE_RADIAL", "RADIAL"}:
                fx = fy = params[0]
                cx, cy = params[1:3]
            else:
                fx, fy, cx, cy = params[:4]
            cameras[camera_id] = (width, height, fx, fy, cx, cy)
    if not cameras:
        raise ValueError(f"No COLMAP cameras found: {cameras_txt}")
    intrinsics = []
    for record in image_records:
        camera_id = record["camera_id"]
        if camera_id not in cameras:
            raise ValueError(
                f"COLMAP image {record['image_id']} references missing camera {camera_id}: {cameras_txt}"
            )
        intrinsics.append(cameras[camera_id])
    return intrinsics


def gs2mesh_legacy_cache_matches(output_dir, colmap_dir, signature):
    output_dir = Path(output_dir)
    camera_data_path = output_dir / "camera_data.json"
    if not camera_data_path.exists():
        return False
    try:
        camera_data = json.loads(camera_data_path.read_text(encoding="utf-8"))
        positions = np.asarray(
            [camera["left"]["pos"] for camera in camera_data], dtype=np.float64
        )
        baselines = np.asarray(
            [camera["left"]["baseline"] for camera in camera_data], dtype=np.float64
        )
    except (OSError, ValueError, KeyError, TypeError):
        return False
    images_txt = Path(colmap_dir) / "sparse" / "0" / "images.txt"
    if (
        positions.ndim != 2
        or positions.shape[1] != 3
        or len(positions) != count_colmap_images(images_txt)
        or not np.all(np.isfinite(positions))
        or not np.all(np.isfinite(baselines))
        or np.any(baselines <= 0)
    ):
        return False
    if not signature.get("scene_360"):
        return False
    try:
        image_records = gs2mesh_colmap_image_records(images_txt)
        current_positions = np.asarray(
            [record["center"] for record in image_records], dtype=np.float64
        )
        current_rotations = np.asarray(
            [record["camera_to_world_rotation"] for record in image_records],
            dtype=np.float64,
        )
        current_intrinsics = gs2mesh_colmap_renderer_intrinsics(
            Path(colmap_dir) / "sparse" / "0" / "cameras.txt",
            image_records,
        )
        cached_extrinsics = np.asarray(
            [camera["left"]["extrinsic"] for camera in camera_data],
            dtype=np.float64,
        )
        cached_intrinsics = [
            (
                int(camera["left"]["width"]),
                int(camera["left"]["height"]),
                float(camera["left"]["fx"]),
                float(camera["left"]["fy"]),
                float(camera["left"]["cx"]),
                float(camera["left"]["cy"]),
            )
            for camera in camera_data
        ]
    except (OSError, ValueError, KeyError, TypeError):
        return False
    if current_positions.shape != positions.shape or not np.allclose(
        current_positions, positions, rtol=1e-6, atol=1e-6
    ):
        return False
    if (
        cached_extrinsics.shape != (len(current_rotations), 4, 4)
        or not np.all(np.isfinite(cached_extrinsics))
        or not np.allclose(
            cached_extrinsics[:, :3, :3],
            current_rotations,
            rtol=1e-6,
            atol=1e-6,
        )
        or not np.allclose(
            cached_extrinsics[:, :3, 3],
            current_positions,
            rtol=1e-6,
            atol=1e-6,
        )
    ):
        return False
    if len(current_intrinsics) != len(cached_intrinsics) or any(
        left[:2] != right[:2]
        or not np.allclose(left[2:], right[2:], rtol=1e-6, atol=1e-6)
        for left, right in zip(current_intrinsics, cached_intrinsics)
    ):
        return False
    radius = float(np.median(np.linalg.norm(positions - positions.mean(axis=0), axis=1)))
    expected_baseline = radius * float(signature["baseline_percentage"]) / 100.0
    if not math.isclose(float(np.median(baselines)), expected_baseline, rel_tol=1e-4, abs_tol=1e-7):
        return False
    dependency_mtimes = [
        identity.get("mtime_ns", 0)
        for key, identity in signature.items()
        if key in {"point_cloud", "runner"} and isinstance(identity, dict)
    ]
    runner = GS2MESH_DIR / "run_single.py"
    if runner.exists():
        dependency_mtimes.append(runner.stat().st_mtime_ns)
    return camera_data_path.stat().st_mtime_ns + 5_000_000_000 >= max(dependency_mtimes or [0])


def quarantine_gs2mesh_render_cache(output_dir):
    output_dir = Path(output_dir)
    if not output_dir.exists():
        return None
    quarantine = output_dir.parent / ".stale"
    quarantine.mkdir(parents=True, exist_ok=True)
    target = quarantine / f"{output_dir.name}_{time.strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:8]}"
    shutil.move(str(output_dir), str(target))
    return target


def gs2mesh_camera_data(output_dir):
    path = Path(output_dir) / "camera_data.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list) or not data:
        raise RuntimeError(f"GS2Mesh camera cache is empty: {path}")
    baselines = np.asarray([camera["left"]["baseline"] for camera in data], dtype=np.float64)
    positions = np.asarray([camera["left"]["pos"] for camera in data], dtype=np.float64)
    if (
        not np.all(np.isfinite(baselines))
        or np.any(baselines <= 0)
        or not np.all(np.isfinite(positions))
    ):
        raise RuntimeError(f"GS2Mesh camera cache contains invalid coordinates: {path}")
    baseline = float(np.median(baselines))
    if np.max(np.abs(baselines - baseline)) > max(1e-7, baseline * 1e-4):
        raise RuntimeError(f"GS2Mesh camera cache contains inconsistent baselines: {path}")
    return data, baseline, positions


def profile_gs2mesh_render_cache(output_dir, options, max_samples=2_000_000):
    output_dir = Path(output_dir)
    opts = mesh_export_options(options)
    use_occlusion_mask = opts["gs2mesh_use_occlusion_mask"]
    camera_data, baseline, positions = gs2mesh_camera_data(output_dir)
    depth_paths = sorted(output_dir.glob("[0-9][0-9][0-9]/out_DLNR_Middlebury/depth.npy"))
    if len(depth_paths) != len(camera_data):
        raise RuntimeError(
            f"GS2Mesh cache has {len(depth_paths)} depth maps for {len(camera_data)} cameras: {output_dir}"
        )
    per_view_target = max(4096, int(max_samples) // max(1, len(depth_paths)))
    sampled = []
    sampled_unmasked = []
    sampled_positive = 0
    sampled_masked = 0
    for depth_path in depth_paths:
        depth = np.load(depth_path, mmap_mode="r")
        if depth.ndim != 2:
            depth = np.squeeze(depth)
        if depth.ndim != 2:
            raise RuntimeError(f"Unsupported GS2Mesh depth shape {depth.shape}: {depth_path}")
        stride = max(1, int(math.ceil(math.sqrt(depth.size / per_view_target))))
        depth_sample = np.asarray(depth[::stride, ::stride], dtype=np.float32).reshape(-1)
        positive = np.isfinite(depth_sample) & (depth_sample > 0)
        sampled_positive += int(np.count_nonzero(positive))
        unmasked_values = depth_sample[positive]
        if unmasked_values.size:
            sampled_unmasked.append(unmasked_values)
        mask_path = depth_path.with_name("occlusion_mask.npy")
        if use_occlusion_mask and mask_path.exists():
            mask = np.load(mask_path, mmap_mode="r")
            if mask.ndim != 2:
                mask = np.squeeze(mask)
            if mask.shape == depth.shape:
                positive &= np.asarray(mask[::stride, ::stride], dtype=bool).reshape(-1)
        values = depth_sample[positive]
        sampled_masked += int(values.size)
        if values.size:
            sampled.append(values)
    occlusion_mask_usable = bool(sampled) or not use_occlusion_mask
    if not sampled and sampled_unmasked:
        sampled = sampled_unmasked
        sampled_masked = sampled_positive
    if not sampled:
        raise RuntimeError(
            "GS2Mesh rendered depth maps contain no positive finite samples. "
            "The stereo cache must be regenerated before TSDF fusion."
        )
    depths = np.concatenate(sampled).astype(np.float64, copy=False)
    percentiles = np.percentile(depths, [0.5, 1.0, 50.0, 99.0, 99.5])
    requested_min = opts["gs2mesh_tsdf_min_depth_baselines"] * baseline
    requested_max = opts["gs2mesh_tsdf_max_depth_baselines"] * baseline
    requested_coverage = float(np.mean((depths >= requested_min) & (depths <= requested_max)))
    return {
        "output_dir": str(output_dir),
        "camera_count": len(camera_data),
        "baseline": baseline,
        "camera_positions": positions.tolist(),
        "sampled_positive": sampled_positive,
        "sampled_after_mask": sampled_masked,
        "masked_fraction": float(sampled_masked / max(1, sampled_positive)),
        "occlusion_mask_usable": occlusion_mask_usable,
        "requested_min_world": float(requested_min),
        "requested_max_world": float(requested_max),
        "requested_coverage": requested_coverage,
        "depth": {
            "min": float(depths.min()),
            "p0_5": float(percentiles[0]),
            "p1": float(percentiles[1]),
            "p50": float(percentiles[2]),
            "p99": float(percentiles[3]),
            "p99_5": float(percentiles[4]),
            "max": float(depths.max()),
        },
    }


def gs2mesh_scene_scale(model_path, iteration, profile, max_samples=250_000):
    point_cloud = (
        Path(model_path)
        / "point_cloud"
        / f"iteration_{int(iteration)}"
        / "point_cloud.ply"
    )
    try:
        vertices = PlyData.read(str(point_cloud), mmap="r")["vertex"].data
        count = len(vertices)
        stride = max(1, int(math.ceil(count / max_samples)))
        xyz = np.column_stack(
            [
                np.asarray(vertices[axis][::stride], dtype=np.float64)
                for axis in ("x", "y", "z")
            ]
        )
        xyz = xyz[np.all(np.isfinite(xyz), axis=1)]
        if len(xyz) >= 2:
            bounds = np.percentile(xyz, [1.0, 99.0], axis=0) if len(xyz) >= 100 else np.asarray([xyz.min(axis=0), xyz.max(axis=0)])
            diagonal = float(np.linalg.norm(bounds[1] - bounds[0]))
            if math.isfinite(diagonal) and diagonal > 0:
                return {"diagonal": diagonal, "source": "3DGS point cloud p1-p99", "sample_count": int(len(xyz))}
    except (OSError, ValueError, KeyError, TypeError):
        pass
    positions = np.asarray(profile.get("camera_positions", []), dtype=np.float64)
    camera_diagonal = 0.0
    if positions.ndim == 2 and positions.shape[1] == 3 and len(positions) >= 2:
        bounds = np.percentile(positions, [1.0, 99.0], axis=0)
        camera_diagonal = float(np.linalg.norm(bounds[1] - bounds[0]))
    fallback = max(camera_diagonal, float(profile["depth"]["p99_5"]))
    if not math.isfinite(fallback) or fallback <= 0:
        raise RuntimeError("Could not estimate a safe GS2Mesh scene scale from the point cloud or camera cache")
    return {"diagonal": fallback, "source": "camera/depth fallback", "sample_count": len(positions)}


def two_dgs_gaussian_extent(scene, iteration, max_samples=1_000_000):
    point_cloud = ply_path(scene, iteration)
    try:
        vertices = PlyData.read(str(point_cloud), mmap="r")["vertex"].data
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"Could not read the 2DGS point cloud for automatic mesh sizing: {point_cloud}") from exc

    count = len(vertices)
    stride = max(1, int(math.ceil(count / max_samples)))
    xyz = np.column_stack(
        [
            np.asarray(vertices[axis][::stride], dtype=np.float64)
            for axis in ("x", "y", "z")
        ]
    )
    xyz = xyz[np.all(np.isfinite(xyz), axis=1)]
    if len(xyz) < 2:
        raise RuntimeError("At least two finite 2DGS points are required for automatic mesh sizing")

    if len(xyz) >= 100:
        bounds = np.percentile(xyz, [1.0, 99.0], axis=0)
        source = "2DGS point cloud p1-p99"
    else:
        bounds = np.asarray([xyz.min(axis=0), xyz.max(axis=0)])
        source = "2DGS point cloud bounds"
    axis_extents = bounds[1] - bounds[0]
    extent = float(np.max(axis_extents))
    if not math.isfinite(extent) or extent <= 0:
        raise RuntimeError("Could not estimate a positive 2DGS Gaussian extent for automatic mesh sizing")
    return {
        "extent": extent,
        "axis_extents": [float(value) for value in axis_extents],
        "source": source,
        "sample_count": int(len(xyz)),
    }


def apply_2dgs_bounded_tsdf_defaults(scene, iteration, options):
    opts = dict(options)
    if opts.get("mode") != "bounded" or float(opts.get("voxel_size", -1.0)) > 0:
        return opts, None

    extent_info = two_dgs_gaussian_extent(scene, iteration)
    voxel_size = extent_info["extent"] / int(opts["mesh_res"])
    if not math.isfinite(voxel_size) or voxel_size <= 0:
        raise RuntimeError("Could not derive a positive 2DGS voxel size")
    opts["voxel_size"] = voxel_size
    if float(opts.get("sdf_trunc", -1.0)) <= 0:
        opts["sdf_trunc"] = voxel_size * 5.0
    return opts, extent_info


def resolve_gs2mesh_scale_options(options, profile, model_path, iteration):
    requested = mesh_export_options(options)
    effective = dict(requested)
    scene_scale = gs2mesh_scene_scale(model_path, iteration, profile)
    if requested["gs2mesh_auto_scale"]:
        baseline = float(profile["baseline"])
        low_baselines = float(profile["depth"]["p0_5"]) / baseline
        high_baselines = float(profile["depth"]["p99_5"]) / baseline
        safe_min = max(1, int(math.floor(low_baselines * 0.85)))
        safe_max = max(safe_min + 1, int(math.ceil((high_baselines * 1.20) / 16.0) * 16))
        safe_voxel = max(1, int(math.ceil(float(scene_scale["diagonal"]) * 512.0 / 1600.0)))
        effective["gs2mesh_tsdf_min_depth_baselines"] = min(
            requested["gs2mesh_tsdf_min_depth_baselines"], safe_min
        )
        effective["gs2mesh_tsdf_max_depth_baselines"] = min(
            100000,
            max(requested["gs2mesh_tsdf_max_depth_baselines"], safe_max),
        )
        effective["gs2mesh_tsdf_voxel"] = min(
            4096,
            max(requested["gs2mesh_tsdf_voxel"], safe_voxel),
        )
        if not profile.get("occlusion_mask_usable", True):
            effective["gs2mesh_use_occlusion_mask"] = False
    effective_min_world = effective["gs2mesh_tsdf_min_depth_baselines"] * profile["baseline"]
    effective_max_world = effective["gs2mesh_tsdf_max_depth_baselines"] * profile["baseline"]
    report = {
        "source_coordinates": "preserved",
        "tsdf_scale": 1.0,
        "scene_scale": scene_scale,
        "baseline": profile["baseline"],
        "depth": profile["depth"],
        "sampled_after_mask": profile["sampled_after_mask"],
        "requested_coverage": profile["requested_coverage"],
        "occlusion_mask_usable": profile.get("occlusion_mask_usable", True),
        "requested": {
            "voxel": requested["gs2mesh_tsdf_voxel"],
            "min_depth_baselines": requested["gs2mesh_tsdf_min_depth_baselines"],
            "max_depth_baselines": requested["gs2mesh_tsdf_max_depth_baselines"],
        },
        "effective": {
            "voxel": effective["gs2mesh_tsdf_voxel"],
            "voxel_world": effective["gs2mesh_tsdf_voxel"] / 512.0,
            "min_depth_baselines": effective["gs2mesh_tsdf_min_depth_baselines"],
            "max_depth_baselines": effective["gs2mesh_tsdf_max_depth_baselines"],
            "min_depth_world": effective_min_world,
            "max_depth_world": effective_max_world,
        },
    }
    return effective, report


def prepare_gs2mesh_render_cache_dir(
    scene,
    iteration,
    options,
    *,
    signature=None,
    quarantine_invalid=False,
):
    legacy, short = gs2mesh_output_dirs(scene, iteration, options)
    runtime_scene = gs2mesh_runtime_colmap_name(scene, options["gs2mesh_downsample"])
    runtime_colmap_dir = GS2MESH_DIR / "data" / "custom" / runtime_scene
    if gs2mesh_cache_complete(short, runtime_colmap_dir):
        if signature is None or gs2mesh_cache_manifest_matches(short, signature):
            return short
        if gs2mesh_legacy_cache_matches(short, runtime_colmap_dir, signature):
            write_gs2mesh_cache_manifest(short, signature)
            return short
        if quarantine_invalid:
            quarantine_gs2mesh_render_cache(short)
    if gs2mesh_cache_complete(legacy, runtime_colmap_dir):
        if signature is not None:
            if not gs2mesh_cache_manifest_matches(legacy, signature) and not gs2mesh_legacy_cache_matches(
                legacy, runtime_colmap_dir, signature
            ):
                return None
        link_or_copy_tree(legacy, short)
        if signature is not None:
            write_gs2mesh_cache_manifest(short, signature)
        return short
    return None


def gs2mesh_cleaned_mesh_pattern(scene, iteration, options):
    runtime_scene = gs2mesh_runtime_colmap_name(scene, options["gs2mesh_downsample"])
    dataset_name = gs2mesh_dataset_string(iteration, options["gs2mesh_baseline_percentage"])
    voxel = options["gs2mesh_tsdf_voxel"]
    min_depth = options["gs2mesh_tsdf_min_depth_baselines"]
    max_depth = options["gs2mesh_tsdf_max_depth_baselines"]
    return f"{runtime_scene}_{dataset_name}_*voxel{voxel}_512_trunc{min_depth}_{max_depth}_cleaned_mesh.ply"


def ply_mesh_element_counts(path):
    vertex_count = None
    face_count = None
    header_complete = False
    header_bytes = 0
    with open(path, "rb") as handle:
        if handle.readline(256).strip() != b"ply":
            raise RuntimeError(f"GS2Mesh output is not a PLY file: {path}")
        for _ in range(256):
            line = handle.readline(4096)
            if not line:
                break
            header_bytes += len(line)
            if header_bytes > 65536:
                break
            fields = line.decode("ascii", errors="replace").strip().split()
            if len(fields) == 3 and fields[:2] == ["element", "vertex"]:
                vertex_count = int(fields[2])
            elif len(fields) == 3 and fields[:2] == ["element", "face"]:
                face_count = int(fields[2])
            elif fields == ["end_header"]:
                header_complete = True
                break
    if not header_complete:
        raise RuntimeError(f"GS2Mesh output has an invalid PLY header: {path}")
    return int(vertex_count or 0), int(face_count or 0)


def build_gs2mesh_command(
    scene,
    iteration,
    options,
    *,
    skip_rendering=False,
    skip_tsdf=False,
):
    opts = mesh_export_options(options)
    command = [
        gs2mesh_python(),
        "run_single.py",
        "--colmap_name",
        safe_name(scene),
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
        str(int(iteration)),
        "--downsample",
        str(opts["gs2mesh_downsample"]),
        "--renderer_baseline_percentage",
        str(opts["gs2mesh_baseline_percentage"]),
        "--TSDF_scale",
        "1.0",
        "--TSDF_voxel",
        str(opts["gs2mesh_tsdf_voxel"]),
        "--TSDF_min_depth_baselines",
        str(opts["gs2mesh_tsdf_min_depth_baselines"]),
        "--TSDF_max_depth_baselines",
        str(opts["gs2mesh_tsdf_max_depth_baselines"]),
        "--TSDF_cleaning_threshold",
        str(opts["gs2mesh_tsdf_cleaning_threshold"]),
    ]
    if opts["task_port"]:
        command.extend(["--GS_port", str(opts["task_port"])])
    if not opts["gs2mesh_scene_360"]:
        command.append("--no-renderer_scene_360")
    if not opts["gs2mesh_use_occlusion_mask"]:
        command.append("--no-TSDF_use_occlusion_mask")
    if skip_rendering:
        command.append("--skip_rendering")
    if skip_tsdf:
        command.append("--skip_TSDF")
    return command


def probe_sugar_runtime(python_path):
    probe_code = (
        "from scripts.run_sugar import install_eager_dynamo_shim\n"
        "torch=install_eager_dynamo_shim()\n"
        "import torch\n"
        "import pytorch3d\n"
        "try:\n"
        " from pytorch3d import _C as pytorch3d_native\n"
        "except Exception as exc:\n"
        " raise RuntimeError(f'PyTorch3D native module probe failed: {exc}') from exc\n"
        "try:\n"
        " import open3d\n"
        "except Exception as exc:\n"
        " raise RuntimeError(f'Open3D native module probe failed: {exc}') from exc\n"
        "parameter=torch.nn.Parameter(torch.zeros(1))\n"
        "optimizer=torch.optim.Adam([parameter])\n"
        "parameter.sum().backward()\n"
        "optimizer.step()\n"
        "optimizer.zero_grad()\n"
        "print('torch', torch.__version__)\n"
        "print('pytorch3d', getattr(pytorch3d, '__version__', 'unknown'))\n"
        "print('open3d', open3d.__version__)\n"
        "print('adam', 'ok')\n"
    )
    return python_probe(
        python_path,
        sugar_env(),
        probe_code,
        timeout=RUNTIME_PROBE_TIMEOUT_SECONDS,
    )


def sugar_blocked_component(detail):
    text = str(detail or "").lower().replace("/", "\\")
    if "pytorch3d" in text or "tinyobjloader" in text:
        return "PyTorch3D"
    if "open3d" in text or "pybind" in text:
        return "Open3D"
    if any(marker in text for marker in ("torch\\", "c10.dll", "torch_cpu.dll", "torch_cuda.dll")):
        return "PyTorch"
    return "SuGaR native runtime"


def isolated_conda_path(env_root, inherited_path=""):
    env_root = Path(env_root).resolve()
    selected = os.path.normcase(str(env_root))
    envs_root = env_root.parent
    entries = [
        env_root,
        env_root / "Library" / "bin",
        env_root / "Library" / "usr" / "bin",
        env_root / "Scripts",
    ]
    seen = {os.path.normcase(str(path)) for path in entries}
    for item in str(inherited_path or "").split(os.pathsep):
        if not item:
            continue
        path = Path(item)
        normalized = os.path.normcase(str(path.resolve()))
        try:
            path.resolve().relative_to(envs_root)
            belongs_to_conda_envs = True
        except ValueError:
            belongs_to_conda_envs = False
        if belongs_to_conda_envs and normalized != selected and not normalized.startswith(selected + os.sep):
            continue
        if normalized not in seen:
            entries.append(path)
            seen.add(normalized)
    return os.pathsep.join(str(path) for path in entries)


def runtime_config_path(*, environ=None, home=None):
    environment = os.environ if environ is None else environ
    configured = environment.get("GS_EDITOR_RUNTIME_CONFIG")
    if configured:
        return Path(configured).expanduser()
    local_app_data = environment.get("LOCALAPPDATA")
    if local_app_data:
        base = Path(local_app_data)
    else:
        user_home = Path.home() if home is None else Path(home)
        base = user_home / "AppData" / "Local"
    return base / "Gaussian Scene Workbench" / "runtime.json"


def load_runtime_config(*, path=None, environ=None, home=None):
    config_path = Path(path) if path is not None else runtime_config_path(environ=environ, home=home)
    try:
        payload = json.loads(config_path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError, TypeError):
        return {}
    return payload if isinstance(payload, dict) else {}


def environment_python_path(env_root):
    root = Path(env_root)
    candidates = [root / "python.exe", root / "Scripts" / "python.exe"]
    return first_existing_path(candidates) or candidates[0]


def same_resolved_path(left, right):
    return os.path.normcase(str(Path(left).resolve())) == os.path.normcase(str(Path(right).resolve()))




def smart_app_control_state():
    if os.name != "nt" or winreg is None:
        return "not_applicable"
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\CI\Policy",
        ) as key:
            value, _ = winreg.QueryValueEx(key, "VerifiedAndReputablePolicyState")
    except OSError:
        return "unknown"
    return {0: "off", 1: "on", 2: "evaluation"}.get(int(value), "unknown")


def native_extension_policy_blocked(error_text, smart_app_state):
    if smart_app_state != "on":
        return False
    text = str(error_text or "").lower()
    markers = (
        "winerror 4551",
        "application control policy",
        "application-control policy",
        "アプリケーション制御ポリシー",
    )
    return any(marker in text for marker in markers)


def recover_2dgs_post_mesh(job, scene, iteration, mode, num_cluster, started_at):
    """Recover a fresh raw mesh when upstream 2DGS post-processing fails."""
    source_path = mesh_output_path(scene, iteration, mode, post=False)
    output_path = mesh_output_path(scene, iteration, mode, post=True)
    if not source_path.is_file():
        return False
    if source_path.stat().st_mtime < float(started_at) - 5.0:
        add_job_log(job, f"2DGS recovery ignored stale raw mesh: {source_path}")
        return False

    helper_path = ROOT / "scripts" / "postprocess_2dgs_mesh.py"
    if not helper_path.is_file():
        raise RuntimeError(f"Missing 2DGS mesh recovery helper: {helper_path}")
    add_job_log(
        job,
        "2DGS produced a fresh raw mesh but its connected-component post-process failed; "
        "recovering without rerunning rendering or TSDF integration.",
    )
    run_logged(
        job,
        [
            training_python("2dgs"),
            helper_path,
            "--input",
            source_path,
            "--output",
            output_path,
            "--num-cluster",
            str(int(num_cluster)),
        ],
        ROOT,
        "2dgs",
    )
    if not output_path.is_file():
        raise RuntimeError(f"2DGS mesh recovery finished without an output file: {output_path}")
    return True


def run_gs2mesh_mesh_job(job, options):
    scene = safe_name(job["scene"])
    iteration = int(job["iteration"])
    model_path = model_dir(scene)
    if scene_backend(scene, iteration) != "3dgs":
        raise ValueError("GS2Mesh mesh export is only available for 3DGS scenes")
    cfg = load_cfg_args(model_path)
    source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / scene)
    requested = mesh_export_options(options)
    source_identity = gs2mesh_source_camera_identity(source_path)
    prepare_gs2mesh_inputs(
        scene,
        model_path,
        source_path,
        iteration,
        requested["gs2mesh_downsample"],
    )
    if gs2mesh_source_camera_identity(source_path) != source_identity:
        raise RuntimeError(
            "GS2Mesh staging changed the aligned source camera files. The task was stopped to protect source coordinates."
        )
    add_job_log(job, f"Aligned source: {source_path}")
    add_job_log(
        job,
        "Coordinate policy: source cameras are read-only; world coordinates are preserved; TSDF_scale is fixed at 1.0.",
    )
    add_job_log(job, "Aligned camera source verification: unchanged after GS2Mesh staging.")

    signature = gs2mesh_cache_signature(scene, iteration, requested, model_path)
    cache_dir = prepare_gs2mesh_render_cache_dir(
        scene,
        iteration,
        requested,
        signature=signature,
        quarantine_invalid=True,
    )
    profile = None
    if cache_dir is not None and requested["gs2mesh_auto_scale"]:
        try:
            profile = profile_gs2mesh_render_cache(cache_dir, requested)
            add_job_log(job, f"Reusing verified GS2Mesh render/depth cache: {cache_dir}")
        except Exception as exc:
            quarantined = quarantine_gs2mesh_render_cache(cache_dir)
            add_job_log(
                job,
                f"WARNING: Cached GS2Mesh depth data failed validation ({exc}); moved to {quarantined} and regenerating.",
            )
            cache_dir = None

    if cache_dir is None and requested["gs2mesh_auto_scale"]:
        add_job_log(job, "GS2Mesh stage 1/2: rendering stereo pairs and depth cache without TSDF fusion.")
        set_job_stage(job, "running", "render_depth")
        render_command = build_gs2mesh_command(
            scene,
            iteration,
            requested,
            skip_tsdf=True,
        )
        run_logged(job, render_command, GS2MESH_DIR, "gs2mesh")
        cache_dir = gs2mesh_output_dirs(scene, iteration, requested)[1]
        runtime_scene = gs2mesh_runtime_colmap_name(scene, requested["gs2mesh_downsample"])
        runtime_colmap_dir = GS2MESH_DIR / "data" / "custom" / runtime_scene
        if not gs2mesh_cache_complete(cache_dir, runtime_colmap_dir):
            raise RuntimeError(
                f"GS2Mesh render/depth stage finished with an incomplete cache: {cache_dir}"
            )
        write_gs2mesh_cache_manifest(cache_dir, signature)
        try:
            profile = profile_gs2mesh_render_cache(cache_dir, requested)
        except Exception as exc:
            quarantined = quarantine_gs2mesh_render_cache(cache_dir)
            raise RuntimeError(
                f"GS2Mesh generated invalid render/depth data ({exc}); moved to {quarantined}"
            ) from exc
        add_job_log(job, f"GS2Mesh render/depth cache complete: {cache_dir}")

    effective = dict(requested)
    scale_report = None
    if requested["gs2mesh_auto_scale"]:
        if profile is None:
            profile = profile_gs2mesh_render_cache(cache_dir, requested)
        effective, scale_report = resolve_gs2mesh_scale_options(
            requested,
            profile,
            model_path,
            iteration,
        )
        with MESH_LOCK:
            job["effective_options"] = dict(effective)
            job["scale_report"] = scale_report
        add_job_log(
            job,
            "GS2Mesh depth profile: "
            f"baseline={scale_report['baseline']:.9g}, "
            f"p0.5={scale_report['depth']['p0_5']:.6g}, "
            f"p99.5={scale_report['depth']['p99_5']:.6g}, "
            f"requested coverage={scale_report['requested_coverage'] * 100:.4f}%.",
        )
        add_job_log(
            job,
            "GS2Mesh auto scale: "
            f"voxel {scale_report['requested']['voxel']} -> {scale_report['effective']['voxel']} "
            f"(world {scale_report['effective']['voxel_world']:.9g}), "
            f"depth {scale_report['requested']['min_depth_baselines']}.."
            f"{scale_report['requested']['max_depth_baselines']} -> "
            f"{scale_report['effective']['min_depth_baselines']}.."
            f"{scale_report['effective']['max_depth_baselines']} baselines "
            f"(world {scale_report['effective']['min_depth_world']:.6g}.."
            f"{scale_report['effective']['max_depth_world']:.6g}).",
        )
        add_job_log(
            job,
            f"Scene scale: {scale_report['scene_scale']['diagonal']:.6g} "
            f"({scale_report['scene_scale']['source']}); source transform remains unchanged.",
        )
        if not scale_report["occlusion_mask_usable"]:
            add_job_log(
                job,
                "WARNING: Occlusion masks removed all valid depth samples; TSDF will use finite stereo depth without that mask.",
            )
    else:
        with MESH_LOCK:
            job["effective_options"] = dict(effective)
        add_job_log(job, "GS2Mesh auto scale is disabled; using manual TSDF parameters without changing source coordinates.")

    cache_ready = cache_dir is not None
    add_job_log(job, "GS2Mesh stage 2/2: TSDF fusion in preserved source coordinates.")
    set_job_stage(job, "running", "tsdf_fusion")
    command = build_gs2mesh_command(
        scene,
        iteration,
        effective,
        skip_rendering=cache_ready,
    )
    started_at = time.time()
    run_logged(job, command, GS2MESH_DIR, "gs2mesh")
    result = collect_gs2mesh_mesh_output(scene, iteration, effective, started_at)
    return mesh_output_path(scene, iteration, "gs2mesh", post=True), effective, result


def task_port_available(port):
    port = int(port)
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        probe.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        probe.close()


def reserve_task_port(job, purpose, preferred=6009, start=TASK_PORT_START, end=TASK_PORT_END):
    preferred = int(preferred)
    start = int(start)
    end = int(end)
    if start > end:
        raise ValueError("Task port range is invalid")
    candidates = [preferred, *range(start, end + 1)]
    with TASK_PORT_LOCK:
        for port in candidates:
            if port < 1024 or port > 65535 or port in TASK_PORT_OWNERS:
                continue
            if not task_port_available(port):
                continue
            owner = str(job.get("id") or uuid.uuid4().hex)
            TASK_PORT_OWNERS[port] = {"job_id": owner, "purpose": str(purpose)}
            job["task_port"] = port
            job["task_port_purpose"] = str(purpose)
            job["task_port_released"] = False
            return port
    raise RuntimeError(
        f"No free task port is available (preferred {preferred}, range {start}-{end}). "
        "Stop the process using those ports and retry."
    )


def release_task_port(job):
    port = job.get("task_port")
    if port is None:
        return
    with TASK_PORT_LOCK:
        owner = TASK_PORT_OWNERS.get(int(port))
        if owner is None or owner.get("job_id") == str(job.get("id")):
            TASK_PORT_OWNERS.pop(int(port), None)
    job["task_port_released"] = True


def install_drive_root():
    return Path(ROOT.anchor) if ROOT.anchor else None


def conda_root_from_prefix(prefix):
    if not prefix:
        return None
    path = Path(prefix)
    if path.parent.name.lower() == "envs":
        return path.parent.parent
    return path


def conda_root_candidates():
    drive = install_drive_root()
    candidates = [
        os.environ.get("CONDA_ROOT"),
        os.environ.get("MINIFORGE_ROOT"),
        os.environ.get("MAMBA_ROOT_PREFIX"),
        conda_root_from_prefix(os.environ.get("CONDA_PREFIX")),
        drive / "miniforge3" if drive else None,
        drive / "conda" if drive else None,
        drive / "anaconda" if drive else None,
        Path.home() / "miniforge3",
    ]
    return [Path(candidate) for candidate in candidates if candidate]


def first_existing_path(candidates):
    for candidate in candidates:
        path = Path(candidate)
        if path.exists():
            return path
    return None


def gaussian_env_root():
    configured = [
        os.environ.get("GAUSSIAN_SPLATTING_CONDA_PREFIX"),
        os.environ.get("GS_CONDA_PREFIX"),
    ]
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix and Path(conda_prefix).name.lower() == "gaussian_splatting":
        configured.append(conda_prefix)
    for candidate in configured:
        if candidate and (Path(candidate) / "python.exe").exists():
            return Path(candidate)
    drive = install_drive_root()
    candidates = [
        drive / "miniforge3" / "envs" / "gaussian_splatting" if drive else None,
        drive / "conda" / "envs" / "gaussian_splatting" if drive else None,
        drive / "anaconda" / "envs" / "gaussian_splatting" if drive else None,
        Path.home() / "miniforge3" / "envs" / "gaussian_splatting",
        *[root / "envs" / "gaussian_splatting" for root in conda_root_candidates()],
    ]
    for candidate in [item for item in candidates if item]:
        if (candidate / "python.exe").exists():
            return candidate
    return next(item for item in candidates if item)


def conda_bat_path():
    candidates = [
        os.environ.get("CONDA_BAT"),
        *[root / "condabin" / "conda.bat" for root in conda_root_candidates()],
    ]
    return first_existing_path([item for item in candidates if item])


def mesh_like_job_kind(job):
    return job.get("kind") in {"mesh", "texture", "colmap"}


def safe_name(name):
    device_base = str(name or "").split(".", 1)[0].upper()
    if (
        not name
        or len(name) > 120
        or name.startswith(".")
        or name.endswith(".")
        or any(part in name for part in ("..", "/", "\\"))
        or not re.fullmatch(r"[A-Za-z0-9_.-]+", name)
        or device_base in WINDOWS_DEVICE_NAMES
    ):
        raise ValueError("Invalid scene name")
    return name


def safe_filename(name):
    name = Path(name or "file").name
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", name)
    return name or f"file_{uuid.uuid4().hex}"


def managed_path(root, *parts, allow_root=False):
    """Resolve a managed path without allowing sibling prefixes or escaping links."""
    root = os.path.realpath(os.fspath(root))
    target = os.path.realpath(os.path.join(root, *(os.fspath(part) for part in parts)))
    root_key = os.path.normcase(root)
    target_key = os.path.normcase(target)
    if target_key == root_key:
        if not allow_root:
            raise ValueError("Unsupported asset file")
    elif not target_key.startswith(root_key.rstrip(os.sep) + os.sep):
        raise ValueError("Unsupported asset file")
    # Keep the actual canonical spelling; normcase is only for Windows comparison.
    return Path(target)


def unique_path(directory, filename):
    candidate = managed_path(directory, filename)
    if not candidate.exists():
        return candidate
    stem = Path(filename).stem
    suffix = Path(filename).suffix
    return managed_path(directory, f"{stem}_{uuid.uuid4().hex[:8]}{suffix}")


def unlink_if_exists(path):
    try:
        Path(path).unlink()
        return True
    except FileNotFoundError:
        return False


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
    return managed_path(OUTPUT_DIR, safe_name(scene))


def ply_path(scene, iteration):
    root = model_dir(scene)
    try:
        return managed_path(root, "point_cloud", f"iteration_{int(iteration)}", "point_cloud.ply")
    except ValueError as exc:
        raise FileNotFoundError(str(exc)) from exc


def latest_iteration(scene):
    root = model_dir(scene)
    try:
        pc_dir = managed_path(root, "point_cloud")
    except ValueError as exc:
        raise FileNotFoundError(str(exc)) from exc
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


def inspect_training_point_cloud(path):
    """Return a lightweight PLY summary or raise when a training result is incomplete."""
    path = Path(path)
    if not path.is_file():
        raise FileNotFoundError(f"Training point cloud is missing: {path}")
    if path.stat().st_size <= 0:
        raise ValueError(f"Training point cloud is empty: {path}")

    header_lines = []
    header_size = 0
    with path.open("rb") as stream:
        while header_size < 1024 * 1024:
            line = stream.readline()
            if not line:
                break
            header_size += len(line)
            header_lines.append(line.decode("latin-1", errors="replace").strip())
            if header_lines[-1] == "end_header":
                break
    if not header_lines or header_lines[0] != "ply" or "end_header" not in header_lines:
        raise ValueError(f"Training point cloud has an invalid PLY header: {path}")
    format_lines = [line for line in header_lines if line.startswith("format ")]
    if not format_lines or format_lines[0].split()[1] not in {"ascii", "binary_little_endian"}:
        raise ValueError(f"Training point cloud uses an unsupported PLY format: {path}")
    vertex_count = 0
    vertex_properties = []
    vertex_stride = 0
    reading_vertex_properties = False
    scalar_sizes = {
        "char": 1, "uchar": 1, "int8": 1, "uint8": 1,
        "short": 2, "ushort": 2, "int16": 2, "uint16": 2,
        "int": 4, "uint": 4, "int32": 4, "uint32": 4,
        "float": 4, "float32": 4, "double": 8, "float64": 8,
    }
    for line in header_lines:
        match = re.fullmatch(r"element\s+vertex\s+(\d+)", line)
        if match:
            vertex_count = int(match.group(1))
            reading_vertex_properties = True
            continue
        if line.startswith("element "):
            reading_vertex_properties = False
            continue
        if reading_vertex_properties and line.startswith("property "):
            parts = line.split()
            if len(parts) != 3 or parts[1] not in scalar_sizes:
                raise ValueError(f"Training point cloud has an unsupported vertex property: {line}")
            vertex_stride += scalar_sizes[parts[1]]
            vertex_properties.append(parts[2])
    if vertex_count <= 0:
        raise ValueError(f"Training point cloud contains no vertices: {path}")
    missing_properties = {"x", "y", "z", "opacity", "scale_0", "rot_0"}.difference(
        vertex_properties
    )
    if missing_properties:
        raise ValueError(
            "Training point cloud is missing Gaussian properties "
            f"{', '.join(sorted(missing_properties))}: {path}"
        )
    if path.stat().st_size <= header_size:
        raise ValueError(f"Training point cloud contains a header but no vertex data: {path}")
    if format_lines[0].split()[1] == "binary_little_endian":
        expected_size = header_size + vertex_count * vertex_stride
        if path.stat().st_size < expected_size:
            raise ValueError(
                f"Training point cloud is truncated ({path.stat().st_size} < {expected_size} bytes): {path}"
            )
    return {
        "path": str(path.resolve()),
        "vertex_count": vertex_count,
        "properties": vertex_properties,
    }


def training_point_cloud(scene, expected_iteration=None):
    """Locate and validate the requested or latest usable training checkpoint."""
    point_cloud_root = managed_path(model_dir(scene), "point_cloud")
    if expected_iteration is not None:
        iterations = [int(expected_iteration)]
    elif point_cloud_root.is_dir():
        iterations = []
        for child in point_cloud_root.iterdir():
            match = re.fullmatch(r"iteration_(\d+)", child.name)
            if child.is_dir() and match:
                iterations.append(int(match.group(1)))
        iterations.sort(reverse=True)
    else:
        iterations = []

    last_error = None
    for iteration in iterations:
        try:
            candidate = ply_path(scene, iteration)
            summary = inspect_training_point_cloud(candidate)
            summary["iteration"] = iteration
            return summary
        except (OSError, ValueError) as exc:
            last_error = exc
    if expected_iteration is not None and last_error is not None:
        raise RuntimeError(str(last_error)) from last_error
    return None


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
    metadata_path = managed_path(model_dir(scene), "training_backend.json")
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


def read_vertices(path):
    ply = PlyData.read(path)
    if "vertex" not in ply:
        raise ValueError(f"No vertex element in {path}")
    vertices = ply["vertex"].data
    for field in ("x", "y", "z"):
        if field not in vertices.dtype.names:
            raise ValueError(f"Missing {field} field in {path}")
    return ply, vertices


def load_cfg_args(model_path):
    cfg_path = managed_path(model_path, "cfg_args")
    if not cfg_path.exists():
        return argparse.Namespace()
    text = cfg_path.read_text(encoding="utf-8")
    # cfg_args is serialized model metadata, never a Python program. Accept the
    # normal argparse representation, with literal keyword values only.
    expression = ast.parse(text, mode="eval").body
    if (not isinstance(expression, ast.Call)
            or not isinstance(expression.func, ast.Name)
            or expression.func.id != "Namespace" or expression.args):
        raise ValueError("Unsupported asset file")
    values = {}
    for keyword in expression.keywords:
        if keyword.arg is None or keyword.arg in values:
            raise ValueError("Unsupported asset file")
        values[keyword.arg] = ast.literal_eval(keyword.value)
    return argparse.Namespace(**values)


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
    return managed_path(model_dir(scene), "train", f"ours_{int(iteration)}", name)


def mesh_texture_dir(scene, iteration, mode="bounded", post=True):
    source = mesh_output_path(scene, iteration, mode, post)
    return managed_path(source.parent, f"{source.stem}_texture")


def mesh_texture_paths(scene, iteration, mode="bounded", post=True):
    scene = safe_name(scene)
    iteration = int(iteration)
    mode = safe_mesh_mode(mode)
    out_dir = mesh_texture_dir(scene, iteration, mode, post)
    stem = mesh_output_path(scene, iteration, mode, post).stem
    return {
        "dir": out_dir,
        "obj": managed_path(out_dir, f"{stem}_textured.obj"),
        "mtl": managed_path(out_dir, f"{stem}_textured.mtl"),
        "png": managed_path(out_dir, f"{stem}_texture.png"),
        "zip": managed_path(out_dir, f"{stem}_textured.zip"),
        "glb": managed_path(out_dir, f"{stem}_textured.glb"),
    }


def mesh_export_options(options):
    options = options or {}
    mode = safe_mesh_mode(options.get("mode", "bounded"))
    task_port = int(options.get("task_port", 0) or 0)
    if task_port and (task_port < 1024 or task_port > 65535):
        raise ValueError("task_port must be between 1024 and 65535")
    mesh_res = int(options.get("mesh_res", 512 if mode == "bounded" else 1024))
    num_cluster = int(options.get("num_cluster", 50))
    depth_ratio = float(options.get("depth_ratio", 0.0))
    if mode not in {"sugar", "gs2mesh"} and (mesh_res < 64 or mesh_res > 4096):
        raise ValueError("mesh_res must be between 64 and 4096")
    if num_cluster < 1 or num_cluster > 500:
        raise ValueError("num_cluster must be between 1 and 500")
    if not math.isfinite(depth_ratio) or depth_ratio < 0.0 or depth_ratio > 1.0:
        raise ValueError("depth_ratio must be finite and between 0 and 1")

    manual_tsdf = {}
    for name in ("voxel_size", "depth_trunc", "sdf_trunc"):
        value = float(options.get(name, -1.0))
        if not math.isfinite(value):
            raise ValueError(f"{name} must be finite")
        if value != -1.0 and value <= 0.0:
            raise ValueError(f"{name} must be positive or -1 for automatic sizing")
        manual_tsdf[name] = value
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
    if gs2mesh_tsdf_voxel < 1 or gs2mesh_tsdf_voxel > 4096:
        raise ValueError("gs2mesh_tsdf_voxel must be between 1 and 4096")
    gs2mesh_tsdf_min_depth_baselines = int(options.get("gs2mesh_tsdf_min_depth_baselines", 4))
    if gs2mesh_tsdf_min_depth_baselines < 1 or gs2mesh_tsdf_min_depth_baselines > 100:
        raise ValueError("gs2mesh_tsdf_min_depth_baselines must be between 1 and 100")
    gs2mesh_tsdf_max_depth_baselines = int(options.get("gs2mesh_tsdf_max_depth_baselines", 20))
    if (
        gs2mesh_tsdf_max_depth_baselines < gs2mesh_tsdf_min_depth_baselines
        or gs2mesh_tsdf_max_depth_baselines > 100000
    ):
        raise ValueError("gs2mesh_tsdf_max_depth_baselines must be greater than min and no more than 100000")
    gs2mesh_tsdf_cleaning_threshold = int(options.get("gs2mesh_tsdf_cleaning_threshold", 100000))
    if gs2mesh_tsdf_cleaning_threshold < 0 or gs2mesh_tsdf_cleaning_threshold > 10000000:
        raise ValueError("gs2mesh_tsdf_cleaning_threshold must be between 0 and 10000000")
    return {
        "mode": mode,
        "task_port": task_port,
        "mesh_res": mesh_res,
        "num_cluster": num_cluster,
        "depth_ratio": depth_ratio,
        "voxel_size": manual_tsdf["voxel_size"],
        "depth_trunc": manual_tsdf["depth_trunc"],
        "sdf_trunc": manual_tsdf["sdf_trunc"],
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
        "gs2mesh_auto_scale": safe_bool(options.get("gs2mesh_auto_scale"), True),
        "gs2mesh_use_occlusion_mask": safe_bool(options.get("gs2mesh_use_occlusion_mask"), True),
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
    try:
        target = managed_path(root, target)
    except ValueError as exc:
        raise RuntimeError(f"Refusing to remove path outside staging root: {target}") from exc
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


GS2MESH_CACHE_MANIFEST = ".gs_editor_cache.json"
GS2MESH_CACHE_SIGNATURE_VERSION = 2


def count_colmap_images(images_txt):
    count = 0
    expecting_image = True
    with open(images_txt, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            if expecting_image:
                if not stripped:
                    continue
                count += 1
                expecting_image = False
            else:
                expecting_image = True
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
    return prepare_gs2mesh_render_cache_dir(scene, iteration, options) is not None


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


def colmap_process_env():
    env = os.environ.copy()
    for name in list(env):
        normalized = name.upper()
        if normalized.startswith("CONDA_") or normalized.startswith("_CONDA_"):
            env.pop(name, None)
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
    canonicalize_gs2mesh_camera_mapping(dataset_dir / "sparse" / "0")
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


def find_latest_gs2mesh_cleaned_mesh(scene, iteration, options, started_at):
    expected_roots = gs2mesh_output_dirs(scene, iteration, options)
    threshold = max(0.0, float(started_at) - 5.0)
    pattern = gs2mesh_cleaned_mesh_pattern(scene, iteration, options)
    candidates = []
    for root in expected_roots:
        if not root.exists():
            continue
        for path in root.rglob(pattern):
            if path.is_file() and path.stat().st_mtime >= threshold:
                candidates.append(path)
    if not candidates:
        roots = ", ".join(str(path) for path in expected_roots)
        raise FileNotFoundError(
            f"GS2Mesh did not create a fresh cleaned mesh for scene '{scene}'. "
            f"Expected {pattern} under: {roots}. Existing render/depth caches were preserved."
        )
    return max(candidates, key=lambda path: path.stat().st_mtime)


def collect_gs2mesh_mesh_output(scene, iteration, options, started_at):
    opts = mesh_export_options(options)
    source = find_latest_gs2mesh_cleaned_mesh(scene, iteration, opts, started_at)
    vertex_count, face_count = ply_mesh_element_counts(source)
    used_raw_fallback = False
    if vertex_count <= 0 or face_count <= 0:
        raw_source = Path(str(source).replace("_cleaned_mesh.ply", "_mesh.ply"))
        if raw_source.exists() and raw_source.stat().st_mtime >= max(0.0, float(started_at) - 5.0):
            raw_vertices, raw_faces = ply_mesh_element_counts(raw_source)
            if raw_vertices > 0 and raw_faces > 0:
                source = raw_source
                vertex_count, face_count = raw_vertices, raw_faces
                used_raw_fallback = True
        if vertex_count <= 0 or face_count <= 0:
            raise RuntimeError(
                f"GS2Mesh produced a cleaned mesh with no vertices or faces for scene '{scene}': {source}"
            )
    target = mesh_output_path(scene, iteration, "gs2mesh", post=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return {
        "source": str(source),
        "mesh": str(target),
        "vertices": vertex_count,
        "faces": face_count,
        "cleaned": not used_raw_fallback,
        "warning": (
            "GS2Mesh cleaning removed every face; the non-empty raw TSDF mesh was preserved instead."
            if used_raw_fallback
            else None
        ),
    }


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
            SUGAR_RUNNER,
            SUGAR_DIR / "train.py",
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
        command = build_gs2mesh_command(
            scene,
            iteration,
            opts,
            skip_rendering=can_reuse_render_cache,
        )
        return command, GS2MESH_DIR, mesh_output_path(scene, iteration, opts["mode"], post=True)
    if backend != "2dgs":
        raise ValueError("Mesh export is only available for 2DGS scenes")
    if not ply_path(scene, iteration).exists():
        raise FileNotFoundError(f"Missing point cloud for mesh export: {ply_path(scene, iteration)}")
    opts, _extent_info = apply_2dgs_bounded_tsdf_defaults(scene, iteration, opts)
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
    env_python = environment_python_path(sugar_env_root())
    return env_python


SUGAR_RUNNER = ROOT / "scripts" / "run_sugar.py"


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
    if not SUGAR_RUNNER.exists():
        problems.append(f"Missing SuGaR compatibility runner: {SUGAR_RUNNER}")
    if same_resolved_path(SUGAR_DIR, GS2MESH_DIR):
        problems.append("SuGaR and GS2Mesh source directories must be different")
    if same_resolved_path(python_path, gs2mesh_python()):
        problems.append("SuGaR and GS2Mesh must use different Python environments")
    if not problems:
        runtime_ok, runtime_detail = probe_sugar_runtime(python_path)
        if not runtime_ok:
            if native_extension_policy_blocked(runtime_detail, smart_app_control_state()):
                component = sugar_blocked_component(runtime_detail)
                problems.append(
                    f"Windows Smart App Control blocked the unsigned {component} native module required by SuGaR. "
                    "Smart App Control has no per-file allow rule, and reinstalling or running as administrator will "
                    "not make an unsigned module trusted. Keep Windows security enabled and use either a PyTorch3D "
                    "build signed by a CA in the Microsoft Trusted Root Program or a Linux/WSL SuGaR worker. "
                    "GS2Mesh remains available as the Windows-native mesh backend. "
                    f"Detail: {runtime_detail}"
                )
            else:
                problems.append(f"SuGaR native runtime imports failed: {runtime_detail}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return {"sugar_dir": str(SUGAR_DIR), "python": str(python_path), "script": str(script_path)}


def gs2mesh_python():
    configured = os.environ.get("GS2MESH_PYTHON")
    if configured:
        return Path(configured)
    return environment_python_path(gs2mesh_env_root())


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
    if same_resolved_path(GS2MESH_DIR, SUGAR_DIR):
        problems.append("GS2Mesh and SuGaR source directories must be different")
    if same_resolved_path(python_path, sugar_python()):
        problems.append("GS2Mesh and SuGaR must use different Python environments")
    if not problems:
        probe = (
            "import torch, cv2, open3d, matplotlib.pyplot\n"
            "import diff_gaussian_rasterization, simple_knn\n"
            "print('cuda', torch.cuda.is_available())\n"
        )
        runtime_ok, detail = python_probe(
            python_path,
            gs2mesh_env(),
            probe,
            timeout=90,
        )
        if not runtime_ok:
            problems.append(
                f"GS2Mesh runtime imports failed in {python_path}: {detail}"
            )
    if problems:
        raise RuntimeError("; ".join(problems))
    return {"gs2mesh_dir": str(GS2MESH_DIR), "python": str(python_path), "script": str(script_path)}


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
    source_obj = find_latest_sugar_obj(source_path, started_at)
    texture_bundle = collect_textured_obj_bundle(source_obj, source_obj.parent, output_paths)
    geometry_ply = obj_to_ascii_ply(output_paths["obj"], mesh_output_path(scene, iteration, "sugar", post=True))
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
        "pngs": texture_bundle["pngs"],
        "texture_pages": texture_bundle["texture_pages"],
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


def versioned_colmap_candidates(install_roots=None):
    drive_root = Path(ROOT.anchor) if ROOT.anchor else ROOT
    if install_roots is None:
        install_roots = (
            drive_root / "Tools" / "COLMAP",
            drive_root / "tools" / "colmap",
            drive_root / "COLMAP",
        )
    candidates = []
    for install_root in map(Path, install_roots):
        direct = install_root / "bin" / "colmap.exe"
        if not install_root.is_dir():
            continue

        def version_key(path):
            try:
                return tuple(int(part) for part in path.name.split("."))
            except ValueError:
                return ()

        version_dirs = sorted(
            (path for path in install_root.iterdir() if path.is_dir()),
            key=version_key,
            reverse=True,
        )
        candidates.extend(
            path / "bin" / "colmap.exe"
            for path in version_dirs
            if (path / "bin" / "colmap.exe").is_file()
        )
        if direct.is_file():
            candidates.append(direct)
    return candidates


def openmvs_executable(name):
    exe_name = f"{name}.exe" if os.name == "nt" and not name.lower().endswith(".exe") else name
    for directory in openmvs_bin_dirs():
        candidate = directory / exe_name
        if candidate.exists():
            return candidate
    found = shutil.which(exe_name)
    return Path(found) if found else None


def colmap_executable():
    for variable in ("COLMAP_EXE", "COLMAP_PATH"):
        configured = os.environ.get(variable)
        if configured and Path(configured).is_file():
            return Path(configured)
    fallback = ROOT / "third_party" / "colmap" / "bin" / "colmap.exe"
    for candidate in (
        fallback,
        ROOT / "tools" / "colmap" / "bin" / "colmap.exe",
        ROOT / "colmap" / "bin" / "colmap.exe",
        *versioned_colmap_candidates(),
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
    work_dir = managed_path(output_paths["dir"], work_subdir)
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
    work_dir = managed_path(output_paths["dir"], "colmap_texturer")
    work_dir.mkdir(parents=True, exist_ok=True)
    workspace = prepare_colmap_texturer_workspace(dataset_path, work_dir)
    texturer_output = managed_path(work_dir, "output")
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


def safe_texture_asset_path(reference, parent, trusted_root):
    """Resolve generated OBJ dependencies without allowing arbitrary file reads."""
    reference = str(reference).strip()
    if len(reference) >= 2 and reference[0] == reference[-1] and reference[0] in {"'", '"'}:
        reference = reference[1:-1]
    normalized = reference.replace("\\", "/")
    if (not normalized or "\x00" in normalized or normalized.startswith("/")
            or re.match(r"^[a-zA-Z]:", normalized)):
        raise RuntimeError(f"Texture dependency must be a relative path: {reference}")
    trusted_root = Path(trusted_root).resolve()
    try:
        dependency = (Path(parent) / normalized).resolve()
    except (OSError, ValueError) as exc:
        raise RuntimeError(f"Invalid texture dependency path: {reference}") from exc
    try:
        dependency.relative_to(trusted_root)
    except ValueError:
        raise RuntimeError(f"Texture dependency escapes its source directory: {reference}")
    if not dependency.is_file():
        raise RuntimeError(f"Texture dependency does not exist: {reference}")
    return dependency


def collect_textured_obj_bundle(source_obj, trusted_root, output_paths, openmvs_opaque=False):
    """Copy each material's image, retaining UV/material bindings and source files.

    A single MTL is supported. Ambiguous multi-library declarations and map
    options are rejected instead of publishing an incomplete textured model.
    Output PNG names do not depend on source basenames, so atlas pages from
    different subdirectories cannot overwrite one another.
    """
    source_obj = Path(source_obj).resolve()
    trusted_root = Path(trusted_root).resolve()
    try:
        source_obj.relative_to(trusted_root)
    except ValueError:
        raise RuntimeError(f"Textured OBJ escapes its source directory: {source_obj}")
    obj_lines = source_obj.read_text(encoding="utf-8", errors="replace").splitlines()
    material_lines = []
    for index, line in enumerate(obj_lines):
        parts = line.strip().split(None, 1)
        if parts and parts[0].lower() == "mtllib":
            if len(parts) != 2:
                raise RuntimeError(f"OBJ has an empty material library: {source_obj}")
            material_lines.append((index, parts[1]))
    if len(material_lines) != 1:
        raise RuntimeError(f"Textured OBJ must reference exactly one MTL library: {source_obj}")
    material_line, material_reference = material_lines[0]
    if len(re.findall(r"\.mtl(?:[\"']|\s|$)", material_reference, flags=re.IGNORECASE)) > 1:
        raise RuntimeError(f"Textured OBJ must reference exactly one MTL library: {source_obj}")
    source_mtl = safe_texture_asset_path(material_reference, source_obj.parent, trusted_root)
    if source_mtl.suffix.lower() != ".mtl":
        raise RuntimeError(f"OBJ material dependency must be one MTL file: {material_reference}")
    mtl_lines = source_mtl.read_text(encoding="utf-8", errors="replace").splitlines()
    material_normalization = None
    if openmvs_opaque:
        # OpenMVS MaterialLib::Save writes Tr 1.000000 unconditionally, while
        # its reader ignores Tr and treats these generated materials as opaque.
        # Normalize that exact scalar convention in the owned copy only; other
        # exporters and actual transparent values retain their original meaning.
        # https://github.com/cdcseacave/openMVS/blob/master/libs/IO/OBJ.cpp
        for index, line in enumerate(mtl_lines):
            parts = line.strip().split()
            if len(parts) != 2 or parts[0] != "Tr":
                continue
            try:
                value = float(parts[1])
            except ValueError:
                continue
            if math.isfinite(value) and value == 1.0:
                mtl_lines[index] = "d 1"
                material_normalization = "openmvs_opaque_Tr"
    image_commands = {"map_ka", "map_kd", "map_ks", "map_ke", "map_ns", "map_d",
                      "map_bump", "bump", "disp", "decal", "norm", "refl"}
    image_lines = []
    for index, line in enumerate(mtl_lines):
        parts = line.strip().split(None, 1)
        if not parts or parts[0].lower() not in image_commands:
            continue
        if len(parts) != 2 or parts[1].startswith("-"):
            raise RuntimeError(f"Unsupported or empty MTL image map: {line.strip()}")
        texture = safe_texture_asset_path(parts[1], source_mtl.parent, trusted_root)
        if texture.suffix.lower() not in IMAGE_EXTS:
            raise RuntimeError(f"Unsupported MTL texture image: {texture.name}")
        image_lines.append((index, parts[0], texture))
    if not any(command.lower() == "map_kd" for _, command, _ in image_lines):
        raise RuntimeError(f"MTL has no diffuse texture image: {source_mtl}")

    # Keep the first diffuse map as the legacy primary PNG even if auxiliary
    # maps precede it. Shared source images only need to be converted once.
    ordered_images = sorted(image_lines, key=lambda item: item[1].lower() != "map_kd")
    converted = {}
    primary_png = Path(output_paths["png"])
    for _, _, source in ordered_images:
        if source in converted:
            continue
        page = len(converted)
        target = primary_png if page == 0 else primary_png.with_name(
            f"{primary_png.stem}-page-{page + 1:02d}.png")
        converted[source] = target
    sources = {source_obj, source_mtl} | set(converted)
    targets = [Path(output_paths[key]) for key in ("obj", "mtl", "zip", "glb")] + list(converted.values())
    for target in targets:
        if target.resolve() in sources:
            raise RuntimeError(f"Texture collection must not overwrite its source: {target}")

    Path(output_paths["dir"]).mkdir(parents=True, exist_ok=True)
    for source, target in converted.items():
        convert_texture_to_png(source, target)
    obj_lines[material_line] = f"mtllib {Path(output_paths['mtl']).name}"
    for index, command, source in image_lines:
        mtl_lines[index] = f"{command} {converted[source].name}"
    for key, lines in (("obj", obj_lines), ("mtl", mtl_lines)):
        with Path(output_paths[key]).open("w", encoding="utf-8", newline="\n") as stream:
            stream.write("\n".join(lines) + "\n")
    with zipfile.ZipFile(output_paths["zip"], "w", zipfile.ZIP_DEFLATED) as archive:
        for path in [Path(output_paths["obj"]), Path(output_paths["mtl"])] + list(converted.values()):
            archive.write(path, path.name)
    result = {"pngs": [str(path) for path in converted.values()], "texture_pages": len(converted)}
    if material_normalization:
        result["material_normalization"] = material_normalization
    return result


def collect_openmvs_texture_outputs(work_dir, output_paths):
    work_dir = Path(work_dir)
    obj_files = sorted(work_dir.rglob("*.obj"), key=lambda p: (0 if "text" in p.stem.lower() else 1, -p.stat().st_mtime))
    if not obj_files:
        raise RuntimeError(f"OpenMVS finished but no OBJ was written in {work_dir}")
    source_obj = obj_files[0]
    texture_bundle = collect_textured_obj_bundle(source_obj, work_dir, output_paths, openmvs_opaque=True)
    glb_error = None
    try:
        export_textured_obj_to_glb(output_paths)
    except Exception as exc:
        glb_error = str(exc)
    result = {
        "obj": str(output_paths["obj"]),
        "mtl": str(output_paths["mtl"]),
        "png": str(output_paths["png"]),
        "pngs": texture_bundle["pngs"],
        "zip": str(output_paths["zip"]),
        "glb": str(output_paths["glb"]) if output_paths["glb"].exists() else None,
        "glb_error": glb_error,
        "openmvs_obj": str(source_obj),
        "texture_pages": texture_bundle["texture_pages"],
    }
    if texture_bundle.get("material_normalization"):
        result["material_normalization"] = texture_bundle["material_normalization"]
    return result


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


def job_snapshot(job):
    return {
        "trainingDelivery": job.get("trainingDelivery"),
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
        "dataset_path": job.get("dataset_path"),
        "resume_from_scene": job.get("resume_from_scene"),
        "pose_policy": job.get("pose_policy"),
        "camera_dataset_path": job.get("camera_dataset_path"),
        "camera_dataset_identity": job.get("camera_dataset_identity"),
        "resolution_policy": job.get("resolution_policy"),
        "cameraDataset": job.get("cameraDataset"),
        "resume_checkpoint": job.get("resume_checkpoint"),
        "resume_checkpoint_iteration": job.get("resume_checkpoint_iteration"),
        "iteration": job.get("iteration"),
        "total_iterations": job.get("total_iterations"),
        "trainingSummary": normalize_training_summary(job.get("trainingSummary")),
        "trainingPerformance": normalize_performance(job.get("trainingPerformance")),
        "trainingGrowth": normalize_growth(job.get("trainingGrowth")),
        "runtimeConfiguration": job.get("runtimeConfiguration"),
        "runtimePolicyHistory": job.get("runtimePolicyHistory", []),
        "trainingFailure": job.get("trainingFailure"),
        "pause_reason": job.get("pause_reason"),
        "progressPercent": job.get("progressPercent"),
        "loss": job.get("loss"),
        "psnr": job.get("psnr"),
        "gaussian_count": job.get("gaussian_count"),
        "density_guard_iteration": job.get("density_guard_iteration"),
        "density_guard_deferred": job.get("density_guard_deferred"),
        "reconstruction_quality": job.get("reconstruction_quality"),
        "reconstruction_quality_phase": job.get("reconstruction_quality_phase"),
        "generation_issue": job.get("generation_issue"),
        "iteration_milliseconds": job.get("iteration_milliseconds"),
        "elapsed_seconds": job.get("elapsed_seconds"),
        "latest_iteration": job.get("latest_iteration"),
        "preview_iteration": job.get("preview_iteration"),
        "preview_kind": job.get("preview_kind"),
        "point_cloud_path": job.get("point_cloud_path"),
        "partial_point_cloud_path": job.get("partial_point_cloud_path"),
        "point_count": job.get("point_count"),
        "log": job["log"][-240:],
        "cancel_requested": bool(job.get("cancel_requested")),
    }


def finite_training_number(value, *, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    numeric = float(value)
    if not math.isfinite(numeric) or numeric < 0:
        return None
    return int(numeric) if integer else numeric


def training_event_payload(line, prefix):
    marker = line.find(prefix)
    if marker < 0:
        return None
    try:
        encoded = line[marker + len(prefix):].lstrip()
        payload, _end = json.JSONDecoder().raw_decode(encoded)
    except (TypeError, ValueError, json.JSONDecodeError):
        return None
    return payload if isinstance(payload, dict) else None


def apply_training_metrics(job, payload):
    iteration = finite_training_number(payload.get("iteration"), integer=True)
    total = finite_training_number(payload.get("total_iterations"), integer=True)
    if total is not None and total > 0:
        job["total_iterations"] = total
    effective_total = job.get("total_iterations")
    if iteration is not None:
        job["iteration"] = min(iteration, effective_total) if effective_total else iteration
    if effective_total and job.get("iteration") is not None:
        job["progressPercent"] = max(
            0, min(100, round(job["iteration"] / effective_total * 100))
        )

    for name in ("loss", "psnr", "iteration_milliseconds", "elapsed_seconds"):
        value = finite_training_number(payload.get(name))
        if value is not None:
            job[name] = value
    gaussian_count = finite_training_number(payload.get("gaussian_count"), integer=True)
    if gaussian_count is not None:
        job["gaussian_count"] = gaussian_count


def apply_training_preview(job, payload):
    iteration = finite_training_number(payload.get("iteration"), integer=True)
    point_cloud_path = payload.get("point_cloud_path")
    if iteration is not None and isinstance(point_cloud_path, str) and point_cloud_path.strip():
        kind = payload.get("preview_kind", "training_checkpoint")
        if kind not in {"training_checkpoint", "gaussian_initial", "gaussian_live"}:
            return
        if (job.get("preview_kind") != "colmap_sparse" and
                iteration < int(job.get("preview_iteration") or 0)):
            return
        if (iteration == job.get("preview_iteration") and
                job.get("preview_kind") == "training_checkpoint" and kind != "training_checkpoint"):
            return
        job["latest_iteration"] = iteration
        job["preview_iteration"] = iteration
        job["partial_point_cloud_path"] = point_cloud_path
        job["preview_kind"] = kind
        count = finite_training_number(payload.get("gaussian_count"), integer=True)
        if count is not None:
            job["gaussian_count"] = count


def apply_density_control(job, payload):
    iteration = finite_training_number(payload.get("iteration"), integer=True)
    deferred = finite_training_number(payload.get("deferred"), integer=True)
    if (type(payload.get("version")) is int and payload["version"] == 1
            and type(payload.get("iteration")) is int and type(payload.get("deferred")) is int
            and iteration is not None and deferred is not None
            and iteration > 0 and 0 < deferred <= 2147483647
            and iteration <= 2147483647
            and iteration >= int(job.get("density_guard_iteration") or 0)):
        job["density_guard_iteration"] = iteration
        job["density_guard_deferred"] = deferred


def apply_training_input(job, payload):
    summary = normalize_training_summary(payload)
    configured = normalize_training_summary(job.get("trainingSummary"))
    if (summary is None or summary["phase"] != "loaded" or configured is None
            or any(summary.get(key) != configured.get(key) for key in PARAMETER_FIELDS)):
        return
    job["trainingSummary"] = summary


def add_job_log(job, message):
    line = str(message).rstrip()
    if not line:
        return
    lock = MESH_LOCK if mesh_like_job_kind(job) else TRAIN_LOCK
    with lock:
        if not mesh_like_job_kind(job):
            metrics_payload = training_event_payload(line, TRAINING_METRICS_PREFIX)
            preview_payload = training_event_payload(line, TRAINING_PREVIEW_PREFIX)
            density_payload = training_event_payload(line, TRAINING_DENSITY_PREFIX)
            input_payload = training_event_payload(line, TRAINING_INPUT_PREFIX)
            performance_payload = normalize_performance(training_event_payload(line, TRAINING_PERFORMANCE_PREFIX))
            growth_payload = normalize_growth(training_event_payload(line, TRAINING_GROWTH_PREFIX))
            acceleration_payload = training_event_payload(line, "[gsw-training-acceleration]")
            runtime_payload = training_event_payload(line, "[gsw-training-runtime-configuration]")
            if runtime_payload is not None:
                try:
                    from native.worker.training_configuration import normalize_runtime_options
                    if runtime_payload.get("backend") != job.get("backend", "3dgs"):
                        raise ValueError("Runtime backend mismatch")
                    for key in ("requested", "effective"):
                        normalized = normalize_runtime_options(runtime_payload[key])["requested"]
                        if normalized != runtime_payload[key]:
                            raise ValueError("Runtime configuration is incomplete")
                    encoded = json.dumps(runtime_payload, ensure_ascii=False, allow_nan=False)
                    if len(encoded.encode("utf-8")) > 1024 * 1024:
                        raise ValueError("Runtime report is too large")
                    job["runtimeConfiguration"] = json.loads(encoded)
                except (KeyError, TypeError, ValueError):
                    pass  # Malformed observation cannot rewrite execution settings.
            runtime_change = training_event_payload(line, "[gsw-training-runtime-policy-change]")
            if (runtime_change is not None and type(runtime_change.get("iteration")) is int
                    and runtime_change["iteration"] >= 0 and isinstance(runtime_change.get("reason"), str)):
                try:
                    encoded = json.dumps(runtime_change, allow_nan=False)
                    if len(encoded.encode("utf-8")) <= 64 * 1024:
                        history = job.setdefault("runtimePolicyHistory", [])
                        history.append(json.loads(encoded))
                        del history[:-256]
                except (TypeError, ValueError):
                    pass
            failure = training_event_payload(line, "[gsw-training-error]")
            if (failure is not None and failure.get("version") == 1
                    and failure.get("backend") == job.get("backend", "3dgs")
                    and failure.get("kind") in ("cuda_oom", "cuda_oom_reported", "training_exception")):
                job["trainingFailure"] = failure
                job["generation_issue"] = ("training_exception" if failure["kind"] == "training_exception"
                                           else "training_" + failure["kind"])
            if acceleration_payload is not None and acceleration_payload.get("resumeBlocked") is True:
                job["generation_issue"] = "training_renderer_resume"
            if growth_payload is not None and growth_payload["iteration"] >= int(
                    (job.get("trainingGrowth") or {}).get("iteration", 0)):
                job["trainingGrowth"] = growth_payload
            if performance_payload is not None and performance_payload["iteration"] >= int(
                    (job.get("trainingPerformance") or {}).get("iteration", 0)):
                job["trainingPerformance"] = performance_payload
            if metrics_payload is not None:
                apply_training_metrics(job, metrics_payload)
            if preview_payload is not None:
                apply_training_preview(job, preview_payload)
            if density_payload is not None:
                apply_density_control(job, density_payload)
            if input_payload is not None:
                apply_training_input(job, input_payload)
            progress_match = TRAINING_PROGRESS_PATTERN.search(line)
            if progress_match and metrics_payload is None:
                iteration = int(progress_match.group(1))
                total_iterations = max(int(progress_match.group(2)), 1)
                job["iteration"] = max(0, min(iteration, total_iterations))
                job["total_iterations"] = total_iterations
                job["progressPercent"] = max(
                    0, min(100, round(job["iteration"] / total_iterations * 100))
                )
        job["log"].append(line)
        job["updated_at"] = time.time()
        if not mesh_like_job_kind(job):
            persist_train_job(job)


def set_job_stage(job, status, stage):
    lock = MESH_LOCK if mesh_like_job_kind(job) else TRAIN_LOCK
    with lock:
        job["status"] = status
        job["stage"] = stage
        job["updated_at"] = time.time()
        if not mesh_like_job_kind(job):
            persist_train_job(job)


def training_env(backend="3dgs"):
    backend = safe_training_backend(backend)
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    loss_path = env.get("GSW_TRAINING_LOSS_MODULE_PATH_" + backend.upper())
    if loss_path:
        env["GSW_TRAINING_LOSS_MODULE_PATH"] = loss_path
    env_root = gaussian_env_root()
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
        configured_python = training_python(backend)
        runtime_prefix = configured_python.parent
        if runtime_prefix.name.lower() == "scripts":
            runtime_prefix = runtime_prefix.parent
        managed_cuda = Path(ROOT.anchor) / "Gaussian-Scene-Workbench-Runtime" / "cuda-11.8"
        cuda_location = os.environ.get("TWO_DGS_CUDA_PATH") or (
            str(managed_cuda) if managed_cuda.is_dir() else os.environ.get("CUDA_PATH"))
        path_parts = [
            str(configured_python.parent), str(runtime_prefix / "Library" / "bin"),
            str(two_dgs_venv / "Scripts"),
            *path_parts,
        ]
        if cuda_location:
            path_parts.insert(0, str(Path(cuda_location) / "bin"))
            env["CUDA_PATH"] = cuda_location
            env["CUDA_HOME"] = cuda_location
        env["TORCH_CUDA_ARCH_LIST"] = env.get("TORCH_CUDA_ARCH_LIST", "8.6")
        env_root = runtime_prefix
    env["PATH"] = os.pathsep.join(path_parts)
    env["CONDA_PREFIX"] = str(env_root)
    env["GAUSSIAN_SPLATTING_CONDA_PREFIX"] = str(env_root)
    env["GS_CONDA_PREFIX"] = str(env_root)
    return env


def sugar_env_root(*, environ=None, home=None):
    environment = os.environ if environ is None else environ
    runtime_config = load_runtime_config(environ=environment, home=home)
    configured = [
        runtime_config.get("sugar_env_root"),
        environment.get("SUGAR_CONDA_PREFIX"),
    ]
    runtime_root = environment.get("GS_EDITOR_RUNTIME_ROOT") or runtime_config.get("runtime_root")
    if runtime_root:
        configured.append(Path(runtime_root) / "envs" / "sugar")
    for candidate in configured:
        if candidate and environment_python_path(candidate).exists():
            return Path(candidate)
    user_home = Path.home() if home is None else Path(home)
    return user_home / "miniforge3" / "envs" / "sugar"


def sugar_env():
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    env_root = sugar_env_root()
    env["PATH"] = isolated_conda_path(env_root, env.get("PATH", ""))
    env["CONDA_PREFIX"] = str(env_root)
    env.pop("PYTHONPATH", None)
    return env


def gs2mesh_env_root():
    configured = os.environ.get("GS2MESH_CONDA_PREFIX")
    if configured:
        return Path(configured).expanduser().resolve()
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix and Path(conda_prefix).name.lower() == "gs2mesh":
        return Path(conda_prefix).resolve()
    drive = install_drive_root()
    candidates = [
        drive / "miniforge3" / "envs" / "gs2mesh" if drive else None,
        drive / "conda" / "envs" / "gs2mesh" if drive else None,
        drive / "anaconda" / "envs" / "gs2mesh" if drive else None,
        Path.home() / "miniforge3" / "envs" / "gs2mesh",
        *[root / "envs" / "gs2mesh" for root in conda_root_candidates()],
    ]
    usable = [Path(candidate) for candidate in candidates if candidate]
    for candidate in usable:
        if (candidate / "python.exe").exists():
            return candidate.resolve()
    return usable[0].resolve()


def gs2mesh_env():
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    env_root = gs2mesh_env_root()
    env["PATH"] = isolated_conda_path(env_root, env.get("PATH", ""))
    env["CONDA_PREFIX"] = str(env_root)
    env.pop("PYTHONPATH", None)
    return env


def training_python(backend="3dgs"):
    backend = safe_training_backend(backend)
    if backend == "2dgs":
        configured = os.environ.get("TWO_DGS_PYTHON")
        if configured:
            return Path(configured)
        local_python = TWO_DGS_DIR / ".venv" / "Scripts" / "python.exe"
        managed_python = Path(ROOT.anchor) / "conda" / "envs" / "gsw_2dgs" / "python.exe"
        return local_python if local_python.is_file() or not managed_python.is_file() else managed_python
    gaussian_python = gaussian_env_root() / "python.exe"
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
            stdin=subprocess.DEVNULL,
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


def training_environment_report(backend="3dgs", camera_policy=None):
    backend = safe_training_backend(backend)
    env = training_env(backend)
    env_root = Path(env["CONDA_PREFIX"])
    conda_bat = conda_bat_path()
    miniforge_root = conda_bat.parents[1] if conda_bat else env_root.parents[1]
    python_path = training_python(backend)
    colmap_path = colmap_executable()
    if colmap_path is None:
        colmap_path = ROOT / "third_party" / "colmap" / "bin" / "colmap.exe"
    ffmpeg_path = env_root / "Library" / "bin" / "ffmpeg.exe"
    git_path = command_path("git")
    vs_install = vs_build_tools_installation()
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
    runtime_code = ("import torch, cv2; from PIL import Image; import " +
        ("diff_surfel_rasterization; " if backend == "2dgs" else "diff_gaussian_rasterization; ") +
        "from simple_knn._C import distCUDA2; "
        "assert torch.cuda.is_available(), 'PyTorch cannot access a CUDA device'; "
        "print('cuda', torch.cuda.get_device_name(0))")
    if camera_policy == 'locked' and backend == '3dgs':
        env['GSW_POSE_POLICY'] = 'locked'
        env['GSW_CAMERA_DATASET'] = 'explicit-known-camera-runtime-admission'
        runtime_code = ('import sys; sys.path.insert(0,' + repr(str(ROOT)) + '); '
                        'from native.worker.camera_rasterizer_runtime import configure_camera_rasterizer; '
                        'configure_camera_rasterizer(' + repr(str(ROOT)) + '); ') + runtime_code
    runtime_ok, runtime_detail = python_probe(python_path, env, runtime_code)
    report = {
        "backend": backend,
        "root": str(ROOT),
        "miniforge": str(miniforge_root),
        "miniforge_exists": miniforge_root.exists(),
        "conda": str(conda_bat or ""),
        "conda_exists": bool(conda_bat and conda_bat.exists()),
        "python": str(python_path),
        "python_exists": python_path.exists(),
        "env_root": str(env_root),
        "env_root_exists": env_root.exists(),
        "colmap": str(colmap_path),
        "colmap_exists": colmap_path.exists(),
        "git": str(git_path) if git_path else "",
        "git_exists": bool(git_path),
        "vs_build_tools": str(vs_install) if vs_install else "",
        "vs_build_tools_exists": bool(vs_install),
        "ffmpeg": str(ffmpeg_path),
        "ffmpeg_exists": ffmpeg_path.exists(),
        "gaussian_dir": str(GAUSSIAN_DIR),
        "gaussian_dir_exists": GAUSSIAN_DIR.exists(),
        "two_dgs_dir": str(TWO_DGS_DIR),
        "two_dgs_dir_exists": TWO_DGS_DIR.exists(),
        "two_dgs_python": str(training_python("2dgs")),
        "two_dgs_python_exists": training_python("2dgs").exists(),
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


def ensure_training_environment(backend="3dgs", require_colmap=True, camera_policy=None):
    backend = safe_training_backend(backend)
    report = (training_environment_report(backend, camera_policy=camera_policy) if camera_policy
              else training_environment_report(backend))
    problems = []
    if not report["python_exists"]:
        problems.append(f"Missing Python executable: {report['python']}")
    if not report["gaussian_dir_exists"]:
        problems.append(f"Missing gaussian-splatting directory: {report['gaussian_dir']}")
    if backend == "2dgs":
        if not report["two_dgs_dir_exists"]:
            problems.append(f"Missing 2DGS directory: {report['two_dgs_dir']}")
        if not report["two_dgs_python_exists"]:
            problems.append(f"Missing 2DGS Python: {report['two_dgs_python']}")
        if not report["two_dgs_train_exists"]:
            problems.append(f"Missing 2DGS train.py: {report['two_dgs_train']}")
    if require_colmap and not report["colmap_exists"]:
        problems.append(f"Missing COLMAP executable: {report['colmap']}")
    if not report["opencv_ok"]:
        problems.append(
            f"OpenCV unavailable for training image/depth loading: {report['opencv_error']}"
        )
    if not report["runtime_imports_ok"]:
        detail = report["runtime_imports_error"]
        lowered_detail = detail.lower()
        if (
            "winerror 4551" in lowered_detail
            or "application control policy" in lowered_detail
            or "アプリケーション制御ポリシー" in lowered_detail
            or "应用控制策略" in lowered_detail
        ):
            problems.append(
                "Windows Enterprise Application Control blocked a PyTorch/CUDA training library. "
                "Ask the system administrator to allow or sign the blocked runtime; the application "
                f"will not bypass enterprise policy. Details: {detail}"
            )
        else:
            problems.append(f"{backend.upper()} PyTorch/CUDA runtime unavailable: {detail}")
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


class NativeTrainingPaused(Exception):
    def __init__(self, manifest):
        super().__init__("Native training paused at iteration {}".format(manifest["iteration"]))
        self.manifest = manifest


def run_logged(job, args, cwd, backend=None):
    if job.get("cancel_requested"):
        raise RuntimeError("Training cancelled by user")
    add_job_log(job, f"> {' '.join(str(a) for a in args)}")
    if backend == "sugar":
        process_env = sugar_env()
    elif backend == "gs2mesh":
        process_env = gs2mesh_env()
    else:
        process_env = training_env(backend or job.get("backend", "3dgs"))
    native_control = job.get("native_control") if len(args) > 1 and str(args[1]) == "train.py" and backend in TRAINING_BACKENDS else None
    camera_training = len(args) > 1 and str(args[1]) == "train.py" and backend in TRAINING_BACKENDS
    if camera_training:
        from native.worker.training_seed import validate_training_seed
        process_env["GSW_TRAINING_SEED"] = str(validate_training_seed(job.get("training_seed", 0)))
        process_env.pop("GSW_CAMERA_DATASET", None)
        process_env.pop("GSW_CAMERA_IMAGE_ROOT", None)
        process_env.pop("GSW_POSE_POLICY", None)
        if job.get("pose_policy"):
            process_env["GSW_POSE_POLICY"] = job["pose_policy"]
        prepared = job.get("camera_prepared")
        if prepared:
            process_env["GSW_CAMERA_DATASET"] = prepared["manifestPath"]
            process_env["GSW_CAMERA_IMAGE_ROOT"] = prepared["imageRoot"]
    if native_control:
        control_payload = dict(native_control)
        if job.get("training_seed", 0):
            control_payload["training_seed"] = job["training_seed"]
        summary = normalize_training_summary(job.get("trainingSummary"))
        if summary is not None:
            control_payload["trainingSummary"] = summary
        process_env["GSW_NATIVE_TRAINING_CONTROL"] = json.dumps(control_payload)
        if backend == "2dgs":
            # Keep external upstream/runtime files untouched. The versioned
            # native entry retains the official 2DGS optimization algorithm.
            args = list(args)
            args[1] = ROOT / "native" / "worker" / "two_dgs_train.py"
            process_env["GSW_TWO_DGS_SOURCE"] = str(Path(cwd).resolve())
    elif camera_training and backend == "2dgs" and job.get("camera_prepared"):
        args = list(args)
        args[1] = ROOT / "native" / "worker" / "two_dgs_train.py"
        process_env["GSW_TWO_DGS_SOURCE"] = str(Path(cwd).resolve())
    process = subprocess.Popen(
        [str(a) for a in args],
        cwd=str(cwd),
        env=process_env,
        stdin=subprocess.DEVNULL,
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
        if process.stdout is not None:
            process.stdout.close()
        with lock:
            if job.get("process") is process:
                job["process"] = None
    if job.get("cancel_requested"):
        raise RuntimeError("Training cancelled by user")
    if rc == 75 and native_control:
        from native.worker.training_checkpoint import read_manifest
        manifest, _ = read_manifest(native_control["output"])
        if (manifest.get("session") != native_control["session"] or
                manifest.get("backend", "3dgs") != backend):
            raise RuntimeError("Pause exited without a checkpoint from this training session")
        raise NativeTrainingPaused(manifest)
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
        "quality_recovery": True,
        # Explicit calibration is preserved unless fallback is opted into.
        "auto_camera_fallback": "camera_model" not in payload,
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
    options["quality_recovery"] = safe_bool(payload.get("quality_recovery"), options["quality_recovery"])
    options["auto_camera_fallback"] = safe_bool(payload.get("auto_camera_fallback"), options["auto_camera_fallback"])
    return options


def colmap_image_input_path(dataset):
    dataset = Path(dataset)
    for candidate in (dataset / "input", dataset / "images", dataset):
        if candidate.exists() and any(
            item.is_file() and item.suffix.lower() in IMAGE_EXTS
            for item in candidate.iterdir()
        ):
            return candidate
    return dataset / "images"


def colmap_snapshot_frames_frequency(dataset, target_count=COLMAP_SNAPSHOT_TARGET_COUNT):
    """Choose a bounded native mapper snapshot cadence for live point previews."""
    image_path = colmap_image_input_path(dataset)
    image_count = 0
    if image_path.exists():
        image_count = sum(
            1
            for item in image_path.iterdir()
            if item.is_file() and item.suffix.lower() in IMAGE_EXTS
        )
    target_count = max(int(target_count), 1)
    return max(1, math.ceil(max(image_count, 1) / target_count))


def configure_colmap_mapper_snapshots(command, snapshot_path, frames_frequency):
    command.extend(
        [
            "--Mapper.snapshot_path",
            Path(snapshot_path),
            "--Mapper.snapshot_frames_freq",
            str(max(int(frames_frequency), 1)),
        ]
    )
    return command


def read_colmap_points3d_binary(path, max_points=COLMAP_POINT_PREVIEW_MAX_POINTS):
    """Read a bounded, deterministic sample from a COLMAP points3D.bin file."""
    path = Path(path)
    file_size = path.stat().st_size
    if file_size < 8:
        raise ValueError(f"COLMAP point model is incomplete: {path}")
    max_points = max(int(max_points), 1)
    points = []
    with path.open("rb") as stream:
        header = stream.read(8)
        if len(header) != 8:
            raise ValueError(f"COLMAP point model has no header: {path}")
        source_count = struct.unpack("<Q", header)[0]
        if source_count > 100_000_000:
            raise ValueError(
                f"COLMAP point model declares an unsafe point count: {source_count}"
            )
        sample_stride = max(1, math.ceil(source_count / max_points))
        record_size = struct.calcsize("<QdddBBBdQ")
        for index in range(source_count):
            record = stream.read(record_size)
            if len(record) != record_size:
                raise ValueError(
                    f"COLMAP point model ended inside point {index}: {path}"
                )
            (
                _point_id,
                x,
                y,
                z,
                red,
                green,
                blue,
                _error,
                track_length,
            ) = struct.unpack("<QdddBBBdQ", record)
            track_bytes = int(track_length) * struct.calcsize("<II")
            if track_bytes > file_size - stream.tell():
                raise ValueError(
                    f"COLMAP point track is incomplete at point {index}: {path}"
                )
            if index % sample_stride == 0 and len(points) < max_points:
                if math.isfinite(x) and math.isfinite(y) and math.isfinite(z):
                    points.append((float(x), float(y), float(z), red, green, blue))
            stream.seek(track_bytes, os.SEEK_CUR)
    return int(source_count), points


def write_point_cloud_preview_ply(path, points):
    """Atomically publish a compact binary PLY consumable by the native viewport."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    header = (
        "ply\n"
        "format binary_little_endian 1.0\n"
        "comment Gaussian Scene Workbench live COLMAP preview\n"
        f"element vertex {len(points)}\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property uchar red\n"
        "property uchar green\n"
        "property uchar blue\n"
        "end_header\n"
    ).encode("ascii")
    try:
        with temporary.open("wb") as stream:
            stream.write(header)
            for x, y, z, red, green, blue in points:
                stream.write(struct.pack("<fffBBB", x, y, z, red, green, blue))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            try:
                temporary.unlink()
            except OSError:
                pass
    return path


class ColmapPointCloudPreviewPublisher:
    """Publish stable mapper snapshots as incrementally numbered point-cloud PLYs."""

    def __init__(
        self,
        job,
        dataset,
        source_roots,
        interval=COLMAP_POINT_PREVIEW_INTERVAL_SECONDS,
        output_dir=None,
    ):
        self.job = job
        self.dataset = Path(dataset)
        self.source_roots = [Path(root) for root in source_roots]
        job_id = safe_filename(job.get("id") or uuid.uuid4().hex)
        self.output_dir = (Path(output_dir) if output_dir is not None else
                           self.dataset / ".gsw" / "previews" / "colmap" / job_id)
        if output_dir is not None:
            from native.backend.camera_dataset import CameraDatasetError
            if self.output_dir.parent.resolve() != self.dataset.resolve():
                raise CameraDatasetError("camera_unsafe_solve_destination", str(self.output_dir))
            try:
                self.output_dir.mkdir(exist_ok=False)
            except FileExistsError as error:
                raise CameraDatasetError("camera_solve_destination_exists", str(self.output_dir)) from error
        self.interval = max(float(interval), 0.05)
        self.stop_event = threading.Event()
        self.thread = None
        self.observed = {}
        self.published_signatures = set()
        self.generation = 0

    def start(self):
        self.output_dir.mkdir(parents=True, exist_ok=True)
        thread_name = safe_filename(self.job.get("id") or "job")[:24]
        self.thread = threading.Thread(
            target=self._run,
            name=f"gsw-colmap-preview-{thread_name}",
            daemon=True,
        )
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join()
            self.thread = None
        self.publish_latest(force=True)
        self.prune()

    def _run(self):
        while not self.stop_event.wait(self.interval):
            self.publish_latest(force=False)

    def candidates(self):
        candidates = []
        for root in self.source_roots:
            if root.exists():
                candidates.extend(root.rglob("points3D.bin"))
        unique = {}
        for candidate in candidates:
            try:
                stat = candidate.stat()
            except OSError:
                continue
            unique[str(candidate.resolve())] = (
                candidate,
                stat.st_mtime_ns,
                stat.st_size,
            )
        return sorted(
            unique.values(), key=lambda item: (item[1], item[2]), reverse=True
        )

    def publish_latest(self, force=False):
        for candidate, modified_ns, size in self.candidates():
            signature = (str(candidate.resolve()), modified_ns, size)
            if signature in self.published_signatures:
                continue
            previous = self.observed.get(signature[0])
            self.observed[signature[0]] = (modified_ns, size)
            if not force and previous != (modified_ns, size):
                continue
            try:
                source_count, points = read_colmap_points3d_binary(candidate)
                if source_count <= 0 or not points:
                    continue
                generation = self.generation + 1
                output = self.output_dir / f"sparse_{generation:06d}.ply"
                write_point_cloud_preview_ply(output, points)
            except (OSError, OverflowError, ValueError, struct.error):
                continue

            self.generation = generation
            self.published_signatures.add(signature)
            lock = MESH_LOCK if mesh_like_job_kind(self.job) else TRAIN_LOCK
            with lock:
                self.job["latest_iteration"] = generation
                self.job["preview_iteration"] = generation
                self.job["partial_point_cloud_path"] = str(output.resolve())
                self.job["point_count"] = source_count
                self.job["preview_kind"] = "colmap_sparse"
                self.job["updated_at"] = time.time()
                self.job.setdefault("log", []).append(
                    f"COLMAP sparse preview {generation}: {source_count} points "
                    f"({len(points)} displayed)"
                )
                if not mesh_like_job_kind(self.job):
                    persist_train_job(self.job)
            return True
        return False

    def prune(self, keep=4):
        if not self.output_dir.exists():
            return
        previews = sorted(self.output_dir.glob("sparse_*.ply"))
        for preview in previews[:-max(int(keep), 1)]:
            try:
                preview.unlink()
            except OSError:
                pass


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


def colmap_convert_commands(dataset, options, undistort_output=None, work_root=None):
    dataset = Path(dataset)
    colmap = colmap_executable()
    if not colmap:
        raise RuntimeError("Missing COLMAP executable")
    image_path = colmap_image_input_path(dataset)
    undistort_output = Path(undistort_output) if undistort_output else dataset / ".colmap-undistorted"
    work_root = Path(work_root) if work_root else dataset / "distorted"
    database_path = work_root / "database.db"
    sparse_path = work_root / "sparse"
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
        undistort_output,
        "--output_type",
        "COLMAP",
        # Calibration/undistortion is still required, but its training images
        # must not acquire an implicit size cap from a COLMAP version default.
        "--max_image_size",
        "-1",
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


def validate_undistorted_colmap_output(output):
    output = Path(output)
    normalize_undistorted_sparse(output)
    images = output / "images"
    if not images.is_dir() or not any(
        item.is_file() and item.suffix.lower() in IMAGE_EXTS
        for item in images.iterdir()
    ):
        raise RuntimeError("COLMAP image undistortion produced no training images")
    if not dataset_has_colmap_scene(output):
        raise RuntimeError("COLMAP image undistortion produced an incomplete sparse model")


def publish_undistorted_colmap_output(dataset, staging, backup=None):
    """Replace trainable COLMAP folders only after staging has been validated."""
    dataset = Path(dataset).resolve()
    staging = Path(staging).resolve()
    if staging.parent != dataset:
        raise ValueError("COLMAP undistortion staging must be a direct dataset child")

    required = ("images", "sparse")
    optional = ("stereo",)
    for name in required:
        if not (staging / name).is_dir():
            raise RuntimeError(f"COLMAP image undistortion is missing {name}")

    preserve_colmap_source_images(dataset)

    backup = Path(backup) if backup is not None else dataset / f".colmap-backup-{uuid.uuid4().hex}"
    if backup.parent.resolve() != dataset or backup.exists():
        raise ValueError("COLMAP backup must be a fresh direct dataset child")
    backup.mkdir()
    moved_existing = []
    published = []
    try:
        for name in (*required, *optional):
            target = dataset / name
            if target.exists():
                target.rename(backup / name)
                moved_existing.append(name)
        for name in (*required, *optional):
            source = staging / name
            if source.exists():
                source.rename(dataset / name)
                published.append(name)
        if not dataset_has_colmap_scene(dataset):
            raise RuntimeError("Published COLMAP output has an incomplete sparse model")
    except Exception:
        for name in reversed(published):
            target = dataset / name
            if target.is_dir():
                shutil.rmtree(target)
            elif target.exists():
                target.unlink()
        for name in reversed(moved_existing):
            source = backup / name
            if source.exists():
                source.rename(dataset / name)
        if backup.exists() and not any(backup.iterdir()):
            backup.rmdir()
        raise
    else:
        shutil.rmtree(backup)


def _archived_extraction_fps(dataset, record):
    value = record.get("extractionFps")
    if isinstance(value, (int, float)) and math.isfinite(value) and value > 0:
        return value
    # Legacy imports did not write FPS to their source manifest. Recover only
    # an unambiguous matching import config; never invent an extraction rate.
    rates = set()
    for config_path in (dataset.parent.parent / ".gsw" / "jobs").glob("import-*.json"):
        try:
            config = json.loads(config_path.read_text(encoding="utf-8-sig"))
            if config.get("scene") != dataset.name:
                continue
            names = {Path(item if isinstance(item, str) else item.get("path", "")).name
                     for item in config.get("files", []) if isinstance(item, (str, dict))}
            fps = float(config.get("fps", 0))
            if record.get("originalFilename") in names and math.isfinite(fps) and fps > 0:
                rates.add(fps)
        except (OSError, ValueError, TypeError):
            continue
    return next(iter(rates)) if len(rates) == 1 else None


def preserve_colmap_source_images(dataset, job=None, staging=None):
    """Retain all raw input frames before publishing a registered subset.

    Older datasets are repaired from their archived media, in a private
    staging folder. Existing input, training images and archive are not edited.
    """
    dataset = Path(dataset).resolve()
    if not dataset.is_dir():
        return dataset / "images"
    destination = dataset / "input"
    manifest_path = dataset / "source" / "metadata_manifest.json"
    records = []
    if manifest_path.is_file():
        records = json.loads(manifest_path.read_text(encoding="utf-8-sig")).get("files", [])
    count = reconstruction_quality.image_count(destination)
    expected = sum(1 if r.get("kind") == "image" else max(int(r.get("extractedFrames", 0)), 0) for r in records)
    if count:
        if expected > count:
            raise RuntimeError("source_frames_missing: existing input does not contain all original frames")
        return destination
    # An explicitly supplied empty input folder must not be replaced silently.
    if destination.exists():
        raise RuntimeError("source_frames_missing: input directory is empty")
    images = dataset / "images"
    staging = Path(staging) if staging is not None else dataset / (".colmap-source-" + uuid.uuid4().hex)
    if staging.parent.resolve() != dataset:
        raise ValueError("COLMAP source stage must be a fresh direct dataset child")
    with owned_directory(staging):
        if records:
            for record in records:
                archive = (dataset / record.get("archivePath", "")).resolve()
                if dataset not in archive.parents or not archive.is_file():
                    raise RuntimeError("source_frames_missing: archived source is unavailable")
                if record.get("kind") == "image":
                    name = Path(record.get("trainingPath") or archive.name).name
                    if (staging / name).exists():
                        raise RuntimeError("source_frames_missing: duplicate original image name")
                    shutil.copy2(archive, staging / name)
                elif record.get("kind") == "video":
                    fps = _archived_extraction_fps(dataset, record)
                    if fps is None:
                        raise RuntimeError("source_frames_missing: legacy extraction FPS is unknown; reimport original media")
                    expected = int(record.get("extractedFrames", 0))
                    extracted = extract_video_frames(archive, staging, fps)
                    if expected > 0 and extracted != expected:
                        raise RuntimeError("source_frames_missing: original frame count changed")
            if not reconstruction_quality.image_count(staging):
                raise RuntimeError("source_frames_missing: no recoverable archived images")
        elif images.is_dir():
            for image in images.iterdir():
                if image.is_file() and image.suffix.lower() in IMAGE_EXTS:
                    shutil.copy2(image, staging / image.name)
        if not reconstruction_quality.image_count(staging):
            return images
        # Preserve existing alpha masks when all source names are available.
        if (dataset / "source" / "masks").is_dir():
            # The regular masking helper uses an images child, in owned staging.
            mask_root = staging / ".mask-work"
            (mask_root / "source").mkdir(parents=True)
            shutil.copytree(dataset / "source" / "masks", mask_root / "source" / "masks")
            (mask_root / "images").mkdir()
            for image in list(staging.iterdir()):
                if image.is_file():
                    image.rename(mask_root / "images" / image.name)
            apply_alpha_masks_to_dataset(mask_root)
            for image in (mask_root / "images").iterdir():
                image.rename(staging / image.name)
            shutil.rmtree(mask_root)
        staging.rename(destination)
        if job is not None:
            add_job_log(job, "Preserved/recovered {} original COLMAP frames: {}".format(reconstruction_quality.image_count(destination), destination))
        return destination


def report_reconstruction_quality(job, report, phase):
    job["reconstruction_quality"] = report
    job["reconstruction_quality_phase"] = phase
    add_job_log(job, "COLMAP quality [{}]: registered {}/{}, valid points {}/{}, model {}, reasons {}".format(
        phase, report.get("registeredImages", 0), report.get("inputImages", 0),
        report.get("validPoints", 0), report.get("sparsePoints", 0),
        report.get("modelPath", "-"), ",".join(report.get("reasons", []))))


def plan_colmap_path_layout(job, dataset, nonce=None):
    """Plan and admit all known COLMAP paths before cache reset/process work.

    Actual IDs and the chosen per-run nonce are retained in the record. IDs
    are never shortened to become task identity or spliced into long paths.
    Snapshot child length follows COLMAP4.2.1 WriteSnapshot's size_t decimal
    formatting, not today's timestamp length or the number of photographs.
    """
    from native.backend.camera_dataset import CameraDatasetError
    from native.backend.camera_admission import _is_reparse
    from native.backend.path_budget import PathBudgetError, require_legacy_windows_paths
    dataset = Path(dataset).resolve()
    nonce = str(nonce if nonce is not None else uuid.uuid4().hex)
    if not nonce or not nonce.isascii() or not nonce.isalnum():
        raise CameraDatasetError("camera_unsafe_solve_destination", "invalid COLMAP run nonce")
    if any(_is_reparse(path) for path in (dataset, *dataset.parents)):
        raise CameraDatasetError("camera_unsafe_solve_destination")
    path_token = hashlib.sha256(nonce.encode("ascii")).hexdigest()[:16]
    paths = {name: dataset / (prefix + path_token) for name, prefix in (
        ("undistort", ".u"), ("quality", ".q"), ("snapshots", ".s"),
        ("preview", ".p"), ("sourceStage", ".i"), ("backup", ".b"))}
    if any(path.exists() or _is_reparse(path) for path in paths.values()):
        raise CameraDatasetError("camera_solve_destination_exists", "COLMAP run path collision")
    image_root = colmap_image_input_path(dataset)
    names = {path.name for path in image_root.iterdir()
             if path.is_file() and path.suffix.lower() in IMAGE_EXTS} if image_root.is_dir() else set()
    metadata = dataset / "source" / "metadata_manifest.json"
    if not names and metadata.is_file():
        for record in json.loads(metadata.read_text(encoding="utf-8-sig")).get("files", []):
            archive = Path(record.get("archivePath", ""))
            if record.get("kind") == "image":
                names.add(Path(record.get("trainingPath") or archive.name).name)
            elif record.get("kind") == "video":
                stem = safe_filename(archive.stem)
                number = max(int(record.get("extractedFrames", 0)) - 1, 0)
                names.add("%s_%06d.jpg" % (stem, number))
    files, directories = set(), {dataset, *paths.values()}
    model_names = ("cameras.bin", "images.bin", "points3D.bin", "rigs.bin", "frames.bin",
                   "cameras.txt", "images.txt", "points3D.txt", "rigs.txt", "frames.txt",
                   "reconstruction_quality.json")
    # x64 size_t has at most twenty decimal digits. %010zu is a minimum,
    # not a maximum field width. Keep the upstream source reference explicit.
    model_index, snapshot_child = "9" * 20, "18446744073709551615"
    for root in (dataset / "distorted" / "sparse" / model_index,
                 paths["quality"] / "simple_radial" / "sparse" / model_index,
                 paths["quality"] / "baseline" / "sparse" / model_index,
                 paths["snapshots"] / snapshot_child,
                 paths["undistort"] / "sparse" / "0", dataset / "sparse" / "0",
                 dataset / ".alignment_cache" / ("sparse_tmp_" + nonce[:8]) / "0"):
        directories.add(root)
        files.update(root / name for name in model_names)
    files.update((dataset / "distorted/database.db", dataset / "distorted/database.db-journal",
                  dataset / "distorted/database.db-wal", dataset / "distorted/database.db-shm",
                  paths["quality"] / "simple_radial/database.db",
                  paths["preview"] / ("sparse_" + "9" * 20 + ".ply"),
                  dataset / ".alignment_cache/metadata.json"))
    for name in names:
        for root in (dataset / "input", dataset / "images", paths["sourceStage"],
                     paths["sourceStage"] / ".mask-work/images", paths["undistort"] / "images"):
            files.add(root / name)
            files.add((root / name).with_suffix(".png"))
    for name in ("depth_maps", "normal_maps", "consistency_graphs"):
        directories.add(paths["undistort"] / "stereo" / name)
    files.update((paths["undistort"] / "stereo/fusion.cfg",
                  paths["undistort"] / "stereo/patch-match.cfg"))
    for original, target in ((dataset / "source/masks", paths["sourceStage"] / ".mask-work/source/masks"),
                             (dataset / "images", paths["backup"] / "images"),
                             (dataset / "sparse", paths["backup"] / "sparse"),
                             (dataset / "stereo", paths["backup"] / "stereo")):
        if original.is_dir():
            for item in original.rglob("*"):
                if _is_reparse(item):
                    raise CameraDatasetError("camera_unsafe_solve_destination", str(item))
                planned = target / item.relative_to(original)
                (files if item.is_file() else directories).add(planned)
    try:
        require_legacy_windows_paths(files=files, directories=directories)
    except PathBudgetError as error:
        raise CameraDatasetError("camera_solve_path_budget_exceeded", str(error)) from error
    transient_roots = {paths[name] for name in ("undistort", "quality", "snapshots", "sourceStage", "backup")}
    def portable(path):
        return not any(path == root or root in path.parents for root in transient_roots)
    return {"schema": "gsw.colmap_path_layout/1", "datasetPath": str(dataset),
            "jobId": str(job.get("id") or ""), "nonce": nonce,
            "pathToken": path_token,
            "paths": {key: str(value) for key, value in paths.items()},
            "files": sorted(map(str, files)), "directories": sorted(map(str, directories)),
            "portableFiles": sorted(str(path) for path in files if portable(path)),
            "portableDirectories": sorted(str(path) for path in directories if portable(path)),
            "transientRoots": sorted(map(str, transient_roots)),
            "snapshotChildBound": {"digits": 20, "kind": "x64_size_t_decimal",
                "source": "https://github.com/colmap/colmap/blob/bd1fcf6/src/colmap/controllers/incremental_pipeline.cc#L102-L114"}}


def run_colmap_convert(job, dataset, options):
    dataset = Path(dataset)
    options = dict(options)
    prior_layout = job.get("colmapPathLayout") or {}
    planned_nonce = (prior_layout.get("nonce") if prior_layout.get("datasetPath") == str(dataset.resolve())
                     and not prior_layout.get("executionStartedAt") else None)
    layout = plan_colmap_path_layout(job, dataset, nonce=planned_nonce)
    if prior_layout.get("executionStartedAt"):
        job.setdefault("colmapPathHistory", []).append({key: prior_layout.get(key) for key in
            ("jobId", "datasetPath", "nonce", "pathToken", "paths", "executionStartedAt")})
    layout["executionStartedAt"] = time.time()
    job["colmapPathLayout"] = layout
    planned = {name: Path(path) for name, path in layout["paths"].items()}
    snapshot_root = planned["snapshots"]
    preview_publisher = None
    preserve_colmap_source_images(dataset, job, staging=planned["sourceStage"])
    if options.get("reset", True):
        database = dataset / "database.db"
        if database.exists():
            add_job_log(job, f"Removing stale COLMAP cache: {database}")
            database.unlink()
        # Only COLMAP's disposable work cache is cleared up front. Published
        # images/sparse/stereo stay usable until validated replacements exist.
        for child in ("distorted",):
            path = dataset / child
            if path.exists():
                add_job_log(job, f"Removing stale COLMAP cache: {path}")
                shutil.rmtree(path)
    (dataset / "distorted" / "sparse").mkdir(parents=True, exist_ok=True)
    if options.get("single_camera") and dataset_has_mixed_image_dimensions(dataset):
        options["single_camera"] = False
        add_job_log(job, "WARNING: Detected mixed image dimensions; disabling COLMAP single_camera to avoid CAMERA_SINGLE_DIM_ERROR.")
    add_job_log(job, f"COLMAP preset: {options['preset']}, matching: {options['matching']}, camera: {options['camera_model']}")
    undistort_staging = planned["undistort"]
    commands = colmap_convert_commands(dataset, options, undistort_staging)
    quality_work = planned["quality"]
    reports = []
    selected = None
    owned = ExitStack()
    try:
        for name in ("undistort", "quality", "snapshots"):
            owned.enter_context(owned_directory(planned[name]))
        for command in commands:
            is_mapper = len(command) > 1 and str(command[1]).lower() == "mapper"
            if len(command) > 1 and str(command[1]).lower() == "image_undistorter":
                total = reconstruction_quality.image_count(colmap_image_input_path(dataset))
                selected, reports = reconstruction_quality.select_model(dataset / "distorted" / "sparse", total)
                needs_repair = selected is None or not selected["preferred"]
                if needs_repair and options.get("quality_recovery", True):
                    repair_options = dict(options)
                    # Bounded, deterministic reinitialization for narrow-baseline
                    # video: first retain calibration, then optionally simplify it.
                    attempts = [("baseline", repair_options, False)]
                    if options.get("auto_camera_fallback") and options.get("single_camera") and options["camera_model"] == "OPENCV":
                        radial = dict(options, camera_model="SIMPLE_RADIAL")
                        attempts.append(("simple_radial", radial, True))
                    for name, retry_options, extract in attempts:
                        if job.get("cancel_requested"):
                            raise RuntimeError("COLMAP quality recovery cancelled")
                        root = quality_work / name
                        (root / "sparse").mkdir(parents=True)
                        retry_commands = colmap_convert_commands(dataset, retry_options, undistort_staging, root)
                        if not extract:
                            for retry in retry_commands[:3]:
                                retry[retry.index("--database_path") + 1] = dataset / "distorted" / "database.db"
                        baseline_report = selected or max(reports, key=lambda r: r["registeredImages"], default={"inputImages": total})
                        report_reconstruction_quality(job, baseline_report, "repairing")
                        add_job_log(job, "COLMAP quality recovery: {} camera={}, init_min_tri_angle=4, random_seed=1".format(name, retry_options["camera_model"]))
                        for retry in retry_commands[:3] if extract else retry_commands[2:3]:
                            if retry[1] == "mapper":
                                retry.extend(["--Mapper.init_min_tri_angle", "4", "--Mapper.random_seed", "1"])
                            run_logged(job, retry, ROOT, "3dgs")
                        candidate, attempt_reports = reconstruction_quality.select_model(root / "sparse", total)
                        reports.extend(attempt_reports)
                        if candidate and (selected is None or (candidate["registeredImages"], candidate["validPoints"]) > (selected["registeredImages"], selected["validPoints"])):
                            selected = candidate
                        if selected and selected["preferred"]:
                            break
                if selected is None:
                    rejected = max(reports, key=lambda r: (r["registeredImages"], r["validPoints"]), default={"inputImages": total, "reasons": ["no_model"]})
                    report_reconstruction_quality(job, rejected, "rejected")
                    raise reconstruction_quality.ReconstructionQualityError(rejected)
                report_reconstruction_quality(job, selected, "accepted" if selected["preferred"] else "partial")
                command[command.index("--input_path") + 1] = Path(selected["modelPath"])
            if not is_mapper:
                run_logged(job, command, ROOT, "3dgs")
                continue

            snapshot_root.mkdir(parents=True, exist_ok=True)
            snapshot_frequency = colmap_snapshot_frames_frequency(dataset)
            configure_colmap_mapper_snapshots(
                command, snapshot_root, snapshot_frequency
            )
            add_job_log(
                job,
                "COLMAP live sparse preview enabled: "
                f"snapshot every {snapshot_frequency} registered frame(s)",
            )
            preview_publisher = ColmapPointCloudPreviewPublisher(
                job, dataset, [snapshot_root], output_dir=planned["preview"]
            )
            preview_publisher.start()
            try:
                run_logged(job, command, ROOT, "3dgs")
            finally:
                preview_publisher.source_roots.append(
                    dataset / "distorted" / "sparse"
                )
                preview_publisher.stop()
        if commands:
            validate_undistorted_colmap_output(undistort_staging)
            validation = reconstruction_quality.assess_model(undistort_staging / "sparse" / "0", selected["inputImages"])
            if not validation["usable"]:
                report_reconstruction_quality(job, validation, "rejected")
                raise reconstruction_quality.ReconstructionQualityError(validation)
            (undistort_staging / "sparse" / "0" / "reconstruction_quality.json").write_text(json.dumps(dict(validation, candidates=reports), indent=2), encoding="utf-8")
            publish_undistorted_colmap_output(dataset, undistort_staging, backup=planned["backup"])
            if preview_publisher is not None:
                preview_publisher.source_roots.append(dataset / "sparse")
                preview_publisher.publish_latest(force=True)
                preview_publisher.prune()
        else:
            # Unit-test and repair callers may provide an already published model.
            normalize_undistorted_sparse(dataset)
    finally:
        owned.close()
    save_alignment_cache(dataset, job)


def train_args_for_quality(quality, backend="3dgs"):
    backend = safe_training_backend(backend)
    quality = quality or "quick"
    if quality == "original_quality":
        # Retain backend-specific highest-quality optimization. Explicit r=1
        # also bypasses the upstream loaders' automatic 1600-pixel fallback.
        options = train_args_for_quality("max_quality", backend)
        options["resolution"] = 1
        return options
    if backend == "2dgs":
        if quality == "quick":
            return {
                "iterations": 10000,
                "resolution": 2,
                "depth_ratio": 0.0,
                "densify_grad_threshold": 0.00025,
                "densification_interval": 100,
                "densify_until_iter": 5000,
            }
        if quality == "full":
            return {
                "iterations": 30000,
                "resolution": 8,
                "depth_ratio": 0.0,
                "densify_grad_threshold": 0.00025,
                "densification_interval": 120,
                "densify_until_iter": 9000,
            }
        if quality == "quality":
            return {
                "iterations": 30000,
                "resolution": 4,
                "depth_ratio": 0.0,
                "densify_grad_threshold": 0.0003,
                "densification_interval": 150,
                "densify_until_iter": 10000,
            }
        if quality == "max_quality":
            return {
                "iterations": 30000,
                "resolution": 2,
                "depth_ratio": 0.0,
                "densify_grad_threshold": 0.00035,
                "densification_interval": 200,
                "densify_until_iter": 10000,
            }
    else:
        if quality == "quick":
            return {
                # Match the shared preview fidelity target, not 2DGS's
                # surfel regularization. Keep a refinement phase after growth.
                "iterations": 10000,
                "resolution": 2,
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


def training_milestone_iterations(iterations):
    iterations = max(1, int(iterations))
    milestones = [iterations]
    for marker in (7000, 15000):
        if marker < iterations:
            milestones.append(marker)
    return sorted(set(milestones))


def write_training_metadata(output, backend, quality=None, options=None, summary=None):
    backend_path = managed_path(output, "training_backend.json")
    scene_path = managed_path(output, "scene.json") if options else None
    metadata = {
        "backend": backend,
        "created_at": time.time(),
        "source": "Gaussian Scene Workbench",
    }
    if quality:
        metadata["quality"] = quality
    output.mkdir(parents=True, exist_ok=True)
    with open(backend_path, "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2)
    if options:
        scene_metadata = {
            "name": output.name,
            "created_at": metadata["created_at"],
            "source": "Gaussian Scene Workbench",
            "training": {
                "backend": backend,
                "quality": quality,
                "options": options,
            },
        }
        normalized_summary = normalize_training_summary(summary)
        if normalized_summary is not None and normalized_summary["backend"] == backend:
            scene_metadata["training"]["summary"] = normalized_summary
        with open(scene_path, "w", encoding="utf-8") as f:
            json.dump(scene_metadata, f, indent=2)


def training_command(backend, dataset, output, options, start_checkpoint=None):
    backend = safe_training_backend(backend)
    iterations = options["iterations"]
    save_iterations = [str(value) for value in training_milestone_iterations(iterations)]
    if backend == "2dgs":
        command = [
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
            *save_iterations,
            "--save_iterations",
            *save_iterations,
            "--checkpoint_iterations",
            *save_iterations,
            "--depth_ratio",
            str(options["depth_ratio"]),
        ]
        command.extend(["--densify_grad_threshold", f"{options['densify_grad_threshold']:.12g}"])
        command.extend(["--densification_interval", str(options["densification_interval"])])
        command.extend(["--densify_until_iter", str(options["densify_until_iter"])])
        if start_checkpoint:
            command.extend(["--start_checkpoint", str(start_checkpoint)])
        return command

    recovery_iterations = sorted({
        iterations,
        *(marker for marker in (1000, 3000, 7000, 15000) if marker < iterations),
    })
    recovery_arguments = [str(value) for value in recovery_iterations]
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
        *recovery_arguments,
        "--checkpoint_iterations",
        *recovery_arguments,
        "--disable_viewer",
        "--enable_gpu_preview",
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
    if start_checkpoint:
        command.extend(["--start_checkpoint", str(start_checkpoint)])
    return command


def run_training_job(job, run_convert, quality, overwrite):
    try:
        if job.get("cancel_requested"):
            raise RuntimeError("Training cancelled by user")
        if (job.get("native_control") or {}).get("resume") and (overwrite or run_convert or job.get("resume_checkpoint")):
            raise ValueError("Native resume cannot overwrite output, rerun COLMAP or use a legacy checkpoint")
        backend = safe_training_backend(job.get("backend", "3dgs"))
        source_dataset = Path(job.get("dataset_path") or (DATASETS_DIR / job["scene"]))
        canonical_path = source_dataset / "camera_dataset.json"
        if job.get("pose_policy") is None and (job.get("camera_dataset_path") or canonical_path.is_file()):
            raise ValueError("camera_explicit_pose_policy_required")
        if job.get("pose_policy") == "solve" and not job.get("camera_dataset_path") and canonical_path.is_file():
            job["camera_dataset_path"] = str(canonical_path.resolve())
        if job.get("pose_policy") == "locked" and job.get("resume_checkpoint"):
            raise ValueError("camera_locked_legacy_checkpoint_forbidden")
        locked_admission = None
        camera_prepared = None
        if job.get("pose_policy") == "locked":
            from native.backend.camera_admission import admit_locked_dataset
            locked_admission = admit_locked_dataset(
                job["camera_dataset_path"], job.get("camera_dataset_identity"), run_convert)
            if Path(locked_admission["root"]).resolve() != Path(job["dataset_path"]).resolve():
                raise ValueError("camera_dataset_root_mismatch")
        elif job.get("pose_policy") == "solve" and job.get("camera_dataset_path"):
            from native.backend.camera_admission import admit_locked_dataset, prepare_solve_dataset_version, validate_solve_dataset_version
            if (job.get("native_control") or {}).get("resume"):
                admit_locked_dataset(job["camera_dataset_path"], job.get("camera_dataset_identity"))
                record_path = OUTPUT_DIR / safe_name(job["output_scene"]) / "training-source.json"
                record = json.loads(record_path.read_text(encoding="utf-8"))
                if record.get("posePolicy") != "solve" or record.get("cameraDatasetIdentity") != job.get("camera_dataset_identity"):
                    raise ValueError("camera_solve_resume_source_mismatch")
                saved_dataset = Path(record["datasetPath"])
                if not saved_dataset.is_absolute():
                    saved_dataset = record_path.parent / saved_dataset
                version = {"datasetPath": str(saved_dataset.resolve())}
                validate_solve_dataset_version(version["datasetPath"], record_path.parent,
                                               job.get("camera_dataset_identity"))
                run_convert = False
            else:
                version = prepare_solve_dataset_version(
                    job["camera_dataset_path"], TRAIN_JOBS_DIR / "camera-solve" / job["id"],
                    expected_identity=job.get("camera_dataset_identity"),
                    cancelled=lambda: bool(job.get("cancel_requested")))
                run_convert = True
            job["source_camera_dataset_path"] = job["camera_dataset_path"]
            job["source_dataset_path"] = job.get("dataset_path")
            job["dataset_path"] = version["datasetPath"]
            job["camera_dataset_identity"] = version.get("sourceDataIdentity", job.get("camera_dataset_identity"))
            job["cameraDataset"] = version.get("cameraDataset")
            # Admit both the actual solver layout and its final portable copy
            # before GPU/runtime probes, SfM or any output replacement.
            from native.backend.camera_admission import plan_solve_training_output
            candidate_output = OUTPUT_DIR / safe_name(job["output_scene"])
            requested_options = training_options_from_payload(backend, quality, job.get("train_options"))
            preview_component = ".p" + uuid.uuid4().hex
            job["solveTrainingPreviewPath"] = str(candidate_output / ".gsw/solve-dataset" / preview_component)
            preview_files = [Path(version["datasetPath"]) / preview_component / ("sparse_" + "9" * 20 + ".ply")]
            preview_directories = [Path(version["datasetPath"]) / preview_component]
            if not (job.get("native_control") or {}).get("resume"):
                job["colmapPathLayout"] = plan_colmap_path_layout(job, version["datasetPath"])
                future = job["colmapPathLayout"]
                job["solveOutputPathPlan"] = plan_solve_training_output(
                    version["datasetPath"], candidate_output,
                    future["portableFiles"] + preview_files, future["portableDirectories"] + preview_directories,
                    iterations=requested_options["iterations"])
            else:
                job["solveOutputPathPlan"] = plan_solve_training_output(
                    version["datasetPath"], candidate_output, preview_files, preview_directories,
                    iterations=requested_options["iterations"], resume=True)
        set_job_stage(job, "running", "environment")
        report = (ensure_training_environment(backend, require_colmap=False, camera_policy='locked') if locked_admission is not None
                  else ensure_training_environment(backend))
        add_job_log(job, f"Backend: {backend.upper()}")
        add_job_log(job, f"Python: {report['python']}")
        if backend == "2dgs":
            add_job_log(job, f"2DGS: {report['two_dgs_dir']}")
        add_job_log(job, f"COLMAP: {report['colmap']}")
        add_job_log(job, "OpenCV: OK")
        dataset = Path(job.get("dataset_path") or (DATASETS_DIR / job["scene"]))
        output_root = OUTPUT_DIR.resolve()
        output = OUTPUT_DIR / safe_name(job["output_scene"])
        if output.exists() and output.resolve().parent != output_root:
            raise ValueError(
                f"Training output resolves outside the configured output root: {output}"
            )
        images_dir = colmap_image_input_path(dataset)
        solve_publication = bool(job.get("source_camera_dataset_path") and
                                 not (job.get("native_control") or {}).get("resume"))
        if output.exists() and any(output.iterdir()):
            ensure_output_backend_compatible(job["output_scene"], backend)
            # A predictable conflict must fail before recalibrating the source
            # used by the preserved model. Actual overwrite deletion stays
            # deferred until reconstruction admission succeeds below.
            if not overwrite and not job.get("allow_existing_output"):
                raise ValueError(f"Output already exists: {output}. Enable overwrite or choose another output name.")
        if locked_admission is not None:
            from native.worker.canonical_training import prepare_training_dataset
            camera_options = training_options_from_payload(backend, quality, job.get("train_options"))
            resolution = 1 if job.get("resolution_policy") in (None, "preserve") else camera_options["resolution"]
            prepared_root = (output / ".gsw" / "camera-input" if
                             (job.get("native_control") or {}).get("resume") else
                             TRAIN_JOBS_DIR / "camera-input" / job["id"])
            if (job.get("native_control") or {}).get("resume") and not (prepared_root / "training-camera-dataset.json").is_file():
                raise ValueError("camera_resume_prepared_manifest_missing")
            camera_prepared = prepare_training_dataset(
                locked_admission["manifestPath"], prepared_root,
                {"resolution": resolution, "resolutionPolicy": job.get("resolution_policy") or "preserve"},
                cancelled=lambda: bool(job.get("cancel_requested")))
            job["cameraDataset"] = locked_admission["cameraDataset"]
            job["camera_prepared"] = camera_prepared
            job["camera_dataset_identity"] = locked_admission["dataset"]["data_identity"]
            set_job_stage(job, "running", "prepare")
            add_job_log(job, "Locked cameras admitted; COLMAP disabled; training views are isolated from validation/test views.")
        else:
            if not images_dir.exists() or not any(p.suffix.lower() in IMAGE_EXTS for p in images_dir.iterdir()):
                raise ValueError(f"No training images found: {images_dir}")
            if (job.get("native_control") or {}).get("resume"):
                if run_convert or overwrite or not dataset_has_recognized_training_scene(dataset):
                    raise ValueError("Native resume requires the original reconstruction and cannot overwrite output or rerun COLMAP")
                if images_dir.name == "input" and not (dataset / "images").exists():
                    raise ValueError("Native resume requires the original undistorted training images")
            if images_dir.name == "input" and not (dataset / "images").exists() and not run_convert:
                add_job_log(job, "Dataset contains input images but no undistorted images; enabling COLMAP conversion.")
                run_convert = True
            if dataset_has_colmap_scene(dataset):
                quality_report = reconstruction_quality.assess_dataset(dataset)
                if not quality_report["usable"]:
                    report_reconstruction_quality(job, quality_report, "repairing")
                    if (job.get("native_control") or {}).get("resume"):
                        raise reconstruction_quality.ReconstructionQualityError(quality_report)
                    run_convert = True

            set_job_stage(job, "running", "prepare")

            if run_convert:
                set_job_stage(job, "running", "colmap")
                run_colmap_convert(job, dataset, job.get("colmap_options") or colmap_options_from_payload({}))
            elif not dataset_has_recognized_training_scene(dataset):
                if not restore_alignment_cache(dataset, job):
                    add_job_log(job, "Existing dataset has no recognized sparse/0 or transforms_train.json; running COLMAP alignment first.")
                    set_job_stage(job, "running", "colmap")
                    run_colmap_convert(job, dataset, job.get("colmap_options") or colmap_options_from_payload({}))

            if dataset_has_colmap_scene(dataset):
                quality_report = reconstruction_quality.require_dataset(dataset)
                report_reconstruction_quality(job, quality_report, "accepted" if quality_report["preferred"] else "partial")
        # Do not discard an existing output until reconstruction admission passes.
        if job.get("cancel_requested"):
            raise RuntimeError("Training cancelled by user")
        if output.exists() and any(output.iterdir()) and not solve_publication:
            ensure_output_backend_compatible(job["output_scene"], backend)
            if overwrite:
                add_job_log(job, f"Removing existing output: {output}")
                shutil.rmtree(output)
            elif job.get("allow_existing_output"):
                add_job_log(job, f"Using existing output directory: {output}")
            else:
                raise ValueError(f"Output already exists: {output}. Enable overwrite or choose another output name.")

        options = training_options_from_payload(backend, quality, job.get("train_options"))
        options = resolve_training_options_for_environment(backend, options, job)
        if job.get("source_camera_dataset_path"):
            training_source = {
                "version": 1, "posePolicy": "solve", "datasetPath": ".gsw/solve-dataset",
                "cameraDatasetPath": job["source_camera_dataset_path"],
                "cameraDatasetIdentity": job.get("camera_dataset_identity"),
                "resolutionPolicy": job.get("resolution_policy") or "manual",
            }
            if not (job.get("native_control") or {}).get("resume"):
                from native.backend.camera_admission import publish_solve_training_output
                try:
                    publication = publish_solve_training_output(
                        dataset, output, job.get("camera_dataset_identity"), training_source,
                        overwrite=overwrite, allow_existing=bool(job.get("allow_existing_output")),
                        cancelled=lambda: bool(job.get("cancel_requested")))
                except BaseException as error:
                    recovery = getattr(error, "solve_output_transaction", None)
                    if recovery:
                        job["solveOutputPublication"] = recovery
                        add_job_log(job, "Solve output recovery journal: " + recovery["journalPath"])
                    raise
                job["solveOutputPublication"] = publication
                dataset = Path(publication["datasetPath"])
                job["dataset_path"] = str(dataset.resolve())
            else:
                atomic_write_json(output / "training-source.json", training_source)
        if camera_prepared is not None:
            if job.get("resolution_policy") in (None, "preserve"):
                options["resolution"] = 1
            destination = output / ".gsw" / "camera-input"
            destination.mkdir(parents=True, exist_ok=True)
            source_manifest = Path(camera_prepared["manifestPath"])
            if source_manifest.parent.resolve() != destination.resolve():
                for key in ("manifestPath", "contractPath", "initializationPath"):
                    source_file = Path(camera_prepared[key])
                    target = destination / source_file.name
                    if target.exists():
                        raise ValueError("camera_training_preparation_output_exists")
                    shutil.copy2(source_file, target)
                    camera_prepared[key] = str(target)
            job["camera_prepared"] = camera_prepared
            atomic_write_json(output / "training-source.json", {
                "version": 1, "posePolicy": "locked", "datasetPath": str(dataset.resolve()),
                "cameraDatasetPath": job["camera_dataset_path"],
                "cameraDatasetIdentity": job["camera_dataset_identity"],
                "resolutionPolicy": job.get("resolution_policy") or "preserve",
            })
        with TRAIN_LOCK:
            job["trainingSummary"] = configured_training_summary(backend, quality, options)
            job["effective_train_options"] = dict(options)
            job["iteration"] = int(job.get("resume_checkpoint_iteration") or 0)
            job["total_iterations"] = int(options["iterations"])
            job["progressPercent"] = max(
                0,
                min(100, round(job["iteration"] / max(job["total_iterations"], 1) * 100)),
            )
            persist_train_job(job)
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
        else:
            add_job_log(
                job,
                "2DGS quality guard: "
                f"depth_ratio={options['depth_ratio']}, "
                f"densify_until={options['densify_until_iter']}, "
                f"densify_interval={options['densification_interval']}, "
                f"densify_grad={options['densify_grad_threshold']:.12g}",
            )
            if options["resolution"] <= 2:
                control = job.get("native_control") or {}
                configured_guard = None
                if control.get("runtimeOptions") is not None or control.get("performance") is not None:
                    from native.worker.training_configuration import normalize_runtime_options
                    configured_guard = normalize_runtime_options(
                        control.get("runtimeOptions"), control.get("performance"),
                        resume=bool(control.get("resume")),
                        checkpoint_runtime=(job.get("runtimeConfiguration") or {}).get("requested"),
                    )["effective"]["resourceGuard"]
                if configured_guard is None:
                    guard_notice = "resource_guard=UNVERIFIED; actual guard policy must be reported by the trainer"
                else:
                    guard_notice = (
                        f"resource_guard={'ON' if configured_guard['enabled'] else 'OFF'}, "
                        f"mode={configured_guard['mode']}; "
                    )
                    if not configured_guard["enabled"]:
                        guard_notice += "software memory thresholds will not defer growth or pause"
                    elif configured_guard["mode"] == "pause":
                        guard_notice += (
                            "memory admission may defer growth and save complete state to "
                            f"pause after {configured_guard['max_consecutive_denials']} consecutive denials"
                        )
                    else:
                        guard_notice += "memory admission may defer growth without automatic resource pause"
                add_job_log(
                    job,
                    "WARNING: 2DGS full/half-resolution training may require substantial VRAM. "
                    f"Configured {guard_notice}. "
                    f"densify_until={options['densify_until_iter']}, "
                    f"densify_interval={options['densification_interval']}, "
                    f"densify_grad={options['densify_grad_threshold']:.12g}; "
                    f"scheduled checkpoint iterations={training_milestone_iterations(options['iterations'])}. "
                    "CUDA out-of-memory remains possible; no device-specific point cap or recoverability is guaranteed.",
                )
        start_checkpoint = job.get("resume_checkpoint")
        if start_checkpoint:
            add_job_log(job, f"Resuming from checkpoint: {start_checkpoint}")
        command = training_command(backend, dataset, output, options, start_checkpoint=start_checkpoint)
        # Existing reconstructions need the same first point-cloud view as a
        # newly run COLMAP job. This also covers starting training after restart.
        if locked_admission is None and job.get("preview_kind") != "colmap_sparse":
            sparse_preview = ColmapPointCloudPreviewPublisher(job, dataset, [dataset / "sparse"],
                output_dir=job.get("solveTrainingPreviewPath") if job.get("source_camera_dataset_path") else None)
            sparse_preview.publish_latest(force=True)
        # Keep sparse path/count correctly typed until a Gaussian frame exists;
        # clearing its kind/count early would mislabel the initialization view.
        set_job_stage(job, "running", "train")
        run_logged(job, command, TWO_DGS_DIR if backend == "2dgs" else GAUSSIAN_DIR, backend)
        result = training_point_cloud(job["output_scene"], options["iterations"])
        if result is None:
            raise RuntimeError(
                f"Training command completed but iteration {options['iterations']} produced no point_cloud.ply"
            )
        write_training_metadata(output, backend, quality, options, job.get("trainingSummary"))
        performance_record = {
            "version": 2, "backend": backend,
            "policy": (job.get("native_control") or {}).get("performance"),
            "runtimeConfiguration": job.get("runtimeConfiguration"),
            "requestedRuntimeOptions": (job.get("native_control") or {}).get("runtimeOptions"),
            "entryMigration": (job.get("native_control") or {}).get("runtimeConfigurationReport"),
            "runtimePolicyHistory": job.get("runtimePolicyHistory", []),
            "lastSample": normalize_performance(job.get("trainingPerformance")),
            "lastGrowth": normalize_growth(job.get("trainingGrowth")),
            "qualityMayBeConstrained": (job.get("trainingGrowth") or {}).get("state") == "deferred",
        }
        if job.get("native_control"):
            from native.worker.training_checkpoint import atomic_json
            output.mkdir(parents=True, exist_ok=True)
            atomic_json(output / "training-performance.json", performance_record)
        if job.get("reconstruction_quality"):
            (output / "reconstruction_quality.json").write_text(json.dumps(job["reconstruction_quality"], indent=2), encoding="utf-8")
        with TRAIN_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["iteration"] = int(result["iteration"])
            job["total_iterations"] = int(options["iterations"])
            job["progressPercent"] = 100
            job["latest_iteration"] = int(result["iteration"])
            job["preview_iteration"] = int(result["iteration"])
            job["preview_kind"] = "training_checkpoint"
            job["point_cloud_path"] = result["path"]
            job["point_count"] = int(result["vertex_count"])
            persist_train_job(job)
        add_job_log(job, f"{backend.upper()} training complete: {result['path']}")
    except NativeTrainingPaused as paused:
        manifest = paused.manifest
        with TRAIN_LOCK:
            job["status"] = "paused"
            job["stage"] = "paused"
            job["returncode"] = 75
            job["iteration"] = manifest["iteration"]
            job["total_iterations"] = manifest["total"]
            job["progressPercent"] = round(manifest["iteration"] * 100 / manifest["total"])
            job["updated_at"] = time.time()
            job["pause_reason"] = manifest.get("pause_reason", "manual_pause")
            if job["pause_reason"] != "manual_pause":
                job["generation_issue"] = "training_resource_pause"
            job["runtimeConfiguration"] = manifest.get("runtime_configuration", job.get("runtimeConfiguration"))
            job["runtimePolicyHistory"] = manifest.get("runtime_policy_history", job.get("runtimePolicyHistory", []))
            persist_train_job(job)
        add_job_log(job, str(paused))
    except Exception as exc:
        if isinstance(exc, reconstruction_quality.ReconstructionQualityError):
            report_reconstruction_quality(job, exc.report, "rejected")
            job["generation_issue"] = "reconstruction_quality"
        elif str(exc).startswith("source_frames_missing:"):
            job["generation_issue"] = "source_frames_missing"
        partial_result = None
        try:
            partial_result = training_point_cloud(job.get("output_scene"))
        except Exception:
            partial_result = None
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
            if partial_result is not None:
                job["latest_iteration"] = int(partial_result["iteration"])
                job["preview_iteration"] = int(partial_result["iteration"])
                job["preview_kind"] = "training_checkpoint"
                job["partial_point_cloud_path"] = partial_result["path"]
            persist_train_job(job)
        add_job_log(job, f"ERROR: {exc}")
        if partial_result is not None:
            add_job_log(
                job,
                "Latest usable checkpoint was preserved: "
                f"iteration {partial_result['iteration']} at {partial_result['path']}",
            )


def run_colmap_alignment_job(job):
    try:
        set_job_stage(job, "running", "colmap")
        dataset = Path(job.get("dataset_path") or (DATASETS_DIR / job["scene"])).resolve()
        images_dir = colmap_image_input_path(dataset)
        if not images_dir.exists() or not any(p.suffix.lower() in IMAGE_EXTS for p in images_dir.iterdir()):
            raise ValueError(f"No training images found: {images_dir}")
        options = job.get("options") or colmap_options_from_payload({})
        add_job_log(job, f"Dataset: {dataset}")
        add_job_log(job, f"Images: {images_dir}")
        run_colmap_convert(job, dataset, options)
        with MESH_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["output_dir"] = str(dataset)
            job["alignment"] = {
                "has_alignment": dataset_has_alignment_source(dataset),
                "cache_dir": str(alignment_cache_dir(dataset)),
            }
        add_job_log(job, f"COLMAP alignment complete: {dataset}")
    except Exception as exc:
        if isinstance(exc, reconstruction_quality.ReconstructionQualityError):
            report_reconstruction_quality(job, exc.report, "rejected")
            job["generation_issue"] = "reconstruction_quality"
        elif str(exc).startswith("source_frames_missing:"):
            job["generation_issue"] = "source_frames_missing"
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


def start_colmap_alignment(scene, options=None, dataset_path=None):
    scene = safe_name(scene)
    options = colmap_options_from_payload(options or {})
    dataset = Path(dataset_path).resolve() if dataset_path else DATASETS_DIR / scene
    job_id = uuid.uuid4().hex
    job = {
        "id": job_id,
        "kind": "colmap",
        "scene": scene,
        "dataset_path": str(dataset),
        "iteration": 0,
        "mode": "colmap",
        "options": options,
        "status": "queued",
        "stage": "queued",
        "created_at": time.time(),
        "updated_at": time.time(),
        "returncode": None,
        "error": None,
        "cancel_requested": False,
        "process": None,
        "output_dir": str(dataset),
        "output_mesh": None,
        "texture": None,
        "alignment": None,
        "latest_iteration": None,
        "preview_iteration": None,
        "preview_kind": None,
        "partial_point_cloud_path": None,
        "point_count": None,
        "log": [],
    }
    with MESH_LOCK:
        MESH_JOBS[job_id] = job
    thread = threading.Thread(target=run_colmap_alignment_job, args=(job,), daemon=True)
    thread.start()
    return mesh_job_snapshot(job)


def start_training(
    scene,
    output_scene,
    quality="quick",
    run_convert=True,
    overwrite=False,
    backend="3dgs",
    colmap_options=None,
    train_options=None,
    allow_existing_output=False,
    dataset_path=None,
    resume_checkpoint=None,
    resume_checkpoint_iteration=None,
    resume_from_scene=None,
    native_control=None,
    pose_policy=None,
    camera_dataset_path=None,
    camera_dataset_identity=None,
    resolution_policy=None,
    training_seed=0,
):
    from native.worker.training_seed import validate_training_seed
    training_seed = validate_training_seed(training_seed)
    if training_seed and not native_control:
        raise ValueError("training_seed_requires_native_checkpoint")
    if pose_policy not in (None, "locked", "solve"):
        raise ValueError("camera_pose_policy_invalid")
    source_dataset = Path(dataset_path) if dataset_path else DATASETS_DIR / safe_name(scene)
    canonical_path = source_dataset / "camera_dataset.json"
    if pose_policy is None and (camera_dataset_path or canonical_path.is_file()):
        raise ValueError("camera_explicit_pose_policy_required")
    if pose_policy == "solve" and not camera_dataset_path and canonical_path.is_file():
        camera_dataset_path = str(canonical_path.resolve())
    if pose_policy == "locked" and run_convert:
        raise ValueError("camera_locked_colmap_conflict")
    if (native_control or {}).get("resume") and (overwrite or run_convert or resume_checkpoint):
        raise ValueError("Native resume cannot overwrite output, rerun COLMAP or use a legacy checkpoint")
    if pose_policy == "locked" and resume_checkpoint:
        raise ValueError("camera_locked_legacy_checkpoint_forbidden")
    if pose_policy == "locked" and not camera_dataset_path:
        raise ValueError("camera_dataset_manifest_required")
    if resolution_policy not in (None, "preserve", "manual", "manual_scale"):
        raise ValueError("camera_resolution_policy_invalid")
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
        "dataset_path": str(dataset_path) if dataset_path else None,
        "resume_checkpoint": str(resume_checkpoint) if resume_checkpoint else None,
        "resume_checkpoint_iteration": int(resume_checkpoint_iteration) if resume_checkpoint_iteration is not None else None,
        "resume_from_scene": safe_name(resume_from_scene) if resume_from_scene else None,
        "native_control": native_control,
        "pose_policy": pose_policy,
        "camera_dataset_path": str(camera_dataset_path) if camera_dataset_path else None,
        "camera_dataset_identity": camera_dataset_identity,
        "resolution_policy": resolution_policy,
        "training_seed": training_seed,
        "process": None,
        "colmap_options": colmap_options_from_payload(colmap_options or {}),
        "train_options": train_options if isinstance(train_options, dict) else {},
        "iteration": 0,
        "total_iterations": None,
        "progressPercent": 0,
        "loss": None,
        "psnr": None,
        "gaussian_count": None,
        "iteration_milliseconds": None,
        "elapsed_seconds": None,
        "latest_iteration": None,
        "preview_iteration": None,
        "preview_kind": None,
        "point_cloud_path": None,
        "partial_point_cloud_path": None,
        "point_count": None,
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
        process = job.get("process")
        if process is None or process.poll() is not None:
            job["status"] = "cancelled"
            job["stage"] = "cancelled"
            job["error"] = job.get("error") or "Training cancelled by user"
        else:
            job["status"] = "cancelling"
        job["updated_at"] = time.time()
        persist_train_job(job)
    terminate_process(process)
    add_job_log(job, "Cancel requested.")
    return job_snapshot(job)


def mesh_job_snapshot(job):
    return {
        "id": job["id"],
        "kind": job.get("kind", "mesh"),
        "scene": job["scene"],
        "output_scene": job.get("output_scene"),
        "iteration": job["iteration"],
        "mode": job["mode"],
        "status": job["status"],
        "stage": job["stage"],
        "created_at": job["created_at"],
        "updated_at": job["updated_at"],
        "returncode": job.get("returncode"),
        "error": job.get("error"),
        "dataset_path": job.get("dataset_path"),
        "output_mesh": job.get("output_mesh"),
        "options": job.get("options"),
        "effective_options": job.get("effective_options"),
        "scale_report": job.get("scale_report"),
        "task_port": job.get("task_port"),
        "task_port_released": bool(job.get("task_port_released")),
        "texture": job.get("texture"),
        "output_dir": job.get("output_dir"),
        "alignment": job.get("alignment"),
        "latest_iteration": job.get("latest_iteration"),
        "preview_iteration": job.get("preview_iteration"),
        "preview_kind": job.get("preview_kind"),
        "partial_point_cloud_path": job.get("partial_point_cloud_path"),
        "point_count": job.get("point_count"),
        "copied_files": job.get("copied_files"),
        "total_files": job.get("total_files"),
        "copied_bytes": job.get("copied_bytes"),
        "total_bytes": job.get("total_bytes"),
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
    return mesh_job_snapshot(job)


def run_mesh_export_job(job):
    try:
        if job.get("cancel_requested"):
            raise RuntimeError("Mesh export cancelled by user")
        set_job_stage(job, "running", "environment")
        options = dict(job.get("options", {}))
        if options.get("mode") == "sugar":
            ensure_sugar_environment()
        elif options.get("mode") == "gs2mesh":
            ensure_gs2mesh_environment()
            task_port = reserve_task_port(job, "GS2Mesh Gaussian renderer")
            options["task_port"] = task_port
            job["options"] = options
            add_job_log(job, f"Task port: {task_port} (GS2Mesh Gaussian renderer)")
        else:
            ensure_mesh_environment()
        command = None
        cwd = None
        output_path = mesh_output_path(job["scene"], job["iteration"], options.get("mode"), post=True)
        gs2mesh_result = None
        effective_options = options
        if options.get("mode") != "gs2mesh":
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
        if run_backend == "2dgs":
            effective_options = mesh_export_options(options)
            command_text = [str(part) for part in command]
            for arg_name in ("voxel_size", "depth_trunc", "sdf_trunc"):
                flag = f"--{arg_name}"
                if flag in command_text:
                    effective_options[arg_name] = float(command_text[command_text.index(flag) + 1])
            with MESH_LOCK:
                job["effective_options"] = dict(effective_options)
            if (
                effective_options["mode"] == "bounded"
                and float(options.get("voxel_size", -1.0)) <= 0
                and effective_options["voxel_size"] > 0
            ):
                gaussian_extent = effective_options["voxel_size"] * effective_options["mesh_res"]
                add_job_log(
                    job,
                    "2DGS automatic TSDF sizing: "
                    f"Gaussian extent {gaussian_extent:.6g}, mesh resolution {effective_options['mesh_res']}, "
                    f"voxel {effective_options['voxel_size']:.6g}, SDF truncation {effective_options['sdf_trunc']:.6g}. "
                    "Source camera poses and model coordinates are unchanged.",
                )
        add_job_log(job, f"Backend: {backend_label}")
        add_job_log(job, f"Scene: output/{job['scene']}")
        add_job_log(job, f"Mode: {job['mode']}")
        add_job_log(job, f"Output mesh: {output_path}")
        set_job_stage(job, "running", "mesh")
        started_at = time.time()
        if job.get("cancel_requested"):
            raise RuntimeError("Mesh export cancelled by user")
        if options.get("mode") == "gs2mesh":
            output_path, effective_options, gs2mesh_result = run_gs2mesh_mesh_job(job, options)
        else:
            try:
                run_logged(job, command, cwd, run_backend)
            except Exception:
                if (
                    run_backend != "2dgs"
                    or job.get("cancel_requested")
                    or not recover_2dgs_post_mesh(
                        job,
                        job["scene"],
                        job["iteration"],
                        options.get("mode", "bounded"),
                        options.get("num_cluster", 50),
                        started_at,
                    )
                ):
                    raise
        if run_backend == "2dgs" and not output_path.exists():
            recover_2dgs_post_mesh(
                job,
                job["scene"],
                job["iteration"],
                options.get("mode", "bounded"),
                options.get("num_cluster", 50),
                started_at,
            )
        texture_result = None
        if options.get("mode") == "sugar":
            cfg = load_cfg_args(model_dir(job["scene"]))
            source_path = Path(getattr(cfg, "source_path", "") or DATASETS_DIR / job["scene"])
            texture_result = collect_sugar_mesh_outputs(job["scene"], job["iteration"], source_path, started_at)
            if texture_result.get("glb_error"):
                add_job_log(job, f"WARNING: GLB export skipped: {texture_result['glb_error']}")
        elif options.get("mode") == "gs2mesh":
            add_job_log(
                job,
                f"GS2Mesh {'cleaned' if gs2mesh_result.get('cleaned', True) else 'raw fallback'} mesh: "
                f"{gs2mesh_result['source']}",
            )
            if gs2mesh_result.get("warning"):
                add_job_log(job, f"WARNING: {gs2mesh_result['warning']}")
            add_job_log(
                job,
                f"GS2Mesh geometry: {gs2mesh_result['vertices']:,} vertices, "
                f"{gs2mesh_result['faces']:,} faces",
            )
        if not output_path.exists():
            raise RuntimeError(f"Mesh export finished but output file was not found: {output_path}")
        with MESH_LOCK:
            job["status"] = "done"
            job["stage"] = "done"
            job["returncode"] = 0
            job["updated_at"] = time.time()
            job["output_mesh"] = str(output_path)
            if texture_result:
                job["texture"] = texture_result
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
    finally:
        release_task_port(job)


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
        "texture": None,
        "log": [],
    }
    with MESH_LOCK:
        MESH_JOBS[job_id] = job
    thread = threading.Thread(target=run_mesh_export_job, args=(job,), daemon=True)
    thread.start()
    return mesh_job_snapshot(job)


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
        if options.get("backend") in {"openmvs", "colmap"} and dataset_has_colmap_scene(dataset_path):
            quality_report = reconstruction_quality.require_dataset(dataset_path)
            report_reconstruction_quality(job, quality_report, "accepted" if quality_report["preferred"] else "partial")
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
        "texture": None,
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
        raise RuntimeError(f"OpenCV is not available in the worker Python: {exc}")

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
    for root in conda_root_candidates():
        for candidate in (root / "Scripts" / "conda.exe", root / "condabin" / "conda.bat"):
            if candidate.exists():
                return candidate
    found = shutil.which("conda")
    return Path(found) if found else None


def ffmpeg_executable():
    candidates = [
        os.environ.get("FFMPEG_PATH"),
        shutil.which("ffmpeg"),
        gaussian_env_root() / "Library" / "bin" / "ffmpeg.exe",
        *[root / "Library" / "bin" / "ffmpeg.exe" for root in conda_root_candidates()],
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
    dataset = managed_path(DATASETS_DIR, safe_name(scene))
    images_dir = managed_path(dataset, "images")
    source_dir = managed_path(dataset, "source")
    originals_dir = managed_path(source_dir, "originals")
    if dataset.exists() and overwrite:
        remove_tree_inside(DATASETS_DIR, dataset)
    images_dir.mkdir(parents=True, exist_ok=True)
    originals_dir.mkdir(parents=True, exist_ok=True)
    return dataset, images_dir, source_dir, originals_dir


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
                records[index]["extractionFps"] = float(fps)
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
                    records[index]["extractionFps"] = float(fps)
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


def clear_texture_outputs(output_paths):
    for key in ("obj", "mtl", "png", "zip", "glb"):
        try:
            Path(output_paths[key]).unlink()
        except FileNotFoundError:
            pass


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
            if job.get("status") in {"queued", "running", "cancelling"}:
                job["cancel_requested"] = True
                job["status"] = "cancelled"
                job["stage"] = "cancelled"
                job["error"] = job.get("error") or "Worker restarted before this training job finished."
                job["updated_at"] = time.time()
                persist_train_job(job)
            jobs[job["id"]] = job
        except Exception:
            continue
    return jobs


def save_path_dataset(data):
    """Import sources explicitly selected by the owned native worker."""
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


