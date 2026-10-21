# Independent training runtime controls / 训练运行策略解耦 / 学習実行設定の分離

This native upgrade separates resource admission, execution optimizations and
monitoring. It does not change the training method, measured scene units,
initialization prior or scientific accuracy threshold. The shared implementation
covers native 3DGS and 2DGS; downstream meshing/refinement programs do not inherit
Gaussian optimizer or resource-guard capabilities merely by sharing this UI.

Implementation and CPU regression evidence are present in the current change.
The final commit-bound GPU matrix, full-state continuation, native window
lifecycle, three-language and installed-package gates must be recorded before
calling this a verified release. A successful configuration preview is not
training, a resource observation is not an allocation guarantee, and a short
training run is not geometry-accuracy certification.

## Independent settings

| Group | Setting | Meaning |
| --- | --- | --- |
| Resource guard | `enabled` | Enable only software pre-densification resource admission and its configured response |
| Resource guard | `mode` | `pause`: save complete state and pause after persistent denials; `defer`: keep training with growth deferred and an explicit quality warning |
| Resource guard | `usage_fraction`, `minimum_spare_mib` | Target utilization and minimum device-free reserve, not a hard quota or an OOM guarantee |
| Resource guard | `max_consecutive_denials` | Number of consecutive denied growth opportunities before quality-first pause |
| Execution | `image_staging` | Exact-pixel host storage and bounded image transfer; not image downsampling |
| Execution | `camera_prefetch` | Predict the next sampled camera without changing RNG or sampler order; requires image staging |
| Execution | `capacity_storage` | Optional verified FP32 parameter/Adam capacity reuse; unsupported layouts keep the standard allocation path |
| Execution | `surfel_auxiliary` | 2DGS on-demand auxiliary-map and regularization-graph work; active geometric loss terms remain enabled |
| Execution | `loss_mode` | `stock` or `auto`; auto may use the verified fused L1/SSIM implementation or report a standard-loss fallback |
| Execution | `engine` | `stock` or experimental `fastergs`, subject to the unchanged backend and resume compatibility gates |
| Execution | `gpu_slots`, `pinned_mib`, `gpu_mib` | Limits for the optional image-transfer implementation |
| Monitoring | `profile_interval` | Sampling interval for whole-step/stage observations, independent of resource admission |

The native **recommended optimizations** action is an explicit preset. It lists
the execution settings it changes; it is not a parent bool that remains above
the individual controls. Applying it does not change the guard, cameras, image
resolution, training iterations or densification parameters. Individual choices
remain editable. Renderer/loss availability is resolved and reported at runtime,
not certified by a checked box.

Guard **OFF** bypasses the software growth-admission probe and estimate: it must
not defer/deny growth or automatically pause because of that policy's thresholds.
The machine event is `bypassed` / `guard_disabled`, explicitly unmeasured rather
than a fabricated successful memory measurement. Ordinary monitoring and
separately enabled optional caches may still observe resources; their fallback
must not become a hidden growth denial. Manual pause, stop and complete-state
save remain available. Guard OFF does not prevent an actual CUDA allocation
failure and is not equivalent to guard ON with `mode=defer`.

Preview controls remain separate. Pausing a preview freezes observation, not
optimization. Its own bounded display-resource fallback is not a change to the
training guard or the original model. Disabling the training guard does not
rewrite preview settings.

## Version-two configuration

The native worker configuration uses one `runtimeOptions` container:

```json
{
  "runtimeOptions": {
    "version": 2,
    "resourceGuard": {
      "enabled": true,
      "mode": "pause",
      "usage_fraction": 0.9,
      "minimum_spare_mib": 512,
      "max_consecutive_denials": 3
    },
    "execution": {
      "image_staging": true,
      "camera_prefetch": true,
      "capacity_storage": true,
      "surfel_auxiliary": true,
      "loss_mode": "auto",
      "engine": "stock",
      "gpu_slots": 2,
      "pinned_mib": 256,
      "gpu_mib": 512
    },
    "monitoring": {"profile_interval": 30}
  }
}
```

`native/worker/training_configuration.py` owns normalization and validation for
native GUI/CLI/worker/batch/recipe paths. Unknown keys, non-boolean boolean
values, non-finite numeric values, unsupported versions and contradictory
old/new settings are explicit errors. A version-two utilization target may be
50–100%; a 100% target or zero reserve remains an explicit risk, not a promise
that allocations will fit.

The normalizer returns four distinct records:

