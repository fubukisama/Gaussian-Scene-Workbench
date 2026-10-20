"""Read-only Gaussian rendered-depth extraction with immutable product output.

No source photo pixels, optimizer, SfM or reference geometry are consumed.
The product is an appearance-depth proxy, never a material-surface guarantee.
"""

import argparse
import copy
import json
import math
import os
from pathlib import Path
import shutil
import sys

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.backend.atomic_file import atomic_sibling, atomic_sibling_directory
from native.backend.gs_geometry import (SCHEMA, GsGeometryError, _no_links, _read_json,
    backproject_camera_z, camera_z_from_moments, file_sha256, geometry_coordinates,
    inspect_geometry_source, json_sha256)


def _poll(cancelled):
    if cancelled is not None and cancelled():
        raise GsGeometryError("geometry_cancelled")


def _write_json(path, value):
    with atomic_sibling(path) as (temporary, stream):
        json.dump(value, stream, ensure_ascii=False, sort_keys=True, allow_nan=False, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
        stream.close()
        os.link(str(temporary), str(path))


def _config(value):
    allowed = {"schema", "model_directory", "dataset_path", "iteration", "output_directory",
        "coordinate_mode", "alpha_threshold", "depth_range", "pixel_stride", "max_points", "memory_budget_mb"}
    if not isinstance(value, dict) or value.get("schema") != "gsw.gs_geometry_request/1" or set(value) - allowed:
        raise GsGeometryError("geometry_invalid_request")
    result = copy.deepcopy(value)
    for key, default in (("coordinate_mode", "canonical"), ("alpha_threshold", .5),
                         ("pixel_stride", 1), ("max_points", 50000000), ("memory_budget_mb", 512)):
        result.setdefault(key, default)
    for key in ("pixel_stride", "max_points", "memory_budget_mb"):
        if type(result[key]) is not int or result[key] <= 0:
            raise GsGeometryError("geometry_invalid_budget", key)
    alpha = result["alpha_threshold"]
    if isinstance(alpha, bool) or not isinstance(alpha, (int, float)) or not math.isfinite(alpha) or not 0 <= alpha <= 1:
        raise GsGeometryError("geometry_invalid_depth_policy")
    if result["coordinate_mode"] not in ("canonical", "source", "training"):
        raise GsGeometryError("geometry_invalid_coordinate_mode")
    return result


def _fresh_paths(config, report_path):
    try:
        output, report = Path(config["output_directory"]), Path(report_path)
        sources = [Path(config[key]) for key in ("model_directory", "dataset_path")]
    except (TypeError, KeyError) as error:
        raise GsGeometryError("geometry_invalid_path", str(error))
    for path in [output, report] + sources:
        if not path.is_absolute():
            raise GsGeometryError("geometry_absolute_path_required")
        _no_links(path)
    output, report = output.resolve(), report.resolve()
    if output.exists() or report.exists() or not output.parent.is_dir() or not report.parent.is_dir():
        raise GsGeometryError("geometry_fresh_output_required")
    for source in sources:
        source = source.resolve()
        root = source if source.is_dir() else source.parent
        for target in (output, report):
            if target == root or root in target.parents or target in root.parents:
                raise GsGeometryError("geometry_output_overlaps_source")
    if report == output or output in report.parents or report in output.parents:
        raise GsGeometryError("geometry_receipt_must_be_outside_product")
    return output, report


def _depth_policy(config, contract):
    clipping = contract["clipping"]
    requested = config.get("depth_range")
    if requested is None:
        requested = {"min": clipping["znear"], "max": clipping["zfar"], "frame": "training"}
    if not isinstance(requested, dict) or set(requested) != {"min", "max", "frame"}:
        raise GsGeometryError("geometry_invalid_depth_policy")
    low, high, frame = requested["min"], requested["max"], requested["frame"]
    if (frame not in ("source", "canonical", "training") or
            any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in (low, high))
            or not 0 < low < high):
        raise GsGeometryError("geometry_invalid_depth_policy")
    transforms = contract["transforms"]
    factor = (1. if frame == "training" else transforms["canonical_to_training"]["s"])
    if frame == "source":
        factor *= transforms["source_to_canonical"]["s"]
    lower, upper = max(low * factor, clipping["znear"]), min(high * factor, clipping["zfar"])
    if not math.isfinite(lower) or not math.isfinite(upper) or not 0 < lower < upper:
        raise GsGeometryError("geometry_depth_range_outside_trained_clipping")
    return {"requested": copy.deepcopy(requested), "requested_to_training_ratio": factor,
        "effective_training_camera_z": {"min": lower, "max": upper},
        "trained_clipping": copy.deepcopy(clipping),
        "clipped_to_trained_range": lower != low * factor or upper != high * factor}


