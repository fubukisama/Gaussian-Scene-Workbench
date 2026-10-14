# GSW combined L1 / SSIM CUDA loss

This release implementation is derived **anew from the MIT-licensed
`rahul-goel/fused-ssim`**, pinned to commit
`1272e21a282342e89537159e4bad508b19b34157`. It is not a relicensing of the
earlier, private LichtFeld GPL prototype.

Original `ssim.cu` UTF-8/LF SHA-256:
`f08a77b47d0903778d84bedffdea401c647d35fd3643dad9ca89d2ffd28988c1`.
The existing vendored file is identical after CRLF normalization (raw Windows
SHA-256 `90313899e715961bd448181be3a82009b808debd11409702e8523a3fbd04658b`).
Source: <https://github.com/rahul-goel/fused-ssim/blob/1272e21a282342e89537159e4bad508b19b34157/ssim.cu>.
The original copyright is Rahul Goel, 2024; the complete MIT license is in
`LICENSE`, with the GSW addition notice. Original research trainers/rasterizers
retain their own licenses; this permissive loss module does not relicense them.

## Actual source adaptation

- Retain the original Gaussian-window convolution and SSIM derivative kernels.
- Use 16×16 tiles and enough convolution scratch columns for the original
  five-pixel horizontal offset; avoid out-of-bounds scratch indexing.
- Add original GSW float32/uint8 target loading, normalize uint8 by 255 without
  materializing a complete float32 target image.
- Reduce L1 and channel-mean SSIM together into the loss and an L1 logging scalar.
- Fuse the L1 sign gradient and scalar upstream chain into the SSIM backward
  kernel; do not allocate an additional full-resolution L1-gradient/map tensor.
- Bind to caller-owned, grow-only Torch workspaces and the current CUDA stream.
- Preserve float32 intermediates, the original same/zero-padding objective,
  `sign(0)=0`, arbitrary scalar upstream gradients and Torch differentiation
  through any contiguous conversion. No automatic valid-padding or reduced-
  precision objective change is made.

The extension exports source commit, MIT license and ABI 2 metadata. Runtime
admission requires those exact fields **and** actual deterministic CUDA value/
gradient comparisons. An import alone is not availability proof; unsupported
inputs or failed verification use the caller's original loss with an explicit
status reason. This is photometric loss only: surfel depth, distortion and
normal terms and exposure remain in the backend's original training graph.

Private GPL exploration artifacts are outside the repository under
`D:/Apps/GSW/validation/fused-loss-experimental-oct14/gpl-private-source` and its
0.1.0 wheels. They must not be included in a release or relabeled MIT. The
publishable extension is 0.2.2/ABI 2. Exact normalized uint8 equality is part of
the startup gate: an explicit rounded multiply prevents compiler FMA contraction
from turning equal pixels into nonzero L1 or an incorrect sign gradient.

中文：发布版重新以 MIT 的 fused-ssim 原始代码为基础，新增 L1 联合归约与梯度，
不是把 GPL 实验代码更改许可证。保留原来的零填充、FP32 和 2DGS 附加损失语义。

日本語：公開版は MIT の fused-ssim 原コードから新たに派生し、L1 の統合リダクション
と勾配を追加したものです。GPL 実験コードのライセンス変更ではありません。
従来のゼロパディング・FP32・2DGS の追加損失の意味を保持します。
