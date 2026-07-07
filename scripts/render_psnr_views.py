import argparse
import csv
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path


IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}


def image_key(path_or_name):
    path = Path(str(path_or_name).replace("\\", "/"))
    return path.stem.lower()


def list_input_images(source_dir, image_dir="images"):
    image_root = Path(source_dir) / image_dir
    if not image_root.exists():
        raise FileNotFoundError(f"Input image directory not found: {image_root}")
    images = [
        path
        for path in image_root.iterdir()
        if path.is_file() and path.suffix.lower() in IMAGE_EXTS
    ]
    return sorted(images, key=lambda path: path.name.lower())


def camera_name(camera):
    return str(getattr(camera, "image_name", "") or getattr(camera, "image_path", "") or "")


def select_cameras_for_images(cameras, image_paths, count):
    by_stem = {}
    by_name = {}
    for camera in cameras:
        name = camera_name(camera)
        if not name:
            continue
        by_name[Path(name.replace("\\", "/")).name.lower()] = camera
        by_stem[image_key(name)] = camera

    selected = []
    missing = []
    for image_path in image_paths[: max(1, int(count))]:
        name = image_path.name.lower()
        stem = image_key(image_path)
        camera = by_name.get(name) or by_stem.get(stem)
        if camera is None:
            missing.append(image_path.name)
            continue
        selected.append((image_path.stem, camera))
    return selected, missing


def psnr_from_mse(mse):
    if mse <= 0:
        return float("inf")
    return 20.0 * math.log10(1.0 / math.sqrt(float(mse)))


def tensor_psnr(rendered, gt):
    import torch

    if not torch.isfinite(rendered).all():
        bad = int((~torch.isfinite(rendered)).sum().item())
        raise RuntimeError(f"Rendered image contains {bad} non-finite value(s).")
    if not torch.isfinite(gt).all():
        bad = int((~torch.isfinite(gt)).sum().item())
        raise RuntimeError(f"Ground-truth image contains {bad} non-finite value(s).")
    rendered = rendered.detach().clamp(0.0, 1.0)
    gt = gt.detach().to(rendered.device).clamp(0.0, 1.0)
    mse = torch.mean((rendered - gt) ** 2).item()
    if not math.isfinite(mse):
        raise RuntimeError(f"PSNR MSE is not finite: {mse}")
    return psnr_from_mse(mse), mse


def save_tensor_image(tensor, path):
    import numpy as np
    from PIL import Image

    if not tensor.isfinite().all():
        raise RuntimeError(f"Cannot save non-finite image tensor: {path}")
    tensor = tensor.detach().clamp(0.0, 1.0).cpu()
    if tensor.ndim == 3 and tensor.shape[0] in (1, 3, 4):
        tensor = tensor.permute(1, 2, 0)
    array = (tensor.numpy() * 255.0 + 0.5).astype(np.uint8)
    if array.ndim == 3 and array.shape[2] == 1:
        array = array[:, :, 0]
    Image.fromarray(array).save(path)


def backend_root(backend, root, two_dgs_dir=None):
    if backend == "2dgs":
        return Path(two_dgs_dir or os.environ.get("TWO_DGS_DIR") or Path.home() / "Documents" / "2dgs")
    return Path(root) / "gaussian-splatting"


def load_backend_args(backend_dir, source, model, iteration, eval_width):
    from argparse import ArgumentParser
    from arguments import ModelParams, PipelineParams, get_combined_args

    parser = ArgumentParser(description="PSNR render arguments")
    model_params = ModelParams(parser, sentinel=True)
    pipeline_params = PipelineParams(parser)
    parser.add_argument("--iteration", default=-1, type=int)
    parser.add_argument("--quiet", action="store_true")

    old_argv = sys.argv
    sys.argv = [
        old_argv[0],
        "-s",
        str(source),
        "-m",
        str(model),
        "--iteration",
        str(iteration),
        "--quiet",
    ]
    if int(eval_width or 0) > 0:
        sys.argv.extend(["--resolution", str(int(eval_width))])
    try:
        args = get_combined_args(parser)
    finally:
        sys.argv = old_argv
    return model_params.extract(args), pipeline_params.extract(args), int(args.iteration)


