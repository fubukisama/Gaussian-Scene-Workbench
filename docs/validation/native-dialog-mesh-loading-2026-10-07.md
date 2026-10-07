# Native dialog, toolbar and mesh-loading repair — 2026-10-07

## Public boundaries and causes

The user confirmed real import-dialog/toolbar behavior and real viewport
first-visible/full-display timings as acceptance boundaries. Smoke processes
isolate settings and never alter the user's running project or original model.

Qt's localized file-dialog size hints produced widths of 354/506/363 logical
pixels for Chinese/English/Japanese at DPR 1.5. A once-per-dialog, post-show
width policy now supplies at least 640 logical pixels, screen-clamped, while
retaining height and later manual resizing. The radius label painted the
generic white widget surface over the gray toolbar (RGB255 vs231). A scoped
transparent toolbar-label rule fixes actual composed pixels in both themes.

The resident mesh loader had a fixed two-million-face threshold and a separate
face/triangle sampling cap. The reported 2DGS geometry needs approximately
95 MiB in exact cache pages, but default loading paged it and took29.235s to
fully display. Forced resident loading was quick but silently retained only
1,999,349 valid triangles. The new bounded resident path preserves all
2,594,994 valid triangles in the model (2,595,834 declared faces).

Paging had two further costs: only two concurrent page reads, and refining
already-complete parent pages. Source-valid triangle counts distinguish a
complete reservoir from a random subset. Complete parents are now reused;
incomplete parents still refine to exact pages when budget permits. Higher
read concurrency is bounded by16 and by128MiB maximum pending/inflight payload
(or a quarter of the per-scene page budget; one indivisible page can progress).
The eight-page/four-ms GPU upload scheduling policy is retained.

## Executed differential evidence

Times are actual setScene-to-presented-frame upper bounds with40ms sampling,
GPU readback included. Generated-grid axis setup adds300ms. No1500ms artificial
floor remains. Source parsing, first-visible and complete times are reported
separately. Cache-resident triangle counts are not rasterized overdraw counts.

| Scenario | Original complete | Reads2→16 only | Exact-parent reuse | Full resident policy |
| --- | ---: | ---: | ---: | ---: |
| Connected16×16, forced paging | 9071ms | 2643ms | 506ms | 547ms paging retained |
| OpenMVS12888 triangles, forced paging | 32663ms | 8508ms | 172ms | Paging regression retained |
| Original2DGS, default | 29235ms | 7572ms | 6392ms | 1073ms, all2594994validtriangles |

Normal default OpenMVS was already resident (~179ms); the forced-paging number
is a fallback regression, not an ordinary user-import speedup. Repeating its
same original cache reduced the old settling time by only~2%, so cache creation
was not the source of the long after-parse delay. The minimized no-atlas289V/
512T grid still failed the old5000ms gate, excluding texture size as necessary.

A first concurrency edit accidentally targeted the similarly named POINT
limit; diff review corrected it before the mesh-only probes. Those mislabeled
repeat timings are not causal evidence and are excluded from the table.

Known-valid connected1024×1024 surface: all2097152triangles now retained and
default complete1171ms, instead of resident's old2000000cap. OriginalMetashape
default light/dark complete1151/1275ms with1049902triangles and100% reference
surface coverage. Its original8192²JPEG title/tool glyph comparison remains
1.0 matching in both themes after the prior GL unpack-alignment repair.

## Resource and capability boundaries

Count safety ceilings remain5million vertices/faces. Per-load byte estimation
is capped at1GiB and includes point-picking buffers, UV seam worst case,
indices and each actual RGBA mip level. Polygon expansion is checked before
append; overflow restarts from the saved PLY payload in fresh data, preserving
decoded texture fields. Original PLY/atlas bytes are not changed.

This is a shared import/rendering repair for2DGS TSDF/OpenMVS and3DGS
SuGaR/GS2Mesh output formats, not a training or optimizer-state change. It is
not a new surface-preserving LOD algorithm. Dense GS2Mesh quality acceptance
still requires a suitable provenance-verified fixture; the existing sparse
reduced fixture is not sufficient to prove continuous-surface quality.

