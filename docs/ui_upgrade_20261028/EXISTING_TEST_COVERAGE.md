# UI Upgrade 2026-10-28 — Existing Test Coverage

## 1. 范围、证据状态与绑定

本表是**源码覆盖映射，不是本轮运行结果**。审阅源码提交为 `4f1dbb8f2ad086b3d855e112b835d6b641f81bf9`；母表为 `SURFACE_INVENTORY.md` / `SURFACE_INVENTORY.json`（最初以 `5183926` 建立）。母表共有 **66 个表面组、16 个独立 QDialog 类、4 个内联 QDialog、4 个 QProgressDialog 入口、147 个 popup 源码调用点、29 个矩阵 ID**。147 是调用点数，不是 147 个独立设计：37 个 QFileDialog、83 个 QMessageBox、26 个其他 Qt 构造点、1 个 Win32 MessageBoxW。

本次未运行任何本地或远端测试、未打开应用或用户工程、未下载 artifact。所有本轮执行 / 图像审阅结果均为 **NOT_RUN**；已有测试只标为 **SOURCE_COVERED**。旧提交 / 历史 run 的 pass 不自动继承为本轮 pass。没有视觉证据的项一律不写“检查后无需修改”。

- **REAL_WINDOW**：真实 `GaussianSceneWorkbench` + Windows 平台 / 真实视口 smoke；仍须本轮日志与 PNG 证明实际执行。
- **OFFSCREEN_CONTROLS**：实际生产控件、配置和 CPU helper 行为，但平台硬编码 offscreen；不是 Windows 标题栏、DPR、OpenGL、字体清晰度证据。
- **QIMAGE_OR_CONTRACT**：数学 / 状态 / 软件绘图 / 文件合同；不能替代真实窗口或输入操作。
- **UI_OPEN_ONLY**：看到真实窗并立刻关闭，只证明入口 / 生命周期，不证明正文布局或深入操作。
- **CAPTURE_CONDITIONAL**：源码有保存 PNG 的分支；必须同时满足 env、测试选择、稳定时点与上传收集，才可称“已留存”。

下文“可冻结”均是**建议在现有公开行为上补充的合同**，不是已经实现的测试或已通过的 case。现有 MainWindow smoke 有 friend / 内部 fixture 安装；本文不把这些安装步骤伪称为公共 API。优先复用实际 QAction、控件输入、公开 getters、信号和测试自有 CPU 进程，不为验收新开生产私有入口。

## 2. CMake 与证据接线事实

测试名以 `native/CMakeLists.txt` 为准：初始化 / 网格 / preset / runtime / batch 独立 adapter 见 666–874；真实应用 tests 见 908–1405；store / 几何 / QImage 辅助 targets 见 1479–2028。不存在名为 `native_viewport_status_controls` 的 CTest：Grid / Details 是 `native_viewport_scheduling_light` 与 `native_viewport_scheduling_dark` 成功路径调用 `runViewportStatusControlsSmokeTest`。

`LanguageSmokeTest.cpp:1943–2032` 响应式循环有 **10 个实例，只有 5 个 Dialog 类**：Training（含 2DGS / 锁定相机等实例）、DatasetImport（普通 / 已知相机）、Reconstruction、MeshGeneration、ModelExport（mesh / Gaussian PLY / SPZ）。循环有 90→150→90% UI 字号、680×460、滚动 / 按钮 / 输入可达性检查；媒体来源动作另有 460→1100→460 重排。不能把它写成“16 类表单全覆盖”。

`LanguageSmokeTest.cpp:2069–2170` 有有条件的真实主窗 / 视口 / 部分表单截图，明确的明暗截图含主窗 / 视口与 Export，另有 Training 3DGS / 2DGS / 原图、已知相机、Meshing / Gaussian PLY / SPZ。独立的语言、主题、字体循环不能相乘后声称是全表单 × 两主题 × DPI 的笛卡尔积。

`native/tests/{InitializationEditorUiTests,InitializationDialogUiTests,MeshGeometryDialogUiTests,TrainingPresetUiTests,QualityPlanTests,RuntimeDecouplingUiTests,TrainingBatchUiTests}.cpp` 的 main **主动 qputenv offscreen**。仅改变 CI 外部 `QT_QPA_PLATFORM` 不会令它们变成真实 Windows 视觉验收。`TransformGizmoVisualTests.cpp` 是 `QTEST_GUILESS_MAIN` 的 QImage 检查。

### 条件截图与上传的实际差异

`.github/workflows/native-windows.yml:106–139` 的 focused upstream step 设置 `GSW_SMOKE_OUTPUT_DIR`、`GSW_LANGUAGE_SCREENSHOT_DIR` 等，并上传 acceptance-artifact / Blender / texture-text 证据；它选入语言、质量、尺度、批量、初始化等 tests，**没有选入** `native_icon_ui_*`、`native_resource_budget_ui`、`native_task_actions_*`、`native_generation_history`。full Test（364–384）会执行完整 CTest，但只显式设置 `QT_FORCE_STDERR_LOGGING`，上传日志与少数 grid 图片。因此：

- IconUI 虽有 atlas / toolbar / edit rail / viewport / window PNG 保存代码，不能仅凭 full suite pass 声称这些图已归档。
- ResourceBudgetUI 的 PNG 分支依赖 `GSW_LANGUAGE_SCREENSHOT_DIR`；当前 focused selection 没有它，full step 也没有该 env。
- 有断言、不保存 PNG 的 TaskDetails / History 仍可保护业务，但不能据此完成本轮视觉验收。
- 更小的补齐是给**被改表面所属的测试**统一设置受控输出目录并上传，而不是重复运行每个图标的一整套 300 秒语言 smoke。

## 3. 可复用的现有测试族

表中 locale 的 `{3 locales}` / `{zh_CN,en_US,ja_JP}` 代表实际展开的三个 CTest 名；组合测试以 CMake 为准。未写 runtime status 即 NOT_RUN。

