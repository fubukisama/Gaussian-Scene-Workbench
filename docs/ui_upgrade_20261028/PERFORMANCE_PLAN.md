# P0/P1 performance and resource plan

Status: **PLAN / NOT_RUN**. This is a narrow engineering acceptance protocol,
not a performance result. Freeze this document's SHA-256 and the measurement
implementation/source identities **before** changing the AA implementation.
No local tests, window/application execution, GPU experiment or additional
artifact download was performed to prepare this plan. It does not modify
`P0_PROTOCOL` or replace its pixel-quality/oracle criteria.

## 1. Question and invariant

Does the P0/P1 UI rendering change improve the required pixel quality without
materially worsening host submission, interactive presentation, event response,
bounded navigation resources or on-demand idle behavior?

Hold model bytes/counts/SH, cameras, projection, transforms, selection, visibility,
background, physical viewport size, Qt/driver, swap configuration and runtime
settings constant within each A/B comparison. No reduced model fidelity,
decimation, full-viewport supersampling, continuous repaint or training change
is an admissible way to pass. All four supported display types receive semantic
coverage; medium-load stress is deliberately a narrower declared case.

## 2. Existing seams: reuse, but label correctly

| Existing code / observable | Reuse | Cannot establish by itself |
| --- | --- | --- |
| `GaussianPerformanceSmokeTest` | Public event injection, 512202-Gaussian seed42 generator, renderer/count/SH/upload/order preservation and residency checks | Its `frame p50/p95` times input + `grabFramebuffer()` completion/readback; its `input p95` is synchronous event-handler elapsed time. Neither is GPU render time or event-to-presentation latency. Its phase named `idle` forces 40 grabs and is **not** an on-demand idle test. |
| `ViewportSchedulingSmokeTest` | Real MainWindow/viewport, four owned point/mesh/3DGS/2DGS fixtures, fresh rendered serial plus `frameSwapped`, actual input/camera changes, timer/idle/minimize/restore assertions | Smoothed UI FPS is not a raw frame-time distribution; incidental top-level composition is not a fresh viewport image. |
| `FrameRateCounter` | Current `steady_clock` completion cadence, 60-interval moving window, `pause()` and resumed-burst anchoring | CPU drawing duration, GPU work duration, swap-to-photon time or independent statistical replicates. |
| `RenderFrameDiagnostics` | Separate opt-in visual run for actual FBO/DPR/GL/blend/sample identity and before/stage/after image provenance | A low-overhead performance observer: it copies FBOs, reads pixels, writes PNG/depth/stencil files and can use up to 32 stages/256 MiB of snapshot budget. |

`NativeViewport` invokes diagnostic presentation processing before updating its
frame counter. Therefore diagnostics can directly contaminate the counter's
timing. **Do not arm diagnostics, take screenshots or call `grabFramebuffer()`
inside any interactive performance/idle measurement window.** Visual evidence
and metadata capture use separate runs or an explicitly excluded phase.
Remove `GSW_RENDER_DIAGNOSTICS_DIR` and `GSW_RENDER_DIAGNOSTICS_SMOKE` from the
owned benchmark child's launch environment; do not share a capturing process.

The actual empty-scene path deliberately pauses performance metrics and can
omit `frameMetricsChanged` even after a new frame. Measure empty-scene raw
`renderedFrameSerial()` + fresh `frameSwapped` instead of requiring live FPS.

## 3. Metrics and evidence contract

Use one monotonic clock per process. Keep bounded raw samples in memory and
flush JSON/CSV only after each measured phase; no per-frame stdout, GL state
queries, process enumeration, image capture or disk I/O in the timed span.

