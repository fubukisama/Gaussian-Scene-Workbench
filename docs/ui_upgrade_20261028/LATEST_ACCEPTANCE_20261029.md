# UI Upgrade — 2026-10-29 范围化验收记录

Status: **EXACT_D873_MENU_CLOUD_PACKAGE_PASS / LOCAL_UNSIGNED_PREVIEW_DELIVERED / NOT_FULL_ACCEPTANCE / NOT_PUBLIC_PRODUCTION_RELEASE**.

## 0G. 用户明确要求更新本地后：d873 本地开发预览已交付

用户在候选包收尾后明确要求“记住 更新本地”。已将这条交付偏好保存为全局记忆注记和根AGENTS规则：
软件升级任务不能只以push、CI或安装包链接代替实际本地更新；仍须遵守适用校验／授权，不擅自启动、
关闭或置顶程序，也不能伪造完整验收或扩展为超长任务。

本机状态实际刷新后确认：`D:/Apps/GSW` 已是未签名本地开发预览安装，protocol1，原活动版为
`v1-e141f382cbed-local-preview`，无 `.update-trust-anchor`／`updates/trust`。
只使用既有、明确隔离的 `local-developer-unsigned` 接纳路径：其原生代码会完整核验旧bootstrap文件集，
内部专门允许现有protocol1；Offer的 `minimumBootstrapProtocol` 仍为2，没有改小或绕过检查。
不走要求 `fullAcceptance=true` 的正式发布凭证路线，也未执行Setup、迁移bootstrap、配置生产feed或密钥。

发布工具从与d873一致的源代码在D盘单独重新编译，只链接QtCore，无应用／GUI／自动测试执行。
工具SHA256 `055990c1b97e9751f3c2633e6fd798dbd2a96c381060f33895ee5aea0e98195c`。
已验d873 payload经现有packager重新封成完整本地更新ZIP，123293303 bytes，SHA256
`a6fb85c1c93b5a49655f4010a9bb9b411b17b75a53b3fbda423367c1a19a2a95`。
原生工具真实返回 `ok=true/code=ACTIVATED/admission=local-developer-unsigned/publisherAuthenticated=false`。

实际独立回读（2026-10-30 17:55 JST）PASS：

- `current.json` 为 `v1-d87343fa2153-local-preview`；活动主EXE SHA256
  `8d61b27177ccf3e3cdf64bbb14932a00d58017036d0c689af2c2ec4d839270e8`，与精确d873候选一致。
- 桌面和开始菜单共2个已识别原生快捷方式仍指向固定启动器，链接文件字节、参数、工作目录、图标和AUMID不变；没有重写快捷方式。
- 固定bootstrap及其完整bin文件集不变；原e141与87两个旧版本保留，旧活动EXE摘要不变。
- 已安装槽静态核验3059文件（3058个payload文件加原生生成的`release-files.json`）、1351后端记录、1597源码记录、28隐藏文件；native-only边界也PASS。
- 未启动、关闭、重启或置顶GSW，没有开始／续训；设置、工程、原始照片、模型和检查点未改动。

独立凭证：`D:/Codex/validation/gsw-ui-upgrade-20261028/local-d873-update-20261030/LOCAL_DELIVERY_READBACK.json`，
SHA256 `c67d49b921a090e7a51c0d02668b030cb645110e945a4bf803ec2b56e0c450f9`；
已安装静态凭证SHA256 `5c394551d9363e101838a1292e4058e517d48469fbe6db74503c8929992bbd53`。
安装根自身另有 `updates/receipts/v1-d87343fa2153-local-preview.json`，mode=developer，SHA256
`e692a50e4dba56da10809350b1404c743fd0740b4a4488ad1baa8c70b4f33fcd`。
原候选包绑定凭证是之前未安装时的不可变快照，不改写其历史字段；本次本地交付单列此节。

这是**本地未签名预览交付**，不是发布者认证、正式生产发布、完整UI／硬件／性能验收或完整更新器首装迁移通过。
原4项性能INCONCLUSIVE、F10／全表面／NVIDIA／原生DPI等门槛和依赖告警继续保留；下次用户自行打开软件即使用新版本。

## 0F. 明确授权后的有界收尾完成：d873 菜单、完整 CI 和候选包通过

用户对上一条明确的2–4小时收尾方案回复“继续”，本轮只处理已有菜单无障碍失败、既定菜单复验
和一个最终候选的完整 CI／包核对，不扩大安全审计、输入框架或本机安装范围。

### 两个真实红绿循环，未修改原断言

