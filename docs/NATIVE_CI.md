# Native CI and security detection / 原生持续集成与安全检测 / ネイティブ CI とセキュリティ検出

## 简体中文（zh_CN）

### 构建与回归测试

`Native Windows`（[工作流](../.github/workflows/native-windows.yml)）在 Windows Server 2022 / MSVC 2022 上配置、构建并测试 Qt 6.8 原生桌面端，再部署运行库、验证原生安装包并上传 artifact。失败的历史运行会保留；只能将对应提交的新运行成功视为修复完成，不能通过关闭测试消除红叉。

Qt 安装器的 Python 与测试后端的 Python 分开管理。测试固定使用 Python 3.12，安装 [`scripts/requirements-native-checks.txt`](../scripts/requirements-native-checks.txt)，并把同一解释器的绝对路径传给 CMake、worker 测试及安装包 Python 语法检查。NumPy/Pillow 的 CMake 预检仍保留。原生视口测试使用软件 OpenGL，训练预检与 worker 回归使用受控样本或测试自有进程，不启动本机真实训练。artifact 可用不等于本机所有 CUDA 训练与可选网格运行库已验证。

### Windows CI 图形与显示预检

修复 Python 依赖后，`Native Windows` 第 #141 次运行进入了后续测试，并暴露 runner 的图形能力与显示尺寸问题：Qt 默认软件 OpenGL 的 GLSL 上限为 1.30，无法编译原生视口要求的 `#version 330` 着色器；过窄的桌面也无法满足 1400 像素宽的真实 UI 测试边界。这不是应当跳过测试的理由，下面的 bootstrap 改为检查并准备 CI 所需能力。

- [`setup_native_ci_graphics.ps1`](../scripts/setup_native_ci_graphics.ps1) 固定下载 Mesa **26.2.4 MSVC** 发布包，先核对 SHA-256 `351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695`，再通过 `QT_OPENGL_DLL` 与 `GALLIUM_DRIVER=llvmpipe` 选择本次 CI 的软件驱动。它不覆盖系统 DLL、不替换用户或应用运行库，也不把 Mesa 放入应用 artifact；没有伪造 OpenGL/GLSL 版本号。
- [`setup_native_ci_display.ps1`](../scripts/setup_native_ci_display.ps1) 只有在 `GITHUB_ACTIONS=true` 时才允许修改显示模式。它枚举真实支持的 32-bpp 模式，通过 `CDS_TEST` 后应用至少 **1600×900** 的模式，并核实实际结果；不安装显示驱动，也不修改分辨率注册表。工作站只能使用 `-InspectOnly` 枚举与检查，绝不改变本地显示模式。
- 全量测试前先运行 3 个已有真实图形入口：`native_infinite_reference_grid`、`native_light_gaussian_interaction`、`native_textured_paged_mesh_scene`。bootstrap 或这组测试失败时不进入昂贵的全量测试。**全部 98 个测试仍保留**，通过预检后完整执行；没有降低图像、交互或 UI 断言。

这些是针对已证明的失败所准备的 CI 配置修复，不代表新运行已经通过。仍须在修复提交对应的真实 Actions 中确认预检、完整测试、安装包上传以及代码扫描上传。

### 持续代码扫描

使用 CodeQL advanced setup，而不是同时开启另一套 default setup：

- C/C++：在 `Native Windows` 的真实原生构建前初始化 CodeQL，采用 `manual` 模式追踪编译，使用 `security-extended` 查询集分析所构建的源代码。
- Python：[`Native Security`](../.github/workflows/native-security.yml) 的独立矩阵任务检查 `native/worker`、`native/tests`、`crop_editor`、`scripts`、`gaussian-splatting` 与 `training_kit`；不包含 vendored SIBR 或 CUDA 子模块，不安装 Torch/CUDA 或可选研究运行库。
- GitHub Actions：同一矩阵中的 `actions` 任务检查 `.github/workflows`。Python 与 Actions 都使用 `build-mode: none`，保留默认安全查询，分别上传分析类别。

新增的代码扫描只接受桌面分支 `agent/native-desktop-0.3` 的 push 或以该分支为 head 的 pull request；手动运行也限制到该 ref。扫描 pull request 时采用 GitHub 的 merge commit。`workflow_dispatch` 的可用性取决于 GitHub 的工作流注册和默认分支要求；push 是本分支的自动运行入口。本轮不修改旧 HTML `main`，也不承诺定时扫描：GitHub 的 `schedule` 需要工作流存在于默认分支。首次运行必须核实初始化、提取/构建、分析与 SARIF 上传，而非仅验证 YAML 能解析。