| Metric | Definition and minimum evidence | Limit / honest name |
| --- | --- | --- |
| `overlay_submit_wall_ms` | Host wall-clock span around the actual navigation/overlay drawing call, including its normal cache lookup/build/composite; record frame serial, cache-hit state, sample count and raw durations. Cold and warm samples separated. | Includes driver/API waiting; **not** process CPU time and **not GPU milliseconds**. No current precise overlay-only span exists. |
| `fresh_present_interval_ms` | Consecutive callback timestamps only when rendered serial has advanced and reached `frameSwapped`; retain serials and exclude the first anchor after a pause. | Interactive presentation/composition cadence, not shader time or physical scan-out. Never divide a CPU render span to advertise FPS. |
| `event_handler_wall_ms` | Before/after the actual synchronous public event dispatch. Preserve the existing camera-change assertion. | Handler service cost only, not end-to-end response. |
| `event_to_fresh_swap_wall_ms` | Timestamp immediately before one event; correlate the expected public camera-state change with the first newly rendered serial reaching `frameSwapped`; record before/after state and serials. Send the next event only after that observation. | Injected Qt event to first fresh composition, **not physical input-to-photon latency**. No polling/forced grabs to create a frame. |
| Owned overlay resources | Account live CPU buffers/images, requested GPU allocation bytes/handles and retired/in-flight resources; capture cold, warm, post-toggle and destruction snapshots. | Requested/owned bytes are not actual driver VRAM residency. Vendor/per-process GPU residency remains `NOT_RUN` if no valid remote provider exists. |
| Idle work | Raw rendered-serial delta, fresh-swap delta, metric-emission delta and zero-interval timer activity after settling, observed while only the normal event loop runs. | Counter-only composition does not count as a new image; no `update()`/capture in the observation interval. |
| Optional process resources | Process CPU-time delta, peak working set/private bytes, GDI/USER counts at phase boundaries, if available on the remote runner. | No claim of GPU utilization, energy use or a leak solely from an allocator's retained RSS. |

For presented intervals, record requested and context-reported swap interval,
observed cadence, refresh information and remote-session identity separately.
Context configuration does not prove that the driver honored a VSync request.

## 4. Controlled fixtures and coverage

| Scenario | Source and role | Minimum sample plan per process |
| --- | --- | --- |
| S0 empty | Empty native scene with normal configured grid/navigation; no invented model/FPS | 60 one-at-a-time real navigation events, then the idle window; DPR1 core and a separately identified DPR2 worst-overlay-area process group |
| S1 light matrix | Existing scheduling generator: 441 points, 4-vertex/2-face mesh, 441 true 3DGS, 441 true two-scale 2DGS | 60 one-at-a-time events per kind plus idle; actual renderer/type/count assertions, perspective and orthographic semantic checks |
| S2 public medium | Existing benchmark's deterministic 512202-Gaussian seed42 generator, full source count and SH0 retained | 8 one-at-a-time events and idle; primary DPR1. Report median and max; an 8-sample p95 is just a low-count descriptive upper order statistic, not a reliable tail estimate. |

Generate each fixture once from pinned code/toolchain, hash the actual bytes,
and pass **the same files** to both arms. A seed alone does not guarantee
identical output across C++ standard-library/compiler changes. No user photos,
models or training are involved. Source SH, point/face count, active renderer,
upload count and source-order invariants remain checked, not just mode labels.

Do not add medium 2DGS/mesh/point-cloud stress silently to the claimed result:
those medium-load cases remain unmeasured in this narrow plan, while their
normal UI behavior is covered by S1 and the unchanged full regression suite.
The task-preview lifecycle remains its own existing remote contract; this plan
does not claim an IPC/GPU-training throughput benchmark.

DPR1.25/1.5/1.75 are covered by the separate real-window pixel/mapping protocol;
this performance slice does not multiply every fixture by every DPI/locale/theme.
Record the actual measured DPR and framebuffer dimensions, never only the
override variable. True multi-monitor and unavailable hardware-driver cases
remain separately `NOT_RUN`.

## 5. A/B execution and replication

- **A** is the frozen pre-AA drawing implementation. Initial production
  reference is `5183926`; record the precise eventual measurement commit/tree
  and any common passive-observer patch separately before execution.
- **B** is the candidate AA/UI implementation with the same observer/harness.
  Production, test and observer identities must all be recorded. If adding a
  CPU timing hook, apply the same observer to both owned source snapshots and
  do not pretend the resulting A executable is an untouched historic binary.
- Build both arms completely before measurement, in isolated runner-owned
  detached source/build directories without topic branches. Match compiler,
  Qt, flags, architecture, dependencies and release configuration. Do not
  compile, scan CodeQL or run another benchmark concurrently on that runner.
- Compare on the **same Windows runner allocation**, display/driver and
  fixed physical viewport size. Historical results from another VM are not a
  paired control. If P1 changes chrome density, adjust the top-level window so
  the actual viewport framebuffer is equal for the rendering comparison.
  Fixed-window shell/layout responsiveness is a separately labelled experiment.
- Use at least **three fresh-process paired rounds** per scenario. Predeclare
  order `A→B`, `B→A`, `A→B`, close each owned process normally, and reset only
  its owned settings/workspace. The odd number leaves an order imbalance;
  report it and make no formal causal/generalization claim. Never treat 24
  frame samples from one process as n=24 independent runs.
- Each round may contain the four S1 kinds in the same frozen sequence; those
  observations are nested within the process round. Record every result, not
  only the best run. Report the three per-round summaries and paired deltas,
  with overall ranges; no p-value, powered significance or hardware-wide claim.
