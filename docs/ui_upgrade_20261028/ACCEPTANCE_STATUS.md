# UI Upgrade — 当前验收证据快照

快照生成：**2026-10-28 08:52:28 UTC**；源码观察时间08:38:59 UTC。这是一份**当前证据索引，不是完成报告，也不是安装/发布许可**。最后本地读取的源码为 `ffb8eb23679575ff55a3207b35c21e6f8225ceff`（main，tree `8d8ec309e578b7c5c9a43865abf4e62ac1ef137b`）。本台账只写此 Markdown 与同名 JSON；没有运行本地 tests/app/GUI、训练、下载 artifact、读取原始用户数据、修改代码/阈值/计划、轮询委派run或重读安装状态。

机器可读真值：[ACCEPTANCE_STATUS.json](ACCEPTANCE_STATUS.json)。29项门槛与66个表面均完整列入；原147个popup调用点/入口/owner沿用 [SURFACE_INVENTORY](SURFACE_INVENTORY.md)，不复制成147条虚假的运行记录。

## 1. 结论必须拆开

| 证据维度 | 当前判定 | 严格范围 |
|---|---|---|
| 源码/开始身份 | PASS | 开始时main/源码/工作区/旧安装及云端快照已登记；并行修改保留。不是旧安装的当前实时状态。 |
| 已测环境身份 | PASS | **旧017f78c**的合成空场景、Qt6.8.3、Windows/Mesa llvmpipe环境与原始元数据；不是用户NVIDIA环境。 |
| 中文导航像素子门 | PASS | **旧017f78c**的20/20 cases、60/60 rings及冻结指标；不使整次run通过。 |
| 当前ffb8eb2全产品视觉 | NOT_RUN | 没有导入该源码的完成截图/操作结果。P1/P2源码实现不等于视觉通过。 |
| 导航/缓存生命周期 | FAIL | 旧017f78c的5项原生崩溃仍未被本台账所读取的匹配证据关闭；不是timeout。 |
| 三语言运行 | FAIL | 中文两族通过不能盖住英文/日文崩溃；静态catalog完整也不能盖住崩溃。 |
| 性能证据合同 | FAIL | 40649923711时窗/曝光无效，`comparisons=[]`；不是已测得性能退化。 |
| 有效性能差值/完整资源结论 | NOT_RUN | 没有可接受的三轮对照；requested GPU bytes不等于实际驱动VRAM驻留。 |
| 新产品包/本地交付 | NOT_RUN | 新UI候选EXE/Setup/托管交付未完成；源码ZIP或编译链接记录不是安装包。 |
| 当前源码完整云端 | NOT_RUN | 委派短门仍未导入；旧87a完整CI或中文短门不能代替当前完整checks。 |

当前29IDs分布：**PASS 1 / FAIL 6 / NOT_RUN 21 / UNSUPPORTED 1 / NOT_APPLICABLE 0**。唯一整项PASS是BASE-01的开始身份登记。**66/66当前表面的运行/视觉验收均为NOT_RUN**；这不删除旧源码的具体PASS或FAIL，而是防止跨源码晋级。

## 2. 分类与源码规则

只使用 `PASS / FAIL / NOT_RUN / UNSUPPORTED / NOT_APPLICABLE`：

- PASS必须写清源码、环境、公开缝和证据，不能跨语言、DPI、表面、模型、版本或性能窗口外推。
- FAIL表示实际执行未满足门槛，保留失败；未读到修复后匹配结果时仍是开放失败。**不把旧017f78c失败伪称为ffb8eb2已经失败。**
- NOT_RUN包括未运行、正在运行但未导入、尚未审阅的结果。已有CTest/代码/静态映射、测试程序存在、历史pass都不是当前候选运行通过。
- UNSUPPORTED不计通过，也不自动豁免需求；必须留下技术原因和开放门槛。
- NOT_APPLICABLE仅用于有明确影响分析的非本改动结论。本次不宣称训练速度、PSNR或几何精度提升，也不宣称把Blender5.2.3源码/GPU AA复制进来；这不豁免已有资源/许可/交付门槛。

`matrix[].status`是任务要求的开放门槛状态；每行另存 `current_source_runtime_status=NOT_RUN`。旧源码的范围化证据位于 `completed_evidence` / `prior_source_claims`。表面的 `inventory_entry_registered=PASS` **只表示基线静态登记存在**，其runtime仍NOT_RUN；baseline owner行号不是当前ffb8eb2行号。

