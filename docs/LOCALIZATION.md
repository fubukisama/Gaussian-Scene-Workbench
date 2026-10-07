# Native desktop localization

## Language switching

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

The change is presentation-only and shared by 3DGS, 2DGS, reconstruction and all mesh-generation workflows. Manual and scheduled switches do not reload models, rewrite RGB/SH/materials, alter camera/selection/transforms, clear training curves, interrupt workers or change exported data. Transparent splats naturally composite against the chosen backdrop; this is not relighting or a new reconstruction. `--theme auto|light|dark` is a non-persistent launch/QA override. A manual CLI override disables scheduled switching for that launch without changing the saved mode. The language smoke matrix toggles both themes while a test-owned worker runs and a real file remains selected. It checks 05:59:59.999 → 06:00 → 17:59:59.999 → 18:00 → 23:59:59.999 → 00:00 against independent expected colors, including no switch at midnight and unchanged manual overrides. Real-clock QA invokes the application's timer callback, reports the local time and effective theme, and saves automatic-mode screenshots before restoring a manual mode. Light-mode Gaussian, navigation and mesh-publication regressions complement existing dark tests.

中文：通过“视图 → 外观 → 自动（按时间）／浅色（白天）／深色（黑夜）”即时切换并记住选择。未保存过外观选择时默认自动，已有的手动浅色或深色设置保留。自动模式按本机时间在 06:00（含）至 18:00（不含）使用浅色，其余时间使用深色，午夜保持深色；运行中持续检查，唤醒或修改时钟后最迟 30 秒更新，重新激活程序时立即更新。手动模式不随时间改变。界面与观察背景一起切换，不修改模型或训练数据。
浅色采用白色工作区、灰色工具栏和深色文字，增强控件边界与蓝色选中状态；网格保持主次层级，深色模式和界面尺寸不变。
左右停靠面板与视口之间使用与标题栏统一的浅灰分隔带和柔和细线，基础宽度适度加宽至 8 像素并随界面缩放；悬停保持灰色，仍可拖动调整宽度。

日本語：「ビュー → 外観 → 自動（時刻に応じて）／ライト（昼間）／ダーク（夜間）」で即時に切り替え、選択を保存します。外観設定が未保存の場合は自動が既定で、既存の手動設定は保持します。自動は本機の時刻に従い、06:00 から 18:00 より前はライト、それ以外はダークです。午前 0:00 もダークを維持します。実行中も確認し、スリープ復帰や時刻変更後は 30 秒以内、アプリの再アクティブ化時は即時に更新します。手動モードは時刻に連動しません。UI と観察用背景のみを変更し、モデルや学習データは変更しません。
ライトテーマは白い作業領域、グレーのツールバー、濃い文字で構成し、コントロールの境界と青い選択状態を明確にします。グリッドの主線と補助線を区別し、ダークテーマと UI の寸法は変更しません。
左右のドックパネルとビューポートの間に、タイトルバーと統一したライトグレーの区切りと柔らかな細線を表示します。基準幅を控えめに 8 ピクセルへ広げ、UI スケールに追従します。ホバー時もグレーを保ち、ドラッグによる幅の調整は引き続き利用できます。

## Terminology

Scene and training-output names accept arbitrary Unicode text, spaces and symbols (including characters forbidden in Windows filenames). Only blank names are rejected. `ManagedName.h` maps display names to backend-safe ASCII storage identifiers; matching Python validation must stay in sync. Legacy safe ASCII names keep their paths, except the reserved `gsw-name-` namespace. Names requiring encoding use that prefix plus the full SHA-256 of the exact UTF-8 name. Do not trim, case-fold, translate or sanitize the display name. `.gsw-name.json` retains the original name alongside the dataset and travels with project migration/backup; import publishes it in the same transaction as the data. Existing job configurations without display metadata remain supported. Actual export filenames still obey operating-system filesystem rules.

