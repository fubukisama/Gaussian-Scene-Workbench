"""Validate the complete runtime and dataset required by desktop 3DGS training."""

import argparse
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}


def configure_stdio():
    try:
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8", line_buffering=True)
    except (AttributeError, ValueError):
        pass


def load_compute_module(backend_root):
    root = Path(backend_root).resolve()
    server_path = root / "native" / "backend" / "compute.py"
    if not server_path.is_file():
        raise FileNotFoundError("native/backend/compute.py is missing: {}".format(server_path))
    # Loading a file by spec does not add its package root to the search path.
    # Match the worker's selected-backend imports, independent of launch cwd.
    sys.path.insert(0, str(root))
    spec = importlib.util.spec_from_file_location("gsw_training_preflight_compute", str(server_path))
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load {}".format(server_path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def count_images(dataset):
    for candidate in (dataset / "images", dataset / "input", dataset):
        if not candidate.is_dir():
            continue
        count = sum(
            1 for item in candidate.iterdir()
            if item.is_file() and item.suffix.lower() in IMAGE_EXTENSIONS
        )
        if count:
            return count, candidate
    return 0, dataset / "images"


def has_reconstruction(dataset):
    sparse = dataset / "sparse" / "0"
    binary = all((sparse / name).is_file() for name in (
        "cameras.bin", "images.bin", "points3D.bin"
    ))
    text = all((sparse / name).is_file() for name in (
        "cameras.txt", "images.txt", "points3D.txt"
    ))
    return binary or text or (dataset / "transforms_train.json").is_file()


def policy_blocked(detail):
    lowered = str(detail or "").lower()
    return (
        "winerror 4551" in lowered
        or "application control policy" in lowered
        or "application control" in lowered and "blocked" in lowered
        or "code integrity" in lowered
        or "did not meet the enterprise signing level" in lowered
        or "アプリケーション制御ポリシー" in lowered
        or "应用控制策略" in lowered
    )


def policy_exit_code(returncode):
    if returncode is None:
        return False
    return (int(returncode) & 0xFFFFFFFF) in {0xC0E90002, 0xC0000428}


def runtime_probe_code(backend="3dgs"):
    return "\n".join((
        "import json",
        "import torch",
        "import cv2",
        "from PIL import Image",
        "import diff_surfel_rasterization" if backend == "2dgs" else "import diff_gaussian_rasterization",
        "from scene import Scene, GaussianModel" if backend == "2dgs" else "import math",
        "from gaussian_renderer import render" if backend == "2dgs" else "import math",
        "from simple_knn._C import distCUDA2",
        "if not torch.cuda.is_available():",
        "    raise RuntimeError('PyTorch cannot access a CUDA device')",
        "print(json.dumps({'torch': torch.__version__, 'cuda': torch.version.cuda, "
        "'device': torch.cuda.get_device_name(0)}))",
    ))


