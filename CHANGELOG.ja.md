<div lang="ja">

# 変更履歴

[English](CHANGELOG.md) · [简体中文](CHANGELOG.zh-CN.md) · [日本語](CHANGELOG.ja.md)

Gaussian Splatting Workbench の利用者向け変更を新しい順に記録します。今回の補記は
2026-10-23～30 の要約であり、それ以前の全変更を復元したものではありません。
日付は日本時間です。[完全なソース履歴](https://github.com/fubukisama/GaussianSplattingWorkbench/commits/main/)。

**状態の意味：** **実装済み**はリンク先のソース変更が存在することを示します。
全検証の完了、公開 Release、特定の PC へのインストールを意味しません。それぞれ
対象バージョン固有の証拠が必要です。2026-10-30 時点のソースバージョンは
`0.3.9-native-preview` で、公開ネイティブ Release はまだありません。
配布状況と要件は[入手ガイド](README_RELEASE.md)と[更新・ロールバック](docs/NATIVE_UPDATES.md)
を参照してください。CI 成果物やローカルプレビューの配布は、自動的に公開 Release にはなりません。

今後の利用者向け機能の追加・変更・修正は、同じ変更内で三言語の履歴を更新し、
対象機能、制限、検証または配布状況を記載します。実装・検証・公開・インストールは
それぞれ別の状態として扱います。

## 2026-10-30

状態：**実装済み**。本節はソース記録であり、公開・インストールの証明ではありません。

- **変更：**日付付きの英語・簡体字中国語・日本語の更新履歴とホームページの入口を復元。
  継続的な履歴更新を共通プロジェクト規則とし、Codex プロジェクトの分岐チャットにも共有します。
  文書チェックは三言語の履歴・日付・ソース参照とホームページの入口を検査します。
- **修正：**タスクバーの再起動メタデータは実行中アプリのリソースから表示ブランドを取得し、
  起動先には固定ランチャーを維持します。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/e1ec20a69f7a44523bc747fbbb71000984864684)。
- **改善：**外観の自動設定、アイドル時の一時停止、利用できない再開の理由を、メニューの
  アクセシビリティ経由でも説明します。
  [外観の説明](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/aa09ab26ba637387f3b1049dc11492916cce2b11) · [一時停止・再開の説明](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d87343fa215389815c6cf037aef85153a5bf6b91)。

## 2026-10-29

状態：**実装済み**。検証・配布は、別途記録された対象ソースバージョンに基づきます。

- **変更：**製品の表示名を **Gaussian Splatting Workbench** に変更しました。既存の
  実行ファイル名、設定キー、アプリケーション ID の互換性は維持します。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b0aacf85bb5e87b82b6bfcde0ce0a6ba5728b93b)。
- **改善：**初期化・バッチ画面の小さなウィンドウへの適合、入力欄のアクセシビリティ、
  ジョブ状態の表示を改善しました。
  [画面配置](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c170af833aa8b35bed560da0a403d1160b4657c0) · [入力欄とジョブの表示](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c3e667a249e0de1b4384aa3d97d29172e08a474b)。
- **追加：**独立した復旧入口、明示的に再検証する復旧候補、ダウンロード前の空き容量確認。
  これらの更新コンポーネントは公開署名付き更新チャネルの開始や、初回インストール・
  更新全体の検証完了を意味しません。
  [復旧](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/3ed5a4857ded622e9c188847494bc4d2c59ed650) · [容量の事前確認](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/9db10d37bb0e9d2ce60d38d91c573dab530a6d2e)。
- **文書：**実際の 3DGS・2DGS 学習段階とネイティブ操作の動画を追加。合成入力と保存段階の
  再生であることを明示し、再構成品質・精度・性能ベンチマークとは扱いません。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/7222c1203ebdc791d89d02768baf81cf27275915)。

## 2026-10-28

状態：**実装済み**。分離コンポーネントの検査は、製品全体・公開チャネルの受入検証ではありません。

- **改善：**初期化画面は限られたウィンドウ内で内容をスクロールし、操作ボタンを利用可能に
  保ちます。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/fa1c39fe2fcf5cacd3bb39e266a2b0bc10dc9b7b)。
- **追加：**既定で静かに動作する**ヘルプ → ソフトウェア更新**画面と、即時に切り替わる
  三言語表示。オンライン利用には互換性のある信頼済みブートストラップと設定済み署名付き
  更新元が必要で、公開サービスの開始を示しません。
  [更新画面](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/55064f8c5c5b26d99f6feab1d063423e20af1e7e) · [本体への統合](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/ce3e4e044647dfd59a9508674f77c2ed5db74320)。
- **安全性：**完全なアーカイブ・ソース検証、長い Unicode 更新パス、オフライン起動検査、
  明示的なロールバック検証を強化。製品用署名や公開更新元の承認とは別です。
  [パスとロールバック](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/fa1c39fe2fcf5cacd3bb39e266a2b0bc10dc9b7b) · [オフライン起動とソースの証明](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/ce3e4e044647dfd59a9508674f77c2ed5db74320)。

## 2026-10-27