- Warm S0/S1 with four explicitly driven frames and at most 3 seconds after
  loading; S2 uses two frames within 5 seconds because its public software-GL
  reference is approximately one second per readback frame. Record cache-cold
  first draw separately; do not exclude later
  misses/slow frames by looking at their values. If uploads/sorting have not
  settled, mark that phase unready instead of silently extending warm-up.
- Event sequences use bounded pan/orbit/wheel/hover inputs frozen with camera
  states. One event at a time avoids ambiguous event coalescing. Medium-load
  samples are back-pressured by the first fresh presentation, not flooded by
  a timer faster than the renderer can consume.
  Presentation/response samples use only inputs specified to change state
  (pan/orbit/wheel or an actual navigation-hover boundary). An unchanged hover
  is a handler/no-redraw observation, not a timeout waiting for a nonexistent
  required frame. Intended state-changing events that fail to change state are
  behavioral failures, not samples to discard or replace.

Maximum wall bounds proposed for the new focused measurement, excluding build:
30 seconds load plus 30 seconds observation for each S0/S1 process; 30 seconds
load plus 90 seconds observation for each S2 process; 12 minutes total for the
three-scenario A/B set. Stop and preserve the first structural/hard-cap failure.
A wall-budget expiration is `INCONCLUSIVE` unless a known contract failed; it
is not permission to extend an old CTest timeout or retry until green.

## 6. Predeclared engineering budgets (proposal to freeze)

These are tolerance limits, **not measured improvements**. Root should bind
their exact adopted values into the remote measurement manifest before the AA
change. Do not loosen them after viewing B. A baseline-only environment check
may reject an unsuitable/noisy runner before collecting candidate outcomes.

For each paired round, `Δ = B − A`; `A50/A95` are that round's raw baseline
summary, not the UI's smoothed FPS reciprocal. Every pair must satisfy the
applicable bound; median-of-rounds must also satisfy it. Preserve outliers.
Use the ordinary median (average of the two central values for even counts)
and nearest-rank empirical p95 (`ceil(0.95 × N)`) consistently in both arms.
Require at least 50 valid raw intervals for an S0/S1 p95; do not fill missing
intervals with zeros. Finite lower-count samples remain descriptive only.

| Measure | Proposed maximum non-regression delta |
| --- | --- |
| Warm overlay host submission p50 | `max(0.25 ms, 20% × A50)` |
| Warm overlay host submission p95 (S0/S1 only) | `max(0.50 ms, 20% × A95)` |
| First navigation cache-build host span (separate low-count cold result) | `max(10 ms, 25% × matched A cold span)`; retain all three process values, no p95 claim |
| Fresh presentation interval p50 | `max(2 ms, 10% × A50)` |
| Fresh presentation interval p95 (S0/S1 only) | `max(4 ms, 15% × A95)` |
| Event-to-fresh-swap p50 | `max(4 ms, 10% × A50)` |
| Event-to-fresh-swap p95 (S0/S1 only) | `max(8 ms, 15% × A95)` |
| S2 low-count max interval/response | `max(50 ms, 15% × baseline max)`; report all eight values |
| Navigation-owned GPU allocations | Hard cap 8 MiB per viewport, including current + retired/in-flight resources; no full-viewport allocation for this fix |
| Navigation-owned CPU images/buffers | Hard cap 8 MiB per viewport; all dimensions/bytes computed before allocation with overflow checks |
| Settled idle | After 600 ms quiet settling, two 1-second windows with **zero** new rendered serials, fresh swaps and metrics, and no active zero-interval rendering timer |

If the range of the three **A process summaries** exceeds the corresponding
fixed delta budget, report that metric `INCONCLUSIVE` due to unstable baseline;
do not inflate its budget by noise or call an underpowered result a pass.
Keep raw time distributions and process/resource context. Rendering semantic
and hard resource/idle failures remain failures even if a latency metric is
too noisy to judge.

For P1 lifecycle, perform 20 fixed cycles of theme/resize/cache-invalidating
changes on S0 at each declared fixed-DPR process group, then the S1 semantic
matrix once. Changing a process-start scale override does not demonstrate a
live DPR transition. Module-level controlled DPR/cache-key changes may be
tested separately; real cross-screen/DPR migration remains `NOT_RUN` without
an actual changing screen/window environment. Check owned live/retired
handles/bytes remain bounded throughout and return to zero after their owner
is destroyed with correct context lifetime. Driver RSS may retain memory; that
alone is not a leak. No screenshot/diagnostic polling during the two idle
windows. Scene switch/restore must finish its real queued upload, then settle;
do not remove the existing minimize/restore/deferred-upload assertions.

