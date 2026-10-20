"""Check the native compute boundary and optionally an installed native package."""

import argparse
import ast
from pathlib import Path
import sys


BACKEND_FILES = (
    "native/backend/atomic_file.py",
    "native/backend/camera_dataset.py",
    "native/backend/camera_admission.py",
    "native/backend/camera_scale_export.py",
    "native/backend/scale_constraints.py",
    "native/backend/model_scale_version.py",
    "native/backend/trajectory_dataset.py",
    "native/backend/training_batch_plan.py",
    "native/worker/camera_import.py",
    "native/worker/scale_constraints.py",
    "native/worker/training_seed.py",
    "native/worker/training_batch.py",
    "native/worker/canonical_training.py",
    "native/worker/camera_rasterizer_runtime.py",
    "native/backend/__init__.py",
    "native/backend/compute.py",
    "native/backend/reconstruction_quality.py",
    "native/backend/video_extract.py",
    "scripts/postprocess_2dgs_mesh.py",
    "scripts/run_sugar.py",
)
RETIRED_TREES = ("crop_editor", "desktop_app", "resources/app")
WEB_IMPORTS = ("http.server", "cgi", "cgitb", "webbrowser", "socketserver", "wsgiref")
WEB_EXTENSIONS = {".htm", ".html", ".js", ".mjs", ".cjs", ".css", ".asar", ".wasm"}


def is_web_import(name):
    return any(name == module or name.startswith(module + ".") for module in WEB_IMPORTS)


def imported_modules(tree):
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            yield from (alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            yield node.module
            yield from (node.module + "." + alias.name for alias in node.names)
        elif isinstance(node, ast.Call) and node.args:
            function = node.func
            dynamic_import = (
                isinstance(function, ast.Name) and function.id == "__import__"
            ) or (
                isinstance(function, ast.Attribute) and function.attr == "import_module"
            )
            if dynamic_import and isinstance(node.args[0], ast.Constant):
                module = node.args[0].value
                if isinstance(module, str):
                    yield module


def verify(root, package=False):
    issues = []
    for relative in RETIRED_TREES:
        if (root / relative).exists():
            issues.append("Retired application tree exists: " + relative)
    for relative in BACKEND_FILES:
        path = root / relative
        if not path.is_file():
            issues.append("Required native backend file is missing: " + relative)
            continue
        try:
            tree = ast.parse(path.read_bytes(), filename=str(path))
        except (OSError, SyntaxError, ValueError) as exc:
            issues.append("Invalid Python in {}: {}".format(relative, exc))
            continue
        if relative == "native/backend/compute.py":
            for module in sorted(set(imported_modules(tree))):
                if is_web_import(module):
                    issues.append("Compute backend imports a retired web dependency: " + module)
    if package:
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            name = path.name.lower()
            if (path.suffix.lower() in WEB_EXTENSIONS
                    or name in {"node", "node.exe", "electron", "electron.exe"}
                    or name.startswith(("qtwebengine", "qt5webengine", "qt6webengine"))):
                issues.append("Forbidden web runtime file: " + path.relative_to(root).as_posix())
    return issues


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--package", action="store_true", help="Also reject web files in an installed package")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        parser.error("Root directory does not exist: {}".format(root))
    issues = verify(root, args.package)
    if issues:
        print("Native backend verification failed:", file=sys.stderr)
        for issue in issues:
            print(" - " + issue, file=sys.stderr)
        return 1
    print("Native {} verified: {}".format("package" if args.package else "compute source", root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
