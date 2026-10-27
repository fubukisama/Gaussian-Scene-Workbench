# Viewport display / 视口显示 / ビューポート表示

## 简体中文

### 使用

- **视图 → 网格设置**：选择自动或固定间距，分别控制网格和坐标轴的可见性。
  自动模式随缩放平滑分级，密度可调为 0.25–4 倍；固定模式使用指定主格间距，
  细分数可选 1、2、5、10，细格间距为主格间距除以细分数。1 表示只显示主格。
  固定网格过密时淡出，不偷偷换成更粗的间距；超出 GPU 表示范围时给出说明。
- **视图 → 视口详情**，或视口底部的信息按钮：查看当前网格间距、视点到观察中心的
  距离、基准面、尺度来源及最近的帧率统计。详情可在导航时保持打开，并复制当前内容。
- 已知物理尺度时，可在场景单位、mm、cm、m 之间切换显示。未知尺度仍显示原始单位
  `u`，不允许据此声明米制。暂时缺少尺度元数据时，保存的显示单位偏好保留，但有效显示
  仍为 `u`。来源声明的尺度不是独立精度检核。
- 确定后设置随工程保存，并进入既有恢复记录；取消不改视口或工程。旧工程未保存该字段时
  使用默认显示，不沿用上一个工程的设置。界面语言可即时切换为简体中文、英语或日语。

### 语义和边界

主格间距保存在未经编辑的共享场景参考坐标单位中，而非当前选中模型的局部单位。
切换模型不更换该参考框架。网格继续遵守既有固定源范围基准面／世界 Z=0 设置，
正交轴向视图可使用相应参考平面；背景保持 GSW 的配置和主题。

这些选项只影响显示：不移动、旋转、缩放或重定心模型，不改相机、源单位、物理定尺度、
训练配置、完整检查点或编辑吸附步长，也不运行 SfM。网格间距和显示小数位都不是重建精度。
透视网格不是整张屏幕通用的二维比例尺。

默认状态只显示渲染模式与性能状态。FPS 只统计新绘制且完成视口呈现的帧；普通窗口合成
复用旧画面时不新增样本。停止重绘后显示“空闲”，处理活动尚在等待新画面时可显示
“等待新画面…”，暂停预览有独立状态。详情中的最近测量可保留用于查看，不能当作当前持续
帧率。平均帧间隔不是 GPU 内核耗时，也不是训练速度；不会仅为更新数字持续重绘 GPU。

同一显示能力适用于点云、网格、3DGS 和 2DGS。文档和静态检查不替代真实 Windows/OpenGL
窗口验收；显示正确不证明训练质量或实体尺寸精度。

## English

### Use

- Open **View → Grid Settings**. Automatic spacing adapts smoothly to zoom; its
  density multiplier is 0.25–4. Fixed spacing uses the selected major interval
  and 1, 2, 5 or 10 subdivisions. The minor interval is major/subdivisions;
  1 draws major lines only. Over-dense fixed lines fade rather than silently
  changing their spacing. Unrepresentable GPU spacing is explained.
- Open **View → Viewport Details**, or the information button at the viewport
  footer, for spacing, camera-to-target distance, reference plane, scale
  provenance and recent frame statistics. Leave the read-only dialog open while
  navigating, or copy its current contents.
- With a declared physical scale, choose scene units, mm, cm or m for display.
  Unknown scale remains `u`; the control cannot declare metres. If scale
  metadata is temporarily unavailable, a saved unit preference is retained but
  the effective display remains `u`. A source scale declaration is not an
  independent accuracy check.
- Confirmed settings are project data and enter the existing recovery records.
  Cancel changes neither the viewport nor the project. Older projects without
  this field use defaults rather than inheriting the previous project's view.
  Chinese, English and Japanese presentation updates immediately.

### Meaning and limits

Major spacing is stored in the unedited shared scene-reference coordinate
units, not the selected object's local units. Selecting another model does not
replace that reference frame. The existing fixed source-bounds or World Z=0
reference remains in effect, with the corresponding plane for orthographic
axis views. GSW's configured background and theme are preserved.

These are display controls. They do not transform/recenter models, change
cameras, source units, physical scale calibration, training configuration, full
checkpoints or edit-snapping increments, and do not run SfM. Grid spacing and
displayed decimal places are not reconstruction accuracy. A perspective grid
is not a universal screen-space ruler.

The default caption contains render mode and performance state. FPS samples
only newly rendered viewport images that complete presentation; composing a
cached image does not add a sample. It becomes **Idle** when redraws stop, may
show **Waiting for a new frame…** during an active wait, and has a separate
preview-paused state. Recent measurements remain inspectable in Details, not
as a claim of ongoing frame cadence. Mean frame interval is neither GPU kernel
time nor training speed. The GPU is not redrawn continuously just to animate
the counter.

