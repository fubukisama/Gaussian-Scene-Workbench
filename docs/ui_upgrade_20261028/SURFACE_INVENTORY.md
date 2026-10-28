# Native UI surface inventory / 原生界面全量入口清单

`2026-10-28` · baseline `5183926e09f5b568379bb1f89ee1667a9bd38121` · **仅静态登记，不是视觉或行为通过报告**。

本清单由源码入口和实际构造点建立。登记的 **66 个表面组**覆盖 **16 个独立 QDialog 类、4 个 MainWindow 内联 QDialog、4 个 QProgressDialog 入口**；另逐行索引 **37 个 QFileDialog、83 个 QMessageBox 调用点，以及 1 个固定启动器 Win32 MessageBoxW**，共 **147 个弹窗构造/调用点**。源码调用点不是唯一运行窗口数：循环按钮、模式和错误分支需分别触发。

配套最低矩阵有 **29 个 ID**，包括 `A11Y-01`。不能用只匹配字母的正则漏掉这个 ID。

机器可读、完整风险/测试要求/37 个源码文件哈希及全部调用点见 [SURFACE_INVENTORY.json](SURFACE_INVENTORY.json)。所有表面初始 `visual_status=NOT_RUN` / `interaction_status=NOT_RUN`；本 worker 没有运行任何应用、测试、窗口或训练，也未读取用户模型或设置。没有匹配的实际画面，**不把任何区域标成“已检查无需修改”**。

## 当前能力差异：背景设置不能臆造

当前源码只提供“视图 → 外观 → 自动/浅色/深色”，消费的持久 key 是 `ui/theme`。`NativeViewport.cpp:123/151/211` 的背景/clear alpha按主题固定：浅色为白色 alpha=1，夜间 clear 为 `(0.047,0.051,0.055,1)`。未找到可达的自定义颜色/透明度界面、`QColorDialog`、背景 setter 或被消费的背景颜色 key。

夜间 `ViewportColors.backdrop` 的8位结构值为 `(12,15,17)`，并不等于保留的实际 clear 浮点值（约 `(12,13,14)`）。图像/FBO元数据与对比底色须取真实清底/合成结果，不能仅抄这个结构值；这条静态差异不是毛边根因结论。

因此 `THEME-01` 的 custom-background 部分是**需要 root 处理的能力缺口**，不是“既有功能已保留/已验收”。此结论只涉及当前源码，不推断用户私有旧设置，也不授权改变背景或强制 Blender 灰。背景 API/设置新增若实施，需另补真实背景采样与状态保持证据。

## 共享样式切口（先复用，勿散落补丁）

| 切口 | 当前源码事实 | 推荐边界 / 尚需证据 |
| --- | --- | --- |
| AppTheme | 应用字体、QSS、scaled尺寸、dock标题度量已集中 | 先测每类控件实际字体/回退与DPR，再统一字号/行高/间距角色；不靠加粗或换字体猜修复 |
| WorkbenchIcons | Lucide SVG、palette态与QIconEngine DPR pixmap共用；缓存颜色化renderer | widget与QPainter入口统一；保留原资产/许可，逐态接触表和缓存/资源证据仍需远端 |
| WindowUi | 主窗、QDialog、QFileDialog、浮动dock的系统按钮/全屏策略 | 不重建可见平台窗口；文件名/选择/目录/取消及生命周期原样 |
| 表单布局 | 各 Dialog 各自设置QFormLayout/QScrollArea、边距、spacing和minimum size | 可提取真实重复的表单壳/间距角色，特殊splitter/table保留专有布局；不能删参数或改精度 |
| NativeViewport | 自绘HUD/工具条/导航，以及底部两个QWidget控件 | 底部状态控件是AppTheme以外的局部stylesheet入口；区分真实GL画面、自绘QPainter与普通控件证据 |
| MultiItemList | 多表格统一局部快捷键/菜单、批量移除确认与进度 | 焦点/删除只作用于列表，不能透传视口；冷门移除/部分失败流程同样登记 |

## 表面清单

以下每行只表明入口与实现位置已静态定位。统一状态 `STATIC_INVENTORIED / 视觉NOT_RUN / 交互NOT_RUN`，后续由 root 随远端证据逐项更新；代码存在或历史结果不能直接关闭矩阵。