结果在仓库 **Security and quality → Code scanning** 中按分支与分析类别查看。成功上传只表示检测已执行，并不保证没有漏洞。真实告警需要检查触发路径与风险后修复；不通过过滤查询、隐藏告警或删除历史失败记录冒充修复。

### 依赖漏洞告警的边界

Dependabot alerts 是管理员在仓库设置中启用的独立功能，不依赖 `dependabot.yml`，也不等于自动版本升级。它根据 GitHub 支持的依赖图和已审核公告检查**默认分支**，因此旧 HTML `main` 的告警不代表桌面开发分支的 pinned runtime 已完成审计。Python/CUDA 扩展、Conda 环境、外部 COLMAP/OpenMVS、模型权重及本机实际安装版本不保证被依赖图完整覆盖。运行库升级需要分别验证 3DGS、2DGS 与网格后端的兼容性，不能直接把 QA 环境或默认分支依赖当作训练环境。

密钥扫描/推送保护由 GitHub 仓库功能、权限和配置决定，与 CodeQL 或 Dependabot 是不同检测边界；应在设置页和告警页核实状态。不要提交访问令牌、密码或本机运行库凭据。

## English (en_US)

### Build and regression checks

`Native Windows` ([workflow](../.github/workflows/native-windows.yml)) configures, builds and tests the Qt 6.8 desktop application on Windows Server 2022 / MSVC 2022, deploys its runtime, validates the native package and uploads an artifact. Historical failures remain historical; completion requires a successful new run for the repaired commit, not disabled tests.

Qt's installer Python is separate from the QA interpreter. QA uses Python 3.12 with [`scripts/requirements-native-checks.txt`](../scripts/requirements-native-checks.txt); its absolute path is passed to CMake, worker tests and packaged Python syntax checks. The NumPy/Pillow configure guard remains enabled. Native viewport checks use software OpenGL; training preflight and worker regression use controlled samples or test-owned processes, without launching a user's real training. A downloadable artifact does not certify every local CUDA or optional meshing runtime.

### Windows CI graphics and display preflight

After the Python dependency fix, `Native Windows` run #141 reached downstream tests and exposed runner graphics and display limitations: Qt's default software OpenGL supported GLSL only up to 1.30 and could not compile the viewport's `#version 330` shaders; a narrow desktop also could not satisfy the real UI checks' 1400-pixel-width boundary. These are capability requirements to prepare and verify, not reasons to skip tests.

- [`setup_native_ci_graphics.ps1`](../scripts/setup_native_ci_graphics.ps1) pins the **Mesa 26.2.4 MSVC** release and verifies SHA-256 `351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695` before selecting the CI software driver through `QT_OPENGL_DLL` and `GALLIUM_DRIVER=llvmpipe`. It does not overwrite system DLLs, replace user/application runtimes or ship Mesa in the application artifact. OpenGL/GLSL version numbers are not spoofed.
- [`setup_native_ci_display.ps1`](../scripts/setup_native_ci_display.ps1) permits display changes only with `GITHUB_ACTIONS=true`. It enumerates supported 32-bpp modes, requires a successful `CDS_TEST`, applies a mode of at least **1600×900** and verifies the actual result. It installs no display driver and makes no resolution registry changes. Workstations may only use `-InspectOnly`, which enumerates and inspects without changing the local mode.
- Three existing real graphics checks run first: `native_infinite_reference_grid`, `native_light_gaussian_interaction` and `native_textured_paged_mesh_scene`. A bootstrap or preflight failure prevents the expensive full suite from starting. **All 98 tests remain** and run after the preflight succeeds; image, interaction and UI assertions are not weakened.

These prepared CI changes address proven failures; they are not a claim that a new run has already passed. Acceptance still requires the repaired commit's real Actions preflight, full suite, package upload and code-scanning upload.

### Ongoing code scanning

Use CodeQL advanced setup rather than enabling a second default setup:

