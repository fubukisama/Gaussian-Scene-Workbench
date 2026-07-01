from __future__ import annotations

import argparse
import csv
from pathlib import Path

from PIL import Image, ImageDraw

from psnr_common import load_config, project_path, read_manifest


def _load_psnr_lookup(detail_path: Path) -> dict[tuple[str, str], str]:
    if not detail_path.exists():
        return {}
    with detail_path.open("r", newline="", encoding="utf-8") as handle:
        return {
            (row["image_id"], row["method"]): row["psnr"]
            for row in csv.DictReader(handle)
        }


def _fit(image: Image.Image, max_width: int) -> Image.Image:
    if image.width <= max_width:
        return image.copy()
    height = int(round(image.height * (max_width / image.width)))
    return image.resize((max_width, height), Image.Resampling.LANCZOS)


def _panel(path: Path, label: str, max_width: int) -> Image.Image:
    with Image.open(path) as image:
        body = _fit(image.convert("RGB"), max_width)
    label_height = 34
    panel = Image.new("RGB", (body.width, body.height + label_height), "white")
    panel.paste(body, (0, label_height))
    draw = ImageDraw.Draw(panel)
    draw.rectangle((0, 0, panel.width, label_height), fill=(20, 20, 20))
    draw.text((8, 9), label, fill="white")
    return panel


def make_grids(config_path: Path) -> int:
    config = load_config(config_path)
    manifest_path = project_path(config, config.get("manifest_path") or "data/manifest.csv")
    output_dir = project_path(config, config.get("output_dir") or "reports")
    assert manifest_path is not None and output_dir is not None
    grid_dir = output_dir / "comparison_grids"
    grid_dir.mkdir(parents=True, exist_ok=True)

    detail_path = output_dir / "detail_metrics.csv"
    psnr_lookup = _load_psnr_lookup(detail_path)
    methods = config.get("methods") or {}
    max_width = 360

    for row in read_manifest(manifest_path):
        image_id = row["image_id"]
        panels = [_panel(Path(row["gt_path"]), "GT", max_width)]
        for method in methods:
            render_value = row.get(f"{method}_path", "")
            if not render_value:
                raise FileNotFoundError(f"{image_id}: missing render path for {method} in manifest")
            psnr = psnr_lookup.get((image_id, method))
            suffix = f" | PSNR {psnr} dB" if psnr else ""
            panels.append(_panel(Path(render_value), f"{method}{suffix}", max_width))

        width = sum(panel.width for panel in panels)
        height = max(panel.height for panel in panels)
        grid = Image.new("RGB", (width, height), "white")
        x = 0
        for panel in panels:
            grid.paste(panel, (x, 0))
            x += panel.width
        grid.save(grid_dir / f"{image_id}_grid.png")

    print(f"Wrote comparison grids under {grid_dir}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate PSNR comparison grids.")
    parser.add_argument("--config", default="configs/psnr_eval.yaml", type=Path)
    args = parser.parse_args()
    return make_grids(args.config)


if __name__ == "__main__":
    raise SystemExit(main())