- `requested`: the canonical version-two user request; this is the wire form.
- `effective`: static dependency resolution, not proof that a CUDA kernel ran.
- `reasons`: machine-readable dependency/fallback reasons.
- `migration`: input source, old/new schema versions and migration notes.

For example, staging OFF and prefetch requested ON preserves the request but
reports prefetch effective OFF with `requires_image_staging`. The UI must not
silently uncheck the child, nor leave it looking active without that explanation.

Batch plans and comparison recipes retain the report with
`runtime_configuration_scope=dependency_resolution_only_runtime_not_executed`.
Their creation/export/reopen operations start no trainer. Observed training
reports separately retain actual image-transfer statistics, renderer admission,
capacity/fallback status and observed loss-engine history. `loss_mode=auto` is
not itself the name of an executed kernel. The numerical/gradient verification
requirements in [Training performance](TRAINING_PERFORMANCE.md) still apply;
neither fused losses nor alternative renderers are declared bitwise equivalent
without their appropriate evidence.

## Legacy migration and full-state continuation

Old `performanceOptions` version one remains readable. Its former parent bool
controlled several effective paths. Migration preserves that old effective
behavior instead of interpreting checked children as newly active features:

| Old input | Migrated resource guard | Migrated execution |
| --- | --- | --- |
| `enabled=true` | ON with old mode/thresholds | Preserve the old child selections and loss policy |
| `enabled=false` | OFF | Staging, prefetch, capacity and surfel auxiliary OFF; stock loss, even if the old child fields remained true |
| Old resume with no policy metadata | Preserve the old unoptimized/off effective path | Do not silently enable new optimizations |

The legacy renderer selection retains its former behavior; it was not controlled
by that parent bool. If both old and new containers are supplied, they must be
semantically equal after migration; otherwise normalization refuses the
conflict. The worker emits only the canonical active wire form and records the
migration. Legacy batch request serialization has a dedicated, strictly
validated compatibility helper so existing semantic run IDs do not change just
because the configuration is described by a newer implementation.

Source identity is a different check. Batch/recipe replay still verifies its
recorded source fingerprints and does not accept modified source by trusting a
stored digest or matching run ID. Recreating a comparison against a new source
version is explicit and produces a new source-bound record; it is not a way to
relabel an incompatible saved optimizer state as resumable.

To change only protection on a complete-state continuation, use a version-two
partial override, for example:

```json
{"runtimeOptions":{"version":2,"resourceGuard":{"enabled":false}}}
```

On resume, omitted execution fields inherit the validated checkpoint policy.
They must not revert to fresh defaults. The runtime records the before/after
canonical policies, checkpoint iteration and
`explicit_resume_runtime_override` reason. Data bytes, known cameras, full
intrinsics, reversible scale chain, seed/RNG/sampler, optimizer, density state
and source compatibility are still checked. Renderer identity remains a resume
gate. A PLY warm start or model reinitialization is not full-state continuation.

The supported change route is complete-state pause and explicit continuation.
Live mutation during an active iteration is not advertised until separately
implemented and verified. Changing execution choices such as a loss kernel may
change floating-point trajectories; its observed history must remain visible.

## Memory evidence and failure classes

Memory accounting distinguishes device free/total, Torch allocated, Torch
reserved, known wholly inactive reusable blocks, excluded split/active storage
and the additional densification peak. Reserved bytes are neither wholly used
nor wholly free. Old/new parameter and Adam storage can coexist during growth.

When a preliminary decision is short of actual device-free reserve but has
eligible inactive cache, the guarded path may reclaim it and remeasure device
free memory before deciding. The event records attempted reclamation, result
and the remeasured observation. A cache-credit estimate alone must not waive the
minimum actual-free check. Unknown probes/estimates are not successful zeros.

| Observation | Meaning / response |
| --- | --- |
| `guard_disabled` | Admission bypassed; no software resource denial or automatic resource pause |
| `vram_pressure` | Measured/estimated policy denial; pause or defer as explicitly configured |
| `probe_unavailable` / invalid observation | Resource observation failed; retain a distinct resource-pause reason |
| `estimate_unavailable` | Growth estimate failed; do not call it measured VRAM pressure |
| `cuda_oom` | Observed typed Torch allocator exception; training fails with the original exception |
| `cuda_oom_reported` | Explicit CUDA OOM text from older RuntimeError; inferred separately from a typed allocator error |
| `training_exception` | Other Exception; not silently renamed OOM |