| 简体中文 | English | 日本語 |
| --- | --- | --- |
| 工程 / 数据集 | Project / Dataset | プロジェクト / データセット |
| 桌面 | Desktop | デスクトップ |
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
| 显存常驻 / 深度顺序 | GPU-Resident / Depth Order | GPU メモリ常駐 / 深度順序 |
| 移动 / 旋转 / 缩放 | Move / Rotate / Scale | 移動 / 回転 / スケール |
| 等比缩放 | Uniform Scale | 均等スケール |
| 全局 / 局部 | Global / Local | グローバル / ローカル |
| 轨迹球 / 吸附 | Trackball / Snap | トラックボール / スナップ |
| 锁定编辑工具 / 操作手柄 | Lock Editing Tools / Gizmo | 編集ツールをロック / ギズモ |
| 观察轨迹球 / 旋转中心 | View Trackball / Rotation Center | ビュートラックボール / 回転中心 |
| 自由旋转 / 按轴旋转 | Free Orbit / Axis-Constrained Rotation | 自由回転 / 軸回転 |
| 导出模型 / 导出坐标 | Export Model / Export Coordinates | モデルをエクスポート / エクスポート座標 |
| 原始坐标 / 场景坐标 | Source Coordinates / Scene Coordinates | 元の座標 / シーン座標 |
| 烘焙变换 | Bake Transforms | トランスフォームをベイク |
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

## Window and file-dialog controls

All Qt open/save/folder dialogs include a translated **Desktop** shortcut in their sidebar. The location comes from `QStandardPaths::DesktopLocation` (including redirected desktops), not a hard-coded `C:` path. Navigation does not create files or change the filename/type. Existing sidebar places and directory history remain available. There is no duplicate window-control row or second Desktop button.

File-dialog detail views stretch the filename column to the available width in normal, maximized and full-screen windows. Metadata columns remain individually resizable; language, appearance and UI-scale changes retain user widths, sort order, selection, directory, filename and file type, growing a column only when needed to fit its translated header. Navigation icons and the Desktop sidebar's minimum width follow the actual font/UI scale, not the maximized window's dimensions. Light-mode splitter handles and header backgrounds use the same soft gray as panel boundaries, including unused header space; form labels inherit the dialog background instead of painting white rectangles.

Training, reconstruction, meshing and model-export forms keep their controls top-aligned, wrap long rows and scroll when space is short; action buttons stay outside the scrolling body. Media import retains its independently scrollable source list. Enlarging a window increases usable content width rather than stretching control heights or row spacing. These are shared presentation changes for all supported generation pipelines, not changes to training parameters or model data.

中文：文件窗口的名称列随窗口宽度伸缩，其他列仍可手动调整；分隔带与表头统一为柔和浅灰。图标和侧栏按界面字号适配，保留文件名、格式、排序与选择。参数表单可换行及滚动，底部操作按钮保持可见。

日本語：ファイルダイアログの名前列はウィンドウ幅に追従し、他の列は手動で幅を調整できます。区切りとヘッダーを柔らかなライトグレーに統一します。アイコンとサイドバーは UI の文字サイズに追従し、ファイル名、形式、並び順と選択を保持します。設定フォームは折り返しとスクロールに対応し、操作ボタンは下部に表示したままにします。

The main window and dialogs keep only the native Windows caption controls: title-bar left double-click **maximizes/restores**, not full screen. **Full Screen** remains available through the View menu or `F11`; `Esc` or `F11` exits it. Full screen restores the previous normal geometry/maximized state without resetting input/model/worker data. Popups and tooltips are excluded. Double-clicking files, text fields or model geometry retains the existing open/select/recenter operation. `Esc` exits full screen first; only a subsequent `Esc` invokes the dialog's normal cancel behavior. Labels change live in all three languages.

Project, Properties and Tasks and Logs panels can be dragged by their compact title bar to any of the four dock areas, split/tabbed with other panels, or floated. Double-click their panel title to dock/undock; Ctrl-drag keeps them floating. Panel captions retain only dock/undock and close buttons; floating panels also support F11/Esc. Qt owns their window flags and mouse drag sequence. Initial floating sizes are 360 × 520 logical pixels for side panels and 840 × 460 for Tasks and Logs (scaled for the UI and clamped to the screen); existing usable restored sizes are retained. Floating size is remembered separately from docked extents and across sessions, without imposing a new minimum size. The bottom dock stays compact by default. Reset Layout also returns detached panels to their default dock areas.

