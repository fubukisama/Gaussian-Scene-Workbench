# Source-ported navigation and streaming (2026-10-14)

## 本轮实际移植

| 优先级 | 来源与改进 | 收益 | 边界与风险 |
| --- | --- | --- | --- |
| 第一 | Blender 的光标锚定缩放、投影像素转空间位移 | 滚轮在相机目标平面保持光标锚点，平移随指针移动；视场角、窗口尺寸、滚转与正交跨度均纳入计算 | 不同步读取表面 GPU 深度，不是表面自动深度导航；轴吸附动画在操作前停止 |
| 第一 | LichtFeld 的整页字节预算与至少一页上传 | 点云不再按“一帧一页”消化已完成队列；点云与网格采用同一批次策略 | 每批默认 32 MiB、8 次尝试、4 ms；首个不可分割大页可超出批次额度，但仍须通过真正 RAM/显存准入 |
| 后续 | LichtFeld 的按字节限制撤销历史、最新兴趣页优先调度 | 适合大删除操作和快速转动后的缓存响应 | 本轮未实现，不能宣称已集成 |

“视图 → 缩放到光标”默认开启，可立即切换并记忆选择。关闭后滚轮仍围绕视图
中心缩放。Shift+左键拖动及导航缩放按钮仍保持原有中心缩放，不因鼠标位于
导航控件而突然移向屏幕边缘。平移的所有现有入口共用投影正确的计算。

批次额度不是模型大小限制，也不是整个帧的全局预算；多个对象/队列分别采用
同一策略，单次 GL 上传可能超过 4 ms。额度只在整页之间检查，不拆页、不抽面、
不改文件，不突破共享资源账本。重复、无效或被拒绝页也计入尝试次数；批次结束
统一重新计费、补读和刷新。点云读取并发仍为 2，因此不能从“最多 8 页”推断
加载快 8 倍。普通点云、高斯显示、2DGS、导入/生成网格和训练预览共用相机；
批量上传改进覆盖点云与网格分页，不替换训练器或 CUDA/OpenGL 共享预览协议。

## English

This tranche directly adapts Blender's cursor-anchored zoom and window-to-delta
math, and LichtFeld's whole-page byte quota with guaranteed first-page progress.
It does not import either application's full renderer, CUDA trainer or scene
engine. The default persistent **View → Zoom to Cursor** option affects the
wheel on the camera-target plane; disabling it restores center-wheel zoom.
Existing drag/button zoom stays center-based. All pan entries use actual
projection rather than a hard-coded distance multiplier.

Point and mesh queues share the same per-batch policy: 32 MiB, eight attempts,
four milliseconds between pages. The first indivisible page can exceed this
scheduling allowance but never actual shared RAM/VRAM admission. This is not a
model-size or global-frame cap. Invalid/duplicate/refused pages count as work;
accounting/refill is finalized after each batch. Two concurrent point reads
remain, so batching does not imply an eightfold loading or FPS improvement.
Camera changes apply to every model/display backend; optimizer and model data
are unchanged. Undo byte budgets and pending-read priority are future work.

## 日本語

本段階では Blender のカーソル位置を基準とするズームと投影に応じたパン、
LichtFeld のページ単位・バイト予算付き転送を実際に移植します。レンダラーや
CUDA 学習器全体の導入ではありません。「ビュー → カーソル位置へズーム」は
既定で有効、即時切替・保存でき、ホイールの基準は注視点を通る平面です。
モデル表面の GPU 深度取得を意味しません。無効時および既存のドラッグ／
ボタン操作は中心ズームを維持し、パンは実際の投影を使います。

点群・メッシュの各転送バッチに 32 MiB・最大 8 試行・ページ間 4 ms の同一
方針を適用します。分割できない最初の大ページはバッチ枠を超えても進めますが、
共有 RAM／GPU メモリの実際の准入を超えません。モデルサイズやフレーム全体の
上限ではなく、単一 GL 転送は 4 ms を超える場合があります。無効・重複・拒否
ページも試行数に含み、終了時に使用量・追加読込・表示を更新します。点群の
読込同時数は 2 のままで、8 倍の高速化や FPS 向上を保証しません。元モデル・
学習状態は変更しません。履歴のバイト予算や読込優先順位は今後の対象です。