### 共享基础

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| INF-01 · 应用字体、密度与控件状态 | 应用启动、主题/语言/比例变化 | `native/src/AppTheme.cpp:300`<br>`native/src/AppTheme.cpp:326` | 按语言选择字体家族并以point-size缩放；实际QFontInfo/字形回退、基线和分数DPR需远端测量。；QSS已有统一菜单、工具栏、输入、列表、dock、status、tooltip规则；不能因存在规则计视觉PASS。 | `FONT-01` `ICON-01` `SHELL-01` `FORM-01` `LOCALE-01` `A11Y-01` |
| INF-02 · 系统标题栏与窗口生命周期 | 主窗口、独立/浮动dock、全部QDialog与QFileDialog | `native/src/WindowUi.cpp:34`<br>`native/src/WindowUi.cpp:359` | WindowUi统一最小化/最大化/全屏与文件窗口初始化；popup/tooltip不按顶层窗口装饰。；改变样式不应重建可见平台窗口或提前结束modal循环。 | `SHELL-01` `FORM-01` `LIFE-01` `LOCALE-01` |
| INF-03 · 语义SVG图标与状态 | 所有workbenchIcon/paintWorkbenchIcon入口 | `native/src/WorkbenchIcons.cpp:15`<br>`native/src/WorkbenchIcons.cpp:70`<br>`native/src/WorkbenchIcons.h:12` | 共用SVG绘制与按逻辑尺寸/DPR生成QPixmap；颜色化QSvgRenderer缓存按图标/颜色。；全量入口状态接触表、实际physical size及边缘仍NOT_RUN。 | `ICON-01` `DPI-01` `A11Y-01` `PORT-01` `PERF-02` |
| INF-04 · 多项列表快捷键/工具条/批量移除 | 项目/任务及历史、质量、尺度、批量等表格的MultiItemList | `native/src/MultiItemList.cpp:1`<br>`native/src/MultiItemList.cpp:144`<br>`native/src/MultiItemList.h:13` | 共享全选/清空/移除、局部Delete合同；移除确认和进度入口纳入弹窗索引。；不得将列表快捷键透传成视口删除，不能为造型移除取消或错误保留语义。 | `TREE-01` `FORM-01` `STATE-01` `A11Y-01` `LIFE-01` |
| INF-05 · 文件选择器与常用位置/历史 | 全部37个QFileDialog调用点，详见popup_call_sites | `native/src/WindowUi.cpp:87`<br>`native/src/FileDialogHistory.cpp:180`<br>`native/src/FileDialogHistory.cpp:428` | 文件/目录类型、原生状态、最初宽度和历史左栏由共用层维护。；同一源回调可生成多个选择按钮，需按父表单目的分别覆盖，不能把37源码点误称37唯一运行窗口。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| INF-06 · 右键菜单、tooltip及键盘焦点 | 主菜单、项目树/任务表、MultiItemList、视口悬停提示 | `native/src/MainWindow.cpp:2455`<br>`native/src/MainWindow.cpp:2639`<br>`native/src/MultiItemList.cpp:1`<br>`native/src/NativeViewport.cpp:2322`<br>`native/src/NativeViewport.cpp:7799` | 存在树/任务/共享列表右键菜单及视口动态tooltip；未发现NativeViewport独立QMenu，不能虚构其现有入口。 | `SHELL-01` `A11Y-01` `LOCALE-01` |

