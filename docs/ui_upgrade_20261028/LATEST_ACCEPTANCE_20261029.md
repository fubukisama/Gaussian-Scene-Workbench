# UI Upgrade — 2026-10-29 最新验收草稿

Status: **ARTIFACT_REVIEW_FOUND_FAILURES / REPAIR_IMPLEMENTED_REMOTE_NOT_RUN / NOT_FULL_ACCEPTANCE / NOT_DELIVERED**.

## 0. 最新原图与下载回读发现（优先于下文待导入文字）

2026-10-29 已取得完整 Forms ZIP：165537903 bytes，SHA256
`9d4b82ca440acd388fb050d423ad19771c4f1a940a453aff3f1b355e1e294d31`。
解压至 `D:/Codex/validation/gsw-ui-upgrade-20261028/forms-run40743097537/`。
真实结果为34/34选定CTest通过、32报告全部finished且无到限、661 native captures、
1681 raw PNG、16类对话框192格完整，300条profile均保留公开参数快照。
40732条字体证据均17 glyph / 0 missing；实际字体含Segoe UI、Yu Gothic UI、
Microsoft YaHei UI及Cascadia Mono，混排glyph-run还记录MS UI Gothic。
这不等于全量文字截切或视觉通过。

正常尺寸审阅16类的英文/浅色/窄窗代表图，并补看相关暗色图，发现：

- **初始化输入框横向裁切**：`standalone-surfaces/process-2288-58e03d3d59fd44f6afc6c11898ecb526/surface-0004.png`。
  560×620时左右split仍横排，左参数窗约193px，输入框在可视区右边；内外双重滚动使横向滚条也不在初始画面。
  修复采用窄窗上下排列及整行字段，外层统一纵向滚动，不缩字体或隐藏参数。
- **批量训练按钮裁字**：`forms-core/surfaces/process-7944-525200a4ce9b426b950d3a7822561b8d/surface-0006.png`。
  680×620时动作横排把完整英文caption左右截去；两组动作改为按原sizeHint自动换行。