| 复用代号 | 真实 CTest 名 / target | 源码 | 已有断言边界（源码可确认） | 不可外推 / 图像条件 |
|---|---|---|---|---|
| L | `native_language_zh_CN/en_US/ja_JP` → `GaussianSceneWorkbench --smoke-test-language` | `LanguageSmokeTest.cpp::runLanguageSmokeTest` + `runListInteractionSmokeTest` | 真实 Windows 主窗/GL；三语即时切换与owned运行进程、模型/遥测保护；10实例响应式循环仅5个Dialog类；另调用公共窗口/列表检查。 | 90/150% UI字体、680×460滚动/按钮与部分460↔1100重排；截图取决于`GSW_LANGUAGE_SCREENSHOT_DIR`，不是全Dialog×主题/DPR组合。 |
| I | `native_icon_ui_{zh_CN,en_US,ja_JP}` → `GaussianSceneWorkbench` | `IconUiSmokeTest.cpp` | 真实工具栏/编辑图标入口、语义资产/正常禁用选中等状态；两主题×90/100/150% UI比例、窄窗入口/溢出。 | 可保存图标接触表/toolbar/window PNG；没有因此证明实际分数DPR、所有Dialog字形或全部键盘Tab路径。 |
| W | `native_window_appearance_{3 locales}`、`native_window_minimize`；L内`runWindowUiSmokeTest` | `WindowUiSmokeTest.cpp` | 实际主窗/dock/文件窗/消息窗的系统按钮、最小化恢复、全屏/最大化和状态保留。 | 共用系统窗策略，不等于16个表单内容布局/每个modal安全默认都测过。 |
| F | `native_file_dialog_history`、`native_file_dialog_history_persistence`、`native_file_dialog_history_ranking`、`native_file_dialog_history_filter`、`native_file_dialog_history_guard`、`native_file_dialog_history_ui_{zh_CN,en_US,ja_JP}` → `GaussianSceneWorkbench` | `FileDialogHistorySmokeTest.cpp`、`FileDialogHistoryGuardSmokeTest.cpp` | 真实文件窗口历史、路径/过滤/取消、重开持久化、Unicode和无效条目；UI family覆盖语言/比例。 | 37个调用点不等于37个独特选择器实现；各父窗目的/保存覆盖和参数仍需登记。 |
| T | `native_task_actions_{zh_CN,en_US,ja_JP}` → `GaussianSceneWorkbench`；依赖`gsw_process_output_fixture` | `TaskActionsSmokeTest.cpp` | 真实TaskRecord/任务表/TaskDetails modal，失败启动、保留日志/精确导出、三语切换不改任务；日语150%字体×360宽检查按钮可见或溢出。 | 不是所有任务状态×两主题截图；本脚本无完整Dialog PNG/glyph-role接触表。 |
| H | `native_generation_history` → `GaussianSceneWorkbench`；`generation_history`为store级补充 | `GenerationHistorySmokeTest.cpp` | 公共history QAction、六种实验记录、选择/参数/续训可用性、取消信任、不覆盖参考对象和保存重开；实时三语。 | 主要状态/来源断言；无专用明暗/窄窗/DPR截图矩阵。 |
| R | `native_resource_budget_settings`、`native_resource_budget_ui`及mesh/shared/texture/page/cache/hud families → `GaussianSceneWorkbench` | `ResourceBudgetUiSmokeTest.cpp`、`ResourceBudgetSmokeTest.cpp` | 公共Action打开modeless ResourceBudgetDialog；三语×明暗、100%默认/150% 360×300、未知GPU probe说明、policy不因语言/主题变化。 | PNG需`GSW_LANGUAGE_SCREENSHOT_DIR`；分页/资源行为不是表单字体清晰度证明，不能故意耗尽GPU。 |
| G | `native_viewport_scheduling_light/dark` → `GaussianSceneWorkbench --smoke-test-viewport-scheduling` | `ViewportSchedulingSmokeTest.cpp`成功后调用`runViewportStatusControlsSmokeTest` | 真实idle/frame状态、Grid/Details公共动作；Grid三语标签几何/单位换算/取消/接受/保存重开，camera/model不变；Details即时关闭重开防DeferredDelete复用。 | 这是Grid/Details现有入口，不存在另一个独立`native_viewport_status_controls` CTest名；输出用`GSW_SMOKE_OUTPUT_DIR`。 |
| Q | `native_quality_comparison_ui`、`native_quality_measurement_ui` → `GaussianSceneWorkbench` | `QualityComparisonSmokeTest.cpp`、`QualityMeasurementSmokeTest.cpp` | 真实exposed Dialog、owned CPU comparer/producer、compare状态/精确数值/输入身份、三语保留report、取消/关闭清理；Measurement从父窗公共按钮打开/关闭/重开。 | Measurement有条件三语manifest PNG；Comparison未见专用全主题/窄窗截图。不是训练质量验收。 |
| S | `native_scale_constraints_ui` → `GaussianSceneWorkbench`；`native_model_scale_coordinates`/`scale_constraint_coordinate_report`低层补充 | `ScaleConstraintsSmokeTest.cpp` | 真实dataset与model来源的公共约束文档/solver/版本输出；三语、精度/不匹配/独立检查/取消，source不覆盖。 | 首个构造表单配置/语言检查与actual.show路径分开；无完整主题/窄窗/glyph截图矩阵。 |
| N | `native_initialization_editor_ui`、`native_initialization_dialog_ui` → `gsw_initialization_editor_ui_tests` / `gsw_initialization_dialog_ui_tests` | `InitializationEditorUiTests.cpp`、`InitializationDialogUiTests.cpp` | **硬设offscreen**：实际生产控件、完整数值/unknown provenance/request保存重开、CPU检查/建议/显式采用/新版本、三语、idle/queued/running关闭自有helper。 | 不提供Windows窗/OS标题栏/真实DPR截图。InitializationPreview的QImage证据不能冒充NativeViewport GL。 |
| B | `native_training_batch_contract_{3 locales}` → `gsw_training_batch_ui_tests`（硬设offscreen）；`native_training_batch_ui` → `GaussianSceneWorkbench` | `TrainingBatchSmokeTest.cpp`、`TrainingBatchUiTests.cpp` | 相同public request/preview/plan/start/resume/observe合同在两adapter执行；实际CPU计划展开、三语、精确request回读、不自动启动训练与取消。 | 显示窗不等于完整layout/glyph测量；offscreen adapter不是Windows视觉证据。 |
| BL | `native_training_batch_lifecycle` → `GaussianSceneWorkbench` | `TrainingBatchLifecycleSmokeTest.cpp` | 真实MainWindow训练队列生命周期、view/source保护；新增真实`initializationAction`打开/关闭Initializer、input identity/忙碌/来源变更/原任务marker保护。 | Initializer部分是入口/状态保护，部分版本信号由fixture注入，不能当真实初始化helper产物或完整窗口视觉验收。 |
| D | `native_runtime_decoupling_ui` → `gsw_runtime_decoupling_ui_tests`（硬设offscreen） | `RuntimeDecouplingUiTests.cpp` | 3DGS/2DGS运行策略精确回读、guard五字段/无效输入/三语；ExistingTraining真实owned CPU inventory/connect/pause/stop安全默认/断开/重连/超时重试/关闭helper。 | 没有真实Windows表单/glyph/DPR截图；不执行实际GPU训练或验证优化器续训数值。 |
| P | `native_training_preset_{3 locales}` → `gsw_training_preset_ui_tests`；`native_quality_plan_ui` → `gsw_quality_plan_ui_tests`（两者硬设offscreen）；`native_training_preflight` → `GaussianSceneWorkbench` | `TrainingPresetUiTests.cpp`、`QualityPlanTests.cpp`、`TrainingPreflightSmokeTest.cpp` | 真实TrainingDialog控件、预设/手工值/请求持久化、default-off quality/JSON/后端不兼容/三语、相机复用/重跑原因和CPU preflight。 | offscreen`dialog.show()`仍不是实际Windows图；Preflight主要检查控件/请求不代表截图。L提供真实主窗layout补充。 |
| M | `native_mesh_geometry_dialog_ui` → `gsw_mesh_geometry_dialog_ui_tests`（硬设offscreen）；`native_mesh_generation` / `native_light_mesh_generation` → `GaussianSceneWorkbench` | `MeshGeometryDialogUiTests.cpp`；`LanguageSmokeTest.cpp::runMeshGenerationSmokeTest` | 五mode稳定ID/三语/gs_depth 3DGS与2DGS真实CPU来源准入/坐标/阈值回读；MainWindow fake worker成果发布/失败/纹理/scene保护。 | 不实际训练或提取长任务；各mode的完整视觉状态和DPI仍要补。 |
| E | `native_spz_interchange` → `GaussianSceneWorkbench`；`model_export` → `gsw_model_export_tests`为非UI补充 | `SpzSmokeTest.cpp`、`ModelExportTests.cpp` | 公共导出/导入/项目保存、真实codec与类型/字段/源文件保护；L覆盖PLY/mesh/SPZ Dialog变体和响应式。 | codec/file测试不能证明Export所有format/disabled原因/进度窗像素；不要扩到真实研究数据。 |
| V | `native_scene_visibility_{3 locales}_{light,dark}`、`native_multi_scene_import`、`native_gaussian_click_visibility`、scene-coordinate/texture/paged-mesh families → `GaussianSceneWorkbench` | `SceneVisibilitySmokeTest.cpp`、`MultiSceneSmokeTest.cpp`、`CoordinateSceneSmokeTest.cpp`、`TextureTextSmokeTest.cpp`、`MeshRenderingSmokeTest.cpp` | 真实collection可见/选择/视图、模型/单位/变换保护，多种renderer/纹理/分页相关显示。 | 需逐case看真实renderer与fixture身份；不因family名字自动宣称所有5类模型/全部GPU资源路径已覆盖。 |
| A | `native_upstream_navigation_{3 locales}`、`native_blender_viewport_presentation_{3 locales}`、`native_edit_navigation_{3 locales}`、`native_scene_coordinates_{3 locales}`、observation families → `GaussianSceneWorkbench` | `UpstreamNavigationSmokeTest.cpp`、`native/tests/BlenderViewportVisualTests.cpp` / `BlenderViewportModelVisualTests.cpp`、`EditNavigationSmokeTest.cpp`、`ObservationNavigationSmokeTest.cpp` | 真实视口click/drag/zoom、透视/正交、grid/axis/场景公共状态；drawing/source navigation合同。 | 编辑导航是相机逻辑，不应据名字当模型Move/Rotate/Scale数值/undo全覆盖。 |
| X | `transform_gizmo_visual` → `gsw_transform_gizmo_visual_tests`；`navigation_gizmo` → `gsw_navigation_gizmo_tests`；`model_interaction`等低层合同 | `TransformGizmoVisualTests.cpp`、`NavigationGizmoTests.cpp` | 前者为QImage/GUILess绘制，四模式×两主题×投影、端帽/杆/深度/中心比例；后者layout/hit与导航几何。 | 不是实际NativeViewport四模式拖动/约束/数值/撤销的完整Windows操作证据。 |
| C | `app_theme`、`multi_item_lists`、`frame_rate_counter`、`native_viewport_locale_contract`以及store/report单位合同 | `AppThemeTests.cpp`、`MultiItemListTests.cpp`、`FrameRateCounterTests.cpp`、`NativeViewportLocaleTests.cpp` | 公共数据/样式/列表/帧计数/语言状态合同补充。 | contract或QImage通过不能替代真实窗口画面、每表单的字体回退和输入生命周期。 |
| PC | `native_processing_preview`、`native_training_isolation`、`native_processing_completion`、`native_training_resume`、`native_2dgs_training_resume` → `GaussianSceneWorkbench` | 对应Processing*/Training* smoke，部分入口在`LanguageSmokeTest.cpp` | 真实UI owned supervisor/持久owner/结果分层/暂停与source保持；Completion新块可关闭/重开并真实打开ExistingTrainingDialog后关窗。 | ExistingTraining的此路径是`UI_OPEN_ONLY`（出现后立即close），不取代D里的连接控制；UI暂停/恢复不是GPU完整状态数值一致性证明。 |
| DROP | `native_workspace_drop_{3 locales}` → `GaussianSceneWorkbench` | `WorkspaceDropSmokeTest.cpp` | 媒体/模型/项目/视频/文件夹的实际public drop与混合/无效/忙/未保存状态保护。 | 不把所有父窗文件选择目的或错误弹窗样式一并计通过。 |
| P0 | `native_ui_clarity_capture` / `native_ui_clarity_edges`；独立 `Native UI Clarity` workflow | `UiClaritySmokeTest.cpp`、`scripts/check_native_ui_clarity.py`、`P0_PROTOCOL.json` | 当前真实空/轻主窗导航的像素/DPR/FBO与可见widget QFontInfo/mixed-script glyph runs。 | 没有打开全部16Dialog；自绘overlay/gizmo/chart/InitializationPreview字体另有角色。本文不下载/判断其本轮远端结果。 |