The actual native trainer entry emits `[gsw-training-error]` and re-raises the
original Exception. It never saves a partially failed model over the last
complete checkpoint. Unknown completed-step/checkpoint-preservation facts are
omitted rather than asserted. Explicit process exits and keyboard interruption
retain their semantics. A typed-error injection test proves classification,
not physical memory exhaustion or successful recovery from arbitrary CUDA
failure. No deliberate exhaustion is required for acceptance.

## Existing-job monitoring

Observation is separate from starting training. The connection interface reads
an explicit job store/job ID and verifies the session, heartbeat and OS process
creation identity; a PID alone is insufficient. It does not call the old
backend recovery loader that rewrites historical running rows as cancelled.
Pause/stop requests require a verified live session and a bounded command inbox;
observers do not own or kill the observed process. Merely seeing `ready.json`
means a checkpoint is present, not that its optimizer/hash/source has passed
the complete resume checks. Old jobs without connection metadata remain
unverified/read-only rather than being falsely called live or controllable.

Closing/disconnecting observation must leave the job and source data intact.
The native GUI's reconnect, detach, pause/stop and explicit resume lifecycle is
a separate real-window acceptance gate; do not infer it from the JSON interface
or claim persistent main-window-exit behavior before that gate passes.

The detached launch/observer lifecycle applies to individual native 3DGS/2DGS
training and full-state continuation. The batch coordinator still uses its
explicit owned group lifecycle: closing/cancelling that coordinator is not a
promise that every queued batch child will survive. Child configuration uses
the same independent runtime contract; detaching the coordinator itself remains
a separate acceptance boundary, not an implicitly enabled capability.

## Backend support and retained gates

| Pipeline | Shared runtime controls | Necessary limits |
| --- | --- | --- |
| Native 3DGS | Independent guard/staging/prefetch/capacity/loss/renderer/monitoring and complete-state continuation | Faster-GS remains experimental, fixed-density/standard-Adam with verified ABI/runtime only; unsupported features retain the standard renderer rather than disabling training terms |
| Native 2DGS | Same independent common controls; surfel auxiliary option; complete-state continuation | Official external surfel runtime remains required; no 3D Faster-GS substitution; active normal/distortion/depth terms are preserved |
| Bounded/unbounded TSDF | Consumes supported 2DGS results; separate external stage | No Gaussian guard or optimizer-resume claim for TSDF; canonical-camera adapter remains gated where unverified |
| SuGaR | Separate refinement/mesh/texture execution | Its optimizer and checkpoint format are not the native Gaussian complete-state format |
| GS2Mesh | Separate rendering/depth/fusion execution | New guard settings do not certify stereo/refinement memory behavior or canonical-camera mapping |
| OpenMVS texturing | Separate explicit geometry/material job | No automatic native Gaussian resume/guard capability and no unverified canonical-camera unlock |

Locked camera versus SfM rerun exclusion, image resize versus full-K update,
backend capability, scale/coordinate transforms and data-split checks are
necessary dependencies. They are not removed in the name of independence.
Image content from the test split must not feed initialization, optimization,
refinement or geometry extraction. Unknown units stay unknown. Export and
project save remain separate operations with provenance; this runtime change
does not turn a fitted residual into physical accuracy.

## Bounded planning/acceptance commands

The project Python interpreter can inspect a planned policy without any input
files or training:

```powershell
& $Python -B scripts/training_stress.py --runtime-plan --guard off --optimizations on
& $Python -B scripts/training_stress.py --runtime-plan --guard on --optimizations off
& $Python -B scripts/training_stress.py --runtime-plan --guard off --optimizations on --image-staging off
& $Python -B scripts/training_stress.py --runtime-plan --guard on --guard-mode defer --guard-denials 5
```

Use both backend tags and all four ON/OFF combinations for actual bounded
acceptance. `--optimizations` is an explicit execution preset; per-feature
arguments override it. `--case legacy` preserves the old harness OFF/OFF meaning.
`--baseline-runtime` preserves its previous prefetch/capacity/surfel-only subset.
The real stress run additionally requires explicit dataset/output/interpreter
arguments, creates independent input copies and retains its documented allocator
cap and owned-process timeout. It never resumes the historical user experiment.

CPU regressions cover normalization/conflicts/dependencies, legacy semantics,
batch and recipe reopen/tampering, explicit control pipes and failure events.
The release evidence must also bind actual 3DGS/2DGS four-way GPU runs, bounded
growth/prune/full optimizer restores, known-camera/scale/test isolation,
per-feature numerical tolerance checks, existing-job lifecycle, three locales
and installed source/EXE/Setup/shortcut identity. Current short or CPU success
must not be written as those pending gates having passed.

## Recorded-frame and metric delivery (instruction version3)

