# 2026-10-29 原图视觉与无障碍首轮纵切复查

状态：**BOUNDED_REVIEW_COMPLETE / A11Y_REMOTE_NOT_RUN / NOT_FULL_ACCEPTANCE**。

本轮逐张实际查看 **40 张未改动的原始 PNG**：16 类表单各一张中文／深色／窄窗和英文／浅色／标准窗，另加 8 张日文／浅色／标准窗高风险状态。40 个实际文件均重新计算 SHA256 并与原始 `ui-surfaces.json` 一致。完整绝对路径、SHA、语言、主题、实际宽高、原始 syntheticState、观察和判断在 [JSON 索引](VISUAL_A11Y_REVIEW_20261029.json)。没有运行本机应用、测试、GUI 或安装器。

## 精确证据身份

- Forms run [40800828340](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40800828340)，源码 `ac5c77b4ab22d5ec3de444e41bb723e1cba68c4f`，tree `74c8a6f5ee0bc941716b905bd61a19edb8e29995`。
- 原图根目录：`D:/Codex/validation/gsw-ui-upgrade-20261028/forms-run40800828340/`。
- QA EXE SHA256：`40f3c4aaf5d9d7641be9e5ababa4da43e7cee7198c4e3dfb66066205de44cd6e`；surface-results SHA256：`36958147f7df747598d2b942c6ffb5619590652c45578a1e7f30919d3fa36d72`。
- 静态核查时当前源码为 `e48d535683d5d5269e422971e53c7ea104f9fd4e`。**旧 ac5 图绝不改标成当前 HEAD 已验收。**
- 35 张代表图未观察到新增布局／底色问题；2 张观察到盘符限定文字；2 张观察到同一作业状态文字不一致；1 张存在须实操才能判断的窄工具栏可达性疑点。
- 以上是图片观察分类，**不是 35 个表面整体 PASS**。原来的 16 类 × 192 最低格、300 捕获 profile 通过仍只保持自己的捕获／布局／公开参数合同。

## 已观察到的问题和下一最小步骤

| ID | 实际观察 | 证据及下一步 |
| --- | --- | --- |
| VIS-TEXT-01 | COLMAP 空路径提示仍要求“选择 E 盘上的 colmap.exe”／“Select colmap.exe on drive E”。这与 D 盘优先、仅 C 盘自动支持的策略不一致。 | 索引 #3、#4；当前 `ReconstructionDialog.cpp:81` 及 catalog 确认同一来源。Root 先补真实对话框三语 placeholder 回归，再改为盘符无关文案；本轮未改生产或 catalog。 |
| VIS-STATE-01 | 同一已选作业的下拉文字为 Running／Liveness Unverified（或実行中／生存未確認），详情与监控却为 Paused／Session Ended（或一時停止中／セッション終了済み）。 | 索引 #26、#38。当前 `ExistingTrainingDialog.cpp:272-279` 分别使用 m->jobs 与 m->report。旧清单假设尚非已证根因；须用自有 observer 作业由运行到暂停／结束的真实回归，保留 ID、原始记录及来源后最小修复。 |
| VIS-REACH-01 | 最窄 TaskDetails 工具栏未直接显示“导出保留日志”。 | 索引 #11。**NOT_CONFIRMED**：没有操作 Qt 原生 overflow，不能从一张图推断不可达；普通路径换行／表格省略不计 bug。后续若选此切口，应测试真实 overflow／键盘可达性。 |

本轮没有重复修复已关闭的初始化横向裁切、批量按钮长 caption 和暗色表头角落；当前代表图中均未再观察到。普通 table 列省略、路径文本滚动、原始 JSON/log、明确可用的纵横滚动不视为缺陷。

## 首个无障碍公开接口纵切：已写 QA，尚未远端执行

新增 [UiAccessibilityAssertions.h](../../native/tests/UiAccessibilityAssertions.h)，仅供既有真实窗口测试调用，不再生成一套 16 表单 gallery。入口：

```cpp
gsw::tests::checkAccessibleEditableField(
    QWidget &ownedWindow, QWidget &tabStart, QLineEdit &field,
    const QString &expectedName,
    const std::function<QJsonObject()> &publicSnapshot);
```