## 3. 已完成远端：像素PASS与运行FAIL同时保留

### 3.1 基线与旧候选像素

[基线run40641681307](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40641681307)绑定 `4f1dbb8f2ad086b3d855e112b835d6b641f81bf9`，20cases中14FAIL/6PASS、60rings中39FAIL。原始图/指标/585文件绑定见 [P0_BASELINE_RESULTS](P0_BASELINE_RESULTS.md) 与JSON，未重算图片。

[旧候选run40649920338](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40649920338)绑定 `017f78cfbf0c35873fccc7029a6a7a056707cacb` / tree `52bc24b59d41f45f08c653ed3197509f03635254`：

- 中文20/20 cases、60/60 rings PASS；max RGB RMSE **0.026438927393213994**、P95 **0.0621412327397675**、outer halo **0.011764705882352941**。
- 冻结阈值仍是 **.06 / .16 / .04**；独立32×32物理像素覆盖oracle不变，未放宽阈值。
- 正常尺寸只读审阅的明确scope是最小 `d1-s0-light` 原图；不能外推全部视角、图标、表单和后端。
- 原始capture元数据确认locale只有 **zh_CN**，五档measured Qt DPR为1/1.25/1.5/1.75/2，但全部 `actualWindowsWindowDpi=96`；UI scale是90或100%，不是降低模型渲染分辨率。
- 97/99条可见widget-default混排字体记录仅属该中文主窗。它们不是16个Dialog或QPainter自绘caption/轴/变换条/曲线/初始化预览的全字体证明。
- 真实GL是Mesa llvmpipe LLVM23.1.2、GL4.6Core/Mesa26.2.4，Qt6.8.3。不是用户NVIDIA驱动的验收，也不是五档原生OS缩放或真实跨屏。
- 基线原始protocol SHA为 `c1d3f422abeac3cebb4d5e3bf2965c7989762e71a0ad45e9fb141b2bc287586e`，候选为 `f25fc6a5e624bc09b5f617127207d5bc183c27975397cb63d7fb261db0845681`。本次只读解析JSON对象相同、阈值相同；**原始字节/hash不同，不称字节相同**。

### 3.2 同一旧候选的真实运行失败

| CTest | 结果 | 用时 |
|---|---|---|
| native_ui_overlay_lifecycle | FAIL — SegFault；声明timeout30秒 | 7.68秒 |
| native_upstream_navigation_en_US | FAIL — exit0xc000041d | 1.57秒 |
| native_upstream_navigation_ja_JP | FAIL — exit0xc000041d | 1.32秒 |
| native_blender_viewport_presentation_en_US | FAIL — SegFault | 1.06秒 |
| native_blender_viewport_presentation_ja_JP | FAIL — SegFault | 1.09秒 |

9项中4PASS/5FAIL，总28.10秒。通过的是两族zh_CN、`ui_overlay_raster`、`navigation_gizmo`。首错误不是30秒超时；根因仍未知，不能从崩溃推定“字体丢失”“驱动坏了”或“缓存泄漏”。直接日志和原navigation.log都保留，见E-NAVFAIL。

### 3.3 性能：证据失败，不是退化结论

[性能run40649923711](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40649923711)的A是 `b0e9fae5c53156b74e247e9c56ab3d0bd1e4a0ff`，B是旧 `017f78c`。原始24份process receipt都存在，18 exit0 / 6 exit3，无timeout；6个DPR2 empty流程因未完全曝光，raw报告记为INCONCLUSIVE。本台账把这些未接受测量记NOT_RUN，不能记PASS。

结果 `FAIL`有10条时窗合同失败，包含actual settle不足600ms、1秒idle窗口太短或重叠；`comparisons=[]`。即使局部raw idle counter为0，也不能接受为冻结空闲门槛已通过。没有有效3轮性能差值、噪声或资源退化/不退化结论。原阈值/计划/失败均保留；需要同等修正A/B producer与observer后再取得有效证据，而不是放宽预算。

## 4. 29 IDs台账

