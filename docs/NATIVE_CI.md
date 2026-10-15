# Native CI and security detection / 原生持续集成与安全检测 / ネイティブ CI とセキュリティ検出

## Upstream adaptation acceptance / 源码移植验收 / ソース移植の検証

中文：`Upstream viewport acceptance` 并行执行三语言真实滚轮／平移／菜单操作、
小样本真实点云分页及两处纯模块回归，上传实际帧缓冲和日志。它没有 CodeQL
跟踪构建的耗时，但不替代原全量构建、107 项原生测试、worker 或安全扫描。
`GSW_FORCE_POINT_PAGING=1` 仅是默认关闭的诊断入口；常规模型加载不改变。
详见 [来源、范围与测量边界](UPSTREAM_NATIVE_UPGRADES.md)。

English: `Upstream viewport acceptance` runs real wheel/pan/menu operations in
three locales, controlled actual point paging and both pure adapter regressions
in parallel. It uploads framebuffers/logs without the traced CodeQL build cost,
but does not replace the full build, 107 native tests, worker or security scans.
`GSW_FORCE_POINT_PAGING=1` is default-off diagnostic input, not a production
loading-policy change. See [scope and measurement limits](UPSTREAM_NATIVE_UPGRADES.md).

日本語：`Upstream viewport acceptance` は三言語の実際のホイール・パン・メニュー
操作、小サンプルの実点群ページング、両アダプターの回帰を並行して実行し、
フレームバッファとログを保存します。CodeQL トレースの時間は不要ですが、
全ビルド・原生 107 テスト・worker・安全検査の代替ではありません。
`GSW_FORCE_POINT_PAGING=1` は既定無効の診断で、通常の読込方針は変えません。

## GPL Blender geometry and independent surfel gate (2026-10-14)

中文：主程序按 GPL-3.0-or-later 分发，并附对应源码与哈希清单。Blender 锥头／
方块几何实际移植，原受限 2DGS 显示着色器不再嵌入，替换为论文公式独立实现。
新增 `native_surfel_projection_gl`：隐藏 OpenGL 上下文实际编译、执行 GLSL，
19 项覆盖包围范围、求交、透视／正交深度和极端尺度。在 NVIDIA 和 Mesa llvmpipe
均通过。既有 3DGS／2DGS 视口、三语言、变换和 worker 测试全部保留。

English: GPL-3.0-or-later desktop packages include matching source and a hash
manifest. Blender cone/box geometry is actually ported. The restricted embedded
2DGS display shader is replaced by an independent paper-based implementation.
`native_surfel_projection_gl` compiles and executes real GLSL in a hidden OpenGL
context: nineteen bounds, intersection, perspective/orthographic depth and
extreme-scale cases pass on NVIDIA and Mesa llvmpipe. All existing 3DGS/2DGS
viewport, three-language, transformation and worker tests remain enabled.

日本語：GPL-3.0-or-later の主プログラム配布物に対応ソースとハッシュ一覧を
同梱します。Blender の円錐／ブロック形状を実際に移植し、制限付き 2DGS 描画
シェーダーの埋め込みを、論文に基づく独立実装へ置き換えます。追加の
`native_surfel_projection_gl` は非表示 OpenGL コンテキストで実際に GLSL を
コンパイル・実行します。範囲・交点・透視／平行深度・極端スケールの 19 項目が
NVIDIA と Mesa llvmpipe で通過しました。既存の視口・三言語・変換・worker
検査はすべて維持します。

## 简体中文（zh_CN）

### 构建与回归测试

`Native Windows`（[工作流](../.github/workflows/native-windows.yml)）在 Windows Server 2022 / MSVC 2022 上配置、构建并测试 Qt 6.8 原生桌面端，再部署运行库、验证原生安装包并上传 artifact。失败的历史运行会保留；只能将对应提交的新运行成功视为修复完成，不能通过关闭测试消除红叉。

Qt 安装器的 Python 与测试后端的 Python 分开管理。测试固定使用 Python 3.12，安装 [`scripts/requirements-native-checks.txt`](../scripts/requirements-native-checks.txt)，并把同一解释器的绝对路径传给 CMake、worker 测试及安装包 Python 语法检查。NumPy/Pillow 的 CMake 预检仍保留。原生视口测试使用软件 OpenGL，训练预检与 worker 回归使用受控样本或测试自有进程，不启动本机真实训练。artifact 可用不等于本机所有 CUDA 训练与可选网格运行库已验证。

### Windows CI 图形与显示预检

修复 Python 依赖后，`Native Windows` 第 #141 次运行进入了后续测试，并暴露 runner 的图形能力与显示尺寸问题：Qt 默认软件 OpenGL 的 GLSL 上限为 1.30，无法编译原生视口要求的 `#version 330` 着色器；过窄的桌面也无法满足 1400 像素宽的真实 UI 测试边界。这不是应当跳过测试的理由，下面的 bootstrap 改为检查并准备 CI 所需能力。

- [`setup_native_ci_graphics.ps1`](../scripts/setup_native_ci_graphics.ps1) 固定下载 Mesa **26.2.4 MSVC** 发布包，先核对 SHA-256 `351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695`，再通过 `QT_OPENGL_DLL` 与 `GALLIUM_DRIVER=llvmpipe` 选择本次 CI 的软件驱动。它不覆盖系统 DLL、不替换用户或应用运行库，也不把 Mesa 放入应用 artifact；没有伪造 OpenGL/GLSL 版本号。
- [`setup_native_ci_display.ps1`](../scripts/setup_native_ci_display.ps1) 只有在 `GITHUB_ACTIONS=true` 时才允许修改显示模式。它枚举真实支持的 32-bpp 模式，通过 `CDS_TEST` 后应用至少 **1600×900** 的模式，并核实实际结果；不安装显示驱动，也不修改分辨率注册表。工作站只能使用 `-InspectOnly` 枚举与检查，绝不改变本地显示模式。
- 全量测试前运行真实软件内存探测 `system_resource_probe`，以及 3 个已有图形入口：`native_infinite_reference_grid`、`native_light_gaussian_interaction`、`native_textured_paged_mesh_scene`。bootstrap 或这组测试失败时不进入昂贵的全量测试。**原有 98 个测试全部保留，新增 1 个资源探测测试，共 99 个**，通过预检后完整执行；没有降低图像、交互或 UI 断言。

