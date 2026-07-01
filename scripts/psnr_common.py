from __future__ import annotations

import csv
import math
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


DEFAULT_IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".tif", ".tiff")


def _parse_scalar(value: str) -> Any:
    value = value.strip()
    if value == "null":
        return None
    if value == "true":
        return True
    if value == "false":
        return False
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    if value.startswith("'") and value.endswith("'"):
        return value[1:-1]
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1].strip()
        if not inner:
            return []
        return [_parse_scalar(part.strip()) for part in inner.split(",")]
    try:
        if "." in value:
            return float(value)
        return int(value)
    except ValueError:
        return value


def _minimal_yaml_load(text: str) -> dict[str, Any]:
    data: dict[str, Any] = {}
    current_map: str | None = None

    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if not raw_line.startswith((" ", "\t")):
            key, _, value = line.partition(":")
            key = key.strip()
            value = value.strip()
            if value:
                data[key] = _parse_scalar(value)
                current_map = None
            else:
                data[key] = {}
                current_map = key
            continue
        if current_map is None:
            raise ValueError(f"Unsupported YAML structure near: {raw_line}")
        key, _, value = line.strip().partition(":")
        data[current_map][key.strip()] = _parse_scalar(value.strip())

    return data


def load_config(config_path: str | Path) -> dict[str, Any]:
    path = Path(config_path).resolve()
    text = path.read_text(encoding="utf-8")
    try:
        import yaml  # type: ignore

        config = yaml.safe_load(text)
    except Exception:
        config = _minimal_yaml_load(text)
    if not isinstance(config, dict):
        raise ValueError(f"Config must be a mapping: {path}")
    root = path.parent.parent if path.parent.name == "configs" else Path.cwd()
    config["_config_path"] = path
    config["_project_root"] = root.resolve()
    return config


def project_path(config: dict[str, Any], value: str | Path | None) -> Path | None:
    if value in (None, ""):
        return None
    path = Path(value)
    if path.is_absolute():
        return path
    return (Path(config["_project_root"]) / path).resolve()


def image_extensions(config: dict[str, Any]) -> tuple[str, ...]:
    values = config.get("image_extensions") or DEFAULT_IMAGE_EXTENSIONS
    return tuple(str(ext).lower() for ext in values)


def list_images(directory: Path, extensions: tuple[str, ...]) -> list[Path]:
    if not directory.exists():
        return []
    return sorted(
        path
        for path in directory.rglob("*")
        if path.is_file() and path.suffix.lower() in extensions
    )


def index_images_by_stem(directory: Path, extensions: tuple[str, ...]) -> dict[str, Path]:
    images = list_images(directory, extensions)
    index: dict[str, Path] = {}
    duplicates: dict[str, list[Path]] = {}
    for path in images:
        stem = path.stem
        if stem in index:
            duplicates.setdefault(stem, [index[stem]]).append(path)
        else:
            index[stem] = path
    if duplicates:
        details = "\n".join(
            f"{stem}: " + ", ".join(str(p) for p in paths)
            for stem, paths in sorted(duplicates.items())
        )
        raise ValueError(f"Duplicate image stems found in {directory}:\n{details}")
    return index


def read_manifest(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise FileNotFoundError(f"Manifest not found: {path}")
    with path.open("r", newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_rgb_image(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0


def load_mask(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("L")) > 0


def resize_image(image: np.ndarray, shape_hw: tuple[int, int], interpolation: str) -> np.ndarray:
    height, width = shape_hw
    pil_mode = Image.Resampling.BOX if interpolation == "area" else Image.Resampling.BILINEAR
    uint8_image = np.clip(image * 255.0, 0, 255).astype(np.uint8)
    with Image.fromarray(uint8_image, mode="RGB") as pil_image:
        resized = pil_image.resize((width, height), pil_mode)
        return np.asarray(resized, dtype=np.float32) / 255.0


def align_render_to_gt(
    gt: np.ndarray,
    render: np.ndarray,
    allow_resize: bool,
    interpolation: str,
    render_path: Path | None = None,
) -> np.ndarray:
    if gt.shape == render.shape:
        return render
    if not allow_resize:
        suffix = f" for {render_path}" if render_path else ""
        raise ValueError(
            f"Image shape mismatch{suffix}: GT {gt.shape}, render {render.shape}. "
            "Set allow_resize: true only if the viewpoints and crop are known to match."
        )
    return resize_image(render, gt.shape[:2], interpolation)


def crop_border_array(array: np.ndarray, border_px: int) -> np.ndarray:
    if border_px <= 0:
        return array
    if array.shape[0] <= border_px * 2 or array.shape[1] <= border_px * 2:
        raise ValueError(f"ignore_border_px={border_px} is too large for image shape {array.shape}")
    return array[border_px:-border_px, border_px:-border_px, ...]


def crop_border_mask(mask: np.ndarray, border_px: int) -> np.ndarray:
    if border_px <= 0:
        return mask
    if mask.shape[0] <= border_px * 2 or mask.shape[1] <= border_px * 2:
        raise ValueError(f"ignore_border_px={border_px} is too large for mask shape {mask.shape}")
    return mask[border_px:-border_px, border_px:-border_px]


def mse_and_psnr(
    gt: np.ndarray,
    render: np.ndarray,
    mask: np.ndarray | None = None,
    max_value: float = 1.0,
) -> tuple[float, float, float]:
    if gt.shape != render.shape:
        raise ValueError(f"Image shape mismatch: GT {gt.shape}, render {render.shape}")
    diff_sq = (gt - render) ** 2
    if mask is not None:
        if mask.shape != gt.shape[:2]:
            raise ValueError(f"Mask shape mismatch: mask {mask.shape}, image {gt.shape[:2]}")
        valid = mask.astype(bool)
        valid_pixel_count = int(valid.sum())
        if valid_pixel_count == 0:
            raise ValueError("Mask has no valid pixels")
        mse = float(diff_sq[valid].mean())
        valid_ratio = valid_pixel_count / float(valid.size)
    else:
        mse = float(diff_sq.mean())
        valid_ratio = 1.0
    if mse == 0.0:
        return mse, math.inf, valid_ratio
    psnr = 10.0 * math.log10((max_value * max_value) / mse)
    return mse, psnr, valid_ratio


def format_float(value: float) -> str:
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    if math.isnan(value):
        return "nan"
    return f"{value:.10g}"


def parse_float(value: str) -> float:
    if value == "inf":
        return math.inf
    if value == "-inf":
        return -math.inf
    return float(value)


def method_columns(config: dict[str, Any]) -> list[str]:
    methods = config.get("methods")
    if not isinstance(methods, dict) or not methods:
        raise ValueError("Config must define a non-empty methods mapping")
    return [f"{method}_path" for method in methods]
