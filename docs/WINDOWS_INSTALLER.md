# Windows setup wizard

`Gaussian-Scene-Workbench-<version>-windows-x64-setup.exe` is the actual Windows
installer. An Actions artifact ZIP or a portable application folder is **not**
an installer. Public Releases should offer this Setup EXE and its `.sha256` file;
the portable directory remains an optional separate download.

## User behavior

- Standard welcome/license/destination/shortcut/install/finish wizard, in
  Simplified Chinese, English or Japanese. Appearance follows Windows at startup.
- Per-user install, without requesting administrator rights. Default destination
  prefers writable `D:\Apps\Gaussian Scene Workbench`, then another writable
  non-system fixed drive, then the normal per-user Programs folder on C-only
  systems. The user can choose another dedicated folder. Existing installer
  upgrades remember the previous directory.
- Start Menu shortcut and an optional desktop shortcut target
  `bin\Gaussian Scene Workbench.exe` with the native AppUserModelID. Setup never
  automatically starts/restarts the app, closes a running app, starts training,
  or resumes a checkpoint. Save and close the application before upgrading.
  Before replacing any file, Setup checks the embedded owned-file list for
  exclusive write access and refuses a busy/read-only target without terminating
  its process or partially starting the upgrade.
- Windows Settings exposes the uninstall entry. Only files installed by Setup
  are removed. No recursive `{app}`/workspace/runtime deletion is used. User-added
  project/model/photo/checkpoint/settings files and separate research environments
  are preserved. Do not save personal data over an installed program file.
- The complete staged desktop application, Qt DLLs/plugins, backend sources,
  original-program corresponding source and individual licenses are mandatory.
  Python/Conda, CUDA, PyTorch, COLMAP, OpenMVS and research weights remain separate
  installations, subject to their own licenses and runtime preflight.

中文：这是安装向导 EXE，而不是 ZIP。默认优先非系统盘，支持开始菜单、可选桌面
快捷方式和系统卸载；不会自动启动程序或训练，也不删除用户新增数据。训练运行库另行配置。

日本語：ZIP ではなくセットアップ EXE です。システム以外のドライブを優先し、
スタートメニュー、任意のデスクトップショートカット、システムのアンインストールに
対応します。アプリや学習を自動起動せず、追加されたユーザーデータを削除しません。
学習ランタイムは別途設定します。

## Build

First create and verify the complete staged package using the existing native
CMake/Qt/backend pipeline. Never package only an EXE, a development checkout,
an arbitrary folder, or this machine's Python environment.

Build tools: PowerShell >=7.2, QA Python >=3.12, Inno Setup 7.1.0 and the official
MSVC `dumpbin.exe` inspector for a real native payload. Before packaging,
`scripts/stage_native_crt.ps1 -PackageRoot <staged-root>` resolves the five-file
baseline and numeric MSVC import closure from official Visual Studio Redist
directories. Existing valid app-local CRT files are preserved; `-VerifyOnly`
is read-only. Original filenames, x64 PE headers, Microsoft signatures, versions
and hashes are recorded. System32/WinSxS/debug libraries are never input.
The Setup builder independently verifies this closure, so a system-wide CRT
cannot mask a missing DLL. The installer never runs `vc_redist.exe` or requires
elevation to install these app-local files.

```powershell
$compiler = ./scripts/bootstrap_inno_setup.ps1
./scripts/build_windows_installer.ps1 -PackageRoot '<verified native package>' `
  -OutputDirectory 'D:/Codex/packages/GSW-setup' -InnoCompiler $compiler `
  -PythonExecutable '<QA Python>'
```

The pinned builder is Inno Setup 7.1.0; bootstrap verifies both the official
SHA-256 and Authenticode publisher before a hidden install on D. Override the
tool/cache locations when the build host uses another non-system drive. The
compiler's source and license are at https://github.com/jrsoftware/issrc and
https://jrsoftware.org/files/is/license.txt . This is unchanged Inno Setup, not a
Blender installer or branding copy. No paid code-signing identity is invented.

Input EXE/source identity, exact corresponding-source file set, backend markers,
required Qt files, native-only boundary and absence of reparse/private data are
validated before a fresh private build copy is made. The original payload is
not modified. Installer definition/tools/catalog subset are added to the
corresponding source, and its manifest is recalculated around the **unchanged**
native EXE. Existing native translation/build/manifest helpers retain their
original bytes and are listed in `preservedNativeBuildTools`; installer-specific
tools and CI recipes are added separately. `installer-payload.json` hashes the full payload for acceptance.
Setup, `.sha256` and `.exe.json` build metadata are produced together.

Pass `-CertificateThumbprint` to sign the final Setup with an existing trusted
code-signing certificate. Without one the package is explicitly `NotSigned`;
Windows may show an unknown-publisher/SmartScreen warning or block it under
application-control policy. Setup does not disable any Windows security policy.

## Acceptance and publication boundary

```powershell
python -B scripts/native_i18n.py
python -B scripts/test_native_i18n.py
python -B scripts/test_windows_installer.py
./scripts/test_windows_shortcut_unicode.ps1 -TestRoot 'D:/Codex/validation/GSW-shortcuts'
./scripts/test_windows_installer_build.ps1 -TestRoot 'D:/Codex/validation/GSW-wrapper' `
  -InnoCompiler '<ISCC.exe>' -PythonExecutable '<QA Python>'
./scripts/build_windows_installer.ps1 -PackageRoot '<verified native package>' `
  -OutputDirectory 'D:/Codex/validation/GSW-setup-test' -InnoCompiler '<ISCC.exe>' `
  -PythonExecutable '<QA Python>' -TestBuild
./scripts/test_windows_installer.ps1 -SetupPath '<test setup.exe>' `
  -TestRoot 'D:/Codex/validation/GSW-setup-acceptance'
```

Test builds have an isolated AppId and are not publishable. Acceptance silently
installs, reinstalls and uninstalls in a fresh space/Unicode test path for all
three languages; checks installed files, corresponding source, uninstall entry,
unique test Start Menu shortcut and preservation of sentinel user/runtime data;
never launches GSW or touches the actual desktop shortcut. Logs remain on D.
Shortcut target and working-directory checks use the native Unicode Windows
Shell interface, not the lossy ANSI values returned by WScript on some hosts.
The non-GUI shortcut regression retains both readings and the unchanged link
hash; installed shortcut acceptance still requires the exact expected paths.
The regular native app/render/training checks are not replaced by installer tests.
The small Windows Installer workflow uses an explicitly marked synthetic
file fixture to exercise the **real Setup wizard and transactions**, without
executing a dummy application. This separate installer-only test was approved
by the user; its `0.0.0-installer-fixture` output is never publishable and normal
production builds reject it. Local full-candidate installation evidence is
retained separately. A published Setup still requires the complete native app,
worker and package workflow; fixture success is never native-program acceptance.

The verified Setup artifact is uploaded by CI only after the complete native
package gate and installer acceptance pass. Main-task publication remains the
single owner of the public native Release/tag; this installer task must not
publish a competing version or treat an in-progress native build as accepted.