def sanitize_gaussians_for_analysis(gaussians, args):
    import torch

    if args.backend != "3dgs":
        return {"enabled": False}
    xyz = gaussians._xyz
    count_before = int(xyz.shape[0])
    if count_before == 0:
        return {"enabled": False, "count_before": 0, "count_after": 0}

    mask = torch.isfinite(gaussians._xyz).all(dim=1)
    mask &= torch.isfinite(gaussians._features_dc).flatten(1).all(dim=1)
    mask &= torch.isfinite(gaussians._features_rest).flatten(1).all(dim=1)
    mask &= torch.isfinite(gaussians._opacity).all(dim=1)
    mask &= torch.isfinite(gaussians._scaling).all(dim=1)
    mask &= torch.isfinite(gaussians._rotation).all(dim=1)

    opacity = gaussians.get_opacity.squeeze()
    if args.prune_opacity > 0:
        mask &= opacity >= float(args.prune_opacity)
    scale_max = gaussians.get_scaling.max(dim=1).values
    if args.max_scale > 0:
        mask &= scale_max <= float(args.max_scale)

    if args.max_gaussians > 0 and int(mask.sum().item()) > args.max_gaussians:
        candidate = torch.nonzero(mask, as_tuple=False).squeeze(1)
        scores = opacity[candidate]
        keep_count = min(int(args.max_gaussians), int(candidate.numel()))
        top = torch.topk(scores, keep_count, largest=True, sorted=False).indices
        top_mask = torch.zeros_like(mask)
        top_mask[candidate[top]] = True
        mask = top_mask

    count_after = int(mask.sum().item())
    if count_after <= 0:
        raise RuntimeError("Analysis Gaussian filter removed all points.")
    if count_after == count_before:
        return {
            "enabled": False,
            "count_before": count_before,
            "count_after": count_after,
            "prune_opacity": float(args.prune_opacity),
            "max_scale": float(args.max_scale),
            "max_gaussians": int(args.max_gaussians),
        }

    gaussians._xyz = torch.nn.Parameter(gaussians._xyz[mask].detach().requires_grad_(False))
    gaussians._features_dc = torch.nn.Parameter(gaussians._features_dc[mask].detach().requires_grad_(False))
    gaussians._features_rest = torch.nn.Parameter(gaussians._features_rest[mask].detach().requires_grad_(False))
    gaussians._opacity = torch.nn.Parameter(gaussians._opacity[mask].detach().requires_grad_(False))
    gaussians._scaling = torch.nn.Parameter(gaussians._scaling[mask].detach().requires_grad_(False))
    gaussians._rotation = torch.nn.Parameter(gaussians._rotation[mask].detach().requires_grad_(False))
    gaussians.max_radii2D = torch.zeros((count_after,), device=gaussians._xyz.device)
    print(f"Analysis Gaussian filter: {count_before} -> {count_after}", flush=True)
    return {
        "enabled": True,
        "count_before": count_before,
        "count_after": count_after,
        "prune_opacity": float(args.prune_opacity),
        "max_scale": float(args.max_scale),
        "max_gaussians": int(args.max_gaussians),
    }


