<div lang="zh-CN">

# 更新日志

[English](CHANGELOG.md) · [简体中文](CHANGELOG.zh-CN.md) · [日本語](CHANGELOG.ja.md)

按时间倒序记录 Gaussian Splatting Workbench 面向用户的变化。本次补记覆盖
2026-10-23 至 30 日，是经过整理的摘要，不声称还原全部更早历史。日期采用日本时间。
[完整源码提交记录](https://github.com/fubukisama/GaussianSplattingWorkbench/commits/main/)。

**状态说明：** **已实现**表示链接对应的源码修改已存在，不等于完成全部验证、
公开发布 Release，或已经安装到某台电脑；后三者分别需要对应版本的证据。
截至 2026-10-30，源码版本为 `0.3.9-native-preview`，尚无公开的原生 Release。
发行状态和要求见[下载指南](README_RELEASE.md)及[更新／回退指南](docs/NATIVE_UPDATES.md)。
CI 附件和本地预览版交付不会自动成为公开 Release。

以后新增、修改或修复面向用户的功能，必须在同一变更中同步更新三语日志，写明影响能力、
限制及验证或交付状态。“已实现、已验证、已发布、已安装”分别记录，不相互替代。

## 2026-10-30

状态：**已实现**；此节是源码记录，不是发布或安装回执。

- **调整：**恢复按日期整理的中、英、日更新日志及首页入口；将持续维护日志列为项目共用
  规则，并同步到 Codex 项目分叉聊天。文档校验同时检查三语历史、日期、源码引用和首页入口。
- **修复：**任务栏重新启动信息从正在运行的程序资源读取显示品牌，同时保留固定启动器
  作为启动目标。[源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/e1ec20a69f7a44523bc747fbbb71000984864684)。
- **改进：**自动外观、空闲时的暂停和不可用的继续菜单项，通过可访问性接口提供原因说明，
  避免只显示无法使用的操作。[外观说明](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/aa09ab26ba637387f3b1049dc11492916cce2b11) · [暂停／继续说明](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d87343fa215389815c6cf037aef85153a5bf6b91)。

## 2026-10-29

状态：**已实现**；验证和交付仍以单独记录的对应源码版本为准。

- **调整：**产品显示名称改为 **Gaussian Splatting Workbench**，保留原有程序文件名、设置键
  和应用标识的兼容性。[源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b0aacf85bb5e87b82b6bfcde0ce0a6ba5728b93b)。
- **改进：**初始化和批处理表单在受限窗口内更一致地适配；补充字段可访问性和任务状态说明。
  [表单布局](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c170af833aa8b35bed560da0a403d1160b4657c0) · [字段与任务呈现](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c3e667a249e0de1b4384aa3d97d29172e08a474b)。
- **新增：**独立恢复入口及显式重新验证的恢复选项；下载开始前检查可用空间。
  这些更新组件不代表已开放公共签名更新通道，也不证明完整首次安装／更新流程已验收。
  [恢复](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/3ed5a4857ded622e9c188847494bc4d2c59ed650) · [空间预检](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/9db10d37bb0e9d2ce60d38d91c573dab530a6d2e)。
- **文档：**加入真实 3DGS／2DGS 训练阶段与原生界面操作视频，明确说明使用合成输入和
  已保存阶段回放；演示不作为重建画质、精度或性能基准。[源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/7222c1203ebdc791d89d02768baf81cf27275915)。

## 2026-10-28

状态：**已实现**；隔离组件检查不等于整套软件或公共更新通道验收。

- **改进：**初始化内容可在受限窗口内滚动，保留底部操作按钮可达。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/fa1c39fe2fcf5cacd3bb39e266a2b0bc10dc9b7b)。
- **新增：**默认静默的**帮助 → 软件更新**界面，支持三语即时切换。联网仍需兼容的可信引导
  组件和已配置的签名更新源，不表示已有公开服务。[更新界面](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/55064f8c5c5b26d99f6feab1d063423e20af1e7e) · [主程序接入](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/ce3e4e044647dfd59a9508674f77c2ed5db74320)。
- **安全：**加强完整压缩包／源码验证、长 Unicode 更新路径、离线启动检查和显式回退验证。
  这些保护不构成生产签名或公共更新源批准。[路径与回退](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/fa1c39fe2fcf5cacd3bb39e266a2b0bc10dc9b7b) · [离线启动与源码证明](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/ce3e4e044647dfd59a9508674f77c2ed5db74320)。