| ID | 门槛状态 | 范围 | 证据ID | 现有证据与剩余gate |
|---|---|---|---|---|
| BASE-01 | PASS | 开始时身份与并行修改保护 | E-BASE, E-INV | 开始时源码/工作区/远端与旧安装快照已登记；此PASS仅指该历史身份记录，不宣称当前安装仍是旧快照。 新提交/包需各自重新绑定；本次不重读安装。 |
| AUDIT-01 | NOT_RUN | 全量表面最终审计 | E-INV, E-COVER, E-AUDIT | 66表面、16Dialog、147调用点已静态登记；没有以无需修改冒充已视觉检查。 逐项远端视觉/交互/限制签收；源码覆盖不是运行结果。 |
| DPI-01 | NOT_RUN | 五档Windows缩放/逻辑物理映射 | E-BPIX, E-CPIX | 017f78c中文controlled Qt DPR五档映射已记录；Windows窗口DPI仍96。 真实OS 100/125/150/175/200%与跨屏迁移另测；不能用QT_SCALE_FACTOR替代。 |
| NAV-01 | NOT_RUN | 导航边缘与正常尺寸视觉 | E-BPIX, E-CPIX, E-P0DOC | 017f78c中文20/20cases、60/60rings通过冻结oracle；d1-s0-light正常尺寸原图已有只读审阅。 当前ffb8eb2未导入匹配运行结果；其余正常尺寸/姿态/语言与整体稳定性不可外推。 |
| NAV-02 | FAIL | 真实导航/相机交互 | E-NAVFAIL | 017f78c的zh_CN两族通过，en_US/ja_JP导航与presentation共4进程崩溃。 先取得并修复真实异常，再按三语真实输入验收；新运行尚未读取，FAIL尚未关闭。 |
| NAV-03 | FAIL | 显示变化与缓存/命中一致 | E-NAVFAIL, E-P0DOC | native_ui_overlay_lifecycle在7.68s SegFault，不是30秒timeout。 完整20次theme/resize+语言生命周期、DPR变化/拾取一致及真实跨屏尚待通过。 |
| THEME-01 | UNSUPPORTED | 浅深与用户自定义RGB/alpha背景 | E-INV, E-BPIX, E-CPIX | 白/暗主题在中文像素fixture有记录；用户任意RGB/alpha的可达设置在基线缺失，P1未声称新增。 补能力或由Root明确记录需求决策；不得以换Blender灰遮缺陷，也不能计为通过。 |
| FONT-01 | NOT_RUN | 全部控件与自绘文字字体/回退 | E-AUDIT, E-BPIX, E-P1 | 基线中文主窗可见widget mixed-script probe为0missing；P1保留声明的三语字体族。 三语全部Dialog与自绘caption/轴/变换/图表/初始化预览的实际字体、基线和清晰度仍缺；glyph probe不是文字视觉判定。 |
| ICON-01 | NOT_RUN | 全部图标正常尺寸/状态 | E-AUDIT, E-P1, E-COVER | 67语义ID/63 SVG的静态资源审计存在；新role/DPR/alpha contracts已实现。 当前候选真实contact sheet、focus/disabled/checked、五档DPR结果尚未导入。 |
| SHELL-01 | NOT_RUN | 主菜单/两层工具栏/dock/属性/任务/状态 | E-P1, E-COVER, E-INV | 共享tokens和若干shell实现有源码记录，不代表全shell已验收。 三语明暗标准/窄窗关键动作、溢出、布局状态保护及截图。 |
| TREE-01 | NOT_RUN | 树/眼睛/空白区/滚动/分隔 | E-COVER, E-INV, E-P1 | 历史实现与可复用测试已映射，当前候选测试未取回。 真实多模型选择/可见性、无贯穿线、滚动与实际resize；不得用旧pass代替。 |
| GIZMO-01 | NOT_RUN | 四模式变换器外观 | E-P1, E-COVER | Move/Rotate/Scale/Combined共用有界coverage已实现；极端close-up有原向量回退。 单项/合并、多距离、边缘角、正交/透视真图；杆/帽/面遮挡与正常尺寸审阅。 |
| GIZMO-02 | NOT_RUN | 变换拾取/约束/数值/撤销 | E-P1, E-COVER | 新实际视口输入测试已实现，静态QImage旧测试仅辅助。 匹配源码的真实手柄拖动、中心/轴/面、Cancel/Undo/Redo矩阵；相机edit-navigation不等于模型变换。 |
| MODEL-01 | NOT_RUN | 多模型选择/隐藏/坐标与相机保护 | E-COVER, E-INV | 可复用visibility/coordinates合同存在；当前候选未运行本项验收。 原/非当前/全隐藏恢复的相机、变换、单位、文件hash和画面。 |
| MODEL-02 | NOT_RUN | 五类renderer正确性 | E-COVER, E-PERF, E-P1 | 性能原始文件含受控轻/中fixture，但性能合同无效，不是完整renderer验收。 点云/网格/3DGS/2DGS/任务预览分别核对实际renderer、画面、选择导航和保真。 |
| OVERLAY-01 | NOT_RUN | 选择/笔刷/裁剪/提示 | E-COVER, E-INV, E-P1 | 已有绘制/状态入口已静态登记。 多尺度/视角/主题真实图与selection/deletion/模型状态保护，不能仅检查SVG或标题。 |
| GRID-01 | NOT_RUN | 网格/单位/固定基准 | E-COVER, E-NAVFAIL | 017f78c中文presentation流程通过部分网格/投影；英文/日文presentation崩溃。 当前候选自动/固定密度、mm/m/u、保存重开与模型切换不改数值；三语图/状态。 |
| FPS-01 | NOT_RUN | 真实新帧计数与空闲 | E-COVER, E-PERF | raw idle计数部分为0，但不满足冻结最短观测时长，不能接受为空闲PASS。 新rendered serial+fresh swap+metrics/idle调度的有效时窗；不以渲染耗时倒数冒充显示FPS。 |
| FORM-01 | NOT_RUN | 全部可达表单 | E-INV, E-COVER, E-P1 | 16类Dialog+4 inline+4 progress和popup目的已映射；新远端form modes已接线。 每个真实入口/布局/错误/禁用、三语明暗窄窗图和公开request/options精确回读；现有源码不计运行PASS。 |
| STATE-01 | NOT_RUN | 空/忙/失败/取消/重试/不支持 | E-COVER, E-INV, E-P1 | 受控CPU/worker状态已有可复用合同；当前候选结果未导入。 真实操作触达各状态，保持安全默认与不自动训练/续训；不制造固定成功文本。 |
| LOCALE-01 | FAIL | 三语即时切换与状态保护 | E-NAVFAIL, E-P1 | 静态三语catalog编译记录通过，但017f78c英文/日文原生测试崩溃。 修复并通过匹配源码三语真实窗口/视口/长文本/混合文件名，worker/模型/参数保持；编译完整不能盖崩溃。 |
| A11Y-01 | NOT_RUN | 键盘焦点/tooltip/可访问名/禁用理由 | E-P1, E-COVER | 新focus inset与公开palette/icon contracts在源码。 真实Tab/快捷键/focus像素与geometry、禁用理由可达、不仅以颜色区分状态。 |
| LIFE-01 | FAIL | 生命周期与持久任务边界 | E-NAVFAIL, E-COVER, E-P1 | 017f78c UI cache lifecycle进程崩溃；没有据此断言训练worker泄漏。 关闭/重开/取消/重连与持久队列合同、本候选窗口/缓存析构；嵌套MainWindow续训信任prompt仍单列未验。 |
| PERF-01 | FAIL | 三轮性能/延迟/资源包络 | E-PERF, E-PERFPLAN | 40649923711结果为证据合同FAIL，comparisons=[]；不是已测得性能退化。 保留阈值，对两臂同一producer/observer修正采样，再获得有效3轮分布与噪声/预算判断。 |
| PERF-02 | FAIL | 空闲与重复操作资源 | E-PERF, E-NAVFAIL, E-PERFPLAN | idle时长不合格/曝光不足，且cache lifecycle崩溃；无有效泄漏或驱动VRAM结论。 有效idle两1s窗、600ms settle、反复切换/销毁资源记录；CPU/requested GPU不等于实际VRAM驻留。 |
| PORT-01 | NOT_RUN | 来源/许可/未移植边界 | E-AUDIT, E-REF, E-P0IMPL, E-P1 | 静态来源含Blender4.5/Lucide revision与NOTICE；coverage是GSW Qt适配器，不是照搬Blender GPU shader。 当前完整候选及最终包的URL/commit/path/hash/license/依赖/修改/NOTICE绑定签收；不能用Blender5.2.3参考图声称5.2源码移植。 |
| PKG-01 | NOT_RUN | EXE/Setup/源码/资源/许可绑定 | E-BASE, E-P1 | 存在静态编译/链接记录与源码快照，不等于新产品包。 构建当前接受源码、候选EXE/Setup/清单/资源/许可/依赖与独立hash边界检查。 |
| DELIVER-01 | NOT_RUN | 托管发布与会话/快捷方式保护 | E-BASE, E-P1 | 旧87a797b安装只是2026-10-28T05:13:28Z历史快照，可能漂移；此台账未重查。 新candidate未交付；后续publisher/current.json/EXE/recognized shortcuts一致，保留会话与回滚。 |
| CLOUD-01 | NOT_RUN | 当前提交完整云端 | E-NAVFAIL, E-PERF, E-PENDING, E-BASE | 最后已读candidate P0/performance失败；ffb8eb2诊断/foundation由其他agents跟进，此处不导入未完成结果。 精确当前source的完整Native Windows/安全/相关checks独立记录；短门/历史87a成功与本地交付分开。 |

