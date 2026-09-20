# Native GSW / LichtFeld Studio / Postshot

核查日期：2026-09-20。原生桌面端已推进 SPZ 交付和完整 SH 显示两批对标升级，不代表已实现两款软件的全部功能。对方功能依据官方公开文档，本地状态依据当前桌面分支代码；没有做同一数据集、同一硬件下的三软件画质/速度排名。

## 对比与优先级

| 工作环节 | 对标依据 | 当前 GSW 状态与差距 |
| --- | --- | --- |
| 训练中查看 | LichtFeld 支持训练中检查；Postshot 提供实时训练预览 | 已有稀疏点云 → 初始高斯 → 中间快照/GPU 预览 → 最终模型的连续显示；不是本轮新增。外部 2DGS 仍依赖检查点。 |
| 高斯交付 | 两者均提供 SPZ；Postshot 文档说明 v3/v4、压缩质量、SH 阶数 | **本轮新增**官方开源编解码、SPZ 导入/导出、版本/质量/SH 选项。已有 PLY/XYZ/CSV/OBJ/STL/GLB 不变；Gaussian GLB 仍是中心点，不是 splat 场景。 |
| 编辑 | LichtFeld 具备高斯选择、变换与历史；Postshot 提供模型变换、裁剪范围 | 已有多对象、选择/删除/撤销与物体变换。尚不能把高斯旋转/尺度连同 SH 正确烘焙到交付文件；本轮不假装支持。 |
| 渲染外观 | Postshot 可选择渲染 SH 阶数及尺度/不透明度 | 已接入 SH 0–4 阶与即时显示阶数控制，采用 gsplat 上游求值源码；属性常驻显存。尚未加入瓦片式 GPU 排序及同等完整的外观面板。 |
| 训练管理 | LichtFeld 支持检查点续训；Postshot 可保存训练上下文 | GSW 有任务停止、检查点结果恢复和工程恢复；这不等价于带优化器状态的桌面暂停/继续。建议单独升级状态机。 |
| 输入质量 | Postshot 提供图像筛选、蒙版与曝光补偿 | GSW 有照片/视频导入及 COLMAP 设置；还没有同等完整的可视化筛图、蒙版与曝光管理工作流。 |
| 输出镜头 | Postshot 有动画时间线 | GSW 的相机轨迹查看不等价于镜头关键帧编辑和离线序列渲染，需独立实现。 |
| 扩展能力 | LichtFeld 公开 Python 插件与 MCP 接口 | GSW 暂不提供同等原生插件接口；本轮不新增网络控制服务。 |