## 4. 16 类 Dialog 的公开入口、窗口冻结与状态合同

下面每一项均要保留现有业务断言。P1/P2 视觉增量应取真实 Windows native window PNG，写入逻辑尺寸、物理尺寸、window / screen DPR、UI 字号、语言 / 主题、可见控件字体解析、稳定阶段和输入快照；不能只交 `dialog.grab()` 而没有实际窗口 / DPI 元数据。普通截图不替代每一复杂控件的值回读。

### DLG-01 — DatasetImportDialog

- **生产入口 / 现有调用**：文件 / 工作流媒体导入、拖放；公开构造器 + `request()`。L 真实窗覆盖普通与已知相机实例；`native_metashape_import_entry`、DROP / F 补入口与选择器；N 补嵌入 InitializationEditor 数据合同。
- **已覆盖维度**：三语即时刷新、响应式 / 字号、来源按钮重排、相机输入 / 参数回读与未提交值保护。现有已知相机截图是条件保存，不是全主题 / 实际 DPI 矩阵。
- **可冻结**：普通照片 / 视频 / 文件夹及已知相机两个主要布局；按现有 CPU fixture 设置表单后，冻结 `request()`（路径、名字、cameraInput、initialization、视频参数），切换语言 / 主题 / 字号后完全一致；取消不触发导入、不记成功历史、不改照片 / 原相机文件。
- **缺口**：未知 provenance、禁用原因 / 验证错误的真实窗口文字与所有 embedded 字体角色未全采；重排测试不能证明37个 picker 目的均正确。

### DLG-02 — ReconstructionDialog

- **生产入口 / 现有调用**：工作流 / 主工具栏重建；公开构造器 + `configuration()`。L 的响应式实例；P 的 preflight 对相机复用 / 重跑决策提供上下文行为，不是此窗全部视觉。
- **已覆盖维度**：三语 / UI 字号 / 680×460 滚动与按钮可达性，COLMAP 路径 / preset / matching / cameraModel / 分辨率 / 特征 / runtime / GPU 等配置。
- **可冻结**：现有默认、手工配置和复用 / 重跑摘要状态的真实窗；before/after `configuration()` 所有字段保持，接受只产生预期请求、取消不启动 COLMAP；source / 项目 / 相机不变。
- **缺口**：两主题、长路径 / 错误说明、窄窗及真实分数 DPR 的完整表单图集；不新增实际长重建压力任务。

### DLG-03 — TrainingDialog

- **生产入口 / 现有调用**：工作流 / 主工具栏训练；构造器、`configuration()`、`configurationDocument()`，初始化按钮通往 DLG-11。L 真实窗；P / D 深度配置为 offscreen；PC 提供 MainWindow 任务隔离 / 续训流程。
- **已覆盖维度**：3DGS / 2DGS、已知相机锁定、原图 / 缩放、preset / 手工值、default-off quality / 后端不兼容；三语 / UI 字号 / 响应式；条件真实表单截图。不能把 offscreen `show()` 记作真窗。
- **可冻结**：沿用 L 的 3DGS、2DGS、已知相机布局与 P 的 quality 不兼容 / 手动配置；`configurationDocument()` roundtrip 精确（含 runtime / quality / backend / camera / guard），语言 / 样式操作不重建模型 / worker、不把已有暂停任务继续训练。
- **缺口**：各分组展开 / disabled / error 的完整主题与实际 DPR 字体图；嵌套初始化与续训 guard 分别用 DLG-11 / 14 契约，不能由主训练窗成功外推。