- **暗色表格白角**：同process的`surface-0012.png`及初始化`surface-0010.png`。
  `QTableCornerButton::section`漏出原生白色；共用表头背景、边界样式已补。
  [Qt官方6.8样式说明](https://doc.qt.io/qt-6.8/stylesheet-reference.html#qtableview-widget)确认该独立section须显式设置背景与边界。

三处修复保留原三语言文字、请求/报告、按钮连接、worker和所有原断言；新加真实字段横向可用、
按钮完整sizeHint/窗口内/无重叠，以及表格角实际像素回归。它们目前只有编译与静态验证，**远端修复验收NOT_RUN**。

### 包下载回读缺陷

完整便携artifact ZIP：123645273 bytes，SHA256
`6aa7e1e92ce2de6ee4a93a07da1be86e21c908964c259c0dbbf6de5fcbd2025c`。
远端staging验证曾PASS，但ZIP中央目录2996entries、dotfile entries=0；本地静态
`verify_native_backend.py --package`实际FAIL，缺11个`.gitignore/.gitmodules/.gitkeep/.appveyor.yml/.travis.yml`路径。
对应source下隐藏源码元数据同样受过滤。这不是本地Expand-Archive漏解压，不是程序已经通过包回读。
修复上传受控payload的`include-hidden-files: true`，新增真实Actions upload→download→manifest/byte回读门；
`.git`仓库元数据仍禁止进入包。旧artifact不被重建后冒充原包通过。

Setup本体已单独核对：113631173 bytes，SHA256
`47c0b7cf4ba841dc7c71ef37dcbde33eb026f997823091993350318acf46dc3d`，
manifest指向EXE `e33c28987d793b91c66e1e061297820b11941cc4fa98e7d1a82802d11e309dba`。
Setup为NotSigned，不捆绑训练环境、不自动打开程序。三语言向导代表图已正常尺寸审阅；
独立test-AppId安装/重装/卸载/锁定升级拒绝/用户数据保留记录通过，但不等于本机已安装。
精确a539 Git源码归档SHA256为`55e7edded5651326d31ff6e79087a78b4463018e984edb8a945d403bafddb41d`；
它是另存的源码快照，不用于掩盖便携artifact漏文件。

这是一份增量证据台账，不是发布许可。旧的
`ACCEPTANCE_STATUS.md/.json` 保留为 `ffb8eb2` 历史快照；其中实际发生的失败、
原始结果和当时的未运行项不被本文件覆盖。本文把源码实现、视觉、交互、性能、
完整云端、包文件和本地交付分别判定，不宣称“全面升级完成”。

## 1. 源码与运行身份

| 身份 | 已确认值与边界 |
|---|---|
| 本轮完整远端候选 | `a5393be3772b8d49c6be93a767dc7dfd5abcec5a` |
| 本轮候选 Git tree | `f8c4b88f4c30976bef7d705802a808f35112d37d` |
| 观察到的后续 main | `82e71320a3604eedc0f804cefc050a446f502e30` |
| 后续 main Git tree | `11dbe6c93ed826706f7b1704083583d5b5bffdb0` |
| 后续差异范围 | Root 已核对为三个 updater-only 提交；与已测候选相比，MainWindow、AppTheme、NativeViewport、Initialization 和 CMake 无共享 UI 差异。这不是后续提交完整 CI 或新二进制已通过的声明。 |
| 新版包身份 | `0.3.9-native-preview` Setup 已下载且 SHA256 匹配；便携 artifact 已完整下载但发现隐藏文件被上传过滤，静态验证 FAIL。详见第 9 节。 |
| 本地执行边界 | 本轮仅静态验证、编译链接、文件检查及已下载远端证据读取；没有运行本地自动测试、应用、GUI 或安装器。 |
| 用户状态 | 未改变活动安装、`current.json`、快捷方式、工程、照片、模型、检查点或运行中的会话；没有开始或恢复真实训练。 |

此前修复后的像素及导航短门绑定的是
`de764f0a5ef3a8e813ec38ed19e3f232e4c83fbe`，不把该提交的截图或计时直接
改写成 `a5393be` / `82e7132` 的新测量。可引用共同实现及源码连续性，但证据仍保留
自己的 source / tree / run 身份。

## 2. 新近完成的云端结果

下列结果由 Root 直接读取 Actions 状态及日志确认；artifact 的逐文件审阅尚未全部导入。

| 远端运行 | 源码 | 已完成结果 | 不可外推的范围 |
|---|---|---|---|
| [Forms 40743097537](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40743097537) | `a5393be` | Actions **SUCCESS**；真实 Windows 窗体／样式通道完成。 | 16 类表单矩阵、原图、hash、实际字体及参数快照的 artifact 审阅仍为 **PENDING_FORMS_ARTIFACT_REVIEW**。不得先填写 16/16 视觉通过。 |
| [Native Windows 40743102181](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40743102181) — Qt 6 native preview | `a5393be` | **SUCCESS**；直接日志显示 4 个 graphics-bootstrap 检查及 175 个 CTests 通过；安装器三语言检查通过。 | 安装器 caption / 原始 PNG 正常尺寸审阅、包下载与静态身份绑定尚待完成；CI 成功不等于本机已更新。 |
| 同一 run — Upstream viewport acceptance | `a5393be` | **SUCCESS**；直接日志显示 65 + 6 项验收通过。 | 不包含用户 NVIDIA 驱动、真实跨屏或用户工程的验收。 |
| 同一 run — Native C++ CodeQL | `a5393be` | **SUCCESS**。 | 不是所有类别安全审计或后续 `82e7132` 全检查的证明。 |

当前可宣称的是：**`a5393be` 的上述完整云端与 Forms 工作流均成功**。
不能把它写成全量视觉、性能、包审阅、后续 main、生产更新源或本地托管交付全部通过。

## 3. 已关闭的旧失败与保留的原始记录

- 旧 `017f78c` 的五个导航／英文／日文／生命周期崩溃真实发生过，不删除其日志。
  诊断 run `40656943272` 的六个故障上下文定位于诊断元数据中的
  `QFontInfo::exactMatch()`；灰度实验没有修复故障。修复只移除该危险查询，保留实际
  字体请求、回退、大小和绘制；`exactMatch:null` 及明确原因不是伪造匹配成功。
- 修复候选 [40663272343](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40663272343)
  的 `navigation.log` 确认 **13/13 CTests 通过**：三语言导航及 presentation、UI cache
  lifecycle、实际视口 gizmo、字体和覆盖层合同。字体为 30 数据行，含 init / cleanup 共 32。
- 同一修复候选的中文像素子门为 **20/20 cases、60/60 rings**；冻结阈值仍是
  RMSE `.06`、P95 `.16`、halo `.04`。它不是三语言 × 所有图层 × 所有视角的全组合。
- 实际 gizmo 为 30 cases / 156 原始 PNG；scope 为中文、暗色、DPR1、透视／正交及两个
  观察姿态。鼠标 X 16 / Y 8，Z 为 6 个数值操作用例；不声称已经实测全部鼠标 Z、
  中心或平面约束组合。
- 旧 Forms `40663276104` 的 initializer 窄窗失败被保留。修复滚动内容区及底部按钮后，
  新通道已成功，但最新 16 类矩阵及正常尺寸原图仍须完成 artifact 审阅再写入本表。

保留证据目录：`D:/Codex/validation/gsw-ui-upgrade-20261028/`。原图不改写，
不把辅助裁切／放大图当作正常尺寸视觉判断。

## 4. 29 项范围化矩阵

本表“状态”仅使用原合同的 `PASS / FAIL / NOT_RUN / UNSUPPORTED / NOT_APPLICABLE`。
`NOT_RUN` 也用于尚未获得完整可接受结论的聚合门槛；已完成子门列在同一行，不抹掉其结果。
这里不提供误导性的“29 项全通过”计数。

| ID | 状态 | 已取得证据及严格范围 | 剩余 gate / 限制 |
|---|---|---|---|
| BASE-01 | PASS | 开始身份、并行修改与历史安装登记保留；本轮 a539 source / tree 已绑定。 | 每个新包及后续 main 仍需各自身份。 |
| AUDIT-01 | NOT_RUN | 66 表面、16 Dialog、147 popup 调用点静态清单存在。 | 最终逐项运行／视觉签收；新增 updater 对话框另列增量表面，不能假称原 16 类已包含。 |
| DPI-01 | NOT_RUN | de764 五档受控 Qt DPR 和逻辑／物理映射有真实元数据；高 DPR 小屏窗口修复已有远端运行。 | Windows 原生五档 OS 缩放及真实跨屏未测；`QT_SCALE_FACTOR` 不替代它们。 |
| NAV-01 | NOT_RUN | de764 中文 20 cases / 60 rings 像素 PASS；部分正常尺寸原图已审阅。 | 全部受影响正常尺寸姿态／状态视觉签收仍未闭环；不跨源码重标截图。 |
| NAV-02 | PASS | 修复后 de764 三语言真实输入短门通过；a539 完整 viewport 通道成功。 | 仅这些受控 Windows / software-GL 行为；不是用户 NVIDIA 或全部工程。 |
| NAV-03 | NOT_RUN | de764 的 20 次主题／resize／语言生命周期通过，缓存合同通过。 | 真实跨屏仍 NOT_RUN，不能因单屏生命周期通过而豁免。 |
| THEME-01 | UNSUPPORTED | 浅／深主题、白／暗场景背景保持；未用 Blender 灰掩盖缺陷。 | 任意用户 RGB / alpha 背景的可达配置在现有能力中缺失；不计为通过。 |
| FONT-01 | NOT_RUN | 三语言安全字体证据 30 行通过，17-glyph / 0 missing 合同保留。 | 所有表单 artifact 的实际字体／基线及自绘 caption、轴、图表、初始化预览角色仍需分别审阅。 |
| ICON-01 | NOT_RUN | AppTheme / UiRoles 公共合同已通过；a539 Forms 工作流成功。 | 最新五 DPR × 三语言 contact sheet、状态及正常尺寸视觉待 artifact 审阅。 |
| SHELL-01 | NOT_RUN | 共享 tokens、透明标签、inspector wrap、有效窗口最小尺寸已实现；完整原生通道通过。 | 宽／窄窗关键动作、溢出、菜单、dock、任务、状态的逐表面签收仍待导入。 |
| TREE-01 | NOT_RUN | 相关原生合同随完整通道通过；静态入口与 owner 已登记。 | 对应图像和选择／眼睛／滚动／分隔 resize 的具体证据映射尚待完整审阅。 |
| GIZMO-01 | NOT_RUN | 四模式共用有界 native-DPR coverage；de764 实际 30 cases / 156 PNG。 | 原图正常尺寸多距离／边缘视觉闭环未完成；极端超预算 close-up 明确使用原向量回退。 |
| GIZMO-02 | NOT_RUN | de764 真实鼠标 X / Y、数值 Z、取消／撤销相关断言通过；当前完整通道成功。 | 不扩大为尚未逐项确认的鼠标 Z、全部中心／平面手柄及组合。 |
| MODEL-01 | NOT_RUN | a539 完整原生测试成功，原有多对象合同没有删除。 | 原／非当前、全隐藏恢复的状态／画面证据与本项逐项绑定尚待导入。 |
| MODEL-02 | NOT_RUN | 受控 receipts 保留实际 renderer、计数、SH、变换和 fixture 身份。 | 点云／网格／3DGS／2DGS／任务预览的完整视觉签收不可由标签或性能 receipt 代替。 |
| OVERLAY-01 | NOT_RUN | 选择／刷选／裁剪／提示入口已登记，完整原生回归通过。 | 多尺度／视角／状态原图及数据保护的具体证据仍需逐项确认。 |
| GRID-01 | NOT_RUN | de764 三语 presentation 及 a539 viewport 通道通过。 | 自动／固定密度、mm / m / u、重开和模型切换证据需按本项签收，不只引用总绿灯。 |
| FPS-01 | NOT_RUN | 新帧 serial / swap / metrics 及有效空闲原始时窗存在；未增加持续重绘计时器。 | 聚合性能尚无完整接受结论，不能只取局部 0 计数宣称整个门槛完成。 |
| FORM-01 | FAIL | a539 Forms 工作流 SUCCESS；16 类192格证据完整，34行为检查通过。 | 原图实际发现初始化窄窗输入框裁切、批量训练按钮裁字及暗色表格白角；不能以完整capture计视觉PASS。修复及新断言待远端验收。 |
| STATE-01 | NOT_RUN | 受控任务、worker 及表单合同随完整通道通过。 | 空／加载／忙／失败／取消／重试／不支持的具体 UI 证据尚待汇入，不制造固定成功文本。 |
| LOCALE-01 | NOT_RUN | 三语言目录检查、修复后真实导航运行通过；当前 Forms / 完整 CI 成功。 | 全表单即时切换布局、长文本、混合文件名及状态保持的 artifact 审阅仍待完成。 |
| A11Y-01 | NOT_RUN | focus inset、geometry 稳定、语义图标公共合同通过。 | 全表面 Tab / 快捷键 / tooltip / 可访问名 / 禁用原因仍需逐项闭环。 |
| LIFE-01 | NOT_RUN | de764 UI cache 生命周期通过；a539 完整回归保留持久队列／取消合同。 | 窗口、任务、重连边界证据需对应签收；MainWindow 嵌套完整续训信任提示不由独立 guard 图替代。 |
| PERF-01 | NOT_RUN | 24 份有效 receipts、空场景及部分 overlay 指标通过；冻结阈值未放宽。 | 原始整体结果 FAIL；修正 checker 尚未在相同 receipts 上取得新的远端复核；中模型四项 INCONCLUSIVE。见第 5 节。 |
| PERF-02 | NOT_RUN | 有效空闲计数与独立生命周期部分通过；CPU / requested GPU 局部上限有记录。 | 驱动 VRAM 驻留、GPU 毫秒、暗色性能、用户 NVIDIA 等未测；requested bytes 不等于显存驻留。 |
| PORT-01 | NOT_RUN | 固定 Blender 4.5 / Lucide 来源与 NOTICE、独立 Qt 适配器和未移植边界已登记。 | 最终候选包的资源／依赖／许可／修改及 hash 仍待静态签收；不声称复制了 Blender 5.2.3 GPU AA。 |
| PKG-01 | FAIL | a539 完整构建及三语言安装器检查通过；Setup本地SHA匹配。 | 下载便携包被GitHub默认hidden-file过滤，backend/source清单不完整；旧包不修写为PASS，上传规则与实际回下载检查待远端验证。 |
| DELIVER-01 | NOT_RUN | 托管安装协议及原有稳定启动器不变；本轮未动活动版本。 | 未发布候选；不得生成虚假的 `fullAcceptance:true`。发布后仍需 current / EXE / recognized shortcuts 独立读回。 |
| CLOUD-01 | PASS | 精确 a539 的 Forms 及完整 Native Windows 三个 jobs 均 SUCCESS；见第 2 节。 | 仅此 source；82e7132 及后续提交的完整云端不能由它继承。PASS 不代替视觉、性能、包或交付门槛。 |

## 5. 性能：原始 FAIL 与 INCONCLUSIVE 必须同时保留

保留文件：
`D:/Codex/validation/gsw-ui-upgrade-20261028/performance-run40663280091/results.json`。

- 原始 `status` 是 **FAIL**，不能改写。
- 原始 failure 是 `light-dpr1: paired fixture/fidelity/camera sequence mismatch: 3dgs/model`。
  已诊断为跨进程累计 attribute / order upload counters 的绝对值比较；每个进程操作前后
  的数值保持，不能把允许的预热调度差异误作模型改变。
- 新 checker 改为比较语义模型身份及操作内零 upload deltas，同时保留原始绝对计数。
  **源码修复不等于相同 receipts 已经重新验收**；远端针对同一批不可变 receipts 的
  复核结果此处仍是 **NOT_RUN**。不需要为了这个 checker 修复重跑计时实验。
- 中模型 `present_p50_ms`、`response_p50_ms`、`present_max_ms`、`response_max_ms`
  四项测量结论是 **INCONCLUSIVE**：A 臂波动超过冻结噪声预算。即使各 pair 的差值
  落在限制内，也不能无视波动改判 PASS。
- 不放宽预算，不反复运行直到绿灯；旧图像和计时不重写。`INCONCLUSIVE` 不是已证明
  性能退化，也不是“没有退化”。
- 未测项继续包括实际 GPU 毫秒、driver VRAM residency、physical input-to-photon、
  dark-theme performance、真实跨屏、用户 NVIDIA 和中等点云／网格／2DGS 压力。

冻结文件仍为 `PERFORMANCE_PLAN.md` 与 `PERFORMANCE_ACCEPTANCE.json`；本报告没有
改变阈值、重复单位、观察时窗、模型质量、渲染精度或原有测试 timeout。

## 6. Forms artifact 与增量 updater 表面

### 6.1 原 16 类表单

**PENDING_FORMS_ARTIFACT_REVIEW**。

待导入字段：source / tree、16 类完整性、三语 × 明暗 × 宽窄覆盖、实际 client geometry、
font / glyph facts、原始 PNG 与 SHA-256、公开 options / request 前后不变快照、任何
SKIP / NOT_RUN / 截断。Actions success 不能代替这些尚未审阅的明细。

### 6.2 新 updater 对话框

此界面来自并行授权的原生自动更新工作，原 66 表面 / 16 Dialog 基线未包含它，必须作为
额外可达表面记录，不扩大原清单的覆盖声明。已知独立证据为
[40718734156](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40718734156)
在 `2e0cac4` 的 `isolated-update-ui`：真实 Qt 6.8.3 Windows widgets 7 PASS / 0 skip、
三语 PNG、无自动弹窗／置顶操作。当前图像／后续 updater-only 改动需保持自己的 source 证据。

UI 接入不代表生产签名、公开更新源、可信根、bootstrap 迁移或生产发布获准；缺失信任
不能显示为“已是最新”。普通 UI 版本发布不得替换 bootstrap 或重写稳定入口。

## 7. 包与本地交付

目前状态：**NOT_DELIVERED**。

| 字段 | 当前值 |
|---|---|
| EXE / SHA-256 | PENDING_PACKAGE_DOWNLOAD_AND_STATIC_REVIEW |
| Setup / SHA-256 | PENDING_PACKAGE_DOWNLOAD_AND_STATIC_REVIEW |
| 对应 source / tree / files manifest | PENDING_PACKAGE_DOWNLOAD_AND_STATIC_REVIEW |
| 安装器原始三语 caption / PNG 审阅 | PENDING_INSTALLER_RAW_PNG_REVIEW |
| 完整接受凭证 | NOT_CREATED；当前仍有明确开放 gate |
| managed publisher / publication record | NOT_RUN |
| current.json / active EXE / recognized shortcuts 一致读回 | NOT_RUN |
| 活动版本、运行／回滚版本保留 | 本轮未改变安装，不将“未改变”写成新版本已交付 |

后续按 `docs/NATIVE_UPDATES.md` 使用托管 publisher；先完成对应 source 的接受与包静态
审阅，再原子切换版本。桌面、开始菜单、任务栏继续使用稳定原生 launcher。普通更新不
重复迁移快捷方式、不关闭程序、不改工程或训练状态。固定目录 Setup 是独立 adapter。

## 8. 下一步及完成条件

1. 导入当前 Forms artifact，逐项核对原图／矩阵／参数快照，保留真实缺项。
2. 下载精确 a539 包和安装器证据，只做本地文件／hash／依赖／许可静态审阅；不运行本地 GUI。
3. 完成正常尺寸代表图及剩余表面的视觉签收；硬件／OS／provider 限制明确保留。
4. 对旧计时 receipts 的 checker 修复做远端复核；中模型噪声仍独立 INCONCLUSIVE，
   不用重复刷绿或放宽门槛消除它。
5. 如后续 main 要作为发布 source，取得其对应接受及包身份；不能把 a539 的绿色改名继承。
6. 只有相关 gate 真正闭环后才生成完整接受凭证及执行 managed publication，并独立读回交付身份。

视觉更精致不等于重建几何更准确、训练更快或质量更高；本轮没有作这些声明。