def probe_training_environment(backend_root, dataset_path, backend="3dgs", run_colmap=False,
                               pose_policy=None, camera_dataset_path=None, expected_identity=None,
                               resolution_policy="preserve", resolution=1):
    root = Path(backend_root).resolve()
    dataset = Path(dataset_path).resolve()
    report = {
        "version": 1,
        "ready": False,
        "backend": backend,
        "dataset": str(dataset),
        "python": str(Path(sys.executable).resolve()),
        "imageCount": 0,
        "hasReconstruction": False,
        "policyBlocked": False,
        "resolutionPolicy": resolution_policy,
        "resolution": resolution,
    }
    try:
        if resolution_policy not in ("preserve", "manual") or resolution not in (1, 2, 4, 8):
            raise ValueError("Unsupported explicit training resolution policy")
        if resolution_policy == "preserve" and resolution != 1:
            raise ValueError("Preserve resolution policy cannot silently downsample images")
        if not dataset.is_dir():
            raise FileNotFoundError("训练数据集不存在：{}".format(dataset))
        server = load_compute_module(root)
        from native.worker.training_reconstruction_summary import summarize_reconstruction
        reconstruction = summarize_reconstruction(dataset, run_colmap, pose_policy,
                                                    camera_dataset_path, expected_identity)
        report["reconstructionSummary"] = reconstruction
        locked = reconstruction["sourceKind"] == "canonical"
        canonical_source = reconstruction["sourceKind"] in ("canonical", "canonical_solve")
        if canonical_source:
            report["imageCount"] = reconstruction["inputImages"]
            report["imagesPath"] = reconstruction["trainingImagesDirectory"]
            report["cameraDataset"] = reconstruction["cameraDataset"]
        else:
            image_count, images_path = count_images(dataset)
            report["imageCount"] = image_count
            report["imagesPath"] = str(images_path)
            report["hasReconstruction"] = has_reconstruction(dataset)
            if image_count <= 0:
                raise RuntimeError("训练数据集中没有可用图像：{}".format(images_path))
        # Current reconstruction is deliberately distinct from a reusable
        # cache. Full optimizer resume requires the former; this read-only
        # environment probe never restores a cache or regenerates alignment.
        report["hasReconstruction"] = (reconstruction["usable"] if locked else False if canonical_source
                                        else server.dataset_has_recognized_training_scene(dataset))
        report["colmapRequired"] = reconstruction["effectiveRunColmap"]
        report["reconstructionRepairRequired"] = reconstruction["decision"] == "repair_required"
        if reconstruction["sourceKind"] == "colmap":
            report["reconstructionQuality"] = reconstruction
        if reconstruction["blocked"]:
            report["sourceBlocked"] = True
            report["errorCode"] = (reconstruction["reasons"][0] if canonical_source and reconstruction["reasons"]
                                   else "reconstruction_source_unusable")
            raise RuntimeError("reconstruction_source_unusable: " + reconstruction["decision"])
        python_path = Path(server.training_python(backend)).resolve()
        report["python"] = str(python_path)
        if not python_path.is_file():
            raise FileNotFoundError("训练 Python 不存在：{}".format(python_path))
        gaussian_dir = Path(server.TWO_DGS_DIR if backend == "2dgs" else server.GAUSSIAN_DIR)
        if not (gaussian_dir / "train.py").is_file():
            raise FileNotFoundError("训练源码不存在：{}".format(gaussian_dir / "train.py"))
        colmap = None
        if not locked:
            colmap = server.colmap_executable()
            if not colmap or not Path(colmap).is_file():
                raise FileNotFoundError("COLMAP 不可用，无法完成训练数据准备。")
            report["colmap"] = str(Path(colmap).resolve())
        process_environment = server.training_env(backend)
        if report["colmapRequired"]:
            colmap_probe = subprocess.run(
                [str(colmap), "-h"],
                cwd=str(Path(colmap).parent),
                env=process_environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                encoding="utf-8",
                errors="replace",
                timeout=20,
                check=False,
            )
            if colmap_probe.returncode != 0:
                detail = (colmap_probe.stderr or colmap_probe.stdout or "").strip()
                report["policyBlocked"] = (
                    policy_blocked(detail) or policy_exit_code(colmap_probe.returncode)
                )
                suffix = "（Windows 企业应用控制已拦截该程序）" if report["policyBlocked"] else ""
                raise RuntimeError(
                    "COLMAP 无法启动{}：退出码 {}{}".format(
                        suffix,
                        colmap_probe.returncode,
                        "\n\n{}".format(detail) if detail else "",
                    )
                )

        probe_code = runtime_probe_code(backend)
        if locked and backend == '3dgs':
            process_environment['GSW_POSE_POLICY'] = 'locked'
            process_environment['GSW_CAMERA_DATASET'] = str(camera_dataset_path or dataset / 'camera_dataset.json')
            probe_code = ('import sys; sys.path.insert(0,' + repr(str(root)) + '); '
                          'from native.worker.camera_rasterizer_runtime import configure_camera_rasterizer; '
                          'configure_camera_rasterizer(' + repr(str(root)) + ');\n') + probe_code
        completed = subprocess.run(
            [str(python_path), "-B", "-c", probe_code],
            cwd=str(gaussian_dir),
            env=process_environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            encoding="utf-8",
            errors="replace",
            timeout=45,
            check=False,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "").strip()
            report["policyBlocked"] = policy_blocked(detail)
            if report["policyBlocked"]:
                raise RuntimeError(
                    "Windows 企业应用控制阻止了 {} 的 PyTorch/CUDA 扩展。"
                    "需要由系统管理员允许或签名被拦截的训练运行库；软件不会绕过企业安全策略。\n\n{}"
                    .format(backend.upper(), detail)
                )
            raise RuntimeError(
                "{} 训练运行库预检失败：{}".format(
                    backend.upper(), detail or completed.returncode
                )
            )

        runtime = json.loads((completed.stdout or "{}").strip().splitlines()[-1])
        report["torch"] = runtime.get("torch")
        report["cuda"] = runtime.get("cuda")
        report["cudaDevice"] = runtime.get("device")
        report["ready"] = True
        return report
    except Exception as exc:
        detail = str(exc)
        if 'camera_pinhole_rasterizer_' in detail:
            report['errorCode'] = 'camera_pinhole_runtime_unavailable'
        report["policyBlocked"] = report["policyBlocked"] or policy_blocked(detail)
        report["error"] = detail
        return report


def main(argv=None):
    configure_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend-root", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--backend", choices=("3dgs", "2dgs"), default="3dgs")
    parser.add_argument("--run-colmap", action="store_true")
    parser.add_argument("--pose-policy", choices=("solve", "locked"))
    parser.add_argument("--camera-dataset")
    parser.add_argument("--camera-dataset-identity")
    parser.add_argument("--resolution-policy", choices=("preserve", "manual"), default="preserve")
    parser.add_argument("--resolution", choices=(1, 2, 4, 8), type=int, default=1)
    arguments = parser.parse_args(argv)
    report = probe_training_environment(
        arguments.backend_root,
        arguments.dataset,
        arguments.backend,
        arguments.run_colmap,
        arguments.pose_policy,
        arguments.camera_dataset,
        arguments.camera_dataset_identity,
        arguments.resolution_policy,
        arguments.resolution,
    )
    print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    return 0 if report["ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