### DLG-04 — MeshGenerationDialog

- **生产入口 / 现有调用**：工作流 / 工具栏生成几何；构造器 + `configuration()`。L 真实布局；M 的 `native_mesh_geometry_dialog_ui` 为 offscreen 五 mode 合同，`native_mesh_generation` / light 真实 MainWindow fake-worker 结果发布。
- **已覆盖维度**：五个稳定 mode ID、3DGS / 2DGS gs_depth 来源准入、坐标 / 阈值 / stride / alpha / memory 参数三语精确回读；部分真实截图；成功 / 失败不覆盖原场景。
- **可冻结**：每个**不同布局** mode 用同一 fixture 打开并捕获（纯文案相同布局可复用），`configuration()` 前后完全一致；接受只发预期请求；取消不生成、不改源 / checkpoint。
- **缺口**：五 mode 的禁用解释 / 错误 / 高字号真实明暗截图；已有 CPU / fake worker 不等于真实网格质量或长提取性能证明。

### DLG-05 — ModelExportDialog

- **生产入口 / 现有调用**：文件 / 场景 / 主工具栏导出；构造器 + `options()`。L 有 mesh / Gaussian PLY / SPZ 三实例、响应式和条件明暗 Export PNG；E 真 codec / 原字段回读；`model_export` 是非 UI。
- **已覆盖维度**：三语、格式 / 坐标框架 / 精度 / 属性 / Gaussian 变换与 SH 保真请求、SPZ 实际导入导出、源文件保护。
- **可冻结**：沿用三种格式布局，锁定 `options()`；切换主题 / 字体不改 destination / transform / SH / 精度；取消不建输出、不改源；接受输出由现有 file tests 再回读。
- **缺口**：所有格式禁用理由、unknown units / strict transform 错误的真实窗；PROG-02 另登记，不能以 codec pass 替代进度窗视觉。

### DLG-06 — TaskDetailsDialog

- **生产入口 / 现有调用**：任务双击 / 详情 QAction / 右键；公开构造器接 `QSharedPointer<TaskRecord>`，没有配置 getter。T 用受控失败启动 / CPU supervisor 打开实际 modal 并操作命名 action / 控件。
- **已覆盖维度**：三语即时切换、状态 / 原始日志保留、精确字节导出 / 输出目录动作；日语 150% UI 字号、360×560 下工具动作可见或溢出且关闭可达。
- **可冻结**：失败和运行 / 结束两个原有 record fixture 的真实窗；before/after `TaskRecord::logText()`、状态、task id / process ownership 不变；界面摘要可译，外部 raw log / 路径不可译；复制 / 导出与保留日志一致。
- **缺口**：完整 Dialog PNG / 溢出菜单 / glyph-role 接触表及明暗组合；不能为了截图启动真实训练或杀用户进程。

### DLG-07 — GenerationHistoryDialog

- **生产入口 / 现有调用**：实验历史 QAction；构造器、`selectedRecord()`、`selectedAction()`、`setResumeAllowed()`。H 真菜单入口 / 六记录 fixture。
- **已覆盖维度**：三语、记录 / 参数 / source variant / 可续训状态、取消信任不启动、保存重开和参考对象不被替换。
- **可冻结**：既有可查看 / 可续训 / 不可续训代表记录；切换 locale / theme / font 后 selected id、原 JSON 参数 / report、记录数不变；关闭不隐式启动；View / Resume 只发对应显式动作。
- **缺口**：两主题、窄窗、禁用与空表三语 native PNG / 字体角色；不用新建另一套 history store 测试重复已有合同。

### DLG-08 — QualityComparisonDialog

- **生产入口 / 现有调用**：工作流质量对照；构造器、`contract()`、`comparisonReport()`、`isRunning()`；Q 的实际 exposed Dialog + owned CPU comparer。
- **已覆盖维度**：unverified / pass / regressed / not_comparable、输入身份 / mask、JSON 重复 / 过大错误、三语 report 保留、取消 / 关闭自有 helper、覆写报告拒绝。
- **可冻结**：沿用既有 report 状态，不重算长训练；`contract()` 与原 report JSON / 路径 before/after 一致；只显式 Adopt measurement 才换 baseline / candidate；Cancel / close 只收束该 CPU helper。
- **缺口**：专用完整主题 / 高字 / 窄窗 PNG、validation / report 状态字体角色。对照 fixture 不构成训练“质量提升”结论。

### DLG-09 — QualityMeasurementDialog

- **生产入口 / 现有调用**：QualityComparison 的公共“主动测量图像”按钮；构造器、`setManifestPath()` / `setPairedFolders()`、`manifestPath()`、`reportPath()`、`measurementReport()`；Q 有实际打开 / 关闭 / 重开路径。
- **已覆盖维度**：owned RGB / mask CPU producer、精确指标、三语已生成 report 不变、显式 baseline / candidate adoption、immutable 持久报告和取消；三语 manifest PNG 依赖 `GSW_SMOKE_OUTPUT_DIR`。
- **可冻结**：空 / 已配对与已完成报告两个既有状态；getter 三项 + source hashes 一致；关闭清理自有 helper，parent contract 只有 `reportChosen` 明确选择时才变。
- **缺口**：两主题 / 窄窗 / 150% 文本和所有字体角色；PNG 文件存在还须本轮 artifact 与 native 窗 DPR 元数据。

### DLG-10 — ScaleConstraintsDialog

- **生产入口 / 现有调用**：相机数据集 / 当前模型两条定尺度入口；公开 `constraintDocument()`、`solutionReport()`、`sourceDescriptor()`、`resultDatasetInfo()` / `versionCreated`。S 有实际 .show + owned CPU solver。
- **已覆盖维度**：dataset/model、三语、数值精度 / 单位 / 独立检查 / 不匹配拒绝 / 取消、版本输出和 source hashes；最前段构造配置检查不是独立 native 截图。
- **可冻结**：两来源布局与原有成功 / 检查失败报告；constraint / source / solution JSON 原样，切语言不重新求解；显式 Create version 产生新路径，原相机 / 模型 / crop 不写回。
- **缺口**：完整明暗 / 长字段 / 高字窄窗 PNG / glyph；保留非均匀方向 SH 拒绝，不为 UI 改成近似变换。

### DLG-11 — InitializationDialog

- **生产入口 / 现有调用**：工作流 `initializationAction` 和 TrainingDialog 的初始化按钮；公开 `requestDocument()` / `setRequestDocument()`、`report()`、`isRunning()`、load/save、inspection / suggestion / apply / version / cancel slots。
- **已覆盖维度**：BL `TrainingBatchLifecycleSmokeTest.cpp:330–435` **真实 QAction 打开 / 关闭**，绑定 current input identity、busy / changed source / crop / adoption 保持；部分 `versionCreated` 为 fixture 注入，不是实际 helper 产物。N 的 CPU inspect / suggest / 显式采用 / 新版本 / request 重开 / 三语 / idle+queued+running 关闭为 **offscreen**。
- **可冻结**：把 N 的公开 request + CPU fixture 接同一 real-window adapter；至少 idle / inspection report / suggestion pending capture，before/after request 与 report 精确，建议不自动采用；Cancel / close 仅停 owned helper，新版本不修改源照片 / 相机 / 原初始化。分别记录工作流和训练子窗来源身份。
- **缺口**：深入控件 / preview 的真实 Windows 两主题、窄窗 / 字体 / DPR；TrainingDialog 子入口的真实捕获尚未在 L 看见；不能把 BL 注入信号当真实生成验收。