## 7. Smallest extra observer, only if needed

Existing public serial/frameSwapped/camera/count/upload signals suffice for
presentation, response, no-decimation and idle observations. They **do not**
provide overlay-only duration or owned overlay-byte accounting.

If those missing measures are required, add only an opt-in bounded snapshot
interface beside the real overlay drawing module: frame serial, host elapsed
nanoseconds, cache-hit/generation, CPU bytes, requested GPU bytes and live/
retired handle counts. Suggested mechanics: passive scope timer + a fixed-cap
sample ring (at most 512 records), counters updated only on allocation/release,
and a read-only snapshot consumed after the phase. Disabled by default; no new
paint/timer, synchronous GL query/readback, per-frame signal fan-out, disk log
or model/worker mutation. Apply the identical observer to both arms and record
its patch hash. If unavailable, mark those measures `NOT_RUN`, never infer
them from total FPS or a diagnostic snapshot's 256 MiB allowance.
Document the precise host-span envelope. If QPainter/driver work is deferred
to a later shared flush, that work is not attributed to the navigation span;
do not move a flush, call `glFinish`, change draw order or add a private painter
solely to force attribution. The measured span remains a host command-build/
submission observation with that coverage limit.

Real GPU timing would need a separate reviewed remote asynchronous GPU-query
measurement with validity/disjoint/driver handling. It is **not required** for
this narrow protocol and must not be fabricated from CPU or readback timings.

## 8. Output and interpretation

Retain `source-build.json`, `environment.json`, `fixture-hashes.json`, run order,
raw per-process event/frame/counter JSONL or CSV, resource/lifetime snapshots,
each round's summaries/deltas, and an independent acceptance summary. Images
and FBO diagnostic manifests are stored separately with their excluded capture
phases. Metadata must include actual logical/physical viewport size, DPR,
font/UI scale, OS/Qt/GL/vendor/renderer/driver, swap configuration, camera,
model source/type/count/SH and render-mode identity.

Use separate verdicts for pixel quality, interaction, CPU submission,
presentation/response, idle/resources and installation/full cloud. Missing
measurements remain `NOT_RUN`; unsuitable/noisy environments are
`INCONCLUSIVE`; software OpenGL results are not RTX 4080 measurements.

### Historical feasibility evidence (not this A/B result)

The already retained public run `40523593396` / source `87a797b` has a software
OpenGL medium fixture reporting 512202/512202 Gaussians, resident attributes,
SH0 and a 1097×693 framebuffer. Its forced-readback phase reported idle
p50/p95 `1013.09/1041.4 ms` and orbit `1057.62/1102.37 ms`; handler p95 was
`0.0013/0.0589 ms`. These values illustrate why 160 forced framebuffer grabs
must not be repeated as a new "short" benchmark or called GPU timing. Existing
four-kind scheduling logs separately record post-idle GL/images/metrics
`0/0/0`. The old source's assertions/deadlines remain intact.

Raw historical log: `D:/Apps/GSW/validation/remote-sync-oct28-87a797b/full-tests/Gaussian-Scene-Workbench/Gaussian-Scene-Workbench/native/build-ci/Testing/Temporary/LastTest.log`.

### Method provenance

The installed `experimental-design` skill informed pairing, process-level
replication, blocking/order disclosure and avoidance of pseudoreplication. No
DOE package was installed/run, and no statistical power/significance or GSW
speed result is supplied by that skill. Its current method reference, verified
by the coordinating agent on 2026-10-28, is Timothy Kassis, Vinayak Agarwal,
Yuhuan He, Darshil Patel and Aubrey M. Brueckner (2026), *Scientific Agent
Skills: A Library of Procedural Knowledge for Research Agents*,
[doi:10.48550/arXiv.2609.00065](https://doi.org/10.48550/arXiv.2609.00065).

Inspected source files (raw SHA-256 at plan drafting; not final candidate proof):

- `GaussianPerformanceSmokeTest.cpp`: `0d2d01a0d048dc4487c59f00177eb546b77a5658cfbc5b37d6a56f5904b829d0`.
- `ViewportSchedulingSmokeTest.cpp`: `ea15672cf67ff429167dbfc63ed005d7f9e0ea487077e8a7d70d8df7f62275c2`.
- `FrameRateCounter.cpp`: `c0c0d4ea8512be4aa5ecb132e79dec296b1f727040bc0b3b73b4d3d998119a51`.
- `RenderFrameDiagnostics.cpp`: `94dfa401333136c91e321444abbdf2ec7e56a9203e032af573660f1799392d01`.