状態：**実装済み**。ソースのコミットだけでは、既存のデスクトップ更新を証明しません。

- **追加：**プロジェクトに保存する自動・固定グリッド間隔、グリッド・軸の表示、表示単位、
  読み取り専用のビューポート詳細。古い FPS 表示をアイドル・待機状態に置き換えます。
  表示設定はモデル尺度を変更せず、再構成精度も表しません。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d9e1d361fa5daee23617852bcacf11cd64156d50)。
- **修正：**最後の可視モデルを非表示にしてもキャンバス配置と基準面を保持し、
  ビューポートの寸法や基準が変わらないようにしました。
  [キャンバス配置](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/6ea81a50c0f8641c03056d9f5fa0922096ec8d70) · [基準範囲](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/17cef1e1222b07c7e4109de59deab5a3e4294d7d)。
- **追加：**固定ネイティブランチャーと、検証したバージョンディレクトリの原子的な公開。
  続いてショートカット設定の保持とアクティブ版ポインターの置換を修正しました。
  実行中・ロールバック用バージョンは新しい準備済みディレクトリとは分離します。
  [ランチャーと公開](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/8653293e4ce7209ce9396cc597a89e199ead4da4) · [ショートカットの保持](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/32f8e8d904ea462d5ace2717211170fb54466045) · [原子的な置換](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/87a797bd2fe5072df1a72e5587414841f62ee494)。
- **修正：**worker が状態ファイルを原子的に公開する際の一時的な Windows 共有競合に対し、
  即失敗ではなく回数を制限した再試行を行います。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b87448ece4bb5664b713881a00ebd553bebb085e)。

## 2026-10-26

状態：**実装済み**。バックグラウンド描画検査は、録画された前面表示不具合の再確認を代替しません。

- **修正：**静的ガウスモデルの選択判定を読み取り専用にし、混合モデルの選択とオーバーレイで
  OpenGL 状態・読み出しを分離して復元します。選択によって元データや他モデルの描画状態を
  意図的に変更しません。
  [ガウスの選択判定](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c8901e2ff70845d23f97a724da34afe8d6552bb6) · [混合モデルの状態分離](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/14be3f9aba02d166a5f25db817f67887c5c6cb0f)。
- **修正：**翻訳生成を単一の共有リソース所有者に統一し、言語切り替え中に並列の利用先が
  未完成のカタログを読み込む競合を防ぎます。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d8df7867e475e4f271eb9c1a505895c6c47b8d90)。

## 2026-10-25

状態：**実装済み**。一律の高速化率やメモリ上限の保証は行いません。

- **改善：**非表示モデルの座標識別を保持しつつ、ビューポートの再描画とリソース割り当てを
  必要時に行い、非表示データの不要な GPU 受入を遅延します。
  [必要時の処理](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/253b35fec2ebea58822066ce2b280129745f8207) · [非表示データのリソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/68186326266ef9cd6abc7a3a0294e4599cc630e9)。
- **修正：**ディスクページングされたデータの出力で元ソースを保持。パッケージ起動検査は
  過去の実行の残存物を分離し、古いファイルを受け入れません。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/bd521628558d62612c8d077df0bd6a655fd70f03)。

## 2026-10-24

状態：**実装済み**。表示操作は、別々の再構成を自動位置合わせ・統合するものではありません。

- **追加：**プロジェクトツリーにモデルごとの目のボタンを追加し、可視状態をプロジェクトに保存。
  名前の選択と可視状態の変更を分離します。
  [可視状態の操作](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b7c91307813c46b9258d48e2fdb3c9c365cee3a0) · [選択の保持](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/3552eb20374f321cf19a60dbb352681d05761597)。
- **変更：**Blender 由来の軸ナビゲーションで、軸クリックは対応する平行投影へ移り、軸ビューからの
  自由回転は透視投影へ戻ります。パン・ズームは投影を維持し、中央の投影切替を削除しました。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/4627ae3db353f027dbb01e695d0563f3e8ac6ea5)。

## 2026-10-23

状態：**実装済み**。実験的手法の実行は、最終画質や独立した形状精度の受入検証ではありません。

- **追加：**属性忠実性を保つ、完全な元データの 3DGS・2DGS PLY 変換ベイク。方向依存の
  球面調和関数と非一様変換の非対応な組合せは暗黙に近似せず拒否し、2 軸の 2DGS に
  架空の厚みを追加しません。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/0a7116bba76ee83ad53b0567df91e60e5f079dba)。
- **追加：**既定で無効の実験的品質計画と Mip を考慮した配布。3DGS・2DGS は共通の
  ワークフロー識別とマスク・深度の教師信号を共有しますが、体積ガウス手法を同等のサーフェルアルゴリズム
  とは扱いません。MCMC は独自の受入検証が必要で、デスクトップでは選択できません。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/bc09c7dbe3b103c7b54617b123bd5a89ee1d4120)。
- **修正：**同等の Windows 結果パスと永続メタデータの共有を処理し、記録された結果公開・
  一時停止の失敗を防ぎます。
  [ソース](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/0bbea50994a0dea6431672977887d6fc5b9bc470)。

</div>
