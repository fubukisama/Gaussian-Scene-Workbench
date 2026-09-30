#
# Copyright (C) 2023, Inria
# GRAPHDECO research group, https://team.inria.fr/graphdeco
# All rights reserved.
#
# This software is free for non-commercial, research and evaluation use
# under the terms of the LICENSE.md file.
#
# For inquiries contact  george.drettakis@inria.fr
#

"""Native adapter of hbb1/2d-gaussian-splatting train.py.

Upstream: f3e3b9fa67bbd1c75e05167ff37391d8dab2a678 (2026-09-23).
License: licenses/2dgs-LICENSE.md. See docs/GENERATION_PIPELINES.md.
GSW changes: stable sampler, durable state, structured telemetry, bounded
observation previews, and no network viewer in a native supervised process.
The surfel renderer, losses, schedules and clone/split remain upstream code;
native density-control safety guards the final pruning pass.
"""
import os
import sys
import json
import time
import hashlib
from pathlib import Path

# Diagnostic images are produced by a supervised worker, never an interactive
# matplotlib window (including when TensorBoard is installed).
os.environ.setdefault("MPLBACKEND", "Agg")

# Import the configured external 2DGS modules, not 3DGS modules from elsewhere.
source_root = Path(os.environ.get("GSW_TWO_DGS_SOURCE", os.getcwd())).resolve()
if not (source_root / "scene" / "gaussian_model.py").is_file():
    raise RuntimeError("GSW_TWO_DGS_SOURCE must point to the official 2DGS source tree")
sys.path.insert(0, str(source_root))
sys.path.append(str(Path(__file__).resolve().parents[2]))
import torch
import numpy as np
from random import randint
from utils.loss_utils import l1_loss, ssim
from gaussian_renderer import render
from scene import Scene, GaussianModel
from utils.general_utils import safe_state
import uuid
from tqdm import tqdm
from utils.image_utils import psnr
from argparse import ArgumentParser, Namespace
from arguments import ModelParams, PipelineParams, OptimizationParams
from native.worker.training_checkpoint import (
    TrainingCheckpoint, training_identity, capture_state, restore_state, file_digest)
from native.worker.training_preview import TrainingPreviewPublisher
from native.worker.training_density_control import NativeDensityControl
try:
    from torch.utils.tensorboard import SummaryWriter
    TENSORBOARD_FOUND = True
except ImportError:
    TENSORBOARD_FOUND = False

def emit_gsw_event(prefix, payload):
    stream = getattr(sys, "__stdout__", None) or sys.stdout
    stream.write("{} {}\n".format(prefix, json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
                                if isinstance(payload, dict) else str(payload)))
    stream.flush()


