"""CPU contracts and camera-Z geometry for native Gaussian extraction.

Geometry is an opacity-rendered appearance proxy, not a measured material
boundary. No Gaussian optimizer, renderer, source photo pixels or reference
geometry is loaded by this module.
"""

import copy
import hashlib
import json
import math
import os
from pathlib import Path
import stat

import numpy as np

from native.backend import camera_dataset


SCHEMA = "gsw.gs_geometry/1"
MAXIMUM_METADATA_BYTES = 16 * 1024 * 1024


class GsGeometryError(ValueError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, str(detail)
        super().__init__(code + (": " + self.detail if self.detail else ""))


def _fail(code, detail=""):
    raise GsGeometryError(code, detail)


def _poll(cancelled):
    if cancelled is not None and cancelled():
        _fail("geometry_cancelled")


def file_sha256(path, cancelled=None):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            _poll(cancelled)
            digest.update(block)
    return digest.hexdigest()


def json_sha256(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _no_links(path):
    for current in (Path(path), *Path(path).parents):
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
            _fail("geometry_unsafe_path", str(path))


def _read_json(path):
    path = Path(path)
    _no_links(path)
    if not path.is_file() or path.stat().st_size > MAXIMUM_METADATA_BYTES:
        _fail("geometry_metadata_missing_or_too_large", str(path))
    return camera_dataset._read_json(path)


def _validate_gaussian_ply(path, backend, cancelled=None):
    from plyfile import PlyData
    try:
        ply = PlyData.read(str(path), mmap="r")
        if "vertex" not in ply or not len(ply["vertex"].data) or "face" in ply:
            _fail("geometry_gaussian_model_required")
        vertices = ply["vertex"].data
        names = set(vertices.dtype.names or ())
        dimensions = 3 if backend == "3dgs" else 2
        required = {"x", "y", "z", "opacity", "f_dc_0", "f_dc_1", "f_dc_2"}
        required.update("scale_" + str(index) for index in range(dimensions))
        required.update("rot_" + str(index) for index in range(4))
        if not required <= names or {name for name in names if name.startswith("scale_")} != {
                "scale_" + str(index) for index in range(dimensions)}:
            _fail("geometry_gaussian_model_required", backend)
        rest = {name for name in names if name.startswith("f_rest_")}
        if rest != {"f_rest_" + str(index) for index in range(len(rest))} or len(rest) % 3:
            _fail("geometry_invalid_gaussian_sh")
        degree = int(math.isqrt(len(rest) // 3 + 1)) - 1
        if 3 * ((degree + 1) ** 2 - 1) != len(rest):
            _fail("geometry_invalid_gaussian_sh")
        for start in range(0, len(vertices), 65536):
            _poll(cancelled)
            chunk = vertices[start:start + 65536]
            for name in names:
                if chunk[name].dtype.kind not in "fiu" or not np.isfinite(chunk[name]).all():
                    _fail("geometry_nonfinite_gaussian_model", name)
            quaternion = np.column_stack([chunk["rot_" + str(index)] for index in range(4)])
            if np.any(np.linalg.norm(quaternion.astype(np.float64), axis=1) <= 1e-20):
                _fail("geometry_invalid_gaussian_rotation")
        return {"point_count": len(vertices), "sh_degree": degree}
    except GsGeometryError:
        raise
    except (OSError, ValueError, TypeError, KeyError) as error:
        _fail("geometry_invalid_gaussian_model", str(error))


def inspect_geometry_source(model_directory, iteration, dataset_path, cancelled=None):
    """Admit one hash-bound native trained model without decoding photo pixels.

    Full current dataset/initializer identity is revalidated, but only train
    views are selected for geometry. No Scene constructor, SfM, optimizer or
    output-writing renderer is invoked. Returned paths are runtime references;
    the immutable product records their hashes and relative output assets.
    """
    from native.worker.canonical_training import load_prepared_training_dataset, training_contract, pinhole_projection
    _poll(cancelled)
    model, dataset = Path(model_directory), Path(dataset_path)
    if (not model.is_absolute() or not dataset.is_absolute() or type(iteration) is not int or iteration <= 0):
        _fail("geometry_invalid_source")
    _no_links(model)
    _no_links(dataset)
    model, dataset = model.resolve(), dataset.resolve()
    dataset_root = dataset if dataset.is_dir() else dataset.parent
    ply = model / "point_cloud" / ("iteration_" + str(iteration)) / "point_cloud.ply"
    sidecar = ply.with_suffix(".camera-scale.json")
    contract_path = model / "training-camera-contract.json"
    _no_links(ply)
    if not ply.is_file():
        _fail("geometry_gaussian_model_missing", str(ply))
    model_hash = file_sha256(ply, cancelled)
    contract, metadata = _read_json(contract_path), _read_json(sidecar)
    if (contract.get("schema") != "gsw.training_camera_contract/1" or contract.get("pose_policy") != "locked"
            or contract.get("backend") not in ("3dgs", "2dgs")):
        _fail("geometry_locked_camera_contract_required")
    if metadata.get("ply_sha256") != model_hash:
        _fail("geometry_model_hash_mismatch")
    if {key: value for key, value in metadata.items() if key != "ply_sha256"} != contract:
        _fail("geometry_model_camera_contract_mismatch")
    backend = contract["backend"]
    marker = model / "training_backend.json"
    if marker.exists() and _read_json(marker).get("backend") != backend:
        _fail("geometry_backend_mismatch")
    manifests = [path for path in (model / ".gsw/camera-input/training-camera-dataset.json",
                 model / ".gsw-camera-input/training-camera-dataset.json") if path.is_file()]
    if len(manifests) != 1:
        _fail("geometry_prepared_camera_contract_required")
    prepared_path = manifests[0]
    _read_json(prepared_path)  # Bound and guard the file before the shared loader.
    try:
        admitted = camera_dataset.load_camera_dataset(dataset, cancelled=cancelled)
        _poll(cancelled)
        prepared = load_prepared_training_dataset(prepared_path, dataset_root)
        _poll(cancelled)
        expected = training_contract(prepared)
        expected["training_initialization_sha256"] = prepared["training_initialization_sha256"]
        if (admitted["data_identity"] != contract.get("input_data_identity")
                or any(contract.get(key) != value for key, value in expected.items())):
            _fail("geometry_dataset_identity_mismatch")
    except GsGeometryError:
        raise
    except (OSError, ValueError, TypeError, KeyError) as error:
        _poll(cancelled)
        _fail("geometry_dataset_identity_mismatch", str(error))
    observed = contract.get("loaded_cameras")
    if (not isinstance(observed, list) or len(observed) != len(contract["views"])
            or any(not isinstance(item, dict) for item in observed)
            or len({item.get("view_id") for item in observed}) != len(observed)):
        _fail("geometry_executed_cameras_required")
    observed = {item["view_id"]: item for item in observed}
    for view in contract["views"]:
        _poll(cancelled)
        actual = observed.get(view["view_id"], {})
        w2c = np.linalg.inv(np.asarray(view["training_c2w_opencv"], dtype=np.float64)).astype(np.float32)
        projection = pinhole_projection(view["training_K"], *view["training_size"],
                                       contract["clipping"]["znear"], contract["clipping"]["zfar"])
        try:
            executed = np.asarray(actual.get("world_view_transform_float32_transposed"), dtype=np.float32)
            executed_projection = np.asarray(actual.get("projection_matrix_float32_transposed"), dtype=np.float32)
        except (ValueError, TypeError, OverflowError):
            _fail("geometry_executed_camera_mismatch", view["view_id"])
        if (actual.get("split") != view["split"] or actual.get("K") != view["training_K"]
                or actual.get("image_size") != view["training_size"] or actual.get("calibration_requires_grad") is not False
                or executed.shape != (4, 4) or not np.isfinite(executed).all()
                or not np.allclose(executed.T, w2c, rtol=8 * np.finfo(np.float32).eps, atol=8 * np.finfo(np.float32).eps)
                or not np.array_equal(executed_projection, np.asarray(projection, dtype=np.float32).T)):
            _fail("geometry_executed_camera_mismatch", view["view_id"])
    model_info = _validate_gaussian_ply(ply, backend, cancelled)
    if file_sha256(ply, cancelled) != model_hash:
        _fail("geometry_model_changed_during_admission")
    return {"schema": "gsw.gs_geometry_source/1", "backend": backend,
            "input_data_identity": contract["input_data_identity"], "model_path": str(ply),
            "model_sha256": model_hash, "model_info": model_info, "model_directory": str(model),
            "dataset_path": str(dataset), "prepared_manifest_path": str(prepared_path), "iteration": iteration,
            "contract": copy.deepcopy(contract), "loaded_cameras": copy.deepcopy(observed),
            "views": [copy.deepcopy(view) for view in contract["views"] if view["split"] == "train"],
            "hashes": {"ply": model_hash, "sidecar": file_sha256(sidecar),
                       "training_camera_contract": file_sha256(contract_path),
                       "prepared_manifest": file_sha256(prepared_path)}}


def _explicit_similarity(value, input_frame, output_frame):
    inverse = camera_dataset.invert_sim3(value)
    return {"version": 1, "s": float(value["s"]), "R": copy.deepcopy(value["R"]), "t": copy.deepcopy(value["t"]),
            "inverse": {key: copy.deepcopy(inverse[key]) for key in ("s", "R", "t")},
            "input_frame": input_frame, "output_frame": output_frame, "mode": "explicit", "applied": False}


def geometry_coordinates(contract, mode="canonical"):
    """Plan an exact-once positive-Sim3 output and explicit length ratios.

    A null meters_per_unit is unknown physical scale, not1. Output-transform
    application remains False until the worker has written transformed points.
    """
    if (contract.get("schema") != "gsw.training_camera_contract/1" or contract.get("pose_policy") != "locked"
            or mode not in ("training", "canonical", "source")):
        _fail("geometry_invalid_coordinate_mode")
    try:
        source = contract["transforms"]["source_to_canonical"]
        training = contract["transforms"]["canonical_to_training"]
        if (source.get("applied") is not True or training.get("applied") is not True
                or source.get("input_frame") != "source" or source.get("output_frame") != "canonical"
                or training.get("input_frame") != "canonical" or training.get("output_frame") != "training"):
            _fail("geometry_unapplied_coordinate_chain")
        inverse_source = camera_dataset.invert_sim3(source)
        inverse_training = camera_dataset.invert_sim3(training)
        scale = contract["scale"]
        status, meters = scale["status"], scale.get("meters_per_unit")
        if status not in ("arbitrary", "source_declared", "constraint_derived", "independently_checked"):
            _fail("geometry_invalid_scale_provenance")
        known_metric = status != "arbitrary" and meters is not None
        if known_metric and (isinstance(meters, bool) or not isinstance(meters, (int, float))
                or not math.isfinite(meters) or meters <= 0 or scale.get("canonical_unit") != "m"
                or not math.isclose(meters, source["s"], rel_tol=1e-12) or not scale.get("evidence")):
            _fail("geometry_invalid_scale_provenance")
        if mode == "training":
            value = {"s": 1., "R": np.eye(3).tolist(), "t": [0., 0., 0.]}
            unit, output_meters = "training", 1. / training["s"] if known_metric else None
        elif mode == "canonical":
            value = inverse_training
            unit, output_meters = ("m", 1.) if known_metric else ("unknown", None)
        else:
            a, b = inverse_source, inverse_training
            rotation = np.asarray(a["R"], dtype=np.float64)
            value = {"s": a["s"] * b["s"], "R": (rotation @ np.asarray(b["R"], dtype=np.float64)).tolist(),
                     "t": (a["s"] * rotation @ np.asarray(b["t"], dtype=np.float64) + np.asarray(a["t"])).tolist()}
            unit, output_meters = scale.get("source_unit", "unknown"), meters if known_metric else None
        transform = _explicit_similarity(value, "training", mode)
        if output_meters is not None and (not math.isfinite(output_meters) or output_meters <= 0):
            _fail("geometry_numeric_range_exceeded")
        frame_id = "gsw:" + contract["input_data_identity"] + ":" + json_sha256(contract) + ":" + mode
        return {"frame": mode, "frame_id": frame_id, "unit": unit, "meters_per_unit": output_meters,
                "scale_status": status, "depth_training_to_output_ratio": transform["s"],
                "training_meters_per_unit": 1. / training["s"] if known_metric else None,
                "transform": transform, "chain": copy.deepcopy(contract["transforms"]), "scale": copy.deepcopy(scale)}
    except GsGeometryError:
        raise
    except (ValueError, TypeError, KeyError, OverflowError) as error:
        _fail("geometry_invalid_coordinate_chain", str(error))


def camera_z_from_moments(alpha, camera_z_moment, *, alpha_threshold, depth_min, depth_max):
    """Decode S/A with an explicit numeric-opacity mask; invalid depth is0.

    A is sum(alpha_i * front_transmittance_i), S is the corresponding weighted
    camera-Z moment. This is not inverse depth, ray range or physical validity.
    """
    values = (alpha_threshold, depth_min, depth_max)
    if (any(isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value)
            for value in values) or not 0 <= alpha_threshold <= 1 or not 0 < depth_min < depth_max):
        _fail("geometry_invalid_depth_policy")
    alpha, moment = np.asarray(alpha, dtype=np.float32), np.asarray(camera_z_moment, dtype=np.float32)
    if alpha.ndim != 2 or moment.shape != alpha.shape:
        _fail("geometry_invalid_depth_arrays")
    mask = (np.isfinite(alpha) & np.isfinite(moment) & (alpha > 0) & (alpha >= alpha_threshold)
            & (alpha <= 1. + 8. * np.finfo(np.float32).eps) & (moment > 0))
    depth = np.zeros(alpha.shape, dtype=np.float32)
    with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
        np.divide(moment, alpha, out=depth, where=mask)
    mask &= np.isfinite(depth) & (depth >= depth_min) & (depth <= depth_max)
    depth[~mask] = 0.
    return {"depth": depth, "mask": mask,
            "statistics": {"total_pixels": int(mask.size), "valid_pixels": int(np.count_nonzero(mask)),
                           "invalid_pixels": int(mask.size - np.count_nonzero(mask))}}


def backproject_camera_z(depth, mask, intrinsics, camera_to_world, *, pixel_stride=1, transform=None):
    """Backproject pixel-border K at (u+.5,v+.5), using positive camera Z.

    Returns float64 positions in the camera pose frame, optionally transformed
    once by an explicitly supplied positive Sim3. No camera/model fitting.
    """
    depth, mask = np.asarray(depth), np.asarray(mask)
    k, pose = np.asarray(intrinsics, dtype=np.float64), np.asarray(camera_to_world, dtype=np.float64)
    if (depth.ndim != 2 or mask.shape != depth.shape or mask.dtype != np.bool_
            or type(pixel_stride) is not int or pixel_stride < 1
            or k.shape != (3, 3) or pose.shape != (4, 4)
            or not np.isfinite(k).all() or not np.isfinite(pose).all()
            or k[0, 0] <= 0 or k[1, 1] <= 0 or k[0, 1] != 0 or k[1, 0] != 0
            or not np.array_equal(k[2], [0., 0., 1.])
            or not np.array_equal(pose[3], [0., 0., 0., 1.])):
        _fail("geometry_invalid_projection")
    camera_dataset.invert_sim3({"s": 1., "R": pose[:3, :3].tolist(), "t": pose[:3, 3].tolist()})
    rows, columns = np.nonzero(mask[::pixel_stride, ::pixel_stride])
    rows, columns = rows * pixel_stride, columns * pixel_stride
    z = depth[rows, columns].astype(np.float64)
    if not np.isfinite(z).all() or np.any(z <= 0):
        _fail("geometry_invalid_camera_z")
    camera = np.column_stack(((columns + .5 - k[0, 2]) * z / k[0, 0],
                              (rows + .5 - k[1, 2]) * z / k[1, 1], z))
    points = camera @ pose[:3, :3].T + pose[:3, 3]
    if transform is not None:
        camera_dataset.invert_sim3(transform)
        points = (float(transform["s"]) * (points @ np.asarray(transform["R"], dtype=np.float64).T)
                  + np.asarray(transform["t"], dtype=np.float64))
    if not np.isfinite(points).all():
        _fail("geometry_numeric_range_exceeded")
    return points