`native/backend/camera_scale_export.py` is the shared CPU module for manual
coordinate export and automatic native result delivery. The module imports no
Torch/renderer and never changes training, checkpoints, source photographs or
the original model/metadata.

- `inspect_coordinate_metadata(input_ply, sidecar=None)` validates the model
  bytes, sidecar, transform chain, applied state, data identity and any camera
  bundle. It exposes current frame/unit, provenance and a current-to-canonical
  Sim3. `metric_available` must be checked before calling that canonical frame
  metres; arbitrary/unknown coordinates do not acquire a guessed unit.
- `export_camera_scale_ply(..., output_unit=None)` preserves its original
  argument order. `--coordinates source|canonical|metric|training|preserve` and
  optional `--unit m|cm|mm` use the actual current file as their starting frame.
  Metric means canonical metre coordinates by default, not source-unit scaling
  applied twice. Re-export to the same frame/unit is an identity operation.
- `deliver_training_result(input_ply, output_directory, ...)` defaults to the
  separate `<stem>.metric-m.ply` only with valid known-scale provenance.
  `training_status=done` remains separate from `export_status`:
  `ready`, `blocked` (unknown metric scale), `failed`, or `cancelled`. A retry
  reuses only a complete matching bundle from this exact input+metadata hash.
  The raw input is retained and used as the non-metric fallback, never labelled
  a successful metric export.

Ordinary export exceptions, including allocation failures, retain the confirmed
optimizer completion and raw model/checkpoint. A later nonzero worker exit does
not revoke an already received optimizer-completed status; the independent
delivery/association error remains recorded. Manual model export retries only
the coordinate stage through this same service. It does not silently rewrite
the original job's automatic-delivery receipt.

The sidecar keeps `schema=gsw.coordinate_export/1` for native compatibility and
adds `export_version=2`, actual `current_frame/current_unit`, output frame/unit,
`transform_applied=true`, and an applied Sim3 whose `applied` flag is also true.
`training_contract` is always the immutable root
`gsw.training_camera_contract/1`, not another export wrapper. Historical nested
wrappers are bounded/unwrapped with identity/hash checks. Crop operations retain
point order, attributes, the new model hash and their input/metadata ancestry;
stale edited files cannot silently borrow the old hash-bound metadata. Baking a
separate scene TRS is not part of this Gaussian-coordinate interface and remains
an explicit unsupported native combination, rather than a guessed transform.
The root dataset identity is the canonical 64-character lowercase SHA-256 value,
matching native model/camera readers; opaque test labels are not valid identities.

Every export publishes a model-specific `<stem>.cameras.json`:

```json
{"schema":"gsw.model_cameras/1","version":1,"model_sha256":"...","input_data_identity":"...","coordinate_frame":"canonical","coordinate_unit":"m","transform_applied":true,"camera_count":1,"views":[{"view_id":"stable-id","split":"train","width":640,"height":480,"K":[[500,0,320],[0,500,240],[0,0,1]],"c2w_opencv":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],"projection_model":"PINHOLE","pixel_coordinates":"pixel_border","image_path":"recorded/path.png"}]}
```

This JSON is illustrative, not a baked-in product calibration. Actual complete
K, image dimensions, stable IDs/splits and camera poses come from the root
contract and use the same final frame/unit as the model. Near/far/backend clip
lengths are scaled too. Missing image files do not preclude pose-only display;
missing camera records produce an explicit zero-camera bundle, not invented
poses. Each sidecar has `camera_reference={path,sha256,schema}`. Its
`bundle_sha256` hashes compact, sorted, ASCII-escaped UTF-8 JSON with exactly five
string keys: `camera_sha256`, `coordinate_frame`, `coordinate_unit`,
`input_data_identity`, `model_sha256`. It does not hash its own sidecar bytes or
create a circular dependency. PLY/camera/sidecar are exclusively published as
one usable group; failures roll back only this writer's files and preserve a
competing writer's files. A bad present reference never authorizes borrowing an
unrelated nearby `cameras.json`.

| Representation | Coordinate/unit restoration | Direction/appearance handling |
| --- | --- | --- |
| 3DGS PLY | XYZ float64; three log sizes add `log(s)` | Quaternion, optional six-component covariance, normal/tangent/bitangent vectors; real SH0–4 rotation |
| 2DGS PLY | XYZ float64; two log sizes, no invented third size | Surfel orientation and the same declared covariance/direction/SH handling |
| Ordinary XYZRGB points | Not accepted by this full-Gaussian interface | Must use a separately validated point-cloud route; not a restored physical surface |
| Indexed meshes | Explicitly refused by this interface | Needs format-specific vertices/normals/material/indices handling; not silently treated as GS |
| Reflection, non-uniform scale, unrecognized SH layout/rotation degree | Explicit refusal | No positive-Sim3 or appearance-equivalence claim |