## 5. 66表面当前候选台账

每行的入口、baseline owner、相关矩阵、已有可复用测试family和详细剩余检查见JSON及母表。JSON逐项保留母表的146条required remote checks，未删减门槛。**没有一行被标为“已视觉检查，无需修改”**。尤其P2类的所有表单、内联窗、进度、消息、嵌入编辑器，仅有静态映射/测试实现不计runtime PASS。

| 表面 | 名称 | 当前ffb8eb2运行/视觉 | 相关矩阵 | 已读证据边界 |
|---|---|---|---|---|
| INF-01 | 应用字体、密度与控件状态 | NOT_RUN | FONT-01, ICON-01, SHELL-01, FORM-01, LOCALE-01, A11Y-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INF-02 | 系统标题栏与窗口生命周期 | NOT_RUN | SHELL-01, FORM-01, LIFE-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INF-03 | 语义SVG图标与状态 | NOT_RUN | ICON-01, DPI-01, A11Y-01, PORT-01, PERF-02 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INF-04 | 多项列表快捷键/工具条/批量移除 | NOT_RUN | TREE-01, FORM-01, STATE-01, A11Y-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INF-05 | 文件选择器与常用位置/历史 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INF-06 | 右键菜单、tooltip及键盘焦点 | NOT_RUN | SHELL-01, A11Y-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-01 | 主菜单及所有子菜单 | NOT_RUN | SHELL-01, A11Y-01, LOCALE-01, STATE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-02 | 第一行主工具栏 | NOT_RUN | SHELL-01, ICON-01, A11Y-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-03 | 第二行渲染/预览工具栏 | NOT_RUN | SHELL-01, ICON-01, MODEL-02, STATE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-04 | 第二行选择工具栏 | NOT_RUN | SHELL-01, ICON-01, A11Y-01, GIZMO-02, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-05 | 第二行编辑操作工具栏 | NOT_RUN | SHELL-01, ICON-01, GIZMO-02, STATE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-06 | Dock标题、浮动/关闭按钮和分隔拖动区 | NOT_RUN | SHELL-01, FONT-01, ICON-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-07 | 工程树及眼睛可见性列 | NOT_RUN | TREE-01, MODEL-01, A11Y-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-08 | 工程树右键菜单 | NOT_RUN | TREE-01, MODEL-01, A11Y-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-09 | 属性面板：工程/数据集 | NOT_RUN | SHELL-01, FONT-01, FORM-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-10 | 属性面板：场景几何/变换/单位 | NOT_RUN | SHELL-01, FORM-01, GRID-01, MODEL-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-11 | 任务表、工具条及右键菜单 | NOT_RUN | SHELL-01, STATE-01, A11Y-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-12 | 共享训练监控及曲线 | NOT_RUN | SHELL-01, FONT-01, STATE-01, LOCALE-01, LIFE-01, FPS-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-13 | 日志页及跟随/导出工具栏 | NOT_RUN | SHELL-01, FONT-01, STATE-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-14 | 系统状态栏与临时消息 | NOT_RUN | SHELL-01, STATE-01, FPS-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-15 | 拖放反馈与导入摘要 | NOT_RUN | OVERLAY-01, STATE-01, LOCALE-01, MODEL-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| SH-16 | 界面设置/布局恢复入口 | NOT_RUN | SHELL-01, DPI-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-01 | 真实模型渲染区域 | NOT_RUN | MODEL-02, PERF-01, PERF-02, THEME-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-02 | 导航朝向轴圆盘/圆环/标签 | NOT_RUN | NAV-01, NAV-02, NAV-03, DPI-01, THEME-01, FONT-01, PERF-01 | 017f78c：中文像素PASS；EN/JP/生命周期FAIL（不代表ffb8eb2已执行） |
| VP-03 | 导航小按钮与投影提示 | NOT_RUN | NAV-02, NAV-03, ICON-01, A11Y-01 | 017f78c：中文像素PASS；EN/JP/生命周期FAIL（不代表ffb8eb2已执行） |
| VP-04 | 模型变换器：全部四种模式 | NOT_RUN | GIZMO-01, GIZMO-02, MODEL-01, DPI-01, PERF-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-05 | 视口内模型编辑工具条 | NOT_RUN | GIZMO-01, ICON-01, A11Y-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-06 | 观察轨迹球 | NOT_RUN | NAV-02, OVERLAY-01, THEME-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-07 | 模型选择轮廓/包围框/轴线 | NOT_RUN | OVERLAY-01, MODEL-01, GIZMO-02 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-08 | 屏幕选择手势与笔刷半径 | NOT_RUN | OVERLAY-01, GIZMO-02, DPI-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-09 | 相机轨迹/相机线框 | NOT_RUN | OVERLAY-01, MODEL-02, GRID-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-10 | 网格/基准轴/标尺 | NOT_RUN | GRID-01, THEME-01, MODEL-01, PERF-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-11 | 常驻场景标题与计数/模式字幕 | NOT_RUN | OVERLAY-01, FONT-01, LOCALE-01, STATE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-12 | 处理/训练预览阶段面板 | NOT_RUN | OVERLAY-01, STATE-01, FPS-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-13 | 底部简洁帧状态与详情按钮 | NOT_RUN | FPS-01, SHELL-01, LOCALE-01, PERF-02 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| VP-14 | 空/加载/失败/全部隐藏状态 | NOT_RUN | MODEL-01, MODEL-02, STATE-01, FPS-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-01 | DatasetImportDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-02 | ReconstructionDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-03 | TrainingDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-04 | MeshGenerationDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-05 | ModelExportDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, MODEL-02 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-06 | TaskDetailsDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-07 | GenerationHistoryDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-08 | QualityComparisonDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-09 | QualityMeasurementDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-10 | ScaleConstraintsDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01, GRID-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-11 | InitializationDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01, MODEL-02 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-12 | TrainingBatchDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-13 | ExistingTrainingDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-14 | RuntimeGuardDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-15 | ResourceBudgetDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, PERF-02 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| DLG-16 | ViewportGridDialog | NOT_RUN | FORM-01, STATE-01, LOCALE-01, GRID-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INL-01 | 视口详情 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INL-02 | 恢复中心 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INL-03 | 版本历史 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| INL-04 | 外部备份历史 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| PROG-01 | 恢复外部备份 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| PROG-02 | 导出模型 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| PROG-03 | 导入场景 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| PROG-04 | 批量移除记录 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| MSG-01 | 所有信息/警告/错误/确认/关于 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, A11Y-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| MSG-02 | 未保存工程/场景编辑与退出确认 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| MSG-03 | 完整续训信任确认与嵌套资源设置 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| MSG-04 | 固定启动器错误提示 | NOT_RUN | FORM-01, STATE-01, LOCALE-01, PKG-01, LIFE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| EMB-01 | 初始化声明编辑器 | NOT_RUN | FORM-01, STATE-01, LOCALE-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |
| EMB-02 | 初始化几何预览 | NOT_RUN | FORM-01, FONT-01, MODEL-02, OVERLAY-01 | 仅E-INV/E-COVER静态映射；新P1/P2运行待验 |

