"""CPU-only provenance for the backend model actually loaded for geometry.

File hashes are local integrity evidence, not publisher attestation. This
module does not import Torch, construct a Gaussian model or create optimizers.
"""
import importlib.metadata
import inspect
from pathlib import Path
import sys

from native.backend.gs_geometry import GsGeometryError, _no_links, file_sha256


SCHEMA = "gsw.geometry_loaded_backend_model/1"


def _root(source_root):
    root = Path(source_root)
    if not root.is_absolute() or not root.is_dir():
        raise GsGeometryError("geometry_renderer_source_missing", str(root))
    _no_links(root)
    return root.resolve()


def _loaded_sources(root):
    sources = {}
    for name, module in sorted(list(sys.modules.items())):
        backend_namespace = (name in ("scene", "utils", "arguments")
                             or name.startswith(("scene.", "utils.", "arguments.")))
        filename = getattr(module, "__file__", None)
        if not filename:
            if backend_namespace:
                paths = list(getattr(module, "__path__", []))
                if not paths:
                    raise GsGeometryError("geometry_renderer_source_mismatch", name)
                for namespace in paths:
                    _no_links(namespace)
                    try:
                        Path(namespace).resolve().relative_to(root)
                    except ValueError:
                        raise GsGeometryError("geometry_renderer_source_mismatch", name)
            continue
        path = Path(filename)
        try:
            relative = path.resolve().relative_to(root)
        except ValueError:
            if backend_namespace:
                raise GsGeometryError("geometry_renderer_source_mismatch", name)
            continue
        if path.suffix != ".py":
            continue
        _no_links(path)
        if not path.is_file():
            raise GsGeometryError("geometry_renderer_source_missing", name)
        sources[name] = {"path": relative.as_posix(), "sha256": file_sha256(path)}
    return sources


def _version(distribution):
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return None


def capture_loaded_backend_model(model, source_root, backend, expected_point_count, expected_sh_degree):
    """Describe actual imported code and loaded arrays; never call load/train."""
    root = _root(source_root)
    model_type = type(model)
    sources = _loaded_sources(root)
    try:
        class_path = Path(inspect.getsourcefile(model_type))
        _no_links(class_path)
        class_source = class_path.resolve().relative_to(root).as_posix()
    except (ValueError, TypeError):
        raise GsGeometryError("geometry_renderer_source_mismatch", "loaded model class source")
    if (model_type.__module__ != "scene.gaussian_model" or "scene" not in sources
            or sources.get(model_type.__module__, {}).get("path") != class_source):
        raise GsGeometryError("geometry_renderer_source_mismatch", "loaded model class/module differs")
    try:
        arrays = {name: getattr(model, name) for name in
            ("_xyz", "_features_dc", "_features_rest", "_opacity", "_scaling", "_rotation")}
    except AttributeError as error:
        raise GsGeometryError("geometry_invalid_gaussian_model", str(error))
    if (backend not in ("3dgs", "2dgs") or type(expected_point_count) is not int or expected_point_count <= 0
            or type(expected_sh_degree) is not int or expected_sh_degree < 0):
        raise GsGeometryError("geometry_invalid_gaussian_model", "expected backend layout")
    dimensions = 3 if backend == "3dgs" else 2
    expected_shapes = {"_xyz": [expected_point_count, 3], "_features_dc": [expected_point_count, 1, 3],
        "_features_rest": [expected_point_count, (expected_sh_degree + 1)**2 - 1, 3],
        "_opacity": [expected_point_count, 1], "_scaling": [expected_point_count, dimensions],
        "_rotation": [expected_point_count, 4]}
    if (any(list(value.shape) != expected_shapes[name]
            or str(value.dtype) not in ("float32", "torch.float32")
            or bool(getattr(value, "requires_grad", False)) for name, value in arrays.items())
            or model.active_sh_degree != expected_sh_degree or model.max_sh_degree != expected_sh_degree
            or getattr(model, "optimizer", None) is not None):
        raise GsGeometryError("geometry_invalid_gaussian_model", "loaded model layout, dtype, gradient or optimizer")
    record = {"schema": SCHEMA, "backend": backend, "class_module": model_type.__module__,
        "class_name": model_type.__qualname__, "class_source": class_source,
        "source_root": str(root), "source_hash_mode": "raw-file-v1", "source_files": sources,
        "point_count": int(arrays["_xyz"].shape[0]), "scale_dimensions": int(arrays["_scaling"].shape[1]),
        "sh_degree": int(model.active_sh_degree), "maximum_sh_degree": int(model.max_sh_degree),
        "tensor_shapes": {name: list(value.shape) for name, value in arrays.items()},
        "tensor_dtypes": {name: str(value.dtype) for name, value in arrays.items()},
        "optimizer_created": getattr(model, "optimizer", None) is not None,
        "dependency_versions": {"numpy": _version("numpy"), "plyfile": _version("plyfile")},
        "upstream_render_invoked": False}
    return record


def verify_loaded_backend_model_sources(record):
    """Reject runtime sources changed or rebound during this extraction."""
    if (not isinstance(record, dict) or record.get("schema") != SCHEMA
            or record.get("source_hash_mode") != "raw-file-v1"):
        raise GsGeometryError("geometry_renderer_source_mismatch", "runtime provenance schema")
    root = _root(record["source_root"])
    if _loaded_sources(root) != record.get("source_files"):
        raise GsGeometryError("geometry_source_changed_during_extraction", "loaded backend source bytes or imports")
