# UI Upgrade — 2026-10-29 范围化验收记录

Status: **FORMS_REPAIR_SUBGATE_PASS / AC5_FULL_CI_FAIL / LATER_SOURCE_CI_RUNNING / NOT_FULL_ACCEPTANCE / NOT_DELIVERED**.

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
| [Native Windows 40801832785](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40801832785) | `9803c86` | 最近交接时 **RUNNING**；已通过 graphics-bootstrap，进入完整 Test。 | 是自然的新 source 运行，不是旧 ac5 失败重分类；完整结果、包和其余 jobs 仍待完成。 |
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

**该 PASS 仅限 fixture。** ac5 完整 Qt job 在 bootstrap 失败，新的完整真实产品包
upload / download / manifest 回读为 **NOT_RUN**。980 的完整运行仍在进行，未据此接受包。

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

单次失败二进制 replay 的诊断 workflow / script 已准备，最近交接时**尚未 dispatch**：
只复用失败二进制、一个进程、原15s deadline、fresh TMP trace；它是诊断，不是性能
复测或接受凭证。后续980自然运行通过 bootstrap 是新 source 的反例观察，不将 ac5
原失败改成 PASS。所有后续结果仍需独立保留 source / EXE / environment 身份。

## 6. 性能结果保持不变

`D:/Codex/validation/gsw-ui-upgrade-20261028/performance-run40663280091/results.json`
的原始整体状态仍是 **FAIL**：轻模型的 `3dgs/model` 跨进程累计 upload counters
绝对值比较产生身份合同失败。新 checker 比较语义模型及操作内零 deltas，仍保留
绝对计数；**相同24份不可变 receipts 的新远端复核尚未取得接受结果**，源码修复
不是测量通过。不为这个 checker 修复重跑计时实验。

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
| GRID-01 | FAIL | 历史三语 presentation PASS；ac5 bootstrap `native_infinite_reference_grid` 15.06s TIMEOUT。 | 无输出、原因未知；单次失败二进制诊断尚未完成，不能延时／删断言关闭。980 新运行的 bootstrap PASS 不改写旧失败。 |
| FPS-01 | NOT_RUN | 新帧 serial / swap / metrics 和有效空闲原始时窗存在，未增加持续重绘 timer。 | 聚合计时未接受，不以局部0计数或耗时倒数冒充完整 FPS 验收。 |
| FORM-01 | NOT_RUN | **修复子门 PASS**：ac5 34 CTests、300 profiles、16类192格0缺项；5张新图关闭3类已知缺陷。 | 全部 popup／特殊状态／provider／嵌套提示／无障碍和全量视觉不是该子门覆盖；不宣称全192格人工审阅。 |
| STATE-01 | NOT_RUN | 受控 task / worker / forms 旧合同及本轮公开 snapshot 保护保留。 | 空／加载／忙／失败／取消／重试／不支持的全表面具体证据仍未齐。 |
| LOCALE-01 | NOT_RUN | 三语 catalog、安全字体、修复后导航以及 ac5 表单矩阵通过；公开参数不变。 | 全表面即时切换、长文本／混合文件名和 worker／模型状态保护仍有范围缺口。 |
| A11Y-01 | NOT_RUN | focus inset、geometry、语义 icon 合同及按钮完整可用尺寸通过。 | 全表面 Tab / shortcut / tooltip / accessible name / disabled reason 仍未齐。 |
| LIFE-01 | NOT_RUN | de764 UI cache 生命周期 PASS，持久队列／取消合同未删除。 | 当前窗口／任务重连和 MainWindow 嵌套完整续训 trust prompt 不能由独立 guard 图替代。 |
| PERF-01 | NOT_RUN | 24有效 receipts 和空场景／局部 overlay 子指标保留；阈值未变。 | 原始整体 FAIL、新 checker 同 receipts 复核未接受；中模型4 INCONCLUSIVE。 |
| PERF-02 | NOT_RUN | 有效空闲／独立 cache 生命周期部分通过；局部 CPU / requested GPU 上限记录存在。 | 实际 driver VRAM／GPU timing、暗色、用户 NVIDIA 等仍未测。 |
| PORT-01 | NOT_RUN | 固定 Blender4.5 / Lucide 来源、NOTICE、独立 Qt 适配和未移植范围已登记。 | 最终接受包 source／资源／依赖／许可／修改／hash 绑定未完成；不声称复制 Blender5.2.3 GPU AA。 |
| PKG-01 | NOT_RUN | 历史 Setup 静态／向导／远端安装检查通过；隐藏文件 roundtrip fixture PASS。 | 旧 a539 便携实际 FAIL 保留；ac5 新完整真包门未执行，980 新包未完成；fixture 不替代产品。 |
| DELIVER-01 | NOT_RUN | 活动旧87版本和稳定 launcher 未改变。 | 没有新 publication / current / EXE / shortcut 读回；不得制造 `fullAcceptance:true`。 |
| CLOUD-01 | FAIL | 历史 a539 完整 SUCCESS 保留；ac5 Forms与upstream SUCCESS。 | ac5 Qt job FAIL，CodeQL最近仍RUNNING；980完整run RUNNING，不继承旧绿灯。 |

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

当前 **NOT_DELIVERED**：没有生成完整接受凭证、没有调用托管 publisher，没有切换
`current.json` 或重写快捷方式。旧运行／回滚版本、用户设置、工程、照片、模型和检查点保留。

接下来必须分别处理：

1. 对 ac5 grid timeout 做一次有界、原失败二进制诊断；原因未知前不猜修、不延时刷绿。
2. 等980对应完整 CI 与实际产品 artifact，核对 source / EXE / Setup / manifest /
   source resources / hidden files / 许可和依赖；不要拿 fixture 或历史包替代。
3. 完成尚未签收的表面、视觉、无障碍及 provider／硬件／OS支持边界；已通过的16类矩阵
   和3处修复不再写成待导入，也不扩张为全部UI完成。
4. 旧 performance receipts 的 checker 修复仅做远端复核；中模型噪声仍独立INCONCLUSIVE，
   不扩大预算或反复计时寻求绿灯。
5. 只有精确候选相关门槛真正闭环后，按 `docs/NATIVE_UPDATES.md` 生成实际完整接受凭证，
   使用 managed publisher，再独立读回 current / active EXE / recognized shortcuts。
   普通更新不改 bootstrap，不重复迁移入口、不关闭程序；固定目录Setup为独立adapter。

本轮没有声称训练更快、模型质量／重建几何更准确；UI清晰度、行为、性能、包与交付必须分别有证据。
