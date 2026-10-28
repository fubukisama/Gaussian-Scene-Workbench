<div lang="en">

# Get Gaussian Scene Workbench

[English](#english) · <span lang="zh-CN">[简体中文](#简体中文)</span> · <span lang="ja">[日本語](#日本語)</span>

Windows x64 · Native preview · [Back to the project](README.md)

<div lang="en">

## English

### Download the preview

There is no public native Release yet. For now, use the artifacts of a **fully successful** [Native Windows run](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml) for the commit you want.

1. Open a completed green run and find **Artifacts**.
2. Download **Gaussian-Scene-Workbench-windows-x64-setup**.
3. Extract that artifact ZIP, then run the contained `Gaussian-Scene-Workbench-<version>-windows-x64-setup.exe`.

GitHub may require sign-in for artifact downloads, and artifacts expire. A source-code ZIP is not an installer. If a checked installer is unavailable, use the [native build guide](native/README.md), not an unverified or in-progress build.

### Before you start

- **Existing models:** import supported PLY / SPZ files after installing the desktop.
- **Reconstruction / training:** configure the separate [Windows runtime](docs/WINDOWS_NATIVE_RUNTIME.md) and [COLMAP](docs/COLMAP_SETUP.md) dependencies.
- **Installation:** keep the full application, DLLs, plugins, notices and matching source together. Choose a writable location; prefer a non-system drive when available. C:-only machines are supported.
- **Security:** preview installers may be unsigned. Use approved signing/runtime policies; never disable Windows protection just to run a preview.

### Updating

Managed installations use one stable native launcher and verified version slots. The current updater **does not automatically check or download online releases**. See [managed updates](docs/NATIVE_UPDATES.md) for publication, migration and rollback; keep old versions and running sessions intact.

Portable artifacts are separate from Setup and may carry a legacy version in the artifact name. Check `build_manifest.json` and the accepted commit rather than inferring the product version from that label. A downloaded portable artifact must pass its complete file manifests before use; if anything is missing, use a verified Setup or rebuild a complete package—do not disable the check.

</div>

<div lang="zh-CN">

## 简体中文

### 下载预览版

目前尚无公开的原生 Release。请在所需提交的 [Native Windows](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml) **完整成功运行**中获取附件：

1. 打开已完成的绿色运行，找到 **Artifacts**。
2. 下载 **Gaussian-Scene-Workbench-windows-x64-setup**。
3. 解压这个附件 ZIP，运行其中的 `Gaussian-Scene-Workbench-<version>-windows-x64-setup.exe`。

下载 Actions 附件可能需要登录 GitHub，附件也有保存期限。源码 ZIP 不是安装器；没有可用的已验安装器时，按[原生构建指南](native/README.md)构建，不使用失败、未验或仍在运行的产物。

### 使用前确认

- **已有模型：**安装桌面端后可导入受支持的 PLY / SPZ。
- **重建／训练：**另行配置 [Windows 运行环境](docs/WINDOWS_NATIVE_RUNTIME.md)和 [COLMAP](docs/COLMAP_SETUP.md)。
- **安装位置：**保留完整程序、DLL、插件、声明和对应源码。选择可写目录，有非系统盘时优先使用；只有 C: 的机器也可使用。
- **安全策略：**预览安装器可能未签名，应使用获准的签名／运行策略，不要为了运行预览版关闭 Windows 防护。

### 更新方式

托管安装使用固定原生启动器和经过校验的版本目录，**目前不会自动联网检查或下载新版**。[更新说明](docs/NATIVE_UPDATES.md)记录了发布、迁移和回退流程；保留旧版，不中断运行中的会话。

便携附件与 Setup 是两种产物，附件名称可能仍含旧版本号。以 `build_manifest.json` 和已验提交确认身份，不凭附件标签判断版本。便携包必须通过完整文件清单校验；缺文件时改用已验 Setup 或重新构建完整包，不能关闭校验后继续使用。

</div>

<div lang="ja">

## 日本語

### プレビュー版の入手

公開のネイティブ Release はまだありません。対象コミットの [Native Windows](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml) で、**全体が成功した実行**から取得します。

1. 完了した緑色の実行を開き、**Artifacts** を確認。
2. **Gaussian-Scene-Workbench-windows-x64-setup** をダウンロード。
3. 添付ZIPを展開し、`Gaussian-Scene-Workbench-<version>-windows-x64-setup.exe` を実行。

Actions 添付の取得には GitHub サインインが必要な場合があり、保存期限もあります。ソースZIPはインストーラーではありません。検証済みSetupがなければ[ネイティブビルド手順](native/README.md)を使い、失敗・未検証・実行中の成果物は使わないでください。

### 利用前の確認

- **既存モデル：**デスクトップ導入後、対応する PLY / SPZ を取り込めます。
- **再構築・学習：**別途 [Windows 実行環境](docs/WINDOWS_NATIVE_RUNTIME.md)と [COLMAP](docs/COLMAP_SETUP.md) を構成します。
- **導入先：**DLL・プラグイン・通知・対応ソースを含め全体を保持します。書込み可能な場所を選び、利用できれば非システムドライブを優先。C: のみの端末も対象です。
- **安全性：**プレビューSetupは未署名の場合があります。承認された署名・実行ポリシーを使い、Windowsの保護を無効にしないでください。

### 更新

管理方式では固定ネイティブランチャーと検証済みバージョンを使います。**オンラインの自動確認・ダウンロードは未実装です。** [更新手順](docs/NATIVE_UPDATES.md)に従い、旧版と起動中のセッションを保持してください。

ポータブル添付はSetupと別物で、添付名に古いバージョンが残る場合があります。`build_manifest.json` と検証済みコミットで確認してください。全ファイルのマニフェストに不足があれば、検証済みSetupまたは完全な再ビルドを使い、検証を無効にしないでください。

</div>

---

[License scope](native/LICENSE.md) · [Third-party notices](THIRD_PARTY_LICENSES.md) · [Source distribution](docs/NATIVE_SOURCE_DISTRIBUTION.md)


</div>