## 6. 在途run与交付，不抢其他agent工作

以下状态来自父任务交接，**不是本台账重新查询的实时状态**：

- [诊断run40656943272](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40656943272)：ffb8eb2，交接时RUNNING；本表分类NOT_RUN，结果由其他agent导入。
- [foundation run40656947039](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40656947039)：ffb8eb2，交接时RUNNING；本表分类NOT_RUN。即使基础contracts通过，也不自动关闭全部66表面、性能或完整云端。

新产品包尚未交付。旧managed安装 `87a797bd2fe5072df1a72e5587414841f62ee494` / `v1-87a797bd2fe5-45e9cb901ee6`，仅是CI_DELIVERY_BASELINE在 **2026-10-28T05:13:28Z** 的历史记录；可能已经漂移。本次没有再读 `current.json`、快捷方式、运行进程或安装收据，也没有发布、启动、关闭、保存或迁移用户会话。后续交付须重新绑定当前接受源码、EXE/Setup、清单/许可及publisher/current/shortcut独立读回。

## 7. 仍开放的明确门槛

- **G-01 FAIL** (NAV-02, NAV-03, LOCALE-01, LIFE-01) — Close five native-crash cases with actual current-source logs/first-fault evidence and full real input/lifecycle rerun;root cause still unknown.
- **G-02 NOT_RUN** (DPI-01, NAV-03) — Five controlled Qt DPR values were measured with native OS window DPI96;real OS scaling and cross-monitor transitions require distinct evidence.
- **G-03 NOT_RUN** (NAV-01, FONT-01, ICON-01, GIZMO-01) — Original-size visual review beyond one retained minimal Chinese case, all affected orientations/distances/themes and relevant states.
- **G-04 UNSUPPORTED** (THEME-01) — Arbitrary user RGB/alpha background has no reachable baseline control/setter/key;Root decision/capability gate remains open.
- **G-05 NOT_RUN** (FONT-01, FORM-01, LOCALE-01) — All16Dialogs and all custom-painter font roles need actual three-language evidence;QWidget-default mixed-script probes do not cover them.
- **G-06 NOT_RUN** (SHELL-01, TREE-01, ICON-01, A11Y-01) — Real wide/narrow action reachability, overflow, keyboard focus/geometry, disabled reasons, tree eyes/selection/resize;new public contracts have no imported current result.
- **G-07 NOT_RUN** (GIZMO-01, GIZMO-02, MODEL-01, MODEL-02, OVERLAY-01, GRID-01) — Four real manipulation modes, numeric/constraint/cancel/undo, all five renderers, multi-object transforms/units/source files and overlays remain separate.
- **G-08 NOT_RUN** (FORM-01, STATE-01, LIFE-01) — P2 ordinary/inline/progress/message/file dialogs, public getters, actual cancellation/reconnect and persistent queue boundaries. MainWindow nested resume trust prompt and native OS providers are not covered by standalone guard captures.
- **G-09 FAIL** (PERF-01, PERF-02, FPS-01) — Repair producer equally in A/B without relaxing budgets;obtain valid3-round timing/noise/idle/resource result. Existing FAIL is invalid evidence,not proof of slow performance or nonzero idle rendering.
- **G-10 NOT_RUN** (PERF-01, PERF-02) — Cache/resource lifetime after all toggles/destruction, actual driver VRAM/GPU timing and user's NVIDIA environment;requested bytes are not residency.
- **G-11 NOT_RUN** (PORT-01, PKG-01) — Current candidate's complete source/license/resource/EXE/Setup manifest binding and explicit modification/NOTICE review.
- **G-12 NOT_RUN** (DELIVER-01) — New source-bound managed publication and independent pointer/exe/shortcut/Unicode readback while retaining user sessions/rollback. Historical87a snapshot may have drifted.
- **G-13 NOT_RUN** (CLOUD-01) — Exact-current-source full cloud checks independently from focused diagnostics, foundation, old87a full CI and local delivery.
- **G-14 NOT_RUN** (AUDIT-01) — 66/66 final per-surface acceptance or explicit evidence-backed limitation;static entry registration alone does not close the task.