### 主窗口与停靠区域

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| SH-01 · 主菜单及所有子菜单 | 文件/编辑/工作流/场景/视图/帮助；清理、语言、外观、资源、渲染/SH、基准、比例、窗口分辨率 | `native/src/MainWindow.cpp:1919` | 6个主菜单和子菜单复用动作；应统一行高/图标/快捷键但保持原触发合同。 | `SHELL-01` `A11Y-01` `LOCALE-01` `STATE-01` |
| SH-02 · 第一行主工具栏 | mainToolbar：新建/打开/保存/导出/媒体/模型导入/重建/训练/几何/暂停/续训/停止 | `native/src/MainWindow.cpp:2234` | 混合IconOnly和TextBesideIcon，固定不可移动；长语言与窄窗口需查溢出/关键入口可达。 | `SHELL-01` `ICON-01` `A11Y-01` `LOCALE-01` |
| SH-03 · 第二行渲染/预览工具栏 | renderToolbar：高斯/网格/点云、SH菜单、相机轨迹、任务预览/暂停预览 | `native/src/MainWindow.cpp:2261` | 按场景状态显示；渲染模式与预览控制必须保留真实后端状态。 | `SHELL-01` `ICON-01` `MODEL-02` `STATE-01` |
| SH-04 · 第二行选择工具栏 | selectionToolbar：查看/锁定/轨迹球/查找、框选/套索/笔刷/仅可见、半径 | `native/src/MainWindow.cpp:2289` | 状态动作保留文字，其他用图形/tooltip；半径spin有86..110逻辑宽度与px单位。 | `SHELL-01` `ICON-01` `A11Y-01` `GIZMO-02` `LOCALE-01` |
| SH-05 · 第二行编辑操作工具栏 | editToolbar：清选/反选/删除/撤销/重做/裁剪导出 | `native/src/MainWindow.cpp:2328` | 动作可用性与当前编辑状态相关；视觉调整不能改变删除/撤销合同。 | `SHELL-01` `ICON-01` `GIZMO-02` `STATE-01` |
| SH-06 · Dock标题、浮动/关闭按钮和分隔拖动区 | 工程、属性、任务与日志3个dock | `native/src/MainWindow.cpp:255`<br>`native/src/MainWindow.cpp:420`<br>`native/src/AppTheme.cpp:430` | 自绘标题配合工具按钮；标题字体pixel-size角色与全局point-size需实际字体/DPR核对。；保留Qt dock拖拽、浮动和重停靠。 | `SHELL-01` `FONT-01` `ICON-01` `LIFE-01` |
| SH-07 · 工程树及眼睛可见性列 | projectTree/SceneTreeWidget/SceneVisibilityDelegate | `native/src/MainWindow.cpp:143`<br>`native/src/MainWindow.cpp:195`<br>`native/src/MainWindow.cpp:2344`<br>`native/src/MainWindow.cpp:7724` | 两列、固定眼睛列、ExtendedSelection；隐藏与transform selection有独立合同。；空白区边线是否存在必须看远端画面，源码扫描不能判缺陷已修。 | `TREE-01` `MODEL-01` `A11Y-01` `LOCALE-01` |
| SH-08 · 工程树右键菜单 | dataset/reconstruction/scene/task树节点 | `native/src/MainWindow.cpp:2440` | 菜单随节点类型生成；切换当前行时须保留source identity与visibility。 | `TREE-01` `MODEL-01` `A11Y-01` |
| SH-09 · 属性面板：工程/数据集 | inspectorPanel：名称、路径、图像、相机/尺度记录 | `native/src/MainWindow.cpp:2479`<br>`native/src/MainWindow.cpp:436`<br>`native/src/MainWindow.cpp:7957` | 只读QLabel可选择且word-wrap；局部边距12/4/12/12、spacing8、行间距7单独写定。；长路径/大数值与新统一令牌的迁移候选，尚非已证明裁切。 | `SHELL-01` `FONT-01` `FORM-01` `LOCALE-01` |
| SH-10 · 属性面板：场景几何/变换/单位 | 文件、数量、格式、相机、坐标关系、单位、中心/尺寸/范围、显示/模型变换、基准面 | `native/src/MainWindow.cpp:2530`<br>`native/src/MainWindow.cpp:7957` | 大量只读动态值，不应因重新排版截断精度或把显示换算写入几何。 | `SHELL-01` `FORM-01` `GRID-01` `MODEL-01` `LOCALE-01` |
| SH-11 · 任务表、工具条及右键菜单 | taskDock → 任务页/双击详情/复制摘要/打开目录/移除记录 | `native/src/MainWindow.cpp:2593`<br>`native/src/MainWindow.cpp:2639`<br>`native/src/MainWindow.cpp:2649` | 动作来自保留TaskRecord；外部原始日志不翻译。 | `SHELL-01` `STATE-01` `A11Y-01` `LIFE-01` |
| SH-12 · 共享训练监控及曲线 | 任务区训练监控页；ExistingTrainingDialog内嵌监控 | `native/src/TrainingMonitorWidget.cpp:139`<br>`native/src/TrainingMonitorWidget.cpp:280` | 7指标按宽度换行、内容滚动；标题加粗与曲线缩小字体是局部字体角色。；重构只改变呈现，不能重置指标、假进度或缩小图表到不可读。 | `SHELL-01` `FONT-01` `STATE-01` `LOCALE-01` `LIFE-01` `FPS-01` |
| SH-13 · 日志页及跟随/导出工具栏 | taskDock → 日志页 | `native/src/MainWindow.cpp:2670`<br>`native/src/MainWindow.cpp:8129`<br>`native/src/AppTheme.cpp:472` | 日志字体使用Cascadia Mono/Consolas角色；用户文件名/外部原始日志需原样保留。 | `SHELL-01` `FONT-01` `STATE-01` `LOCALE-01` |
| SH-14 · 系统状态栏与临时消息 | project status、renderer status、edit status、ui scale status | `native/src/MainWindow.cpp:2706`<br>`native/src/MainWindow.cpp:2725`<br>`native/src/MainWindow.cpp:4265` | renderer动态状态不得被重译为空场景默认文本；底色需按状态角色检查。 | `SHELL-01` `STATE-01` `FPS-01` `LOCALE-01` |
| SH-15 · 拖放反馈与导入摘要 | 主窗口/工程树/视口文件拖放 | `native/src/MainWindow.cpp:989`<br>`native/src/MainWindow.cpp:1089` | 反馈是copy-only分类合同，不应为了UI统一开始导入/训练或上传私有输入。 | `OVERLAY-01` `STATE-01` `LOCALE-01` `MODEL-01` |
| SH-16 · 界面设置/布局恢复入口 | 视图 → 外观、显示与适配、dock显示、恢复布局；F11/最大化 | `native/src/MainWindow.cpp:2020`<br>`native/src/MainWindow.cpp:3982`<br>`native/src/MainWindow.cpp:4015`<br>`native/src/MainWindow.cpp:4168` | 手动与自动比例/日夜主题、窗口几何和dock状态持久化；不能重置工程/训练偏好。 | `SHELL-01` `DPI-01` `LOCALE-01` `LIFE-01` |