选择既有 `InitializationDialogUiTests` 中真实的 `initializationKind` → `initializationCount`。独立预期名称为中文“初始化点数”、英文“Initialization Point Count”、日文“初期化点数”。不是把机器 objectName 改成用户名称后自测。

helper 检查 Qt 实际 `QAccessibleInterface` 的 Name、EditableText role、enabled/focusable/visible/non-readOnly state，再发送最多 64 次真实 Tab key event，观察字段是否在自有窗口内得到焦点。它不调用 protected focusNextPrevChild，不写焦点顺序、不屏蔽 Qt focusProxy 语义、不扫描 Qt 匿名内部控件。随后恢复原自有焦点并比较调用者公开 request/report/running snapshot 不变；名称与键盘可达性分别留下真实结果。

只允许既有 GitHub Actions Windows 的已显示／激活自有窗口调用；不激活其他窗口，不改变生产 accessibleName、enabled、focus policy 或数值。

- **STARTED / COMPLETE（静态）**：使用现有 MSVC 14.34、Qt 6.8.3 执行 `/Zs /std:c++20 /Zc:__cplusplus /W4 /permissive-` 语法编译，exit 0；没有生成／运行 EXE。完整命令及输出见 JSON。
- **NOT_STARTED（远端运行）**：尚未整合到 Root 所有的现有测试/CMake/workflow；本轮没有 dispatch。没有观察到 runtime RED 或 GREEN，也不预先认定 Qt 无法推导表单标签。
- **STATIC_SUSPICION**：初始化 row 创建 QLabel + field，但未显式 setBuddy／accessibleName；Qt 可能有推导行为。Root 应先在当前源码远端运行，实际失败后再修生产标签关联／实时翻译。
- **NOT_RUN**：其他字段、禁用原因、完整 Tab／Shift-Tab、快捷键／菜单与 OS screen reader；图片可见 disabled 文案不等于其助残语义已通过。

