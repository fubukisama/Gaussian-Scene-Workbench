# Gaussian interaction performance

## Change

Static imported Gaussians use GPU-resident attributes when their count fits the
OpenGL texture-buffer limit. All positions, colors, opacity, scales and rotations
remain float32. Camera rotation updates a uint32 instance-order buffer rather
than rebuilding and uploading the 60-byte vertex record for every Gaussian.
There is no new point-count cap, quantization, delayed sort or navigation LOD.

`GaussianDepthSorter` computes each depth once and uses a stable four-pass radix
sort of float32 depth keys. It preserves the previous back-to-front ordering and
source-index tie-breaker, including signed-zero equality. Non-finite depths have
deterministic ordering. Scratch arrays are reused per scene. Selection and
deletion rebuild attributes; camera/model orientation changes only update order.
Pan/zoom/hover reuse the order when direction has not changed. Camera-release and
snap completion no longer force a duplicate whole-scene rebuild.

The compatibility instanced-attribute renderer retains the same exact sorter for
devices/data exceeding texture-buffer limits. `GSW_DISABLE_INDEXED_GAUSSIANS=1`
forces this path for developer comparison. Training CUDA/shared-GPU-memory VAOs
continue to use their existing attribute layout, not the static-scene indirection.

## Reproduce and guard

Run the packaged or build-tree executable with:

```powershell
$env:GSW_GAUSSIAN_ORBIT_BUDGET_MS = '35'
$env:GSW_EXPECT_INDEXED_GAUSSIANS = '1'
& '.\Gaussian Scene Workbench.exe' --smoke-test-gaussian-performance --smoke-scene 'E:\model.ply'
```

Without `--smoke-scene`, the smoke test creates a deterministic 512,202-Gaussian
fixture in a temporary directory beside the executable and removes it afterward.
Keep the executable/build on a non-system drive. Smoke settings are isolated.
`GSW_GAUSSIAN_BENCHMARK_DIR` optionally saves QA frames to a chosen directory.

The benchmark replays 40 frames each of idle, orbit, alternating wheel zoom and
hover. Timing includes input dispatch, completed rendering and framebuffer
readback, not just CPU submission. It is not a claim about every user's sustained
FPS. The optional budget fails when orbit median exceeds idle median + the given
milliseconds; ordinary CTest uses structural checks without hardware-dependent
timing thresholds.

Regression checks cover full rendered count, no attribute uploads during
navigation, fresh order on rotation, reused order for unchanged direction,
resident/compatibility image agreement (maximum one 8-bit color level rounding),
point/Gaussian mode switching, selection, deletion, undo and scene cleanup.
The mixed-scene test also exercises picking and double-click recentering on
inactive Gaussian layers. Its point-picking VAO is refreshed on load/edits, not on
camera motion. Model rotation/nonuniform scaling must refresh order without
reuploading attributes.
`gaussian_depth_sorter` additionally compares every field of 512,202 sorted
records against the former comparison sort across four directions, tied depths,
reordered inputs, signed zero, infinities, NaN and empty/resized input.

## Measured on the development workstation

User model: 512,202 Gaussians; framebuffer: 1674 × 1032 physical pixels;
RelWithDebInfo; same camera/input replay. Original input file was not modified.

| Renderer | Idle median | Orbit median | Orbit p95 |
| --- | ---: | ---: | ---: |
| Original comparison sort + whole-record uploads | 5.08 ms | 98.76 ms | 101.39 ms |
| Exact radix sort + whole-record uploads | 5.12 ms | 58.91 ms | 63.72 ms |
| Exact radix order + resident attributes | 5.27 ms | 9.69 ms | 10.89 ms |

Per-rotation upload for this model changes from 30,732,120 bytes of attributes to
2,048,808 bytes of indices. The new initial attribute buffer is 32,780,928 bytes
(four RGBA32F texels per Gaussian). CPU filtering, gather and driver vertex-data
updates no longer run for each camera rotation. Timed CPU upload calls alone did
not account for the entire old frame stall, so the measurements above report the
complete rendering path rather than attributing all improvement to bus bandwidth.
The existing 60-byte point VAO is retained alongside the new buffer for picking;
total static vertex/index storage in this example is 65,561,856 bytes (about
35 MB more than the old path), excluding framebuffers and other scene resources.

Very large on-screen splats can still be GPU fill-rate limited. This change does
not alter the rasterizer, SH representation, splat size or source data.

## Read-only static Gaussian object selection

中文：静态文件中的 3DGS／2DGS 对象点选使用当前可见点中心的只读几何检测，不在主视口创建拾取帧缓冲、绘制拾取图像或切换 OpenGL 上下文。保留既有六逻辑像素方形邻域、圆形点覆盖、删除掩码、裁剪、模型变换和多对象最近深度规则；不改变高斯参数、源文件或显示点数。驱动的点光栅边界取整可能与几何计算略有差异，不宣称逐像素位级等价。表面双击定位、网格／分页数据以及没有 CPU 几何的 IPC 实时预览仍使用原来的 GPU 路径；此修正不代表这些路径的上下文隔离已验收。录屏中的前台消失现象仍需实际界面复测，后台正常不能替代该验收。

English: Static file-backed 3DGS/2DGS object clicks use read-only geometry tests against currently displayed point centers, without creating a pick framebuffer, drawing a pick mask, or switching the main viewport's OpenGL context. The existing six-logical-pixel square neighborhood, circular point footprint, deletion mask, clipping, model transforms and nearest-depth object ordering remain. Gaussian parameters, source files and displayed counts are unchanged. Driver point-raster rounding may differ at pixel boundaries; bit-exact raster equivalence is not claimed. Surface double-click recentering, mesh/paged data and IPC live previews without CPU geometry retain the GPU path; their context isolation is not accepted by this fix. The recorded foreground disappearance still requires interactive acceptance; successful background checks do not replace it.

日本語：静的ファイルの 3DGS／2DGS オブジェクトのクリック選択は、表示中の点中心に対する読み取り専用の幾何判定を使用し、主ビューポートに選択用フレームバッファーを作成せず、選択画像の描画や OpenGL コンテキストの切り替えを行いません。既存の論理 6 ピクセルの正方形領域、円形点の被覆、削除マスク、クリッピング、モデル変換、複数オブジェクトの最前面深度を維持します。ガウスパラメーター、元ファイル、表示点数は変更しません。画素境界ではドライバーの丸めとの差があり、ビット単位の同一性は保証しません。表面のダブルクリックによる再中心化、メッシュ／ページングデータ、CPU 幾何を持たない IPC ライブプレビューは GPU 経路を維持し、この修正でそれらのコンテキスト分離を検証済みとはしません。録画の前面表示で発生した消失は実際の画面での再確認が必要で、バックグラウンド検査だけでは代替できません。