这些是针对已证明的失败所准备的 CI 配置修复，不代表新运行已经通过。仍须在修复提交对应的真实 Actions 中确认预检、完整测试、安装包上传以及代码扫描上传。

软件渲染可能比实际 GPU 慢。高斯交互测试仍保留 512202 个高斯、160 帧及完整图像／驻留／撤销断言，只将深浅色两项的完成时限设为 600 秒；测试提前完成就立即结束，不改变 FPS 或性能阈值。包含编译追踪与安装包上传的任务总时限为 90 分钟，为实际出现的 20–35 分钟编译及全套测试留足时间。烟雾测试输出实际 OpenGL 渲染器信息，不能仅凭环境变量认定驱动已生效。

实际 `llvmpipe`／`softpipe` 使用系统内存，不应把其 NVX 扩展返回值当成独立显存。资源探测明确保留 GPU 统计“未知”，继续测量真实 RAM；自动安全回退与手动预算语义保持不变。真实硬件 DXGI/NVX 的零余量保护不变。真实无窗口上下文回归防止软件读数随机变化导致连小网格都被拒绝；详见当前 Mesa revision 的[软件内存查询](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9a0169f2a7/src/gallium/auxiliary/util/u_helpers.c)与[NVX 查询实现](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9a0169f2a7/src/mesa/main/get.c)。

分页网格还通过同一真实视口的串行／同时导入回归验证：两个单独可完整加载、合计超过 64 MiB 的模型必须一起显示、归还加载预留，并在静止的连续 12 帧保持稳定。请求只包含当前缺失的目标页；没有可绘制覆盖时增加一个根页作为冷启动回退，不再读取整条祖先链。共享预算、原有回退绘制和显存保护不变。移除常驻模型后，剩余模型仍须自动提升为完整加载。此项适用于各生成流程的网格成果显示，不改变训练参数。