## Fixed source and license

### Shared model-generation/display matrix

| Source/display | Cursor wheel / projected pan | Upload batch path | Generation/state |
| --- | --- | --- | --- |
| Ordinary resident or paged point cloud | Shared camera | Paged point queues | Original files unchanged |
| 3DGS / 2DGS Gaussian or surfel display | Shared camera | Existing resident/shared-GPU renderer preserved; point-cache fallback uses point queues | Training and full-state resume unchanged |
| Imported plain/textured mesh | Shared camera | Paged mesh queues; texture/resident upload policy unchanged | Source coordinates/material data unchanged |
| 2DGS bounded/unbounded TSDF / OpenMVS output | Shared camera | Same native mesh queues | Generation/runtime acceptance gates unchanged |
| 3DGS SuGaR / GS2Mesh output | Shared camera | Same native mesh queues | Optional runtimes/quality gates not newly certified |
| Live task preview | Shared camera when live drawing is enabled | Existing CPU/GPU preview protocol retained | Pausing/continuing optimization not affected; a deliberately frozen preview remains a frozen image |

- Blender v4.5.0 `8cb6b388974a817afedf1317ce26f0c75aa5f181`: actual
  `view_zoom_to_window_xy_3d`, `ED_view3d_win_to_delta` and pan adaptation.
  See `native/third_party/blender-navigation/README.gsw.md` for source hashes,
  original 2008/2023 notices, GPL-2.0-or-later and modification details.
- LichtFeld Studio `a8d2feafbe0abd91211e20f46b7793b8839b2922`: actual
  `LodPageCache::drainPendingUploads` policy adaptation. See
  `native/third_party/lichtfeld-streaming/README.gsw.md`, original 2026 notices
  and GPL-3.0-or-later. This self-contained CPU module links no NVIDIA library.

Both subsets and their provenance ship in `licenses/` and the corresponding
native source payload. Original native main code remains GPL-3.0-or-later;
separate research training licenses are not changed.

## Verification and measurement boundaries

- Public Qt value-math tests first failed for distance-only zoom and old pan,
  then passed the port. Forty-four QtTest cases cover independent numeric
  oracles, 36 projection/aspect/orientation combinations, limits, focal aspect,
  invalid matrices and immutable input values.
- Upload policy tests first failed for one-page draining, byte/time quotas and
  overflow, then passed: twelve QtTest cases, no skipped behavior.
- `GSW_FORCE_POINT_PAGING=1` is a default-off diagnostic analogous to existing
  mesh admission overrides. Only exact `1` forces a non-mesh source through the
  existing read-only point cache. Gaussian/surfel sources are excluded from this
  diagnostic override; their existing over-count behavior is retained. It is not a new production loading strategy;
  all other values and ordinary imports retain the default path. The public
  loader regression preserves exact source bytes and counts and checks mesh
  isolation.
- `--smoke-test-upstream-navigation` drives real wheel/pan/menu input and
  framebuffer checks in three locales, verifies original bytes/TRS/selection
  and stores frames. `--smoke-test-point-streaming` uses 110,592 colored points
  (~1.6 MiB source), 512 leaves, a fixed 640x400 real framebuffer, bounded resource admission
  and explicit cleanup. It reports first-visible/ready time and composed frames.
- Real-window tests run on remote Windows, not the user's foreground desktop.
  A compile or helper-only pass is not evidence that those window gates passed.
  No same-data/hardware speed comparison with LichtFeld or Blender is claimed.
  The point gate normalizes software-GL pixel work, not source/page count or
  production loading. Its original 30-second readiness wait and full requested
  cut, resource-lease release and source-byte assertions remain unchanged;
  failure logs include observed residency, leases, frame composites and an image.