### 全部视口显示/交互叠加层

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| VP-01 · 真实模型渲染区域 | 点云/网格/3DGS/2DGS/训练预览 | `native/src/NativeViewport.cpp:3835`<br>`native/src/NativeViewport.cpp:5887`<br>`native/src/NativeViewport.cpp:5965`<br>`native/src/NativeViewport.cpp:6096` | UI修改不得降低模型质量或只改模式标签；原renderer身份与GL状态需实际记录。 | `MODEL-02` `PERF-01` `PERF-02` `THEME-01` |
| VP-02 · 导航朝向轴圆盘/圆环/标签 | 右上navigation gizmo轴端/轨道命中区 | `native/src/NavigationGizmo.cpp:1`<br>`native/src/BlenderNavigationPresentation.h:87`<br>`native/src/NativeViewport.cpp:8930` | 用户明确报毛边；DPR/FBO/采样/合成成因仍待真实绘制链测量。；不能以setRenderHint或请求samples声明代替实际边缘证据。 | `NAV-01` `NAV-02` `NAV-03` `DPI-01` `THEME-01` `FONT-01` `PERF-01` |
| VP-03 · 导航小按钮与投影提示 | 相机/投影、平移、缩放、projectionLabel | `native/src/BlenderNavigationPresentation.h:169`<br>`native/src/NavigationGizmo.cpp:1`<br>`native/src/NativeViewport.cpp:6646` | 必须统一视觉与逻辑命中区域，保留缩放到光标/投影/相机约定。 | `NAV-02` `NAV-03` `ICON-01` `A11Y-01` |
| VP-04 · 模型变换器：全部四种模式 | Move/Rotate/Scale/Combined；G/R/RR/S/轴与平面/数值输入 | `native/src/TransformGizmoRenderer.cpp:1`<br>`native/src/TransformGizmo.cpp:1`<br>`native/src/NativeViewport.cpp:8584`<br>`native/src/NativeViewport.cpp:7823` | 杆/锥/端帽、前后遮挡、中心自由移动需逐模式验证；不能把导航轴或3D cursor当模型变换器。 | `GIZMO-01` `GIZMO-02` `MODEL-01` `DPI-01` `PERF-01` |
| VP-05 · 视口内模型编辑工具条 | 左侧Move/Rotate/Scale/Transform及Global/Local/锁定提示 | `native/src/NativeViewport.cpp:8600` | 独立于MainWindow四个QToolBar；自绘卡片/图标/锁定badge需与命中一致。 | `GIZMO-01` `ICON-01` `A11Y-01` `LOCALE-01` |
| VP-06 · 观察轨迹球 | 观察轨迹球开关与相机环绕 | `native/src/NativeViewport.cpp:2337` | 只变相机不变模型；描边与alpha需在自定义背景检查。 | `NAV-02` `OVERLAY-01` `THEME-01` |
| VP-07 · 模型选择轮廓/包围框/轴线 | 已选/非当前/隐藏对象选择反馈 | `native/src/NativeViewport.cpp:8389`<br>`native/src/NativeViewport.cpp:8530` | 保留深度遮挡与source选择语义，不能把普通选择画成破坏模型的点阵。 | `OVERLAY-01` `MODEL-01` `GIZMO-02` |
| VP-08 · 屏幕选择手势与笔刷半径 | 框选/套索/笔刷 | `native/src/NativeViewport.cpp:8693` | 真实屏幕半径与input逻辑坐标要同一DPR合同；仅可见点过滤不变。 | `OVERLAY-01` `GIZMO-02` `DPI-01` |
| VP-09 · 相机轨迹/相机线框 | 显示相机开关、源相机与任务预览 | `native/src/NativeViewport.cpp:8477`<br>`native/src/CameraTrajectory.cpp:1` | 图形符号长度不是设备物理尺寸；UI升级不改locked相机/full-K/坐标。 | `OVERLAY-01` `MODEL-02` `GRID-01` |
| VP-10 · 网格/基准轴/标尺 | 自动/固定网格与坐标基准 | `native/src/NativeViewport.cpp:6163`<br>`native/src/ViewportPresentation.h:1`<br>`native/src/ViewportGridSettings.h:1` | 已有自动/固定、单位/密度合同；不为视觉改变几何单位、参考面或训练尺度。 | `GRID-01` `THEME-01` `MODEL-01` `PERF-01` |
| VP-11 · 常驻场景标题与计数/模式字幕 | 视口左上caption | `native/src/NativeViewport.cpp:8746` | 自绘字幕已去常驻卡框；动态名/数量/readonly/texture等分支须全部实际覆盖。 | `OVERLAY-01` `FONT-01` `LOCALE-01` `STATE-01` |
| VP-12 · 处理/训练预览阶段面板 | 重建、加载和训练结果过渡中的processing HUD | `native/src/NativeViewport.cpp:8906` | 进度/状态来源必须真实，不能固定动画伪造工作；不要遮主体或标题。 | `OVERLAY-01` `STATE-01` `FPS-01` `LOCALE-01` |
| VP-13 · 底部简洁帧状态与详情按钮 | viewportStatusLabel + viewportDetailsButton | `native/src/NativeViewport.cpp:541`<br>`native/src/NativeViewport.cpp:4660`<br>`native/src/FrameRateCounter.cpp:1` | 这是仅有的AppTheme之外局部stylesheet入口族；控件透明/hover及文字/宽度按实际状态刷新。；常驻状态不要回填完整诊断；新帧/空闲/测量中不混淆。 | `FPS-01` `SHELL-01` `LOCALE-01` `PERF-02` |
| VP-14 · 空/加载/失败/全部隐藏状态 | NativeViewport drawOverlay/frame policy | `native/src/NativeViewport.cpp:3710`<br>`native/src/NativeViewport.cpp:8746` | 空场景和有数据但全隐藏不同；不能用原source数量假称可见rendering。 | `MODEL-01` `MODEL-02` `STATE-01` `FPS-01` |

