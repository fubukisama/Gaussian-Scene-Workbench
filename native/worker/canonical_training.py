"""Canonical camera-dataset to native 3DGS/2DGS training adapter.

Camera calibration is immutable; numerical localization is a separately
recorded positive similarity and never an SfM or camera-optimization step.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
from pathlib import Path
import uuid

import numpy as np


def _json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode("utf-8")).hexdigest()


def _reject_reparse(path):
    path = Path(path)
    if path.exists() and (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
        raise ValueError("camera preparation output cannot be a link or reparse point")


def transform_points(transform, points):
    """Apply a recorded similarity in float64, without re-normalizing input."""
    points = np.asarray(points, dtype=np.float64)
    if points.size == 0:
        return points.reshape(-1, 3)
    return (float(transform["s"]) * (points @ np.asarray(transform["R"], dtype=np.float64).T)
            + np.asarray(transform["t"], dtype=np.float64))


def training_normalization(views, points=None, bounds=None, explicit=None):
    """Build reversible local coordinates from actual camera/initial bounds.

    Camera coverage is a numerical extent, not evidence of object depth or
    metric accuracy. Random initialization records that assumption separately.
    """
    if explicit is not None:
        result = copy.deepcopy(explicit)
        scale = float(result["s"])
        rotation = np.asarray(result["R"], dtype=np.float64)
        translation = np.asarray(result["t"], dtype=np.float64)
        if not (scale > 0 and np.isfinite(scale) and np.isfinite(rotation).all()
                and np.isfinite(translation).all()
                and np.allclose(rotation.T @ rotation, np.eye(3), atol=2e-5)
                and np.linalg.det(rotation) > 0):
            raise ValueError("invalid explicit training similarity")
    else:
        centers = np.asarray([[view["c2w_opencv"][i][3] for i in range(3)] for view in views],
                             dtype=np.float64)
        if not len(centers) or not np.isfinite(centers).all():
            raise ValueError("camera coverage is unavailable for initialization")
        sample = centers
        if points is not None and np.asarray(points).size:
            sample = np.concatenate((sample, np.asarray(points, dtype=np.float64)), axis=0)
        if bounds is not None:
            sample = np.concatenate((sample, np.asarray(bounds, dtype=np.float64)), axis=0)
        center = (sample.min(axis=0) + sample.max(axis=0)) * .5
        radius = float(np.linalg.norm(sample - center, axis=1).max())
        if not np.isfinite(radius) or radius <= np.finfo(np.float64).eps * max(1., float(np.abs(center).max())):
            raise ValueError("zero camera coverage; declare initialization bounds or provide geometry")
        scale, rotation, translation = 1. / radius, np.eye(3), -center / radius
        result = {"version": 1, "input_frame": "canonical", "output_frame": "training",
                  "s": scale, "R": rotation.tolist(), "t": translation.tolist(),
                  "applied": False, "mode": "camera_and_initialization_extent",
                  "evidence": [{"kind": "numerical_localization", "canonical_center": center.tolist(),
                                "canonical_extent": radius, "not_metric_evidence": True}]}
    inverse_rotation = rotation.T
    result["inverse"] = {"s": 1. / scale, "R": inverse_rotation.tolist(),
                         "t": (-(inverse_rotation @ translation) / scale).tolist()}
    result.setdefault("applied", False)
    return result


def resized_intrinsics(intrinsics, source_size, output_size):
    """Scale pixel-border K consistently with the actual image resize."""
    width, height = source_size
    out_width, out_height = output_size
    if min(width, height, out_width, out_height) <= 0:
        raise ValueError("image dimensions must be positive")
    k = np.asarray(intrinsics, dtype=np.float64).copy()
    k[0] *= out_width / width
    k[1] *= out_height / height
    return k.tolist()


def pinhole_projection(intrinsics, width, height, znear, zfar):
    """Full pixel-border K for the Graphdeco and surfel ndc2Pix convention."""
    k = np.asarray(intrinsics, dtype=np.float64)
    if (k.shape != (3, 3) or not np.isfinite(k).all() or min(width, height) <= 0
            or k[0, 0] <= 0 or k[1, 1] <= 0 or abs(k[0, 1]) > 1e-12
            or not 0 < znear < zfar):
        raise ValueError("unsupported pinhole projection")
    projection = np.zeros((4, 4), dtype=np.float64)
    projection[0, 0], projection[1, 1] = 2. * k[0, 0] / width, 2. * k[1, 1] / height
    projection[0, 2], projection[1, 2] = 2. * k[0, 2] / width - 1., 2. * k[1, 2] / height - 1.
    projection[3, 2] = 1.
    projection[2, 2] = zfar / (zfar - znear)
    projection[2, 3] = -zfar * znear / (zfar - znear)
    return projection.tolist()


def camera_dataset_source(source, explicit=None):
    """Select an explicit canonical input before inspecting incidental sparse."""
    if os.environ.get("GSW_POSE_POLICY") == "solve":
        # An explicit new SfM strategy is a separate version. Its freshly
        # solved reconstruction is not overridden by an older nearby manifest.
        return None
    root = Path(source).resolve()
    selected = explicit or os.environ.get("GSW_CAMERA_DATASET")
    if selected:
        path = Path(selected)
        if not path.is_absolute():
            path = root / path
        if not path.is_file():
            raise ValueError("explicit camera dataset manifest is missing: " + str(path))
        return path.resolve()
    manifest = root / "camera_dataset.json"
    return manifest if manifest.is_file() else None


def _training_size(width, height, resolution):
    if resolution in (-1, 1, None):
        return [width, height]
    if resolution in (2, 4, 8):
        return [max(1, round(width / resolution)), max(1, round(height / resolution))]
    if not isinstance(resolution, (float, int)) or resolution <= 0:
        raise ValueError("invalid explicit camera training resolution")
    return [max(1, round(resolution)), max(1, round(height * resolution / width))]


def prepare_training_dataset(source, out_root=None, config=None, cancelled=None, *, resolution=-1, explicit=None):
    """Validate bytes, split and full K, then localize exactly once in float64.

    This public CPU boundary produces the same immutable camera contract used
    by both trainers. It never executes COLMAP or changes source files.
    """
    from native.backend import camera_dataset
    source = Path(source).resolve()
    manifest = source if source.is_file() else camera_dataset_source(source, explicit)
    if manifest is None:
        raise ValueError("canonical camera dataset manifest is unavailable")
    root = manifest.parent
    config = config or {}
    if "resolution" in config:
        resolution = config["resolution"]
    if config.get("resolutionPolicy") == "preserve":
        resolution = 1
    dataset = camera_dataset.load_camera_dataset(manifest, cancelled=cancelled)
    canonical_dataset = copy.deepcopy(dataset)
    if out_root is not None:
        _reject_reparse(out_root)
        existing = Path(out_root).resolve() / "training-camera-dataset.json"
        _reject_reparse(existing)
        if existing.is_file():
            reused = load_prepared_training_dataset(existing, root)
            sizes = {view["view_id"]: _training_size(view["width"], view["height"], resolution)
                     for view in canonical_dataset["views"]}
            if (reused["input_data_identity"] != dataset["data_identity"] or
                    any(view["training_size"] != sizes[view["view_id"]] for view in reused["views"])):
                raise ValueError("camera preparation changed; create a new dataset/task version")
            with existing.open(encoding="utf-8") as stream:
                payload = json.load(stream)
            return {"manifestPath": str(existing), "imageRoot": str(root),
                    "contractPath": str(existing.with_name("training-camera-contract.json")),
                    "contract": payload["contract"], "dataset": payload["contract"],
                    "initializationPath": str(existing.parent / payload["initialization_file"])}
    dataset = copy.deepcopy(dataset)
    input_identity = dataset["data_identity"]
    initialization = dataset["initialization"]
    points = colors = None
    if initialization["kind"] != "random":
        points, colors = camera_dataset.read_initialization_points(dataset, root)
        points, colors = np.asarray(points, dtype=np.float64), np.asarray(colors, dtype=np.float64)
        if not len(points):
            raise ValueError("declared initialization point cloud is empty")
    bounds = initialization.get("bounds")
    if bounds is not None:
        bounds = np.asarray(bounds, dtype=np.float64)
        if bounds.shape != (2, 3) or not np.isfinite(bounds).all() or np.any(bounds[1] <= bounds[0]):
            raise ValueError("initialization bounds must contain finite increasing minima and maxima")
        if initialization.get("frame", "source") == "source":
            corners = np.asarray([[bounds[x, 0], bounds[y, 1], bounds[z, 2]]
                                  for x in (0, 1) for y in (0, 1) for z in (0, 1)])
            transformed = transform_points(dataset["transforms"]["source_to_canonical"], corners)
            bounds = np.asarray([transformed.min(axis=0), transformed.max(axis=0)])
    configured = dataset["transforms"]["canonical_to_training"]
    explicit_transform = configured if configured.get("mode") == "explicit" else None
    training_views = [view for view in dataset["views"] if view["split"] == "train"]
    normalization = training_normalization(training_views, points=points, bounds=bounds,
                                            explicit=explicit_transform)
    # A normalized manifest stores canonical camera coordinates, never modified
    # training poses. Re-loading therefore cannot multiply this chain twice.
    normalization["applied"] = True
    dataset["transforms"]["canonical_to_training"] = normalization
    if points is None:
        seed = initialization.get("seed")
        count = initialization.get("count")
        seed, count = 0 if seed is None else seed, 10000 if count is None else count
        if type(seed) is not int or not 0 <= seed <= 2**32 - 1 or type(count) is not int or not 4 <= count <= 10_000_000:
            raise ValueError("random initialization requires an integral seed and 4..10000000 points")
        if bounds is None:
            centers = np.asarray([[view["c2w_opencv"][i][3] for i in range(3)]
                                  for view in training_views], dtype=np.float64)
            middle = (centers.min(axis=0) + centers.max(axis=0)) * .5
            radius = float(np.linalg.norm(centers - middle, axis=1).max())
            if radius <= 0:
                raise ValueError("random initialization needs camera coverage or explicit bounds")
            bounds = np.asarray([middle - radius * .5, middle + radius * .5])
            initialization["coverage_assumption"] = "scene_near_camera_coverage_center_not_depth_evidence"
        points = np.random.RandomState(seed).uniform(bounds[0], bounds[1], size=(count, 3))
        colors = np.full((count, 3), .5, dtype=np.float64)
        initialization.update(seed=seed, count=count, actual_canonical_bounds=bounds.tolist())
    localized = transform_points(normalization, points)
    rotation = np.asarray(normalization["R"], dtype=np.float64)
    for view in dataset["views"]:
        pose = np.asarray(view["c2w_opencv"], dtype=np.float64)
        pose[:3, 3] = transform_points(normalization, [pose[:3, 3]])[0]
        pose[:3, :3] = rotation @ pose[:3, :3]
        view["training_c2w_opencv"] = pose.tolist()
        view["training_size"] = _training_size(view["width"], view["height"], resolution)
        view["training_K"] = resized_intrinsics(view["K"], (view["width"], view["height"]), view["training_size"])
    coverage = []
    maximum_depth = .2
    for view in dataset["views"]:
        pose = np.asarray(view["training_c2w_opencv"], dtype=np.float64)
        local = (localized - pose[:3, 3]) @ pose[:3, :3]
        if view["split"] != "train":
            continue
        maximum_depth = max(maximum_depth, float(local[:, 2].max()))
        positive = local[:, 2] > .2
        if not positive.any():
            raise camera_dataset.CameraDatasetError("camera_no_forward_initialization", view["view_id"])
        k = np.asarray(view["training_K"], dtype=np.float64)
        image_xy = local[positive, :2] / local[positive, 2:3]
        image_xy *= [k[0, 0], k[1, 1]]
        image_xy += [k[0, 2], k[1, 2]]
        size = view["training_size"]
        visible = (image_xy[:, 0] >= 0) & (image_xy[:, 0] < size[0]) & (image_xy[:, 1] >= 0) & (image_xy[:, 1] < size[1])
        if not visible.any():
            raise camera_dataset.CameraDatasetError("camera_no_visible_initialization", view["view_id"])
        coverage.append({"view_id": view["view_id"], "initial_points": len(local),
                         "forward_past_backend_near": int(positive.sum()), "inside_image": int(visible.sum()),
                         "minimum_initial_view_z": float(local[:, 2].min()),
                         "maximum_initial_view_z": float(local[:, 2].max())})
    dataset["clipping"] = {"backend_near": .2, "znear": .2, "zfar": maximum_depth * 2.,
                           "frame": "training", "mode": "stock_fixed_positive_depth_clip",
                           "limitation": "geometry_with_view_z_at_or_below_backend_near_is_culled",
                           "initialization_coverage": coverage}
    dataset["resolution_policy"] = "preserve" if resolution in (-1, 1, None) else "manual_scale"
    dataset["input_data_identity"] = input_identity
    dataset["initial_points"], dataset["initial_colors"] = localized, np.asarray(colors, dtype=np.float64)
    dataset["root"] = str(root)
    if out_root is not None:
        _reject_reparse(out_root)
        destination = Path(out_root).resolve()
        destination.mkdir(parents=True, exist_ok=True)
        if cancelled and cancelled():
            raise ValueError("camera_import_cancelled")
        initial_path = destination / "training-initialization.npz"
        _reject_reparse(initial_path)
        temporary = destination / (".training-initialization-" + uuid.uuid4().hex + ".npz")
        try:
            np.savez(temporary, points=localized, colors=dataset["initial_colors"])
            os.replace(str(temporary), str(initial_path))
        finally:
            if temporary.exists():
                temporary.unlink()
        initial_digest = hashlib.sha256(initial_path.read_bytes()).hexdigest()
        contract = training_contract(dataset)
        contract["training_initialization_sha256"] = initial_digest
        prepared_path = destination / "training-camera-dataset.json"
        payload = {"schema": "gsw.prepared_camera_dataset/1", "image_reference": "managed-dataset-root",
                   "canonical_dataset": canonical_dataset, "contract": contract,
                   "contract_sha256": _json_digest(contract),
                   "initialization_file": initial_path.name, "initialization_sha256": initial_digest}
        camera_dataset.write_camera_dataset(payload, prepared_path)
        contract_path = destination / "training-camera-contract.json"
        camera_dataset.write_camera_dataset(contract, contract_path)
        return {"manifestPath": str(prepared_path), "imageRoot": str(root),
                "contractPath": str(contract_path), "contract": contract,
                "dataset": contract, "initializationPath": str(initial_path)}
    return dataset


def training_contract(dataset):
    """JSON-only camera/scale contract; no machine-specific absolute paths."""
    return {"schema": "gsw.training_camera_contract/1", "input_data_identity": dataset["input_data_identity"],
            "pose_policy": dataset["pose_policy"], "source": copy.deepcopy(dataset["source"]),
            "scale": copy.deepcopy(dataset["scale"]), "transforms": copy.deepcopy(dataset["transforms"]),
            "splits": copy.deepcopy(dataset["splits"]), "views": copy.deepcopy(dataset["views"]),
            "initialization": copy.deepcopy(dataset["initialization"]),
            "resolution_policy": dataset["resolution_policy"],
            "independent_test": dataset.get("independent_test", False),
            "warnings": copy.deepcopy(dataset.get("warnings", [])),
            "clipping": copy.deepcopy(dataset["clipping"]),
            "kernel_depth_contract": {"minimum_positive_view_z": .2,
                                      "mode": "stock_graphdeco_surfel_fixed_clip"}}


def load_prepared_training_dataset(manifest, image_root):
    """Revalidate current managed image bytes, then consume saved transforms.

    This path does not estimate normalization again, including on full resume.
    """
    from native.backend import camera_dataset
    path = Path(manifest).resolve()
    with path.open(encoding="utf-8") as stream:
        prepared = json.load(stream)
    if prepared.get("schema") != "gsw.prepared_camera_dataset/1" or prepared.get("image_reference") != "managed-dataset-root":
        raise ValueError("invalid prepared camera dataset schema")
    canonical = camera_dataset.validate_camera_dataset(prepared["canonical_dataset"], image_root)
    contract = prepared["contract"]
    if _json_digest(contract) != prepared.get("contract_sha256"):
        raise ValueError("prepared camera contract checksum mismatch")
    if contract.get("schema") != "gsw.training_camera_contract/1" or contract.get("input_data_identity") != canonical["data_identity"]:
        raise ValueError("prepared camera dataset identity changed")
    if contract.get("splits") != canonical["splits"] or contract.get("pose_policy") != canonical["pose_policy"]:
        raise ValueError("prepared camera dataset strategy or split changed")
    if any(contract.get(key) != canonical.get(key) for key in ("scale", "source", "warnings", "independent_test")):
        raise ValueError("prepared camera source or scale provenance changed")
    if contract["transforms"]["source_to_canonical"] != canonical["transforms"]["source_to_canonical"]:
        raise ValueError("prepared camera source coordinate transform changed")
    if contract.get("training_initialization_sha256") != prepared.get("initialization_sha256"):
        raise ValueError("prepared training initialization identity changed")
    initialization = path.parent / prepared["initialization_file"]
    if initialization.resolve().parent != path.parent or hashlib.sha256(initialization.read_bytes()).hexdigest() != prepared["initialization_sha256"]:
        raise ValueError("prepared training initialization checksum mismatch")
    with np.load(initialization, allow_pickle=False) as values:
        points, colors = values["points"].copy(), values["colors"].copy()
    if (points.ndim != 2 or points.shape[1] != 3 or colors.shape != points.shape
            or not np.isfinite(points).all() or not np.isfinite(colors).all()):
        raise ValueError("invalid prepared training initialization")
    current_views = {view["view_id"]: view for view in canonical["views"]}
    normalization = contract["transforms"]["canonical_to_training"]
    camera_dataset.invert_sim3(normalization)  # Validate positive scale, proper R and supplied inverse.
    if normalization.get("input_frame") != "canonical" or normalization.get("output_frame") != "training":
        raise ValueError("prepared training normalization has inconsistent frames")
    if not normalization.get("applied"):
        raise ValueError("prepared training normalization was not applied")
    rotation = np.asarray(normalization["R"], dtype=np.float64)
    # Reject tampering or stale derived poses/K instead of deriving a new frame.
    for view in contract["views"]:
        source_view = current_views.pop(view["view_id"], None)
        if source_view is None or any(view.get(key) != source_view.get(key)
                                      for key in ("image_path", "sha256", "K", "c2w_opencv", "split")):
            raise ValueError("prepared camera source calibration changed")
        expected = np.asarray(source_view["c2w_opencv"], dtype=np.float64)
        expected[:3, 3] = transform_points(normalization, [expected[:3, 3]])[0]
        expected[:3, :3] = rotation @ expected[:3, :3]
        if not np.allclose(view["training_c2w_opencv"], expected, rtol=1e-12, atol=1e-12):
            raise ValueError("prepared camera training transform changed")
        if view["training_K"] != resized_intrinsics(source_view["K"],
                [source_view["width"], source_view["height"]], view["training_size"]):
            raise ValueError("prepared camera training intrinsics changed")
    if current_views:
        raise ValueError("prepared camera view membership changed")
    result = copy.deepcopy(contract)
    result.update(initial_points=points, initial_colors=colors, root=str(Path(image_root).resolve()))
    return result


def composite_training_image(image, white_background=False):
    """RGBA composition with unsigned bytes, valid on Pillow 10 and 12."""
    from PIL import Image
    if image.mode not in ("RGB", "RGBA", "L", "LA", "P"):
        raise ValueError("canonical training currently supports only verified eight-bit image modes")
    if image.mode in ("RGBA", "LA") or "transparency" in image.info:
        rgba = np.asarray(image.convert("RGBA"), dtype=np.uint8)
        alpha = rgba[:, :, 3:4].astype(np.float64) / 255.
        background = 255. if white_background else 0.
        rgb = rgba[:, :, :3].astype(np.float64) * alpha + background * (1. - alpha)
        return Image.fromarray(np.clip(rgb, 0., 255.).astype(np.uint8), "RGB")
    return image.convert("RGB")


def assert_camera_optimizer_locked(scene, gaussians):
    """Fail before any optimizer step if fixed calibration became trainable."""
    contract = getattr(gaussians, "_gsw_camera_contract", None)
    if contract is None:
        return
    parameters = {id(parameter) for optimizer in (getattr(gaussians, "optimizer", None),
                                                  getattr(gaussians, "exposure_optimizer", None))
                  if optimizer is not None for group in optimizer.param_groups for parameter in group["params"]}
    for cameras in (scene.getTrainCameras(), scene.getValCameras(), scene.getTestCameras()):
        for camera in cameras:
            for attribute in ("world_view_transform", "projection_matrix", "full_proj_transform", "camera_center"):
                tensor = getattr(camera, attribute)
                if tensor.requires_grad or id(tensor) in parameters:
                    raise ValueError("locked camera calibration cannot enter the optimizer")


def _camera_from_view(view, index, args, root, backend, clipping):
    import torch
    from PIL import Image
    from scene.cameras import Camera
    pose = np.asarray(view["training_c2w_opencv"], dtype=np.float64)
    w2c = np.linalg.inv(pose)
    rotation, translation = w2c[:3, :3].T, w2c[:3, 3]
    width, height = view["training_size"]
    k = view["training_K"]
    fov_x, fov_y = 2. * math.atan(width / (2. * k[0][0])), 2. * math.atan(height / (2. * k[1][1]))
    with Image.open(root / view["image_path"]) as source_image:
        image = composite_training_image(source_image, args.white_background)
    common = dict(colmap_id=index, R=rotation, T=translation, FoVx=fov_x, FoVy=fov_y,
                  image_name=view["view_id"], uid=index, data_device=args.data_device)
    if backend == "3dgs":
        camera = Camera((width, height), depth_params=None, image=image, invdepthmap=None,
                        train_test_exp=False, is_test_dataset=view["split"] != "train",
                        is_test_view=view["split"] != "train", **common)
    elif backend == "2dgs":
        from utils.camera_utils import PILtoTorch
        tensor = PILtoTorch(image, (width, height))
        camera = Camera(image=tensor, gt_alpha_mask=None, **common)
    else:
        raise ValueError("unsupported canonical training backend")
    camera.view_id, camera.split = view["view_id"], view["split"]
    camera.image_path = view["image_path"]
    camera.source_K, camera.K = copy.deepcopy(view.get("source_K", view["K"])), copy.deepcopy(k)
    camera.znear, camera.zfar = clipping["znear"], clipping["zfar"]
    matrix = pinhole_projection(k, width, height, camera.znear, camera.zfar)
    camera.projection_matrix = torch.tensor(matrix, dtype=torch.float32, device="cuda").T.contiguous()
    camera.full_proj_transform = camera.world_view_transform @ camera.projection_matrix
    # None of these tensors are nn.Parameters or optimizer inputs.
    for attribute in ("world_view_transform", "projection_matrix", "full_proj_transform", "camera_center"):
        getattr(camera, attribute).requires_grad_(False)
    return camera


def _install_two_dgs_full_k_depth():
    """Use the same actual K for canonical surfel normal reconstruction."""
    import torch
    import gaussian_renderer
    previous = gaussian_renderer.depth_to_normal
    if getattr(previous, "_gsw_canonical_full_k", False):
        return

    def depth_to_normal(view, depth):
        if not hasattr(view, "K"):
            return previous(view, depth)
        c2w = torch.linalg.inv(view.world_view_transform.T)
        width, height = view.image_width, view.image_height
        x, y = torch.meshgrid(torch.arange(width, device=depth.device, dtype=depth.dtype) + .5,
                              torch.arange(height, device=depth.device, dtype=depth.dtype) + .5,
                              indexing="xy")
        pixels = torch.stack((x, y, torch.ones_like(x)), dim=-1).reshape(-1, 3)
        k = torch.tensor(view.K, dtype=depth.dtype, device=depth.device)
        rays = pixels @ torch.linalg.inv(k).T @ c2w[:3, :3].T
        points = (depth.reshape(-1, 1) * rays + c2w[:3, 3]).reshape(height, width, 3)
        normal = torch.zeros_like(points)
        dx = points[2:, 1:-1] - points[:-2, 1:-1]
        dy = points[1:-1, 2:] - points[1:-1, :-2]
        normal[1:-1, 1:-1] = torch.nn.functional.normalize(torch.cross(dx, dy, dim=-1), dim=-1)
        return normal

    depth_to_normal._gsw_canonical_full_k = True
    gaussian_renderer.depth_to_normal = depth_to_normal


class CanonicalScene:
    """Equivalent native Scene interface for both fixed-camera backends."""
    def __init__(self, args, gaussians, prepared, backend, load_iteration=None):
        import torch
        from utils.graphics_utils import BasicPointCloud
        from native.worker.training_checkpoint import atomic_json
        self.model_path, self.gaussians = args.model_path, gaussians
        self.loaded_iter = load_iteration
        self._canonical_backend = backend
        self._camera_contract = training_contract(prepared)
        if prepared.get("training_initialization_sha256"):
            self._camera_contract["training_initialization_sha256"] = prepared["training_initialization_sha256"]
        self._sample_stream = None
        root, output = Path(prepared["root"]), Path(self.model_path)
        output.mkdir(parents=True, exist_ok=True)
        self._camera_contract["backend"] = backend
        import importlib
        from native.worker.training_checkpoint import file_digest
        rasterizer = importlib.import_module('diff_gaussian_rasterization' if backend == '3dgs' else 'diff_surfel_rasterization')
        if backend == '3dgs' and getattr(rasterizer._C, 'GSW_PINHOLE_COVARIANCE_VERSION', 0) != 1:
            raise ValueError('camera_pinhole_rasterizer_runtime_missing')
        self._camera_contract['backend_identity'] = {
            'python': '{}.{}'.format(__import__('sys').version_info.major, __import__('sys').version_info.minor),
            'torch': torch.__version__, 'cuda': torch.version.cuda,
            'rasterizer_wrapper_sha256': file_digest(rasterizer.__file__),
            'rasterizer_binary_sha256': file_digest(rasterizer._C.__file__),
            'pinhole_covariance_version': getattr(rasterizer._C, 'GSW_PINHOLE_COVARIANCE_VERSION', 0),
            'runtime_proof_sha256': os.environ.get('GSW_CAMERA_RASTERIZER_PROOF_SHA256') if backend == '3dgs' else None,
        }
        self.train_cameras, self.val_cameras, self.test_cameras = {1.: []}, {1.: []}, {1.: []}
        loaded_cameras = []
        for index, view in enumerate(prepared["views"]):
            camera = _camera_from_view(view, index, args, root, backend, prepared["clipping"])
            {"train": self.train_cameras, "val": self.val_cameras, "test": self.test_cameras}[view["split"]][1.].append(camera)
            loaded_cameras.append({"view_id": camera.view_id, "split": camera.split,
                "image_size": [camera.image_width, camera.image_height], "K": copy.deepcopy(camera.K),
                "world_view_transform_float32_transposed": camera.world_view_transform.detach().cpu().tolist(),
                "projection_matrix_float32_transposed": camera.projection_matrix.detach().cpu().tolist(),
                "calibration_requires_grad": any(getattr(camera, name).requires_grad for name in
                    ("world_view_transform", "projection_matrix", "full_proj_transform", "camera_center"))})
        self._camera_contract["loaded_cameras"] = loaded_cameras
        args._gsw_camera_contract = copy.deepcopy(self._camera_contract)
        gaussians._gsw_camera_contract = copy.deepcopy(self._camera_contract)
        if not self.train_cameras[1.]:
            raise ValueError("camera dataset has no training cameras")
        # Extent and all actual camera transforms are now in the same frame.
        centers = np.asarray([camera.camera_center.detach().cpu().numpy() for camera in self.train_cameras[1.]], dtype=np.float64)
        initial_points = np.asarray(prepared["initial_points"], dtype=np.float64)
        center = centers.mean(axis=0)
        self.cameras_extent = max(float(np.linalg.norm(centers - center, axis=1).max()),
                                  float(np.linalg.norm(initial_points - center, axis=1).max())) * 1.1
        if not self.cameras_extent > 0:
            raise ValueError("training extent is degenerate")
        point_cloud = BasicPointCloud(points=initial_points, colors=prepared["initial_colors"],
                                      normals=np.zeros_like(initial_points))
        if load_iteration:
            if load_iteration == -1:
                from utils.system_utils import searchForMaxIteration
                self.loaded_iter = searchForMaxIteration(str(output / "point_cloud"))
            with (output / "training-camera-contract.json").open(encoding="utf-8") as stream:
                if json.load(stream) != self._camera_contract:
                    raise ValueError("camera/scale contract changed; cannot warm start this output")
            path = output / "point_cloud" / ("iteration_{}".format(self.loaded_iter)) / "point_cloud.ply"
            gaussians.load_ply(str(path))
            gaussians.spatial_lr_scale = self.cameras_extent
            gaussians.max_radii2D = torch.zeros((gaussians.get_xyz.shape[0]), device="cuda")
            if backend == "3dgs":
                gaussians.setup_exposure(self.train_cameras[1.])
        elif backend == "3dgs":
            gaussians.create_from_pcd(point_cloud, self.train_cameras[1.], self.cameras_extent)
        else:
            gaussians.create_from_pcd(point_cloud, self.cameras_extent)
        atomic_json(output / "training-camera-contract.json", self._camera_contract)
        # Compatibility camera list now records full K and stable view identity.
        atomic_json(output / "cameras.json", [{"id": index, "view_id": view["view_id"],
            "img_name": view["view_id"], "image_path": view["image_path"], "split": view["split"],
            "width": view["training_size"][0], "height": view["training_size"][1],
            "fx": view["training_K"][0][0], "fy": view["training_K"][1][1],
            "cx": view["training_K"][0][2], "cy": view["training_K"][1][2],
            "position": [row[3] for row in view["training_c2w_opencv"][:3]],
            "rotation": [row[:3] for row in view["training_c2w_opencv"][:3]]}
            for index, view in enumerate(prepared["views"])])
        if backend == "2dgs":
            _install_two_dgs_full_k_depth()

    def getTrainCameras(self, scale=1.):
        return self.train_cameras[scale]

    def getValCameras(self, scale=1.):
        return self.val_cameras[scale]

    def getTestCameras(self, scale=1.):
        return self.test_cameras[scale]

    def audit_sample(self, camera, iteration):
        if camera.split != "train" or camera.view_id not in self._camera_contract["splits"]["train"]:
            raise ValueError("held-out camera cannot participate in training")
        if self._sample_stream is None:
            self._sample_stream = (Path(self.model_path) / "training-camera-samples.jsonl").open("a", encoding="utf-8")
        self._sample_stream.write(json.dumps({"iteration": iteration, "view_id": camera.view_id,
                                               "split": "train"}, separators=(",", ":")) + "\n")
        self._sample_stream.flush()

    def save(self, iteration):
        from native.worker.training_checkpoint import atomic_json, file_digest
        path = Path(self.model_path) / "point_cloud" / ("iteration_{}".format(iteration)) / "point_cloud.ply"
        self.gaussians.save_ply(str(path))
        atomic_json(path.with_suffix(".camera-scale.json"), dict(self._camera_contract, ply_sha256=file_digest(path)))
        if self._canonical_backend == "3dgs":
            exposure = {name: self.gaussians.get_exposure_from_name(name).detach().cpu().tolist()
                        for name in self.gaussians.exposure_mapping}
            atomic_json(Path(self.model_path) / "exposure.json", exposure)


def create_canonical_scene(args, gaussians, backend, load_iteration=None):
    """Use explicitly prepared native input; never fall back when it is bad."""
    selected = camera_dataset_source(args.source_path, getattr(args, "camera_manifest", None))
    if selected is None:
        return None
    image_root = os.environ.get("GSW_CAMERA_IMAGE_ROOT", args.source_path)
    with selected.open(encoding="utf-8") as stream:
        schema = json.load(stream).get("schema")
    if schema == "gsw.prepared_camera_dataset/1":
        prepared = load_prepared_training_dataset(selected, image_root)
    else:
        # Direct CLI input is prepared once into the task output, not into the
        # source directory. Native jobs prepare at the admission boundary.
        report = prepare_training_dataset(selected, Path(args.model_path) / ".gsw-camera-input",
                                           {"resolution": args.resolution})
        prepared = load_prepared_training_dataset(report["manifestPath"], report["imageRoot"])
    return CanonicalScene(args, gaussians, prepared, backend, load_iteration)
