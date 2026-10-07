# Native textured-mesh text repair — 2026-10-07

## Cause and scope

Mesh texture upload changed `GL_UNPACK_ALIGNMENT` to 1 and left it in the
OpenGL context shared with Qt painting. Qt 6.8.3's 8-bit glyph uploader relies
on four-byte-padded image rows. Newly introduced glyphs therefore became
diagonal fragments after texture upload. The native uploader now saves and
restores the incoming alignment immediately around `glTexImage2D`, including
the later upload-error path. No shader, mesh, model data, or training state was
changed. [Qt 6.8.3 source](https://github.com/qt/qtbase/blob/v6.8.3/src/opengl/qopengltextureglyphcache.cpp#L195-L202).

## Public regression and evidence

`scripts/test_native_texture_text.py` launches separate native application
processes for identical textured/untextured fixtures. It introduces fresh
Latin, Chinese and Japanese project-data glyphs only after the mesh/texture
has actually rendered, then compares public viewport framebuffers. Static
title and tool-state glyphs must retain at least 98% matching pixels, with
mean channel difference at most 7. The tiny colored quad must also have at
least 1,000 DPR-scaled model pixels changed by actual texture sampling.
Statistics, FPS and model geometry are excluded from the glyph masks.

- Final regression against the unfixed renderer: title 2,604 pixels,
  matching 0.657066, mean difference 65.8251; tool-state 601 pixels,
  matching 0.687188, mean difference 38.8597. Model changed 362,641 pixels.
- With alignment restoration, both static glyph regions match 1.0. The
  original Metashape fixture also renders readable title, statistics and
  tool labels; its 8192 x 8192 JPEG remains visible.
- The test label is short so translated statistics cannot change its
  elision. Badge detection selects a dense filled region, excluding thin
  world-axis lines and background pixels. These test corrections did not
  lower the acceptance thresholds, and the original red capture still fails.

## Verified build and installation

- Native CTest: 60/60 passed; translation catalog and immediate-language
  checks cover `zh_CN`, `en_US`, `ja_JP`. Final glyph-predicate refinement
  was followed by another 6/6 texture-text CTest run.
- Worker regression suite: 154 tests, OK with 8 environment-dependent skips.
  73 staged Python files compiled; packaged preview preflight reported ready.
- Installed tiny matrix: 12 passed — three languages/two themes, PPM/PNG/JPEG,
  resident and forced-paged mesh paths.
- Installed real-texture matrix: 16 passed — Metashape in all three languages
  and both themes, plus forced paging; OpenMVS and SuGaR in both themes and
  both residency paths. Title/tool glyph matching is 1.0 in every case.
- Installed surface matrix: 6 passed — original Metashape, original 2DGS mesh,
  and OpenMVS, both themes. Visible surface preservation is at least 0.999993.
- Source/build/package/installed executable SHA-256:
  `42911D71A16FCD0E661C3F9C2161071602B4BCA3D5758F245909014F8FC55CD9`.
- Side-by-side installation: `D:/Apps/GSW/native/0.3.1-texture-text-oct7`.
  The Native shortcut targets its executable and working directory. Previous
  installations and the user's already-open process were preserved.
- Original Metashape mesh SHA-256:
  `29AD18D29AC408A7AF474F292B99BE17D90C21206E450758013E0C269DF15F95`.
  Original JPEG SHA-256:
  `0DA1C90BE98FC445261F283413812FB524D9CA559ADCE80DBFEEDBD7A12075B0`.
  Both remained unchanged. Real regression fixtures are independent copies;
  binary mesh payloads and atlas bytes are not re-encoded.

## Shared-pipeline boundary

| Pipeline / input | This repair's verification | Remaining boundary |
| --- | --- | --- |
| 3DGS | Native Gaussian, processing-preview and worker regressions pass; common texture uploader restored | No new long-running training run or optimizer-resume claim |
| 2DGS | Original untextured mesh surface passes in both themes; native/worker workflow regressions pass | No change to training or resume capabilities |
| TSDF | Existing mesh-generation/worker regressions pass; no backend-specific renderer branch added | No new TSDF extraction run in this text-only repair |
| OpenMVS | Real 2048 x 2048 atlas, both themes and residency modes; text and surface pass | Existing material-generation capability unchanged |
| SuGaR | Real 856 x 856 atlas, both themes and residency modes; text passes | No new research-training run |
| GS2Mesh | Worker regressions pass; available mesh header has XYZ, normals and RGB, but no UV/TextureFile, so it does not exercise atlas upload | Previously recorded disconnected-island surface gate and over-budget continuous LOD remain unresolved; see [mesh generation](../MESH_GENERATION.md) |

The fix is common to imported/generated textured meshes, independent of the
generation backend. A materialless output is not presented as a textured
capability, and previews/warm starts are not full optimizer-state resume.

## 中文摘要

已修复纹理上传遗留像素行对齐状态、导致新字形斜条纹破碎的问题。保存并恢复
原状态即可，无需修改模型、字体、训练代码或关闭安全策略。三语深浅主题、
不同贴图格式、分页及真实 Metashape/OpenMVS/SuGaR 模型均通过。原始文件保持
不变；旧窗口保留，保存当前工程后从更新后的 Native 快捷方式重新打开即可。
GS2Mesh 已记录的碎片表面验收及超显存连续 LOD 限制不属于本次修复，未声称解决。

## 日本語の要約

テクスチャ転送後にピクセル行のアライメントが残り、新しい文字が斜めの断片に
なる問題を修正しました。転送前の状態を保存・復元し、モデル、フォント、学習
コードやセキュリティ設定は変更していません。3言語・明暗2テーマ、複数画像形式、
ページング、実際の Metashape/OpenMVS/SuGaR モデルで検証済みです。元ファイルと
既存ウィンドウは保持しました。現在のプロジェクトを保存してから、更新した
Native ショートカットで開き直してください。GS2Mesh の既存の断片的な表面の
検証課題と、GPU メモリ予算を超える連続 LOD の制限は解決済みとは扱っていません。
