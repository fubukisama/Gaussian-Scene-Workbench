# Gaussian Scene Workbench 0.2.6

## English

- Added direct import of Metashape camera projects exported in COLMAP text format.
- Validates every aligned image against its camera dimensions before changing the local dataset.
- Preserves imported camera poses, sparse points, original image dimensions, and Metashape world-coordinate scale for both 3DGS and 2DGS training.
- Converts `SIMPLE_PINHOLE` intrinsics to the projection-equivalent `PINHOLE` representation required by the official Gaussian loaders; focal length, principal point, poses, and scale remain unchanged.
- Archives the original Metashape sparse text files and prevents COLMAP realignment from overwriting imported cameras.
- Shows imported camera count and image dimensions in the aligned-source selector in English, Chinese, and Japanese.
- Verified with the 59-camera, 3804 x 2146 Metashape project in `C:\Users\Ishida_Lab\Desktop\カメラ` and a real 3DGS training smoke run.

## 中文

- 加入 Metashape 导出的 COLMAP 文本相机工程直接导入功能。
- 修改本地数据集前，会逐张核对已对齐图像与相机记录的尺寸。
- 3DGS 与 2DGS 训练均保留导入的相机位姿、稀疏点、原始图像尺寸和 Metashape 世界坐标尺度。
- 将 `SIMPLE_PINHOLE` 内参转换为官方 Gaussian 加载器要求的投影等价 `PINHOLE` 表示；焦距、主点、位姿和尺度均不变。
- 原始 Metashape 稀疏文本文件会单独归档，并禁止 COLMAP 重新对齐覆盖导入相机。
- 中、日、英三语的对齐源列表会显示导入相机数量和图像尺寸。
- 已使用 `C:\Users\Ishida_Lab\Desktop\カメラ` 中 59 个相机、3804 x 2146 图像完成真实导入和 3DGS 训练冒烟验证。

## 日本語

- Metashape から COLMAP テキスト形式で出力したカメラプロジェクトを直接インポートできるようにしました。
- ローカルデータセットを変更する前に、全整列画像とカメラ記録の寸法を検証します。
- 3DGS／2DGS 学習で、カメラ姿勢、疎点群、元画像寸法、Metashape のワールド座標スケールを保持します。
- `SIMPLE_PINHOLE` を公式 Gaussian ローダーが要求する投影等価な `PINHOLE` 表現へ変換します。焦点距離、主点、姿勢、スケールは変わりません。
- 元の Metashape 疎再構成テキストを別途保存し、COLMAP の再実行によるカメラ上書きを防止します。
- 英語・中国語・日本語の整列済みデータ一覧に、カメラ数と画像寸法を表示します。
- `C:\Users\Ishida_Lab\Desktop\カメラ` の 59 カメラ、3804 x 2146 画像で実インポートと 3DGS 学習スモークテストを確認しました。
