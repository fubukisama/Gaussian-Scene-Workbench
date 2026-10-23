# Quality method integration audit (P0)

Checked 2026-10-23 against local base `161b5581d55ea0e501c903074ccbd381c1707927` and the official sources pinned in
[PAPER_VERSIONS.json](PAPER_VERSIONS.json). This is an implementation-route and
license audit, not CUDA, training, resume, quality or release acceptance. No
upstream repository was cloned and no model weights or datasets were acquired.

## Recommended route

Preserve `canonical3dgs` as the baseline. Add small, explicit strategy/filter and
supervision seams around the existing Graphdeco model and renderer. Extend the
current native rasterizer only for the selected mechanism; do not replace it
with an older paper fork. Current source has the full-pinhole contract marker
`GSW_PINHOLE_COVARIANCE_VERSION=1`, inverse-depth output, antialiasing and native
runtime changes that the paper forks do not establish compatibility with.

An alternative gsplat backend is technically credible but is not the smallest
baseline-preserving change. Its Apache-2.0 code provides AbsGS-style absolute
gradients and an MCMC strategy. However, its parameter/optimizer dictionaries,
render conventions, strategy state and checkpoints differ from the current
`GaussianModel`; all camera, resume, worker, export and viewer proofs would need
to be repeated. Its 2D antialiasing is not a full 3D Mip filter, and the audited
strategy/rasterization interface does not establish Pixel-GS contributor counts.
[Official strategy contract](https://docs.gsplat.studio/main/apis/strategy.html),
[pinned renderer](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/gsplat/rendering.py#L373-L386).

The route below is an engineering recommendation, not an assertion that any
new method is already supported by GSW.

## License boundary

AbsGS, Pixel-GS, Mip-Splatting, DNGaussian and the official MCMC implementation
ship the Graphdeco Gaussian-Splatting research/evaluation license. Their public
availability does not grant commercial use. Preserve the complete upstream
license and notices for any copied or derived research-worker code; do not
label that code MIT, Apache or GPL. Existing GSW third-party notices already
separate restricted training components from the GPL native desktop program.
Keep that boundary explicit; a separate worker process alone is not a blanket
legal compatibility determination. [AbsGS license](https://github.com/TY424/AbsGS/blob/87d7a5aac21c0b237a6af9e715e3a0c7df4902d3/LICENSE.md),
[Pixel-GS license](https://github.com/zhengzhang01/Pixel-GS/blob/05c9d557aa3749bb1ff05e2627943afdd9c18ffd/LICENSE.md),
[Mip license](https://github.com/autonomousvision/mip-splatting/blob/dda02ab5ecf45d6edb8c540d9bb65c7e451345a9/LICENSE.md),
[DNGaussian license](https://github.com/Fictionarry/DNGaussian/blob/22a588c74db48e587294451005a05b156435b3d2/LICENSE.md),
[MCMC license](https://github.com/ubc-vision/3dgs-mcmc/blob/7b4fc9f76a1c7b775f69603cb96e70f80c7e6d13/LICENSE.md).

Clean-GS code is MIT; retain its notice if reused. Its paper license, website
license, datasets, generated masks and model weights are separate subjects.
gsplat is Apache-2.0; that does not relicense any Graphdeco code retained around
it. Before shipping a new combination, audit exact copied files, dependency
licenses and notices. This document is not a legal opinion.
[Clean-GS license](https://github.com/smlab-niser/clean-gs/blob/ba5c2fa97e9e4bec0510e25aee9f79f6bc8fa822/LICENSE),
[gsplat license](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/LICENSE).

## Method contracts and smallest implementation slices

| Method | Required information/state | Minimal compatible slice | Not sufficient |
| --- | --- | --- | --- |
| Standard | Existing signed screen-space gradient, visible-view count, radii, model/optimizer state | Preserve current defaults and off-path behavior | Rewriting baseline with a different renderer and calling it unchanged |
| AbsGS | Per-pixel absolute screen-gradient components before cancellation; separate signed optimizer gradient | Optional CUDA backward statistics plus an AbsGS density adapter | `abs()` of an already-reduced mean gradient |
| Pixel-GS | Actual contributing-pixel count per Gaussian/view; camera-Z and scene-relative threshold; signed screen gradient | Optional forward counter, depth statistic, weighted density adapter | Projected radius/area, all-image pixel count or a fixed millimetre cutoff |
| Full Mip | Training-camera sampling bound per Gaussian; raw/filtered scales and opacity; 2D Mip compensation | Python-side differentiable 3D filtering around current renderer plus renderer-equivalence tests | Current `antialiasing=true` or gsplat `antialiased` alone |
| Generic depth | Explicit convention/frame/unit/alignment/validity/confidence; differentiable renderer depth in same convention | Populate canonical cameras and use validity-normalized loss with evidence | Nonexistent/all-invalid inputs reported as active supervision |
| MCMC | Covariance/opacity-gated noise, relocation correction, budget, regularizers, optimizer surgery, distinct RNG | Separate strategy adapter; pinned relocation operator/reference; no standard split simultaneously | Adding position noise alone |
| Clean-inspired postprocess | Raw model identity, cameras, explicit mask semantics, view evidence and keep/remove reasons | Independent derived-output operation; preserve all retained attributes | Claiming pruning recreates absent detail or exact Clean-GS reproduction |

### AbsGS

The official backward kernel accumulates signed x/y derivatives for geometry and
absolute per-pixel derivatives separately (z/w of a float4). The official model
uses signed statistics for cloning and absolute statistics for splitting. Keep
those criteria and thresholds separately named. gsplat's `DefaultStrategy`
absolute mode applies the selected gradient signal to both duplication and
splitting, so it is an AbsGS-style implementation, not automatically a bitwise
match to the author model. [Author kernel](https://github.com/TY424/AbsGS/blob/87d7a5aac21c0b237a6af9e715e3a0c7df4902d3/submodules/diff-gaussian-rasterization-abs/cuda_rasterizer/backward.cu#L545-L550),
[author density model](https://github.com/TY424/AbsGS/blob/87d7a5aac21c0b237a6af9e715e3a0c7df4902d3/scene/gaussian_model.py#L461-L483),
[gsplat strategy](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/gsplat/strategy/default.py#L243-L275).

Prefer a separate `[N,2]` optional statistic rather than overloading the current
signed-gradient tensor ABI. Return its coordinate convention explicitly (pixel
or NDC), account for width and height independently, and retain signed geometry
gradients unchanged. Allocate/compute only when requested. Save accumulator,
denominator and strategy version; resize/prune alongside Gaussian IDs and Adam
states. Required proofs: two-pixel cancellation, real-CUDA statistic, resulting
split, off-path overhead, checkpoint equivalence (QT007-QT009).

Exclude the author's initial and contribution-weight pruning from the first
AbsGS ablation: the README explicitly says these additions were not used in the
paper. Do not silently filter initialization points.
[Upstream additions](https://github.com/TY424/AbsGS/blob/87d7a5aac21c0b237a6af9e715e3a0c7df4902d3/README.md#L77-L99).

### Pixel-GS

The author forward kernel increments `pixels[g]` only when that Gaussian
actually participates in compositing, after alpha and transmittance rejection.
The density numerator adds `norm(screen_grad) * pixels`; its denominator adds
`pixels`. In the wrapper, only the returned screen-statistic gradient is scaled
by `min(1, (camera_Z / threshold)^2)`; the already-computed 3D optimizer gradient
is returned separately. `threshold` is a declared dimensionless factor times
scene camera extent. Preserve this distinction so camera-near growth control
does not accidentally change the optimizer.
[Contribution counter](https://github.com/zhengzhang01/Pixel-GS/blob/05c9d557aa3749bb1ff05e2627943afdd9c18ffd/submodules/diff-gaussian-rasterization/cuda_rasterizer/forward.cu#L327-L364),
[weighted accumulation](https://github.com/zhengzhang01/Pixel-GS/blob/05c9d557aa3749bb1ff05e2627943afdd9c18ffd/scene/gaussian_model.py#L405-L407),
[depth scaling](https://github.com/zhengzhang01/Pixel-GS/blob/05c9d557aa3749bb1ff05e2627943afdd9c18ffd/submodules/diff-gaussian-rasterization/diff_gaussian_rasterization/__init__.py#L141-L162),
[scene-relative threshold](https://github.com/zhengzhang01/Pixel-GS/blob/05c9d557aa3749bb1ff05e2627943afdd9c18ffd/train.py#L84-L87).

Return optional `[N]` contributor counts and camera-Z (or equivalently derive
the latter with the canonical view transform). Do not use tile intersections,
radius or bounding-box area as coverage. Reject invalid scene extent or depth
threshold instead of changing units. Test rectangular images, camera intrinsics,
unit rescaling, occlusion/alpha rejection and weighting before quality claims
(QT010-QT012). Keep this separate from AbsGS initially.

### Full Mip-Splatting

The paper requires both a 3D smoothing bound induced by input training views
and a 2D Mip filter. The official model stores one filter width per Gaussian,
uses `sqrt(scale^2 + filter^2)`, and adjusts opacity by the square root of the
raw-to-filtered covariance determinant ratio. Its fused export bakes that 3D
scale/opacity effect into conventional attributes. Its CUDA projection also
implements the 2D covariance determinant compensation.
[Paper sections 5.1-5.2](https://arxiv.org/html/2311.16493v1),
[3D filter and opacity](https://github.com/autonomousvision/mip-splatting/blob/dda02ab5ecf45d6edb8c540d9bb65c7e451345a9/scene/gaussian_model.py#L100-L191),
[fused export](https://github.com/autonomousvision/mip-splatting/blob/dda02ab5ecf45d6edb8c540d9bb65c7e451345a9/scene/gaussian_model.py#L288-L307).

Keep raw trainable scales/opacity, persistent filter state and fused derived
output distinct. Implement the camera sampling calculation using the existing
full canonical intrinsics, not the upstream centered-principal-point assumption;
handle no visible training camera explicitly. Update filter state under the
declared schedule and topology changes, save it in full checkpoints, and never
use validation/test cameras to constrain training. Reusing current 2D
antialiasing is conditional on numerical equivalence tests, including gradients;
its presence alone is not that proof. Baking 3D state does not eliminate the
need for matching 2D filtering in the viewer; record renderer mode and prevent
double application. Required gates: QT013-QT017.

Do not import current Mip repository densification into a Mip-only comparison:
the pinned README says it includes a later GOF density metric.
[GOF addition](https://github.com/autonomousvision/mip-splatting/blob/dda02ab5ecf45d6edb8c540d9bb65c7e451345a9/README.md#L34-L35).

### Depth: generic first, DNGaussian mechanism separately

Current source already exposes a differentiable **alpha-weighted accumulated
inverse camera-Z** image, not normalized expected camera-Z: forward adds
`(1 / Z[g]) * alpha * T` and does not divide by accumulated alpha. The current
canonical camera builder nevertheless supplies no depth input. Reuse that
existing contract first only when the reference is explicitly compatible;
do not label it ray range or silently compare it with metric Z. For a different
depth estimator, add the required renderer outputs and matching gradients.
Local evidence: `canonical_training.py:375`,
`cuda_rasterizer/forward.cu:405`, `scene/cameras.py:75`, `train.py:264`.

DNGaussian is more than global-local normalization: its official route starts
from random initialization, uses a neural renderer, and runs separate hard
depth (fixed opacity; detached scale/rotation) and soft depth (detached position
and scale/rotation) passes. The reference loss has local patch normalization
and a global standard-deviation variant. Name any transplant
`dngaussian_inspired_depth`, recording which pieces are used; do not replace
SfM initialization or call generic inverse-depth L1 a DNGaussian reproduction.
[Author method](https://fictionarry.github.io/DNGaussian/),
[selective-parameter renderers](https://github.com/Fictionarry/DNGaussian/blob/22a588c74db48e587294451005a05b156435b3d2/gaussian_renderer/__init__.py#L128-L269),
[normalization losses](https://github.com/Fictionarry/DNGaussian/blob/22a588c74db48e587294451005a05b156435b3d2/utils/loss_utils.py#L85-L103).

Validity/confidence and foreground/background/unknown semantics must travel
with each view through camera creation, prefetch and loss. Normalize by valid
weight, do not treat missing depth as zero, and report valid pixels plus actual
nonzero gradients. Alignment must use declared K/distortion/pixel conventions;
the legacy camera constructor's blind resize is not a generic reprojection.
Use training-only scale/shift evidence, preserve canonical scene scale, and
disable or reject uncertain transparent/specular depth. No DPT/SAM model is
needed merely to implement this file-input contract (QT018-QT025).

### MCMC

The official trainer applies opacity and scale penalties, reuses low-opacity
samples through relocation, adds samples to a declared cap, then injects
covariance- and opacity-gated position noise scaled with the position learning
rate. Relocation corrects opacity/scale rather than simply copying centers;
its CUDA operator is in a pinned `gs-mcmc` rasterizer submodule. The model also
resets affected Adam moments. [Trainer](https://github.com/ubc-vision/3dgs-mcmc/blob/7b4fc9f76a1c7b775f69603cb96e70f80c7e6d13/train.py#L102-L142),
[relocation and optimizer surgery](https://github.com/ubc-vision/3dgs-mcmc/blob/7b4fc9f76a1c7b775f69603cb96e70f80c7e6d13/scene/gaussian_model.py#L450-L529),
[pinned relocation extension](https://github.com/shakibakh/diff-gaussian-rasterization/blob/1761eb93b8b9bfebec9fbf06af4654583c2b1ddb/ext.cpp#L15-L19).

Implement a distinct adapter and explicitly reject unvalidated standard-split
combinations. Keep noise and sampling RNGs separate from image order and
initialization; save their real states with strategy, scheduler, optimizer and
topology. Include all-dead samples, no-dead samples, cap reached, empty input,
finite relocation and resumed event-sequence tests. A gsplat MCMC adapter still
needs trainer-level regularizers (its example sets them separately) and full
GSW state restoration; its class name is not acceptance (QT026-QT027).
[gsplat example regularization](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/examples/simple_trainer.py#L1002-L1006).

#### Independent MCMC adapter slice (2026-10-23, Torch CPU implemented)

`native/worker/quality_mcmc.py` is original MIT tensor/optimizer orchestration
from published equations, not copied Graphdeco-restricted implementation and
not an official-paper reproduction. The source/licensing distinction remains
even though the restricted reference was inspected for behavior. Eq.9/18's
double binomial sum is independently collapsed with the hockey-stick identity
and evaluated in float64; opacity uses a stable log1p/expm1 root. The explicit
multiplicity numerical domain rejects excess rather than copying the author's
silent CUDA clamp. Real Torch CPU integral/peak tests cover multiplicity
1/2/7/50; no CUDA test has run. A specialized relocation kernel is not needed for this bounded CPU slice;
large-scale CUDA performance and equivalence remain unverified.

The implemented public seam is `MCMCAdapter(parameters, optimizer, seed=...,
cap=...)`. Required tensors are `means[N,3]`, `log_scales[N,3]`,
`quaternions[N,4]` in wxyz order and `opacity_logits[N,1]`; optional floating
per-Gaussian attributes follow the same topology. One real `torch.optim.Adam`
has one named group per tensor. An explicit `optimizer_group_names` mapping
binds canonical keys to existing group names without renaming them. Checkpoint
format v2 binds that mapping in its identity; old v1 adapter snapshots are
explicitly incompatible. Zero weight decay and descent are enforced.
Add `regularization_loss()` before backward, then call
`step(position_lr=..., refine=..., grow_count=...)`. The adapter performs actual
Adam drift, clears gradients, optionally relocates/grows, then applies noise.
Mean-reduced opacity/scale L1 matches the executable reference; an explicit
sum reduction is also available. Calling the noise helper alone is not MCMC.

Targets are sampled proportional to live opacity before any family correction.
Live parent Adam moments reset; recycled dead-slot moments remain, and newly
added slots start at zero moments. Growth rebinds every Parameter, optimizer
reference and row-shaped moment, preserves extra attributes, and honors cap.
Empty/all-dead/no-dead/cap-reached paths report explicit reasons. Numerical
multiplicity rejection leaves topology and sampling RNG unchanged. The default
50-copy domain is tested; overrides above 50 are not numerically accepted by
this test report.

There are deliberate differences from the author trainer: topology occurs
after Adam (not before), growth samples live parents only, and the pure Eq.9
opacity correction is not forced up to the author's minimum opacity floor.
Consequently, a corrected low-opacity family may still classify as dead on a
later step. These distinctions must remain in manifests/experiment names; this
is not a bitwise official reproduction or a quality-approved replacement.

`state_dict/load_state_dict` snapshot all tensor attributes, Adam groups,
defaults and moments, stable Gaussian IDs, counters, event metadata and separate
noise/relocation Torch generator states. Restore validates a complete candidate
before commit. Checkpoints require cleared gradients, matching config, device,
Torch version and parameter layout. A CPU continuation test grows the pool,
restores into a fresh adapter, and exactly matches later events, parameters and
all Adam tensors. This is complete **adapter** restoration, not complete worker
resume: the caller still owns dataset/sampler/scheduler/renderer identity and
state. Whole-step failure after Adam is not rolled back; abort that attempt and
resume the prior valid checkpoint, never label a failed step completed.

arXiv v3 Eq.8's displayed sigmoid sign conflicts with its suppression prose and
the pinned author's `train.py`; the implementation uses the executable
low-opacity-high-noise sign `sigmoid(k*(t-opacity))`, with `Sigma @ eta`, not
`sqrt(Sigma) @ eta`. [Paper equations](https://arxiv.org/html/2404.09591v3),
[pinned executable reference](https://github.com/ubc-vision/3dgs-mcmc/blob/7b4fc9f76a1c7b775f69603cb96e70f80c7e6d13/train.py#L134-L142).

Validation: `python -m unittest native.worker.test_quality_mcmc -v` passes 19
real Torch CPU tests, including drift, covariance/rotation/gate values,
regularizer gradients, cap/moments, RNG isolation, exact continuation, invalid
state and explicit rejection. No fake optimizer, mocked CUDA, GPU kernel,
training loop, renderer, UI or real dataset is used. CUDA throughput/numerics,
worker integration, mixed strategy combinations, 2D surfel equations, rendered
appearance and quality/default promotion are all remaining acceptance gates.
The adapter's MIT notice does not change restrictions on a Graphdeco trainer
or other restricted component it may later accompany.

#### Actual GaussianModel protocol bridge (CPU protocol validation only)

`native/worker/quality_mcmc_model.py` provides `GSWMCMCBridge` over the actual
GSW attribute/group protocol. Its fixed mapping is:

| Canonical key | Model attribute | Existing Adam group |
| --- | --- | --- |
| means | _xyz | xyz |
| features_dc | _features_dc | f_dc |
| features_rest | _features_rest | f_rest |
| opacity_logits | _opacity | opacity |
| log_scales | _scaling | scaling |
| quaternions | _rotation | rotation |

The caller performs Adam once; `after_optimizer_step(position_lr=..., refine=...,
grow_count=...)` verifies real Adam counters advanced exactly once, clears the
consumed gradients, and runs only the post-optimizer MCMC phase. It cannot call
Adam twice. `xyz` stays `xyz`, preserving native `update_learning_rate` and
native optimizer capture/restore group layout. Growth rebinds model attributes
to the new Parameters. Required density-statistic lengths stay N-aligned;
topology work clears those unused Standard statistics, preserving their dtype
and device. Temporary radii remain None or resize coherently. Exposure objects
and unrelated model metadata are not modified. Standard split/prune/reset
must remain disabled in the trainer's MCMC branch; no trainer is changed here.

Before any topology commit, `preflight_topology` simulates the complete
relocation-then-growth draw sequence with cloned opacity/log-scale tensors and
a cloned sampling RNG. Both multiplicities and formula domains must pass.
Thus a valid relocation cannot commit before a later growth-multiplicity
rejection. Exact planning uses the **post-Adam** opacity distribution; a
pre-Adam estimate cannot guarantee post-Adam categories or weights. Caller-owned
Adam is not rolled back by this preview. If the post-phase rejects, bridge
capture is refused as incomplete; restore the prior complete native capture
and sidecar to roll back the iteration. Memory-allocation/device failures are
not claimed transactional. Parameter references synchronize even on a later
failure so model/optimizer are not left pointing at different pools.

Resume order is explicit:

```python
saved = bridge.capture()  # native model.capture() plus full bridge sidecar
# On resume, retain the trainer's own additional worker/exposure/sampler state.
model.restore(saved['model'], training_args)
bridge.rebind_after_model_restore(saved['bridge'])
```

The rebind rejects the wrong native parameters/Adam/statistics rather than
silently overwriting a mismatched capture. Native restore may replace both
Parameter objects and the optimizer; stale bridge calls fail until rebind.
Checkpoint copies/equality validation have not passed CUDA memory/performance
acceptance. A live `_gsw_capacity_storage` manager is explicitly rejected with
`quality_mcmc_capacity_storage_adapter_required`; it is never detached or
disabled. Non-default/sparse Adam needs its own adapter and acceptance. Root
resource policy still owns growth admission; Gaussian cap is not a VRAM proof.

`python -m unittest native.worker.test_quality_mcmc native.worker.test_quality_mcmc_model -v`
passes 26 real Torch CPU tests (19 core, 7 bridge). The model facade carries real
Parameters and Adam and tests the public protocol without mocked updates; it
does not instantiate the CUDA-hardcoded Graphdeco GaussianModel or establish
renderer/worker/GPU success. Actual native training, memory-policy/capacity
integration, quality, other backends and release/default promotion remain
separate gates. No quality-kernel closure, trainer, controller or runtime
selector is changed by this bridge slice.

### Clean-GS-inspired independent postprocess

The official preprint describes mask whitelist, depth-buffered color checking
and neighbor outlier stages. Its code uses masked RGB with a near-black cutoff,
resizes masks, projects Gaussian centers and compares SH-DC color; its README
and paper titles also differ slightly. These are source-specific assumptions,
not a generic mask/depth contract. [Paper](https://arxiv.org/abs/2601.00913v1),
[actual script](https://github.com/smlab-niser/clean-gs/blob/ba5c2fa97e9e4bec0510e25aee9f79f6bc8fa822/clean-gs.py#L70-L144).

Prefer a separately named GSW operation consuming explicit masks and canonical
cameras, writing a new model plus stable Gaussian IDs, source hashes, view
evidence and per-removal reasons. Preserve every attribute of retained
Gaussians. Mask-unseen/occluded parts and thin structures require explicit
tests; a center outside a mask is not necessarily the whole splat outside the
object. Make optional color/neighbor rules visible and predeclare thresholds.
No generic pruning result proves the absent high-frequency detail was learned
(QT028-QT030).

## Kernel capability and ABI gates

Add a semantic capability version rather than inferring support from an import
or module name. A future probe must report source and binary hashes, Torch/CUDA
build identity, full-pinhole version, output names/shapes/conventions and
optional support flags for `abs_screen_grad`, `contributor_pixels`,
`camera_z`, `render_alpha`, and depth convention/gradient. Validate the exact
requested combination before creating a job. An old binary must fail clearly;
never silently run Standard.

Keep off-path forward/backward signatures and allocations stable where
possible. If an ABI changes, bump its version and rebuild/prove wrapper and
binary together. Preserve current sparse optimizer, exposure, prefetch,
checkpoint and camera-contract behavior. Add no always-on large image/Gaussian
buffers just to advertise an optional method.

## 2DGS and other generation paths

Shared QualityPlan identity, supervision metadata, raw/derived provenance,
benchmark protocol, status and fail-closed capability checks must cover 3DGS
and 2DGS together. The examined 3D papers do not establish their formulas as
validated 2D surfel equivalents. Full 3D Mip covariance fusion and MCMC
volumetric relocation are not automatically valid for surfels. Record those
as unsupported with reasons until backend-specific derivation and CUDA tests
exist; do not present a PLY warm start as optimizer-state resume. gsplat's
2DGS/absolute-gradient API is a possible future source, not evidence for the
installed `diff_surfel_rasterization` binary.

## Acceptance and current status

P0 source/license pinning is complete for these six references. Algorithms,
Windows/CUDA binary compatibility, checkpoint reproducibility, worker execution,
native renderer equivalence and quality improvement remain **not run in this
audit**. No defaults should be promoted from source inspection. Run independent
method contrasts before combinations, using the fixed fixture protocol and
bounded budgets; preserve poor and inconclusive results. The official baseline
FAQ's reduced position/scaling learning-rate contrast is a useful inexpensive
first experiment, not proof that this alone repairs any specific scene.
[Official FAQ](https://github.com/graphdeco-inria/gaussian-splatting#faq).
