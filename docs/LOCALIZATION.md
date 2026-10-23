# Native desktop localization

## Per-install runtime component environment

`runtime-components.json` belongs beside an installation's `bin/` directory,
not in global settings. The optional `gsw.runtime_components/1` object contains
only `camera_rasterizer` and `quality_rasterizer` components. Component roots
must be absolute local directories without links/junctions or traversal.
`manifest_sha256` binds the fixed camera proof or quality build manifest;
quality additionally requires `acceptance_sha256` for the fixed acceptance file.
These hashes establish local integrity, not publisher trust or quality acceptance.
Actual Python selectors retain all source/ABI/GPU checks unchanged.

Missing configuration preserves every inherited environment entry. Explicit
inherited component variables, including empty values, take precedence; no
process/global variable is written. Failed resolution never applies a partial
overlay. Before any Python launch, the shared environment factory returns the
localized error through the existing dialog/export error path. Ordinary probes,
workers, persistent training/batches and coordinate-export Python use the same
binding. Language changes affect the error caption only, not the plan/runtime.

| English | 简体中文 | 日本語 |
| --- | --- | --- |
| Python runtime unavailable | Python 运行环境不可用 | Python 実行環境を利用できません |
| Runtime component configuration | 运行组件配置 | 実行コンポーネントの設定 |

中文：绑定仅作用于本安装启动的 Python 子进程，不覆盖旧组件、全局环境或
暂停工程；配置错误会在启动前返回，不会静默回退。

日本語：この設定は本インストールから起動する Python 子プロセスのみに
適用します。旧コンポーネント、グローバル環境、一時停止中のプロジェクトは
変更しません。設定エラーは起動前に返し、暗黙に切り替えません。

## Experimental training quality plans

The training dialog keeps the experimental plan disabled by default. An empty
plan is omitted from the version-one saved request and worker configuration,
preserving legacy behavior and sampling RNG policy. The explicit checkbox,
geometry learning-rate multiplier, densification and sampling-filter controls
are separate from quality presets, camera policy, resolution and resource guard.
The interface never claims quality acceptance or manufactures a plan hash.
Python remains the sole execution resolver for actual input/kernel capability.
Positive multipliers that the native numeric editor cannot represent exactly
are rejected before restoration; they are never silently clamped or rounded.

Quality-plan JSON import is read-only, limited to a 256 KiB object and rejects
unknown structures and unverified combinations. Imported method parameters,
supervision declarations and paths remain verbatim; no files are probed and no
worker starts. Mask registration scale/evidence fields must be paired. Changing
only the multiplier preserves method parameters. Only an explicit strategy or
filter change resets that section's method parameters. MCMC remains visible but
disabled pending GPU/trainer acceptance. Unsupported 3DGS choices do not become
Standard when switching to 2DGS; the request is retained and starting is blocked.
All captions, tooltips and validation summaries change language immediately,
without altering the plan, input identities, runtime options or worker state.

| English | 简体中文 | 日本語 |
| --- | --- | --- |
| Experimental quality plan | 实验性质量计划 | 実験的な品質計画 |
| Geometry learning-rate multiplier | 几何学习率倍率 | 幾何学習率の倍率 |
| Densification strategy | 增密策略 | 高密度化の戦略 |
| Absolute gradient | 绝对梯度 | 絶対勾配 |
| Contributing pixel coverage | 贡献像素覆盖 | 寄与画素の被覆 |
| Sampling filter | 采样过滤 | サンプリングフィルター |
| Full Mip-Splatting | 完整 Mip-Splatting | 完全な Mip-Splatting |

中文：质量计划默认关闭且不生成默认字段；启用只提交实验性请求，不承诺质量
改善。导入只读取 JSON，保留方法参数、监督和路径；只改倍率不丢参数，明确
切换策略或过滤才重置对应部分。未验证组合和后端不兼容会阻止开始，MCMC
尚不可选。语言即时切换仅更新文字，不改计划或任务。

日本語：品質計画は既定で無効で、既定フィールドも生成しません。有効化は
実験的な要求を送るだけで、品質向上を保証しません。JSON の取り込みは手法
パラメーター・教師信号・パスを保持し、倍率だけの変更では失いません。
戦略またはフィルターを明示的に変更したときのみ該当部分を初期化します。
未検証の組合せと非対応バックエンドでは開始を拒否し、MCMC は未選択と
します。言語変更は表示のみを更新し、計画やタスクは変更しません。

## Known-camera initialization terminology

| English | 简体中文 | 日本語 |
| --- | --- | --- |
| Initialization recipe | 初始化配方 | 初期化の生成条件 |
| Declared coarse bounds | 声明的粗范围 | 宣言した概略範囲 |
| Optical-axis convergence | 光轴会聚 | 光軸の収束 |
| Camera Z depth | 相机 Z 深度 | カメラの Z 深度 |
| Geometric visibility check | 几何可见性检查 | 幾何学的な可視性の確認 |
| Frozen initialization points | 冻结初始化点集 | 固定した初期点群 |

The raw import editor records a declaration only. Its source/canonical frame,
seed, count, priors and provenance survive native request round trips. The
post-import initialization dialog uses the admitted dataset identity and the
actual shared CPU helper for points, transformed declared box corners, full-K
camera frusta and per-training-view visibility. Display frustum length is a
notation, not measured object depth. Validation/test cameras are labeled, but
their image pixels do not generate initialization or suggestions.

Suggestions require explicit adoption and another check. An optical-axis
intersection cannot establish object extent; a user-specified camera-Z interval
creates a frustum AABB, not a measured object boundary. Source/canonical frame
changes never just relabel existing coordinates. Editing invalidates old
reports; changing a saved recipe invalidates its old frozen point artifact.
Creating a version preserves the original dataset and locked cameras. Frozen
random points remain random initialization, not an external geometry prior.
Visibility checks are not training, surface coverage or model accuracy proof.

中文：原始导入页仅记录声明；导入后才对真实点集、完整内参相机、变换后的声明
范围与训练视图可见性进行检查。建议必须明确采用并重新检查，不把光轴或相机
轨迹当作对象深度或边界；不读取验证、测试图像。源／规范框架不能仅换标签，
修改立即使旧报告失效；修改已保存配方时重新生成点集并显示警告。创建独立
版本保留原数据与已知相机。冻结随机点不构成外部几何先验，可见性不是模型精度。

日本語：インポート画面は宣言の記録のみを行い、インポート後に実際の点群、
完全な内部行列を持つカメラ、変換した宣言範囲と学習視点での可視性を確認
します。提案は明示的に採用して再確認し、光軸や軌跡を物体の深度・境界と
見なしません。検証・テスト画像の画素は読み込みません。元／正規座標系の
ラベルだけの変更を拒否し、編集時は古い報告を無効にします。保存済みの
生成条件を変更すると点群を再生成し警告します。新しい版は元データと既知
カメラを保持します。固定したランダム点群は外部形状の事前情報ではなく、
可視性の確認はモデル精度の証明ではありません。

## Separate Windows setup wizard

Setup offers `zh_CN`, `en_US` and `ja_JP` before opening its pages. Its custom
runtime/safety/finish text is generated from `native/i18n/catalog.json` and
validated by the same command below. Its exact subset accompanies the installer
under `source/packaging/windows/installer-catalog.json`; standard wizard controls
use maintained Inno translations. This separate process does not change the
desktop application's language, settings, model or worker state.

## Language switching

Blender-derived viewport navigation uses localized **Perspective / Orthographic**
labels (`透视 / 正交`, `透視 / 平行`) rather than calling every orthographic view
`Iso`. Its center projection-control tooltips are translated in all three
languages and refresh through the existing live viewport language callback.
The unframed captions retain translated scene/task information. These changes
only redraw presentation; they do not rebuild the camera, model or worker.
Blender's separate 3D cursor is not added as a coordinate-origin marker.

中文：Blender 风格视口的“透视／正交”和中心投影控件提示均随语言即时更新，
不把所有正交视图误称为等轴视图，也不把三维游标当作坐标原点或编辑手柄。

日本語：Blender 由来のビューポートでは「透視／平行」と中央投影コントロール
の説明を即時に翻訳します。平行投影をすべて等角投影と呼ぶことはなく、
3D カーソルを座標原点や編集ハンドルとして追加しません。

Open **View → Language** (`视图 → 语言 / Language`, `ビュー → 言語 / Language`) and choose **简体中文**, **English**, or **日本語**. The language changes immediately in the current window and is remembered for future launches. No restart or confirmation is needed. Switching refreshes text and fonts without reloading the model, resetting its transforms/camera/selection, or interrupting processing and backup jobs. Dock layout, active tabs, edited dialog parameters, training progress and curve samples are retained.

The initial default is Simplified Chinese, independent of the Windows display language. Menus, panels, dialogs, viewport/transform hints, training-monitor labels, validation errors, and recovery/backup messages use the selected language. Qt file dialogs are used so their standard controls follow this choice too.

User filenames, entered project/scene names, paths, project data, JSON/protocol identifiers, and third-party raw logs are not translated. Existing project/task names and historical log records keep their original text; current status labels and newly generated messages use the new language. Units (`mm`, `cm`, `m`, `dB`) and established identifiers (`PLY`, `COLMAP`, `CUDA`, `PSNR`) remain stable. Language changes never rename user data. Command-line help and developer-only smoke-test output remain English.

## Mandatory feature maintenance

1. Use `QCoreApplication::translate("Workbench", "source text")` for dynamically formatted UI text. Bind persistent widget/action properties using `AppLanguage::text` / `AppLanguage::bind` with `AppLanguage::source("source text")`. Use `bindComboItem` for translated combo captions (it blocks signals so edited presets are not reset). Refresh dynamic labels and custom drawing with `AppLanguage::onChanged`; callbacks must change presentation only, never reload a scene or re-ingest telemetry. Connections are tied to the target QObject's lifetime. Never reverse-match rendered text to translate it. Prefer complete sentences and keep command/item identifiers separate from labels.
2. Add/update `zh_CN`, `en_US`, and `ja_JP` in `native/i18n/catalog.json` in the same change. Source keys may be Chinese or English. Preserve placeholders, keyboard shortcuts, wildcard file filters and HTML markup. Update the glossary when introducing terms.
3. Run `python scripts/native_i18n.py` and `python scripts/test_native_i18n.py`. Validation rejects missing entries/locales, duplicate keys, inconsistent placeholders, filters and markup. Literal auditing catches Chinese literals and English phrases in native implementation files; reviewers must also inspect short English labels and dynamically assembled UI text.
4. Run the native CTest suite, including `native_language_zh_CN`, `native_language_en_US`, and `native_language_ja_JP`. Inspect all three languages for clipping when changing forms, toolbars or overlays. Package and verify the installed build before delivery.