官方依据：[LichtFeld README](https://github.com/MrNeRF/LichtFeld-Studio)、[Postshot 概览](https://www.jawset.com/)、[导出](https://activation.jawset.com/docs/d/Postshot%2BUser%2BGuide/Interface/Radiance%2BField%2BExport)、[模型与训练上下文](https://activation.jawset.com/docs/d/Postshot%2BUser%2BGuide/Interface/Scene%2BTree/Rdnc%2BField)、[训练配置](https://activation.jawset.com/docs/d/Postshot%2BUser%2BGuide/Interface/Training%2BConfiguration)、[时间线](https://activation.jawset.com/docs/d/Postshot%2BUser%2BGuide/Interface/Timeline)。

本地核查入口：`TrainingDialog.cpp`、`MainWindow.cpp`、`NativeViewport.cpp`、`ModelExport.cpp`、`CameraTrajectory.cpp`、`native/worker/training_preview.py`，以及 `CONTINUOUS_TRAINING_PREVIEW.md`。

## 第一批落地：SPZ 高斯交付

1. **文件 → 导出模型**：高斯模型增加 SPZ；v4 为默认并行 Zstandard，v3 为旧查看器提供 gzip 兼容选项。
2. 三档 SH 量化：紧凑 4/3 bit、均衡 5/4 bit、高精度 8/8 bit（分别为 1 阶/其余阶）。默认保留源 SH 阶数，可限制到 0–4。绝不为了压缩而抽点；只排除用户已删除的高斯。
3. **文件 → 导入模型（PLY / SPZ）**：标准 SPZ v1–v4 在后台解码到工程内部 PLY 工作副本，再通过已有原生加载/选择/编辑路径使用。支持与其他对象并存；源 SPZ 不改写。
4. PLY 原始属性 → 官方 `GaussianCloud` → `saveSpz`；导入使用 `loadSpzPacked` / `unpack`。RDF ↔ RUB 的位置、旋转和 SH 符号变换直接使用官方实现。不是改扩展名，也不是用点云颜色冒充高斯外观。
5. 三语立即切换，失败/取消不覆盖现有文件；解码后的工作副本随工程另存与恢复迁移。

源码：[Niantic/Adobe SPZ](https://github.com/nianticlabs/spz/tree/affd0ecea7fbb4c265ee119475af7ee5b2997482)，MIT。仓库内的固定版本及许可证见 `native/third_party/spz/README.gsw.md`。LichtFeld 主程序的 GPL 代码没有混入现有许可组合；未找到可用于本轮集成的 Postshot 官方公开实现源码，因此只对照其公开工作流。

### 明确边界

- SPZ 所有档位都是有损量化；降低 SH 阶数会进一步损失视角相关外观。它不是检查点、不是无损归档。
- 原始坐标导出，不烘焙物体变换；编码精度约为 1/4096 源单位，坐标须在约 ±2048 源单位内。越界时报错并建议 PLY，不会偷偷移动原点。
- 不保存 CRS、物理单位、任意自定义 PLY 字段和优化器状态。按 SPZ 的量化规则，尺度、颜色、SH 的可表达范围也有限。
- 不支持带扩展/未知标志的 SPZ；避免静默丢失坐标系扩展。Mip-splat 抗锯齿标志会保留在工作 PLY 专用注释中，但当前视口不宣称具备该渲染模式。
- 官方编解码器驻留 RAM，转换采用 2 GiB 保守工作预算（输入 SPZ 文件至多 1 GiB）；不是跨磁盘分页编解码。取消在阶段边界生效，压缩期间保持界面事件处理。
- 第一批 SPZ 交付时仅显示 DC；第二批已接入下面的完整 SH 显示。交付阶数与显示阶数独立，降低显示阶数不会损失文件属性。

### 验证

`model_export` 覆盖数量/删除、v3/v4、SH0–4、坐标和四元数/SH 往返、v2 上游样本、饱和不透明度、无效输入、取消、源文件变化和原子写入。`native_spz_interchange` 操作真实导出对话框和导入流程，验证同时加载、工程另存/重开、导入失败保留未保存裁剪。三语测试验证 SPZ 参数不会随切换语言重置。额外真实文件 QA 通过 `GSW_SPZ_SOURCE` 或 `--smoke-test-spz --smoke-scene` 运行，不改写源文件。

本地 512,202 高斯、SH3 样本使用默认 v4/均衡设置：原 PLY 127,027,627 字节，SPZ 9,605,279 字节（减少约 92.4%）；往返保持高斯数量，包围盒坐标误差小于 0.001 源单位。这是有损文件体积实测，不是无损或渲染加速指标，也未与其他软件做同场景画质排名。

## 第二批落地：SH 0–4 阶显示

1. 工具栏 **球谐显示** 或 **视图 → 渲染模式 → 球谐显示**：自动采用源阶数，或即时切换 0–4 阶上限。低阶减少着色计算；不重新载入模型、不修改源文件和导出设置，也不额外抽点。
2. 在读取 PLY 的同一遍处理中保留原始 DC 与高阶系数；兼容 ASCII、大小端二进制和 SPZ 解码工作副本。独立 RGB float 数组不扩大普通点云的顶点结构；GPU 系数缓冲在转动相机和切换显示阶数时复用。
3. 直接移植 Apache-2.0 的 [gsplat 前向 SH 求值源码](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/gsplat/cuda/csrc/SphericalHarmonicsCUDA.cu)，保留署名、固定提交和许可证。仅适配 GLSL，不引入 CUDA/PyTorch 运行时。
4. 求值方向在对象局部空间计算，覆盖透视、正交、物体旋转及非均匀缩放。删除/撤销和深度重排通过源索引映射系数；选择颜色仍保留。
5. 状态栏显示实际生效的阶数。源字段不完整、系数非有限、单模型 SH CPU 预算超出 512 MiB 或设备缓冲限制时明确显示 DC 回退。当前实时共享显存训练协议仍只发送基础颜色；快照 PLY 可以显示其真实 SH。超过编辑上限而走点云分页的场景不是完整高斯 SH 分页。

验证入口：`gaussian_spherical_harmonics` 用独立连带勒让德递推核对 25 个基函数、五档阶数、多方向 GPU 输出和删除/重排映射；`workspace_document` 核对格式、采样、属性排列及异常输入；`native_gaussian_interaction` 检查真实视口颜色、物体变换、正交投影、常驻/兼容路径画面一致性以及转动时不重新上传 SH。三语测试覆盖即时切换且不重置质量设置。

剩余优先级：优化器状态暂停/续训 → 输入图像质量/蒙版/曝光工作流 → 镜头关键帧与离线序列输出。瓦片式 GPU 排序、抗锯齿训练模型的等价渲染、带 SH 的变换烘焙仍需独立升级。

### 第二批验证记录（2026-09-20）

- 全量 39/39 CTest、51/51 Python 后端测试通过；中英日切换和占位符校验通过。
- 独立安装包、仅系统 PATH 下验证真实 512,202 高斯 PLY：源 SH3 → 实际 SH3，保留全部已加载高斯；原文件 SHA-256 前后均为 `6C94FBDCA4F8FFFA3CCA18A2AC24CF3CC80887045A0CC967C80906D56E8F80BC`。
- 此机此视图在 1674×1032 帧缓冲下，含读取回帧缓冲的中位耗时：静止 4.13 ms、旋转 8.84 ms、滚轮 3.98 ms；旋转输入处理 p95 0.085 ms。不是所有模型/显卡的性能保证，也不是与竞品的速度对比。
- 索引常驻路径与兼容路径最大颜色差 1/255；导航/阶数切换不重新上传 SH，选择、删除、撤销及场景清理通过。
- 同一模型经 SPZ 解码后的 PLY 工作副本也通过安装版 SH3 显示、512,202 数量、重排/编辑和路径一致性检查。
- 安装包 `Gaussian-Scene-Workbench-0.3.1-native-full-sh-win-x64`；应用 SHA-256：`957E52A55E626647A7B3AAB412DF0023F20D5CFE3CD6DFD5FF88D10BF099DECB`。

QA summary: 39 native tests and 51 backend tests passed. The installed build rendered all 512,202 loaded Gaussians at source SH3 without changing the input file; navigation and degree switches reused SH buffers. Timings above are local measurements, not cross-product benchmarks.

検証概要：ネイティブ 39 件とバックエンド 51 件が成功しました。インストール版で元ファイルを変更せず、読み込んだ 512,202 ガウシアンすべてをソースの SH3 で描画し、視点・次数切り替えで SH バッファを再利用しました。上記時間はローカル測定であり、他製品との比較ではありません。

## English

These are scoped comparison-driven upgrades, not full parity. GSW already has continuous training previews and multi-object editing. Batch one added true SPZ interchange using pinned Niantic/Adobe MIT source. Batch two adds SH degrees 0–4 using the actual gsplat Apache-2.0 evaluator, live source-capped display quality and resident coefficient buffers. Display quality does not change exported data. Malformed/unavailable coefficients and resource limits use an explicitly labeled DC fallback; live shared-memory training remains DC, while PLY snapshots support SH. The 512 MiB per-model CPU SH budget and device limits apply; this is not disk-paged Gaussian rendering. Optimizer-state pause/resume, image-quality management, tile-based rendering and camera-animation output remain separate priorities.

## 日本語

比較に基づく段階的な改良で、全機能の同等性を意味しません。第一段階は Niantic/Adobe の MIT ソースを直接利用する SPZ 入出力、第二段階は gsplat の Apache-2.0 評価ソースによる SH 0–4 次描画と即時の表示次数切り替えです。係数は GPU に常駐し、表示品質を変えてもエクスポートデータは変更しません。不完全・非有限の係数やリソース上限では DC フォールバックを明示します。共有 GPU 学習プレビューは引き続き DC、PLY スナップショットは SH に対応します。モデルごとに CPU SH メモリ 512 MiB とデバイス上限があり、ディスクページング式ガウシアン描画ではありません。最適化状態の一時停止・再開、画像品質管理、タイル描画、カメラアニメーション出力は今後の別工程です。