def _point_chunks(depth, mask, view, source, coordinates, stride, capacity):
    actual = source["loaded_cameras"][view["view_id"]]
    w2c = np.asarray(actual["world_view_transform_float32_transposed"], dtype=np.float32).T.astype(np.float64)
    c2w = np.linalg.inv(w2c)
    # Float32 executed rigid matrices contain small orthogonality error. The
    # public backprojector validates desired proper poses; the exact executed
    # matrix is retained and used explicitly for this reconstruction instead.
    height, width = depth.shape
    columns_per_row = (width + stride - 1) // stride
    rows_per_block = max(1, capacity // max(1, columns_per_row)) * stride
    for row in range(0, height, rows_per_block):
        for column in range(0, width, capacity * stride):
            end_row, end_col = min(row + rows_per_block, height), min(column + capacity * stride, width)
            rows, columns = np.nonzero(mask[row:end_row:stride, column:end_col:stride])
            rows, columns = rows * stride + row, columns * stride + column
            z = depth[rows, columns].astype(np.float64)
            k = view["training_K"]
            camera = np.column_stack(((columns + .5 - k[0][2]) * z / k[0][0],
                (rows + .5 - k[1][2]) * z / k[1][1], z))
            points = camera @ c2w[:3, :3].T + c2w[:3, 3]
            transform = coordinates["transform"]
            points = transform["s"] * points @ np.asarray(transform["R"], dtype=np.float64).T + transform["t"]
            if not np.isfinite(points).all():
                raise GsGeometryError("geometry_numeric_range_exceeded")
            yield np.asarray(points, dtype="<f8")


def _write_array(path, array):
    with path.open("xb") as stream:
        np.save(stream, array, allow_pickle=False)
        stream.flush()
        os.fsync(stream.fileno())
    return {"path": path.as_posix(), "sha256": file_sha256(path), "shape": list(array.shape), "dtype": str(array.dtype)}


def _source_fingerprints():
    repository = Path(__file__).resolve().parents[2]
    files = ["native/backend/gs_geometry.py", "native/worker/gs_geometry.py", "native/backend/camera_dataset.py",
        "native/worker/canonical_training.py", "native/worker/camera_rasterizer_runtime.py", "native/worker/gs_geometry_runtime.py"]
    import hashlib
    return {relative: hashlib.sha256((repository / relative).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            for relative in files}


def extract_geometry(config, report_path, renderer_factory=None, cancelled=None, progress=None):
    """Public transaction; injected renderers are a test seam, not a CLI option."""
    config = _config(config)
    output, report = _fresh_paths(config, report_path)
    _poll(cancelled)
    source = inspect_geometry_source(config["model_directory"], config.get("iteration"), config["dataset_path"], cancelled=cancelled)
    coordinates = geometry_coordinates(source["contract"], config["coordinate_mode"])
    policy = _depth_policy(config, source["contract"])
    definition = ("expected_camera_z_gaussian_center_attribute_alpha_compositing" if source["backend"] == "3dgs"
                  else "expected_camera_z_surfel_ray_intersection_alpha_compositing")
    manifest = {"schema": SCHEMA, "status": "running", "request": config,
        "source": {key: copy.deepcopy(source[key]) for key in ("backend", "input_data_identity", "iteration", "hashes", "model_info")},
        "frames": coordinates, "depth_policy": policy, "views": [], "geometry": None,
        "surface_semantics": "appearance_proxy_not_material_boundary", "model_geometry_accuracy": "not_measured",
        "sampling": {"pixel_stride": config["pixel_stride"], "automatic_quality_reduction": False},
        "memory_budget": {"megabytes": config["memory_budget_mb"], "scope": "CPU_staging_and_backprojection_not_GPU_reservation"}}
    manifest["source"].update(config_sha256=json_sha256(config), split_sha256=json_sha256(source["contract"]["splits"]),
        source_hash_mode="text-lf-v1", source_fingerprints=_source_fingerprints(),
        training_camera_contract=copy.deepcopy(source["contract"]))
    count, renderer, failure = 0, None, None
    with atomic_sibling_directory(output) as stage:
        try:
            _poll(cancelled)
            renderer = (renderer_factory or _CudaRenderer)(source)
            manifest["source"]["renderer"] = copy.deepcopy(renderer.identity)
            (stage / "views").mkdir()
            raw = stage / "points.raw"
            with raw.open("xb") as points_stream:
                for index, view in enumerate(source["views"]):
                    _poll(cancelled)
                    width, height = view["training_size"]
                    budget = config["memory_budget_mb"] * 1024 * 1024
                    if width * height * 32 > budget:
                        raise GsGeometryError("geometry_view_exceeds_memory_budget", view["view_id"])
                    moments = renderer.render(view)
                    decoded = camera_z_from_moments(moments["alpha"], moments["camera_z_moment"],
                        alpha_threshold=config["alpha_threshold"], depth_min=policy["effective_training_camera_z"]["min"],
                        depth_max=policy["effective_training_camera_z"]["max"])
                    if decoded["depth"].shape != (height, width):
                        raise GsGeometryError("geometry_render_size_mismatch")
                    _poll(cancelled)
                    directory = stage / "views" / ("v%06d" % index)
                    directory.mkdir()
                    record = {"view_id": view["view_id"], "split": "train", "K": view["training_K"],
                        "image_size": view["training_size"], "training_c2w_opencv": view["training_c2w_opencv"],
                        "executed_camera": source["loaded_cameras"][view["view_id"]], "statistics": decoded["statistics"],
                        "depth_definition": definition, "depth_frame": "training.camera_z", "invalid_depth_value": 0.,
                        "mask_definition": "finite_positive_alpha_and_camera_z_with_explicit_opacity_and_depth_thresholds_not_physical_confidence"}
                    for key, array in (("depth", decoded["depth"]), ("mask", decoded["mask"]),
                                       ("alpha", np.asarray(moments["alpha"], dtype=np.float32))):
                        asset = _write_array(directory / (key + ".npy"), array)
                        asset["path"] = (directory.relative_to(stage) / (key + ".npy")).as_posix()
                        record[key] = asset
                    manifest["views"].append(record)
                    capacity = max(1, min(65536, budget // 256))
                    for chunk in _point_chunks(decoded["depth"], decoded["mask"], view, source, coordinates,
                                               config["pixel_stride"], capacity):
                        _poll(cancelled)
                        if count + len(chunk) > config["max_points"]:
                            raise GsGeometryError("geometry_point_budget_exceeded")
                        points_stream.write(chunk.tobytes(order="C"))
                        count += len(chunk)
                    if progress:
                        progress({"completed_views": index + 1, "total_views": len(source["views"]), "point_count": count})
                points_stream.flush()
                os.fsync(points_stream.fileno())
            if count == 0:
                raise GsGeometryError("geometry_no_valid_points")
            _poll(cancelled)
            again = inspect_geometry_source(config["model_directory"], config["iteration"], config["dataset_path"], cancelled=cancelled)
            if again["hashes"] != source["hashes"] or again["input_data_identity"] != source["input_data_identity"]:
                raise GsGeometryError("geometry_source_changed_during_extraction")
            if _source_fingerprints() != manifest["source"]["source_fingerprints"]:
                raise GsGeometryError("geometry_source_changed_during_extraction", "geometry adapter source bytes")
            if hasattr(renderer, "verify_sources"):
                renderer.verify_sources()
            cloud = stage / "point_cloud.ply"
            with cloud.open("xb") as stream, raw.open("rb") as data:
                stream.write(("ply\nformat binary_little_endian 1.0\ncomment gsw rendered_depth_appearance_proxy\n"
                    "element vertex %d\nproperty double x\nproperty double y\nproperty double z\nend_header\n" % count).encode("ascii"))
                shutil.copyfileobj(data, stream, 4 * 1024 * 1024)
                stream.flush()
                os.fsync(stream.fileno())
            raw.unlink()
            coordinates["transform"]["applied"] = True
            sidecar = cloud.with_suffix(".camera-scale.json")
            cloud_hash = file_sha256(cloud)
            _write_json(sidecar, {"schema": "gsw.coordinate_export/1", "success": True,
                "coordinate_mode": coordinates["frame"], "input_frame": "training", "output_frame": coordinates["frame"],
                "output_unit": coordinates["unit"], "meters_per_unit": coordinates["meters_per_unit"],
                "scale_status": coordinates["scale_status"], "scale_evidence": coordinates["scale"].get("evidence", []),
                "input_sha256": source["model_sha256"], "input_metadata_sha256": source["hashes"]["sidecar"],
                "ply_sha256": cloud_hash, "output_sha256": cloud_hash, "vertex_count": count, "gaussian_dimensions": 0,
                "applied_transform": copy.deepcopy(coordinates["transform"]),
                "input_data_identity": source["input_data_identity"], "training_contract": copy.deepcopy(source["contract"]),
                "geometry_kind": "depth_point_cloud", "depth_definition": definition, "fusion": "none",
                "surface_semantics": "appearance_proxy_not_material_boundary", "model_geometry_accuracy": "not_measured",
                "original_training_model_retained": True})
            manifest["geometry"] = {key: coordinates[key] for key in ("frame", "frame_id", "unit", "meters_per_unit", "scale_status")}
            manifest["geometry"].update(kind="depth_point_cloud", path="point_cloud.ply", sha256=cloud_hash,
                point_count=count, fusion="none", surface_semantics="appearance_proxy_not_material_boundary",
                coordinate_sidecar={"path": sidecar.name, "sha256": file_sha256(sidecar)})
            manifest["status"] = "completed"
        except Exception as error:
            failure = error if isinstance(error, GsGeometryError) else GsGeometryError("geometry_extraction_failed", str(error))
            manifest["status"] = "cancelled" if failure.code == "geometry_cancelled" else "failed"
            manifest["error"] = {"code": failure.code, "detail": failure.detail}
            for name in ("points.raw", "point_cloud.ply", "point_cloud.camera-scale.json"):
                if (stage / name).is_file():
                    (stage / name).unlink()
        finally:
            if renderer is not None:
                try:
                    renderer.close()
                except Exception as error:
                    if failure is None:
                        failure = GsGeometryError("geometry_renderer_cleanup_failed", str(error))
                        manifest["status"] = "failed"
                        manifest["geometry"] = None
                        manifest["error"] = {"code": failure.code, "detail": failure.detail}
        _write_json(stage / "manifest.json", manifest)
        _no_links(output.parent)
        if output.exists():
            raise GsGeometryError("geometry_fresh_output_required")
        os.rename(str(stage), str(output))
    result = {"schema": "gsw.gs_geometry_report/1", "success": failure is None,
        "manifestPath": str(output / "manifest.json"), "geometryPath": str(output / "point_cloud.ply") if failure is None else "",
        "summary": {"backend": source["backend"], "status": manifest["status"], "viewCount": len(manifest["views"]),
            "pointCount": count if failure is None else 0, "coordinateMode": coordinates["frame"], "unit": coordinates["unit"],
            "metersPerUnit": coordinates["meters_per_unit"], "surfaceSemantics": "appearance_proxy_not_material_boundary"}}
    if failure:
        result["error"] = manifest["error"]
    _write_json(report, result)
    return result


class _CudaRenderer:
    def __init__(self, source):
        import importlib
        repository = Path(__file__).resolve().parents[2]
        self.backend, self.source = source["backend"], source
        if any(name in sys.modules for name in ("scene", "diff_gaussian_rasterization", "diff_surfel_rasterization")):
            raise GsGeometryError("geometry_renderer_requires_fresh_process")
        if self.backend == "3dgs":
            source_root = repository / "gaussian-splatting"
            os.environ["GSW_POSE_POLICY"] = "locked"
            os.environ["GSW_CAMERA_DATASET"] = source["prepared_manifest_path"]
            from native.worker.camera_rasterizer_runtime import configure_camera_rasterizer
            configure_camera_rasterizer(repository)
        else:
            explicit = os.environ.get("GSW_TWO_DGS_SOURCE") or os.environ.get("TWO_DGS_DIR")
            source_root = Path(explicit) if explicit else Path(sys.executable).resolve().parent.parent.parent / "2dgs"
        if not (source_root / "scene/gaussian_model.py").is_file():
            raise GsGeometryError("geometry_renderer_source_missing")
        _no_links(source_root)
        sys.path.insert(0, str(source_root.resolve()))
        import torch
        if not torch.cuda.is_available():
            raise GsGeometryError("geometry_cuda_required")
        self.torch = torch
        rasterizer = importlib.import_module("diff_gaussian_rasterization" if self.backend == "3dgs" else "diff_surfel_rasterization")
        observed = {"python": "%d.%d" % sys.version_info[:2], "torch": torch.__version__, "cuda": torch.version.cuda,
            "rasterizer_wrapper_sha256": file_sha256(rasterizer.__file__),
            "rasterizer_binary_sha256": file_sha256(rasterizer._C.__file__),
            "pinhole_covariance_version": getattr(rasterizer._C, "GSW_PINHOLE_COVARIANCE_VERSION", 0),
            "runtime_proof_sha256": os.environ.get("GSW_CAMERA_RASTERIZER_PROOF_SHA256") if self.backend == "3dgs" else None}
        if observed != source["contract"].get("backend_identity"):
            raise GsGeometryError("geometry_trained_rasterizer_identity_mismatch")
        self.identity = dict(observed, kind="actual_CUDA_rasterizer", antialiasing=False,
            antialiasing_policy="explicit_fixed_false_not_inferred_from_training_configuration",
            upstream_render_invoked=False, upstream_renderer_source_scope="reference_only_not_executed",
            render_source_sha256=file_sha256(source_root / "gaussian_renderer/__init__.py"),
            gaussian_model_source_sha256=file_sha256(source_root / "scene/gaussian_model.py"))
        self.rasterizer = rasterizer
        from scene.gaussian_model import GaussianModel
        with torch.no_grad():
            self.model = GaussianModel(source["model_info"]["sh_degree"])
            self.model.load_ply(source["model_path"])
            for name in ("_xyz", "_features_dc", "_features_rest", "_opacity", "_scaling", "_rotation"):
                getattr(self.model, name).requires_grad_(False)
            for value in (self.model.get_xyz, self.model.get_scaling, self.model.get_rotation, self.model.get_opacity):
                if not torch.isfinite(value).all().item():
                    raise GsGeometryError("geometry_nonfinite_activated_gaussian_model")
        from native.worker.gs_geometry_runtime import capture_loaded_backend_model
        self.identity["loaded_backend_model"] = capture_loaded_backend_model(self.model, source_root, self.backend,
            source["model_info"]["point_count"], source["model_info"]["sh_degree"])

    def verify_sources(self):
        from native.worker.gs_geometry_runtime import verify_loaded_backend_model_sources
        verify_loaded_backend_model_sources(self.identity["loaded_backend_model"])
        if (file_sha256(self.rasterizer.__file__) != self.identity["rasterizer_wrapper_sha256"]
                or file_sha256(self.rasterizer._C.__file__) != self.identity["rasterizer_binary_sha256"]):
            raise GsGeometryError("geometry_source_changed_during_extraction", "rasterizer wrapper or binary")

    def render(self, view):
        torch, pc, rasterizer = self.torch, self.model, self.rasterizer
        actual = self.source["loaded_cameras"][view["view_id"]]
        width, height = view["training_size"]
        with torch.no_grad():
            world_view = torch.tensor(actual["world_view_transform_float32_transposed"], dtype=torch.float32, device="cuda")
            projection = torch.tensor(actual["projection_matrix_float32_transposed"], dtype=torch.float32, device="cuda")
            settings = dict(image_height=height, image_width=width,
                tanfovx=width / (2. * view["training_K"][0][0]), tanfovy=height / (2. * view["training_K"][1][1]),
                bg=torch.zeros(3, dtype=torch.float32, device="cuda"), scale_modifier=1., viewmatrix=world_view,
                projmatrix=world_view @ projection, sh_degree=pc.active_sh_degree,
                campos=torch.linalg.inv(world_view)[3, :3], prefiltered=False, debug=False)
            if self.backend == "3dgs":
                settings["antialiasing"] = False
                z = (pc.get_xyz * world_view[:3, 2]).sum(dim=1) + world_view[3, 2]
                features = torch.stack((torch.ones_like(z), z, torch.zeros_like(z)), dim=1)
            else:
                features = torch.zeros_like(pc.get_xyz)
            output = rasterizer.GaussianRasterizer(raster_settings=rasterizer.GaussianRasterizationSettings(**settings))(
                means3D=pc.get_xyz, means2D=torch.zeros_like(pc.get_xyz), shs=None, colors_precomp=features,
                opacities=pc.get_opacity, scales=pc.get_scaling, rotations=pc.get_rotation, cov3D_precomp=None)
            if self.backend == "3dgs":
                alpha, moment = output[0][0], output[0][1]
            else:
                alpha, moment = output[2][1], output[2][0]
            # Direct raster output: never the RGB-clamping upstream render().
            return {"alpha": alpha.detach().cpu().numpy(), "camera_z_moment": moment.detach().cpu().numpy()}

    def close(self):
        if hasattr(self, "model"):
            del self.model
        self.torch.cuda.empty_cache()


def main(argv=None):
    # Machine-readable pipes are always UTF-8, independent of Windows locale
    # or an inherited PYTHONIOENCODING (including cp932).
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", line_buffering=True)
        except (AttributeError, ValueError, OSError):
            pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args(argv)
    from native.worker.camera_import import _InputCancellation
    cancel = _InputCancellation()
    def progress(value):
        print("[worker-event] " + json.dumps({"version": 1, "type": "progress", "stage": "gs_geometry", **value}), flush=True)
    try:
        result = extract_geometry(_read_json(args.config), args.report, cancelled=cancel, progress=progress)
    except Exception as error:
        typed = error if isinstance(error, GsGeometryError) else GsGeometryError("geometry_extraction_failed", str(error))
        result = {"schema": "gsw.gs_geometry_report/1", "success": False, "error": {"code": typed.code, "detail": typed.detail}}
    print(json.dumps(result, ensure_ascii=False, allow_nan=False), flush=True)
    return 0 if result["success"] else (130 if result.get("error", {}).get("code") == "geometry_cancelled" else 1)


if __name__ == "__main__":
    sys.exit(main())