CMake runs validation, generates TS files in the build directory, compiles them with Qt Linguist `lrelease`, and embeds the QM catalogs. Qt base catalogs for Chinese and Japanese are embedded for standard controls. Qt Linguist Tools and Python 3 are build dependencies. Runtime translation is offline.

QA example: `"Gaussian Scene Workbench.exe" --smoke-test-language --language ja_JP`. The language override does not modify the user's preference. Language smoke tests isolate their settings and cycle all three languages twice in one window. They verify live actions/dialogs, Qt buttons, persistence, camera/transforms/selection, telemetry samples and unchanged backend/preset/user-name data. Build-tree tests also keep a real test-owned worker process running during the switches. Set `GSW_LANGUAGE_SCREENSHOT_DIR` to a non-system-drive directory for application-only QA images.

The shared 3DGS / 2DGS training monitor reflows its seven metrics by available width and font size. Narrow or short panels scroll vertically instead of shrinking the chart below its readable height; the default bottom dock remains compact. Long task titles use plain text and middle elision, with their complete original text in the tooltip. The effective-input tooltip also retains the full report. Resizing, scrolling and font/language changes only update presentation, not training samples, progress, backend settings or worker state. Language smoke tests cover both backends at 320 / 520 / 900 px with 90% / 150% fonts and return to a wide layout. Other generation jobs retain their own status displays; no Gaussian training metrics are fabricated for reconstruction or meshing.

## Day and night appearance

**View → Appearance → Automatic (Time-based) / Light (Day) / Dark (Night)** changes the interface immediately and remembers the selected mode (`ui/theme`: `auto`, `light` or `dark`). Automatic is the initial default when no appearance preference has been saved; an existing manual light/dark preference is retained. Automatic follows the computer's local time: from 06:00 (inclusive) until 18:00 (exclusive) is light, and all other times are dark. Midnight stays dark; light mode starts at 06:00. Light or Dark remains a manual override and does not follow the clock or Windows theme.

The schedule updates during use. A precise single-shot timer is scheduled toward the next 06:00/18:00 boundary, with at most 30 seconds between checks to detect clock or time-zone changes and resume from sleep; application activation also refreshes it immediately. The remembered mode remains `auto` while its effective light/dark palette changes. Light mode covers menus, dialogs and file browsers, dock panels, logs, training charts, status text, viewport background, grid and navigation/transform overlays. Language and UI-scale changes retain the selected mode and do not interrupt the schedule.

Light mode uses the neutral hierarchy illustrated in the user's Metashape reference: white workspaces, gray toolbars/panel headers, near-black text and distinct blue focus/selection states. Muted and disabled text remains legible rather than fading into the background. Field/button boundaries are darker, while minor viewport grid lines remain quieter than major lines and colored axes. This is an original palette adaptation, not a copy of proprietary code or assets; dark mode and UI density are unchanged. Validation covers rendered label/header/control contrast as well as palette contrast, framebuffer opacity and actual viewport clear color.

Light-mode dock separators match the light gray of the panel headers, with subtle one-pixel edges and neutral-gray hover feedback. Their modestly wider 8-pixel base extent distinguishes the left/right panels from the white viewport through spacing rather than dark outlines. They are Qt's actual resize handles, not overlays; their extent follows the UI scale, and drag-resize, docking/floating, immediate language switching and dark-mode styling are preserved. Tests inspect the actual rendered colors and widths and resize both side panels.

