"""Check the native compute boundary and optionally an installed native package."""

import argparse
import ast
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys


QUALITY_WORKER_FILES = tuple("native/worker/" + name + ".py" for name in (
    "training_quality_plan", "quality_random", "quality_densification", "quality_mip",
    "quality_supervision", "quality_training_runtime", "quality_render_adapter", "quality_rasterizer_runtime",
    "quality_mcmc", "quality_mcmc_model", "quality_postprocess", "quality_export", "surfel_depth_adapter",
))
QUALITY_SCRIPT_FILES = tuple("scripts/" + name for name in (
    "cleanup_gaussians.py", "run_quality_benchmark.py", "prepare_quality_real_fixture.py",
    "verify_quality_rasterizer.py", "verify_pinhole_rasterizer_gpu.py", "verify_quality_pipeline_cuda.py",
    "verify_quality_training.py", "verify_supervision_gradient.py", "build_quality_rasterizer.ps1",
    "verify_native_backend.py", "native_i18n.py",
))
# Public defaults/catalog only. Never discover/copy local bindings, photos,
# models, checkpoint files or validation directories into an application.
QUALITY_ASSET_FILES = ("native/i18n/catalog.json", "docs/quality_upgrade/benchmark_protocol.json")
QUALITY_STAGE_FILES = QUALITY_WORKER_FILES + QUALITY_SCRIPT_FILES + QUALITY_ASSET_FILES


BACKEND_FILES = (
    "native/backend/atomic_file.py",
    "native/backend/path_budget.py",
    "native/backend/camera_dataset.py",
    "native/backend/camera_admission.py",
    "native/backend/camera_scale_export.py",
    "native/backend/scale_constraints.py",
    "native/backend/model_scale_version.py",
    "native/backend/trajectory_dataset.py",
    "native/backend/training_batch_plan.py",
    "native/backend/initialization.py",
    "native/backend/gs_geometry.py",
    "native/backend/geometry_evaluation.py",
    "native/backend/reconstruction_comparison.py",
    "native/worker/camera_import.py",
    "native/worker/scale_constraints.py",
    "native/worker/training_seed.py",
    "native/worker/training_batch.py",
    "native/worker/training_batch_connection.py",
    "native/worker/initialization.py",
    "native/worker/gs_geometry.py",
    "native/worker/gs_geometry_runtime.py",
    "native/worker/geometry_evaluation.py",
    "native/worker/reconstruction_comparison.py",
    "native/worker/canonical_training.py",
    "native/worker/camera_rasterizer_runtime.py",
    "native/worker/training_configuration.py",
    "native/worker/training_failure.py",
    "native/worker/training_job_connection.py",
    "native/backend/__init__.py",
    "native/backend/compute.py",
    "native/backend/reconstruction_quality.py",
    "native/backend/video_extract.py",
    "scripts/postprocess_2dgs_mesh.py",
    "scripts/run_sugar.py",
) + QUALITY_WORKER_FILES + tuple(name for name in QUALITY_SCRIPT_FILES if name.endswith(".py"))
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


def verify_manifest(root):
    """Bind actual staged backend bytes, not an import or GPU certificate."""
    path = root / "backend_manifest.json"
    try:
        manifest = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as exc:
        return ["Invalid native backend manifest: " + str(exc)]
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 2 or not isinstance(manifest.get("files"), list):
        return ["Native backend manifest requires schemaVersion2 file identities"]
    issues, recorded = [], set()
    components = manifest.get("components")
    if (not isinstance(components, list) or not all(isinstance(value, str) for value in components)
            or not set(QUALITY_STAGE_FILES).issubset(components)):
        issues.append("Native backend manifest is missing quality components")
    for record in manifest["files"]:
        if not isinstance(record, dict):
            issues.append("Invalid backend manifest file record")
            continue
        relative = record.get("path")
        if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative
                or PurePosixPath(relative).is_absolute() or any(part in ("", ".", "..") for part in relative.split("/"))):
            issues.append("Unsafe backend manifest file path")
            continue
        if relative in recorded:
            issues.append("Duplicate backend manifest file: " + relative)
            continue
        recorded.add(relative)
        selected = root / relative
        try:
            selected.resolve().relative_to(root.resolve())
            if not selected.is_file():
                raise ValueError("missing")
            digest = hashlib.sha256()
            with selected.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 ** 2), b""):
                    digest.update(block)
            if record.get("bytes") != selected.stat().st_size or record.get("sha256") != digest.hexdigest():
                issues.append("Native backend manifest hash mismatch: " + relative)
        except (OSError, ValueError):
            issues.append("Native backend manifest file missing/escaping root: " + relative)
    missing = set(BACKEND_FILES) | set(QUALITY_STAGE_FILES)
    for relative in sorted(missing - recorded):
        issues.append("Native backend manifest has no file identity: " + relative)
    return issues


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
    for relative in QUALITY_STAGE_FILES:
        if not (root / relative).is_file():
            message = "Required native backend file is missing: " + relative
            if message not in issues:
                issues.append(message)
    catalog = root / "native/i18n/catalog.json"
    if catalog.is_file():
        try:
            entries = json.loads(catalog.read_text(encoding="utf-8"))
            if (not isinstance(entries, dict) or not entries or any(
                    not isinstance(row, dict) or set(row) != {"zh_CN", "en_US", "ja_JP"}
                    or any(not isinstance(value, str) or not value for value in row.values())
                    for row in entries.values())):
                issues.append("Native catalog must retain complete zh_CN/en_US/ja_JP entries")
        except (OSError, ValueError) as exc:
            issues.append("Invalid native catalog: " + str(exc))
    if package:
        issues.extend(verify_manifest(root))
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
    parser.add_argument("--list-quality-stage-files", action="store_true", help="Print the explicit public quality payload file list as JSON")
    args = parser.parse_args(argv)
    if args.list_quality_stage_files:
        print(json.dumps(QUALITY_STAGE_FILES))
        return 0
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
