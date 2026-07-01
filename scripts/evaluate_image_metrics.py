from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import numpy as np

from psnr_common import (
    align_render_to_gt,
    crop_border_array,
    crop_border_mask,
    format_float,
    load_config,
    load_mask,
    load_rgb_image,
    mse_and_psnr,
    parse_float,
    project_path,
    read_manifest,
)


def _summary(values: list[dict[str, str]]) -> list[dict[str, str]]:
    by_method: dict[str, list[dict[str, str]]] = {}
    for row in values:
        by_method.setdefault(row["method"], []).append(row)

    summary_rows: list[dict[str, str]] = []
    for method, rows in sorted(by_method.items()):
        psnrs = np.asarray([parse_float(row["psnr"]) for row in rows], dtype=np.float64)
        mses = np.asarray([float(row["mse"]) for row in rows], dtype=np.float64)
        finite_psnrs = psnrs[np.isfinite(psnrs)]
        if finite_psnrs.size:
            std_psnr = float(np.std(finite_psnrs, ddof=1)) if finite_psnrs.size > 1 else 0.0
            median_psnr = float(np.median(finite_psnrs))
        else:
            std_psnr = math.nan
            median_psnr = math.inf if np.all(np.isposinf(psnrs)) else math.nan
        summary_rows.append(
            {
                "method": method,
                "n": str(len(rows)),
                "mean_psnr": format_float(float(np.mean(psnrs))),
                "std_psnr": format_float(std_psnr),
                "median_psnr": format_float(median_psnr),
                "min_psnr": format_float(float(np.min(psnrs))),
                "max_psnr": format_float(float(np.max(psnrs))),
                "mean_mse": format_float(float(np.mean(mses))),
            }
        )
    return summary_rows


def evaluate(config_path: Path) -> int:
    config = load_config(config_path)
    manifest_path = project_path(config, config.get("manifest_path") or "data/manifest.csv")
    output_dir = project_path(config, config.get("output_dir") or "reports")
    assert manifest_path is not None and output_dir is not None
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(manifest_path)
    methods = config.get("methods") or {}
    allow_resize = bool(config.get("allow_resize", False))
    interpolation = str(config.get("resize_interpolation", "area"))
    border_px = int(config.get("ignore_border_px", 0))
    use_mask = bool(config.get("use_mask", False))
    max_value = float(config.get("psnr_max_value", 1.0))

    detail_rows: list[dict[str, str]] = []
    for row in rows:
        image_id = row["image_id"]
        gt_path = Path(row["gt_path"])
        gt = crop_border_array(load_rgb_image(gt_path), border_px)

        mask = None
        if use_mask:
            if not row.get("mask_path"):
                raise FileNotFoundError(f"{image_id}: manifest row has no mask_path")
            mask = crop_border_mask(load_mask(Path(row["mask_path"])), border_px)
            if mask.shape != gt.shape[:2]:
                raise ValueError(f"{image_id}: mask shape {mask.shape} does not match GT {gt.shape[:2]}")

        for method in methods:
            render_value = row.get(f"{method}_path", "")
            if not render_value:
                raise FileNotFoundError(f"{image_id}: missing render path for {method} in manifest")
            render_path = Path(render_value)
            render = load_rgb_image(render_path)
            render = align_render_to_gt(
                load_rgb_image(gt_path),
                render,
                allow_resize=allow_resize,
                interpolation=interpolation,
                render_path=render_path,
            )
            render = crop_border_array(render, border_px)
            mse, psnr, valid_ratio = mse_and_psnr(gt, render, mask=mask, max_value=max_value)
            detail_rows.append(
                {
                    "method": method,
                    "image_id": image_id,
                    "psnr": format_float(psnr),
                    "mse": format_float(mse),
                    "width": str(gt.shape[1]),
                    "height": str(gt.shape[0]),
                    "used_mask": str(mask is not None).lower(),
                    "valid_pixel_ratio": format_float(valid_ratio),
                    "gt_path": str(gt_path),
                    "render_path": str(render_path),
                }
            )

    detail_path = output_dir / "detail_metrics.csv"
    summary_path = output_dir / "summary_metrics.csv"
    detail_fields = [
        "method",
        "image_id",
        "psnr",
        "mse",
        "width",
        "height",
        "used_mask",
        "valid_pixel_ratio",
        "gt_path",
        "render_path",
    ]
    summary_fields = [
        "method",
        "n",
        "mean_psnr",
        "std_psnr",
        "median_psnr",
        "min_psnr",
        "max_psnr",
        "mean_mse",
    ]
    with detail_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=detail_fields)
        writer.writeheader()
        writer.writerows(detail_rows)
    with summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=summary_fields)
        writer.writeheader()
        writer.writerows(_summary(detail_rows))

    print(f"Wrote {detail_path}")
    print(f"Wrote {summary_path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate image PSNR metrics.")
    parser.add_argument("--config", default="configs/psnr_eval.yaml", type=Path)
    args = parser.parse_args()
    return evaluate(args.config)


if __name__ == "__main__":
    raise SystemExit(main())