def render_selected_views(args):
    import torch

    root = Path(args.root).resolve()
    source = Path(args.source).resolve()
    model = Path(args.model).resolve()
    output_dir = Path(args.output_dir).resolve()
    render_dir = output_dir / "renders"
    gt_dir = output_dir / "gt"
    render_dir.mkdir(parents=True, exist_ok=True)
    gt_dir.mkdir(parents=True, exist_ok=True)

    backend_dir = backend_root(args.backend, root, args.two_dgs_dir).resolve()
    if not backend_dir.exists():
        raise FileNotFoundError(f"{args.backend.upper()} source directory not found: {backend_dir}")
    sys.path.insert(0, str(backend_dir))

    from scene import Scene
    from gaussian_renderer import GaussianModel, render

    dataset, pipeline, iteration = load_backend_args(backend_dir, source, model, args.iteration, args.eval_width)
    if args.backend == "3dgs" and args.python_sh:
        # Keep PSNR on the official renderer path while avoiding unstable SH
        # evaluation inside the Windows CUDA rasterizer for large/cropped scenes.
        pipeline.convert_SHs_python = True
    with torch.no_grad():
        if args.backend == "3dgs":
            gaussians = GaussianModel(dataset.sh_degree, getattr(dataset, "optimizer_type", "default"))
        else:
            gaussians = GaussianModel(dataset.sh_degree)
        scene = Scene(dataset, gaussians, load_iteration=iteration, shuffle=False)
        filter_info = sanitize_gaussians_for_analysis(gaussians, args)
        loaded_iteration = int(getattr(scene, "loaded_iter", iteration))
        cameras = scene.getTrainCameras()
        image_paths = list_input_images(source, dataset.images or args.image_dir)
        selected, missing = select_cameras_for_images(cameras, image_paths, args.count)
        if args.view_index is not None:
            view_index = int(args.view_index)
            if view_index < 0 or view_index >= len(selected):
                raise RuntimeError(f"Requested view index {view_index} but only {len(selected)} matched view(s).")
            selected = [selected[view_index]]
        if not selected:
            raise RuntimeError("No train cameras matched the first input images.")

        bg_color = [1, 1, 1] if dataset.white_background else [0, 0, 0]
        background = torch.tensor(bg_color, dtype=torch.float32, device="cuda")
        rows = []
        psnr_values = []
        for index, (stem, camera) in enumerate(selected, 1):
            if args.backend == "3dgs":
                rendered = render(
                    camera,
                    gaussians,
                    pipeline,
                    background,
                    use_trained_exp=getattr(dataset, "train_test_exp", False),
                )["render"]
            else:
                rendered = render(camera, gaussians, pipeline, background)["render"]
            gt = camera.original_image[0:3, :, :]
            if getattr(dataset, "train_test_exp", False):
                rendered = rendered[..., rendered.shape[-1] // 2:]
                gt = gt[..., gt.shape[-1] // 2:]
            psnr, mse = tensor_psnr(rendered, gt)
            psnr_values.append(psnr)
            render_path = render_dir / f"{stem}.png"
            gt_path = gt_dir / f"{stem}.png"
            save_tensor_image(rendered, render_path)
            save_tensor_image(gt, gt_path)
            rows.append({
                "index": index,
                "image": stem,
                "camera": camera_name(camera),
                "psnr": psnr,
                "mse": mse,
                "render": str(render_path),
                "gt": str(gt_path),
            })
            print(f"[{index}/{len(selected)}] {stem}: PSNR {psnr:.4f} dB", flush=True)

    finite_psnr = [value for value in psnr_values if math.isfinite(value)]
    average_psnr = sum(finite_psnr) / len(finite_psnr) if finite_psnr else float("inf")
    result = {
        "backend": args.backend,
        "source": str(source),
        "model": str(model),
        "iteration": loaded_iteration,
        "requested_count": int(args.count),
        "rendered_count": len(rows),
        "eval_width": int(args.eval_width or 0),
        "analysis_filter": filter_info,
        "render_pipeline": {
            "python_sh": bool(args.python_sh),
            "python_covariance": bool(getattr(pipeline, "compute_cov3D_python", False)),
        },
        "average_psnr": average_psnr,
        "missing_images": missing,
        "output_dir": str(output_dir),
        "renders_dir": str(render_dir),
        "gt_dir": str(gt_dir),
        "rows": rows,
    }

    with open(output_dir / "psnr_results.csv", "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["index", "image", "camera", "psnr", "mse", "render", "gt"])
        writer.writeheader()
        writer.writerows(rows)
    with open(output_dir / "psnr_results.json", "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    print(f"Average PSNR: {average_psnr:.4f} dB over {len(rows)} image(s)", flush=True)
    print(f"PSNR output: {output_dir}", flush=True)
    return result


def safe_worker_command(args, view_index, output_dir):
    command = [
        sys.executable,
        str(Path(__file__).resolve()),
        "--backend",
        args.backend,
        "--root",
        args.root,
        "--source",
        args.source,
        "--model",
        args.model,
        "--iteration",
        str(args.iteration),
        "--count",
        str(args.count),
        "--eval-width",
        str(args.eval_width),
        "--image-dir",
        args.image_dir,
        "--output-dir",
        str(output_dir),
        "--prune-opacity",
        str(args.prune_opacity),
        "--max-scale",
        str(args.max_scale),
        "--max-gaussians",
        str(args.max_gaussians),
        "--view-index",
        str(view_index),
    ]
    if not args.python_sh:
        command.append("--no-python-sh")
    if args.two_dgs_dir:
        command.extend(["--two-dgs-dir", args.two_dgs_dir])
    return command


def copy_row_assets(row, output_dir):
    render_dir = output_dir / "renders"
    gt_dir = output_dir / "gt"
    render_dir.mkdir(parents=True, exist_ok=True)
    gt_dir.mkdir(parents=True, exist_ok=True)
    render_src = Path(row["render"])
    gt_src = Path(row["gt"])
    render_dst = render_dir / render_src.name
    gt_dst = gt_dir / gt_src.name
    shutil.copy2(render_src, render_dst)
    shutil.copy2(gt_src, gt_dst)
    row["render"] = str(render_dst)
    row["gt"] = str(gt_dst)


def run_safe_mode(args):
    output_dir = Path(args.output_dir).resolve()
    work_dir = output_dir / "_safe_views"
    if output_dir.exists():
        shutil.rmtree(output_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    failures = []
    for view_index in range(max(1, int(args.count))):
        child_dir = work_dir / f"view_{view_index:05d}"
        command = safe_worker_command(args, view_index, child_dir)
        print(f"Safe PSNR view {view_index + 1}/{args.count}: starting", flush=True)
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if completed.stdout:
            print(completed.stdout.rstrip(), flush=True)
        result_path = child_dir / "psnr_results.json"
        if completed.returncode != 0 or not result_path.exists():
            failures.append({
                "view_index": view_index,
                "returncode": completed.returncode,
                "tail": "\n".join((completed.stdout or "").splitlines()[-40:]),
            })
            print(f"Safe PSNR view {view_index + 1}: failed and skipped", flush=True)
            continue
        child_result = json.loads(result_path.read_text(encoding="utf-8"))
        for row in child_result.get("rows", []):
            copy_row_assets(row, output_dir)
            row["source_view_index"] = view_index
            rows.append(row)

    if not rows:
        failure_path = output_dir / "psnr_failures.json"
        failure_path.parent.mkdir(parents=True, exist_ok=True)
        failure_path.write_text(json.dumps({"failures": failures}, ensure_ascii=False, indent=2), encoding="utf-8")
        raise RuntimeError(f"All PSNR safe-mode views failed. Details: {failure_path}")

    finite_psnr = [row["psnr"] for row in rows if math.isfinite(row["psnr"])]
    average_psnr = sum(finite_psnr) / len(finite_psnr) if finite_psnr else float("inf")
    result = {
        "backend": args.backend,
        "source": str(Path(args.source).resolve()),
        "model": str(Path(args.model).resolve()),
        "iteration": int(args.iteration),
        "requested_count": int(args.count),
        "rendered_count": len(rows),
        "failed_count": len(failures),
        "eval_width": int(args.eval_width or 0),
        "safe_mode": True,
        "analysis_filter": {
            "prune_opacity": float(args.prune_opacity),
            "max_scale": float(args.max_scale),
            "max_gaussians": int(args.max_gaussians),
        },
        "render_pipeline": {
            "python_sh": bool(args.python_sh),
        },
        "average_psnr": average_psnr,
        "failures": failures,
        "output_dir": str(output_dir),
        "renders_dir": str(output_dir / "renders"),
        "gt_dir": str(output_dir / "gt"),
        "rows": rows,
    }
    with open(output_dir / "psnr_results.csv", "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["index", "image", "camera", "psnr", "mse", "render", "gt", "source_view_index"])
        writer.writeheader()
        writer.writerows(rows)
    with open(output_dir / "psnr_results.json", "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    print(f"Average PSNR: {average_psnr:.4f} dB over {len(rows)} image(s); skipped {len(failures)} failed view(s)", flush=True)
    print(f"PSNR output: {output_dir}", flush=True)
    return result


def parse_args():
    parser = argparse.ArgumentParser(description="Render fixed train cameras and compute PSNR.")
    parser.add_argument("--backend", choices=["3dgs", "2dgs"], required=True)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--two-dgs-dir", default=None)
    parser.add_argument("--source", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--iteration", type=int, required=True)
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--eval-width", type=int, default=0, help="Target evaluation image width. Use 0 to keep the model cfg resolution.")
    parser.add_argument("--image-dir", default="images")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--safe-mode", action="store_true", help="Render each view in a child process and skip views that crash the rasterizer.")
    parser.add_argument("--prune-opacity", type=float, default=0.0, help="Analysis-only 3DGS opacity pruning threshold after sigmoid.")
    parser.add_argument("--max-scale", type=float, default=0.0, help="Analysis-only 3DGS max activated scale threshold.")
    parser.add_argument("--max-gaussians", type=int, default=0, help="Analysis-only 3DGS cap by highest opacity.")
    parser.add_argument("--no-python-sh", dest="python_sh", action="store_false", help=argparse.SUPPRESS)
    parser.set_defaults(python_sh=True)
    parser.add_argument("--view-index", type=int, default=None, help=argparse.SUPPRESS)
    return parser.parse_args()


def main():
    args = parse_args()
    result = run_safe_mode(args) if args.safe_mode and args.view_index is None else render_selected_views(args)
    return 0 if result["rendered_count"] > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
