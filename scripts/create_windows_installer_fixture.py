"""Create a remote-CI-only, never-executable GPL installer acceptance fixture.

SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Gaussian Scene Workbench contributors
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_windows_installer import create_fixture_payload, file_hash, write_fixture_source_manifest


ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.0.0-installer-fixture"


def create_remote_fixture(output_root):
    """Publish only new, marked test bytes. No app, compiler or worker is run."""
    if os.environ.get("GITHUB_ACTIONS") != "true":
        raise ValueError("Installer fixtures are restricted to remote GitHub Actions acceptance")
    output = Path(output_root).absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Fixture output root must be fresh; existing content is never replaced")
    for ancestor in (output, *output.parents):
        if ancestor.exists() and (ancestor.is_symlink()
                                 or getattr(ancestor.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("Reparse/symlink fixture output is not allowed")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    package = output / "payload"
    build = create_fixture_payload(package, version=VERSION, installer_fixture=True)
    # A real ICO is needed by the Setup compiler; it is a repository-owned icon,
    # not Blender branding or a synthetic executable resource.
    icon = ROOT / "native/assets/gsw-app-icon.ico"
    shutil.copyfile(icon, package / "source/native/assets/gsw-app-icon.ico")
    for relative in ("COPYING", "LICENSE", "THIRD_PARTY_LICENSES.md", "native/LICENSE.md"):
        for destination in (package / relative, package / "source" / relative):
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
    warning = (
        "# INSTALLER ACCEPTANCE FIXTURE ONLY\n\n"
        "English: Synthetic test payload, not the GSW desktop application. Its EXE/DLL files are "
        "non-executable marker bytes. Never launch them or publish this as a production release. "
        "No CUDA, Python, training runtime, compiler output or user data is included.\n\n"
        "中文：仅用于远端安装器验收的合成样本，不是 GSW 桌面程序。EXE/DLL 为不可执行的标记字节，"
        "禁止启动或作为正式版发布。不含训练环境、编译产物或用户数据。\n\n"
        "日本語：リモートのインストーラー検証専用の合成データであり、GSW 本体ではありません。"
        "EXE/DLL は実行不能な識別用バイトです。起動や製品版としての公開は禁止です。"
        "学習環境・コンパイル成果物・ユーザーデータは含みません。\n"
    )
    (package / "FIXTURE-ONLY.md").write_text(warning, encoding="utf-8")
    (package / "source/FIXTURE-ONLY.md").write_text(warning, encoding="utf-8")
    manifest = write_fixture_source_manifest(package)
    record = {
        "schema": "gsw-installer-fixture-v1", "installerFixture": True,
        "version": build["version"], "packageRoot": str(package), "outputRoot": str(output),
        "executableSha256": manifest["executableSha256"], "iconSha256": file_hash(icon),
        "sourceFileCount": manifest["sourceFileCount"], "applicationExecutable": False,
        "trainingRuntimesBundled": False, "productionReleaseAllowed": False,
    }
    (output / "fixture.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        record = create_remote_fixture(arguments.output_root)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Installer fixture rejected: {error}\n")
    print(json.dumps(record, ensure_ascii=False))


if __name__ == "__main__":
    main()