## Editing guard

**Lock Editing Tools** is a persistent view preference, available beside **Inspect** in the selection toolbar and in the **View** menu (`Ctrl+Shift+L`). Locking cancels uncommitted transforms and selection gestures, hides transform handles, the transform strip, model bounds and model-selection tint, and disables transform/trim/delete/undo/redo actions. It preserves committed transforms, selection, gizmo mode and undo history. Camera orbit/pan/zoom, axis views and Find Model remain available; viewport geometry clicks navigate without changing selection. Unlocking restores the tools in Inspect mode without resuming an unfinished edit. The action and locked-state labels update immediately in all three languages without changing the lock state. This is a viewport editing guard, not a project/file permission lock: explicit import, project-tree selection and background processing remain available.

## References

### Model file export

SPZ delivery is available for Gaussian models only, with v4 (Zstandard) / v3 (gzip), Compact / Balanced / High Precision and a maximum SH degree. The native Niantic/Adobe MIT codec is compiled from pinned source. Export keeps all undeleted source splats, not the viewport sample. SPZ is quantized/lossy at every quality level; lower SH degrees intentionally remove view-dependent information. It does not carry arbitrary PLY metadata, CRS, units, or optimizer state. Original source coordinates only, no Gaussian transform baking, no hidden recenter/scale; positions outside the format's 24-bit fixed-point range (approximately ±2048 source units at 12 fractional bits) are rejected. The buffered codec has a conservative 2 GiB working-memory budget; use PLY for larger models. Cancellation is honoured between codec stages and before atomic publication.

**Import Model (PLY / SPZ)** accepts standard SPZ v1–v4 and decodes it off the UI thread to a project-owned PLY working copy. The original SPZ and existing objects are untouched; Append / Replace remains explicit. The work copy follows normal project save-as/recovery migration. Unsupported extensions/flags are rejected because they may change coordinate conventions. RGB/SH, scale, quaternion and opacity are decoded, with finite ±20 opacity logits at the quantized 0/255 endpoints. The original SPZ's quantization loss cannot be undone by saving a PLY. Native resident Gaussian rendering supports SH degrees 0–4. Options, explanations and progress text switch language immediately without changing parameters.

**SH Display** is a rendering-only, source-capped degree preference (`view/maximumShDegree`, -1 = automatic, 0–4 = cap), separate from the export degree. Menus/tooltips are bound through `AppLanguage`, and overlays translate the actual effective degree per frame. Language changes and quality changes never reload the scene or reset workers. Raw DC must not be clamped before evaluating higher bands. Missing/invalid/budget-limited SH falls back to clearly labeled DC; the live shared-GPU training protocol remains DC-only. Per-model SH sidecars have a 512 MiB CPU budget and must fit the device's buffer-texture limit. Ordinary point clouds and disk-paged previews do not acquire SH storage.

**File → Export Model** (`Ctrl+E`) and the labeled toolbar button are separate from **Save Project**. Export works without cropping and while editing tools are locked. It exports the active model only, reading complete source records rather than preview/LOD buffers. Model selection, transforms, undo history and the original source remain unchanged. Point deletions are applied to exports. Current processing must finish first; live GPU-only training state is not exported as if it were a complete file.

Formats: PLY preserves source vertex attributes, topology and face UVs; unchanged sources stream in large blocks. XYZ/CSV contain source-unit XYZ and 8-bit RGB. OBJ contains full triangulated geometry, normals, vertex colors and face UVs plus a companion MTL/texture directory. Binary STL contains triangles only (no colors, texture or unit metadata). GLB 2.0 stores meshes or points, embeds PNG textures and uses Y-up/metres; undeclared source units explicitly default to one metre. GLB writes local float coordinates with a separate translation for large-coordinate models; the format's 32-bit file size bounds are checked. Conversion formats do not carry arbitrary PLY fields or CRS as a standardized coordinate system. GLB includes source-coordinate metadata in application-specific extras.