### 全部独立原生对话框

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| DLG-01 · DatasetImportDialog | 文件/工作流 → 导入图像/视频/文件夹/已知相机数据集；拖放媒体 | `native/src/MainWindow.cpp:5855`<br>`native/src/DatasetImportDialog.cpp:193` | 来源多项列表、SourceActions、名称、相机输入、InitializationEditor、导入/取消；工作流/媒体入口共用同一类；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-02 · ReconstructionDialog | 工作流/主工具栏 → 重建 | `native/src/MainWindow.cpp:6405`<br>`native/src/ReconstructionDialog.cpp:30` | COLMAP预设/完整参数、滚动表单、说明/开始/取消；界面改造不实际重建；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` |
| DLG-03 · TrainingDialog | 工作流/主工具栏 → 训练；实验参数重开/续训入口的独立确认另列 | `native/src/MainWindow.cpp:7310`<br>`native/src/TrainingDialog.cpp:152` | 3DGS/2DGS、预设、实验质量、显存保护/执行、输出名、相机来源、初始化；共享滚动表单和长状态文案；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-04 · MeshGenerationDialog | 工作流/主工具栏 → 生成几何 | `native/src/MainWindow.cpp:7219`<br>`native/src/MeshGenerationDialog.cpp:29` | 2DGS bounded/unbounded TSDF、SuGaR、GS2Mesh、GS depth；参数/单位/模式说明不能混译或改请求；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` |
| DLG-05 · ModelExportDialog | 文件/场景/主工具栏 → 导出模型 | `native/src/MainWindow.cpp:5388`<br>`native/src/ModelExportDialog.cpp:21` | PLY/SPZ/网格可用性、SH/坐标/精度、输出选择与取消；取消/源属性保留；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `MODEL-02` |
| DLG-06 · TaskDetailsDialog | 任务表双击/详情动作/任务右键 | `native/src/MainWindow.cpp:2909`<br>`native/src/TaskDetailsDialog.cpp:54` | 摘要滚动、任务操作工具条、跟随/保留日志；视窗关闭不停止任务；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-07 · GenerationHistoryDialog | 工作流 → 实验历史；相应训练结果入口 | `native/src/MainWindow.cpp:6904`<br>`native/src/GenerationHistoryDialog.cpp:47` | 实验列表/详情/参数/查看成果/续训状态，显示真实可用性；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-08 · QualityComparisonDialog | 工作流 → 质量对照 | `native/src/MainWindow.cpp:6537`<br>`native/src/QualityComparisonDialog.cpp:83` | 基准/候选、阈值表、比较/保存、报告/设置差异、实测子窗入口；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-09 · QualityMeasurementDialog | 质量对照 → 主动测量图像 | `native/src/QualityComparisonDialog.cpp:366`<br>`native/src/QualityMeasurementDialog.cpp:122` | 三种目录输入reference/render/mask、清单、配对/进度/视图表、选择为基准/候选；CPU helper生命周期；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-10 · ScaleConstraintsDialog | 工作流 → 相机数据集定尺度与对齐；工作流 → 当前模型定尺度 | `native/src/MainWindow.cpp:6543`<br>`native/src/ScaleConstraintsDialog.cpp:166` | 两个source模式；距离/点/平面标签页、约束/结果JSON、估计/独立版本、精度与单位原样；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` `GRID-01` |
| DLG-11 · InitializationDialog | 工作流 → 相机数据集初始化与检查；训练窗口 → 初始化 | `native/src/MainWindow.cpp:6696`<br>`native/src/TrainingDialog.cpp:1102`<br>`native/src/InitializationDialog.cpp:145` | 左右splitter、InitializationEditor与InitializationPreview、覆盖表/报告、选择视图、创建版本；两入口合同都保留；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` `MODEL-02` |
| DLG-12 · TrainingBatchDialog | 工作流 → 批量训练 | `native/src/MainWindow.cpp:6728`<br>`native/src/TrainingBatchDialog.cpp:118` | 数据集/参数轴/基础JSON、展开预览、保存历史、启动/续训/观察；关闭planner不误停持久queue；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-13 · ExistingTrainingDialog | 工作流 → 现有训练；批量计划 → 观察队列 | `native/src/MainWindow.cpp:6828`<br>`native/src/MainWindow.cpp:6808`<br>`native/src/ExistingTrainingDialog.cpp:80` | 两个观察模式、store浏览/任务选择、连接/断开/控制、共享监控/详情；断开窗不终止worker；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-14 · RuntimeGuardDialog | 继续训练信任确认 → 续训显存保护设置 | `native/src/MainWindow.cpp:7060`<br>`native/src/RuntimeGuardDialog.cpp:17` | 嵌套modal；enabled/mode/usage/spare/denials、说明/错误/OK/Cancel；原guard未知字段保留；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| DLG-15 · ResourceBudgetDialog | 视图 → 资源预算 | `native/src/MainWindow.cpp:2112`<br>`native/src/ResourceBudgetDialog.cpp:50` | 模式/内存显存预算与实时状态、应用/关闭；UI修改不触碰训练策略；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `PERF-02` |
| DLG-16 · ViewportGridDialog | 视图 → 网格设置；视口详情 → 网格设置 | `native/src/MainWindow.cpp:2823`<br>`native/src/ViewportGridDialog.cpp:105` | 显示/轴线、自动/固定间距、subdivision/density/display unit/provenance、应用/取消；各表单布局/最小尺寸由本类分别定义；现有AppTheme/WindowUi只能证明共用样式入口，不能证明本轮无裁切。 | `FORM-01` `STATE-01` `LOCALE-01` `GRID-01` |