The SH rotation uses independently implemented associated-Legendre real
Condon–Shortley harmonics in `l*l+l+m` order. A band-preserving change of basis is
calculated in float64, then checked for orthogonality and on independent
directions before coefficients are written in their existing storage type.
This is not a new CUDA renderer. The convention was checked against the pinned
[gsplat forward evaluator](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/gsplat/cuda/csrc/SphericalHarmonicsCUDA.cu)
(Apache-2.0; existing vendored notices retained), which references
[Sloan's SH evaluation paper](https://jcgt.org/published/0002/02/06/).
The public export tests independently evaluate before/after direction functions
with the existing BSD PlenOctree polynomial evaluator in
`gaussian-splatting/utils/sh_utils.py`; no upstream training implementation is
copied into this restoration module.

CPU acceptance covers both Gaussian representations, m/cm/mm, normalization
on/off, nonzero translation, non-identity rotation and large origins (48 matrix
conditions), repeated/current-frame exports, hashes/ancestry, cancellation,
publication races, camera K/pose/clip transforms, and SH degrees1–4 in both
backends. SH direction-function absolute tolerance is `2e-7` after float32
coefficient storage; basis-only residual is at most `2e-12`. Large-origin XYZ
tests allow `2e-7 m`, separately from actual physical model accuracy.
Native multi-layer projection, camera import, the actual installed worker's
automatic delivery and external-application round trips are separate acceptance
gates owned by the integration evidence; CPU tests alone do not mark them done.
No independently measured real-world scale/accuracy is invented for arbitrary
real photographs or for short synthetic Gaussian training.

The separately retained actual CloudCompare CLI read/write check covers both
current-turn generated GPU models in metres and millimetres (four cases, no ICP
or scale fitting). Numeric same-index XYZ round-trip maximum was
`2.026435025475876e-9 m`; CloudCompare returned float32 XYZ and retained the unit
comments in those files, but did not retain 53 original Gaussian attribute
names. This establishes coordinate interchange only, not complete-GS appearance
interchange, material-boundary accuracy, or support for every external plugin.
All input bundle hashes remained unchanged. The accepted process route used
CloudCompare's `-SILENT`, explicit Windows plugin paths and hidden process flags;
the locally unavailable offscreen platform was not treated as an accepted route.
This tool is validation-only, not a new GSW runtime dependency.

## 中文摘要

显存保护只控制增密前的资源准入及其“完整保存后暂停／暂缓增密继续”策略，
不再充当性能总开关。图像暂存、预取、FP32 容量复用、联合损失、渲染器、曲面
辅助计算和监测分别配置；预取确实依赖暂存，因此保留请求值并说明无效原因。
关闭保护不保证不会发生真实 CUDA 内存不足，也不取消手动暂停或完整状态保存。

旧总开关关闭时，迁移保留旧版实际关闭的优化路径，不能因为子选项仍为 true 就
擅自重新开启。完整续训先验证数据、相机、尺度、优化器、随机采样和源码身份，
再应用并记录明确的运行策略修改。规划实效、实际执行后端和历史观测必须分开。
真实 GPU、原生窗口生命周期和安装验收仍须绑定本轮版本逐项记录，不以预览正常
或短程训练替代几何精度验证。

## 日本語要約

VRAM 保護は増密前のリソース判定と「完全保存して一時停止／増密を延期して継続」
のみを制御し、性能機能の親スイッチではありません。画像ステージング、予取、
FP32 容量再利用、融合損失、描画、サーフェル補助計算、監視を個別に設定します。
予取はステージングに依存するため、要求値を保持して無効理由を表示します。
保護 OFF は実際の CUDA OOM を防ぐ保証ではなく、手動停止や完全保存は維持します。

旧総スイッチ OFF からの移行では、旧版で実際に無効だった機能を勝手に再有効化
しません。完全状態の継続はデータ・カメラ・尺度・最適化器・乱数・ソースの検証後に
明示的な方針変更を適用し、その前後と反復位置を保存します。設定上の実効値と実行
された処理、観測履歴を区別します。実 GPU、原生ウィンドウのライフサイクル、
インストール元の検証は本変更の証拠が必要で、プレビューや短時間学習を物理精度の
証明にはしません。
