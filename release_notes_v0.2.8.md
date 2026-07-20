# Gaussian Scene Workbench 0.2.8

## English

- Fixes false 2DGS environment failures when training from imported Metashape/COLMAP camera data.
- Probes video-import packages in the server Python that actually performs frame extraction, instead of requiring them in the 2DGS training virtual environment.
- Extends the CUDA extension cold-start probe from 20 to 90 seconds to tolerate first-run DLL loading and security scanning.
- Requires video tooling only when the current import contains a video; existing aligned images and Metashape cameras are no longer blocked by unrelated video packages.
- Verified with all 59 imported Metashape cameras in a real 2DGS training smoke run, including Gaussian and checkpoint output.
- Updates the source version to `36.10.8` and the package version to `0.2.8`.

## 中文

- 修复使用已导入的 Metashape/COLMAP 相机数据训练 2DGS 时环境检查误报失败的问题。
- 视频导入组件改由真正负责抽帧的服务端 Python 检查，不再错误要求安装到 2DGS 训练虚拟环境。
- CUDA 扩展冷启动检查由 20 秒延长至 90 秒，兼容首次 DLL 加载及系统安全扫描造成的延迟。
- 只有当前导入内容包含视频时才要求视频工具；已有对齐图片和 Metashape 相机不再被无关视频组件拦截。
- 已使用 59 个导入的 Metashape 相机完成真实 2DGS 冒烟训练，并成功保存 Gaussian 与 checkpoint。
- 源码版本更新至 `36.10.8`，封装版本更新至 `0.2.8`。

## 日本語

- インポート済みの Metashape/COLMAP カメラデータから 2DGS を学習する際の、環境チェックの誤検出を修正しました。
- 動画取り込みパッケージは 2DGS 仮想環境ではなく、実際にフレーム抽出を行うサーバー Python で確認します。
- CUDA 拡張のコールドスタート確認を 20 秒から 90 秒へ延長し、初回 DLL 読み込みやセキュリティスキャンの遅延に対応しました。
- 現在の入力に動画が含まれる場合だけ動画ツールを必須とし、既存の整列済み画像や Metashape カメラを無関係な動画パッケージで停止しません。
- インポートした 59 台分の Metashape カメラで実際の 2DGS スモーク学習を行い、Gaussian とチェックポイントの保存まで確認しました。
- ソースバージョンを `36.10.8`、パッケージバージョンを `0.2.8` に更新します。
