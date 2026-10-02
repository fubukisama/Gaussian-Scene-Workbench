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


def load_server_module(backend_root):
    root = Path(backend_root).resolve()
    server_path = root / "crop_editor" / "server.py"
    if not server_path.is_file():
        raise FileNotFoundError("crop_editor/server.py is missing: {}".format(server_path))
    # Loading a file by spec does not add its package root to the search path.
    # Match the worker's selected-backend imports, independent of launch cwd.
    sys.path.insert(0, str(root))
    spec = importlib.util.spec_from_file_location("gsw_training_preflight_server", str(server_path))
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


def probe_training_environment(backend_root, dataset_path, backend="3dgs", run_colmap=False):
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
    }
    try:
        if not dataset.is_dir():
            raise FileNotFoundError("训练数据集不存在：{}".format(dataset))
        image_count, images_path = count_images(dataset)
        report["imageCount"] = image_count
        report["imagesPath"] = str(images_path)
        report["hasReconstruction"] = has_reconstruction(dataset)
        if image_count <= 0:
            raise RuntimeError("训练数据集中没有可用图像：{}".format(images_path))

        server = load_server_module(root)
        if report["hasReconstruction"] and (dataset / "sparse" / "0").is_dir():
            quality = server.reconstruction_quality.assess_dataset(dataset)
            report["reconstructionQuality"] = quality
            report["reconstructionRepairRequired"] = not quality["usable"]
        python_path = Path(server.training_python(backend)).resolve()
        report["python"] = str(python_path)
        if not python_path.is_file():
            raise FileNotFoundError("训练 Python 不存在：{}".format(python_path))
        gaussian_dir = Path(server.TWO_DGS_DIR if backend == "2dgs" else server.GAUSSIAN_DIR)
        if not (gaussian_dir / "train.py").is_file():
            raise FileNotFoundError("训练源码不存在：{}".format(gaussian_dir / "train.py"))
        colmap = server.colmap_executable()
        if not colmap or not Path(colmap).is_file():
            raise FileNotFoundError("COLMAP 不可用，无法完成训练数据准备。")
        report["colmap"] = str(Path(colmap).resolve())
        report["colmapRequired"] = bool(run_colmap or not report["hasReconstruction"] or report.get("reconstructionRepairRequired"))
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

        completed = subprocess.run(
            [str(python_path), "-B", "-c", runtime_probe_code(backend)],
            cwd=str(gaussian_dir),
            env=process_environment,
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
    arguments = parser.parse_args(argv)
    report = probe_training_environment(
        arguments.backend_root,
        arguments.dataset,
        arguments.backend,
        arguments.run_colmap,
    )
    print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    return 0 if report["ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