### DLG-12 — TrainingBatchDialog

- **生产入口 / 现有调用**：批量训练 QAction；公开 `requestDocument()` / `previewReport()` / `planPath()`，preview / create / start / resume / observe / cancel slots，`runRequested` / `observeRequested` 信号。
- **已覆盖维度**：B 同一个 `TrainingBatchSmokeTest` 被真实 app 与 hard-offscreen standalone 两种 adapter 复用；两后端 / 两 seed 的 CPU 展开、三语 request 精确、持久计划 / 历史重开 / 无自动训练；BL 再保护 MainWindow queue 生命周期。
- **可冻结**：未展开 / 预览完成 / 计划已保存三个已有状态；request/preview JSON 与 planPath 原样，语言 / 样式不增 revision、不启动 worker；关闭收自有 planning helper，Start / Resume / Observe 只显式发预期信号。
- **缺口**：真实窗全主题 / 高字窄窗 / glyph 图集；无需给同一 smoke 再复制16个 target；不能把 Observe request 当训练已恢复。

### DLG-13 — ExistingTrainingDialog

- **生产入口 / 现有调用**：现有训练 QAction、批量观察队列；公开 `currentReport()` / `isConnected()` / `isBusy()`、refresh / connect / disconnect / pause / stop / connectBatch。
- **已覆盖维度**：D offscreen 真实 owned CPU inventory / connect / 暂停 / stop 安全默认 No / 断开 / 重连 / timeout+retry / 关闭 helper、三语 identity 保持。PC `ProcessingCompletionSmokeTest.cpp:616–667` **实际显示此窗后立即 close，属于 UI_OPEN_ONLY**，不是没有入口。
- **可冻结**：复用 `OwnedTrainingConnectionFixture.py` 接真实窗口，冻结 disconnected / connected / timeout 三个已有状态；report job id / backend / source identity、process handle / ownership 不因主题 / 语言变化；Disconnect / close 不杀 trainer，仅停短 CPU helper；Pause / Stop 只命令测试自有进程且 Stop 仍默认 No。
- **缺口**：实际观察正文 / 监控曲线 / 控制按钮的完整三语两主题窄窗 PNG 与字形；不进行真实 GPU 训练 / 用户暂停任务续训来验证 UI。

### DLG-14 — RuntimeGuardDialog

- **生产入口 / 现有调用**：完整续训信任确认 → 显存保护设置；构造器 + `configuration()`。D `RuntimeDecouplingUiTests.cpp:192–212` 的公开构造 / 回读在 offscreen；H / PC 提供周边信任取消，不等于 guard 正文已操作。
- **已覆盖维度**：五 guard 字段精确 roundtrip（含 `.901234567891`）、手动编辑、三语刷新不改值；fraction 1.2 无效使 OK disabled，并返回原文档，不静默 clamp。
- **可冻结**：用现成暂停 / 信任 fixture 从真实 nested button 打开，原值 / 手改值 / 1.2 无效状态三张可共享截图；`configuration()` 精确且 Cancel 不把值写回父请求、不触发 Resume；Accept 只回传改过的五字段。
- **缺口**：未找到真实 Windows 嵌套 guard 正文截图 / 控件输入 / 返回父确认的验收，是最小新增 public seam 的优先项；绝不自动续训用户任务。

### DLG-15 — ResourceBudgetDialog

- **生产入口 / 现有调用**：`resourceBudgetAction` → modeless dialog。无自身 configuration getter；公开 `NativeViewport::resourceBudgetPolicy()` / `resourceBudgetStatus()` 可回读，命名 controls 做输入。R 实际 QAction / 冷启动无 GPU probe 实例。
- **已覆盖维度**：三语 × 两主题、默认与150% 360×300、body 无横滚 / Close 可达、manual值 / unknown GPU 说明，换语言 / 主题不改 policy；条件 PNG 在源码存在。
- **可冻结**：延用既有自动 / manual / naturally-unknown GPU fixture；policy 与 resource ledger reservation 保护，live 应用设置只产生预期差值，关闭不删除场景对象 / 不重复计费。
- **缺口**：当前 focused workflow selection 不含该 test，full step 也未设语言截图 env；需补证据收集，不应把本轮缺 PNG 解释成必须重写表单测试；不故意耗尽资源。

### DLG-16 — ViewportGridDialog

- **生产入口 / 现有调用**：`viewportGridSettingsAction`、Details 的网格按钮；构造器 + `settings()`。G 经 scheduling smoke 实际触发公共动作 / modal 控件。
- **已覆盖维度**：三语标签几何、mm / metre / unknown 单位、未提交模式 / 数值 / 单位 / subdiv / density保留、取消不提交、接受 / 保存重开，camera / model / recovery JSON 不变；条件 grid-dialog PNG。
- **可冻结**：沿用现有 metric / unknown source fixture；before/after `settings()` 与 viewport grid 公共状态精确，取消和语言切换不提交。Details close→reopen 应是新活实例（DeferredDelete 未处理时也不复用死窗）。
- **缺口**：actual fractional DPR / 全字体角色、网格色彩 customBG 未有可达设置，不能凭固定主题 PNG 标成 custom background pass。

## 5. 66 个表面组逐一映射

“测试族”指第3节可复用源码，不代表本轮已运行。所有行的执行 / 图像审阅状态：**NOT_RUN**。Dialog 详细 freeze 合同见第4节；源码 owner 与147调用点逐项记录沿用母表，避免另建会漂移的重复调用索引。

