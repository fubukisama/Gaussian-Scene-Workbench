<div lang="ja">

<p align="center">
  <img src="native/assets/gsw-app-icon.png" width="88" alt="Gaussian Splatting Workbench のアイコン">
</p>

<h1 align="center" lang="en">Gaussian Splatting Workbench</h1>

<p align="center">
  <strong>写真からガウスシーンへ。再構築・学習・編集を一つのネイティブ環境で。</strong>
</p>

<p align="center">
  <a href="README_RELEASE.md#プレビュー版の入手" title="ネイティブプレビューを入手"><img src="https://img.shields.io/badge/native_preview-0.3.8-2563eb?style=flat-square" alt="ネイティブプレビュー 0.3.8"></a>
  <a href="README_RELEASE.md#利用前の確認" title="Windows 対応環境と実行環境の要件"><img src="https://img.shields.io/badge/platform-Windows_x64-334155?style=flat-square" alt="Windows x64"></a>
  <a href="native/LICENSE.md"><img src="https://img.shields.io/badge/desktop_license-GPL--3.0--or--later-16a34a?style=flat-square" alt="独自ネイティブ主プログラム：GPL-3.0-or-later"></a>
</p>

<p align="center">
  <a href="README.md" lang="en">English</a> · <a href="README.zh-CN.md" lang="zh-CN">简体中文</a> · <strong lang="ja">日本語</strong>
</p>

<p align="center">
  <a href="README_RELEASE.md#日本語"><strong>Windows プレビューを入手</strong></a> ·
  <a href="docs/README.md">ドキュメント</a> ·
  <a href="native/README.md">ソースからビルド</a> ·
  <a href="https://github.com/fubukisama/Gaussian-Scene-Workbench/issues">不具合を報告</a>
</p>

---

<strong lang="en">Gaussian Splatting Workbench (GSW)</strong> は、Gaussian Splatting・写真測量・点群・メッシュを扱う Qt / C++ のネイティブデスクトップです。写真、動画、既知カメラ、既存モデルから作業を始められます。

## 一つの環境、六つの基本機能

| 再構築と学習 | 確認と出力 |
| --- | --- |
| **シーンを再構築**<br>写真・動画の追加、データセットの関連付け、COLMAP の実行。 | **モデルを表示**<br>ガウシアン、2DGS surfel、点群、テクスチャ付きメッシュを共通のビューで確認。 |
| **学習を管理**<br>3DGS / 2DGS の実験、進捗確認、安全な一時停止、互換性のある完全状態からの再開。 | **シーンを編集**<br>対応モデルの選択・変換・クロップ。取り消しと元ファイルを保持。 |
| **座標を追跡**<br>固定カメラ、単位、スケール制約、学習・検証データの分割を記録。 | **抽出と評価**<br>PLY / SPZ 出力、オプションのメッシュ生成パイプライン、独立した参照データとの形状比較。 |

## 3 ステップで開始

1. **プレビュー版を導入。** [入手ガイド](README_RELEASE.md#日本語)でインストーラーと必要な環境を確認。
2. **シーンを追加。** 写真・動画、データセット、または既存の PLY / SPZ モデルを取り込み。
3. **作業を選択。** 学習、確認、編集、出力へ。[目的別ガイド →](docs/README.md)

> **本版はプレビューで、完全な学習環境は同梱しません。** 再構築・CUDA 学習には互換性のある外部環境が必要です。完全状態の再開は対応するネイティブ 3DGS / 2DGS チェックポイントに限ります。描画深度の成果は外観プロキシであり、物理的な材質境界や形状精度の認証ではありません。

<details>
<summary><strong>ネイティブビューポートを見る</strong></summary>

![四つの独立した合成レイヤーを同時表示するネイティブビューポート](docs/images/native-viewport-fixture.png)

独立した合成回帰シーンの実際の画面です。混在モデルの表示例であり、再構築品質の作例ではありません。[画像の出典](docs/images/README.md)。

</details>

## 詳しく見る

[学習と再開](docs/TRAINING_RESUME.md) · [既知カメラとスケール](docs/CAMERA_SCALE_P0.md) · [形状と独立評価](docs/GENERAL_RECONSTRUCTION.md) · [対応パイプライン](docs/GENERATION_PIPELINES.md) · [表示と操作](docs/VIEWPORT_DISPLAY.md)

## ライセンス

独自のネイティブ主プログラムは **GPL-3.0-or-later** です。対象外の独自コードと第三者コンポーネントは各ライセンスを維持します。Graphdeco の研究用バックエンドには別途非商用の制限があり、主プログラムの GPL は全バックエンドの商用許諾を意味しません。

[ライセンス案内](docs/LICENSING.md#日本語) · [適用範囲](native/LICENSE.md) · [リポジトリーのライセンス](LICENSE) · [第三者の通知](THIRD_PARTY_LICENSES.md)

</div>
