# Gaussian Scene Workbench 0.2.14

## English

- Fixes 2DGS mesh export when the extracted mesh has fewer connected components than the requested cluster count.
- Automatically recovers a freshly generated raw 2DGS mesh without rerunning image rendering or TSDF integration if upstream post-processing fails.
- Rejects stale raw meshes during recovery, preventing outputs from different runs or scenes from being mixed.
- Derives bounded 2DGS TSDF voxel size from the trained Gaussian extent instead of distant camera radius, preventing extremely coarse meshes while preserving source camera poses, coordinates, and scale.
- Keeps explicit voxel, depth-truncation, and SDF-truncation settings as manual overrides and reports the effective automatic values in the task log.
- Adds regression coverage for connected-component bounds, stale-file isolation, and the complete recovery job path.
- Updates the source version to `36.10.14` and the package version to `0.2.14`.

## 中文

- 修复 2DGS 提取网格的实际连通块少于请求保留数量时，后处理发生数组越界的问题。
- 当上游后处理失败但本次原始网格已生成时，自动恢复结果，不重复执行图像渲染和 TSDF 融合。
- 恢复时拒绝旧任务留下的原始网格，防止不同任务或场景的结果混用。
- 有界 2DGS 的 TSDF 体素改为按已训练 Gaussian 的实际范围自动推导，不再受远距离相机半径影响；源相机位姿、坐标和比例保持不变。
- 显式指定的体素、深度截断和 SDF 截断仍优先使用，并在任务日志中显示实际自动参数。
- 新增连通块边界、旧文件隔离和完整任务恢复流程的回归测试。
- 源码版本更新至 `36.10.14`，封装版本更新至 `0.2.14`。

## 日本語

- 抽出された 2DGS メッシュの連結成分数が指定保持数より少ない場合に、後処理で配列範囲外エラーが発生する問題を修正しました。
- 今回生成された raw メッシュが存在する場合、上流後処理の失敗後に画像レンダリングや TSDF 統合を再実行せず自動復旧します。
- 復旧時に古い raw メッシュを拒否し、別タスクや別シーンの出力混在を防止します。
- 有界 2DGS の TSDF ボクセルを遠距離カメラ半径ではなく学習済み Gaussian の実範囲から自動算出し、元のカメラ姿勢、座標、スケールを保ったまま粗すぎるメッシュを防止します。
- 明示したボクセル、深度打ち切り、SDF 打ち切り設定を優先し、自動算出された実効値をタスクログに表示します。
- 連結成分の境界、古いファイルの分離、タスク全体の復旧経路に対する回帰テストを追加しました。
- ソースバージョンを `36.10.14`、パッケージバージョンを `0.2.14` に更新しました。
