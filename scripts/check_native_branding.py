"""Statically check display branding without changing persistent product identities.

Use --ref to inspect an existing Git snapshot, without checking it out or running
the application. This is source validation, not package or window acceptance.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DISPLAY = "Gaussian Splatting Workbench"
LEGACY = "Gaussian Scene Workbench"
APP_ID = "Fubukisama.GaussianSceneWorkbench.Native"


def inspect(ref=None):
    def read(relative):
        if ref:
            return subprocess.run(
                ["git", "show", f"{ref}:{relative}"], cwd=ROOT, check=True,
                capture_output=True, timeout=15,
            ).stdout.decode("utf-8-sig").replace("\r\n", "\n")
        return (ROOT / relative).read_text(encoding="utf-8-sig")

    checks = []
    def require(condition, name):
        checks.append({"check": name, "passed": bool(condition)})

    for relative in ("README.md", "README.zh-CN.md", "README.ja.md"):
        text = read(relative)
        heading = re.search(r"<h1\b[^>]*>(.*?)</h1>", text)
        require(heading is not None and heading[1] == DISPLAY,
                f"{relative}: formal display heading")
        require("github.com/fubukisama/Gaussian-Scene-Workbench" in text,
                f"{relative}: repository URL remains compatible")

    main = read("native/src/main.cpp")
    require(f'setApplicationDisplayName(QStringLiteral("{DISPLAY} Native"))' in main,
            "native application display name")
    require(f'setApplicationName(QStringLiteral("{LEGACY}"))' in main,
            "persistent application settings identity unchanged")
    require('setOrganizationName(QStringLiteral("GaussianSceneWorkbench"))' in main,
            "persistent organization identity unchanged")
    launcher = read("native/src/NativeLauncher.cpp")
    require(f'setApplicationName(QString::fromLatin1("{LEGACY}"))' in launcher,
            "launcher settings identity unchanged")

    build = json.loads(read("native/build_manifest.json"))
    require(build.get("product") == LEGACY, "package product identity unchanged")
    require(build.get("appUserModelId") == APP_ID, "Windows taskbar identity unchanged")
    cmake = read("native/CMakeLists.txt")
    require(f'OUTPUT_NAME "{LEGACY}"' in cmake and
            f'OUTPUT_NAME "{LEGACY} Launcher"' in cmake,
            "application and stable-launcher filenames unchanged")
    names = read("native/src/NativeInstallation.h")
    require(f'kNativeApplicationName[] = "{LEGACY}.exe"' in names and
            f'kNativeLauncherName[] = "{LEGACY} Launcher.exe"' in names,
            "installation binary lookup compatibility")
    require(f'201 "{DISPLAY}"' in read("native/assets/gsw-app-icon.rc"),
            "taskbar display-name resource")

    catalog = json.loads(read("native/i18n/catalog.json"))
    branded = {key: value for key, value in catalog.items() if DISPLAY in key}
    require(len(branded) >= 6, "localized native and Setup brand messages present")
    for source, translations in branded.items():
        require(set(translations) == {"zh_CN", "en_US", "ja_JP"} and
                all(DISPLAY in value and LEGACY not in value for value in translations.values()),
                f"three-language display brand: {source[:80]}")
    require(not any(LEGACY in key for key in catalog), "no stale branded UI catalog keys")

    setup = read("packaging/windows/gsw.iss")
    for field, value in (("AppName", DISPLAY), ("AppVerName", DISPLAY + " {#GswVersion}"),
                         ("UninstallDisplayName", DISPLAY), ("DefaultGroupName", DISPLAY)):
        require(f"{field}={value}\n" in setup, f"Setup {field}")
    require(f'Name: "{{group}}\\{DISPLAY}"' in setup and
            f'Name: "{{autodesktop}}\\{DISPLAY}"' in setup,
            "new installation shortcut labels")
    require(f'Filename: "{{app}}\\bin\\{LEGACY}.exe"' in setup and
            APP_ID in setup and 'AppId={#GswAppId}' in setup,
            "Setup executable and upgrade identity unchanged")
    require(f"Product={LEGACY}\\nAppId=" in read("scripts/prepare_windows_installer.py"),
            "Setup receipt identity unchanged")

    project = read("native/src/WorkspaceDocument.cpp")
    require(project.count(f'QStringLiteral("{LEGACY}")') >= 3,
            "saved project application identity unchanged")
    license_text = read("LICENSE")
    require(license_text.startswith(DISPLAY + " - License overview\n"), "license overview display title")
    grant = license_text.partition("MIT License\n")[2].partition("\nThird-party components\n")[0]
    require(LEGACY in grant and DISPLAY not in grant and "Copyright (c) 2026 fubukisama" in grant,
            "original MIT grant retains its original attribution")
    return {"displayName": DISPLAY, "persistentProductIdentity": LEGACY,
            "sourceRef": ref or "working-tree", "checks": checks,
            "passed": all(check["passed"] for check in checks),
            "windowOrPackageAcceptance": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = inspect(args.ref)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("x", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
