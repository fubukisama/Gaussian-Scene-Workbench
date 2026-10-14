# Native training performance / 原生训练性能 / 原生学習の性能

## Shared 3DGS and 2DGS runtime

Both native trainers use the same bounded image staging, pre-growth memory
admission, full-step telemetry and quality-first policy. 2DGS keeps its surfel
renderer and depth/distortion/normal terms. Mesh-generation stages consume the
result as before; they do not gain an optimizer-state resume capability.
In the new 2DGS runtime, zero-weight depth/distortion/normal regularization
does not construct its unused loss/autograd path. Active terms retain the
original formula/order and schedules. This does not disable the surfel
rasterizer's auxiliary forward outputs; the legacy runtime keeps its old graph.

| Capability | 3DGS | 2DGS |
| --- | --- | --- |
| Lossless 8-bit host cache; float/HDR fallback | Yes | Yes, native adapter; external source unchanged |
| Bounded pinned pool and 1–2 CUDA image slots | Yes | Yes |
| Pre-growth admission using actual tensor/Adam byte sizes | Yes | Yes, two-component scales |
| Quality-first full-state pause after three consecutive denials | Yes | Yes |
| Optional MIT fused L1/SSIM, numerical and ABI gate | Yes | Yes; geometric losses remain separate |
| Experimental Faster-GS | Fixed-density standard-Adam only | Not supported; keep surfel renderer |

The default VRAM target is90%, with at least512MiB spare. The actual spare is
the larger of that minimum and the target's unused fraction. Admission accounts
for old and replacement tensors coexisting during clone/split/prune, gradients,
Adam moments and density statistics. Only half of wholly inactive allocator
blocks is credited; active/pending storage and split fragments are excluded.
This is a conservative estimate, not a universal hard quota or an OOM guarantee.

Memory pressure does **not** delete existing Gaussians or downsample images.
It initially defers growth without clearing its statistics. Persistent deferral
can reduce detail if training keeps running until the growth schedule ends.
Therefore **Quality first** is the default: after three consecutive denials,
the trainer finishes the optimizer update, durably saves complete state and
pauses. **Continue** is an explicit alternative with a retained quality warning.
Free resources before resuming; repeatedly resuming under the same pressure is
not a quality or speed solution.

Old checkpoints without performance metadata retain their legacy execution
mode unless the user checks the upgrade option in the existing trust-confirmed
Resume prompt. That option enables exact-pixel image storage/transfer and
quality-first resource protection while retaining the original optimizer,
sampler, resolution, loss and renderer. No dataset/optimizer identity check is
disabled. New checkpoints retain the chosen runtime policy, actual renderer and
growth counters. PLY warm starts are never labelled full-state resume.

## Timing and observability

The speed and ETA use a completed, synchronized **whole-step wall sample**,
including optimizer, growth and preview/checkpoint work. The high-resolution
`perf_counter` uses QPC on the installed older Windows Python; its coarse
`monotonic` clock is not used for millisecond samples. Sampling is normally once
per30 iterations, not an unconditional CUDA synchronization every iteration.

Host intervals include GPU submissions and synchronization waits. CUDA event
intervals are not pure kernel time; the two maps can overlap and must not be
added. The legacy render/loss/backward CUDA interval is explicitly not a full
step and cannot manufacture it/s or ETA. The monitor shows the sample iteration,
actual renderer/loss, Torch allocated/reserved/job-wide peak and measured device
free/total. A Qt warning remains visible if the last growth decision was denied.
WDDM shared-memory samples can include mapped pinned host storage; nonzero
shared usage alone is not proof of VRAM spill.

## Optional kernels and publication boundary

The release fused-loss extension is derived anew from MIT `rahul-goel/fused-ssim`
commit `1272e21a282342e89537159e4bad508b19b34157`, with original GSW combined L1
reduction/backward additions. It preserves same/zero padding, FP32 partials,
`sign(0)=0` and the original photometric objective. Source/license/ABI2 metadata
and real CUDA value/gradient tests are checked at startup. Failure or unsupported
input selects the existing loss and reports the effective engine. Build it in
an isolated overlay with `scripts/gsw_fastgs_experimental_loss_install.ps1`;
set the backend-specific `GSW_TRAINING_LOSS_MODULE_PATH_3DGS` or `_2DGS` to its
site-packages. Native staging excludes compiled `.pyd`, build and egg-info files.

