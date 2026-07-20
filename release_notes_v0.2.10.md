# Gaussian Scene Workbench 0.2.10

## English

- Prevents Asset Manager and Experiment Manager actions from replacing progress or starting competing UI work while a large mesh is loading.
- Assigns a verified free TCP port to every active 3DGS and 2DGS training process instead of letting parallel jobs collide on port 6009.
- Assigns GS2Mesh its own task port and records the allocated port, purpose, and release state in job snapshots and logs.
- Keeps the desktop/web server policy separate: packaged builds use 7860 and development uses 7862.
- Confirms that COLMAP alignment, SuGaR extraction, texture baking, PSNR analysis, splat export, and mesh viewing do not open task-listening ports.
- Updates the source version to `36.10.10` and the package version to `0.2.10`.

## 中文

- 大型网格加载期间锁定资产管理和实验管理入口，避免刷新状态或启动竞争性的前端操作。
- 每个活动的 3DGS、2DGS 训练进程都会获得经过占用检测的独立 TCP 端口，不再让并行任务争用默认的 6009。
- GS2Mesh 使用独立任务端口，并在任务快照与日志中记录端口、用途和释放状态。
- 桌面/Web 服务端口策略保持独立：封装版使用 7860，开发版使用 7862。
- 已确认 COLMAP 对齐、SuGaR 抽取、纹理烘焙、PSNR、Splat 导出和网格阅览不会启动任务监听端口。
- 源码版本更新至 `36.10.10`，封装版本更新至 `0.2.10`。

## 日本語

- 大規模メッシュの読み込み中はアセット管理と実験管理をロックし、進捗表示の上書きや競合する UI 操作を防ぎます。
- 実行中の各 3DGS・2DGS 学習プロセスに空き確認済みの専用 TCP ポートを割り当て、既定の 6009 の競合を防ぎます。
- GS2Mesh にも専用タスクポートを割り当て、ポート番号、用途、解放状態をジョブ情報とログに記録します。
- デスクトップ/Web サーバーポリシーは分離したままです。パッケージ版は 7860、開発版は 7862 を使用します。
- COLMAP アライメント、SuGaR、テクスチャベイク、PSNR、Splat 出力、メッシュ表示はタスク用待受ポートを開かないことを確認しました。
- ソースバージョンを `36.10.10`、パッケージバージョンを `0.2.10` に更新します。