## 2026-10-27

状态：**已实现**；存在源码提交不代表已有桌面安装完成更新。

- **新增：**随工程保存的自动／固定网格间距、网格／坐标轴可见性、显示单位和只读视口详情。
  用空闲／等待状态替代过期 FPS；显示设置不改变模型尺度，也不代表重建精度。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d9e1d361fa5daee23617852bcacf11cd64156d50)。
- **修复：**隐藏最后一个可见模型时保留画布布局和参考平面，避免改变视口尺寸或重置参考基础。
  [画布布局](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/6ea81a50c0f8641c03056d9f5fa0922096ec8d70) · [参考范围](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/17cef1e1222b07c7e4109de59deab5a3e4294d7d)。
- **新增：**固定原生启动器和经校验的版本目录原子发布；后续修复快捷方式选项保留与活动版本
  指针替换。正在运行和用于回退的版本，与新准备的版本目录相互独立。
  [启动器与发布](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/8653293e4ce7209ce9396cc597a89e199ead4da4) · [快捷方式保留](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/32f8e8d904ea462d5ace2717211170fb54466045) · [原子替换](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/87a797bd2fe5072df1a72e5587414841f62ee494)。
- **修复：**worker 原子发布状态文件时遇到 Windows 短暂文件共享冲突，采用有界重试，
  不再立即失败。[源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b87448ece4bb5664b713881a00ebd553bebb085e)。

## 2026-10-26

状态：**已实现**；后台渲染检查不替代录屏所示前台异常的实际复测。

- **修复：**静态高斯对象拾取改用只读路径；混合模型选择、叠加层独立处理并恢复 OpenGL
  状态和像素读回，选择操作不应修改模型数据或其他模型的渲染状态。
  [高斯拾取](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c8901e2ff70845d23f97a724da34afe8d6552bb6) · [混合模型状态隔离](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/14be3f9aba02d166a5f25db817f67887c5c6cb0f)。
- **修复：**翻译生成统一由共享资源目标负责，避免多个使用方在语言切换时并行读取未完成的目录。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d8df7867e475e4f271eb9c1a505895c6c47b8d90)。

## 2026-10-25

状态：**已实现**；不据此承诺通用加速比例或内存硬上限。

- **改进：**隐藏模型保留坐标身份，视口重绘和资源调度按需进行；隐藏数据推迟不必要的 GPU
  资源申请。[按需调度](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/253b35fec2ebea58822066ce2b280129745f8207) · [隐藏数据资源](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/68186326266ef9cd6abc7a3a0294e4599cc630e9)。
- **修复：**磁盘分页数据导出时保留原始数据来源；包启动检查隔离旧运行残留，不接受过期文件。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/bd521628558d62612c8d077df0bd6a655fd70f03)。

## 2026-10-24

状态：**已实现**；这些查看控件不会自动配准或合并独立重建。

- **新增：**工程树逐模型眼睛按钮，显示／隐藏状态随工程保存；名称选择与可见性更新保持分离。
  [可见性控件](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b7c91307813c46b9258d48e2fdb3c9c365cee3a0) · [保留选择](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/3552eb20374f321cf19a60dbb352681d05761597)。
- **调整：**采用 Blender 衍生轴向导航：点击轴进入对应正交视图，从轴向视图自由旋转时恢复
  透视；平移和缩放保持当前投影，移除中心投影切换控件。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/4627ae3db353f027dbb01e695d0563f3e8ac6ea5)。

## 2026-10-23

状态：**已实现**；实验性方法执行不等于最终画质或独立几何精度验收。

- **新增：**完整源数据的 3DGS／2DGS PLY 变换烘焙，保留属性保真度。不支持的非均匀变换
  与方向球谐组合明确拒绝，不静默近似；双尺度 2DGS 不添加虚构厚度。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/0a7116bba76ee83ad53b0567df91e60e5f079dba)。
- **新增：**默认关闭的实验性质量计划及 Mip 感知交付。3DGS／2DGS 共享工作流身份和遮罩／深度监督，
  但不会把体积方法直接当作等效的面元算法；MCMC 仍需独立验收，在桌面选择器中不可用。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/bc09c7dbe3b103c7b54617b123bd5a89ee1d4120)。
- **修复：**处理等价 Windows 结果路径和持久元数据共享，避免已记录的成果发布／暂停失败。
  [源码](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/0bbea50994a0dea6431672977887d6fc5b9bc470)。

</div>