Qt 官方公开接口核对：[QAccessible](https://doc.qt.io/qt-6.8/qaccessible.html#queryAccessibleInterface)、[Name/role/state](https://doc.qt.io/qt-6.8/qaccessibleinterface.html#text)、[setFocus](https://doc.qt.io/qt-6.8/qwidget.html#setFocus)、[focus proxy](https://doc.qt.io/qt-6.8/qwidget.html#setFocusProxy)。

## 当前已有键盘／无障碍证据的真实边界

静态查看现有入口及测试：`AppThemeTests.cpp` 有自建标准控件的真实 Tab / focus 样式测试，不能代替业务表单语义；`WindowUiSmokeTest.cpp` 有 Windows UIAutomation 最小化系统按钮及 F11/Escape 窗口行为，不能代替初始化字段的 QAccessible Name；`InitializationDialogUiTests.cpp` 已保护公开请求、报告、运行和原始文件，在 visual 模式提供真实窗口与 300-profile 捕获接线。此次 helper 复用这些实际 seam，不模拟内部 collaborator。

66 表面组／16 Dialog／147 popup 调用点的静态 inventory 仍保留原边界；baseline 之外新增 updater/startup-health 表面是独立增量。自绘 NativeViewport 操作器、HUD、菜单／tooltip、嵌套 popup、OS 文件提供器、跨屏缩放不能通过 QWidget 默认 probe 或命名整个 canvas 蒙混通过。

## 逐图复查索引

以下每行均是本轮实际看过的原图；详尽路径、SHA 和观察在 JSON 的相同 index。narrow 是各类窗口实际记录的最窄规格，不强行写成统一 680 px。

| # | Dialog | 语言／主题／尺寸 | 原始状态 | 观察分类 |
| --- | --- | --- | --- | --- |
| 1 | DatasetImportDialog | zh_CN / dark / 680×620 | import-media-request-narrow | PASS_REPRESENTATIVE |
| 2 | DatasetImportDialog | en_US / light / 1000×760 | import-media-request-standard | PASS_REPRESENTATIVE |
| 3 | ReconstructionDialog | zh_CN / dark / 680×620 | reconstruction-request-narrow | OBSERVED_TEXT_ISSUE |
| 4 | ReconstructionDialog | en_US / light / 1000×760 | reconstruction-request-standard | OBSERVED_TEXT_ISSUE |
| 5 | TrainingDialog | zh_CN / dark / 680×620 | training-2dgs-request-narrow | PASS_REPRESENTATIVE |
| 6 | TrainingDialog | en_US / light / 1000×760 | training-2dgs-request-standard | PASS_REPRESENTATIVE |
| 7 | MeshGenerationDialog | zh_CN / dark / 680×620 | mesh-generation-request-narrow | PASS_REPRESENTATIVE |
| 8 | MeshGenerationDialog | en_US / light / 1000×760 | mesh-generation-request-standard | PASS_REPRESENTATIVE |
| 9 | ModelExportDialog | zh_CN / dark / 680×620 | model-export-request-narrow | PASS_REPRESENTATIVE |
| 10 | ModelExportDialog | en_US / light / 1000×760 | model-export-request-standard | PASS_REPRESENTATIVE |
| 11 | TaskDetailsDialog | zh_CN / dark / 360×560 | retained-task-details-narrow | FOLLOW_UP_NOT_CONFIRMED |
| 12 | TaskDetailsDialog | en_US / light / 1000×720 | retained-task-details-standard | PASS_REPRESENTATIVE |
| 13 | GenerationHistoryDialog | zh_CN / dark / 680×620 | persisted-generation-history-narrow | PASS_REPRESENTATIVE |
| 14 | GenerationHistoryDialog | en_US / light / 1000×720 | persisted-generation-history-standard | PASS_REPRESENTATIVE |
| 15 | QualityComparisonDialog | zh_CN / dark / 680×620 | quality-comparison-completed-narrow | PASS_REPRESENTATIVE |
| 16 | QualityComparisonDialog | en_US / light / 1000×760 | quality-comparison-completed-standard | PASS_REPRESENTATIVE |
| 17 | QualityMeasurementDialog | zh_CN / dark / 680×620 | quality-measurement-completed-narrow | PASS_REPRESENTATIVE |
| 18 | QualityMeasurementDialog | en_US / light / 1000×760 | quality-measurement-completed-standard | PASS_REPRESENTATIVE |
| 19 | ScaleConstraintsDialog | zh_CN / dark / 680×620 | scale-constraints-adopted-narrow | PASS_REPRESENTATIVE |
| 20 | ScaleConstraintsDialog | en_US / light / 1100×760 | scale-constraints-adopted-standard | PASS_REPRESENTATIVE |
| 21 | InitializationDialog | zh_CN / dark / 560×620 | initializer-suggestion-pending-narrow | PASS_REPRESENTATIVE |
| 22 | InitializationDialog | en_US / light / 1100×760 | initializer-suggestion-pending-standard | PASS_REPRESENTATIVE |
| 23 | TrainingBatchDialog | zh_CN / dark / 680×620 | training-batch-plan-created-narrow | PASS_REPRESENTATIVE |
| 24 | TrainingBatchDialog | en_US / light / 1100×760 | training-batch-plan-created-standard | PASS_REPRESENTATIVE |
| 25 | ExistingTrainingDialog | zh_CN / dark / 420×620 | observer-ended-narrow | PASS_REPRESENTATIVE |
| 26 | ExistingTrainingDialog | en_US / light / 840×760 | observer-ended-standard | OBSERVED_STATE_TEXT_INCONSISTENCY |
| 27 | RuntimeGuardDialog | zh_CN / dark / 360×460 | guard-invalid-narrow | PASS_REPRESENTATIVE |
| 28 | RuntimeGuardDialog | en_US / light / 800×390 | guard-invalid-standard | PASS_REPRESENTATIVE |
| 29 | ResourceBudgetDialog | zh_CN / dark / 360×300 | resource-budget-manual-narrow | PASS_REPRESENTATIVE |
| 30 | ResourceBudgetDialog | en_US / light / 800×560 | resource-budget-manual-standard | PASS_REPRESENTATIVE |
| 31 | ViewportGridDialog | zh_CN / dark / 640×600 | unknown-unit-grid-controls-narrow | PASS_REPRESENTATIVE |
| 32 | ViewportGridDialog | en_US / light / 900×600 | unknown-unit-grid-controls-standard | PASS_REPRESENTATIVE |
| 33 | TrainingDialog | ja_JP / light / 1000×760 | training-2dgs-request-standard | PASS_REPRESENTATIVE |
| 34 | MeshGenerationDialog | ja_JP / light / 1000×760 | mesh-generation-request-standard | PASS_REPRESENTATIVE |
| 35 | QualityComparisonDialog | ja_JP / light / 1000×760 | quality-comparison-completed-standard | PASS_REPRESENTATIVE |
| 36 | ScaleConstraintsDialog | ja_JP / light / 1100×760 | scale-constraints-adopted-standard | PASS_REPRESENTATIVE |
| 37 | InitializationDialog | ja_JP / light / 1100×760 | initializer-suggestion-pending-standard | PASS_REPRESENTATIVE |
| 38 | ExistingTrainingDialog | ja_JP / light / 840×760 | observer-ended-standard | OBSERVED_STATE_TEXT_INCONSISTENCY |
| 39 | RuntimeGuardDialog | ja_JP / light / 800×390 | guard-invalid-standard | PASS_REPRESENTATIVE |
| 40 | ResourceBudgetDialog | ja_JP / light / 800×560 | resource-budget-manual-standard | PASS_REPRESENTATIVE |

## 已有作业状态不一致的最小 tracer（追加静态核查）

追加核查时 HEAD 为 `ed43bd6f4a88bc277190e3589bd9f8f7a0c3a7a8`，tree `9e9657d6e574b7b9733f69ef0de9da505f57dd83`；不把 ac5 原图改标为这个源码。实际源码／测试／worker SHA 记录在 JSON 的 `existingTrainingStateTracer.sourceFiles`。

`ExistingTrainingDialog::refreshJobs()` 在已连接时只执行 inspect；`finishHelper(list)` 更新 m->jobs，但 `finishHelper(inspect)` 只更新已验证的 m->report；`refreshPresentation()` 用旧 jobs 画 combo，用新 report 画详情。真实 worker 的 `list_jobs` 只是 `inspect_job` 的只读摘要，不能靠改 worker 状态或重复扫清单来遮盖 UI 投影问题。这条静态路径与 #26/#38 的原图一致，但新断言仍需实际远端 RED。

新增仅 QA [ExistingTrainingStateAssertions.h](../../native/tests/ExistingTrainingStateAssertions.h)，复用 `RuntimeDecouplingUiTests.cpp:297-325` 已有自有 CPU lease 暂停，不新增进程、训练或 gallery。保留原 pauseAcknowledged、NormalExit/exit0、paused/ended、无完整检查点不能续训的断言；在既有 observer-ended 的 real-window matrix optional validator 调用 `checkPausedObserverPresentation(observer,id,"generated CPU lease fixture",pausedLabel,endedLabel)`。

独立三语预期：中文 `已暂停` / `会话已结束`；英文 `Paused` / `Session Ended`；日文 `一時停止中` / `セッション終了済み`。helper 记录真实 currentData/currentText/ToolTipRole、jobId、完整 currentReport/afterCurrentReport 及公开报告不变，检查完整 caption/tooltip 与暂停终态一致；不设置状态／文字、不读私有 State。窄窗的视觉省略不会改变被检查的完整字符串。

此新增 header 在 MSVC14.34 / Qt6.8.3 的 `/Zs` 仅语法编译 **PASS**，SHA256 `6b1e394495a8ce2a471685992e1e18efd65bc6fc1257407e156ca4b3193d9bfd`。**远端 RED/生产修复/GREEN 均 NOT_STARTED**；Root 正负责接线，不在本轮提交或调度。实际 RED 后可考虑仅将已验证且 jobId 匹配的 inspect report 投影到该选中行，保留其他行、原始记录、后端与控制策略；这里未实施生产修复。

## 明确未签收的范围

- 本轮未人工查看其余 152 个 minimum matrix cells；也不将 300 capture profiles 统称人工签收。
- 仅 ac5 原图，不是当前／后续源码的视觉通过；本轮没有新产品包下载或 7f76 重复验收。
- 不是全部 popup、message/progress/file provider、main shell、dock、tooltip、menu、hover/disabled/failed/retry 状态或全部图标的签收。
- 未测 OS 实际五档缩放／跨显示器、screen reader、custom painter a11y、用户 NVIDIA／真实显存、GPU 时序。
- 不改性能原始 FAIL 与 4 项 INCONCLUSIVE，不重测求绿；不宣称训练、质量或几何改进。
- 不生成 fullAcceptance，不激活本地版本、不改 current/快捷方式、不重启用户程序、不碰工程／照片／模型／检查点／设置。
