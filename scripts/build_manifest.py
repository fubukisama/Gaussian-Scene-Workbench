from __future__ import annotations

import argparse
import csv
from pathlib import Path

from psnr_common import image_extensions, index_images_by_stem, load_config, project_path


def build_manifest(config_path: Path, allow_missing: bool) -> int:
    config = load_config(config_path)
    extensions = image_extensions(config)
    gt_dir = project_path(config, config.get("gt_dir"))
    if gt_dir is None:
        raise ValueError("gt_dir is required")

    manifest_path = project_path(config, config.get("manifest_path") or "data/manifest.csv")
    assert manifest_path is not None
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    gt_index = index_images_by_stem(gt_dir, extensions)
    if not gt_index:
        raise FileNotFoundError(f"No GT images found in {gt_dir}")

    use_mask = bool(config.get("use_mask", False))
    mask_dir = project_path(config, config.get("mask_dir"))
    mask_index = index_images_by_stem(mask_dir, extensions) if use_mask and mask_dir else {}
    if use_mask and mask_dir is None:
        raise ValueError("use_mask is true, but mask_dir is not set")

    methods = config.get("methods") or {}
    method_indexes: dict[str, dict[str, Path]] = {}
    for method, directory in methods.items():
        method_dir = project_path(config, directory)
        if method_dir is None:
            raise ValueError(f"Method {method} has an empty directory")
        method_indexes[method] = index_images_by_stem(method_dir, extensions)

    rows: list[dict[str, str]] = []
    missing: list[str] = []
    fieldnames = ["image_id", "gt_path", "mask_path"] + [f"{method}_path" for method in methods]

    for image_id, gt_path in sorted(gt_index.items()):
        row = {
            "image_id": image_id,
            "gt_path": str(gt_path),
            "mask_path": "",
        }
        if use_mask:
            mask_path = mask_index.get(image_id)
            if mask_path is None:
                missing.append(f"{image_id}: missing mask in {mask_dir}")
            else:
                row["mask_path"] = str(mask_path)

        for method, index in method_indexes.items():
            render_path = index.get(image_id)
            column = f"{method}_path"
            if render_path is None:
                row[column] = ""
                missing.append(f"{image_id}: missing {method} render")
            else:
                row[column] = str(render_path)
        rows.append(row)

    if missing and not allow_missing:
        preview = "\n".join(missing[:30])
        extra = f"\n... and {len(missing) - 30} more missing entries" if len(missing) > 30 else ""
        raise FileNotFoundError(
            "Manifest cannot be built because required files are missing.\n"
            f"{preview}{extra}\n"
            "Use --allow-missing only to create an incomplete manifest for inspection."
        )

    with manifest_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(f"Wrote {len(rows)} rows to {manifest_path}")
    if missing:
        print(f"Warning: manifest has {len(missing)} missing entries")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Build PSNR image-pair manifest.")
    parser.add_argument("--config", default="configs/psnr_eval.yaml", type=Path)
    parser.add_argument("--allow-missing", action="store_true")
    args = parser.parse_args()
    return build_manifest(args.config, args.allow_missing)


if __name__ == "__main__":
    raise SystemExit(main())
