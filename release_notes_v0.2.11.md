# Gaussian Scene Workbench 0.2.11

## English

- Fixes GS2Mesh output collection so a mesh from another scene can never be copied into the active scene.
- Requires the cleaned mesh to match the active scene, iteration, downsample, baseline, voxel, and depth-range settings.
- Rejects stale or empty GS2Mesh PLY files instead of reporting a false success.
- Preserves completed render and depth caches when final TSDF fusion fails, allowing a retry without recomputing those stages.
- Updates the source version to `36.10.11` and the package version to `0.2.11`.

## 中文

- 修复 GS2Mesh 输出收集逻辑，禁止将其他场景的网格复制到当前场景。
- 清理后的网格必须匹配当前场景、迭代、降采样、基线、体素和深度范围参数。
- 过期或空的 GS2Mesh PLY 不再被误报为成功。
- 最终 TSDF 融合失败时保留已完成的渲染和深度缓存，重试时无需重新计算这些阶段。
- 源码版本更新至 `36.10.11`，封装版本更新至 `0.2.11`。

## 日本語

- 別シーンのメッシュが現在のシーンへコピーされる GS2Mesh 出力収集不具合を修正しました。
- cleaned mesh はシーン、反復、ダウンサンプル、ベースライン、ボクセル、深度範囲と一致する必要があります。
- 古い、または空の GS2Mesh PLY を成功として扱わないようにしました。
- 最終 TSDF 統合に失敗してもレンダリング・深度キャッシュを保持し、再計算なしで再試行できます。
- ソースバージョンを `36.10.11`、パッケージバージョンを `0.2.11` に更新しました。