The dialog offers original coordinates or baked translation/rotation/scale about the model pivot; temporary viewport shifts are never exported. Gaussian PLY preserves its source coordinates and full Gaussian/SH attributes; baking Gaussian transforms remains unsupported and is disabled with an explanation. Gaussian XYZ/CSV/GLB exports are explicitly labeled as center points/base colors, not splat appearance or reconstructed surfaces. OBJ/STL require real mesh faces. Mirrored mesh transforms reverse winding in GLB/OBJ/STL; PLY explicitly requires original coordinates for mirrored meshes. Texture export supports the loader's single-texture, face-UV model; missing textures fail rather than silently disappearing. Generated opaque-diffuse triangle materials can first use the native-owned OBJ/MTL/image bridge to a PLY plus full-resolution atlas, preserving original output packages and UV seams. This internal generated-result adapter enables automatic preview and the existing PLY/OBJ/GLB export paths; it is not general OBJ/GLB import or support for arbitrary material shaders. See MESH_GENERATION.md for the 8192 px / 256 MiB limits and explicit failure contract.

Export runs off the UI thread with progress and cancellation. Temporary geometry resides beside the destination on its chosen drive with a bounded 64 MiB vertex-page cache. The final model uses atomic replacement; unique companion asset directories are retained only on success. Failed or cancelled exports keep an existing destination unchanged and remove only their own temporary files. Saving over a loaded source/project is blocked. Export does not mark crop history as saved in the project or turn conversion into an import.

Format references: [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html), [Wavefront OBJ specification](https://paulbourke.net/dataformats/obj/obj_spec.pdf), and [STL structure](https://paulbourke.org/dataformats/stl/). Explicit attribute-loss notices, source-coordinate handling and round-trip tests guard export fidelity.

Observation navigation uses the familiar [Metashape Model-view navigation controls](https://www.agisoft.com/pdf/metashape_2_3_en.pdf) and Agisoft's [surface double-click centering guidance](https://www.agisoft.com/forum/index.php?topic=7955.0), with unrestricted viewport dragging: while editing is locked, left-drag orbits freely anywhere in the canvas, Ctrl+left-drag pans, Shift+left-drag zooms, and the wheel zooms. Right/middle drag remain pan aliases. Free orbit uses camera-local quaternion rotation with consistent logical-pixel sensitivity, even at the poles, across the sphere boundary and with the sphere hidden; the former sphere-exterior roll-only mapping is no longer used. Pressing a colored front arc explicitly constrains rotation about its world axis for that drag. Ctrl/Shift can interrupt rotation while the button remains held; releasing the modifier resumes free orbit without snapping back or capturing another arc. Pan takes priority over zoom when both are requested. Double-clicking model geometry sets the rotation center and recenters the view without changing model transforms, selection, zoom scale or projection. Empty space does not move the center. The GPU-only pick pass reads the nearest covered pixel's depth and unprojects that exact physical pixel (HiDPI included), considering all model layers and their display shifts/transforms. Meshes use triangle depth; point clouds and Gaussians use their rendered point centers, including resident paged data and live preview buffers. It does not load full source datasets or target grid/transform overlays. The navigation sphere is a screen-space guide (24% of the shorter viewport dimension), separate from world-space model gizmos; **View Trackball** toggles its visibility immediately and persistently. No proprietary Metashape code or assets are copied.

`--smoke-test-observation-navigation` drives real viewport mouse events against a disposable point fixture; optional `--smoke-scene` accepts a read-only real model. It covers radial drags outside the sphere, boundary crossing, hidden-sphere navigation, modifier transitions, pan/zoom, projection retention and unchanged model data. The matching CTest is `native_observation_navigation`.

Terminology references: [CloudCompare official translations](https://github.com/CloudCompare/CloudCompare/tree/master/qCC/translations), [Agisoft official manuals](https://www.agisoft.com/downloads/user-manuals/), and [Blender Japanese manual](https://docs.blender.org/manual/ja/latest/). Application messages are original translations, not wholesale copies of another application's language library. Loading follows the official [QTranslator documentation](https://doc.qt.io/qt-6/qtranslator.html). Qt base catalogs remain upstream Qt assets; see `THIRD_PARTY_LICENSES.md`.