| 表面 ID | 入口 / 表面 | 矩阵 ID | 可复用族 | 现有边界与剩余验收 |
|---|---|---|---|---|
| INF-01 | 应用字体、密度与控件状态 | FONT-01, ICON-01, SHELL-01, FORM-01, LOCALE-01, A11Y-01 | L,I,C,P0 | 现有共享字体/比例/主题断言可复用；P0只采当前主窗可见控件。16类表单和自绘文字角色尚未被统一采集。 |
| INF-02 | 系统标题栏与窗口生命周期 | SHELL-01, FORM-01, LIFE-01, LOCALE-01 | W,L | 共用系统按钮/浮动dock策略已有真实窗断言；每一表单的close/reject/helper释放仍按下表单独登记。 |
| INF-03 | 语义SVG图标与状态 | ICON-01, DPI-01, A11Y-01, PORT-01, PERF-02 | I,X,A | 共用资产接触表、正常/禁用/选中及窄窗断言可复用；不能外推为实际分数DPR或每个图标全部交互通过。 |
| INF-04 | 多项列表快捷键/工具条/批量移除 | TREE-01, FORM-01, STATE-01, A11Y-01, LIFE-01 | L,C,T,H,Q,S,B | 公共列表键盘/多选/移除原语及若干父窗有断言；新样式需验证每个实际父窗的选择与取消保护，不必重测每个图标。 |
| INF-05 | 文件选择器与常用位置/历史 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | F,W,L | 共用历史/过滤/无效路径/取消/重开和系统按钮已有覆盖；37个目的入口的过滤器、初始目录、覆盖默认值并未等价全跑。 |
| INF-06 | 右键菜单、tooltip及键盘焦点 | SHELL-01, A11Y-01, LOCALE-01 | I,L,T,C,A | 菜单/动作及部分tooltip被查验；未见全部可达右键菜单的逐入口截图、全键盘焦点/读屏合同。 |
| SH-01 | 主菜单及所有子菜单 | SHELL-01, A11Y-01, LOCALE-01, STATE-01 | L,I,W,C | 语言/主题/显示菜单与溢出断言可复用；未知/禁用的工作流菜单状态须按实际fixture补截，不宣称所有子菜单均视觉验收。 |
| SH-02 | 第一行主工具栏 | SHELL-01, ICON-01, A11Y-01, LOCALE-01 | I,L,T,P,M,PC | 真实顶层动作、busy/暂停/继续/停止状态由对应流程断言；公共图标矩阵可复用，业务动作状态不能只查SVG存在。 |
| SH-03 | 第二行渲染/预览工具栏 | SHELL-01, ICON-01, MODEL-02, STATE-01 | I,L,V,PC | 渲染模式/预览隔离状态及语义图标可复用；所有后端×预览暂停/恢复的全图像组合尚未形成清晰度矩阵。 |
| SH-04 | 第二行选择工具栏 | SHELL-01, ICON-01, A11Y-01, GIZMO-02, LOCALE-01 | I,L,A,V | 编辑/观察入口和相机导航已有断言；不把相机edit-navigation误记为模型变换正确性。笔刷/仅可见键盘与焦点尚缺完整真实窗回归。 |
| SH-05 | 第二行编辑操作工具栏 | SHELL-01, ICON-01, GIZMO-02, STATE-01 | I,L,V,X | 选中、删除/撤销等部分流程和纯几何合同可复用；样式迁移后仍需真实视口数值/撤销/取消的窄链路。 |
| SH-06 | Dock标题、浮动/关闭按钮和分隔拖动区 | SHELL-01, FONT-01, ICON-01, LIFE-01 | W,L,I | 实际dock/恢复布局/浮动按钮、标题语言与比例已有覆盖；窄窗/浮动时字体角色与所有拖动分隔器还未逐项截图。 |
| SH-07 | 工程树及眼睛可见性列 | TREE-01, MODEL-01, A11Y-01, LOCALE-01 | V,L,C | 可见性三语×两主题、集合选择与场景对象保护有明确family；树眼睛委托真实DPR/焦点图像需保留，不能仅用数据合同代替。 |
| SH-08 | 工程树右键菜单 | TREE-01, MODEL-01, A11Y-01 | V,L,C | 部分右键动作的行为随集合/历史列表执行；未发现每类树节点右键菜单各状态的系统化截图。 |
| SH-09 | 属性面板：工程/数据集 | SHELL-01, FONT-01, FORM-01, LOCALE-01 | L,S,N,BL | 项目/数据集文字与source identity随语言/初始化/尺度流程保护；路径极长、未知单位等属性版式的完整主题/窄窗证据尚缺。 |
| SH-10 | 属性面板：场景几何/变换/单位 | SHELL-01, FORM-01, GRID-01, MODEL-01, LOCALE-01 | L,V,A,S | 共享坐标/单位/模型变换、选择稳定性有行为断言；属性面板实际编辑控件的全焦点、无效输入与字体角色图集尚缺。 |
| SH-11 | 任务表、工具条及右键菜单 | SHELL-01, STATE-01, A11Y-01, LIFE-01 | T,L,PC | 实际任务表/详情/精确日志导出/失败启动和持久任务状态覆盖强；不能据此宣称每种状态×明暗截图均存在。 |
| SH-12 | 共享训练监控及曲线 | SHELL-01, FONT-01, STATE-01, LOCALE-01, LIFE-01, FPS-01 | L,D,PC,C | 主窗监控窄/宽/滚动和三语，observer CPU连接生命周期可复用；自绘曲线文字不在普通QWidget glyph列表中。 |
| SH-13 | 日志页及跟随/导出工具栏 | SHELL-01, FONT-01, STATE-01, LOCALE-01 | T,L,C | 保留原始日志、跟随与精确字节导出/任务语言切换保护已有；日志字号/等宽回退/截断提示实际像素仍需专门角色捕获。 |
| SH-14 | 系统状态栏与临时消息 | SHELL-01, STATE-01, FPS-01, LOCALE-01 | L,I,C,G | label底色/主题及状态文本、帧计数和idle有合同；临时消息被刷新覆盖等状态须按稳定截图时点测试。 |
| SH-15 | 拖放反馈与导入摘要 | OVERLAY-01, STATE-01, LOCALE-01, MODEL-01 | DROP,L | 实际媒体/模型/项目/视频/目录drop、混合无效/busy/未保存状态有行为断言；拖拽悬停中每个overlay像素未全登记。 |
| SH-16 | 界面设置/布局恢复入口 | SHELL-01, DPI-01, LOCALE-01, LIFE-01 | W,L,C | 自动/手动UI比例、恢复布局、最大化/全屏/最小化有现有入口；不代表所有显示器实际DPR切换已测。 |
| VP-01 | 真实模型渲染区域 | MODEL-02, PERF-01, PERF-02, THEME-01 | V,R,PC,A | 点云/网格/纹理/高斯与分页、资源预算和处理预览有独立行为测试；不得把小样本显示升级成训练质量或压力性能结论。 |
| VP-02 | 导航朝向轴圆盘/圆环/标签 | NAV-01, NAV-02, NAV-03, DPI-01, THEME-01, FONT-01, PERF-01 | A,X,P0 | 真实导航操作、Blender绘制证据、layout/hit数学和P0像素管线可复用；P0不是本文件判定结果，字体/actual DPR须读本轮artifact。 |
| VP-03 | 导航小按钮与投影提示 | NAV-02, NAV-03, ICON-01, A11Y-01 | A,I,X | 实际导航按钮/投影切换和布局命中可复用；焦点/tooltip及清晰度矩阵仍需实际PNG而非图标资产即可。 |
| VP-04 | 模型变换器：全部四种模式 | GIZMO-01, GIZMO-02, MODEL-01, DPI-01, PERF-01 | X,V,A | 四模式绘制端帽/杆/深度等QImage合同可复用；真正NativeViewport的四模式拖动/约束/数值/取消/undo并无完整矩阵。 |
| VP-05 | 视口内模型编辑工具条 | GIZMO-01, ICON-01, A11Y-01, LOCALE-01 | I,X,L | 图标/模式文字和绘制入口可复用；自绘编辑条状态与全局/局部切换的实际视口截图/焦点还需专门小链路。 |
| VP-06 | 观察轨迹球 | NAV-02, OVERLAY-01, THEME-01 | A,L | 观察轨迹球相机交互及部分真实GL图像可复用；不等价于模型旋转器业务合同。 |
| VP-07 | 模型选择轮廓/包围框/轴线 | OVERLAY-01, MODEL-01, GIZMO-02 | V,A,X | 集合选择/隐藏、边界反馈和变换数据有断言；所有后端的高亮轮廓像素对照尚非现有完整矩阵。 |
| VP-08 | 屏幕选择手势与笔刷半径 | OVERLAY-01, GIZMO-02, DPI-01 | V,I | 选择与工具入口有片段覆盖；真实框选/套索/笔刷边界、可见点切换、半径各DPR不是现有图标测试保证。 |
| VP-09 | 相机轨迹/相机线框 | OVERLAY-01, MODEL-02, GRID-01 | L,V,A,C | 相机线框/相机来源/显示映射和trajectory数据合同可复用；原始图像尺寸/相机glyph等主题截图仍需按case辨认。 |
| VP-10 | 网格/基准轴/标尺 | GRID-01, THEME-01, MODEL-01, PERF-01 | A,G,C,S | 真实基准网格/场景轴/单位及GridDialog接受取消重开有断言；customBG仍是缺少可达设置的baseline gap。 |
| VP-11 | 常驻场景标题与计数/模式字幕 | OVERLAY-01, FONT-01, LOCALE-01, STATE-01 | L,V,PC | 三语计数/模式及纹理文字像素专用测试可复用；caption compact/strong等自绘字体不由主窗QWidget采集自动覆盖。 |
| VP-12 | 处理/训练预览阶段面板 | OVERLAY-01, STATE-01, FPS-01, LOCALE-01 | L,PC,C | 处理/训练结果过渡、暂停预览不暂停任务和source隔离已有流程；阶段HUD自绘文字各字体角色/实时变化截图需冻结时点。 |
| VP-13 | 底部简洁帧状态与详情按钮 | FPS-01, SHELL-01, LOCALE-01, PERF-02 | G,C,R,L | Details/Grid公共入口、idle帧策略/真实计数和资源HUD已覆盖；勿把renderer耗时当显示FPS。 |
| VP-14 | 空/加载/失败/全部隐藏状态 | MODEL-01, MODEL-02, STATE-01, FPS-01 | V,PC,L,G | 空/全隐藏/来源切换与处理失败有fixture；各后端失败画面的完整theme/glyph截图还未全列。 |
| DLG-01 | DatasetImportDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,F,DROP,N | 详见16Dialog合同；共享布局循环已覆盖，初始化编辑器深度行为在offscreen。 |
| DLG-02 | ReconstructionDialog | FORM-01, STATE-01, LOCALE-01 | L,P | 详见16Dialog合同；响应式/配置值已有，完整明暗错误状态与glyph未全捕获。 |
| DLG-03 | TrainingDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,P,D,PC | 详见16Dialog合同；多backend配置强，嵌套initializer/guard需区分真实窗口与offscreen。 |
| DLG-04 | MeshGenerationDialog | FORM-01, STATE-01, LOCALE-01 | L,M | 详见16Dialog合同；五mode数据合同不等于五mode视觉完备。 |
| DLG-05 | ModelExportDialog | FORM-01, STATE-01, LOCALE-01, MODEL-02 | L,E | 详见16Dialog合同；格式/字段文件保真与弹窗视觉必须分别验收。 |
| DLG-06 | TaskDetailsDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | T | 详见16Dialog合同；日语150%窄窗断言现成，完整PNG/glyph还未归档。 |
| DLG-07 | GenerationHistoryDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | H | 详见16Dialog合同；三语/选择/保存重开已有，样式可借同一fixture加截图。 |
| DLG-08 | QualityComparisonDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | Q | 详见16Dialog合同；CPU对照报告/取消已有，无完整明暗窄窗图集。 |
| DLG-09 | QualityMeasurementDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | Q | 详见16Dialog合同；真实子窗入口与可回读报告、条件截图已有。 |
| DLG-10 | ScaleConstraintsDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01, GRID-01 | S | 详见16Dialog合同；dataset/model双模式及源保护已有。 |
| DLG-11 | InitializationDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01, MODEL-02 | N,BL,P | 详见16Dialog合同；真实公共Action开关窗存在，但深入CPU生命周期在offscreen。 |
| DLG-12 | TrainingBatchDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | B,BL | 详见16Dialog合同；同一smoke已可双adapter复用，不能只跑standalone就声称Windows视觉。 |
| DLG-13 | ExistingTrainingDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | D,PC,B | 详见16Dialog合同；Completion仅UI_OPEN_ONLY，不能当真实窗连接控制与视觉已验。 |
| DLG-14 | RuntimeGuardDialog | FORM-01, STATE-01, LOCALE-01, LIFE-01 | D,H,PC | 详见16Dialog合同；五字段/无效输入/三语在offscreen，未找到Windows嵌套guard编辑截图链路。 |
| DLG-15 | ResourceBudgetDialog | FORM-01, STATE-01, LOCALE-01, PERF-02 | R | 详见16Dialog合同；三语×明暗×150%窄窗已有，需确保截图env与上传选择真正覆盖此test。 |
| DLG-16 | ViewportGridDialog | FORM-01, STATE-01, LOCALE-01, GRID-01 | G | 详见16Dialog合同；挂在viewport_scheduling两个theme test内，不新增虚构CTest名。 |
| INL-01 | 视口详情 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | G | Details真实Action、快速close/reopen与新实例检查已有；主题/语言截图须读对应输出。 |
| INL-02 | 恢复中心 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,C | 实际恢复中心列表/三语/多选/取消与确认移除可复用；恢复每种失败/进度图像未全覆盖。 |
| INL-03 | 版本历史 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,C | 版本列表实际modal三语/选中保留已有；具体恢复/源修改错误状态各图像未全覆盖。 |
| INL-04 | 外部备份历史 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,C | 外部备份列表/选择/三语及store合同可复用；全恢复过程视觉与异步取消必须另标未证。 |
| PROG-01 | 恢复外部备份 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,C | 外部备份store校验/操作合同存在；未找到此progress四阶段稳定截图的完整真实窗验收。 |
| PROG-02 | 导出模型 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | E,L | 真实导出/取消与文件保真流程可复用；瞬时progress是否实际出现不能仅由产物成功推断。 |
| PROG-03 | 导入场景 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | V,DROP,R | 实际导入/分页完整显示与取消流程可复用；不能保证每个模型后端均显示同一progress阶段截图。 |
| PROG-04 | 批量移除记录 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | L,C,T | 通用记录删除的取消/确认与若干父表格行为有断言；延迟/批量/部分失败的progress视觉还要独立冻结。 |
| MSG-01 | 所有信息/警告/错误/确认/关于 | FORM-01, STATE-01, LOCALE-01, A11Y-01, LIFE-01 | W,L,T,H,Q,S,D,B,PC,DROP,F | 共用系统窗/翻译与代表性错误、确认已执行；83调用点不是83个独立实现，也不是83条业务已全跑。 |
| MSG-02 | 未保存工程/场景编辑与退出确认 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | W,L,PC,DROP | 实际close/new/source-change防未保存丢失有多个fixture；不得在本机用户未保存工程上验收。 |
| MSG-03 | 完整续训信任确认与嵌套资源设置 | FORM-01, STATE-01, LOCALE-01, LIFE-01 | H,D,PC | 现有取消信任/完整续训声明和guard值合同可复用；缺真实Windows嵌套guard body截图/回读。 |
| MSG-04 | 固定启动器错误提示 | FORM-01, STATE-01, LOCALE-01, PKG-01, LIFE-01 | C | 属于launcher错误UI；有managed installation解析/文件验收基础，但未找到真实Win32 MessageBox三语截图。此轮不重复安装调查。 |
| EMB-01 | 初始化声明编辑器 | FORM-01, STATE-01, LOCALE-01 | N,L,BL | 生产编辑器unknown provenance/精确数值/序列化/语言有offscreen深度断言，L露出部分表单；不可把parent打开当所有控件视觉已验。 |
| EMB-02 | 初始化几何预览 | FORM-01, FONT-01, MODEL-02, OVERLAY-01 | N,BL | CPU初始化预览数据/绘制由专用测试辅助；未形成实际Windows内嵌预览的三语/主题/DPR字体角色图集。 |