- C/C++: `Native Windows` initializes CodeQL before the real native build, traces compilation with `manual` build mode and analyzes the built sources with the `security-extended` query suite.
- Python: an independent [`Native Security`](../.github/workflows/native-security.yml) matrix job covers `native/worker`, `native/tests`, `crop_editor`, `scripts`, `gaussian-splatting` and `training_kit`. Vendored SIBR and CUDA submodules are excluded; Torch/CUDA and optional research runtimes are not installed.
- GitHub Actions: the matrix's `actions` job checks `.github/workflows`. Both interpreted jobs use `build-mode: none`, retain the default security queries and upload separate analysis categories.

The new scanning accepts pushes to `agent/native-desktop-0.3` and pull requests whose head is that desktop branch; manual runs are also restricted to that ref. Pull requests use GitHub's merge commit. Availability of `workflow_dispatch` depends on GitHub's workflow registration and default-branch requirements; push is this branch's automatic entry point. The legacy HTML `main` branch is not changed, and no scheduled scan is promised: GitHub requires scheduled workflows to exist on the default branch. Verify initialization, extraction/build, analysis and SARIF upload on the first real run, not only YAML parsing.

Inspect **Security and quality → Code scanning** by branch and analysis category. A successful upload means the detection ran, not that the repository is vulnerability-free. Investigate the reachable path and risk of real findings before fixing them; do not substitute query exclusions, hidden alerts or deleted failed runs for remediation.

### Dependency-alert coverage

Dependabot alerts are a separate administrator-enabled repository setting; they do not require `dependabot.yml` and are not automatic version upgrades. They inspect the **default branch**, using supported dependency graphs and reviewed advisories. Alerts for legacy HTML `main` are not an audit of the desktop branch's pinned runtimes. Python/CUDA extensions, Conda environments, external COLMAP/OpenMVS, model weights and actual local installations are not guaranteed complete graph coverage. Runtime upgrades require separate 3DGS, 2DGS and mesh-backend compatibility checks; neither QA dependencies nor default-branch dependencies represent the training runtime.

Secret scanning and push protection depend on GitHub features, permissions and repository configuration and are separate from CodeQL and Dependabot. Verify their state in settings and alert pages. Never commit access tokens, passwords or local runtime credentials.

## 日本語（ja_JP）

### ビルドと回帰テスト

`Native Windows`（[ワークフロー](../.github/workflows/native-windows.yml)）は Windows Server 2022 / MSVC 2022 で Qt 6.8 デスクトップ版を構成・ビルド・テストし、ランタイムを配置してネイティブパッケージを検証し、artifact をアップロードします。過去の失敗履歴は残します。修正対象コミットの新しい実行が成功して初めて完了と判断し、テストの無効化を修正の代わりにしません。

Qt インストーラーの Python と QA 用 Python を分離します。QA は Python 3.12 と [`scripts/requirements-native-checks.txt`](../scripts/requirements-native-checks.txt) を使用し、同じインタープリターの絶対パスを CMake、worker テスト、パッケージ内 Python の構文検証に渡します。NumPy/Pillow の構成時検査は維持します。ネイティブビューポートはソフトウェア OpenGL で検証し、学習の事前検査と worker 回帰テストは管理されたサンプルやテスト専用プロセスを使います。ユーザーの実際の学習は開始しません。artifact の取得可能性は、すべてのローカル CUDA 環境や追加メッシュ環境の検証を意味しません。

### Windows CI のグラフィックスと画面の事前検査

Python 依存関係の修正後、`Native Windows` の実行 #141 は後続テストに進み、runner のグラフィックス能力と画面サイズの不足が判明しました。Qt 既定のソフトウェア OpenGL は GLSL 1.30 までしか対応せず、ビューポートの `#version 330` シェーダーをコンパイルできませんでした。また、狭いデスクトップでは実際の UI テストが要求する幅 1400 ピクセルを満たせません。テストを省略するのではなく、必要な能力を準備・確認します。

