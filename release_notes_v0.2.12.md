# Gaussian Scene Workbench 0.2.12

## English

- Preserves Metashape/COLMAP source camera coordinates byte-for-byte and performs every GS2Mesh compatibility conversion in an isolated staging copy.
- Correctly expands mixed-camera COLMAP projects so every image keeps its own camera model, intrinsics, pose, and scale.
- Adds a default-on GS2Mesh Auto Scale mode that profiles rendered depth before fusion and adapts the depth range and voxel size without applying any scene transform (`TSDF_scale=1.0`).
- Splits GS2Mesh into reusable render/depth and TSDF stages, validates caches against camera, checkpoint, runner, and scene identities, and quarantines incompatible caches instead of silently reusing them.
- Falls back to a valid raw TSDF mesh when optional cleaning removes all faces, while still rejecting empty or cross-scene outputs.
- Verified on the 59-camera Metashape-aligned `scene_20260720.r4` dataset with a 1,943,061-vertex, 3,742,704-face mesh in preserved source coordinates.
- Updates the source version to `36.10.12` and the package version to `0.2.12`.

## 中文

- 对 Metashape/COLMAP 源相机数据实行逐字节只读保护，所有 GS2Mesh 兼容转换都只在隔离副本中进行。
- 正确处理混合相机 COLMAP 项目，保证每张图像使用自身对应的相机模型、内参、位姿和尺度。
- 新增默认启用的 GS2Mesh“自动尺度”：先分析渲染深度，再自动调整深度范围与体素大小，全程不改变场景坐标，固定 `TSDF_scale=1.0`。
- 将 GS2Mesh 拆分为可复用的渲染/深度与 TSDF 两阶段；缓存同时校验相机、检查点、运行脚本和场景身份，不兼容缓存会被隔离而不会静默复用。
- 可选清理步骤误删全部面时保留有效的原始 TSDF 网格，同时仍会拒绝空网格或其他场景的输出。
- 已使用 59 个 Metashape 对齐相机的 `scene_20260720.r4` 实测，生成 1,943,061 顶点、3,742,704 面的同坐标尺度网格。
- 源码版本更新至 `36.10.12`，封装版本更新至 `0.2.12`。

## 日本語

- Metashape/COLMAP の元カメラデータをバイト単位で読み取り専用として保護し、GS2Mesh 用の変換はすべて隔離されたステージングコピーだけで実行します。
- 複数カメラを含む COLMAP プロジェクトで、各画像に対応するカメラモデル、内部パラメータ、姿勢、スケールを正しく維持します。
- 既定で有効な GS2Mesh 自動スケールを追加しました。融合前に深度を解析し、座標変換を行わず (`TSDF_scale=1.0`) 深度範囲とボクセルサイズを調整します。
- GS2Mesh を再利用可能なレンダー/深度段階と TSDF 段階に分離し、カメラ、チェックポイント、実行スクリプト、シーン識別情報でキャッシュを検証します。不一致キャッシュは再利用せず隔離します。
- 任意のクリーニングで全ポリゴンが削除された場合は有効な raw TSDF メッシュを保持し、空メッシュや別シーンの出力は引き続き拒否します。
- Metashape で整列した 59 カメラの `scene_20260720.r4` で検証し、元座標系のまま 1,943,061 頂点、3,742,704 面のメッシュを生成しました。
- ソースバージョンを `36.10.12`、パッケージバージョンを `0.2.12` に更新しました。
