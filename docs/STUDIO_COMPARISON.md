# Native GSW / LichtFeld Studio / Postshot

核查日期：2026-09-19。本轮是原生桌面端的第一批对标升级，不代表已实现两款软件的全部功能。对方功能依据官方公开文档，本地状态依据当前桌面分支代码；没有做同一数据集、同一硬件下的三软件画质/速度排名。

## 对比与优先级

| 工作环节 | 对标依据 | 当前 GSW 状态与差距 |
| --- | --- | --- |
| 训练中查看 | LichtFeld 支持训练中检查；Postshot 提供实时训练预览 | 已有稀疏点云 → 初始高斯 → 中间快照/GPU 预览 → 最终模型的连续显示；不是本轮新增。外部 2DGS 仍依赖检查点。 |
| 高斯交付 | 两者均提供 SPZ；Postshot 文档说明 v3/v4、压缩质量、SH 阶数 | **本轮新增**官方开源编解码、SPZ 导入/导出、版本/质量/SH 选项。已有 PLY/XYZ/CSV/OBJ/STL/GLB 不变；Gaussian GLB 仍是中心点，不是 splat 场景。 |
| 编辑 | LichtFeld 具备高斯选择、变换与历史；Postshot 提供模型变换、裁剪范围 | 已有多对象、选择/删除/撤销与物体变换。尚不能把高斯旋转/尺度连同 SH 正确烘焙到交付文件；本轮不假装支持。 |
| 渲染外观 | Postshot 可选择渲染 SH 阶数及尺度/不透明度 | 当前 GSW 视口为 DC SH，完整属性保存在源文件。建议下一批优先补完整 SH 渲染与可控的性能档位。 |
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
- 视口仍为 DC SH；高阶 SH 被保留并能再次导出供支持它的查看器使用。这一批没有把 SPZ 支持伪装成完整 SH 渲染升级。

### 验证

`model_export` 覆盖数量/删除、v3/v4、SH0–4、坐标和四元数/SH 往返、v2 上游样本、饱和不透明度、无效输入、取消、源文件变化和原子写入。`native_spz_interchange` 操作真实导出对话框和导入流程，验证同时加载、工程另存/重开、导入失败保留未保存裁剪。三语测试验证 SPZ 参数不会随切换语言重置。额外真实文件 QA 通过 `GSW_SPZ_SOURCE` 或 `--smoke-test-spz --smoke-scene` 运行，不改写源文件。

本地 512,202 高斯、SH3 样本使用默认 v4/均衡设置：原 PLY 127,027,627 字节，SPZ 9,605,279 字节（减少约 92.4%）；往返保持高斯数量，包围盒坐标误差小于 0.001 源单位。这是有损文件体积实测，不是无损或渲染加速指标，也未与其他软件做同场景画质排名。

## English

This is the first scoped comparison-driven upgrade, not full parity. GSW already has continuous training previews and multi-object editing. This batch adds true SPZ Gaussian interchange using pinned Niantic/Adobe MIT source, quality/SH/version controls, asynchronous project-owned import, atomic export and live trilingual UI. Lossy delivery does not replace PLY archives or optimizer checkpoints. Complete SH rendering, optimizer-state pause/resume, image-quality management and camera-animation output remain separate priorities. See the limits above and `LOCALIZATION.md` for the export contract.

## 日本語

今回は比較に基づく第一段階の改良で、全機能の同等性を意味しません。既存の連続学習プレビューと複数モデル編集に、Niantic/Adobe の MIT ソースを直接利用する SPZ 入出力、品質・球面調和関数の次数・バージョン設定を追加しました。非同期インポートとアトミックな保存により元ファイルを保護し、三言語は即時切り替えに対応します。SPZ は非可逆の配布形式で、PLY 原本や学習チェックポイントの代替ではありません。完全な SH 描画、最適化状態を保つ一時停止・再開、画像品質管理、カメラアニメーション出力は今後の別工程です。