- [`setup_native_ci_graphics.ps1`](../scripts/setup_native_ci_graphics.ps1) は **Mesa 26.2.4 MSVC** リリースに固定し、SHA-256 `351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695` を検証してから、`QT_OPENGL_DLL` と `GALLIUM_DRIVER=llvmpipe` で CI 用ソフトウェアドライバーを選びます。システム DLL、ユーザー環境、アプリのランタイムは置き換えず、Mesa をアプリ artifact に含めません。OpenGL/GLSL のバージョン番号も偽装しません。
- [`setup_native_ci_display.ps1`](../scripts/setup_native_ci_display.ps1) は `GITHUB_ACTIONS=true` の場合だけ画面モードの変更を許可します。実際に対応する 32-bpp モードを列挙し、`CDS_TEST` が成功した **1600×900** 以上のモードを適用して結果を確認します。ディスプレイドライバーを導入せず、解像度のレジストリも変更しません。ワークステーションでは `-InspectOnly` だけを使い、列挙・確認のみでローカル画面モードを変更しません。
- 全テストの前に既存の実グラフィックス検査 3 件（`native_infinite_reference_grid`、`native_light_gaussian_interaction`、`native_textured_paged_mesh_scene`）を実行します。bootstrap または事前検査が失敗した場合、負荷の大きい全テストを開始しません。**98 件すべてのテストを維持**し、事前検査の成功後に実行します。画像、操作、UI の検証条件は緩和しません。

これらは確認済みの失敗に対して準備した CI 設定修正であり、新しい実行の成功を示すものではありません。修正コミットの実際の Actions で、事前検査、全テスト、パッケージおよびコードスキャンのアップロードまで確認する必要があります。

### 継続的なコードスキャン

CodeQL の advanced setup を使い、別の default setup を同時に有効化しません。

- C/C++：`Native Windows` の実際のネイティブビルド前に CodeQL を初期化し、`manual` モードでコンパイルを追跡して、`security-extended` クエリでビルドしたソースを解析します。
- Python：[`Native Security`](../.github/workflows/native-security.yml) の独立したマトリクスジョブが `native/worker`、`native/tests`、`crop_editor`、`scripts`、`gaussian-splatting`、`training_kit` を検査します。同梱 SIBR と CUDA サブモジュールは対象外です。Torch/CUDA や追加研究ランタイムはインストールしません。
- GitHub Actions：マトリクスの `actions` ジョブが `.github/workflows` を検査します。両インタープリター言語のジョブは `build-mode: none` と既定のセキュリティクエリを使い、解析カテゴリを分けてアップロードします。

追加スキャンの対象は `agent/native-desktop-0.3` への push と、このデスクトップブランチを head とする pull request です。手動実行も同じ ref に限定します。pull request は GitHub の merge commit を検査します。`workflow_dispatch` の利用可否には、GitHub のワークフロー登録およびデフォルトブランチ要件が適用されます。このブランチの自動起動経路は push です。旧 HTML の `main` は変更しません。定期スキャンも保証しません。GitHub の `schedule` はワークフローがデフォルトブランチに存在する必要があります。最初の実行では YAML の構文だけでなく、初期化、抽出/ビルド、解析、SARIF アップロードまで確認します。

結果は **Security and quality → Code scanning** でブランチと解析カテゴリを指定して確認します。アップロード成功は検査実行の確認であり、脆弱性がないことの保証ではありません。実際のアラートは到達経路とリスクを調べて修正します。クエリ除外、アラートの非表示、失敗履歴の削除を修正の代わりにしません。

### 依存関係アラートの範囲

Dependabot alerts は管理者が設定で有効にする別機能です。`dependabot.yml` は不要で、自動バージョン更新とも異なります。対応する依存関係グラフと審査済みアドバイザリを使って**デフォルトブランチ**を検査します。旧 HTML `main` のアラートは、デスクトップ開発ブランチの固定ランタイムを監査した結果ではありません。Python/CUDA 拡張、Conda 環境、外部 COLMAP/OpenMVS、モデルの重み、ローカルに実際に導入したバージョンを完全に検査できるとは限りません。ランタイム更新時は 3DGS、2DGS、メッシュバックエンドごとに互換性を検証し、QA 用やデフォルトブランチ用の依存関係を学習環境と同一視しません。

シークレットスキャンと push protection は GitHub の提供機能、権限、リポジトリ設定に依存し、CodeQL や Dependabot とは別の検査境界です。設定画面とアラート画面で状態を確認してください。アクセストークン、パスワード、ローカル環境の認証情報をコミットしないでください。

## Official references / 官方资料 / 公式資料

- [CodeQL workflow configuration](https://docs.github.com/en/code-security/reference/code-scanning/workflow-configuration-options)
- [CodeQL init inputs](https://github.com/github/codeql-action/blob/v4/init/action.yml) and [analyze inputs](https://github.com/github/codeql-action/blob/v4/analyze/action.yml)
- [Dependabot alerts and coverage](https://docs.github.com/en/code-security/concepts/supply-chain-security/dependabot-alerts)
- [Events that trigger workflows](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows)