官方 Qt6.8.3 的默认 MenuItem 仅提供 Name／Accelerator，不返回 Description／Help；
原 tooltip 已有三语内容，所以追加 statusTip 属性不能修复真实接口。
[Qt6.8.3 菜单源码](https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/accessible/qaccessiblemenu.cpp#L255)
和[公开接口注册文档](https://doc.qt.io/qt-6.8/qaccessible.html#registerAccessibleInterface)
共同限定了最小修复：仅为指定 leaf QAction 在插入真实 QMenu 前登记公共接口适配，
保持角色、状态、几何、父级、Qt 缓存生命周期及标准辅助激活语义。没有全局 factory、
Qt 私有 ABI、缓存替换、全局键盘输入或测试内手动 show／focus／trigger。

1. `aa09ab26ba637387f3b1049dc11492916cce2b11`／
   [41016738223](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/41016738223)：
   三语 language、appearance 实际 PASS，自动主题 Description 含06:00／18:00；
   接着真实 workflow-idle FAIL，暂停／继续没有辅助禁用原因；help 因此前失败 NOT_RUN。
   原6PASS／3FAIL／3NOT_RUN结果保留，不改成总绿。唯一 artifact12959119916（4387795 bytes）
   SHA256 `48d0e03c6b02b5d8c70aa8a1ddff49ffc42fda9f36a40087694bd0d195688ca0`，
   直接日志 SHA256 `8824e9bcf27bc555ef5934cfe255b1ccb0acafcac2a0c23ffa97160018b3f64d`。
2. 最终候选 `d87343fa215389815c6cf037aef85153a5bf6b91`、tree
   `242c92f46a967d499c08e856d543876c964d7215`，仅把同一适配用于暂停／继续，
   从已计算的运行／恢复阻塞／续训可用状态选择说明。两条 setEnabled 条件、worker、
   检查点和I/O逻辑未改；运行中或恢复阻塞不会被误写为“没有可用检查点”。新增两条说明
   同步中英日并保留 PLY≠优化器续训的语义。compile-only v24／v25 exit0，
   native_i18n 2292条 UI／2398条 catalog 全三语完整；不是本机运行验收。

### 最终候选菜单：实际3个 CTests／12条路径 PASS

[Native UI Surfaces41018104866](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/41018104866)
精确 d873 最终 SUCCESS，三语言各 language／appearance／workflow-idle／help 全部 PASS。
实际 Alt 链、名称、勾选／禁用状态、独立时间／禁用原因片段、ESC／焦点恢复及公开快照断言未放宽；
所有路径 publicStateUnchanged=true、无触发动作。

证据目录 `D:/Codex/validation/gsw-ui-upgrade-20261028/menu-final-d873-run41018104866/`；
唯一 artifact12961210483（4369437 bytes）ZIP SHA256
`b5a9e737a97d4805824d093d4aa3b3b97784529b83d9831add29734e22970eb1`；直接日志 SHA256
`ead299598dd8d5b528812880e36f87b25d3565bf5da1d70b62ba772d298686ea`。
三语 receipt SHA256：zh_CN `67311b140fb384080f42f5195338a4eebaf115f55419476d98a5ddb3f057e850`；
en_US `5e6aad95bf93c1a86f51d787f5f4a18b42cc083d0bbba5859664669ee7f9cd77`；
ja_JP `3481b2cf0f908834fb3efcda7d90f25b845dd4781172948f072d29466d4ca77e`。

这仅是合成 Qt Alt 和真实 Qt 公共无障碍接口的范围化通过，不是物理键盘／原生 OS 读屏测试、
F10、原34表单矩阵、完整视觉或全部无障碍通过。旧 F10 NOT_RUN 和旧失败仍保留。

### 同源完整 CI／安装包：实际完成，不继承旧版本

同一 d873 的唯一
[Native Windows41018107906](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/41018107906)
三个 job 最终均 SUCCESS（2026-10-30 08:33:30 UTC API确认），CodeQL实际1小时26分20秒。
CodeQL直接日志 SHA256 `14d1834ffe92b9fe02e0bc98f54ac650db22da55a2874cd0bbad497cc0fb4650`。
Qt 直接日志确认4项 bootstrap PASS（231.76秒）、175/175 CTests PASS（1583.70秒）；
实际分发包上传后回读 PASS、baselineCompared=true，3058文件／1351后端记录／1597源码记录／28隐藏文件。
远端主 EXE SHA256 `8d61b27177ccf3e3cdf64bbb14932a00d58017036d0c689af2c2ec4d839270e8`；
正式 Setup SHA256 `b2e8b960b798fb93e93ff365e9d5f0809037d30eb17316790c8ad55ef0893b5c`、NotSigned。
同源候选随后各下载一次，并完成下列本机只读核对；没有运行其中的EXE或安装器。
upstream 直接日志另确认83项前置测试、65项真实视口与6项集中合同通过。
Qt／upstream 日志分别保存为 `full-d873-run41018107906/qt-direct-job.log`（SHA256
`b19e3731aca8116cdfb55d5ff61828ca9b4583bef77d31634a0c0f2af906cafb`）及
`upstream-direct-job.log`（SHA256 `bd69ed7514ad209503adf66e2905570fa4df129fb3235c6f163399e98dad1239`）。
不重复调度相同源码，不从旧0ec／e141／7f76继承绿灯，不将旁线独立 e141 预览改标为 d873。
候选源码只读参考已归档于
`D:/Codex/validation/gsw-ui-upgrade-20261028/full-d873-run41018107906/candidate-source-reference.zip`。

| 实际下载对象 | artifact ID／bytes | 服务端 digest 与本机 ZIP SHA256 一致 |
|---|---|---|
| App及对应源码 | 12962941243／123951417 | `8aa30298aee87d6f1fe4dd7d13100df16b9c975af62125c8ed9e977a17fbbda2` |
| 正式Setup及元数据 | 12962009568／113323360 | `b6ea39671dc780a5f0c194fbf63a45e3935da81db770266771be0848b108d77e` |
| 远端实际回读凭证 | 12963010943／1025 | `7a44657c9997ea7f00b582603902c627659b296fc7854c82ca75cd517a5c8c3c` |

本机 `verify_native_artifact.py` 和 native-only backend文件边界均 PASS：3058文件、1351后端记录、
1597对应源码记录、28隐藏文件，EXE9862656 bytes，SHA与上方远端记录相同。
本机无staging baseline，所以凭证明确 `baselineCompared=false`、`checkerSourceCommit=null`；
远端真实上传／下载回读另为 `baselineCompared=true`，未混称同一次证明。
本机静态凭证 SHA256 `29ca1c7ea0cef873958b054616dec6093c53cf2cbdb480a9df42584026ac65dd`；
远端回读凭证 SHA256 `e633b7a1d4826a3e46c67a1b7e2e188844755e13fb1aae0f3e065eb42390fec6`。

正式Setup113801452 bytes，SHA与上方记录、`.sha256`及`.exe.json`完全一致；
元数据的主EXE SHA与App包相同，`testBuild/installerFixture/automaticAppLaunch/trainingRuntimesBundled`
均false；三语言为zh_CN/en_US/ja_JP，实际签名仍 **NotSigned**。
独立只读复核也确认这些字段、许可文件和新头文件。正式Setup未在本机实装，
CI安装行为测试仍使用独立Test-AppID／test Setup，不伪称两者为相同安装器。

对应源码身份不仅依赖manifest标签：真实run的head_sha/tree、三个artifact ID/digest均固定为d873，
本机还将包内 MainWindow.cpp、新 DescribedMenuAction.h、catalog.json、CMakeLists.txt
与冻结Git archive逐文件SHA256及Git blob比对，均相同。source-manifest、backend_manifest、
native-crt-manifest及COPYING／native/LICENSE.md／THIRD_PARTY_LICENSES.md已逐清单核验。

全部文件和绑定凭证位于
`D:/Codex/validation/gsw-ui-upgrade-20261028/full-d873-run41018107906/`，
最终文件凭证 `PACKAGE_BINDING_d87343f.json`。正式候选Setup位于其`setup/`子目录。
本轮运行代码已push至main的d873，后续收尾文档提交不被冒充为重新构建／运行过的源码。

上述CI／文件核验阶段未启动本机应用／自动测试／安装器，也未改 current、快捷方式、工程、照片、模型、设置或训练；
后续用户新授权的本地预览交付独立记录于第0G节。
完整66表面／29-ID、原性能 FAIL与4项 INCONCLUSIVE、硬件未验等边界保持不变；没有制造
fullAcceptance 或本机发布许可。原293项依赖安全提示未在这次 UI 收尾中审计。

## 0E. 2026-10-30 收齐已有运行；停止后台，等待明确授权

本节取代下文历史快照中的“正在运行／继续扩展探针”行动建议。此次仅收齐既有运行、
读取直接日志并归档失败证据，没有调度新 CI、改生产代码、运行本机应用或切换安装。
用户要求超长任务先获明确许可；下一轮修复及最终同源构建／包核对尚未获准，不自动继续。

### 已完成完整 CI，仅对应精确 0ec 源码

[Native Windows 40834717531](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40834717531)
的 Qt、Upstream viewport、CodeQL 三个 job 最终均 **SUCCESS**。source
`0ec86155d8e5480bb9a7b9ad23e583120c8d8944`、tree
`f67a4e0788a97eae47b0fda20084ffa3138f2eb5`；不能改标成 6609643 或 9b5518f。

直接 Qt job123316700062 日志确认4项 bootstrap PASS（298.17秒）、175/175 CTests PASS
（2202.16秒）。实际产品上传后下载回读为 PASS、baselineCompared=true：3056文件、
1351后端记录、1595源码记录、28隐藏文件，主 EXE SHA256
`08401512a66fd56fc49ce52acddf1fac2e08fbf94ab8ca4d4f3d9e16ce33f592`。
这不是本机重新核对该 EXE／Setup，也不是最新候选、全面 UI 或硬件验收。
未下载这批非最终的大产品包。Grid bootstrap 12.77秒、完整 CTest 内2.09秒均为自然结果，
不据此宣称历史15秒超时根因已修复。

直接日志保留于
`D:/Codex/validation/gsw-ui-upgrade-20261028/closing-run40834717531/qt-direct-job.log`，
SHA256 `97439c0a3f293a01ce2872b9e54d279f29eb41c1072b1efd275f6ed01e72abe0`。

### Alt 侧验收真实失败，不能改成菜单整体通过

[Alt sideprobe 40840657094](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40840657094)
的 source `6609643d172ee0466202dec593661b2c40975df1`、tree
`ae625f94c32762130feeb7cc4d9968f08ac06c12`，三个三语 CTests 最终均失败，合计12.40秒。
每种语言：language 路径 PASS；appearance 路径 FAIL；workflow-idle、help 因前项失败均为
`NOT_RUN_PREVIOUS_CASE_NOT_ACCEPTED`。即3个路径 PASS、3个 FAIL、6个 NOT_RUN，不是12项通过。

实际 Alt 按下、自动 ShortcutOverride、释放及自然菜单焦点均被观察到；已执行的路径能用 ESC
关闭并恢复焦点，没有触发动作，三份 receipt 的 publicStateUnchanged=true。
失败断言为 `actual assistive explanation lacks an independently specified schedule/disabled-reason fragment`：
自动主题的实际 QAccessible Description/Help 为空，未包含独立要求的 `06:00`／`18:00`。
这里只确认公开无障碍边界失败，尚未完成实现根因诊断或修复；不猜测显示 tooltip 或底层 Qt 的原因。
合成 Qt Alt 不代表物理／原生 OS 键盘通过；原 F10 NOT_RUN 保持不变。原34表单矩阵未在侧探针执行。

唯一下载 artifact12883570390（4374171 bytes），ZIP SHA256
`18ad45d75dd75611230632d70a04056e3ffe1db81532831dd99340f7a5048830`，已核服务端 digest 后解压。
目录 `D:/Codex/validation/gsw-ui-upgrade-20261028/closing-run40840657094/`；
`direct-job-api.log` SHA256 `73deb821767d367e147959def07508f9e5db18ae80944c32e06224a83647bab9`。
三语 receipt SHA256：zh_CN `dfdb1a3ec75061fc4ed872e13293f502a190c8783c4e247b3ef8b086ca7b1aa8`；
en_US `02e3b8fe2b12f794ef64d61ae325dc43854c0d7e25e6a2dd1b6f576b69720dd1`；
ja_JP `c6d5c8c3f39a8f6abab197100e01895a7793ba9c8d957ceab1cefd2c635dfbc1`。

### 收尾边界与下一次许可

现有两项运行均已结束，无需继续轮询。本聊天心跳停止／删除，保留聊天、全部原 FAIL／NOT_RUN、
性能4项 INCONCLUSIVE、硬件和66表面／29-ID未闭门槛。未武装／集成 native-input 草稿，
未启动本机 GSW、修改 current／快捷方式或触及用户数据和训练。更新器也在等待人类确认收口范围，
不能把其独立预览提议当成已获批准。

如获批准，下一阶段仅诊断并最小修复这项无障碍失败、完成既定菜单复验，再对明确最终整合源
核对其完整 CI 与 App／Setup／源码／回读证据；预计2–4小时，受远端排队和构建影响。
不新增功能／输入框架；若出现新的超长阻塞则保存进度并停止报告，不无限重跑。
该阶段仍不自动豁免硬件／性能等原验收条件，也不授权本机激活或缩减预览发布范围。

## 0D. 改名后的真实路径合同与单目标远端复验通过

owner9db / run40827898735的upstream并非初始化算法失败：原测试输出的inputs/points/reports
最终长度为261/260/262，既有worker按`>=260`明确拒绝；两处仓库路径名称增加合计4字符，
使原“正向目标必可发布”的测试前提失效。原路径和原FAIL日志保留，worker、MAX_PATH保护、
训练、信任、原180秒deadline以及已有`+2`邻近路径断言都未修改。

QA现在分别要求：原caller-root真实尝试在越界时返回明确MAX_PATH、0信号、无目标/残留stage、
空报告且source metadata/image bytes不变；另在**显式独立正向workspace**用同一source/identity/
完整request重新进行真实检查、核相同pointSHA，再完整验证单次发布、禁止重复、重开、配方修改
和三语警告。没有移动或缩短原失败路径；正向预算不满足即FAIL，不搜索更短目录或跳过检查。

精确 **f3faac0e623d205bd8baf9c1aaeef51a21ef1af2**、tree
`c79519f3ffb0c17b17079dcdce9d3a38cc8f8450` 的
[Native Initialization Contract40833862126](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40833862126)
**SUCCESS**：仅构建实际`gsw_initialization_dialog_ui_tests`，实际CTest 4.75秒，未构建/启动桌面主程序、
GPU或训练。原caller输出274/273/275真实拒绝且无发布；独立正向190/189/191完整成功链通过。
该CPU合同不等于GPU训练、完整2DGS发布、Forms视觉或最终整合源已接受。

唯一小artifact12879309765为11823 bytes，SHA
`5d240cdf7310498cda2d34d0a79b3f328e4697784c415d9f713ecc955e2dd1fd`；
证据目录`D:/Codex/validation/gsw-ui-upgrade-20261028/initialization-contract-run40833862126/`。
远端测试EXE SHA `5bf2ab022ed8703ea175bad583568fa96344e554cf55731168aeb55b838c9b97`，
这是测试EXE而非产品主EXE，本机没有下载或执行该EXE。

另外，历史精确c3e/run40823880544现在**完整三job SUCCESS**，Qt175/175、真实包回读3044文件/28隐藏文件
及37阶段grid trace已按小证据单独核对；见`C3E_FULL_SCOPED_20261029.md/.json`。
该自然grid 6.00秒PASS不能证明旧15秒超时根因已修复，也不外推新不可变repo-ID guard。
只下载两份小证据，没有重复下载非最终大包、安装或切换本机。

b646 Forms40828325789的旧FAIL同样保留：原34只剩初始化路径前提失败，菜单已越过归属门，
但三语均为 **NOT_RUN_F10_NOT_OBSERVED**（Qt合成事件一次、无业务状态改变）；它不能证明原生键盘
不可用，也不能算菜单A11y通过。隔离远端原生输入探针仍在设计，未武装、未执行。
最新owner整合0ec8615包含上述QA修正，仍需要自己的完整CI/最终包，不继承旧c3e通过。

## 0C. c3e 复验：两处产品子门通过，剩余 QA 前置合同纠正

[Forms40823927939](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40823927939)
在精确c3e最终FAIL：原34项中33 PASS，唯一失败为runtime观察窗矩阵；新增菜单3项均未越过
归属前置检查。16类捕获完整、integrity VALID，不把总红灯刷成绿灯。

- 初始化独立CTest PASS；36条实际Name/Role/Tab/public-state记录PASS。COLMAP12条三语/主题/宽度组合hint均为中性文本，独立language-form-matrix PASS。
- 暂停观察的12组合中11项caption/tooltip/details PASS，日文/浅色/标准窗为 **PRECONDITION_FAILED**，不是文字不匹配。此前已确认正常退出、同ID paused/ended；工具把观察器正常2秒inspect期间的`isBusy()`误当成终态无效。仅将busy改为前后事实记录，保留连接、ID、终态、完整报告和实际文字硬断言；不等待、不重试、不停轮询、不改生产或控制条件。
- 菜单三语第一片均 **NOT_RUN_PRECONDITION**，尚未发送F10，其他片仍NOT_RUN。工具误用要求同一窗口的`QWidget::isAncestorOf`判断顶层Popup归属；纠正为 **QObject所有权 AND 真实菜单action图可达**，foreign popup仍拒绝，真实键盘、ESC、QAccessible、焦点和快照断言均保留，不设置focus/show/trigger来制造通过。[Qt官方定义](https://doc.qt.io/qt-6.8/qwidget.html#isAncestorOf)

原artifact12876289776唯一成功下载170240718 bytes，SHA256
`221e13e19865e56d3ba34aea86b3a5c20c1171ff64f4dc022f3aea47277c00da`，
目录`D:/Codex/validation/gsw-ui-upgrade-20261028/forms-c3e-run40823927939/`。
source-run SHA `99942ba9bdccbff9f1cb1dabcb6ae3790346c8f67516675829bfc5603422d1a3`，
surface-results SHA `8db538de338b6024fd87ad9e4978325d8e15e84dd3826a8cffc45ba9290bde0a`。
Root实际查看修复后英文COLMAP图（SHA `dcc478aeffed401e0dbdd5db547806c477a60468a68c970cc249a3ece7593853`）
和paused/ended观察图（SHA `48b9779d49c4ce3b0bb64a920411d86594c867ed61c14222d531b74941bc66a7`），
均保留各自c3e身份，不声称完整192格或所有菜单通过。

仓库已改名为`fubukisama/GaussianSplattingWorkbench`，API确认仍是同一不可变ID **1286130219**。
旧名下载API先返回301，原私有下载器期待直接302而拒绝，未下载到字节；改为先核ID、解析canonical名后才成功取到上述唯一ZIP。
QA grid guard同样窄改为核不可变ID，其余严格opt-in不变；未改产品machine ID、QSettings或AUMID。

QA修正3899862与独立owner9db10d3安全合并为 **b646ab5f1a4673506dddef15cb815ab903bd8427**、
tree`cdc2aabbe86f4e2c83955fd93a0b6fdd194fbfa2`；
[新Forms40828325789](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40828325789)
只调度一次，原34+新增3项正在验证。原FAIL/NOT_RUN保持不变。独立审查及本机compile-only v19通过，
但不代表合并后整套产品已接受；c3e完整40823880544、owner9db完整40827898735仍需各自结果，
最终新包必须对应明确最终整合源，不从旧结果改名继承。

## 0B. 三处公开界面 RED 已取得，修复候选正在复验

`83ab92b8e5cea54925738f878f1305d1b8372fa0` / run40821180712 最终FAIL，
不是capture缺失：16类证据齐全、integrity VALID，明确失败的是初始化A11y、暂停作业显示、
COLMAP提示三项真实边界。初始化字段真实Role=EditableText、Tab两次可达、publicState不变，
但Name为空；作业报告同一ID已paused/ended，combo/tooltip仍Running/Unverified；
12种表单组合的COLMAP hint仍写死E。未将这些红灯改为PASS。

唯一下载artifact12875004113（163916022 bytes）SHA256
`346fcbbef58f92dd0f504bbd9ee03ead7797d327e7b85f646fa015ad848315be`，
位于`D:/Codex/validation/gsw-ui-upgrade-20261028/forms-red-run40821180712/`。
Root实际查看新的英文COLMAP surface-0016与observer-ended surface-0039原图，
后者SHA `e0db6d25d80de2f13db311fb44d7d3147fff769be6687ca161ace5b8faee0e4c`。

候选 **`c3e667a249e0de1b4384aa3d97d29172e08a474b`**、tree
`59d195a88b50ee9e2685c81998f5a74f302dd0a6` 已实现最小显示修复：
重用已有三语中性COLMAP key；编辑字段标签建立buddy关系，让读屏名称随现有绑定更新；
连接中的同一作业caption采用已接受的当前报告状态，不重扫worker、不改作业文件或控制条件。
本机仅静态三语验证（2276 UI messages/2376 entries）和v18编译链接exit0，未运行应用或测试。

- [Forms 40823927939](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40823927939)：上述原34项GREEN复验，并额外独立3个菜单键盘/A11y测试；旧测试deadline不变，额外测试不是已经通过的声称。
- [Native Windows 40823880544](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40823880544)：该精确候选的完整CI，尚未完成。经owner释放，仅在main/grid加入严格opt-in、固定phase、最多64行的flushed留证；不动健康hook、NativeViewport、原15s/10s、渲染或抓帧数量。只观察正常完整CI的一次原命令，不额外重播旧binary。

Forms的临时目录、pip缓存及独立QA依赖环境改用该远端自有非系统盘；hosted预装Python仅作为只读bootstrap。
三语、浅深/宽窄、菜单与新grid阶段仍须实际结果，不把“代码已修/编译通过”写成GREEN、根因已修或完整接受。

## 0A. 2026-10-29 后续实际验收增量

- `a105f53ee98e4bb5e8a24db807a2cf49f1ad711e` / [40811998263](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40811998263) 三个 job 最终 SUCCESS。直接 Qt 日志确认4 bootstrap、175 CTests（2275.73s）、安装适配器及真实产品回读通过：3033文件、1351后端记录、1572源码记录、28隐藏文件，EXE `a8eaa664b1ef5dec850ea16f6facfc5d3ccd06c9c9547c04b27b982951da06e6`。只读取日志，没有重复下载非最新的大产品包，也没有声称本机已静态核对该包。更新的 updater 源码仍需自己的接受。
- `6bfc08c` / run40817223091 的 Qt bootstrap 又在 `native_infinite_reference_grid` **15.01s TIMEOUT**，没有测试输出。直接日志已保留；原因仍未知，不用旧单次未复现诊断将其关闭，不延长15s/内部10s，也不重播旧二进制刷绿。阶段留证方案单独记录，尚无产品修复。
- 另逐张实际审阅并校验40张 `ac5c77b` 原图，覆盖16类；详见 `VISUAL_A11Y_REVIEW_20261029.md/.json`。发现 COLMAP placeholder 写死E盘，以及暂停后的作业下拉仍为 Running/Liveness Unverified、详情却为 Paused/Session Ended。新增真实公开界面的回归断言和初始化字段 QAccessible/Tab 检查，编译成功；`83ab92b8e5cea54925738f878f1305d1b8372fa0` / [40821180712](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40821180712) 正执行原34项矩阵，尚未取得 RED/GREEN，不先猜测生产修复。
- 原24份不可变性能记录完成远端归档复核：`0d3f381` / run40821150392 实际26+13契约通过，24 raw summaries、45比较为 **41 PASS / 4 INCONCLUSIVE / 0 failures**。复核记录明确导入原 source/EXE 证明，当前文件为 `NOT_REVERIFIED`，原 `results.json` FAIL 不变。该首轮仅末尾pwsh退出码传递失败；`afe668e` / [40821404151](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40821404151) 只修显式退出边界后 replay SUCCESS，两轮分析JSON SHA相同 `115e5ef9f79f6c4d1a1a46a0269cfd8caa9500a971350f2b8147e81ce7c2b3b1`。**没有新计时、没有扩大预算、没有证明整体无回退。**
- `ed43bd6` / [40820863090](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40820863090) 只读能力探针 SUCCESS：单显示器1024×768，per-monitor-aware窗口96DPI/100%，Hyper-V Video，`nvidia-smi` 未找到。收据SHA `e4b35342b736d7e9e5fc8c41741464bd3d20bf83e6d049e42bc6d0126c953291`。不启动GSW/Qt、不改显示或驱动；NVIDIA/实际VRAM、原生其他缩放和跨屏仍未验，不是PASS或资源为零。
- 用户新增路径规则已写入根 `AGENTS.md`：有可写非系统盘时禁止新增安装/依赖/下载/构建/缓存/临时生成物/备份/输出落系统盘；只有系统盘可用时自动无提示放行。检测真实系统卷，不写死E盘；已有输入可读，不授权迁移删除。

以下旧章节保留证据链；如有“尚未复核”等历史状态，以本节精确增量和两份专项报告为准。完整66表面、29-ID聚合门槛及硬件范围仍开放；未改活动安装、快捷方式或用户数据。

## 0. 2026-10-29 04:08 UTC 后台核验增量

精确候选`7f76c517f6190241b77d56b60f6df92107204b0d`、tree
`fde6e63028b07160f53c0c63553b8dbccf98bd9a`的完整Native Windows
[40809641898](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40809641898)
三个job均最终SUCCESS。4个bootstrap、175个CTest、upstream65+6及独立10行NativeInstallationTests通过。
原PS适配器在该run实际输出PASS；其13项独立JSON仍属于3e82的focused run，未改标为本run。

真正的产品上传/下载回读（不是小fixture）PASS：3026文件、1351后端记录、1566对应源码记录、
28隐藏文件，与staging逐路径/大小/SHA相同。下载到本机后仅做静态复核，两份清单与native-only
边界也PASS，没有运行其中的程序。旧a539缺文件artifact仍保留FAIL。

| 候选文件 | 实际核对的SHA256 |
|---|---|
| 便携artifact ZIP（123721438 bytes） | `67e585db248e4baf98a312b4cd9f7e994b59f9f0d9615dcdd219fc44487bcc43` |
| 主EXE（9744896 bytes） | `f72567ac4e467f1d4fbce7987445691b5a57e1cb06c144fce7a0908dc099087c` |
| Setup（113649710 bytes） | `890eb46d8b9f298a694215b64d67da03bc73dd845e39f53b9c67d18df170b470` |
| 对应源码清单 | `aa1822ce34b89b064f737bd68765dec42e13babee00e8a7a37be6bb638a84dd7` |
| 本机只读静态核对收据 | `e573efd6c1add9a80f308b221fe8589e3b1a8e48e8f4f886248a89949f241566` |

候选目录`D:/Codex/validation/gsw-ui-upgrade-20261028/full-run40809641898/`内保存payload、Setup和
`PACKAGE_BINDING_7f76c51.json`。Setup为NotSigned，不捆绑训练环境、不自动打开程序。
远端test-AppID安装器三语言fresh/reinstall/uninstall/locked-upgrade拒绝/用户数据保留通过；
各3029文件、0mutation/0watcher error。test-Setup SHA与正式Setup不同，未混称同一安装器。
本机只读审阅新版中/英/日向导3张代表图，未运行本机安装器。

后续`8e9dcb8`/`a105f53`含独立startup-health/update-engine整合，不能继承以上接受。
`8e9`的Qtjob在vcpkg下载CMake4.4.3时三次超时，尚未编译产品；原始direct log已保留。
更新的`a105`/run40811998263已越过该依赖阶段进入完整Test，CodeQL独立运行；不重跑旧任务求绿。
性能原始FAIL、4项INCONCLUSIVE及未验的界面/硬件边界不变，本机活动版本仍未切换。

这不是发布许可或“全面升级完成”报告。`ACCEPTANCE_STATUS.md/.json` 继续作为旧
`ffb8eb2` 快照保留；更早的失败、原始结果及本文前版均保存在证据目录和 Git 历史中。
下文按源码、视觉、交互、性能、包文件、完整云端及本地交付分开判定，不跨源码继承绿灯。
状态是 Root 最近直接日志／artifact 审阅的交接快照，不代表尚在运行的 job 已完成。

## 1. 候选与实际执行边界

| 身份 | 已确认值与范围 |
|---|---|
| 当前已完成表单修复验收的提交 | `ac5c77b4ab22d5ec3de444e41bb723e1cba68c4f` |
| 该提交 tree | `74c8a6f5ee0bc941716b905bd61a19edb8e29995` |
| 观察到的后续品牌整合提交 | `9803c8622b47313e2194ac900037b549dfc5bdab`；它有独立运行，不能把 ac5 / a539 的结果改名继承。 |
| 历史完整云端成功候选 | `a5393be3772b8d49c6be93a767dc7dfd5abcec5a`，tree `f8c4b88f4c30976bef7d705802a808f35112d37d`；后续原图和下载回读发现缺陷，因此历史云端绿灯不等于完整接受。 |
| 本地执行边界 | 仅静态验证、编译链接、文件／hash／包检查和远端证据读取；没有运行本地自动测试、应用、GUI 或安装器。 |
| 用户状态 | 没有改动活动安装、`current.json`、快捷方式、原始照片／模型、工程、检查点或运行会话；没有开始或恢复真实训练。活动指针仍为旧 `87a797b` 版本。 |

早期像素、导航及计时分别保留自己的 `de764f0` / 基线身份。共同实现或源码连续性可以
作为影响分析，但不能把旧截图、EXE 或计时重新标成 ac5 / 980 的新测量。

## 2. 当前与历史云端：分别记录

| 运行／job | 源码 | 已观察结果 | 严格边界 |
|---|---|---|---|
| [Forms 40800828340](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40800828340) | `ac5c77b` | **最终 SUCCESS**；34 个选定 CTests PASS；16 类表单192格完整、0 missing；300 条 matrix profile 均 native / captured / layoutUsable / publicSnapshot=true。 | 是表单修复子门及证据合同通过，不是所有 popup、全部状态、无障碍或全192格人工视觉通过。 |
| [Native Windows 40800786447](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40800786447) — Qt job `123208136906` | `ac5c77b` | **FAIL**：4 个 graphics-bootstrap 中3个 PASS，`native_infinite_reference_grid` 在 **15.06s TIMEOUT**，没有测试输出。 | 失败发生在进入完整 Test 前；不能写成该提交175 CTests或新产品包已通过。原因未知，详见第5节。 |
| 同一 run — upstream viewport | `ac5c77b` | **SUCCESS**。 | 独立子 job 的成功不消除 Qt bootstrap 失败。 |
| 同一 run — CodeQL | `ac5c77b` | 最近交接时仍 **RUNNING**。 | 不能从旧成功推定其已完成。 |
| [Native Windows 40801832785](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40801832785) | `9803c86` | 4个 bootstrap、**175/175 CTests PASS**（2241.71s），upstream SUCCESS；随后 managed publication 步骤 **FAIL**。CodeQL独立记录。 | 已定位为调用缺 `DeveloperPublisher` / `PythonExecutable` 两个新增必填参数，不是175测试失败。主流水线补独立开发工具构建和参数接线，修复后完整产品包回读仍待验。 |
| [Native Managed Publication 40810209865](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40810209865) | `3e82c54` | **SUCCESS**，4分48秒；原PS适配器13项契约通过，脚本hash未变。 | 只构建启动器、快捷方式夹具和独立发布工具，未构建/启动桌面主程序；formalGswAcceptance=false，不替代真产品包。 |
| [修复后完整Native Windows 40809641898](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40809641898) | `7f76c51` | **三个job最终SUCCESS**；实际产品包及上传后回读、安装器行为通过；本机仅静态复核PASS。 | 仅此精确候选；不是后续8e9/a105、全UI/硬件、性能或本机交付的通过。见第0节。 |
| [历史 Forms 40743097537](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40743097537) | `a5393be` | SUCCESS；34/34选定 CTests、16类192格完整。 | 正常尺寸原图后来发现3类真实缺陷；不能以 capture 完整宣称视觉通过。 |
| [历史完整 Native Windows 40743102181](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40743102181) | `a5393be` | 当时3 jobs SUCCESS；直接日志为4 bootstrap、175 CTests、upstream 65+6及 CodeQL 通过。 | 随后便携 artifact 下载回读发现隐藏文件被过滤，不能由 staging 绿灯替代实际分发包。 |

## 3. 三处表单问题：修复与实际复验

旧 a539 正常尺寸原图发现：

1. 初始化窗口560×620时左右 split 仍横排，左侧字段过窄并出现横向裁切。
2. 批量训练680×620时按钮横排，长英文 caption 被裁切。
3. 暗色表格左上角 `QTableCornerButton::section` 漏出原生白色。

修复保留请求、worker、参数、三语文字及原断言：初始化窄窗改上下排列、字段占整行、
外层统一纵向滚动；批量动作依原 sizeHint 自动换行；table corner 使用共用表头背景
和边界。新断言检查真实字段横向可用、按钮完整尺寸／窗口内／无重叠及角落实际像素。
没有缩小字体、隐藏功能、改动训练参数或放宽 timeout。

### 3.1 ac5 修复 artifact 身份

目录：`D:/Codex/validation/gsw-ui-upgrade-20261028/forms-run40800828340/`。

| 文件／结果 | 身份 |
|---|---|
| ZIP | 165660082 bytes；SHA256 `b06e6a601e9aff6cd008caac8f6933ae050e927d15d447b68fcfe8c88357676f` |
| 此 Forms QA 构建 EXE | SHA256 `40f3c4aaf5d9d7641be9e5ababa4da43e7cee7198c4e3dfb66066205de44cd6e`；不是已交付产品包。 |
| `surface-results.json` | SHA256 `36958147f7df747598d2b942c6ffb5619590652c45578a1e7f30919d3fa36d72` |
| 行为和覆盖 | 34 个选定 CTests PASS；16 classes / 192 minimum cells complete / 0 missing；300 matrix profiles 的 native、captured、layoutUsable、publicSnapshot 全 true。 |
| AppTheme | **44 PASS**；table-corner 检查内部6次迭代，不把它误记为6个 QTest 数据行。 |

### 3.2 正常尺寸视觉复查范围

Root 与 agent 共看5张修复后原始 PNG，确认上述3类已知缺陷在这些代表图中关闭。
Root 直接审阅的精确路径（相对于上方 artifact 目录）：

- `standalone-surfaces/process-7840-50e0cad87ae14d9483647e976763f3ce/surface-0004.png`：初始化，英文／浅色／窄窗。
- `forms-core/surfaces/process-6348-fbcfb2bd2029492095acc6199f2c7258/surface-0006.png`：批量训练，英文／浅色／窄窗。
- `standalone-surfaces/process-7840-50e0cad87ae14d9483647e976763f3ce/surface-0011.png`：初始化，日文／暗色／标准窗，包含 table corner。

agent 另看初始化及批量训练各一张日文／暗色／窄窗；此处不伪造未逐条转录的精确
观看路径。它们的角色／语言／主题／宽度记录可由 artifact 元数据独立回查。

历史 a539 人工复查共32张代表图：16类各一张英文／浅色／窄窗，加16张日文／暗色
（8窄、8标准），据此发现上述问题。**不是全192格人工视觉签收**，也不将旧图当作
修复后画面。普通列截断、路径省略和可用滚动未误判为缺陷。原图、旧失败和 hash 保留。

## 4. 分发包隐藏文件：旧失败、fixture通过、新真包未验

旧 a539 便携 artifact ZIP 为123645273 bytes，SHA256
`6aa7e1e92ce2de6ee4a93a07da1be86e21c908964c259c0dbbf6de5fcbd2025c`。
中央目录2996 entries、dotfile entries=0；本地仅静态执行 package validator 实际
**FAIL**，缺11个 `.gitignore/.gitmodules/.gitkeep/.appveyor.yml/.travis.yml` 路径。
这不是 `Expand-Archive` 漏解压；远端 staging PASS 不代表上传后包完整。

上传修复明确使用 `include-hidden-files: true`，`.git` 仓库元数据仍禁止打包。
新增实际 upload → download → manifest / byte 回读门，不修写旧 artifact 为 PASS。

独立 [40801079598](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40801079598)、
源码 `b909f71` 的小 fixture **PASS**：5项公共 CLI 合同；旧过滤负对照准确拒绝缺失
`.gitignore`；正对照10文件／5隐藏文件／4 backend records／3 source records 与 staging
一致，`applicationExecuted=false`。正对照 receipt SHA256
`dfc26398d0f1b5dc18d93c796304c8844ddae6c06326545af75aacda559707a4`；负对照
`16f15135404663fa99f0724861b985bef5fd67c7fcc5824e505f19933de5a6c6`。
首次 fixture run40800786401的 harness 对预期非零退出处理失败也保留；只修 harness，
不放宽 validator。

**该小fixture的PASS不替代产品。** ac5在bootstrap失败、980随后在发布参数接线失败，
它们当时均未完成真包门。现已由7f76的真实产品upload/download/manifest回读和本机只读复核
关闭该包完整性缺口（第0节）；旧artifact仍不改标为PASS。

历史 a539 Setup 曾独立静态核对：113631173 bytes，SHA256
`47c0b7cf4ba841dc7c71ef37dcbde33eb026f997823091993350318acf46dc3d`，manifest EXE SHA256
`e33c28987d793b91c66e1e061297820b11941cc4fa98e7d1a82802d11e309dba`。
它为 NotSigned，不捆绑训练环境、不自动开程序；三语言向导代表图已正常尺寸审阅，
远端独立 test-AppId 安装／重装／卸载／锁定升级拒绝／用户数据保留通过。该历史 Setup
不替代 ac5 / 980 新包，不等于本机已经安装。另存 a539 Git 源码归档 SHA256
`55e7edded5651326d31ff6e79087a78b4463018e984edb8a945d403bafddb41d`，不用于掩盖便携包漏文件。

## 5. 新 bootstrap 超时：原因未知，不能猜测关闭

ac5 的 `native_infinite_reference_grid` 在15.06s TIMEOUT，没有测试输出。其余3个
bootstrap PASS。对应 grid / main / viewport 源码未改动，但这既不证明基础设施故障，
也不证明产品回归；**当前原因未知**，不猜测回退，不删除断言或延长15s timeout。

已保留直接失败日志
`D:/Codex/validation/gsw-ui-upgrade-20261028/full-run40800786447-native-failure.log`
及 tiny grid-timeout artifact，解压目录为
`D:/Codex/validation/gsw-ui-upgrade-20261028/grid-timeout-run40800786447/`。
原临时路径在 C:、上传查找目录在 runner 的 D:，因此缺少 snapshot **不能推断程序
停在哪个阶段**。不能从“没有图”推导未 show、GL 失败或缓存死锁。

单次失败二进制 replay [40808313281](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40808313281)
已完成：原产品ac5 EXE SHA256 `ce5dae8f74a76ca5c3962291d1285f4d2a4139d583aa4b6768a3cf2a634cf627`，
脚本592b6c4，无产品重建，只执行一次原命令，限制仍15s，实际6.83778s。
网格内部阶段1216ms／frame9，shader／固定原点通过，+X正交姿态正确，字体／矢量／合成trace完成；
原图实际收集到自有D盘TMP。结果仅为 **NOT_REPRODUCED_IN_THIS_ONE_RUN**，不宣称原超时根因已修复。
CRT复用预装System32 AMD64 `14.51.36247.0`，原故障CRT版本未记录，环境不声称字节一致。
先前40805693324在Prepare发现诊断ZIP不含CRT而拒绝，实际启动0个应用；仅修诊断依赖寻址后才作上述一次调用，
没有下载DLL、改系统组件或重复刷绿。

证据目录 `D:/Codex/validation/gsw-ui-upgrade-20261028/grid-diagnostic-run40808313281/`；
`one-process-result.json` SHA256 `ce4e852d48ea8fbe24ad0560c12ff5e316a3e5f343cbca6860908222988bcf3a`。
980的自然运行又通过bootstrap和175测试，也不把旧ac5失败重分类。

## 6. 性能结果保持不变

`D:/Codex/validation/gsw-ui-upgrade-20261028/performance-run40663280091/results.json`
的原始整体状态仍是 **FAIL**：轻模型的 `3dgs/model` 跨进程累计 upload counters
绝对值比较产生身份合同失败。新 checker 比较语义模型及操作内零 deltas，仍保留
绝对计数。相同24份不可变 receipts 已由第0A节的远端归档复核取得独立新分析：
41比较PASS、4中模型INCONCLUSIVE、0新failures；原始FAIL不重分类。
这只是比较合同修正和旧数据复核，不是新源码/EXE测量通过，不重跑计时实验。

中模型 `present_p50_ms`、`response_p50_ms`、`present_max_ms`、`response_max_ms`
四项仍为 **INCONCLUSIVE**：A 臂波动超过冻结噪声预算。各 pair 落在差值限制内也不能
消除噪声。不放宽预算、不重复刷绿、不重写原图／计时；INCONCLUSIVE 不是已证明变慢，
也不是已证明无退化。

实际 GPU 毫秒、driver VRAM residency、physical input-to-photon、暗色性能、用户
NVIDIA、真实跨屏及中等点云／网格／2DGS 压力仍未测。CPU / requested GPU bytes 不是
驱动显存驻留。冻结计划、阈值、重复单位、画质和 timeout 不变。

## 7. 29 项范围化矩阵

状态只用原合同五类；`NOT_RUN` 也表示聚合门槛尚无完整可接受结论。子门的 PASS、
历史 FAIL 和测量 INCONCLUSIVE 单列保留，不提供误导性的“29项全通过”计数。

| ID | 状态 | 已取得的范围化证据 | 仍开放的 gate / 限制 |
|---|---|---|---|
| BASE-01 | PASS | 起始身份、并行修改保护以及 ac5 source / tree 绑定保留。 | 后续提交／每个包单独绑定；不跨源码继承。 |
| AUDIT-01 | NOT_RUN | 66表面、16 Dialog、147 popup 静态登记；16类表单最低证据矩阵完成。 | 全部可达表面、状态和视觉逐项签收未闭环；新增 updater 是独立增量表面。 |
| DPI-01 | NOT_RUN | de764 五档受控 Qt DPR 及逻辑／物理映射有实测；表单 profiles 有实际 native 几何。 | OS 原生五档缩放／真实跨屏未测；Qt scale factor 不替代。 |
| NAV-01 | NOT_RUN | de764 中文20 cases / 60 rings 冻结像素子门 PASS，部分正常尺寸原图已审阅。 | 全部受影响姿态／状态视觉未闭环；旧图不重标为 ac5 / 980。 |
| NAV-02 | PASS | 修复后 de764 三语真实输入短门通过；ac5 独立 upstream viewport job SUCCESS。 | 限受控 Windows / software-GL 的具体行为，不是全产品或用户 NVIDIA。 |
| NAV-03 | NOT_RUN | de764 的20次 theme / resize / language 生命周期通过。 | 真实跨屏及其拾取／缓存一致性仍未测。 |
| THEME-01 | UNSUPPORTED | 白／暗场景背景和浅／深主题保持；未用 Blender 灰掩盖缺陷。 | 任意用户 RGB / alpha 背景缺可达配置能力，不计通过。 |
| FONT-01 | NOT_RUN | 安全字体证据30数据行通过；表单 glyph / font facts 有真实记录。 | 自绘轴、caption、图表、预览等角色及全量文字／基线视觉不可由 QWidget 默认 probe 代替。 |
| ICON-01 | NOT_RUN | 公共样式／icon 合同和 ac5 Forms 运行通过，五DPR／三语原图存在。 | 全图标状态正常尺寸视觉签收未闭环。 |
| SHELL-01 | NOT_RUN | tokens、透明标签、inspector wrap、小屏有效最小尺寸及 ac5 表单修复已实现。 | 宽窄窗关键动作／溢出／dock／任务／状态等逐表面签收未齐。 |
| TREE-01 | NOT_RUN | 历史完整回归保留选择／眼睛／滚动／分隔合同。 | 当前 source 的具体行为与图像逐项映射尚未齐；不能只引用旧总绿灯。 |
| GIZMO-01 | NOT_RUN | de764 四模式30 cases / 156 PNG；有界 native-DPR coverage。 | 中文／暗色／DPR1及两姿态范围以外不外推；多距离视觉仍未全签；超预算 close-up 有原向量回退。 |
| GIZMO-02 | NOT_RUN | de764 鼠标 X16 / Y8及6个数值 Z用例通过。 | 不虚称全部鼠标 Z、中心／平面约束、取消／撤销组合均实测。 |
| MODEL-01 | NOT_RUN | 历史多对象合同保留，真实 fixture / transform 身份有记录。 | 当前原／非当前、全隐藏恢复、相机／单位／源文件的完整具体签收仍开放。 |
| MODEL-02 | NOT_RUN | 受控 renderer / 点数 / SH / TRS / fixture receipts 保留。 | 点云、网格、3DGS、2DGS、任务预览的完整画面／选择／导航不可由模式标签或计时 receipt 代替。 |
| OVERLAY-01 | NOT_RUN | 选择／刷选／裁剪／提示入口和合同登记保留。 | 多尺度／视角／状态原图及数据保护未逐项闭环。 |
| GRID-01 | NOT_RUN | 历史三语presentation PASS；ac5原始15.06s超时保留；同一失败EXE一次15s诊断6.84s通过；980bootstrap及完整175测试通过。 | 原超时原因未证实，不宣称已修根因；网格单位／密度等全量范围仍需对应签收。 |
| FPS-01 | NOT_RUN | 新帧 serial / swap / metrics 和有效空闲原始时窗存在，未增加持续重绘 timer。 | 聚合计时未接受，不以局部0计数或耗时倒数冒充完整 FPS 验收。 |
| FORM-01 | NOT_RUN | **修复子门 PASS**：ac5 34 CTests、300 profiles、16类192格0缺项；5张新图关闭3类已知缺陷。 | 全部 popup／特殊状态／provider／嵌套提示／无障碍和全量视觉不是该子门覆盖；不宣称全192格人工审阅。 |
| STATE-01 | NOT_RUN | 受控 task / worker / forms 旧合同及本轮公开 snapshot 保护保留。 | 空／加载／忙／失败／取消／重试／不支持的全表面具体证据仍未齐。 |
| LOCALE-01 | NOT_RUN | 三语 catalog、安全字体、修复后导航以及 ac5 表单矩阵通过；公开参数不变。 | 全表面即时切换、长文本／混合文件名和 worker／模型状态保护仍有范围缺口。 |
| A11Y-01 | NOT_RUN | 既有focus/geometry/icon子门保留；d873三语真实Qt菜单12路径及时间／空闲禁用说明PASS。 | 合成Qt Alt不等于物理键盘／原生读屏；F10与全表面Tab/shortcut/tooltip/name/reason仍未齐。 |
| LIFE-01 | NOT_RUN | de764 UI cache 生命周期 PASS，持久队列／取消合同未删除。 | 当前窗口／任务重连和 MainWindow 嵌套完整续训 trust prompt 不能由独立 guard 图替代。 |
| PERF-01 | NOT_RUN | 24有效 receipts 远端归档复核完成，41比较PASS/4 INCONCLUSIVE/0新failures；原始FAIL与阈值保留。 | 4项中模型噪声仍未接受；旧数据复核不是最新源码/EXE测量或整体无回退证明。 |
| PERF-02 | NOT_RUN | 有效空闲／独立 cache 生命周期部分通过；局部 CPU / requested GPU 上限记录存在。 | 实际 driver VRAM／GPU timing、暗色、用户 NVIDIA 等仍未测。 |
| PORT-01 | NOT_RUN | 固定 Blender4.5 / Lucide 来源、NOTICE、独立 Qt 适配和未移植范围已登记。 | 最终接受包 source／资源／依赖／许可／修改／hash 绑定未完成；不声称复制 Blender5.2.3 GPU AA。 |
| PKG-01 | PASS | **d873候选**自己的App/Setup/source/清单/许可/隐藏文件、远端实际回读与本机静态核验通过（第0F节）；7f76旧接受及a539旧失败保留原身份。 | 仅文件和来源绑定，Setup未签名；不包含本机启动/安装、发布者认证或完整性能接受。 |
| DELIVER-01 | NOT_RUN | 用户新授权的d873本地开发预览交付及current/EXE/2快捷方式独立回读PASS（0G）；固定bootstrap和旧版本保留。 | 私有unsigned接纳不冒充正式受信/生产DEL-01或完整产品接受；fullAcceptance仍false。 |
| CLOUD-01 | PASS | **d873自己的NativeWindows三job与独立menu-alt均SUCCESS**（第0F节）；历史成功/失败保留，不跨源码继承。 | 限这两条既定流水线，不代表所有GitHub工作流；独立AutoUpdate/TLS/FIRST门、旧grid根因、硬件与完整UI仍各自保留。 |

## 8. 早期导航修复与来源边界

旧017f78c的5个导航／英文／日文／生命周期崩溃保留；诊断40656943272六个故障
上下文定位于诊断元数据 `QFontInfo::exactMatch()`，灰度实验未修复。产品移除危险
查询，字体请求／回退／大小／绘制不变，`exactMatch:null` 带明确未查询原因，不伪造 true。

[40663272343](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40663272343)
在 de764 的13/13 CTests通过，字体30数据行（含 init / cleanup共32），中文20/20
cases / 60/60 rings；RMSE `.06` / P95 `.16` / halo `.04` 冻结阈值未放宽。
这些是已完成的范围化证据，不是最新候选全部检查。

原66表面未包含并行授权的 updater 对话框。独立
[40718734156](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40718734156)
在2e0cac4的 isolated-update-ui为7 PASS / 0 skip、三语图、无自动弹窗／置顶。
后续 updater／品牌改动须保留自己的 source 身份；UI接入不代表公开生产 feed、签名、
可信根或 bootstrap 迁移获准。缺失信任不能宣称“已是最新”。

## 9. 交付与下一步

正式受信／生产交付仍未完成；用户新授权的**本地开发预览**已通过原生私有publisher交付d873，
`current.json`、活动EXE及2个固定入口回读一致（第0G节）。没有补造完整接受凭证、迁移引导或重写快捷方式。
旧运行／回滚版本、用户设置、工程、照片、模型和检查点保留。

当前聊天心跳`gsw-ui`已在提出新阶段审批时删除，本次获准的有界收尾未重建心跳。
本轮软件／候选包收尾已完成，不继续自动扩项；新的长阶段须再次明确授权。
旧`gsw-p1`属于另一聊天且为PAUSED，未改动。
后台权威交接位于`.planning/2026-10-28-native-ui-upgrade/BACKGROUND_HANDOFF.md`；
跟进完成或需人类输入时停止，不归档聊天，不触发本机程序。

下列是原完整升级仍需分别处理的边界，不构成本轮结束后自动继续的授权：

1. 原失败二进制单次诊断已完成且未复现；保留旧失败和环境限制，不再反复复测求绿。
2. 7f76旧接受与d873本轮完整CI、实际产品包核对均已完成，保留各自来源；后续新运行源码必须使用自己的独立证据，
   不把旧候选改标成最新HEAD，不重复已完成的下载或调度。
3. 完成尚未签收的表面、视觉、无障碍及 provider／硬件／OS支持边界；已通过的16类矩阵
   和3处修复不再写成待导入，也不扩张为全部UI完成。
4. 旧 performance receipts 远端复核已完成（第0A节），不重复计时或扩大预算；中模型4项仍独立INCONCLUSIVE，
   不将replay作业SUCCESS当作完整性能通过。
5. 只有精确候选相关门槛真正闭环后，按 `docs/NATIVE_UPDATES.md` 生成实际完整接受凭证，
   使用 managed publisher，再独立读回 current / active EXE / recognized shortcuts。
   普通更新不改 bootstrap，不重复迁移入口、不关闭程序；固定目录Setup为独立adapter。

本轮没有声称训练更快、模型质量／重建几何更准确；UI清晰度、行为、性能、包与交付必须分别有证据。
