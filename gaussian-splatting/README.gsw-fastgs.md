# Optional Faster-GS renderer

The optional extension is the official Apache-2.0
[`nerficg-project/faster-gaussian-splatting`](https://github.com/nerficg-project/faster-gaussian-splatting)
CUDA backend, pinned to `c60ea269b7cf3ebda527d640b6fdd31462f91b41`.
Its original source, root license and component notices remain in the pinned
research checkout used by `scripts/gsw_fastgs_experimental_install.ps1`.

`gsw_fastgs_adapter.py` is an original MIT GSW adapter to the documented raw
Torch renderer interface. The Inria-licensed reference adaptation at
`fhahlbohm/gaussian-splatting` (`f87071e1ccd6a27e3d509d6c8c280d4816f57576`)
was read as a compatibility reference; its source was not copied or relicensed.
The stock trainers/rasterizers keep their original research licenses.

## First supported scope

- 3DGS only, float32 raw Gaussian parameters, SH degrees 0–3. Native column-major
  parameter layouts receive differentiable contiguous copies at the kernel
  boundary; model Parameters and Adam slots retain their original layouts.
- Standard Torch Adam and fixed density (`densify_until_iter=0`).
- Raw renderer AA and differentiable Torch exposure postprocessing are probed.
- Default CUDA stream only: this upstream extension does not expose stream
  dispatch in its Python interface. Non-default streams are explicitly refused.
- No precomputed covariance/color overrides or custom activations/debug path.
- No depth, normal, screen-radius or screen-mean-gradient outputs. The adapter
  returns `None` for unavailable fields; it does not invent zero/approximate
  outputs to satisfy a stock trainer. Native selection falls back before the
  renderer is used when those outputs, sparse Adam or densification are needed.
- 2DGS always keeps its surfel renderer and depth/distortion/normal semantics.

## Admission and resume

`native.worker.training_acceleration.select_acceleration` is a pure policy
interface. Import success is insufficient: runtime admission needs the exact
pinned source/adapter version plus real render, backward and training checks.
The probe reports the actual extension SHA-256, Python ABI, Torch/CUDA versions
and GPU name, allowing the caller to check the currently imported binary against
the proof before admitting it. A missing/mismatched module uses stock explicitly.

An old or unspecified checkpoint renderer must remain stock. A Faster-GS resume
must have a recorded, matching engine and a verified same-engine serialized
standard-Adam state roundtrip. The tiny kernel/optimizer probe does not substitute
for the separate complete native checkpoint/sampler/RNG/exposure lifecycle test.
There is no low-precision/FusedAdam checkpoint migration in this first scope.

The independent overlay at
`D:/Apps/GSW/runtime/envs/gsw_fastgs_experimental` reads the unchanged stock
Torch 2.0.1/cu118 environment through system-site-packages. Only the uniquely
named optional extension is installed in the writable overlay. Stock Torch and
`diff_gaussian_rasterization` are not replaced. Installer defaults describe the
validated RTX 4080 Laptop / CUDA 11.8 / MSVC 14.34 host, not universal deployment
requirements. All new build/temp/cache/wheel outputs are on D.

`gsw_fastgs_probe.py` uses test-owned tiny tensors. It probes SH/AA/exposure,
performs 16 fixed-density standard-Adam steps, and compares continuous training
with a real torch.save/load split state. No user photographs, source models or
the paused iteration-4407 checkpoint are opened or rewritten. Results are not
a same-dataset reconstruction-quality or speed benchmark.

The separate opt-in `native.worker.test_training_fastgs_cuda` runs the actual
native `train.py` lifecycle on four test-owned 32×32 RGBA photos and 32 Gaussians.
It checks admission against the imported extension fingerprint, real step engine
records, pause at iteration 1, exact complete model/Adam/exposure/sampler/RNG
restore, another pause at iteration 2 and continuation to iteration 16. On the
validated host, continuous and interrupted final PLY parameters and PSNR had
zero difference; the original synthetic input hashes were unchanged. These
fixed-density lifecycle results do not establish 2DGS, densification, arbitrary
hardware compatibility or production reconstruction speed/quality.

中文：这是受能力门限制的可选 3DGS 渲染器，不替换 2DGS，不伪造深度或半径，
也不把小样本优化器验证说成整个桌面训练生命周期验证。

日本語：能力チェックで制限される任意の 3DGS レンダラーです。2DGS は置換せず、
深度・半径は捏造しません。小規模なオプティマイザー検証をデスクトップ学習の
ライフサイクル全体の検証と同一視しません。