The controls are shared by point-cloud, mesh, 3DGS and 2DGS rendering. This
document and static inspection do not replace real Windows/OpenGL acceptance;
correct display does not establish training quality or physical accuracy.

## 日本語

### 操作

- **ビュー → グリッド設定**で自動／固定間隔、グリッドと座標軸の表示を設定します。
  自動間隔はズームに滑らかに追従し、密度は 0.25～4 倍です。固定間隔の分割数は
  1・2・5・10 で、細グリッド間隔は主間隔／分割数です。1 は主線だけを表示します。
  密すぎる固定線はフェードし、勝手に粗い間隔へ変更しません。GPU で表現できない
  間隔には説明を表示します。
- **ビュー → ビューポートの詳細**、または下部の情報ボタンで、間隔、視点から注視点
  までの距離、基準面、尺度の由来、直近のフレーム統計を確認できます。読取り専用の
  詳細を開いたまま移動でき、現在の内容をコピーできます。
- 物理尺度が宣言されている場合のみ、シーン単位・mm・cm・m を表示用に選べます。
  不明な尺度は元の単位 `u` のままで、メートルとは推定しません。尺度情報が一時的に
  不明でも保存済みの単位の選択は保持し、有効な表示は `u` にします。出典の尺度宣言と
  独立した精度検証は別です。
- 確定した設定はプロジェクトと既存の復元記録に保存されます。キャンセルは表示も
  プロジェクトも変更しません。この項目のない旧プロジェクトは既定値を使い、前の
  プロジェクトの設定を引き継ぎません。簡体字中国語・英語・日本語は即時切替です。

### 意味と制限

主間隔は未編集の共有シーン基準座標の単位で保存し、選択モデルのローカル単位では
保存しません。選択を変えても基準座標系は変わりません。既存の固定ソース範囲／世界
Z=0 の基準面、軸方向の平行投影に対応する平面、GSW の背景設定とテーマを保ちます。

表示設定だけを変更し、モデルの移動・回転・拡縮・再中心化、カメラ、元の単位、物理尺度、
学習設定、完全チェックポイント、編集スナップ間隔は変更せず、SfM も実行しません。
グリッド間隔や小数桁数は再構成精度ではありません。透視グリッドは画面全体に共通する
二次元の縮尺ではありません。

通常の表示は描画モードと性能状態だけです。FPS は新たに描画して表示が完了した
ビューポート画像だけを数え、キャッシュ画像のウィンドウ合成は数えません。再描画が
止まると「待機中」とし、処理中の待機やプレビューの一時停止も区別します。詳細に残る
直近の計測値は、現在も描画が続いているという意味ではありません。平均フレーム間隔は
GPU カーネル時間でも学習速度でもなく、数字を動かすだけの連続 GPU 再描画はしません。

点群・メッシュ・3DGS・2DGS に共通する表示機能です。この文書や静的確認は実際の
Windows/OpenGL 検証を代替せず、表示の正しさは学習品質や実物の寸法精度を証明しません。

## Source reuse and reference boundary / 源码与参考边界 / ソース再利用と参照範囲

Actual source adaptation: **Blender 4.5.0**, pinned commit
`8cb6b388974a817afedf1317ce26f0c75aa5f181`. The existing grid line filter,
multi-level blending and fades, plus the unitless subdivision loop in
[`view3d_grid_steps_ex`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_draw.cc),
are adapted with the original GPL-2.0-or-later notices. The combined native
application is GPL-3.0-or-later. Exact retained sources, hashes, modifications
and rendering contracts are in
[`native/third_party/blender-grid/README.gsw.md`](../native/third_party/blender-grid/README.gsw.md).
This is not an embedded Blender engine or a copied Blender unit system.

[CloudCompare](https://github.com/CloudCompare/CloudCompare),
[LichtFeld Studio](https://github.com/MrNeRF/LichtFeld-Studio) and the
[Metashape public manual](https://www.agisoft.com/pdf/metashape-pro_2_3_en.pdf)
were workflow/design references for this change; their code is **not** part of
this grid-settings or FPS implementation. Metashape's proprietary code is not
copied. No new rendering or training dependency is introduced.

中文：本次实际移植的是上述固定版本 Blender 网格源码，并保留许可证和来源；
CloudCompare、LichtFeld Studio、Metashape 仅作为本次功能的公开行为／设计参考。
不是复制完整 Blender 引擎、单位系统或 Metashape 源码，也不引入新的渲染或训练依赖。

日本語：実際に移植するのは上記固定版 Blender のグリッドコードで、元のライセンスと
由来を保持します。この機能では CloudCompare・LichtFeld Studio・Metashape は公開動作／
設計の参考に限定し、Blender エンジン全体・単位システムや Metashape のソースはコピー
しません。新たな描画・学習依存関係も追加しません。