## 6. 内联窗、进度、消息与文件窗的去重原则

- **4 个内联 QDialog**：Details 已有 G 的完整语言 / 状态保护与快速关闭重开；Recovery / snapshot / external backup 列表在 L::runListInteractionSmokeTest 中实际打开，检查多选、三语、选择保留 / 取消与确认移除，Recovery 有条件 PNG。不能从 store unit test 推断备份恢复进度窗视觉。
- **4 个 progress 入口**：逐一按 PROG-01..04 留档。成功产物不能证明“进度窗曾实际显示”。最小补充是沿现有 fixture 为可见 / 取消 / 完成（必要时失败）设置稳定捕获点与输入 / 输出保护，不为图像人为拉长真实训练。
- **83 个 QMessageBox**：共享 QStyle / 翻译 / 系统窗口策略只需按实现测试一次，但业务的父对象、按钮集合、安全默认、Cancel / No 不改变状态、长文本和嵌套 modal 要分组。W / L / T / H / Q / S / D / B / PC / DROP / F 有代表性例子，不是83条路径全验。任何未被具体case触达的调用点保持 NOT_RUN。
- **37 个 QFileDialog**：F 复用共用历史 / 常用路径实现；每个业务目的仍应列过滤器、初始目录 / 文件名、single / multi / directory、保存覆盖确认与取消副作用。已有window appearance / minimize不等于37个业务参数全通过。
- **Win32 launcher error**：母表 MSG-04 单独保留；不混为 QMessageBox，不为UI task重新做安装 / 交付调查。必要截图仅在远端破坏测试自有 launch receipt。