技术能力/环境限制不能被静默改成“不适用”：custom RGB/alpha背景缺可达设置；当前单屏软件GL环境没有真实多屏或用户NVIDIA证据；native OS文件提供器无owned QWidget时不在现有自动surface adapter观测范围；widget-default glyph/font不包含custom-painter字体；极端close-up超过8MiB/4096px tile限时使用原向量fallback，不承诺所有距离都走新tile。

## 8. 证据索引与文件绑定

| 证据ID | 类型 | 保留路径 | 使用边界 |
|---|---|---|---|
| E-BASE | historical-source-cloud-install-audit | `docs/ui_upgrade_20261028/CI_DELIVERY_BASELINE.md` | 5183926 start identity and historical 87a797b cloud/install snapshot; not current installation freshness or current-source cloud acceptance |
| E-INV | static-surface-inventory | `docs/ui_upgrade_20261028/SURFACE_INVENTORY.md`<br>`docs/ui_upgrade_20261028/SURFACE_INVENTORY.json` | 66 surfaces / 16 Dialog classes / 147 popup callsites at5183926; source owners/line numbers belong to that baseline |
| E-COVER | static-existing-test-map | `docs/ui_upgrade_20261028/EXISTING_TEST_COVERAGE.md` | Earlier4f1dbb8 source inspection of24 test families; no test was run by that audit. New opt-in form adapters are described by E-P1, not missing merely because this old map predates them. |
| E-AUDIT | static-render-font-icon-audit | `docs/ui_upgrade_20261028/RENDER_FONT_ICON_AUDIT.md` | Rendering paths, font-role requests and67 semantic icon IDs/63 SVG files; not actual font resolution for every surface |
| E-REF | source-license-reference-index | `docs/ui_upgrade_20261028/RENDER_REFERENCES.json` | Pinned Blender4.5/Lucide references and notices; not a Blender5.2.3 source-port or final package-license signoff |
| E-BPIX | completed-baseline-pixel-evidence | `docs/ui_upgrade_20261028/P0_BASELINE_RESULTS.md`<br>`docs/ui_upgrade_20261028/P0_BASELINE_RESULTS.json`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307` | 20 zh_CN controlled-Qt-DPR empty-window cases;14 case/39 ring failures |
| E-CPIX | completed-candidate-pixel-subgate | `D:/Codex/validation/gsw-ui-upgrade-20261028/p0-candidate-run40649920338/source-run.json`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/p0-candidate-run40649920338/edge-results.json`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/p0-candidate-run40649920338/protocol.json` | Chinese20cases/60rings and frozen edge metrics only; not overall run, locale, interaction, performance or current ffb8eb2 |
| E-P0DOC | split-candidate-results-review | `docs/ui_upgrade_20261028/P0_CANDIDATE_RESULTS.md` | Normal-size d1-s0-light review and explicit split between pixel PASS and runtime/performance FAIL |
| E-P0IMPL | implementation-provenance | `docs/ui_upgrade_20261028/P0_IMPLEMENTATION.md` | GSW-native Qt coverage adapter, not copied Blender GPU AA; source bounds/fallback are implementation facts, not measured driver residency |
| E-NAVFAIL | completed-candidate-navigation-lifecycle-failure | `D:/Codex/validation/gsw-ui-upgrade-20261028/p0-candidate-run40649920338/navigation.log`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/p0-candidate-run40649920338-failure.log` | 9 CTest cases,5 native crashes. First lifecycle7.68s SegFault;EN/JP early crashes;not timeout;root cause unknown |
| E-PERF | completed-paired-performance-evidence-contract-failure | `D:/Codex/validation/gsw-ui-upgrade-20261028/performance-run40649923711/sources.json`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/performance-run40649923711/manifest.json`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/performance-run40649923711/results.json` | 24 raw process receipts;18 exit0,6 exit3/exposure-ineligible;idle timing contract failures;comparisons empty. Not a measured performance-regression verdict |
| E-PERFPLAN | frozen-performance-protocol | `docs/ui_upgrade_20261028/PERFORMANCE_PLAN.md`<br>`docs/ui_upgrade_20261028/PERFORMANCE_ACCEPTANCE.json` | Three fresh-process paired rounds, unchanged budgets;no timing/quality conclusion from plan existence |
| E-P1 | static-implementation-and-pending-gates | `docs/ui_upgrade_20261028/P1_IMPLEMENTATION.md` | Shared styles/icons, inspector, four gizmo modes, public contracts, evidence adapter and opt-in form modes implemented;runtime acceptance pending |
| E-COMPILE | local-static-compile-catalog-record | `D:/Codex/validation/gsw-ui-upgrade-20261028/compile-v6-diagnostics.log`<br>`D:/Codex/validation/gsw-ui-upgrade-20261028/compile-v7-final-harness.log` | Compiler/linker and three-language catalog record only. No local tests/apps;not an exact source-bound product package or remote whole-product pass |
| E-PENDING | delegated-runs-not-observed-here | 见第6节run链接；不读取运行结果 | Run IDs40656943272/40656947039 were RUNNING in parent handoff;other agents own observation. No polling/download/current results imported into this ledger. |

JSON保留26个输入文件的实际byte length / SHA256。文件绑定是本次只读内容快照，不改变原始日志、图像或计划；原始大artifact、PNG和用户文件均未下载/改写。前期来源审计、已有测试地图或旧安装快照只在其各自commit/时间/scope有效。后续收到匹配结果时应增补新source/run/证据，并保留旧FAIL与旧范围化PASS，不覆盖成一个总PASS。