### MainWindow 内联对话框

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| INL-01 · 视口详情 | 视图/视口信息按钮 → 视口详情 | `native/src/MainWindow.cpp:2755` | 只读详情/复制/网格设置/关闭，WA_DeleteOnClose和快速重开目标清理；MainWindow内联构建而非独立Dialog类，不能因16个类已覆盖就漏掉。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| INL-02 · 恢复中心 | 启动恢复提示；文件 → 恢复中心 | `native/src/MainWindow.cpp:4589` | 恢复记录表、多选移除、恢复/稍后、busy/损坏/不可用提示；MainWindow内联构建而非独立Dialog类，不能因16个类已覆盖就漏掉。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| INL-03 · 版本历史 | 文件 → 版本历史 | `native/src/MainWindow.cpp:4827` | 快照表、移除、恢复/取消、来源保护；MainWindow内联构建而非独立Dialog类，不能因16个类已覆盖就漏掉。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| INL-04 · 外部备份历史 | 文件 → 外部备份；配置外部备份目录 | `native/src/MainWindow.cpp:5030` | 备份表、恢复目标目录、校验进度、结果说明；MainWindow内联构建而非独立Dialog类，不能因16个类已覆盖就漏掉。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |

### 进度对话框

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| PROG-01 · 恢复外部备份 | 外部备份历史 → 校验恢复 | `native/src/MainWindow.cpp:5116` | QProgressDialog不是独立QDialog子类；状态、取消/不可取消策略沿用原语义。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| PROG-02 · 导出模型 | ModelExportDialog接受后 | `native/src/MainWindow.cpp:5401` | QProgressDialog不是独立QDialog子类；状态、取消/不可取消策略沿用原语义。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| PROG-03 · 导入场景 | importSceneFile耗时导入 | `native/src/MainWindow.cpp:6107` | QProgressDialog不是独立QDialog子类；状态、取消/不可取消策略沿用原语义。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| PROG-04 · 批量移除记录 | MultiItemList/installRecordRemoval | `native/src/MultiItemList.cpp:168` | QProgressDialog不是独立QDialog子类；状态、取消/不可取消策略沿用原语义。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |

### 确认、错误及启动器边界

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| MSG-01 · 所有信息/警告/错误/确认/关于 | 83个QMessageBox源码调用点（逐点索引，动态目的另按父窗覆盖） | `native/src/MainWindow.cpp:8155`<br>`native/src/FileDialogHistory.cpp:221`<br>`native/src/MultiItemList.cpp:144` | 多处短错误/不可用提示、长diagnostic/detail及默认安全按钮；不能只验证showError通用入口就把全部流程计PASS。 | `FORM-01` `STATE-01` `LOCALE-01` `A11Y-01` `LIFE-01` |
| MSG-02 · 未保存工程/场景编辑与退出确认 | 关闭/新建/打开/切换源时confirmDiscardChanges/SceneEdits/confirmExit | `native/src/MainWindow.cpp:4390`<br>`native/src/MainWindow.cpp:4411`<br>`native/src/MainWindow.cpp:4440` | 独立于一般错误：Save/Discard/Cancel和退出确认不可因主题/布局改变实际选择结果。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| MSG-03 · 完整续训信任确认与嵌套资源设置 | 实验历史/暂停恢复 → resumeTrainingJob | `native/src/MainWindow.cpp:7037`<br>`native/src/MainWindow.cpp:7060` | 检查点信任提示、Yes/No安全默认与guard action循环；原参数/完整状态身份不得被UI重排改写。 | `FORM-01` `STATE-01` `LOCALE-01` `LIFE-01` |
| MSG-04 · 固定启动器错误提示 | 启动解析失败时Win32 MessageBoxW；非Windowsstderr | `native/src/NativeLauncher.cpp:71` | 这不是Qt QMessageBox，AppTheme样式不直接作用；登记为跨adapter边界，不为外观替换稳定启动协议。 | `FORM-01` `STATE-01` `LOCALE-01` `PKG-01` `LIFE-01` |