The per-load allowance is not a hard total-VRAM promise. Existing resident
objects are not reclassified when more are added; initial project/batch loads
may capture a budget before final GPU/scene-count information. Full paged
textures remain separate from the page allowance. Models exceeding the
available exact-surface budget can still fall back to sampled coarse pages.

## Final delivery gates

- Independent64MiB budget fallback: connectedgrid768 default complete2746ms,
  all1179648valid triangles,650065/650065 interiorpixels retained against an
  independently captured normal-budget resident reference.
- Fresh300000-vertex singleUV polygon (299998fantriangles) checks mid-parse
  budget fallback/rewind. Highbudget resident creates no cache;64MiB first load
  builds a new cache and complete919ms, preserving299842validtriangles after
  identical156degenerate removals. Independentresident→paged frame retains
  all289837interiorpixels. SourceSHA256 forbothfreshcopies:
  `2f0de1420801ba229d1cd92b10d49df0773d282ba9a90efa7d3a853b8d8c19ef`.

- Public UI matrix18/18 (three locales, both themes,90/100/150% scale): initial
  width640/640/960logical; Qt height retained; screen-contained; manual narrower
  resize preserved across live language/theme changes; actual label/bar RGB
  difference0. Spec assertions use independent known widths.
- Full native CTest65/65PASS in415.75s, including translationcatalog/validator,
  language/livewindow matrix, sharedgeneration, mesh surface/performance,
  previous texture/text regressions and multi-scene/import/export tests.
- Native worker154testsOK (8environment-dependent skips);73packaged Python
  files compiled without bytecode; staged preview server ready. UpstreamSIBR
  SyntaxWarnings remain third-party warnings, not package failures.
- Isolated installed UI matrix18/18PASS; texture/text matrix6/6PASS (all three
  locales and both themes), glyph matching1.0. The default mesh matrix14/14PASS
  covers original2DGS, Metashape, bounded/unbounded TSDF, OpenMVS, SuGaR and
  texturedSuGaR in both themes. All retain100% reference surface coverage and
  the same valid triangle count; first-visible to complete is0–1ms. Two tiny
  forced-paging regressions also pass at512/510ms with512 exact GPU triangles.
- Installed GS2Mesh reduced-fixture import readiness2/2PASS (light/dark):
  source25454vertices/45093faces, mesh available, paging settled and a captured
  framebuffer with the expected no-texture state. This separate public capture
  harness does not assert continuous-surface quality; that gate remains open.
- Installed original2DGS complete1163/1174ms (light/dark); Metashape1327/1312ms.
  These are separate installed runs, not the earlier causal probe timings.
- Build/package/installed executable SHA256 is identical:
  `AD5A8B894B91A667926AD131D9F94A56202797D76D9F067D3D84A76FF0224256`.
  New installation: `D:/Apps/GSW/native/0.3.1-mesh-fastload-oct7`.
  The existing Native desktop shortcut now resolves there; its previous target
  is backed up onD. A smoke launch through the resolved target passes with only
  installed/Windows runtime paths. The user's existing process22544 remains
  running, preserving the open project. Previous installations are retained.

中文：已修复文件窗口默认过窄、工具栏标签白底和普通网格不必要的缓慢分页。
真实2DGS默认完整显示由约29.2秒降到约1.1秒，所有有效三角形均保留。
超大模型仍使用受控分页；不改原始模型或训练状态，不声称实现新的连续曲面
LOD或所有对象的总显存硬上限。

日本語：ファイルダイアログの初期幅、ツールバーラベルの白い背景、通常の
メッシュで不要だった遅いページングを修正しました。実モデル2DGSの完全
表示は約29.2秒から約1.1秒へ短縮し、全有効三角形を保持します。大規模モデル
は制限付きページングを維持します。元データや学習状態は変更せず、新しい
表面維持LODや総VRAM上限を実装したとは主張しません。
