"""Validate a native payload and generate Inno inputs from the native catalog."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_i18n import load_catalog
from verify_native_backend import verify

LOCALES = ("zh_CN", "en_US", "ja_JP")
MESSAGES = {
    "GswRuntimeTitle": "安装内容与训练环境",
    "GswRuntimeSubtitle": "桌面程序与研究运行环境分开管理。",
    "GswRuntimeBody": "本安装程序包含原生桌面程序、Qt 运行库、计算后端代码、对应源码和许可证。CUDA、PyTorch、Python/Conda、COLMAP 及其他研究运行库需要另行配置；不会复制或改动你已有的训练环境。\n\n默认优先安装到可写的非系统盘；你可以在下一步更改目录。升级和卸载只处理已安装的程序文件，不删除你新增的工程、模型、照片、检查点或设置。升级前请自行保存并关闭正在运行的程序；安装器不会强制结束或自动启动程序。",
    "GswFinished": "安装完成。可从开始菜单或你选择的桌面快捷方式打开 Gaussian Scene Workbench。程序不会自动启动。",
    "GswUnsafeDirectory": "请选择专用的程序子文件夹，不要选择磁盘根目录、Windows 目录或用户主目录。",
    "GswNonemptyDirectory": "此目录包含其他文件，且不是已有的 Gaussian Scene Workbench 安装目录。请选择空目录或已有程序目录；不会覆盖无关文件。",
    "GswBusyInstall": "已安装文件正在使用或无法写入。请保存并关闭正在使用此安装目录的程序，确认写入权限后重试；安装器不会强制结束任何进程。",
}
REQUIRED = (
    "bin/Gaussian Scene Workbench.exe", "bin/Qt6Core.dll", "bin/Qt6Gui.dll",
    "bin/Qt6Widgets.dll", "bin/Qt6OpenGL.dll", "bin/Qt6OpenGLWidgets.dll",
    "bin/Qt6Svg.dll", "bin/platforms/qwindows.dll", "COPYING", "LICENSE",
    "THIRD_PARTY_LICENSES.md", "native/LICENSE.md", "source-manifest.json",
    "build_manifest.json", "backend_manifest.json", "source/native/CMakeLists.txt",
    "source/native/assets/gsw-app-icon.ico", "native/worker/gsw_worker.py",
    "native/worker/import_preflight.py", "native/backend/compute.py",
    "gaussian-splatting/train.py", "scripts/check_3dgs_env.ps1",
    "source/native/src/shaders/SurfelProjection.glsl",
    "source/native/third_party/blender-gizmo/BlenderGizmoGeometry.cpp",
)
SOURCE_MATERIALS = (
    "packaging/windows/gsw.iss", "scripts/prepare_windows_installer.py",
    "scripts/build_windows_installer.ps1", "scripts/bootstrap_inno_setup.ps1",
    "scripts/test_windows_installer.py", "scripts/test_windows_installer.ps1",
    "scripts/test_windows_installer_ui.ps1",
    "docs/WINDOWS_INSTALLER.md", "scripts/native_i18n.py",
    "scripts/verify_native_backend.py", "scripts/write_native_source_manifest.ps1",
)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_payload(root):
    root = Path(root).resolve(strict=True)
    missing = [name for name in REQUIRED if not (root / name).is_file()]
    if missing:
        raise ValueError("Incomplete native payload: " + ", ".join(missing))
    for path in root.rglob("*"):
        if path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("Reparse/symlink payload is not allowed: " + str(path))
        if path.name.lower() in {".git", "__pycache__", "workspaces", "test-settings"}:
            raise ValueError("Private/generated data in payload: " + str(path))
    issues = verify(root, package=True)
    if issues:
        raise ValueError("; ".join(issues))
    manifest = json.loads((root / "source-manifest.json").read_text(encoding="utf-8-sig"))
    if (manifest.get("schema") != "gsw-native-corresponding-source-v1"
            or manifest.get("license") != "GPL-3.0-or-later"
            or manifest.get("executableSha256", "").lower() != sha256(root / REQUIRED[0])):
        raise ValueError("Executable/corresponding-source identity mismatch")
    source = root / "source"
    recorded = set()
    for entry in manifest.get("files", []):
        relative = entry["path"]
        pure = PurePosixPath(relative)
        if (not isinstance(relative, str) or pure.is_absolute() or "\\" in relative
                or ":" in relative or any(part in {"", ".", ".."} for part in relative.split("/"))):
            raise ValueError("Non-canonical source manifest path")
        path = (source / relative).resolve()
        if not path.is_relative_to(source) or relative in recorded:
            raise ValueError("Unsafe or duplicate source manifest path")
        recorded.add(relative)
        if (not path.is_file() or path.stat().st_size != entry["size"]
                or sha256(path) != entry["sha256"].lower()):
            raise ValueError("Corresponding source mismatch: " + relative)
    actual = {path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file()}
    if not recorded or len(recorded) != manifest.get("sourceFileCount") or actual != recorded:
        raise ValueError("Incomplete corresponding-source manifest")
    build = json.loads((root / "build_manifest.json").read_text(encoding="utf-8-sig"))
    if build.get("product") != "Gaussian Scene Workbench" or build.get("legacyWebRuntimeIncluded") is not False:
        raise ValueError("Not a native GSW package")
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", build.get("version", "")):
        raise ValueError("Invalid package version")
    return build


def inno_string(value):
    value = str(value)
    if any(char in value for char in ('"', "\r", "\n", "\x00")):
        raise ValueError("Unsafe Inno preprocessor string")
    return '"' + value + '"'


def render_messages(catalog):
    lines = []
    for identifier, source in MESSAGES.items():
        for locale in LOCALES:
            text = catalog[source][locale]
            if not text.strip() or any(char in text for char in ("\x00", "\r")):
                raise ValueError("Invalid installer translation: " + identifier)
            # Inno custom message line breaks. User paths are never interpolated.
            if "%n" in text:
                raise ValueError("Unexpected installer message directive")
            lines.append(f"{locale}.{identifier}={text.replace(chr(10), '%n')}")
    return "\n".join(lines) + "\n"


def prepare(package, work, output, source_root, test_build=False):
    package, work, output, source_root = (Path(p).resolve() for p in (package, work, output, source_root))
    if output.is_relative_to(package) or work.is_relative_to(package) or work.exists():
        raise ValueError("Fresh work directory outside the immutable input payload is required")
    build = validate_payload(package)
    work.mkdir(parents=True)
    payload = work / "payload"
    shutil.copytree(package, payload)
    # Installer definition and build tools accompany the program's exact source.
    for relative in SOURCE_MATERIALS:
        target = payload / "source" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_root / relative, target)
    catalog = load_catalog(source_root / "native/i18n/catalog.json")
    if not all(source in catalog for source in MESSAGES.values()):
        # Rebuilding only the installer around an older accepted binary must not
        # replace that binary's matching native translation source.
        catalog = load_catalog(source_root / "packaging/windows/installer-catalog.json")
    (payload / "source/packaging/windows/installer-catalog.json").write_text(
        json.dumps({source: catalog[source] for source in MESSAGES.values()}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    (work / "messages.iss").write_text(render_messages(catalog), encoding="utf-8-sig")
    shutil.copyfile(source_root / "packaging/windows/gsw.iss", work / "gsw.iss")
    app_id = "Fubukisama.GaussianSceneWorkbench.Native"
    if test_build:
        app_id = "GSW.Test." + uuid.uuid4().hex
    (payload / "gsw-installation.ini").write_text(
        "[Installation]\nProduct=Gaussian Scene Workbench\nAppId=" + app_id +
        "\nVersion=" + build["version"] + "\n", encoding="ascii")
    name = f"Gaussian-Scene-Workbench-{build['version']}-windows-x64-setup"
    if test_build:
        name += "-test"
    values = {
        "GswAppId": app_id, "GswVersion": build["version"],
        "GswNumericVersion": build["version"].split("-")[0] + ".0",
        "GswPayload": str(payload), "GswOutput": str(output), "GswOutputName": name,
        "GswIcon": str(payload / "source/native/assets/gsw-app-icon.ico"),
    }
    (work / "generated.iss").write_text(
        "\n".join(f"#define {key} {inno_string(value)}" for key, value in values.items()) + "\n",
        encoding="utf-8-sig")
    return {"schema": "gsw-windows-installer-v1", "appId": app_id, "testBuild": bool(test_build),
            "version": build["version"], "packageRoot": str(package), "workRoot": str(work),
            "payloadRoot": str(payload), "setupPath": str(output / (name + ".exe")),
            "executableSha256": sha256(package / REQUIRED[0]), "locales": list(LOCALES),
            "trainingRuntimesBundled": False, "automaticAppLaunch": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--test-build", action="store_true")
    args = parser.parse_args()
    try:
        record = prepare(args.package_root, args.work_root, args.output_root, args.source_root, args.test_build)
        (args.work_root / "installer-build.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    except (ValueError, OSError, KeyError) as exc:
        parser.exit(1, f"Installer input validation failed: {exc}\n")
    print(json.dumps(record))


if __name__ == "__main__":
    main()