def training(dataset, opt, pipe, testing_iterations, saving_iterations, checkpoint_iterations, checkpoint):
    if checkpoint:
        raise ValueError("Native resume cannot be combined with a legacy checkpoint")
    control = os.environ.get("GSW_NATIVE_TRAINING_CONTROL")
    if not control:
        raise RuntimeError("This entry point is for supervised native 2DGS jobs only")
    first_iter = 0
    tb_writer = prepare_output_and_logger(dataset)
    gaussians = GaussianModel(dataset.sh_degree)
    scene = Scene(dataset, gaussians)
    gaussians.training_setup(opt)

    bg_color = [1, 1, 1] if dataset.white_background else [0, 0, 0]
    background = torch.tensor(bg_color, dtype=torch.float32, device="cuda")

    iter_start = torch.cuda.Event(enable_timing = True)
    iter_end = torch.cuda.Event(enable_timing = True)

    ema_loss_for_log = 0.0
    ema_dist_for_log = 0.0
    ema_normal_for_log = 0.0
    started = time.monotonic()
    native_checkpoint = TrainingCheckpoint(scene.model_path, json.loads(control))
    # Do not resume against changed runtime implementations, even when the
    # tensors happen to have matching shapes.
    runtime_files = [source_root / name for name in (
        "scene/gaussian_model.py", "scene/__init__.py", "scene/dataset_readers.py",
        "arguments/__init__.py", "gaussian_renderer/__init__.py")]
    identity = hashlib.sha256((training_identity(dataset, opt, pipe) +
                              "".join(file_digest(path) for path in runtime_files)).encode()).hexdigest()
    cameras = scene.getTrainCameras()
    density_control = NativeDensityControl(torch, gaussians, scene.cameras_extent, "2dgs", emit_gsw_event)
    cameras.sort(key=lambda camera: (camera.image_name, camera.colmap_id))
    camera_names = [(camera.image_name, camera.colmap_id) for camera in cameras]
    viewpoint_indices = list(range(len(cameras)))
    if native_checkpoint.control.get("resume"):
        state = native_checkpoint.load(torch, identity)
        restore_state(torch, np, gaussians, opt, state, identity, camera_names, backend="2dgs")
        density_control.restore(state["density_control"])
        first_iter = state["iteration"]
        viewpoint_indices = state["pending_indices"]
        started -= state["elapsed"]
        ema_loss_for_log = state["ema_loss"]
        ema_dist_for_log = state["ema_depth"]
        ema_normal_for_log = state["ema_normal"]
        del state
    file_preview = TrainingPreviewPublisher(scene.model_path, emit_gsw_event)
    try:
        file_preview.publish_gaussians(gaussians, first_iter, initial=True)
    except Exception as error:
        emit_gsw_event("[gsw-preview-warning]", str(error))
        file_preview.close()
        file_preview = None
    paused = False
    progress_interval = max(1, min(100, opt.iterations // 100))

    progress_bar = tqdm(range(first_iter, opt.iterations), desc="Training progress")
    first_iter += 1
    for iteration in range(first_iter, opt.iterations + 1):

        iter_start.record()

        gaussians.update_learning_rate(iteration)

        # Every 1000 its we increase the levels of SH up to a maximum degree
        if iteration % 1000 == 0:
            gaussians.oneupSHdegree()

        # Pick a random Camera
        if not viewpoint_indices:
            viewpoint_indices = list(range(len(cameras)))
        viewpoint_cam = cameras[viewpoint_indices.pop(randint(0, len(viewpoint_indices)-1))]

        render_pkg = render(viewpoint_cam, gaussians, pipe, background)
        image, viewspace_point_tensor, visibility_filter, radii = render_pkg["render"], render_pkg["viewspace_points"], render_pkg["visibility_filter"], render_pkg["radii"]

        gt_image = viewpoint_cam.original_image.cuda()
        Ll1 = l1_loss(image, gt_image)
        loss = (1.0 - opt.lambda_dssim) * Ll1 + opt.lambda_dssim * (1.0 - ssim(image, gt_image))

        # regularization
        lambda_normal = opt.lambda_normal if iteration > 7000 else 0.0
        lambda_dist = opt.lambda_dist if iteration > 3000 else 0.0

        rend_dist = render_pkg["rend_dist"]
        rend_normal  = render_pkg['rend_normal']
        surf_normal = render_pkg['surf_normal']
        normal_error = (1 - (rend_normal * surf_normal).sum(dim=0))[None]
        normal_loss = lambda_normal * (normal_error).mean()
        dist_loss = lambda_dist * (rend_dist).mean()

        # loss
        total_loss = loss + dist_loss + normal_loss

        total_loss.backward()

        iter_end.record()

        with torch.no_grad():
            # Progress bar
            ema_loss_for_log = 0.4 * loss.item() + 0.6 * ema_loss_for_log
            ema_dist_for_log = 0.4 * dist_loss.item() + 0.6 * ema_dist_for_log
            ema_normal_for_log = 0.4 * normal_loss.item() + 0.6 * ema_normal_for_log
            if iteration == first_iter or iteration % progress_interval == 0 or iteration == opt.iterations:
                torch.cuda.synchronize()
                emit_gsw_event("[gsw-training-progress]", "{}/{}".format(iteration, opt.iterations))
                emit_gsw_event("[gsw-training-metrics]", {
                    "iteration": iteration, "total_iterations": opt.iterations,
                    "loss": total_loss.item(), "psnr": psnr(image, gt_image).mean().item(),
                    "gaussian_count": int(gaussians.get_xyz.shape[0]),
                    "iteration_milliseconds": iter_start.elapsed_time(iter_end),
                    "elapsed_seconds": time.monotonic() - started})


            if iteration % 10 == 0:
                loss_dict = {
                    "Loss": f"{ema_loss_for_log:.{5}f}",
                    "distort": f"{ema_dist_for_log:.{5}f}",
                    "normal": f"{ema_normal_for_log:.{5}f}",
                    "Points": f"{len(gaussians.get_xyz)}"
                }
                progress_bar.set_postfix(loss_dict)

                progress_bar.update(10)
            if iteration == opt.iterations:
                progress_bar.close()

            # Log and save
            if tb_writer is not None:
                tb_writer.add_scalar('train_loss_patches/dist_loss', ema_dist_for_log, iteration)
                tb_writer.add_scalar('train_loss_patches/normal_loss', ema_normal_for_log, iteration)

            training_report(tb_writer, iteration, Ll1, loss, l1_loss, iter_start.elapsed_time(iter_end), testing_iterations, scene, render, (pipe, background))
            if (iteration in saving_iterations):
                if file_preview is not None and iteration == opt.iterations:
                    file_preview.close()
                    file_preview = None
                print("\n[ITER {}] Saving Gaussians".format(iteration))
                scene.save(iteration)
                emit_checkpoint_preview(scene, iteration)


            # Densification
            if iteration < opt.densify_until_iter:
                gaussians.max_radii2D[visibility_filter] = torch.max(gaussians.max_radii2D[visibility_filter], radii[visibility_filter])
                gaussians.add_densification_stats(viewspace_point_tensor, visibility_filter)

                if iteration > opt.densify_from_iter and iteration % opt.densification_interval == 0:
                    size_threshold = 20 if iteration > opt.opacity_reset_interval else None
                    density_control.densify_and_prune(
                        gaussians, opt.densify_grad_threshold, opt.opacity_cull, size_threshold,
                        iteration, opt.opacity_reset_interval,
                        white_background=dataset.white_background, densify_from_iter=opt.densify_from_iter)

                if iteration % opt.opacity_reset_interval == 0 or (dataset.white_background and iteration == opt.densify_from_iter):
                    gaussians.reset_opacity()

            # Optimizer step
            if iteration < opt.iterations:
                gaussians.optimizer.step()
                gaussians.optimizer.zero_grad(set_to_none = True)

            if (iteration in checkpoint_iterations):
                print("\n[ITER {}] Saving Checkpoint".format(iteration))
                torch.save((gaussians.capture(), iteration), scene.model_path + "/chkpnt" + str(iteration) + ".pth")

            if iteration < opt.iterations and native_checkpoint.pause_requested():
                # AFTER optimizer and density updates: continue at iteration + 1.
                if file_preview is not None:
                    file_preview.close()
                    file_preview = None
                scene.save(iteration)
                state = capture_state(torch, np, gaussians, iteration, opt.iterations, identity,
                                      camera_names, viewpoint_indices, time.monotonic() - started,
                                      ema_loss_for_log, ema_dist_for_log, backend="2dgs")
                state["ema_normal"] = ema_normal_for_log
                state["density_control"] = density_control.capture()
                native_checkpoint.save(torch, state)
                emit_checkpoint_preview(scene, iteration)
                paused = True
                progress_bar.close()
                break
            if file_preview is not None:
                try:
                    file_preview.publish_gaussians(gaussians, iteration)
                except Exception as error:
                    emit_gsw_event("[gsw-preview-warning]", str(error))
                    file_preview.close()
                    file_preview = None

    if file_preview is not None:
        file_preview.close()
    if tb_writer:
        tb_writer.close()
    return paused


def emit_checkpoint_preview(scene, iteration):
    emit_gsw_event("[gsw-training-preview]", {
        "iteration": iteration, "point_cloud_path": str((Path(scene.model_path) /
        "point_cloud" / ("iteration_{}".format(iteration)) / "point_cloud.ply").resolve())})

def prepare_output_and_logger(args):
    if not args.model_path:
        if os.getenv('OAR_JOB_ID'):
            unique_str=os.getenv('OAR_JOB_ID')
        else:
            unique_str = str(uuid.uuid4())
        args.model_path = os.path.join("./output/", unique_str[0:10])

    # Set up output folder
    print("Output folder: {}".format(args.model_path))
    os.makedirs(args.model_path, exist_ok = True)
    with open(os.path.join(args.model_path, "cfg_args"), 'w') as cfg_log_f:
        cfg_log_f.write(str(Namespace(**vars(args))))

    # Create Tensorboard writer
    tb_writer = None
    if TENSORBOARD_FOUND:
        tb_writer = SummaryWriter(args.model_path)
    else:
        print("Tensorboard not available: not logging progress")
    return tb_writer

@torch.no_grad()
def training_report(tb_writer, iteration, Ll1, loss, l1_loss, elapsed, testing_iterations, scene : Scene, renderFunc, renderArgs):
    if tb_writer:
        tb_writer.add_scalar('train_loss_patches/reg_loss', Ll1.item(), iteration)
        tb_writer.add_scalar('train_loss_patches/total_loss', loss.item(), iteration)
        tb_writer.add_scalar('iter_time', elapsed, iteration)
        tb_writer.add_scalar('total_points', scene.gaussians.get_xyz.shape[0], iteration)

    # Report test and samples of training set
    if iteration in testing_iterations:
        torch.cuda.empty_cache()
        validation_configs = ({'name': 'test', 'cameras' : scene.getTestCameras()},
                              {'name': 'train', 'cameras' : [scene.getTrainCameras()[idx % len(scene.getTrainCameras())] for idx in range(5, 30, 5)]})

        for config in validation_configs:
            if config['cameras'] and len(config['cameras']) > 0:
                l1_test = 0.0
                psnr_test = 0.0
                for idx, viewpoint in enumerate(config['cameras']):
                    render_pkg = renderFunc(viewpoint, scene.gaussians, *renderArgs)
                    image = torch.clamp(render_pkg["render"], 0.0, 1.0).to("cuda")
                    gt_image = torch.clamp(viewpoint.original_image.to("cuda"), 0.0, 1.0)
                    if tb_writer and (idx < 5):
                        from utils.general_utils import colormap
                        depth = render_pkg["surf_depth"]
                        norm = depth.max()
                        depth = depth / norm
                        depth = colormap(depth.cpu().numpy()[0], cmap='turbo')
                        tb_writer.add_images(config['name'] + "_view_{}/depth".format(viewpoint.image_name), depth[None], global_step=iteration)
                        tb_writer.add_images(config['name'] + "_view_{}/render".format(viewpoint.image_name), image[None], global_step=iteration)

                        try:
                            rend_alpha = render_pkg['rend_alpha']
                            rend_normal = render_pkg["rend_normal"] * 0.5 + 0.5
                            surf_normal = render_pkg["surf_normal"] * 0.5 + 0.5
                            tb_writer.add_images(config['name'] + "_view_{}/rend_normal".format(viewpoint.image_name), rend_normal[None], global_step=iteration)
                            tb_writer.add_images(config['name'] + "_view_{}/surf_normal".format(viewpoint.image_name), surf_normal[None], global_step=iteration)
                            tb_writer.add_images(config['name'] + "_view_{}/rend_alpha".format(viewpoint.image_name), rend_alpha[None], global_step=iteration)

                            rend_dist = render_pkg["rend_dist"]
                            rend_dist = colormap(rend_dist.cpu().numpy()[0])
                            tb_writer.add_images(config['name'] + "_view_{}/rend_dist".format(viewpoint.image_name), rend_dist[None], global_step=iteration)
                        except:
                            pass

                        if iteration == testing_iterations[0]:
                            tb_writer.add_images(config['name'] + "_view_{}/ground_truth".format(viewpoint.image_name), gt_image[None], global_step=iteration)

                    l1_test += l1_loss(image, gt_image).mean().double()
                    psnr_test += psnr(image, gt_image).mean().double()

                psnr_test /= len(config['cameras'])
                l1_test /= len(config['cameras'])
                print("\n[ITER {}] Evaluating {}: L1 {} PSNR {}".format(iteration, config['name'], l1_test, psnr_test))
                if tb_writer:
                    tb_writer.add_scalar(config['name'] + '/loss_viewpoint - l1_loss', l1_test, iteration)
                    tb_writer.add_scalar(config['name'] + '/loss_viewpoint - psnr', psnr_test, iteration)

        torch.cuda.empty_cache()

if __name__ == "__main__":
    # Set up command line argument parser
    parser = ArgumentParser(description="Training script parameters")
    lp = ModelParams(parser)
    op = OptimizationParams(parser)
    pp = PipelineParams(parser)
    parser.add_argument('--ip', type=str, default="127.0.0.1")
    parser.add_argument('--port', type=int, default=6009)
    parser.add_argument('--detect_anomaly', action='store_true', default=False)
    parser.add_argument("--test_iterations", nargs="+", type=int, default=[7_000, 30_000])
    parser.add_argument("--save_iterations", nargs="+", type=int, default=[7_000, 30_000])
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--checkpoint_iterations", nargs="+", type=int, default=[])
    parser.add_argument("--start_checkpoint", type=str, default = None)
    args = parser.parse_args(sys.argv[1:])
    args.save_iterations.append(args.iterations)

    print("Optimizing " + args.model_path)

    # Initialize system state (RNG)
    safe_state(args.quiet)

    # Start GUI server, configure and run training
    torch.autograd.set_detect_anomaly(args.detect_anomaly)
    paused = training(lp.extract(args), op.extract(args), pp.extract(args), args.test_iterations, args.save_iterations, args.checkpoint_iterations, args.start_checkpoint)

    # All done
    print("\nTraining paused." if paused else "\nTraining complete.")
    sys.exit(75 if paused else 0)