集合事务完成后立即重平衡共享资源，不等待周期回调。资源采样保留 1000 ms 间隔，使用精确定时器，避免 Windows 低优先级 `WM_TIMER` 在连续视口更新时饥饿；[Qt 实现](https://raw.githubusercontent.com/qt/qtbase/6.8/src/corelib/kernel/qeventdispatcher_win.cpp)和 [Windows 说明](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-timer)支持这一调度区别。`GSW_MESH_PAGING_QA_DIAGNOSTICS=1` 可开启有界开发诊断，默认关闭；不修改预算、训练保护或验收期限。

### 持续代码扫描

使用 CodeQL advanced setup，而不是同时开启另一套 default setup：

- C/C++：在 `Native Windows` 的真实原生构建前初始化 CodeQL，采用 `manual` 模式追踪编译，使用 `security-extended` 查询集分析所构建的源代码。
- Python：[`Native Security`](../.github/workflows/native-security.yml) 的独立矩阵任务检查 `native/worker`、`native/tests`、`native/backend`、`scripts`、`gaussian-splatting` 与 `training_kit`；不包含 vendored SIBR 或 CUDA 子模块，不安装 Torch/CUDA 或可选研究运行库。
- GitHub Actions：同一矩阵中的 `actions` 任务检查 `.github/workflows`。Python 与 Actions 都使用 `build-mode: none`，保留默认安全查询，分别上传分析类别。

当前原生桌面端以默认 `main` 为开发与发布入口。代码扫描覆盖 `main` 的 push、以 `main` 为目标分支的 pull request，以及该 ref 的手动运行；PR 使用 GitHub 的 merge commit，因此功能分支无需取名为 `main`。`workflow_dispatch` 通过默认分支工作流注册；是否定时执行以实际 workflow 的 `schedule` 为准。验收必须核实初始化、提取/构建、分析与 SARIF 上传，而非仅验证 YAML 能解析。

结果在仓库 **Security and quality → Code scanning** 中按分支与分析类别查看。成功上传只表示检测已执行，并不保证没有漏洞。真实告警需要检查触发路径与风险后修复；不通过过滤查询、隐藏告警或删除历史失败记录冒充修复。

### 共享后端安全回归

Windows worker 检查与 `Native Security` 的独立 Ubuntu 回归任务运行 `native/backend/tests` 及原生 worker 测试，直接调用计算入口并使用测试自有子进程，验证目录边界、合法 3DGS/2DGS/网格读写、命令参数和取消。Ubuntu 还覆盖 Windows 可能因权限不可用的文件符号链接。样本位于 runner 临时目录，不读取原始研究工程，也不启动真实训练。

模型 `cfg_args` 只接受 `Namespace` 及字面值参数，不再使用 `eval`。实验查看回归同时验证合法参数、原始 Unicode 路径与列表/字典不变，以及查看不可信配置时不执行表达式。目录边界在读写、缓存和预览生成前检查，而不只在最终下载时检查。Windows 比较保留大小写不敏感语义，但返回真实路径大小写；不能为消除静态分析告警而改成错误的路径行为。任何残余告警仍须按实际数据流核查，不自动忽略。

导入的原始副本和工作图像写入前也检查候选叶子路径，拒绝指向目录外、包括目标尚不存在的符号链接。用户明确选择的外部照片／视频仍允许读取；这是本地导入功能，不应把来源强制限制到工程目录。测试同时保留正常外部导入与拒绝越界写入的行为。

原生后端不再保留 Node 转换器或浏览器用导出入口。SPZ 使用 C++ 编解码器，后端与安装包测试确认原生依赖完整，同时拒绝重新引入旧网页、Electron、Node 清单与 HTTP 监听入口。计算进程的参数和取消边界仍需验证；环境就绪报告必须与实际启动条件一致。

原生 worker 通过自有进程直接导入用户选定的外部文件。HTTP 服务与路由已删除，因此不再运行已退役路由的 Host／Origin／上传协议测试；仍在使用的路径约束、符号链接防逃逸、配置解析、导入事务和取消行为在直接计算入口验证。后端不是网络共享服务，也不承担对不可信本地进程的隔离。

退出确认回归的异步状态通过共享所有权存活至最后一个回调，不引用已结束作用域的布尔变量。每次 `close()` 完整返回后才发起下一次关闭，避免模态事件循环中的关闭重入；只点击已经可见的真实对话框，保留默认／escape 取消按钮、保存提示和取消后重新确认的全部断言。整体 watchdog 为 10 秒，CTest 上限仍为 15 秒。网格图像对比先完成一次真实首帧 readback，再等待实际 +Z 视角到达并取景，不把固定 300 ms 等待当成动画已完成，也不强制推进动画。加载计时仍从 `setScene()` 前开始，首帧开销计入加载耗时；默认策略受控样本完整显示的 10 秒性能门槛、+Z 的 1500 ms 等待、相机容差和至少 90% 的连续表面覆盖率均保留。不更改生产退出或导航逻辑。

`native_language_zh_CN`、`native_language_en_US`、`native_language_ja_JP` 的完整语言／外观／缩放／表单／窗口矩阵完成上限由 90 秒调整为 300 秒，各阶段有界等待与全部断言不变；这不是放宽模型加载、FPS 或其他性能阈值。测试自有窗口先满足已呈现（exposed）与视口初始化前置条件，并取得真实非空白首帧。外观验收保留 alpha 不透明性与原有 GL clear RGBA 检查，还验证 framebuffer 四个固定侧部区域：每区至少 70% 的像素在 RGB 各通道 ±2 的容差内匹配预期背景，用实际画面验收，而不只检查内部 GL 状态。矩阵使用测试自有的 `lifetime-worker`：寿命由 stdin 管道、EOF 或测试显式关闭控制，不再在 60 秒后自然退出，作用域清理保证失败或提前返回时也释放自有进程；原 `tree-child` 模式保留供进程树测试使用。阶段计时和失败诊断属于开发者烟雾测试输出，按[本地化规则](LOCALIZATION.md)保持英文，不替代三语言用户界面验证。修复仍须由对应新提交的真实 Actions 完整运行验收，不能据此宣称已全绿。

### 依赖漏洞告警的边界

Dependabot alerts 是管理员在仓库设置中启用的独立功能，不依赖 `dependabot.yml`，也不等于自动版本升级。它根据 GitHub 支持的依赖图和已审核公告检查默认分支 `main`，现在对应原生桌面端源码。旧网页依赖移除后，仍需检查原生分支实际声明的依赖；Python/CUDA 扩展、Conda 环境、外部 COLMAP/OpenMVS、模型权重及本机安装版本不保证被依赖图完整覆盖。运行库升级仍分别验证 3DGS、2DGS 与网格后端，QA 依赖不能当作实际训练环境。

密钥扫描/推送保护由 GitHub 仓库功能、权限和配置决定，与 CodeQL 或 Dependabot 是不同检测边界；应在设置页和告警页核实状态。不要提交访问令牌、密码或本机运行库凭据。

## English (en_US)

### Build and regression checks

`Native Windows` ([workflow](../.github/workflows/native-windows.yml)) configures, builds and tests the Qt 6.8 desktop application on Windows Server 2022 / MSVC 2022, deploys its runtime, validates the native package and uploads an artifact. Historical failures remain historical; completion requires a successful new run for the repaired commit, not disabled tests.

Qt's installer Python is separate from the QA interpreter. QA uses Python 3.12 with [`scripts/requirements-native-checks.txt`](../scripts/requirements-native-checks.txt); its absolute path is passed to CMake, worker tests and packaged Python syntax checks. The NumPy/Pillow configure guard remains enabled. Native viewport checks use software OpenGL; training preflight and worker regression use controlled samples or test-owned processes, without launching a user's real training. A downloadable artifact does not certify every local CUDA or optional meshing runtime.

### Windows CI graphics and display preflight

After the Python dependency fix, `Native Windows` run #141 reached downstream tests and exposed runner graphics and display limitations: Qt's default software OpenGL supported GLSL only up to 1.30 and could not compile the viewport's `#version 330` shaders; a narrow desktop also could not satisfy the real UI checks' 1400-pixel-width boundary. These are capability requirements to prepare and verify, not reasons to skip tests.

- [`setup_native_ci_graphics.ps1`](../scripts/setup_native_ci_graphics.ps1) pins the **Mesa 26.2.4 MSVC** release and verifies SHA-256 `351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695` before selecting the CI software driver through `QT_OPENGL_DLL` and `GALLIUM_DRIVER=llvmpipe`. It does not overwrite system DLLs, replace user/application runtimes or ship Mesa in the application artifact. OpenGL/GLSL version numbers are not spoofed.
- [`setup_native_ci_display.ps1`](../scripts/setup_native_ci_display.ps1) permits display changes only with `GITHUB_ACTIONS=true`. It enumerates supported 32-bpp modes, requires a successful `CDS_TEST`, applies a mode of at least **1600×900** and verifies the actual result. It installs no display driver and makes no resolution registry changes. Workstations may only use `-InspectOnly`, which enumerates and inspects without changing the local mode.
- The real software-memory check `system_resource_probe` and three existing graphics checks run first: `native_infinite_reference_grid`, `native_light_gaussian_interaction` and `native_textured_paged_mesh_scene`. A bootstrap or preflight failure prevents the expensive full suite from starting. **All 98 existing tests remain, plus one new resource-probe test: 99 total**, and run after the preflight succeeds; image, interaction and UI assertions are not weakened.

These prepared CI changes address proven failures; they are not a claim that a new run has already passed. Acceptance still requires the repaired commit's real Actions preflight, full suite, package upload and code-scanning upload.

Software rendering may be slower than a physical GPU. Gaussian interaction retains 512202 Gaussians, 160 frames and every image/residency/undo assertion; only the dark/light checks' completion timeout is 600 seconds. Successful checks finish immediately; FPS and performance thresholds are unchanged. The job's 90-minute total limit accommodates observed 20–35-minute traced builds, full regressions and package upload. Smoke diagnostics report the actual OpenGL renderer, rather than assuming a driver is active from environment variables alone.

Actual `llvmpipe`/`softpipe` contexts use system memory; their NVX fields must not be treated as dedicated VRAM. GPU statistics remain explicitly unavailable while real RAM probing continues. Automatic safe fallback and manual-budget semantics are unchanged, as is real hardware's DXGI/NVX zero-headroom protection. Real windowless-context regression prevents unstable software counters from refusing even tiny meshes. See the pinned Mesa revision's [software memory query](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9a0169f2a7/src/gallium/auxiliary/util/u_helpers.c) and [NVX implementation](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9a0169f2a7/src/mesa/main/get.c).

Paged meshes also use the same real-viewport serial/same-batch regression: two models that each fit alone but exceed a combined 64 MiB must render together, return loading reservations, and remain stable for 12 consecutive static frames. Requests contain missing desired pages only, with one root page as a cold-start fallback when no drawable coverage exists, instead of reading every ancestor. Shared caps, existing fallback drawing and GPU protections remain unchanged. Removing the resident model must still promote the remaining model without reimport. This covers mesh results from all generation pipelines without changing training parameters.

Rebalance shared resources immediately after the collection transaction. Sampling retains its 1000 ms interval but uses a precise timer to avoid starvation in the low-priority Windows `WM_TIMER` path during continuous viewport updates; see the [Qt implementation](https://raw.githubusercontent.com/qt/qtbase/6.8/src/corelib/kernel/qeventdispatcher_win.cpp) and [Windows documentation](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-timer). Bounded developer diagnostics require `GSW_MESH_PAGING_QA_DIAGNOSTICS=1` and are off by default. Budgets, training protections and acceptance deadlines are unchanged.

### Ongoing code scanning

Use CodeQL advanced setup rather than enabling a second default setup:

- C/C++: `Native Windows` initializes CodeQL before the real native build, traces compilation with `manual` build mode and analyzes the built sources with the `security-extended` query suite.
- Python: an independent [`Native Security`](../.github/workflows/native-security.yml) matrix job covers `native/worker`, `native/tests`, `native/backend`, `scripts`, `gaussian-splatting` and `training_kit`. Vendored SIBR and CUDA submodules are excluded; Torch/CUDA and optional research runtimes are not installed.
- GitHub Actions: the matrix's `actions` job checks `.github/workflows`. Both interpreted jobs use `build-mode: none`, retain the default security queries and upload separate analysis categories.

The default `main` branch is the native desktop development and release entry point. Scanning covers pushes to `main`, pull requests targeting `main`, and manual runs on that ref. Pull requests use GitHub's merge commit, so feature branches do not need to be named `main`. The default-branch workflow registers `workflow_dispatch`; scheduled execution exists only if the actual workflow declares `schedule`. Verify initialization, extraction/build, analysis and SARIF upload, not only YAML parsing.

Inspect **Security and quality → Code scanning** by branch and analysis category. A successful upload means the detection ran, not that the repository is vulnerability-free. Investigate the reachable path and risk of real findings before fixing them; do not substitute query exclusions, hidden alerts or deleted failed runs for remediation.

### Shared-backend security regression

Both the Windows worker checks and an independent Ubuntu regression job in `Native Security` run `native/backend/tests` and native worker checks. Direct compute calls and test-owned subprocesses verify directory boundaries, legitimate 3DGS/2DGS/mesh IO, command arguments and cancellation. Ubuntu also covers file symbolic links that Windows privileges may prevent. Fixtures live in the runner's temporary directory, without reading original research projects or starting real training.

Model `cfg_args` accepts only `Namespace` and literal keyword values; `eval` is no longer used. Experiment-view checks preserve legitimate values, original Unicode paths, lists and dictionaries while verifying that untrusted expressions never execute. Directory boundaries are checked before reads, writes, cache and preview generation, not only at the final download. Windows comparisons remain case-insensitive while returning the actual path spelling; runtime correctness must not be sacrificed to silence static-analysis findings. Remaining findings still require reachable-flow review and are not ignored automatically.

Candidate leaf paths are checked before writing imported originals or working images, including dangling symbolic links targeting outside the destination directory. Explicitly selected external photos/videos remain readable: local import must not restrict its source to the project directory. Regression checks preserve legitimate external imports while rejecting escaping writes.

The native backend no longer retains the Node converter or browser export routes. SPZ uses the C++ codec. Backend and package checks verify required native dependencies and reject retired web/Electron assets, Node manifests and HTTP listener entry points. Compute-process arguments and cancellation remain tested; readiness reports must match actual launch requirements.

Native workers import explicitly chosen external paths through owned processes. HTTP listeners and routes have been removed, so tests of retired Host/Origin/upload protocols are retired with them. Live path containment, symbolic-link escape prevention, configuration parsing, import transactions and cancellation are tested through direct compute entry points. This backend is not a network-sharing service or isolation from untrusted local processes.

Exit-confirmation regression state has shared ownership until the last asynchronous callback instead of referencing expired block-local booleans. Each `close()` must return fully before the next close is issued, avoiding close reentry within a modal event loop. Checks click visible real dialogs and retain every assertion for the default/escape cancel buttons, the save prompt, and renewed confirmation after cancellation. The overall watchdog is 10 seconds and the CTest limit remains 15 seconds. Mesh-image comparisons complete one real first-frame readback before awaiting the actual +Z view and framing; neither a fixed 300 ms delay nor forced animation advancement stands in for completion. Loading time still starts before `setScene()`, including the first-frame cost. The bounded default-policy sample's 10-second complete-display performance gate, 1500 ms +Z wait, camera tolerances and at least 90% continuous-surface coverage remain unchanged. Production exit/navigation behavior is not modified.

The complete language/appearance/scale/form/window matrices in `native_language_zh_CN`, `native_language_en_US` and `native_language_ja_JP` have a 300-second completion limit instead of 90 seconds, retaining every phase's bounded wait and all assertions. This does not relax model-loading, FPS or other performance thresholds. The test-owned window first meets exposed-window and initialized-viewport prerequisites and produces a real nonblank first frame. Appearance checks retain alpha opacity and the original GL clear-RGBA assertions, and also inspect four fixed framebuffer side regions: at least 70% of each region's pixels must match the expected background within ±2 per RGB channel. Acceptance therefore measures the actual displayed image, not only internal GL state. The matrices use a test-owned `lifetime-worker`, whose lifetime follows the stdin pipe, EOF or explicit test shutdown instead of expiring naturally after 60 seconds. Scope cleanup releases the owned process on failure or early return; the existing `tree-child` mode remains for process-tree tests. Stage timings and failure diagnostics are English developer-only smoke output under the [localization rules](LOCALIZATION.md), not a replacement for three-language UI verification. Acceptance still requires a complete real Actions run for the repaired commit; these scheduling changes do not claim an all-green result.

### Dependency-alert coverage

Dependabot alerts are a separate administrator-enabled repository setting; they do not require `dependabot.yml` and are not automatic version upgrades. They inspect the default `main` branch, now the native desktop source, using supported dependency graphs and reviewed advisories. Removing old web dependencies does not remove the need to review declared native dependencies. Python/CUDA extensions, Conda environments, external COLMAP/OpenMVS, model weights and actual local installations are not guaranteed complete graph coverage. Runtime upgrades still require separate 3DGS, 2DGS and mesh-backend checks; QA dependencies do not represent the training runtime.

Secret scanning and push protection depend on GitHub features, permissions and repository configuration and are separate from CodeQL and Dependabot. Verify their state in settings and alert pages. Never commit access tokens, passwords or local runtime credentials.

## 日本語（ja_JP）

### ビルドと回帰テスト

`Native Windows`（[ワークフロー](../.github/workflows/native-windows.yml)）は Windows Server 2022 / MSVC 2022 で Qt 6.8 デスクトップ版を構成・ビルド・テストし、ランタイムを配置してネイティブパッケージを検証し、artifact をアップロードします。過去の失敗履歴は残します。修正対象コミットの新しい実行が成功して初めて完了と判断し、テストの無効化を修正の代わりにしません。

Qt インストーラーの Python と QA 用 Python を分離します。QA は Python 3.12 と [`scripts/requirements-native-checks.txt`](../scripts/requirements-native-checks.txt) を使用し、同じインタープリターの絶対パスを CMake、worker テスト、パッケージ内 Python の構文検証に渡します。NumPy/Pillow の構成時検査は維持します。ネイティブビューポートはソフトウェア OpenGL で検証し、学習の事前検査と worker 回帰テストは管理されたサンプルやテスト専用プロセスを使います。ユーザーの実際の学習は開始しません。artifact の取得可能性は、すべてのローカル CUDA 環境や追加メッシュ環境の検証を意味しません。

### Windows CI のグラフィックスと画面の事前検査

Python 依存関係の修正後、`Native Windows` の実行 #141 は後続テストに進み、runner のグラフィックス能力と画面サイズの不足が判明しました。Qt 既定のソフトウェア OpenGL は GLSL 1.30 までしか対応せず、ビューポートの `#version 330` シェーダーをコンパイルできませんでした。また、狭いデスクトップでは実際の UI テストが要求する幅 1400 ピクセルを満たせません。テストを省略するのではなく、必要な能力を準備・確認します。

- [`setup_native_ci_graphics.ps1`](../scripts/setup_native_ci_graphics.ps1) は **Mesa 26.2.4 MSVC** リリースに固定し、SHA-256 `351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695` を検証してから、`QT_OPENGL_DLL` と `GALLIUM_DRIVER=llvmpipe` で CI 用ソフトウェアドライバーを選びます。システム DLL、ユーザー環境、アプリのランタイムは置き換えず、Mesa をアプリ artifact に含めません。OpenGL/GLSL のバージョン番号も偽装しません。
- [`setup_native_ci_display.ps1`](../scripts/setup_native_ci_display.ps1) は `GITHUB_ACTIONS=true` の場合だけ画面モードの変更を許可します。実際に対応する 32-bpp モードを列挙し、`CDS_TEST` が成功した **1600×900** 以上のモードを適用して結果を確認します。ディスプレイドライバーを導入せず、解像度のレジストリも変更しません。ワークステーションでは `-InspectOnly` だけを使い、列挙・確認のみでローカル画面モードを変更しません。
- 全テストの前に、実際のソフトウェアメモリ検査 `system_resource_probe` と既存の実グラフィックス検査 3 件（`native_infinite_reference_grid`、`native_light_gaussian_interaction`、`native_textured_paged_mesh_scene`）を実行します。bootstrap または事前検査が失敗した場合、負荷の大きい全テストを開始しません。**既存 98 件をすべて維持し、資源検査 1 件を追加して計 99 件**を事前検査の成功後に実行します。画像、操作、UI の検証条件は緩和しません。

これらは確認済みの失敗に対して準備した CI 設定修正であり、新しい実行の成功を示すものではありません。修正コミットの実際の Actions で、事前検査、全テスト、パッケージおよびコードスキャンのアップロードまで確認する必要があります。

ソフトウェア描画は実 GPU より遅い場合があります。ガウシアンの操作テストは 512202 個、160 フレーム、画像・常駐・取り消しの全検証を維持し、ライト／ダークの完了タイムアウトだけを 600 秒にします。完了すれば直ちに終了し、FPS や性能基準は変更しません。実測で 20–35 分かかったコンパイル追跡、全テスト、アップロードの時間を確保し、ジョブの上限は 90 分です。スモークテストの診断で実際の OpenGL レンダラーを報告し、環境変数だけでドライバーの有効化を判断しません。

実際の `llvmpipe`／`softpipe` はシステムメモリを使うため、NVX の値を専用 GPU メモリとして扱いません。GPU 統計を明示的に取得不能とし、実際の RAM は引き続き測定します。自動の安全な代替予算、手動予算、実 GPU の DXGI/NVX による余量ゼロの保護は維持します。表示ウィンドウのない実コンテキストで、不安定なソフトウェア統計による小さなメッシュの読み込み拒否を防ぐ回帰検査を実行します。固定 Mesa revision の[ソフトウェアメモリ検査](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9a0169f2a7/src/gallium/auxiliary/util/u_helpers.c)と[NVX 実装](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/9a0169f2a7/src/mesa/main/get.c)を参照してください。

ページ化メッシュは同じ実ビューポートで逐次／同時インポートを検査します。単独では完全ロードでき、合計では 64 MiB を超える 2 モデルを同時に表示し、ロード用の予約を返し、静止した連続 12 フレームでも安定していることを確認します。要求するのは現在不足する目標ページのみで、描画可能な範囲がない場合に限り初期表示用のルートページを 1 つ追加します。祖先チェーン全体を読み直しません。共有上限、既存の代替描画、GPU 保護は維持します。常駐モデルを除くと、残ったモデルは再インポートせず完全ロードへ昇格する必要があります。すべての生成パイプラインのメッシュ成果表示が対象で、学習パラメーターは変更しません。

集合の更新処理が完了したら共有資源を直ちに再配分します。資源の観測間隔は 1000 ms のまま、精確タイマーを使い、連続描画による低優先度の Windows `WM_TIMER` の遅延を避けます。[Qt 実装](https://raw.githubusercontent.com/qt/qtbase/6.8/src/corelib/kernel/qeventdispatcher_win.cpp)と [Windows の説明](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-timer)を参照してください。有界な開発用診断は `GSW_MESH_PAGING_QA_DIAGNOSTICS=1` の場合のみ有効で、既定では無効です。予算、学習の保護、検証期限は変更しません。

### 継続的なコードスキャン

CodeQL の advanced setup を使い、別の default setup を同時に有効化しません。

- C/C++：`Native Windows` の実際のネイティブビルド前に CodeQL を初期化し、`manual` モードでコンパイルを追跡して、`security-extended` クエリでビルドしたソースを解析します。
- Python：[`Native Security`](../.github/workflows/native-security.yml) の独立したマトリクスジョブが `native/worker`、`native/tests`、`native/backend`、`scripts`、`gaussian-splatting`、`training_kit` を検査します。同梱 SIBR と CUDA サブモジュールは対象外です。Torch/CUDA や追加研究ランタイムはインストールしません。
- GitHub Actions：マトリクスの `actions` ジョブが `.github/workflows` を検査します。両インタープリター言語のジョブは `build-mode: none` と既定のセキュリティクエリを使い、解析カテゴリを分けてアップロードします。

既定の `main` がネイティブ版の開発・配布入口です。`main` への push、`main` を対象とする pull request、同 ref の手動実行を検査します。PR は GitHub の merge commit を使うため、機能ブランチを `main` と名付ける必要はありません。`workflow_dispatch` は既定ブランチの workflow で登録し、定期実行の有無は実際の `schedule` 設定に従います。YAML 構文だけでなく、初期化、抽出/ビルド、解析、SARIF アップロードを確認します。

結果は **Security and quality → Code scanning** でブランチと解析カテゴリを指定して確認します。アップロード成功は検査実行の確認であり、脆弱性がないことの保証ではありません。実際のアラートは到達経路とリスクを調べて修正します。クエリ除外、アラートの非表示、失敗履歴の削除を修正の代わりにしません。

### 共有バックエンドのセキュリティ回帰テスト

Windows の worker 検査と `Native Security` の独立した Ubuntu 回帰は、`native/backend/tests` とネイティブ worker を検査します。計算入口の直接呼出しとテスト専用子プロセスで、ディレクトリ境界、正規の 3DGS/2DGS/メッシュ入出力、引数、キャンセルを確認します。Ubuntu は Windows の権限制限で使えないファイルシンボリックリンクも対象にします。サンプルは runner の一時ディレクトリに置き、元の研究データを読み込まず、実際の学習を開始しません。

モデルの `cfg_args` は `Namespace` とリテラルのキーワード値だけを受け入れ、`eval` は使いません。実験表示の検査では、正規の値、元の Unicode パス、リスト、辞書を保持し、不信な式を実行しないことを確認します。ディレクトリ境界は最終ダウンロードだけでなく、読み書き、キャッシュ、プレビューの生成前に検査します。Windows は大文字小文字を区別しない比較を維持し、戻り値の実際の表記は保存します。静的解析の警告を消すために正常動作を壊してはいけません。残りのアラートも到達経路を調べ、自動的には無視しません。

読み込んだ元データのコピーと作業画像を書き込む前に、候補の末端パスも検査します。保存先の外部を指すシンボリックリンクは、参照先が未作成でも拒否します。ユーザーが明示的に選んだ外部の写真・動画は引き続き読み込めます。ローカル読み込みの元データをプロジェクト内に制限しません。正規の外部読み込みと、範囲外への書き込み拒否を両方検証します。

ネイティブバックエンドから Node 変換器とブラウザー用の出力ルートを削除し、SPZ は C++ コーデックを使います。バックエンドとパッケージ検査は必要な依存関係を確認し、旧 Web/Electron、Node の依存一覧、HTTP リスナーの再導入を拒否します。計算プロセスの引数と取消は引き続き検査し、準備状況は実際の起動条件に合わせます。

ネイティブ worker は自身が所有するプロセスで選択された外部ファイルを直接取り込みます。HTTP リスナーとルートを削除したため、廃止済み Host/Origin/アップロードプロトコルの検査も終了します。現在使うパス境界、シンボリックリンクによる逸脱防止、設定解析、取込トランザクション、取消は直接の計算入口で確認します。バックエンドはネットワーク共有サービスでも、不信なローカルプロセスを隔離する仕組みでもありません。

終了確認の回帰テスト状態は最後の非同期コールバックまで共有所有され、寿命が終わったブロック内の bool を参照しません。各 `close()` が完全に戻ってから次の終了操作を開始し、モーダルイベントループ中の終了処理への再入を避けます。表示済みの実ダイアログをクリックし、既定／escape のキャンセルボタン、保存確認、キャンセル後の再確認に関する全検証条件を維持します。全体の watchdog は 10 秒、CTest 上限は 15 秒のままです。メッシュ画像比較は最初の実フレームを一度 readback してから、実際の +Z 視点への到達を待ってフレーミングします。固定 300 ms の待機を完了と見なさず、アニメーションも強制的に進めません。読み込み時間は引き続き `setScene()` の直前から測り、最初のフレームの処理時間も含めます。既定ポリシーの管理サンプルに適用する完全表示 10 秒の性能基準、+Z の 1500 ms 待機、カメラ許容誤差、90% 以上の連続面被覆率は維持し、製品の終了／ナビゲーション動作は変更しません。

`native_language_zh_CN`、`native_language_en_US`、`native_language_ja_JP` の言語／外観／拡大率／フォーム／ウィンドウの完全マトリクスは、完了上限を 90 秒から 300 秒に変更します。各段階の有界な待機と全検証条件は維持し、モデル読み込み、FPS などの性能基準を緩めません。テスト専用ウィンドウが実際に表示され（exposed）、ビューポートが初期化済みであることを先に確認し、空白ではない実際の最初のフレームを取得します。外観検査では alpha の不透明性と従来の GL clear RGBA 条件を維持し、framebuffer 側部の固定 4 領域も検査します。各領域の画素の 70% 以上が、RGB 各チャンネル ±2 の許容差で期待する背景色に一致する必要があります。内部 GL 状態だけでなく、実際に描かれた画面で検証します。マトリクスのテスト専用 `lifetime-worker` は stdin パイプ、EOF、またはテストの明示的な終了操作で寿命を管理し、60 秒経過による自然終了は使いません。失敗や途中の return でもスコープの後始末でテスト専用プロセスを解放します。既存の `tree-child` モードはプロセスツリー検査用に維持します。段階別の時間と失敗診断は[ローカライズ規則](LOCALIZATION.md)に従う開発者専用スモーク出力として英語のままとし、三言語 UI 検証の代わりにはしません。修正コミットの実際の Actions 全体を確認する必要があり、この調整だけで全件成功とは判断しません。

### 依存関係アラートの範囲

Dependabot alerts は管理者が設定で有効にする独立機能です。`dependabot.yml` は不要で、自動バージョン更新とも異なります。対応する依存グラフと審査済みアドバイザリで既定の `main` を検査し、現在はネイティブ版ソースが対象です。旧 Web 依存を削除した後も、現在宣言する依存関係を確認します。Python/CUDA 拡張、Conda、外部 COLMAP/OpenMVS、モデルの重み、本機の導入版を完全に網羅する保証はありません。ランタイム更新では 3DGS、2DGS、メッシュを個別に検証し、QA 依存を実学習環境と同一視しません。

シークレットスキャンと push protection は GitHub の提供機能、権限、リポジトリ設定に依存し、CodeQL や Dependabot とは別の検査境界です。設定画面とアラート画面で状態を確認してください。アクセストークン、パスワード、ローカル環境の認証情報をコミットしないでください。

## 2026-10-13 follow-up / 后续修复 / 追加修正

中文：第 #148 次运行保留全部 99 项测试，97 项通过，剩余为中文全屏对话框的语言切换和大网格的 +Z 视角吸附。回归捕获到 `QMessageBox` 在字体／样式重建期间先退出全屏，旧的延后恢复随后只能读取普通窗口状态。语言更新现在在替换目录前保存显示状态，完成动态文本和 Qt `LanguageChange` 事件后恢复；用户明确改变状态或已关闭的窗口不会被恢复。测试新增立即投递翻译事件后的全屏保持断言，原有 Esc、输入值和进度检查全部保留。可选 `GSW_WINDOW_STATE_DIAGNOSTICS=1` 只记录测试自有对话框的有界状态轨迹，不改变状态或验收。

English: Run #148 retained all 99 tests; 97 passed, leaving Chinese full-screen dialog retranslation and large-mesh +Z view snapping. Regression evidence showed `QMessageBox` leaving full screen during font/style rebuilding before deferred restoration could capture the original state. Language updates now snapshot presentation before replacing catalogs and restore it after dynamic text and Qt `LanguageChange` delivery, without reopening closed windows or overriding explicit state changes. Immediate language-delivery retention is an additional assertion; existing Escape, input and progress checks remain. Optional `GSW_WINDOW_STATE_DIAGNOSTICS=1` records a bounded trace of test-owned dialogs without changing state or acceptance.

日本語：第 #148 回は 99 件をすべて維持し、97 件が成功しました。残りは中国語の全画面ダイアログの言語変更と、大きなメッシュの +Z 軸ビューです。回帰検査では、フォント／スタイルの再構築により `QMessageBox` が全画面を解除し、遅延した復元では元の状態を取得できないことを確認しました。言語変更はカタログの置換前に表示状態を保存し、動的テキストと Qt の `LanguageChange` の処理後に復元します。閉じたウィンドウを再表示せず、明示的な状態変更を上書きしません。即時の言語イベント処理後の全画面保持を追加検査し、Esc、入力値、進捗の検証をすべて維持します。任意の `GSW_WINDOW_STATE_DIAGNOSTICS=1` はテスト所有のダイアログの状態を有限件だけ記録し、状態や合格条件を変更しません。

中文：视角吸附从用户请求时刻启动独立单调时钟，精确定时器只负责调度更新；延迟回调按实际经过时间追上，不再把延后的全局动画驱动初始化当作起点。原来的 220 ms、OutCubic 缓出、最短偏航路径、取消行为和全部模型渲染路径保持一致。没有修改帧率、1500 ms 等待、10 秒完整显示和 90% 表面覆盖门槛，也没有在测试中直接推进动画时间或设置目标视角。新增相机计算回归覆盖起点、中点、终点、长回调延迟与偏航环绕；这仍需修复提交的真实完整 Actions 验收。

English: Axis-view snapping starts an independent monotonic clock at the user's request; a precise timer only schedules updates. Late callbacks catch up to real elapsed time instead of treating deferred global animation-driver initialization as the start. The original 220 ms, OutCubic easing, shortest yaw path, cancellation and every model-rendering path are retained. FPS policy, the 1500 ms wait, 10-second complete-display gate and 90% surface coverage are unchanged; tests do not directly advance animation time or set the target pose. Added camera regression covers start, midpoint, endpoint, long dispatch delays and yaw wrapping. The fixed commit still requires actual complete Actions acceptance.

日本語：軸ビューへのスナップはユーザーの要求時刻に独立した単調時計を開始し、精密タイマーは更新の実行だけを担当します。遅延したコールバックは実際の経過時間に追いつき、遅れた全体アニメーションの初期化を開始時刻としません。従来の 220 ms、OutCubic、最短の yaw 経路、キャンセルと全モデルの描画経路を維持します。FPS 方針、1500 ms の待機、完全表示 10 秒、表面被覆率 90% の基準は変更せず、テストからアニメーション時間や目標姿勢を直接設定しません。開始、中間、終了、長い実行遅延、yaw の折り返しを検査します。修正コミットの実際の Actions 全体での確認は引き続き必要です。

## Retired web runtime / 旧网页运行时退役 / 旧 Web ランタイムの廃止

中文：2026-10-15 起，共享后端位于 `native/backend/compute.py`，旧网页、Electron 和 HTTP 服务从默认原生 `main` 分支删除。当前测试不再依赖 `crop_editor` 或 `resources/app` 存在。迁移后的后端与打包回归必须覆盖照片/视频导入、COLMAP、3DGS/2DGS、网格/纹理、任务状态与路径保护；源码移位不放宽现有数据与完整状态续训校验。GitHub 历史失败、标签和旧发行记录保留。参见[迁移说明](NATIVE_MIGRATION.md)。

English: From 2026-10-15, shared compute code resides in `native/backend/compute.py`; the old web, Electron and HTTP runtime is removed from the default native `main` branch. Current tests must not require `crop_editor` or `resources/app`. Migrated backend and package regression cover photo/video import, COLMAP, 3DGS/2DGS, mesh/texturing, job state and path protection. Relocation does not relax data or full-state-resume validation. GitHub historical failures, tags and previous releases remain. See [migration](NATIVE_MIGRATION.md).

日本語：2026-10-15 以降、共通計算コードは `native/backend/compute.py` に配置し、旧 Web・Electron・HTTP 実行部を既定のネイティブ `main` 分岐から削除します。現在の検査は `crop_editor` や `resources/app` の存在に依存しません。移行した後端とパッケージで写真/動画取込、COLMAP、3DGS/2DGS、メッシュ/テクスチャ、状態とパス保護を検証し、データや完全状態再開の検証は緩和しません。GitHub の過去の失敗・タグ・配布記録は保持します。[移行説明](NATIVE_MIGRATION.md)を参照してください。

## Canonical Windows fixtures / Windows 规范路径夹具 / Windows の正規化パス

中文：第 #150 次实际 Windows 运行的图形启动检查 4/4 和原生 CTest 99/99 均通过，包括三语言和侧视旋转。后续 worker 检查的 8 个失败来自测试期望保留 `RUNNER~1` 短名，而安全路径规范化正确返回 `runneradmin` 长名。仅在这 8 个用例的临时目录入口执行 `Path(tmp).resolve()`；全部精确路径断言、生产 `realpath` 及目录边界检查保留。D 盘自有 junction 复现了同样的 8 个失败，规范夹具后同一 146 项测试通过，未增加跳过项。新操作器额外加入一个纯 `QImage` 的 CTest 目标；真实窗口验收保留在 GitHub runner，不在用户桌面抢焦点。

English: Actual Windows run #150 passed graphics bootstrap 4/4 and native CTest 99/99, including all three languages and edge-on rotation. Eight later worker failures came from fixture expectations retaining the `RUNNER~1` short name while secure canonical paths correctly expanded it to `runneradmin`. Only the temporary roots in those eight cases now use `Path(tmp).resolve()`; exact path assertions, production `realpath` and directory containment remain. An owned D-drive junction reproduced the same eight failures, and the same 146-test matrix passed with canonical fixtures and no added skips. The new gizmo adds one pure `QImage` CTest target; actual-window acceptance remains on GitHub runners without taking desktop focus.

日本語：実際の Windows 第 #150 回は、描画起動検査 4/4 とネイティブ CTest 99/99 に成功し、三言語と側面回転も通過しました。その後の worker の 8 件の失敗は、テスト期待値が短名 `RUNNER~1` を保持し、安全な正規化パスが正しく長名 `runneradmin` を返したためです。対象 8 件の一時ディレクトリ入口だけを `Path(tmp).resolve()` で正規化し、厳密なパス比較、製品の `realpath` とディレクトリ制約を維持します。D ドライブのテスト専用 junction で同じ 8 件の失敗を再現し、同じ 146 件の検査が正規化後に成功しました。新しいスキップはありません。新操作器は純 `QImage` の CTest を 1 つ追加し、実ウィンドウの検査は GitHub runner で行ってユーザーのフォーカスを奪いません。

## Official references / 官方资料 / 公式資料

中文：后续完整验收发现，参考轴／侧视旋转测试仍通过查找私有 `QVariantAnimation` 强制跳到终点。视角计时实现改变后，这段测试会在默认视角立即发送鼠标输入并取消尚未完成的吸附，导致并未真正进入侧视的错误验收。现在只在公开 +X 视角确实到达后才开始旋转，使用与网格验收相同的 1500 ms 有界等待和 `1e-6` 角度容差；保留全部旋转、修饰键、数字输入、撤销和重做断言，不设置内部状态。本机可见界面测试已停止；未经用户允许不自动弹出或置顶窗口，剩余可见界面验收由 GitHub runner 完成。

English: Follow-up full acceptance found that the reference-axis/edge-on rotation smoke still searched for a private `QVariantAnimation` and forced it to its endpoint. After the timing implementation changed, that obsolete test immediately sent mouse input at the default pose and cancelled the unfinished snap, so it never actually reached the required side view. It now observes the public +X pose before rotation, with the same bounded 1500 ms wait and `1e-6` angle tolerance as mesh acceptance. All rotation, modifier, numeric-input, undo and redo assertions remain; no private state is set. Local visible GUI testing has stopped. Windows must not open or rise to the foreground without user consent; remaining visible acceptance runs on GitHub runners.

日本語：追加の全体検査では、参照軸／側面の回転テストが私有の `QVariantAnimation` を検索し、終点へ強制的に進めていたことが分かりました。計時の実装変更後、この古い検査は既定の視点で直ちにマウス入力を送り、未完了のスナップをキャンセルしていたため、必要な側面ビューに到達していませんでした。今は公開されている +X の姿勢に到達してから回転を開始し、メッシュ検査と同じ 1500 ms の有限待機と `1e-6` の角度許容差を用います。回転、修飾キー、数値入力、元に戻す／やり直すの検証はすべて維持し、内部状態を設定しません。ローカルの可視 GUI 検査は停止しました。ユーザーの同意なくウィンドウを開いたり前面に出したりせず、残りの可視検査は GitHub runner で実行します。

- [CodeQL workflow configuration](https://docs.github.com/en/code-security/reference/code-scanning/workflow-configuration-options)
- [CodeQL init inputs](https://github.com/github/codeql-action/blob/v4/init/action.yml) and [analyze inputs](https://github.com/github/codeql-action/blob/v4/analyze/action.yml)
- [Dependabot alerts and coverage](https://docs.github.com/en/code-security/concepts/supply-chain-security/dependabot-alerts)
- [Events that trigger workflows](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows)