Text labels, checkbox/radio captions, dialog button areas, selection toolbars and resize-grip backgrounds reveal their containing surface in both themes. Inputs, buttons, lists, selected rows, progress bars and viewport overlay cards keep their own intentional surfaces. Appearance checks compare the actual rendered status-bar and dialog pixels across light/dark switching, all three languages and 90%/100%/150% UI scale, so a label cannot quietly reintroduce a white or dark rectangle. This shared treatment follows [Qt's style-sheet cascading rules](https://doc.qt.io/qt-6.8/stylesheet-syntax.html#cascading).

The change is presentation-only and shared by 3DGS, 2DGS, reconstruction and all mesh-generation workflows. Manual and scheduled switches do not reload models, rewrite RGB/SH/materials, alter camera/selection/transforms, clear training curves, interrupt workers or change exported data. Transparent splats naturally composite against the chosen backdrop; this is not relighting or a new reconstruction. `--theme auto|light|dark` is a non-persistent launch/QA override. A manual CLI override disables scheduled switching for that launch without changing the saved mode. The language smoke matrix toggles both themes while a test-owned worker runs and a real file remains selected. It checks 05:59:59.999 → 06:00 → 17:59:59.999 → 18:00 → 23:59:59.999 → 00:00 against independent expected colors, including no switch at midnight and unchanged manual overrides. Real-clock QA invokes the application's timer callback, reports the local time and effective theme, and saves automatic-mode screenshots before restoring a manual mode. Light-mode Gaussian, navigation and mesh-publication regressions complement existing dark tests.

中文：通过“视图 → 外观 → 自动（按时间）／浅色（白天）／深色（黑夜）”即时切换并记住选择。未保存过外观选择时默认自动，已有的手动浅色或深色设置保留。自动模式按本机时间在 06:00（含）至 18:00（不含）使用浅色，其余时间使用深色，午夜保持深色；运行中持续检查，唤醒或修改时钟后最迟 30 秒更新，重新激活程序时立即更新。手动模式不随时间改变。界面与观察背景一起切换，不修改模型或训练数据。
浅色采用白色工作区、灰色工具栏和深色文字，增强控件边界与蓝色选中状态；网格保持主次层级，深色模式和界面尺寸不变。
左右停靠面板与视口之间使用与标题栏统一的浅灰分隔带和柔和细线，基础宽度适度加宽至 8 像素并随界面缩放；悬停保持灰色，仍可拖动调整宽度。
深浅两种主题的文字标签、复选框／单选框文字区、对话框按钮区、批量选择工具条和窗口缩放把手均显示所在容器的底色，避免独立的白色或深色色块。输入框、按钮、列表选中、进度条和视口信息卡片保留各自需要的底色。实际界面像素验收覆盖三种语言、深浅色往返切换及 90%／100%／150% 界面缩放。

日本語：「ビュー → 外観 → 自動（時刻に応じて）／ライト（昼間）／ダーク（夜間）」で即時に切り替え、選択を保存します。外観設定が未保存の場合は自動が既定で、既存の手動設定は保持します。自動は本機の時刻に従い、06:00 から 18:00 より前はライト、それ以外はダークです。午前 0:00 もダークを維持します。実行中も確認し、スリープ復帰や時刻変更後は 30 秒以内、アプリの再アクティブ化時は即時に更新します。手動モードは時刻に連動しません。UI と観察用背景のみを変更し、モデルや学習データは変更しません。
ライトテーマは白い作業領域、グレーのツールバー、濃い文字で構成し、コントロールの境界と青い選択状態を明確にします。グリッドの主線と補助線を区別し、ダークテーマと UI の寸法は変更しません。
左右のドックパネルとビューポートの間に、タイトルバーと統一したライトグレーの区切りと柔らかな細線を表示します。基準幅を控えめに 8 ピクセルへ広げ、UI スケールに追従します。ホバー時もグレーを保ち、ドラッグによる幅の調整は引き続き利用できます。
両テーマで、ラベル、チェックボックス／ラジオボタンの文字領域、ダイアログのボタン領域、一括選択ツールバー、サイズ変更グリップは親領域の背景を表示し、白や暗色の矩形が浮く状態を防ぎます。入力欄、ボタン、リストの選択行、進捗バー、ビューポートの情報カードは必要な固有の背景を維持します。実際の描画ピクセルによる検証は、3 言語、ライト／ダークの往復切替、90%／100%／150% の UI スケールを対象とします。

## Explicit quality comparison

**Workflow → Compare Quality…** opens only on an explicit user action. It compares
two supplied JSON measurement reports against the user's enabled regression and
absolute limits; it does not train, render or measure quality. PSNR, SSIM, LPIPS
and optional depth/normal/mesh errors use the reports' metric-protocol identity,
without guessing or converting geometry units. No limit is enabled by default.
The shared Python comparison module owns every delta and verdict. The dialog
localizes pass/regression/unverified/incomparable states and reasons, while
retaining raw setting paths, values and protocol identifiers. Editing an input
or limit clears the old verdict; changing language does not change inputs,
thresholds, the process or its retained report.

Input snapshots and the contract are created only in a fresh workspace-owned
temporary directory. Each input and the combined helper output are bounded at
16 MiB; original reports are never overwritten. Cancel and window close stop
only this dialog's JSON comparison helper, not a training worker. Missing
metrics, absent independent evidence or training-fit-only results remain
unverified. A contract pass applies to those supplied reports, not all views or
an unrelated active experiment; it does not automatically relabel the training
monitor's quality-evidence warning.
**Save Comparison Report…** is available only for a current valid machine
report, including an explicit unverified/incomparable result. It writes a new
JSON file containing the complete comparison and selected contract, never
overwriting an existing file or either input report. Changing inputs/limits
disables saving until comparison completes again. No report opens automatically.
The settings table displays at most 512 changes and summarizes long values;
the machine result and exported JSON retain their complete original values.

**Measure Images…** opens a separate dialog only when explicitly requested.
It runs the CPU-only image measurement command on existing reference/render
pairs listed in a JSON manifest, preserving relative paths against the original
manifest directory. It does not train, render, resize images or claim geometry
accuracy. The dialog displays per-view and aggregate PSNR/SSIM, localized
progress/cancellation/errors, and an explicit missing-independent-validation
notice. The raw view identifiers and numeric metric protocol remain unchanged.
Only **Use as Baseline / Use as Candidate** publishes a fresh report under the
current workspace's `.gsw/quality/measurements` directory and fills the chosen
comparison input. The report survives closing the measurement dialog; originals
and existing reports are never overwritten. Without that choice, temporary
results are discarded on close. No report or unrelated window opens
automatically. Inputs and combined helper output are capped at 16 MiB; the
helper has a ten-minute limit. Cancel/close terminate only this owned CPU helper.
Language changes retain the manifest, live process, results and explicit choice.
The saved measurement is a persistent workspace report with a visible path,
not an automatic experiment association. Its migration through Save As and
backup is not established by this feature. Every adoption rechecks the report's
owned canonical path and exact bytes. If an adopted file was externally changed
or deleted, explicit adoption publishes a new UUID from the still-trusted
temporary result without overwriting the changed file.
Adopted reports use the application workspace base outside disposable project children;
they are not automatically attached to an experiment or its backup.
The default **Pair Folders (Exact Filenames)** mode accepts reference/render
folders and an optional mask folder. It lists all exact top-level, non-hidden
PNG/JPEG filename pairs (case and extension included), without image decoding,
recursion, links, stem guessing, resizing or sampling. Every reference must
match; choosing masks requires every matching mask too. Missing/ambiguous pairs
block measurement rather than evaluating a subset. The advanced JSON manifest
remains available for distinct filenames, including PNG masks for JPEG images.
At most 4096 pairs are accepted. If the preview table displays only its first
512 rows, an explicit notice states that the manifest and measurement retain
every pair. Directory contents are rechecked before starting.
**Refresh Pairs** explicitly rereads folder entries after files are repaired
without changing the folder paths; it is disabled during measurement. A compact
protocol caption/tooltip states encoded RGB8 without color conversion, PSNR
from pooled valid-channel MSE and Gaussian 11×11 sigma-1.5 valid-window SSIM,
distinct from training-loss SSIM. Validation failures show translated format,
dimension and mask guidance before clearly labelled original technical logs.

中文：“工作流 → 质量对照…”仅由用户主动打开。选择两份 JSON 实测报告后，可独立
启用允许退化、绝对上下限和几何证据要求，默认不启用任何门限。界面展示同一 Python
比较模块给出的逐项变化、状态、原因及原始参数差异，不启动训练、渲染或质量测量。
缺少指标、缺少独立证据或只有训练拟合结果时保持“未验证”；通过只适用于所选报告
与门限，不会替无关联的训练任务宣称质量通过。编辑输入或门限清除旧判定，切换
语言保留数据和进程。原报告不改写，取消和关闭只停止本窗口自己的比较辅助进程。
“保存对照报告…”仅保存当前有效的机器结果与所选门限，拒绝覆盖已有文件或输入报告，
不会自动打开文件。表格最多展示 512 项参数差异并摘要长值，导出的 JSON 保留完整内容。
“图像实测…”由用户主动打开，按 JSON 清单在 CPU 上测量已有参考图／渲染图的
逐视图及汇总 PSNR、SSIM；不训练、不渲染、不缩放或证明几何精度。相对路径仍以
原清单目录为准，缺少留出集来源证明保持未验证。只有点击“用作基准／用作候选”
才将新报告持久保存到当前工作区并填入所选输入；关闭窗口后报告保留，不覆盖原图、
清单或旧报告。不选择则关闭时清理临时结果，取消只停止本窗口的 CPU 辅助进程。
默认文件夹模式列出第一层非隐藏 PNG/JPEG 的严格同名配对，不读取图像像素、
递归、跟随链接、猜测同名主干或抽样。每张参考图及已选择目录中的遮罩都必须
完全同名匹配，否则阻止实测；不同名称可使用高级 JSON 清单。最多 4096 对，
表格超过 512 行会明确说明仅预览截断、清单和实测仍处理全部配对。
报告保存于应用工作区基目录、独立于可清理的未命名工程子目录，显示实际路径；
不自动关联实验，也不保证随工程另存为或备份迁移。每次采用
都复查工作区归属和原始字节；若已被外部修改或删除，会从仍可信的临时结果生成
新 UUID 报告，不覆盖已修改文件。

日本語：「ワークフロー → 品質を比較…」はユーザーの明示操作でのみ開きます。
2 つの JSON 測定報告に対し、許容する品質低下、絶対上下限、幾何品質の根拠を
個別に設定します。既定ではしきい値は無効です。同じ Python 比較モジュールの
変化量・判定・理由・元の設定差分を表示し、学習・描画・品質測定は開始しません。
指標や独立した根拠が不足する場合、または学習への適合結果だけの場合は未検証を
維持します。合格は選択した報告としきい値に限り、無関係な学習の品質合格には
しません。入力・しきい値の編集で古い判定を消去し、言語変更ではデータとプロセスを
保持します。元の報告は変更せず、キャンセルと終了は本ウィンドウ自身の比較用
補助プロセスのみを停止します。
「比較報告を保存…」は現在の有効な機械判定と選択したしきい値を新しい JSON に
保存し、既存ファイルや入力報告は上書きせず、自動で開きません。表の設定差分は
最大 512 件で長い値を要約しますが、JSON には完全な内容を保持します。
「画像を測定…」は明示操作でのみ開き、JSON の対応表に記載した既存の参照画像と
描画画像を CPU で測定し、ビューごとと集計の PSNR・SSIM を表示します。学習、
描画、リサイズはせず、幾何精度も保証しません。相対パスは元の対応表のフォルダー
を基準とし、ホールドアウトの出所を証明できなければ未検証を維持します。
「基準として使用／候補として使用」を押した場合のみ新しい報告を現在の
ワークスペースに保存して選択した入力に設定し、ウィンドウを閉じても報告は保持
します。元画像、対応表、既存報告は上書きしません。採用しない一時結果は終了時に
削除し、キャンセルはこのウィンドウの CPU 補助プロセスのみを停止します。
既定のフォルダーモードでは、直下の非表示でない PNG/JPEG を完全に同じ名前で
対応付けし、画素の読み込み、再帰探索、リンク追跡、名前の推測や間引きはしません。
すべての参照画像と任意に指定したマスクの対応が必要で、不一致なら測定を止めます。
異なる名前には詳細 JSON 対応表を使えます。最大 4096 組で、表が 512 行を超える
場合も表示のみの省略であることを明記し、測定では全組を処理します。
実際の保存パスを表示し、実験への自動関連付けや、名前を付けて保存・バックアップ
による移行は保証しません。採用のたびにワークスペースの所属と元のバイト列を
確認します。外部で変更・削除された場合は、信頼できる一時結果から新しい UUID の
報告を作成し、変更済みファイルは上書きしません。

## Terminology

### General training batches

**Workflow → General Training Batch Plans…** edits dataset references, arbitrary
supported parameter axes and an advanced base JSON configuration. Iteration count
and resolution must be explicit; quality labels cannot silently choose them.
Batch resolution factors are restricted to `1`, `2`, `4` and `8`; other values
are rejected and never interpreted as target pixel widths by a legacy loader.
Previewing expands the Cartesian product and pins verified canonical dataset
identities, but does not start training. Creating a plan rechecks those identities
and saves an immutable plan with its separate durable state. Start and Resume are
separate explicit requests handled by the main window's process supervisor.

Initialization follows the dataset declaration and evaluation follows declared
splits. `trainingSeed` controls actual training randomness, not the initialization
geometry seed or the held-out split. `repeat` identifies repeated trials and is
not an optimizer option. GPU concurrency is one; invalid or oversized matrices
fail rather than being sampled. Opening history, importing a request, changing
language, refreshing state or restarting the application never starts a batch.
Resume availability remains subject to backend validation of input identities,
parameters, checksums and genuine full-state checkpoints. A PLY or preview is not
optimizer-state resume. Closing or cancelling the planner stops only its own CPU
helper and does not terminate a real training worker.
Persisted `running` states may be crash remnants: their Resume action only asks
for verification and explicit trust confirmation. It does not change the saved
status or bypass worker-lock, PID, input or checkpoint checks. Declining the
confirmation registers nothing and launches no worker.

中文：“通用训练批量计划”使用可编辑的数据集表、参数轴和基础 JSON 配置。
迭代数与分辨率必须明确，不能由质量标签暗中决定。预览只展开组合并固定核验后的
数据身份，创建计划会再次核验并持久保存；启动和继续是分别执行的明确操作。
初始化与评估沿用数据集声明和原划分，训练随机种子与初始化种子分开，repeat
只表示重复试验。GPU 串行执行，超量或非法组合明确拒绝。打开、导入、切换语言、
刷新状态及重启程序均不自动训练；继续仍须后台验证完整检查点，不能以 PLY
或预览代替。关闭计划窗口只停止其 CPU 辅助进程，不影响真实训练任务。

日本語：「汎用学習バッチ計画」は編集可能なデータセット表、パラメーター軸、
基本 JSON 設定を使います。反復数と解像度は明示し、品質名から黙って決めません。
プレビューは組合せの展開と検証済み ID の固定のみで、計画作成時に再検証して
保存します。開始と再開は別の明示操作です。初期化と評価は宣言済みの内容と分割に
従い、学習の乱数シードと初期化シードを分け、repeat は反復試行のみを表します。
GPU は直列実行し、過大・不正な組合せは拒否します。履歴の表示、取り込み、言語
変更、状態更新、アプリ再起動では自動学習せず、再開には完全なチェックポイントの
バックエンド検証が必要です。PLY やプレビューは代替になりません。計画ウィンドウ
の終了は自身の CPU 補助プロセスのみを止め、実際の学習には影響しません。

### Scale constraints and independent checks

**Workflow → Scale and Align…** uses separate fit constraints and independent
checks. Known distances, camera baselines and control points retain stable IDs,
reference units, requested weights, reference uncertainty and provenance. A
blank uncertainty means unknown, not an error-free reference. The fit-conflict
tolerance is explicitly entered; it is not a requested or measured model
accuracy. Scale-only estimation does not establish position or orientation;
full similarity alignment is a distinct mode with its own geometry checks.

Fit residuals describe agreement with the constraints used to compute the
transform. Independent checks are excluded from fitting and retain their own
declared independence, source, measurement IDs and tolerances. Missing evidence
remains unverified, and a failed check prevents creating a scale version. A
passing reference-constraint check does not establish whole-model geometric
accuracy. The machine report states `model_geometry_accuracy=not_measured`.
The original request, solution, transform, inverse and separate check results
are retained. Estimation and creation/use of a fresh dataset version are
separate actions; source cameras, images and old checkpoints are unchanged.
Language changes retain edits, active helper processes and machine results.

The same dialog can explicitly target an existing PLY model. It first runs a
hidden cancellable source inspection that hashes the model and linked resources
without relying on a preview. The semantic source identity is distinct from the
raw PLY checksum. Model-only sources have no camera baselines; baseline imports
are refused rather than silently discarded. Supported transformations create a
fresh baked model, while directional-SH rotations that cannot be baked create
only an explicitly labelled transform manifest and unchanged model copy. Such a
manifest is never adopted as a baked aligned PLY. Both cases retain constraints,
transform and independent checks, and neither modifies the original model.

Native reading of `gsw.model_scale_export/1` checks the actual PLY checksum,
positive proper similarity/inverse, output frame and cross-record provenance
bindings. Already-baked metric output has exactly `1 m` per output coordinate;
`scale.meters_per_unit` records the input-to-metric factor and is not applied
again to measurements. The embedded record stays portable with the PLY and does
not require a source archive beside it. Native provenance explicitly records
`verification_level=native_structural_and_ply_hash` and does not claim a second
solver replay or independent physical model accuracy. Worker-side creation and
delivery acceptance remain responsible for solver replay.
The newly fitted local frame never inherits an output CRS from an old PLY
header or `.prj`. Source CRS declarations remain provenance only; a local Sim3
is not CRS registration or reprojection.

中文：同一窗口可明确选择已有 PLY 模型，先在后台核验模型与关联资源的语义身份，
不依据预览猜尺度。模型没有相机来源时禁止基线约束，不静默丢弃导入行。支持的
变换生成新模型；不能烘焙的方向相关 SH 旋转只生成明确标注的变换清单和原模型
副本，不能作为已烘焙的对齐模型采用。两种结果均保留约束和检核，原模型不变。

日本語：同じダイアログで既存の PLY モデルも明示選択でき、先にモデルと関連
リソースの意味的な ID をバックグラウンドで検証します。プレビューからスケールを
推測しません。カメラ情報のないモデルでは基線拘束を禁止し、取り込み行を黙って
捨てません。対応する変換は新モデルへ適用し、未対応の方向依存 SH 回転は明示した
変換マニフェストと元モデルのコピーのみを生成します。これをベイク済みモデルと
して採用せず、どちらも拘束・検査を保持して元モデルを変更しません。

中文：“工作流 → 定尺度与对齐…”分别管理拟合约束和独立检核。已知距离、相机
基线与控制点保留 ID、参考单位、权重、不确定度和来源。空白不确定度表示未知，
不是零误差。拟合冲突容差需要明确输入，不是模型精度指标。仅定尺度不确定方向
和位置；完整相似变换对齐使用独立模式。拟合残差只说明与拟合约束的一致程度；
检核不参与拟合。证据不足保持未验证，检核失败禁止创建尺度新版本。检核通过
也不代表完整模型的几何精度。估计与创建／使用新版本分开，保留原请求、变换、
逆变换和检核结果，不修改原相机、图像或旧检查点；切换语言保留输入与进程。

日本語：「ワークフロー → スケール設定と位置合わせ…」では適合用拘束と独立
検査を分けます。既知の距離、カメラ基線、基準点の ID、参照単位、重み、不確かさ、
出所を保持します。不確かさの空欄は不明であり、誤差ゼロではありません。適合矛盾
の許容値を明示入力し、モデル精度とは扱いません。スケールのみでは方向・位置を
確定せず、完全な相似変換は別モードです。適合残差は使用した拘束との整合性のみを
表し、独立検査は適合に使いません。根拠不足は未検証、検査不合格は新規版の作成を
禁止します。検査合格もモデル全体の幾何精度を保証しません。推定と新しい版の
作成・採用を分け、要求、変換、逆変換、検査結果を保持します。元のカメラ、画像、
チェックポイントは変更せず、言語変更でも入力と処理を維持します。

Scene and training-output names accept arbitrary Unicode text, spaces and symbols (including characters forbidden in Windows filenames). Only blank names are rejected. `ManagedName.h` maps display names to backend-safe ASCII storage identifiers; matching Python validation must stay in sync. Legacy safe ASCII names keep their paths, except the reserved `gsw-name-` namespace. Names requiring encoding use that prefix plus the full SHA-256 of the exact UTF-8 name. Do not trim, case-fold, translate or sanitize the display name. `.gsw-name.json` retains the original name alongside the dataset and travels with project migration/backup; import publishes it in the same transaction as the data. Existing job configurations without display metadata remain supported. Actual export filenames still obey operating-system filesystem rules.

| 简体中文 | English | 日本語 |
| --- | --- | --- |
| 工程 / 数据集 | Project / Dataset | プロジェクト / データセット |
| 批量计划 / 参数轴 | Batch Plan / Parameter Axis | バッチ計画 / パラメーター軸 |
| 数据身份固定 / 串行执行 | Dataset Identity Pinning / Serial Execution | データ ID の固定 / 直列実行 |
| 距离约束 | Distance Constraint | 距離拘束 |
| 相机基线 | Camera Baseline | カメラ基線 |
| 控制点 | Control Point | 基準点 |
| 拟合残差 | Fit Residual | 適合残差 |
| 独立检核 | Independent Check | 独立検査 |
| 正尺度相似变换（Sim3） | Positive-scale Similarity Transform (Sim3) | 正のスケールの相似変換（Sim3） |
| 桌面 | Desktop | デスクトップ |
| 常用文件夹 / 常用文件 | Frequent Folders / Frequent Files | よく使うフォルダー / よく使うファイル |
| 文件窗口历史 / 清除历史 | File Dialog History / Clear History | ファイルダイアログの履歴 / 履歴を消去 |
| 外观 / 自动（按时间） / 浅色（白天） / 深色（黑夜） | Appearance / Automatic (Time-based) / Light (Day) / Dark (Night) | 外観 / 自動（時刻に応じて） / ライト（昼間） / ダーク（夜間） |
| 全屏 / 退出全屏 | Full Screen / Exit Full Screen | 全画面表示 / 全画面表示を終了 |
| 最大化窗口 / 还原窗口 | Maximize Window / Restore Window | ウィンドウを最大化 / ウィンドウを元に戻す |
| 停靠面板 / 浮动面板 | Dock Panel / Float Panel | パネルをドッキング / パネルをフローティング |
| 显示名称 / 存储标识 | Display Name / Storage Identifier | 表示名 / 保存用識別子 |
| 点云 | Point Cloud | 点群 |
| 压缩高斯 / 有损量化 | Compressed Gaussians / Lossy Quantization | 圧縮ガウシアン / 非可逆量子化 |
| 最大球谐阶数 | Maximum SH Degree | 球面調和関数の最大次数 |
| 球谐显示 | SH Display | SH 表示 |
| 自动（源文件阶数） | Automatic (source degree) | 自動（ソースの次数） |
| 基础颜色 | Base color | 基本色 |
| 压缩质量 / 工作副本 | Compression Quality / Working Copy | 圧縮品質 / 作業用コピー |
| 稀疏点云 | Sparse Point Cloud | 疎な点群 |
| 相机注册 / 拍摄覆盖 | Camera Registration / Capture Coverage | カメラ登録 / 撮影カバレッジ |
| 重投影误差 / 法线约束 | Reprojection Error / Normal Regularization | 再投影誤差 / 法線の正則化 |
| 退化重建 / 最低有效性检查 | Degenerate Reconstruction / Minimum Viability Check | 退化した再構築 / 最低限の有効性検査 |
| 最终模型 / 只读预览 | Final Model / Read-only Preview | 最終モデル / 読み取り専用プレビュー |
| 初始化高斯 | Initializing Gaussians | ガウシアンを初期化 |
| 密度控制 | Density Control | 密度制御 |
| 过度裁剪保护 | Over-pruning Protection | 過剰な枝刈りの抑制 |
| 暂停训练 / 继续训练 | Pause Training / Resume Training | 学習を一時停止 / 学習を再開 |
| 任务预览 / 暂停预览 | Task Preview / Pause Preview | タスクプレビュー / プレビューを一時停止 |
| 实验与生成档案 | Experiments and Generation History | 実験と生成履歴 |
| 查看所选成果 / 继续所选实验 | View Selected Result / Resume Selected Experiment | 選択した成果を表示 / 選択した実験を再開 |
| 记录参数（原始标识） | Recorded Settings (Original Identifiers) | 保存済み設定（元の識別子） |
| 相机与重建来源 / COLMAP 决策 | Camera and Reconstruction Sources / COLMAP Decision | カメラと再構築の参照元 / COLMAP の処理方針 |
| 来源格式 / 注册照片 | Source Format / Registered Images | 参照元の形式 / 登録画像 |
| 完整状态元数据 | Full-state Metadata | 完全な状態のメタデータ |
| 已暂停 | Paused | 一時停止中 |
| 训练检查点 / 优化器状态 | Training Checkpoint / Optimizer State | 学習チェックポイント / 最適化器の状態 |
| 曲面光栅化 / 薄片近似 | Surfel Rasterization / Thin-Disk Approximation | サーフェル描画 / 薄い円盤による近似 |
| 透视校正 / 混合 | Perspective Correction / Compositing | 透視補正 / 合成 |
| 定时快照回退 | Periodic Snapshot Fallback | 定期スナップショットにフォールバック |
| 网格 / 三角网格 | Mesh / Triangle Mesh | メッシュ / 三角形メッシュ |
| 顶点 / 面 / 法线 | Vertex / Face / Normal | 頂点 / 面 / 法線 |
| 纹理 / UV 坐标 | Texture / UV Coordinates | テクスチャ / UV 座標 |
| 材质预览 | Material Preview | マテリアルプレビュー |
| 纹理图集 | Texture Atlas | テクスチャアトラス |
| 生成网格 / 深度融合 | Generate Mesh / Depth Fusion | メッシュを生成 / 深度融合 |
| 有界 / 无界 TSDF | Bounded / Unbounded TSDF | 有界 / 非有界 TSDF |
| 连通分量 / 纹理烘焙 | Connected Component / Texture Baking | 連結成分 / テクスチャベイク |
| 相机位姿 | Camera Pose | カメラ姿勢 |
| 重建 / 训练 | Reconstruction / Training | 再構築 / 学習 |
| 迭代 / 损失 | Iteration / Loss | 反復 / 損失 |
| 最高精度（原始分辨率） | Maximum Fidelity (Original Resolution) | 最高精細（元の解像度） |
| 原始分辨率 / 降采样 | Original Resolution / Downsampling | 元の解像度 / ダウンサンプリング |
| 图像尺寸摘要 / 生效参数 | Image Dimensions / Effective Settings | 画像寸法の概要 / 有効な設定 |
| 训练监视内容 | Training Monitor Content | 学習モニターの内容 |
| 预计尺寸 / 实际训练图像 | Estimated Dimensions / Loaded Training Images | 予想寸法 / 読み込み済み学習画像 |
| 稀疏 Adam / 曝光补偿 | Sparse Adam / Exposure Compensation | スパース Adam / 露出補正 |
| 高斯点 / 增密 | Gaussian Splat / Densification | ガウシアンスプラット / 高密度化 |
| 跨步图像预取 | Cross-step Image Prefetch | ステップ間の画像先読み |
| FP32 容量缓冲复用 | FP32 Capacity Buffer Reuse | FP32 容量バッファの再利用 |
| 按需计算曲面法线 | On-demand Surface Normals | 必要時のみ曲面法線を計算 |
| 质量验证 / 独立验证结果 | Quality Validation / Independent Validation Results | 品質検証 / 独立した検証結果 |
| 质量对照 / 允许退化 | Quality Comparison / Allowed Regression | 品質比較 / 許容する品質低下 |
| 图像质量实测 / 图像配对清单 | Image Quality Measurement / Image Pair Manifest | 画像品質の測定 / 画像の対応表 |
| 用作基准 / 用作候选 | Use as Baseline / Use as Candidate | 基準として使用 / 候補として使用 |
| 绝对下限 / 绝对上限 | Absolute Minimum / Absolute Maximum | 絶対下限 / 絶対上限 |
| 按所选门限通过 / 超出质量门限 / 未验证 / 不具可比性 | Within Selected Limits / Outside Quality Limits / Unverified / Not Comparable | 選択したしきい値を満たしています / 品質のしきい値を超えています / 未検証 / 比較不可 |
| 显存常驻 / 深度顺序 | GPU-Resident / Depth Order | GPU メモリ常駐 / 深度順序 |
| 移动 / 旋转 / 缩放 | Move / Rotate / Scale | 移動 / 回転 / スケール |
| 等比缩放 | Uniform Scale | 均等スケール |
| 全局 / 局部 | Global / Local | グローバル / ローカル |
| 轨迹球 / 吸附 | Trackball / Snap | トラックボール / スナップ |
| 锁定编辑工具 / 操作手柄 | Lock Editing Tools / Gizmo | 編集ツールをロック / ギズモ |
| 观察轨迹球 / 旋转中心 | View Trackball / Rotation Center | ビュートラックボール / 回転中心 |
| 缩放到光标 / 视点目标平面 | Zoom to Cursor / View-target Plane | カーソル位置へズーム / 注視点を通る平面 |
| 自由旋转 / 按轴旋转 | Free Orbit / Axis-Constrained Rotation | 自由回転 / 軸回転 |
| 导出模型 / 导出坐标 | Export Model / Export Coordinates | モデルをエクスポート / エクスポート座標 |
| 原始坐标 / 场景坐标 | Source Coordinates / Scene Coordinates | 元の座標 / シーン座標 |
| 场景坐标关系 | Scene Coordinate Relationship | シーン座標の関係 |
| 原始场景底部（固定） / 固定基准 | Source Scene Base (Fixed) / Fixed Reference | 元のシーン底面（固定） / 固定基準 |
| 场景参考 / 单位换算 / 配准未确认 | Scene Reference / Unit Conversion / Registration Unconfirmed | シーン基準 / 単位換算 / 位置合わせ未確認 |
| 烘焙变换 | Bake Transforms | トランスフォームをベイク |
| 非等比缩放 / 严格保真 | Non-uniform Scale / Strict Fidelity | 不均等スケール / 厳密な保持 |
| 方向球谐系数 / 自定义属性 | Directional SH Coefficients / Custom Attributes | 方向依存の SH 係数 / カスタム属性 |
| 坐标系 | Coordinate System | 座標系 |
| 透视 / 正交 | Perspective / Orthographic | 透視投影 / 平行投影 |
| 包围盒 | Bounding Box | バウンディングボックス |
| 快照 / 恢复 | Snapshot / Recovery | スナップショット / リカバリー |
| 多选 / 范围选择 | Multiple Selection / Range Selection | 複数選択 / 範囲選択 |
| 全选 / 取消全选 | Select All / Clear Selection | すべて選択 / 選択を解除 |
| 卸载所选模型 | Unload Selected Models | 選択したモデルをアンロード |
| 批量移除 | Batch Removal | 一括削除 |

## Multi-item lists

Recovery projects, project snapshots, external backup snapshots, scene models, task records and media sources use extended row selection: Ctrl-click toggles an item, Shift-click selects a range, Ctrl+A selects all and Delete invokes the list's removal operation. Right-click preserves a multi-selection and provides Select All / Clear Selection / Remove Selected. Shortcuts are scoped to the focused list so Delete cannot trim viewport points. Recovery/history/backup dialogs include a compact selection-count bar. Restore requires exactly one selected record; multiple selection never restores several projects over each other.

Recovery directory deletion is permanent, validated against catalog scope and identity, and confirmed once with count and paths (full list in Details). Project snapshot deletion leaves the current project and data intact. External backup deletion removes manifests only and retains shared deduplicated objects, so it does not reclaim the full logical size. Record deletion runs in a worker; successful rows disappear in place and failures remain for retry. Models are unloaded by stable ID without deleting source files. Removing task records preserves logs/output and skips the running task; the active worker row remains correctly addressed after history removal. Media source removal only changes the pending import list. All captions, counts and explanations change language immediately without resetting selection.

## Task details and retained logs

The shared **Tasks and Logs** panel provides **Task Details**, **Open Task Folder** and **Copy Task Summary** for the selected task. Double-clicking a task opens its details. The dialog reports its original task name, current state, start/end time, configured output directory and retained log tail, with summary-copy and log-export actions. Output navigation uses existing local directories only; it never creates a missing folder or opens a remote URL. A task folder is a configured location, not proof of completed or valid output. Launch attempts are recorded before starting the worker, so a missing or blocked executable still leaves a failed task with its diagnostic instead of disappearing from history.

Task records and their logs are **session-only**, not a persistent task queue. Each record keeps at most the newest 512 Ki UTF-16 code units (524,288 units); a truncation notice appears when earlier text has been dropped. The global log view retains at most 10,000 Qt text blocks. The UI describes this as lines, but a Qt block can contain line breaks. **Copy Task Summary** copies task metadata and retention/truncation notices, not the raw log body. **Export Retained Logs** and **Export Current Logs** save only the currently retained text, not a complete worker transcript. Export uses UTF-8 and atomic replacement; a failed write keeps an existing destination intact. External raw log text, task names and paths stay verbatim. Removing a task record does not delete generated files or the separate global log view.

**Follow Latest Logs** can be disabled while reading or selecting earlier text. Incoming messages do not force that view back to the newest message; turning follow on resumes tailing. Language changes update actions, field captions, state labels and retention notices immediately, retaining the active task, worker state, original names/paths/logs, and log selection/scroll position. Details continue to refresh as a running task produces output or finishes.

This action center is shared by media import, COLMAP reconstruction, 3DGS, 2DGS, bounded/unbounded TSDF, SuGaR and GS2Mesh, including their optional OpenMVS texturing stages. The presentation and task-launch checks do not change generation parameters or fabricate training metrics for other job types. No new backend retry, automatic restart, persisted queue, or optimizer-state resume is introduced. Acceptance uses actual window task/log operations and the worker's task-start signals, including failed executable launches, without starting long-running research jobs.

中文：在“任务与日志”中选中任务后，可查看详情、打开已有任务文件夹及复制摘要；双击任务也能打开详情。日志只在本次会话保留，每项最多保留最近 512 Ki UTF-16 代码单元，全局日志最多 10000 个文本块；复制摘要不包含日志正文，导出的只是当前保留内容而非完整日志。关闭“跟随最新日志”后可以阅读、选择旧内容，不会被新消息强制拉回末尾。启动失败仍会留下任务和错误信息。上述功能适用于所有支持的导入、重建、训练及网格生成流程，不增加持久队列、重试或续训能力。

日本語：「タスクとログ」でタスクを選択すると、詳細の表示、既存のタスクフォルダーを開く操作、概要のコピーを利用できます。ダブルクリックでも詳細を表示できます。ログはこのセッションでのみ保持し、各タスクは最新の最大 512 Ki UTF-16 コード単位、全体のログは最大 10000 テキストブロックです。概要のコピーにはログ本文は含まれず、エクスポートも保持中の内容のみで、完全なログではありません。「最新のログを追跡」を無効にすると、新しいメッセージで末尾へ強制移動されず、過去の内容を読んだり選択したりできます。起動に失敗したタスクと診断も保持します。すべての対応インポート・再構築・学習・メッシュ生成に共通し、永続キュー、再試行、学習再開機能は追加しません。

## Experiment history and observation controls (2026-10-08)

**Experiments and Generation History** is a separate persistent metadata archive,
not a replacement for the session-only task logs above. Every native training or
mesh experiment retains its own settings, input/output locations, recorded state,
result association and resume availability; optional OpenMVS stages belong to
their mesh experiment. State labels, availability reasons, dialog/action captions
and launch-source summaries follow `AppLanguage` immediately. The selection,
raw parameter JSON, saved camera/model/worker state and unchanged source report
are retained when only the language changes. Parameter keys, backend identifiers,
user names, filenames, paths and external logs remain verbatim. Opening history
or changing language never deserializes a checkpoint or starts a worker. Native
3DGS/2DGS full-state resume is explicit and verified; mesh/texture history must
display its unsupported resume reason, not suggest PLY-based optimizer recovery.

**Pause Preview** must be translated distinctly from **Pause Training**. Its
checked state freezes only the task picture while background work continues;
the overlay says so only while a task is actually running. A retained terminal
observation instead says that resuming will show the latest available frame.
Progress/stage captions remain live, and **Task Preview** remains a separate
layer-selection action. New tasks/project-context changes reset preview pause.
Resource-yield explanations describe display/observation scheduling, not a hard
training budget or maximum Gaussian count.

The shared 3DGS/2DGS training dialog reports **Camera and Reconstruction Sources**
and the actual **COLMAP Decision**. Translate source type, minimum-viability
reasons and reuse/rerun/undistortion/repair explanations, but never the file paths
or machine-facing decision identifiers. Unknown/unavailable counts are not
rendered as a fabricated zero. Transforms frame counts and reference checks are
not labeled as COLMAP registration/calibration counts or geometric accuracy.
Language changes redraw the stored report without rescanning sources or changing
the worker configuration; dataset/request changes perform their own source check.

中文：“实验与生成档案”持久保存各训练/网格实验的参数、输入输出、状态、成果和续训可用性，与仅会话保留的任务日志分开；可选 OpenMVS 随所属网格实验归档。所有状态、原因、操作与来源摘要即时切换三语，保留选择和原始参数 JSON，文件名、路径、标识与外部日志不翻译。打开档案不启动训练或读取检查点。3DGS/2DGS 的完整续训需要明确操作与核验；网格/纹理必须说明未支持，而不是暗示 PLY 可恢复优化器。“暂停预览”只冻结观察，后台处理继续；已结束任务使用“最新可用画面”的说明，不误报训练仍在运行。“任务预览”是单独的层切换。来源预检显示真实复用/重跑/去畸变/修复决定，语言切换仅重绘已有报告，不重扫数据或改参数；最低有效性不是精度保证。

日本語：「実験と生成履歴」は各学習・メッシュ実験の設定、入出力、状態、成果、再開可否を永続化し、セッション内のタスクログとは分けます。任意の OpenMVS は所属メッシュ実験に記録します。状態・理由・操作・参照元概要は三言語で即時更新し、選択と元の JSON を保持します。ファイル名、パス、識別子、外部ログは翻訳しません。履歴を開いても学習や状態の読み込みを始めません。3DGS/2DGS の完全な再開には明示操作と検証が必要で、メッシュ・テクスチャは未対応の理由を示し、PLY から最適化器を復元できるとは表示しません。「プレビューを一時停止」は観察のみを止め、バックグラウンド処理は継続します。終了済みのタスクは最新の使用可能な表示について説明し、学習中とは誤表示しません。「タスクプレビュー」は別のレイヤー切り替えです。参照元の事前確認は実際の再利用・再実行・歪み補正・修復の方針を示し、言語変更では既存の報告のみを再描画して再検査や設定変更をしません。最低限の有効性は精度保証ではありません。

The public desktop boundaries are `native_training_isolation`,
`native_generation_history` and `native_training_preflight`; catalog validation
also scans their new user-facing source strings. Full workflow/capability details
are in [GENERATION_PIPELINES.md](GENERATION_PIPELINES.md),
[TRAINING_RESUME.md](TRAINING_RESUME.md) and
[CONTINUOUS_TRAINING_PREVIEW.md](CONTINUOUS_TRAINING_PREVIEW.md).

## Window and file-dialog controls

Qt file dialogs now initially use at least 640 logical pixels of width (960 at
150% UI scale), clamped to the available screen, without increasing their
initial height or forcing a new minimum size. Live language/theme changes
retain later manual sizes. All toolbar labels inherit the toolbar background
instead of showing white rectangles in light appearance.

中文：文件窗口默认宽度至少为 640 逻辑像素，150% 界面缩放时为 960，且不超出
当前屏幕；不增加初始高度、不限制后续缩窄。语言与主题切换保留手动尺寸。
工具栏标签继承工具栏底色，浅色主题不再出现白色文字底块。

日本語：ファイルダイアログの初期幅は 640 論理ピクセル以上（UI 150% では
960）とし、画面内に収めます。初期高さを増やさず、後の手動縮小を妨げません。
言語やテーマの切り替えでも手動サイズを保持します。ツールバーのラベルは
背景色を継承し、ライトテーマで白い四角が現れません。

All Qt open/save/folder dialogs include a translated **Desktop** shortcut in their sidebar. The location comes from `QStandardPaths::DesktopLocation` (including redirected desktops), not a hard-coded `C:` path. Navigation does not create files or change the filename/type. Existing sidebar places and directory history remain available. There is no duplicate window-control row or second Desktop button.

The same left column includes **Frequent Folders** and **Frequent Files**. A successful dialog selection increases its usage count; more-used entries come first, with the most recently accepted choice breaking ties. Clicking a folder navigates to it; clicking an existing file navigates and selects the original Unicode filename without accepting the dialog. Browsing or cancelling does not enter the history. Save destinations remember only their existing parent folder, not an input file. The app's local preferences retain at most 128 folder and 128 file records across launches, within a 512 KiB encoded limit; oldest records are evicted so new choices remain eligible. The compact list displays up to eight available folders and twelve compatible existing files, and follows the current file-type filter. This records confirmed file-dialog choices, not the success of downstream model import or training. Source files are never rewritten by the history feature.

**Clear History** asks for confirmation with No as the default and clears only these application preference records. The current directory, filename, filter and file selection remain unchanged; source files are neither deleted nor modified. Language and font-scale changes update the sidebar's text and row metrics immediately without resetting those inputs or the user's resized window. Long place names are elided within the left column rather than adding a horizontal scrollbar.

中文：文件窗口左栏增加“常用文件夹”和“常用文件”，按成功选择次数排序，同次数时最近使用优先。点击文件只定位并选中，不直接打开；取消及仅浏览不记入历史。记录保存在本机程序设置中，重开程序后仍保留，不修改原文件。文件列表跟随当前类型筛选，失效路径不显示。“清除历史”需要确认，仅清除记录，保留当前目录、文件名、筛选和选择，不删除文件。语言与字号即时更新，长路径名称在左栏内省略显示。

日本語：ファイルダイアログの左側に「よく使うフォルダー」と「よく使うファイル」を表示します。選択を確定した回数で並べ、同数なら最近の使用を優先します。ファイルをクリックすると元の名前で選択するだけで、ダイアログは確定しません。キャンセルや閲覧のみは履歴に追加せず、再起動後も本機のアプリ設定に履歴を保持します。ファイル一覧は現在の種類フィルターに従い、無効なパスは表示しません。「履歴を消去」は確認後に記録のみを削除し、現在のフォルダー、ファイル名、フィルター、選択と元ファイルを保持します。言語と文字サイズは即時更新し、長い名前は左欄内で省略表示します。

File-dialog detail views stretch the filename column to the available width in normal, maximized and full-screen windows. Metadata columns remain individually resizable; language, appearance and UI-scale changes retain user widths, sort order, selection, directory, filename and file type, growing a column only when needed to fit its translated header. Navigation icons and the Desktop sidebar's minimum width follow the actual font/UI scale, not the maximized window's dimensions. Light-mode splitter handles and header backgrounds use the same soft gray as panel boundaries, including unused header space; form labels inherit the dialog background instead of painting white rectangles.

Training, reconstruction, meshing and model-export forms keep their controls top-aligned, wrap long rows and scroll when space is short; action buttons stay outside the scrolling body. Media import retains its independently scrollable source list. Enlarging a window increases usable content width rather than stretching control heights or row spacing. These are shared presentation changes for all supported generation pipelines, not changes to training parameters or model data.

中文：文件窗口的名称列随窗口宽度伸缩，其他列仍可手动调整；分隔带与表头统一为柔和浅灰。图标和侧栏按界面字号适配，保留文件名、格式、排序与选择。参数表单可换行及滚动，底部操作按钮保持可见。

日本語：ファイルダイアログの名前列はウィンドウ幅に追従し、他の列は手動で幅を調整できます。区切りとヘッダーを柔らかなライトグレーに統一します。アイコンとサイドバーは UI の文字サイズに追従し、ファイル名、形式、並び順と選択を保持します。設定フォームは折り返しとスクロールに対応し、操作ボタンは下部に表示したままにします。

The main window and dialogs keep only the native Windows caption controls: title-bar left double-click **maximizes/restores**, not full screen. **Full Screen** remains available through the View menu or `F11`; `Esc` or `F11` exits it. Full screen restores the previous normal geometry/maximized state without resetting input/model/worker data. Popups and tooltips are excluded. Double-clicking files, text fields or model geometry retains the existing open/select/recenter operation. `Esc` exits full screen first; only a subsequent `Esc` invokes the dialog's normal cancel behavior. Labels change live in all three languages.

Main windows and dialogs also enable the native **Minimize** button before their first native show. No flag recreation is performed on visible dialogs. Minimizing and restoring does not accept/cancel a file dialog or reconstruct its controls; the directory, filename, file type and current selection remain in progress. Qt continues to own floating dock-panel flags and dragging.

Project, Properties and Tasks and Logs panels can be dragged by their compact title bar to any of the four dock areas, split/tabbed with other panels, or floated. Double-click their panel title to dock/undock; Ctrl-drag keeps them floating. Panel captions retain only dock/undock and close buttons; floating panels also support F11/Esc. Qt owns their window flags and mouse drag sequence. Initial floating sizes are 360 × 520 logical pixels for side panels and 840 × 460 for Tasks and Logs (scaled for the UI and clamped to the screen); existing usable restored sizes are retained. Floating size is remembered separately from docked extents and across sessions, without imposing a new minimum size. The bottom dock stays compact by default. Reset Layout also returns detached panels to their default dock areas.

## Shared scene-coordinate explanations

The Properties panel's **Scene Coordinate Relationship** summary and tooltip
refresh through the existing `updateInspector` language path. **Append** explains
that source coordinates are retained and that declared-unit conversion is not
registration or CRS reprojection; its open dialog and buttons also follow live
language changes. **Source Scene Base (Fixed)** describes the union of the
collection's unedited source bounds after declared-unit conversion. Selection,
translation, rotation and scale do not change the plane; adding, removing or
replacing a source model recalculates it. The **Fixed reference Z=%1** overlay
retains the placeholder and source double precision, and never implies a model
movement constraint. The tooltip explains the same distinction in all three
languages. The machine-facing `modelBase` preference and persistence schema
remain unchanged. Saving/reopening with the same sources preserves the plane;
source filenames, CRS text and numerical coordinate data are never translated
or rewritten.
See [SCENE_COORDINATES.md](SCENE_COORDINATES.md) for the shared-frame contract and
the distinction between object transforms, display precision and registration.

中文：“场景坐标关系”摘要与提示通过现有属性刷新即时切换语言。导入窗口明确
保留源坐标，区分单位换算与配准／CRS 重投影。“原始场景底部（固定）”使用
换算已声明单位后的未经编辑源范围并集，不随选择、移动、旋转、缩放和保存重开
变化；新增、移除或替换源模型时重新计算。“固定基准 Z=%1”保留占位符和源数据
双精度；提示明确其只是观察参考，不限制模型移动。内部设置标识仍为 `modelBase`，
工程格式不变。文件名、CRS 和坐标数据不翻译。

日本語：「シーン座標の関係」の概要とヒントは既存のプロパティ更新で即時に
言語を切り替えます。インポートでは元の座標を保持し、単位換算と位置合わせ／
CRS 再投影を区別します。「元のシーン底面（固定）」は宣言済み単位を換算した
編集前のソース範囲の和集合を使い、選択、移動、回転、スケール、保存後の
再読み込みでは変えません。ソースモデルの追加、削除、置換時に再計算します。
「固定基準 Z=%1」はプレースホルダーと元データの倍精度を保持します。ヒントは
観察用の基準であり、モデルの移動を拘束しないことを説明します。内部設定の
`modelBase` とプロジェクトの形式、ファイル名、CRS、座標値は保持します。

## Editing guard

**Lock Editing Tools** is a persistent view preference, available beside **Inspect** in the selection toolbar and in the **View** menu (`Ctrl+Shift+L`). Locking cancels uncommitted transforms and selection gestures, hides transform handles, the transform strip, model bounds and model-selection tint, and disables transform/trim/delete/undo/redo actions. It preserves committed transforms, selection, gizmo mode and undo history. Camera orbit/pan/zoom, axis views and Find Model remain available; viewport geometry clicks navigate without changing selection. Unlocking restores the tools in Inspect mode without resuming an unfinished edit. The action and locked-state labels update immediately in all three languages without changing the lock state. This is a viewport editing guard, not a project/file permission lock: explicit import, project-tree selection and background processing remain available.

## References

### Model file export

SPZ delivery is available for Gaussian models only, with v4 (Zstandard) / v3 (gzip), Compact / Balanced / High Precision and a maximum SH degree. The native Niantic/Adobe MIT codec is compiled from pinned source. Export keeps all undeleted source splats, not the viewport sample. SPZ is quantized/lossy at every quality level; lower SH degrees intentionally remove view-dependent information. It does not carry arbitrary PLY metadata, CRS, units, or optimizer state. Original source coordinates only, no Gaussian transform baking, no hidden recenter/scale; positions outside the format's 24-bit fixed-point range (approximately ±2048 source units at 12 fractional bits) are rejected. The buffered codec has a conservative 2 GiB working-memory budget; use PLY for larger models. Cancellation is honoured between codec stages and before atomic publication.

**Import Model (PLY / SPZ)** accepts standard SPZ v1–v4 and decodes it off the UI thread to a project-owned PLY working copy. The original SPZ and existing objects are untouched; Append / Replace remains explicit. The work copy follows normal project save-as/recovery migration. Unsupported extensions/flags are rejected because they may change coordinate conventions. RGB/SH, scale, quaternion and opacity are decoded, with finite ±20 opacity logits at the quantized 0/255 endpoints. The original SPZ's quantization loss cannot be undone by saving a PLY. Native resident Gaussian rendering supports SH degrees 0–4. Options, explanations and progress text switch language immediately without changing parameters.

**SH Display** is a rendering-only, source-capped degree preference (`view/maximumShDegree`, -1 = automatic, 0–4 = cap), separate from the export degree. Menus/tooltips are bound through `AppLanguage`, and overlays translate the actual effective degree per frame. Language changes and quality changes never reload the scene or reset workers. Raw DC must not be clamped before evaluating higher bands. Missing/invalid/budget-limited SH falls back to clearly labeled DC; the live shared-GPU training protocol remains DC-only. Per-model SH sidecars have a 512 MiB CPU budget and must fit the device's buffer-texture limit. Ordinary point clouds and disk-paged previews do not acquire SH storage.

**File → Export Model** (`Ctrl+E`) and the labeled toolbar button are separate from **Save Project**. Export works without cropping and while editing tools are locked. It exports the active model only, reading complete source records rather than preview/LOD buffers. Model selection, transforms, undo history and the original source remain unchanged. Point deletions are applied to exports. Current processing must finish first; live GPU-only training state is not exported as if it were a complete file.

Formats: PLY preserves source vertex attributes, topology and face UVs; unchanged sources stream in large blocks. XYZ/CSV contain XYZ and 8-bit RGB in the selected export-coordinate units. OBJ contains full triangulated geometry, normals, vertex colors and face UVs plus a companion MTL/texture directory. Binary STL contains triangles only (no colors, texture or unit metadata). GLB 2.0 stores meshes or points, embeds PNG textures and uses Y-up/metres; an unknown absolute scale is rejected rather than assuming one unit equals one metre. GLB writes local float coordinates with a separate translation for large-coordinate models; the format's 32-bit file size bounds are checked. Conversion formats do not carry arbitrary PLY fields or CRS as a standardized coordinate system. GLB includes source-coordinate metadata in application-specific extras.

The export explanation distinguishes **Original Coordinates**, which retain source units, from **Scene Coordinates**, which apply object transforms and convert known source/reference units into the shared scene-reference unit. Unknown units keep the raw numeric factor rather than guessing a physical scale. GLB performs its separate format-required metre conversion afterwards. Gaussian PLY without a coordinate provenance record supports both choices; recorded PLY uses its own validated frame-recovery controls and scene-edit checkbox. SPZ remains original-coordinate-only. Coordinate/dimension reports retain source units; their shared reference-plane elevation is converted back to the active source unit. Neither export-unit conversion nor report output performs CRS reprojection or geometric registration. These explanations update immediately with the language without changing export options.

中文：导出说明区分“原始坐标”的源单位与“场景坐标”的场景参考单位；仅在单位已知时换算，未知单位不猜物理比例。GLB 随后按格式约定转为米制；不带坐标来源记录的高斯 PLY 支持两种坐标，带记录的 PLY 使用已验证的框架恢复选项及场景变换复选框。SPZ 仍限原始坐标。坐标／尺寸报告使用源单位，共享基准面高程也换算回当前源单位；不执行 CRS 重投影或配准。语言切换只刷新说明，不改变导出选择。

日本語：エクスポート説明では、「元の座標」のソース単位と「シーン座標」のシーン基準単位を区別します。既知の単位のみ換算し、不明な単位の物理的な比率は推測しません。GLB はその後に形式の規約に従ってメートルへ変換します。座標の出所記録がないガウシアン PLY は両方の座標を扱い、記録付き PLY は検証済みの座標系復元設定とシーン変換のチェックボックスを使用します。SPZ は元の座標に限定します。座標／寸法レポートはソース単位を使い、共通基準面の高さも現在のソース単位へ戻します。CRS 再投影や位置合わせは行いません。言語変更は説明のみ更新し、選択した出力設定を保持します。

For PLY without a coordinate provenance record, the dialog offers original coordinates or baked translation/rotation/scale about the model pivot; temporary viewport shifts are never exported. These Gaussian PLY files default to source coordinates but now enable scene-coordinate baking for both 3DGS and 2DGS. Baking recalculates Gaussian centers, scales, orientations and directional SH appearance while preserving all Gaussian and custom properties; transformed values are not byte-for-byte copies. Unknown custom values retain their original values without inferring a geometric interpretation. Uniform scales (including reflections) can transform directional SH within the original degree. Strict-fidelity export rejects non-uniform scaling when any directional SH coefficient is nonzero, rather than silently projecting to the same degree, removing SH, or approximating its appearance. Non-uniform geometry with DC-only or all-zero directional SH remains supported. Original-coordinate export retains its source-record preservation path. The coordinate selector, dynamic explanation and rejection messages follow live language changes without resetting the chosen format, coordinates or destination; SPZ disables the selector and enforces original coordinates.

中文：不带坐标来源记录的 3DGS／2DGS PLY 默认保留原始坐标，也可选择场景坐标烘焙位移、旋转和缩放。烘焙重算中心、尺度、方向与方向相关球谐外观，不能称为逐字节不变；全部高斯属性和自定义属性仍保留，未知自定义值不猜测几何含义。等比缩放（含镜像）可在原阶数内变换方向球谐；非等比缩放遇到任意非零方向球谐系数时，严格保真模式明确拒绝，不静默近似、降阶或丢弃属性。只有 DC 或方向球谐全零时，仍可烘焙非等比几何。原始坐标模式继续保留源记录。格式、坐标、输出路径在三语切换时不重置，说明即时更新；SPZ 强制原始坐标。

日本語：座標の出所記録がない 3DGS／2DGS PLY は既定で元の座標を保持し、シーン座標を選ぶと移動・回転・スケールをベイクできます。中心、スケール、向き、方向依存の SH 外観を再計算するため、バイト単位の保持とは異なります。すべてのガウシアン属性とカスタム属性を保持し、未知のカスタム値の幾何学的な意味は推測しません。均等スケール（鏡映を含む）では元の次数内で方向依存の SH を変換できます。不均等スケールで方向依存の SH 係数が一つでもゼロでない場合は、厳密な保持のため明示的に拒否し、近似・次数削減・属性破棄を黙って行いません。DC のみ、または方向依存の SH がすべてゼロなら、不均等な幾何形状のベイクも可能です。元の座標モードはソースレコードを保持します。三言語の切り替えで形式・座標・出力先をリセットせず、説明を即時に更新します。SPZ は元の座標を強制します。

Recorded PLY keeps the existing validated coordinate-recovery route, output-unit selector and **Also apply the current model's scene transform** checkbox. Its model and camera records share a proper Sim3 chain, so only proper rotations and positive uniform scales are accepted; reflections and non-uniform edits are rejected even when they would be representable by an unrecorded Gaussian alone. Directional SH rotation is supported by this route. Presence of a coordinate sidecar, including an invalid one, never enables the plain scene-coordinate path as a bypass. An invalid record must be repaired or re-established explicitly, not silently discarded. The original checkpoint and source assets are not transformed.

中文：带坐标来源记录的 PLY 保留已验证的框架恢复路线、输出单位选择与“同时应用当前模型的场景变换”复选框。模型和相机共用正相似变换链，因此只接受正确旋转与正等比缩放；即使单独高斯可表达，也拒绝镜像和非等比编辑。该路线支持方向球谐旋转。有坐标记录（包括失效记录）时，不能通过普通场景坐标路线绕过验证；必须明确修复或重新建立记录，不静默丢弃。原始检查点和源资源不变换。

日本語：座標の出所記録付き PLY は、検証済みの座標系復元ルート、出力単位選択、「現在のモデルのシーン変換も適用」チェックボックスを維持します。モデルとカメラは向きを保つ Sim3 チェーンを共有するため、適切な回転と正の均等スケールのみを受け付け、単独のガウシアンで表現できる場合でも鏡映と不均等な編集を拒否します。このルートは方向依存の SH 回転に対応します。無効なものも含め座標記録がある場合、通常のシーン座標ルートで検証を迂回することはできません。記録は明示的に修復または再作成し、黙って破棄しません。元のチェックポイントとソースアセットは変換しません。

Gaussian XYZ/CSV/GLB exports are explicitly labeled as center points/base colors, not splat appearance or reconstructed surfaces. OBJ/STL require real mesh faces. Mirrored mesh transforms reverse winding in GLB/OBJ/STL; PLY explicitly requires original coordinates for mirrored meshes. Texture export supports the loader's single-texture, face-UV model; missing textures fail rather than silently disappearing. Generated opaque-diffuse triangle materials can first use the native-owned OBJ/MTL/image bridge to a PLY plus full-resolution atlas, preserving original output packages and UV seams. This internal generated-result adapter enables automatic preview and the existing PLY/OBJ/GLB export paths; it is not general OBJ/GLB import or support for arbitrary material shaders. See MESH_GENERATION.md for the 8192 px / 256 MiB limits and explicit failure contract.

Export runs off the UI thread with progress and cancellation. Temporary geometry resides beside the destination on its chosen drive with a bounded 64 MiB vertex-page cache. The final model uses atomic replacement; unique companion asset directories are retained only on success. Failed or cancelled exports keep an existing destination unchanged and remove only their own temporary files. Saving over a loaded source/project is blocked. Export does not mark crop history as saved in the project or turn conversion into an import.

Format references: [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html), [Wavefront OBJ specification](https://paulbourke.net/dataformats/obj/obj_spec.pdf), and [STL structure](https://paulbourke.org/dataformats/stl/). Explicit attribute-loss notices, source-coordinate handling and round-trip tests guard export fidelity.

Observation navigation uses the familiar [Metashape Model-view navigation controls](https://www.agisoft.com/pdf/metashape_2_3_en.pdf) and Agisoft's [surface double-click centering guidance](https://www.agisoft.com/forum/index.php?topic=7955.0), with unrestricted viewport dragging: while editing is locked, left-drag orbits freely anywhere in the canvas, Ctrl+left-drag pans, Shift+left-drag zooms, and the wheel zooms. Right/middle drag remain pan aliases. Free orbit uses camera-local quaternion rotation with consistent logical-pixel sensitivity, even at the poles, across the sphere boundary and with the sphere hidden; the former sphere-exterior roll-only mapping is no longer used. Pressing a colored front arc explicitly constrains rotation about its world axis for that drag. Ctrl/Shift can interrupt rotation while the button remains held; releasing the modifier resumes free orbit without snapping back or capturing another arc. Pan takes priority over zoom when both are requested. Double-clicking model geometry sets the rotation center and recenters the view without changing model transforms, selection, zoom scale or projection. Empty space does not move the center. The GPU-only pick pass reads the nearest covered pixel's depth and unprojects that exact physical pixel (HiDPI included), considering all model layers and their display shifts/transforms. Meshes use triangle depth; point clouds and Gaussians use their rendered point centers, including resident paged data and live preview buffers. It does not load full source datasets or target grid/transform overlays. The navigation sphere is a screen-space guide (24% of the shorter viewport dimension), separate from world-space model gizmos; **View Trackball** toggles its visibility immediately and persistently. No proprietary Metashape code or assets are copied.

`--smoke-test-observation-navigation` drives real viewport mouse events against a disposable point fixture; optional `--smoke-scene` accepts a read-only real model. It covers radial drags outside the sphere, boundary crossing, hidden-sphere navigation, modifier transitions, pan/zoom, projection retention and unchanged model data. The matching CTest is `native_observation_navigation`.

Editing-view orbit, temporary Ctrl+left orbit in trim tools, and the corner navigation rotation control use the same camera-local screen directions as locked observation. Rolled, upside-down and pole views do not reverse or swap pointer directions when editing is unlocked. Existing pan/zoom bindings and object transform tools are unchanged; navigation changes only the camera. This shared viewport behavior applies to point clouds, 3DGS/2DGS previews and meshes. `--smoke-test-edit-navigation` checks real viewport input and displayed motion with disposable fixtures in the three UI languages.

中文：编辑模式下的视角旋转、裁剪工具中的 Ctrl+左键临时旋转以及角落导航旋转控件，均与锁定观察模式使用相同的屏幕方向。侧转、倒置或越过极点后，解锁编辑不会让上下左右反向或串方向。原有平移／缩放快捷键和模型变换工具保持不变，导航只改变相机；点云、3DGS／2DGS 预览和网格共用此逻辑。

日本語：編集モードの視点回転、トリミング中の Ctrl+左ドラッグによる一時的な回転、隅のナビゲーション回転操作は、編集ロック時と同じ画面方向を使用します。視点が傾いた状態、上下逆の状態、極点を越えた状態でも、編集ロック解除後に上下左右が反転したり入れ替わったりしません。パン／ズームの既存操作とモデルのトランスフォームは変更せず、ナビゲーションはカメラだけを動かします。点群、3DGS／2DGS プレビュー、メッシュで共通の動作です。

Terminology references: [CloudCompare official translations](https://github.com/CloudCompare/CloudCompare/tree/master/qCC/translations), [Agisoft official manuals](https://www.agisoft.com/downloads/user-manuals/), and [Blender Japanese manual](https://docs.blender.org/manual/ja/latest/). Application messages are original translations, not wholesale copies of another application's language library. Loading follows the official [QTranslator documentation](https://doc.qt.io/qt-6/qtranslator.html). Qt base catalogs remain upstream Qt assets; see `THIRD_PARTY_LICENSES.md`.

## Paged mesh preview

Native loading is not required to page ordinary models merely because the
legacy HTML did. Resident mesh admission now follows available RAM/VRAM and a
shared reservation ledger rather than fixed five-million / 1 GiB thresholds.
Estimates include UV-corner duplication, actual GPU buffers, CPU transient
arrays and texture conversion/mip levels. Oversized polygon fans rewind into the
existing disk-backed path, never a silently sampled resident mesh. Complete
cache parents (all valid source triangles present) need no further refinement.
Page reads can run 16 at a time with a bounded pending/in-flight byte allowance.
Startup loads wait for the actual OpenGL device probe. Manual settings apply
immediately; existing meshes can change residency without resetting the camera
or transform. Model loading and display targets are shared, not separate pools
for every object. Texture atlases are included but cannot themselves be paged;
insufficient mandatory storage reports a resource error. This is not an OS hard
quota, a training-process scheduler or a continuous-surface LOD algorithm.

中文：常规模型不因 HTML 旧版逻辑而必须分页。根据实时可用内存、显存和共享
资源预留决定是否常驻，不再采用默认 500 万／1 GiB 门槛；计入 UV 接缝、实际
GPU 缓冲区、CPU 临时数组和纹理转换／mip 层级。多边形展开超预算时回到磁盘分页，
不悄悄抽面。完整父页不再重复细分；分页读取最多并行 16 项并限制排队字节数。
预算设置即时生效；已有模型可改变常驻方式而保留视点和变换。纹理计入预算，
但纹理本身不支持分页，必需资源不足会明确报错。这不是操作系统硬配额、训练
资源调度器或新的连续曲面 LOD。

日本語：通常のモデルは HTML 旧版を理由にページングする必要はありません。
現在の空き RAM・GPU メモリと共有予約台帳で常駐を判断し、既定の 500 万／
1 GiB のしきい値を使用しません。UV シーム、実際の GPU バッファ、CPU 作業
配列、テクスチャ変換と全 mip レベルを含みます。多角形の展開が予算を超えるとディスク経由へ
戻し、面を黙って間引きません。完全な親ページを再分割せず、最大 16 件の
並列読み込みと待機バイト数を制限します。予算は即時反映され、モデル変換と
視点を保持して常駐方式を変更できます。テクスチャ自身はページングせず、
必須領域が不足すると明示的なエラーを表示します。OS の強制配分上限、学習
リソースのスケジューラ、新しい表面維持 LOD を保証するものではありません。

Acceptance for this repair compares resident and paged framebuffers against matching empty backgrounds, retaining at least 90% of a reference surface with at least 1,000 interior pixels. The reported 2DGS model, bounded/unbounded TSDF, OpenMVS-textured and SuGaR plain/textured outputs passed this gate in light and dark installed builds. The existing reduced GS2Mesh fixture has only 297 reference interior pixels across sparse disconnected islands; paging retains those pixels, but the fixture cannot establish continuous-surface correctness. Dense GS2Mesh output acceptance remains pending with a suitable bounded, provenance-verified fixture; the common renderer fix is not a claim that GS2Mesh generation quality has been validated.

中文：本次修复以相同背景下的常驻与分页实际画面比较验收，参考表面至少有 1000 个内部像素，分页必须保留至少 90%。用户报告的 2DGS 模型、有界/无界 TSDF、OpenMVS 纹理网格与 SuGaR 普通/纹理网格均已在安装版的深浅主题下通过。已有缩减版 GS2Mesh 样本只有分散小片和 297 个参考内部像素；分页保留了这些像素，但该样本不能验证连续表面。仍需合适、有来源证明且规模可控的稠密 GS2Mesh 样本完成验收；共用显示修复不代表 GS2Mesh 生成质量已经验证。

日本語：本修正は同じ背景で常駐表示とページング表示の実際の画像を比較し、参照面の内部画素が 1000 以上で、その 90% 以上を保持することを条件とします。報告された 2DGS モデル、有界・無界 TSDF、OpenMVS のテクスチャ付きメッシュ、SuGaR の通常・テクスチャ付きメッシュは、インストール版の明暗両テーマで合格しました。既存の縮小 GS2Mesh サンプルは分散した小片で、参照内部画素は 297 しかありません。ページングで同じ画素を保持しますが、連続した面の正しさは検証できません。出所を確認した適切な規模の高密度 GS2Mesh サンプルによる検証は未完了であり、共通の表示修正は GS2Mesh の生成品質を検証したという意味ではありません。

**Paged Mesh Preview** describes disk-backed mesh display, not a surface-preserving LOD algorithm. Coarse cache ancestors are random subsets of the original triangles and can look incomplete. When the exact visible pages fit the shared viewport GPU budget, the renderer continues loading and displays those complete pages rather than stopping at a random sample because of viewing distance or interaction. Pending CPU page reads are deduplicated before GPU upload. The original PLY is unchanged and UV seams are preserved. **Base color** means source vertex RGB when available, otherwise the default material color; **UV · Base-color fallback** does not claim that an RGB-free mesh contains vertex colors.

This display fix is shared by imported meshes and all supported mesh outputs, including 2DGS TSDF/OpenMVS and 3DGS SuGaR/GS2Mesh; generation parameters and optimizer state are unchanged. Completed pages upload in small batches bounded by eight pages and a four-millisecond scheduling budget per frame (an individual upload may exceed that time), so waiting pages do not accumulate behind a one-page-per-frame limit. If all visible complete pages exceed the shared GPU budget, an incomplete sampled preview can still remain. Genuine surface-preserving large-mesh LOD is a remaining acceptance gate, not a capability delivered by this fix.

中文：“网格分页预览”不是保持连续表面的 LOD 算法；缓存的粗层级只是原始三角形的随机子集，可能显示不完整。可见完整页能放入场景共享 GPU 预算时，会继续加载并显示完整页，不因视距或交互停留在随机采样；待处理的 CPU 页读取在上传前去重。每帧批量上传最多八页，并采用四毫秒调度预算（单次上传可能超时），避免每帧仅上传一页造成积压。原始 PLY 与 UV 接缝不变。“基础颜色”指已有顶点 RGB 或缺少 RGB 时的默认材质颜色。此显示修正覆盖导入网格、2DGS TSDF/OpenMVS 和 3DGS SuGaR/GS2Mesh，不改生成参数或优化器状态。可见完整页超出 GPU 预算时仍可能是不完整采样预览；真正保持表面的大网格 LOD 尚未完成。

日本語：「ページングメッシュプレビュー」は表面を維持する LOD ではありません。キャッシュの粗い階層は元の三角形のランダムな部分集合で、不完全に見える場合があります。可視領域の完全なページがシーン共有の GPU 予算に収まる場合、視距離や操作中であることを理由にランダムサンプルで停止せず、完全なページを読み込んで表示します。待機中の CPU ページ読み込みは GPU 転送前に重複を除きます。各フレームで最大八ページをまとめて転送し、四ミリ秒の処理予算を設けます（単一の転送は予算を超える場合があります）。一フレーム一ページの制限による滞留を防ぎます。元の PLY と UV シームは変更しません。「基本色」は頂点 RGB、または RGB がない場合の既定のマテリアル色です。インポートしたメッシュ、2DGS TSDF/OpenMVS、3DGS SuGaR/GS2Mesh に共通し、生成パラメーターや最適化器の状態は変更しません。完全な可視ページが GPU 予算を超えると、不完全なサンプル表示が残る場合があります。表面を維持する大規模メッシュ LOD は未完了の検証項目です。