## 7. P1/P2 最少新增远端 seam 建议

以下是缺口建议，未在此文改生产 / 测试 / workflow：

1. **一个共用的有界 form capture adapter**。只用于 smoke 模式；接受当前已通过公开入口打开的 dialog / visible window 与“公开 getter 快照”回调。捕获一个已曝光且稳定的原生窗口，记录源码 SHA / case id / native window id / DPR / logical+physical size / theme / locale / UI font / visible widget glyph family 与 0-glyph；保存 before+after public state。设置每窗 / 每case / 总图量与时间 / 日志上限，关闭自有窗和 helper；不自动保存用户工程，不改业务数值，不靠 foreground抢用户窗口。本轮已有P0捕获管线可复用元数据/目录绑定，但不能无界遍历/抓取所有窗口。
2. **优先把现成真实窗测试接上 capture，而非复制测试**：L 的5类10实例；T 的窄窗 details；H；Q 的两个Dialog；S；B；R；G。为同一个Dialog的纯文本 / 图标变化复用该实现的语义/语言/字号/主题接触表，受影响布局状态再拍补图，不对每个icon重复整套语言长测。
3. **只补三处当前 real-window body 弱覆盖**：DLG-11 用 N 的 owned CPU fixture 接 BL 的真实入口；DLG-13 用 D 的 OwnedTrainingConnectionFixture 接 PC 的真实观察窗；DLG-14 用现有信任确认 public nested button 打开 guard并回读五字段。保留 N / D 轻量 offscreen合同时，不假设外部env可覆盖其硬编码平台。训练dialog与workflow两入口至少核对相同 source identity；不另造新生产请求API。
4. **自绘文字按角色采集**：当前 `UiClaritySmokeTest.cpp:186+` 只对当前 `viewport.window()->findChildren<QWidget*>()` 中 `isVisible()` 的控件取 QFontInfo + 固定混排 probe 的 glyph runs；没有打开16类Dialog，也不自动覆盖 NativeViewport compact/strong caption、transform rail、chart / 初始化预览等 QPainter 字体。Navigation adapter 的单独轴标签元数据不能代替其余角色。新增角色读数应来自实际布局使用的 font / text，而非另猜一套字体，不为采集改变 UI 字体或 numeric state。
5. **真实编辑 gizmo 小链路**：四模式软件绘制已有 X；为改动命中/几何的片段，仅补一个 actual NativeViewport fixture，覆盖 Move / Rotate / Scale / Combined 的原有实际手柄、约束、numeric commit / Cancel / Undo 与模型矩阵回读；保留相机 / scene source不变。A 的 edit-navigation 是相机，不拿来冒充这项。优先小的可重复输入，不进行复杂用户模型训练。
6. **收集接线先补后判定**：把被选 tests 的 `GSW_SMOKE_OUTPUT_DIR` / `GSW_LANGUAGE_SCREENSHOT_DIR` 指向单一新 owned证据根，`if: always()` 保留失败日志与已生成图；为输出建 case manifest 绑定 testcase +源码 SHA + 图 / JSONhash。没有照片 / shader / 表单像素证据，仍标 NOT_RUN，不以“测试程序返回0”填满视觉矩阵。

## 8. 建议按实现切口选择远端包，避免重复作业

| 改动切口 | 最小复用测试组合 | 仍需保留的增量 |
|---|---|---|
| 共用字体 / QSS / SVG / toolbar tokens | `native_icon_ui_{3 locales}`、`native_window_appearance_{3 locales}`、L 的三个语言 test；加 catalog validator | 受影响16Dialog捕获分批复用；真实DPR与字体角色 / disabled contrast，原始配置快照不变 |
| 文件窗 / 窗口按钮 / history | F 的5行为tests + UI三语、`native_window_minimize` / W | 被改业务目的的过滤 / 覆盖 / 取消状态；不重跑长训练 |
| 单个表单布局 | 第4节该Dialog的真实窗family + 对应OFFSCREEN合同（若有）+三语catalog | 该布局默认 / 窄窗高字、明暗、错误或禁用的少量稳定native PNG与getter前后对照 |
| 多项列表 / task / history / observer | `multi_item_lists` + T / H / D / PC受影响子集 | selected id / raw log / owned process /安全默认保护；observer真窗body补链路 |
| 视口导航 / grid / HUD | A受影响三语、G两主题、`native_texture_text_{3 locales}_{light,dark}`、帧计数 / locale合同 | 实际像素与FBO / native窗一致、camera / model矩阵不变、customBG缺能力显式保留 |
| 编辑gizmo / rail | X + I +实际视口受影响小链路；camera A只作回归 | 四模式手柄输入 /约束 /取消 /undo；不得以静态QImage替代交互 |
| 完整合入 / 交付 | 现有完整 Native Windows suite（由主任务安排） | 当前提交日志、截图/状态manifest、包与source绑定；不要把本文件当发布许可 |

## 9. 未完成判定清单

- 66/66 个表面已与可复用测试或明确缺口相连；16/16 Dialog有公开入口、已有 adapter边界、freeze / getter / state保护契约；147 调用点索引由母表保留，不伪造147条运行记录。
- **本轮执行与截图判定：NOT_RUN**。P0真实像素baseline由主任务处理；此文不检查其远端结果、不改变AA生产实现。
- FONT-01 / FORM-01 / LOCALE-01 不能仅因主窗glyph采集或10实例循环填满；A11Y-01仍缺完整键盘 / focus / 读屏证据。
- THEME-01的customBG：当前仅 `ui/theme` 的auto/light/dark可达，无已发现的用户自定义背景色 / alpha setter；保持原有主题背景，不擅自替换成Blender灰、不声称customBG已通过。
- BASE-01 / AUDIT-01 / PKG-01 / DELIVER-01 / CLOUD-01等跨表面证据由主任务台账与实际run / package receipt负责；没有在此重复CI / 安装 / 下载调查。

