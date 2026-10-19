# Blender-derived native viewport / Blender 原生视口移植 / Blender 由来のネイティブビューポート

## Scope

GSW directly adapts Blender v4.5.0, commit
`8cb6b388974a817afedf1317ce26f0c75aa5f181`, under its original
GPL-2.0-or-later notices, selecting GPLv3 for the combined native desktop.
This is source reuse, not embedding Blender's entire application or engine.

- `native/third_party/blender-grid/`: upstream line filtering, eight decimal
  scales, cubic three-level blending, viewing-angle and distance fade. The
  retained upstream files, hashes and precise adapter/depth boundaries are in
  its `README.gsw.md`.
- `native/src/ViewportPresentation.h`: upstream theme-axis inks, without
  introducing a Blender 3D cursor.
- `native/src/BlenderNavigationPresentation.h`: axis-end discs, front/back
  ordering, faded negative ends and XYZ labels from Blender's navigation gizmo.
  The GSW click/drag/projection behavior and semantic command icons remain.

The background still uses GSW's existing live light/dark theme setting: white
or the existing night color, **not Blender gray**. No new custom-color picker
or persistent color override is introduced. Axis presentation is consistently
X red, Y green, Z blue; source data and axis coordinates are never swapped.

The grid remains on the configured shared source plane (source-base
or World Zero). Blender's red/white ring is a 3D cursor, not a world-origin or
model transform handle, and is not added to GSW's default viewport. Models are not
recentered, rescaled, grounded or re-registered. The grid is drawn before scene
geometry, with depth writes disabled and destination alpha preserved. This is
not Blender's separate post-scene soft-depth/in-front overlay engine.

Viewport captions are unframed with a small contrast shadow; navigation is at
the upper right. Task-progress information remains available. The central grid
readout denotes the emphasized decimal level; perspective can show adjacent
blended levels at other distances.

Point, mesh, 3DGS and 2DGS renderers, source colors/textures/SH, selection,
editing constraints, undo, processing preview isolation and optimizer-state
training/resume semantics are not replaced by Blender code.

## 中文

本轮实际移植固定版本 Blender 的网格过滤、多级平滑过渡、远景淡出和轴向导航
绘制，保留原版权与 GPL 声明。背景继续使用 GSW 已配置的实时
深浅主题，不换成 Blender 灰色，也不新增颜色选择器。统一 X 红、Y 绿、Z 蓝
只是显示配色，不交换源坐标，不移动模型、观察中心或变换轴心。

网格仍遵守同源共享坐标和“模型底部/世界零点”参考面。Blender 的红白圆环是
三维游标，不是世界原点或模型变换手柄，因此不放入默认视口。右上导航使用圆点轴端及
前后层次，文字信息去除常驻底框，任务进度保留。各类模型、编辑、训练预览和
完整优化器状态续训语义保持原有实现。此移植不等于嵌入完整 Blender 引擎。

## 日本語

固定した Blender のソースから、グリッドのフィルター、多段階の滑らかな
遷移、遠景のフェード、軸ナビゲーションの描画を移植し、
元の著作権と GPL 表示を保持します。背景は GSW の設定済みライト／ダーク
テーマを維持し、Blender の灰色には変更しません。新しい色選択機能は追加
しません。X 赤・Y 緑・Z 青は表示色のみで、座標やモデルを入れ替えません。

共有ソース座標と基準面を維持します。Blender の赤白リングは 3D カーソルで
あり、座標原点やモデル変換ハンドルではないため、既定の表示には追加しません。
右上のナビゲーションと枠なしの情報表示に変更し、タスク進捗は残します。
各モデルの描画・編集・学習プレビュー・完全状態再開は既存の実装を保ちます。
Blender 全体のエンジンを組み込んだものではありません。

## Verification

`--smoke-test-blender-viewport` routes before any interactive MainWindow is
constructed. It uses a real hidden Windows OpenGL framebuffer, rejects a
visible native window or unavailable context as failure, isolates settings and
uses bounded test-owned fixtures. It observes actual rendered axes, continuous
wheel zoom, live theme switching, no misleading default cursor, six signed
orthographic planes and shallow perspective horizon fade. Tiny test-owned
point, indexed-mesh, 3DGS and 2DGS fixtures must actually render in both themes
and projections without changing transforms or original fixture bytes. No real
project or training task is used. `GSW_BLENDER_VIEWPORT_EVIDENCE` may name a D-drive QA
directory for retained PNGs.

Existing native geometry/navigation, scene-coordinate and rendering checks
remain in the full CI suite. The older orientation smoke now tests the compact
world-axis local pixel footprint rather than requiring obsolete world-sized
tripod growth; independent physical-axis geometry tests remain unchanged.
