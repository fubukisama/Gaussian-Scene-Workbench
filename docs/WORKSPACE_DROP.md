# Workspace drag-and-drop import

## 简体中文

可从 Windows 资源管理器将本地文件拖入 GSW 工作区。拖动时显示操作提示，松开后走相应的原生导入流程；默认使用复制语义，不移动或删除源文件。

| 拖入内容 | 行为 |
| --- | --- |
| 一个 `.gsw.json` 或 `.gswjson` 工程 | 打开工程，而非追加到当前场景；切换前处理当前工程的未保存修改。坏工程先验证，不会因验证失败清空当前编辑。 |
| 一个或多个 `.ply`、`.spz` 模型 | 逐个追加，保留现有对象、变换和裁剪。保留源坐标；只沿用已有的已声明单位换算规则，不自动居中或配准。 |
| 照片、视频、媒体目录 | 打开媒体导入窗口，可调整来源列表与视频抽帧设置；确认后才导入。目录的递归扫描在确认导入后执行。不会自动重建或开始训练。 |

当前模型存在尚未导出的裁剪时，追加模型后仍保持旧对象为活动对象，并保留当前视角；新对象在后台载入，避免切走后隐藏裁剪提醒。没有未导出裁剪时，新模型照常成为活动对象。之后切换对象、打开其他工程或退出时，仍会询问导出、放弃或取消。裁剪结果需要导出；仅保存工程 JSON 并不保存这些裁剪操作。

照片支持 `.jpg`、`.jpeg`、`.png`、`.bmp`、`.tif`、`.tiff`、`.webp`；视频支持 `.mp4`、`.mov`、`.avi`、`.mkv`、`.webm`、`.m4v`。视频能否解码还取决于已配置的 FFmpeg 与文件实际编码，不因扩展名受支持就保证所有视频可用。同一媒体批次可以混合照片、视频及目录；抽帧仍使用现有导入流程，不直接把视频当作模型载入视口。

工程须单独拖入；模型与媒体须分批。网页链接、失效路径、不支持的文件类型，以及不同类别的混合批次会被拒绝并显示原因。当前任务或其他导入操作未结束时，不接受新的拖入操作；不会用拖放中断、续训或替换正在运行的任务。取消媒体设置不会启动导入。

这是借鉴 Metashape 拖放工作流的原创原生实现，不复用其专有源码或资源。交互保留所有支持的 3DGS、2DGS 与网格流程各自的导入及任务边界。

## English

Drag local files from Windows File Explorer into the GSW workspace. A hint identifies the proposed operation; dropping routes to the corresponding native import flow. Copy semantics are used: original files are not moved or deleted.

| Dropped content | Behavior |
| --- | --- |
| One `.gsw.json` or `.gswjson` project | Open the project rather than append it. Resolve unsaved changes before switching. Validate a malformed project before changing the current edited project. |
| One or more `.ply` or `.spz` models | Append sequentially, retaining existing objects, transforms and crops. Preserve source coordinates and the existing declared-unit conversion rules; do not auto-center or register models. |
| Photos, videos or media folders | Open media import settings to review sources and frame extraction. Import starts only after confirmation; recursive folder scanning also occurs after import confirmation. Reconstruction and training do not start automatically. |

If the current model has unexported crops, appending keeps that object active and preserves the current view while loading the new objects in the background. This keeps the crop warning visible to subsequent guards. Without unexported crops, new models become active normally. Switching objects or projects, or exiting, still offers export, discard or cancel. Crops must be exported; saving the project JSON alone does not persist those crop operations.

Photo extensions: `.jpg`, `.jpeg`, `.png`, `.bmp`, `.tif`, `.tiff`, `.webp`. Video extensions: `.mp4`, `.mov`, `.avi`, `.mkv`, `.webm`, `.m4v`. Decoding still depends on the configured FFmpeg and the actual codec; a supported extension does not guarantee every video can be decoded. Photos, videos and folders may be mixed within one media batch. Videos use the existing frame-extraction flow, not direct model loading into the viewport.

Drop a project on its own, and keep model and media batches separate. Reject web links, missing paths, unsupported file types and incompatible mixed batches with a reason. While a task or another import is busy, new drops are refused; dropping never interrupts, resumes or replaces that task. Canceling media settings starts no import.

This original native implementation takes workflow inspiration from Metashape without copying proprietary source or assets. Each supported 3DGS, 2DGS and mesh pipeline retains its existing import and task boundaries.

## 日本語

Windows のエクスプローラーからローカルファイルを GSW の作業領域へドラッグできます。操作内容を示すヒントを表示し、ドロップ後は対応するネイティブのインポート処理へ進みます。コピーとして扱い、元のファイルは移動・削除しません。

| ドロップする内容 | 動作 |
| --- | --- |
| 1 個の `.gsw.json` または `.gswjson` プロジェクト | 現在のシーンへの追加ではなく、プロジェクトを開きます。切り替える前に未保存の変更を確認します。不正なプロジェクトは事前に検証し、検証失敗で現在の編集を消去しません。 |
| 1 個以上の `.ply`、`.spz` モデル | 順番に追加し、既存のオブジェクト、変換、クロップを保持します。元の座標と既存の宣言済み単位の換算規則を保持し、自動的な中心合わせや位置合わせは行いません。 |
| 写真、動画、メディアフォルダー | メディアのインポート画面でソースとフレーム抽出設定を確認します。確定後にインポートし、フォルダーの再帰走査もインポート確定後に行います。再構築や学習は自動開始しません。 |

現在のモデルに未エクスポートのクロップがある場合、追加後も元のオブジェクトをアクティブに保ち、現在の視点を維持したまま新しいオブジェクトをバックグラウンドで読み込みます。切り替えによってクロップの確認が見落とされることを防ぎます。未エクスポートのクロップがない場合、新しいモデルが通常どおりアクティブになります。その後のオブジェクトやプロジェクトの切り替え、終了時には、引き続きエクスポート・破棄・キャンセルを確認します。クロップはエクスポートが必要で、プロジェクトの JSON を保存するだけではクロップ操作は保持されません。

写真の拡張子は `.jpg`、`.jpeg`、`.png`、`.bmp`、`.tif`、`.tiff`、`.webp`、動画は `.mp4`、`.mov`、`.avi`、`.mkv`、`.webm`、`.m4v` に対応します。デコードには設定済みの FFmpeg と実際のコーデックが関係するため、拡張子が対応していてもすべての動画を読み込めるとは限りません。同じメディアの一括処理に写真、動画、フォルダーを混在できます。動画は既存のフレーム抽出処理へ渡し、モデルとしてビューポートへ直接読み込みません。

プロジェクトは単独で、モデルとメディアは別々にドロップしてください。ウェブリンク、存在しないパス、非対応形式、種類の異なる不適切な混在は理由とともに拒否します。タスクや別のインポートの実行中は新たなドロップを受け付けず、そのタスクを中断・再開・置換しません。メディア設定をキャンセルしてもインポートは始まりません。

Metashape のドラッグ＆ドロップの流れを参考にした独自のネイティブ実装であり、専有ソースコードや素材は再利用しません。対応する 3DGS、2DGS、メッシュの各処理では既存のインポートとタスクの境界を保持します。

## Verification scope

Acceptance must exercise actual drop events, single-project safety, multi-model append, photo/video settings, busy rejection and immediate three-language refresh using independent fixtures. This document describes the intended public contract; it does not claim that those tests have passed. See the matching test results for the delivered build.