### 内嵌表单/预览

| ID / 表面 | 入口 | 实际实现 owner | 静态观察与待查重点 | 矩阵 |
| --- | --- | --- | --- | --- |
| EMB-01 · 初始化声明编辑器 | DatasetImportDialog及InitializationDialog | `native/src/InitializationEditor.cpp:45` | 公共输入编辑器source/frame/count/seed/prior/范围字段；更改标签或展开布局不改值、精度和输入身份。 | `FORM-01` `STATE-01` `LOCALE-01` |
| EMB-02 · 初始化几何预览 | InitializationDialog右侧预览 | `native/src/InitializationPreview.cpp:70` | 独立QWidget绘制不等于NativeViewport真实OpenGL；需分别验证字体/线宽、选择视图与纯观察导航。 | `FORM-01` `FONT-01` `MODEL-02` `OVERLAY-01` |

## 高价值迁移顺序与回归职责

1. P0先打通实际导航边缘/DPI纵切：`VP-02/03 + INF-03`，不以源码配置代替毛边测量。
2. 实际字体证据到位后统一 `INF-01 + SH-06/12 + VP-11/13`，兼顾三语/明暗/分数DPR。
3. 主窗口工具栏、工程树/眼睛列、属性长值优先：`SH-02..10`；保留选择、相机和单位语义。
4. 模型Move/Rotate/Scale/Combined及全部overlay一起验：`VP-04..10`，不能只改单项或合并球。
5. P2先共用滚动表单，再长生命周期/复杂表格与恢复/备份冷门窗；每个入口至少一次实际检查。
6. `BASE/PORT/PKG/DELIVER/CLOUD`由 root 与专属 worker 管理；本清单不重复CI或安装调查，也不冒充这些项通过。

## 使用与覆盖边界

- 37 个文件选择调用点和 83 个信息/确认调用点都在 JSON 中保留源码行、所属方法和可定位的首行。构造器 lambda 归其父方法；少量文件级helper明确标为file-scope。
- 没有发现 NativeViewport 自己的独立 QMenu；实际存在的是工程树、任务表和 MultiItemList 菜单，以及导航/编辑悬停 tooltip。不要把右键环绕相机当作一个现有视口菜单。
- 全部窗至少覆盖一次入口/特殊模式；共用样式可复用底层测试，但不能用空主窗或一个父窗截图代替所有Dialog。
- 明暗、三语、标准/窄窗口按风险组合；导航五缩放、实际分数DPR、用户背景能力和字体解析必须有真实元数据。真实跨屏未执行时仍标NOT_RUN。
- 所有错误/不支持/忙/取消状态必须由确定性owned夹具触发，不使用用户真实工程/训练作为测试负载。
- 本清单绑定上述baseline和观测文件SHA。后续生产源码或入口变化应更新绑定/源行；不回写旧证据，不把静态登记变成验收PASS。
