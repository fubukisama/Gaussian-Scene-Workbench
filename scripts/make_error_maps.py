from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

from psnr_common import (
    align_render_to_gt,
    crop_border_array,
    load_config,
    load_rgb_image,
    project_path,
    read_manifest,
)


def _error_to_rgb(error: np.ndarray, scale: float) -> np.ndarray:
    normalized = np.clip(error / max(scale, 1e-12), 0.0, 1.0)
    red = np.clip(normalized * 2.0, 0.0, 1.0)
    green = np.clip((normalized - 0.25) * 2.0, 0.0, 1.0)
    blue = np.clip((normalized - 0.75) * 4.0, 0.0, 1.0)
    return (np.stack([red, green, blue], axis=-1) * 255.0).astype(np.uint8)


def make_error_maps(config_path: Path) -> int:
    config = load_config(config_path)
    manifest_path = project_path(config, config.get("manifest_path") or "data/manifest.csv")
    output_dir = project_path(config, config.get("output_dir") or "reports")
    assert manifest_path is not None and output_dir is not None
    error_root = output_dir / "error_maps"
    error_root.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(manifest_path)
    methods = config.get("methods") or {}
    allow_resize = bool(config.get("allow_resize", False))
    interpolation = str(config.get("resize_interpolation", "area"))
    border_px = int(config.get("ignore_border_px", 0))

    for row in rows:
        image_id = row["image_id"]
        gt_full = load_rgb_image(Path(row["gt_path"]))
        errors: dict[str, np.ndarray] = {}
        for method in methods:
            render_value = row.get(f"{method}_path", "")
            if not render_value:
                raise FileNotFoundError(f"{image_id}: missing render path for {method} in manifest")
            render = load_rgb_image(Path(render_value))
            render = align_render_to_gt(gt_full, render, allow_resize, interpolation, Path(render_value))
            gt = crop_border_array(gt_full, border_px)
            render = crop_border_array(render, border_px)
            errors[method] = np.mean(np.abs(gt - render), axis=2)

        scale = max(float(error.max()) for error in errors.values())
        for method, error in errors.items():
            method_dir = error_root / method
            method_dir.mkdir(parents=True, exist_ok=True)
            Image.fromarray(_error_to_rgb(error, scale), mode="RGB").save(method_dir / f"{image_id}_error.png")

    print(f"Wrote error maps under {error_root}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate PSNR error maps.")
    parser.add_argument("--config", default="configs/psnr_eval.yaml", type=Path)
    args = parser.parse_args()
    return make_error_maps(args.config)


if __name__ == "__main__":
    raise SystemExit(main())
