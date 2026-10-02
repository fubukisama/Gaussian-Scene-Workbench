"""Validate the Python runtime used by the native managed-media importer."""

import argparse
import importlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


def load_server_module(backend_root):
    root = Path(backend_root).resolve()
    server_path = root / "crop_editor" / "server.py"
    if not server_path.is_file():
        raise FileNotFoundError("staged crop_editor/server.py was not found: {}".format(server_path))
    # Loading a file by spec does not add its package root to the search path.
    # Match the worker's selected-backend imports, independent of launch cwd.
    sys.path.insert(0, str(root))
    spec = importlib.util.spec_from_file_location("gsw_staged_crop_server", str(server_path))
    if spec is None or spec.loader is None:
        raise RuntimeError("could not create an import spec for {}".format(server_path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _ffmpeg_video_backend(ffmpeg, backend_root):
    if not ffmpeg or not Path(ffmpeg).is_file():
        return False, "FFmpeg executable is unavailable"
    try:
        completed = subprocess.run(
            [str(ffmpeg), "-version"],
            cwd=str(Path(backend_root).resolve()),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=15,
            check=False,
        )
    except Exception as exc:
        return False, "FFmpeg probe failed: {}".format(exc)
    if completed.returncode == 0:
        return True, (completed.stdout or completed.stderr or "").strip()
    detail = (completed.stderr or completed.stdout or "").strip()
    return False, detail or "FFmpeg probe exited {}".format(completed.returncode)


def _conda_video_backend(server, backend_root):
    helper = Path(backend_root).resolve() / "crop_editor" / "video_extract.py"
    conda = server.conda_executable()
    if not conda or not Path(conda).is_file() or not helper.is_file():
        return False, "Conda or crop_editor/video_extract.py is unavailable"
    command = [
        str(conda),
        "run",
        "-n",
        "gaussian_splatting",
        "python",
        "-c",
        "import cv2; print(cv2.__version__)",
    ]
    try:
        completed = subprocess.run(
            command,
            cwd=str(Path(backend_root).resolve()),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=30,
            check=False,
        )
    except Exception as exc:
        return False, "Conda video probe failed: {}".format(exc)
    if completed.returncode == 0:
        return True, (completed.stdout or "").strip()
    detail = (completed.stderr or completed.stdout or "").strip()
    return False, detail or "Conda video probe exited {}".format(completed.returncode)


def probe_import_environment(backend_root, require_video=False):
    root = Path(backend_root).resolve()
    report = {
        "version": 1,
        "ready": False,
        "python": str(Path(sys.executable).resolve()),
        "server": str(root / "crop_editor" / "server.py"),
        "videoRequired": bool(require_video),
        "videoBackend": None,
    }
    try:
        server = load_server_module(root)
        importlib.import_module("numpy")
        importlib.import_module("plyfile")
    except Exception as exc:
        report["error"] = "Unable to load staged crop_editor/server.py with numpy and plyfile: {}: {}".format(
            type(exc).__name__, exc
        )
        return report

    if not require_video:
        report["ready"] = True
        return report

    errors = []
    try:
        ffmpeg = server.ffmpeg_executable()
        ffmpeg_ready, ffmpeg_detail = _ffmpeg_video_backend(ffmpeg, root)
        if ffmpeg_ready:
            report["videoBackend"] = "ffmpeg"
            report["videoExecutable"] = str(Path(ffmpeg).resolve())
            report["ready"] = True
            return report
        errors.append("FFmpeg is unavailable: {}".format(ffmpeg_detail))
    except Exception as exc:
        errors.append("FFmpeg probe failed: {}".format(exc))

    try:
        importlib.import_module("cv2")
        report["videoBackend"] = "opencv-current-python"
        report["ready"] = True
        return report
    except Exception as exc:
        errors.append("OpenCV is unavailable in the selected Python: {}".format(exc))

    try:
        conda_ready, conda_detail = _conda_video_backend(server, root)
    except Exception as exc:
        conda_ready, conda_detail = False, str(exc)
    if conda_ready:
        report["videoBackend"] = "conda-video-extract"
        report["ready"] = True
        return report
    errors.append("Conda video_extract fallback is unavailable: {}".format(conda_detail))
    report["error"] = "; ".join(errors)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend-root", required=True)
    parser.add_argument("--require-video", action="store_true")
    arguments = parser.parse_args(argv)
    report = probe_import_environment(arguments.backend_root, arguments.require_video)
    print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    return 0 if report["ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
