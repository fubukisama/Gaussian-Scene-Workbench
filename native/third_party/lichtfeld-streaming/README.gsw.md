# LichtFeld Studio upload-drain budget

Pinned upstream: LichtFeld Studio commit
`a8d2feafbe0abd91211e20f46b7793b8839b2922` (master inspected 2026-10-14).

This is an actual source adaptation of the whole-page byte-budget and
at-least-one-page logic in `LodPageCache::drainPendingUploads`, not a claim to
have integrated LichtFeld's renderer, CUDA backend, RAD format or training
engine. Original authorship and GPL-3.0-or-later notices are retained. The
complete GPL version 3 text is in repository `COPYING` and native packages.

Source (original upstream file SHA-256):

- [`src/visualizer/rendering/lod_page_cache.cpp`, lines 549-564](https://github.com/MrNeRF/LichtFeld-Studio/blob/a8d2feafbe0abd91211e20f46b7793b8839b2922/src/visualizer/rendering/lod_page_cache.cpp#L549-L564):
  `17f9f4979823a4e2b6f0f3913c4ed825b02777a4b31ed7a224feb676a868a3b6`.

Validation reference:

- [`tests/test_lod_page_cache.cpp`, `UploadDrainHonorsByteBudget`](https://github.com/MrNeRF/LichtFeld-Studio/blob/a8d2feafbe0abd91211e20f46b7793b8839b2922/tests/test_lod_page_cache.cpp#L388-L409):
  `218b07a0caf305130df6c26cbf419e3edb790de3e6619ba2867bac8c0544210d`.
  GSW's tests are independently written at its public batch-policy interface.

Modifications, 2026-10-14:

- Extract the upstream whole-page admission behavior as an allocation-free,
  standard C++ module for GSW's point-cloud and mesh upload adapters.
- Adapt fixed upstream page bytes to variable-size GSW pages.
- Add an attempt-count and caller-measured elapsed-time limit; every popped
  page, including failed/invalid work, consumes an attempt. Default policy is
  32 MiB, 8 attempts and 4 ms per upload batch.
- Retain `maxBytes == 0` as unlimited. An indivisible first page can exceed the
  byte/time allowance so it cannot be starved forever; limits bound additional
  work, not the execution time of a single OpenGL allocation/transfer.
- Use saturation/subtraction to prevent integer wraparound. A zero page limit
  permits no work; a zero elapsed-time limit is unlimited.

The caller must check `permits(nextBytes, elapsedNs)` before dequeuing, then
call `recordPage(pageBytes)` exactly once for every dequeued attempt. The module
does not allocate GPU storage, consume adaptive RAM/VRAM reservations, cancel
workers, publish residency or mutate model data. Those remain GSW adapter
responsibilities. This is a per-batch limit: point and mesh calls or multiple
scenes do not automatically share a global frame quota.

No NVIDIA library is linked by this adaptation. LichtFeld's optional NVIDIA
GPU Library Exception is not needed for these standard C++ source portions.

中文：实际移植 LichtFeld 的整页字节预算与“至少推进一页”逻辑，扩展为点云和
网格共用的批次策略；不移植 CUDA 引擎，不改变模型、训练或自适应显存预算。

日本語：LichtFeld のページ単位のバイト予算と最低 1 ページを進める処理を
実際に移植し、点群・メッシュ共通のバッチ方針へ適合させています。CUDA
エンジンの移植ではなく、モデル・学習・適応的 VRAM 予算を変更しません。