The private GPL LichtFeld prototype is not the release kernel and is not
included in packages. The original desktop main program remains GPL; independent
MIT helpers and Apache-2.0 Faster-GS do not relicense restricted research
trainers. See[third-party notices](../THIRD_PARTY_LICENSES.md).

Faster-GS is pinned to `c60ea269b7cf3ebda527d640b6fdd31462f91b41` and remains
experimental. Its current Torch API has no Graphdeco screen radii, view-space
point gradients or depth output. Native admission therefore requires fixed
density, standard Adam, supported SH0–3 and a verified matching runtime
fingerprint. Unsupported density/sparse-Adam/depth/normal/covariance/debug or
legacy-resume configurations keep the standard renderer; features are not
disabled to force compatibility. Default training presets still densify and
therefore do not silently become Faster-GS experiments. Full optimizer recovery
requires the same recorded renderer; kernel import or a tiny serialized Adam
probe alone is not full native lifecycle acceptance.

Low-precision/quantized Adam, changing the density algorithm and replacing the
2DGS surfel rasterizer remain separate research gates. No such capability or
speedup is implied by this runtime upgrade.

## Reproducible bounded acceptance

`scripts/training_stress.py` copies photos/reconstruction to a fresh independent
output, hashes originals before/after, caps the test allocator to75%, samples
whole steps and optionally observes the test PID's WDDM memory. A time limit
requests durable pause, then terminates only the test-owned process after a
bounded grace period. It never resumes the user's saved experiment. A copied
PLY initialization is explicitly reported as warm start, not optimizer resume.

The user-authorized local fixture has59JPEG images at3804×2142. Short paired
runs use the same resolution/cameras/model size/schedule; they are not a
30,000-step convergence or final-quality benchmark. Artifacts remain local,
including photos and source hashes. No user photos are uploaded to CI.

Tests cover both real CUDA runtimes, exact host pixels/masks/depth/camera
matrices, bounded stream lifetime, optimizer/RNG recovery, joint-loss values and
gradients, resource-denial preservation, and three-language real Qt monitor
behavior. Local tests do not launch UI; GUI acceptance uses a separate Windows
runner. See[CI boundaries](NATIVE_CI.md).

## 中文

3DGS 与2DGS共用有上限的图像传输、增密前显存估算及整步监测。8位照片保留
uint8主存数据，高位深/HDR不强制降成8位；2DGS的曲面、深度、失真和法线损失
保持不变。默认显存目标90%，至少留512MiB，并计入增密期间旧/新张量共存的峰值。
这不是全程序硬配额，也不能保证任何模型都不会显存不足。

暂缓增密可能影响细节。默认“质量优先”在连续3次被拒绝后，完成优化器更新、保存
完整状态并暂停，不删除已有高斯、不降分辨率。旧实验通过原有信任确认中的勾选项
选择新缓存/保护策略，保留原优化器、随机采样、分辨率、损失与渲染后端；不自动续训。
整步速度与CUDA局部阶段分开，显示实际后端与显存；GPU共享内存含锁页主存映射时，
非零值不等于显存溢出。联合损失核经过数值/梯度门，缺失时明确保留原损失。
Fast-GS仅是受限的3DGS固定密度实验路径，不能替代2DGS或默认增密流程。

## 日本語

3DGSと2DGSは、上限付き画像転送、増密前のGPUメモリ判定、ステップ全体の測定を
共有します。8ビット画像は主存でuint8のまま保持し、高ビット深度/HDRを強制的に
8ビットへ変換しません。2DGSのサーフェル・深度・歪み・法線項は維持します。
既定の目標は90%、最低512MiBを確保し、増密時の旧/新テンソルの共存も見積もります。
プログラム全体の厳密な割当量でも、メモリ不足が絶対に起きない保証でもありません。

増密の延期が続くと細部が減る可能性があります。既定の「品質優先」は連続3回の
拒否後、最適化器更新を終えて完全な状態を保存し、安全に一時停止します。既存の
ガウシアンを削除せず、解像度も下げません。従来の実験は信頼確認時の選択で新しい
キャッシュ/保護を使えますが、元の最適化器・サンプリング・解像度・損失・描画を
保持し、自動的には再開しません。CUDA区間と全体速度を区別し、実際のバックエンド
とメモリを表示します。共有GPUメモリには固定主存のマッピングが含まれ得ます。
融合損失は数値/勾配を検証し、利用不能なら従来の損失を維持します。Fast-GSは
増密なしの3DGS実験用であり、2DGSや既定の増密処理の代替ではありません。
