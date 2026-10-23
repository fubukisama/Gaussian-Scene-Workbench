# MCMC resume numerical diagnostic

Scope: investigate the preserved P5 strict CUDA tensor comparison failure without changing production source, the original `rtol=1e-5, atol=1e-6`, or its failed result. No original photographs, GUI, default promotion, or quality/geometry claims.

## Investigation status

- Read `p5/mcmc-v3/protocol.json`, `p5/mcmc-v4/{protocol,result,verify}.json/py`, and the real trainer/checkpoint/MCMC bridge code.
- The v4 child completed successfully, but the strict model tensor comparison failed at `model.1`. This remains a failed acceptance gate.
- Model restore precedes quality controller restore. `GSWMCMCBridge.restore_native_checkpoint` checks an exact native model/Adam/statistics binding hash and completed Adam counters, then rebinds the strategy to the restored model's actual Parameters/optimizer and restores its two independent RNG streams.
- A/B control authorized by root: at most two fresh continuous64 owned-T0/seed17 children, each at most30s, aggregate child wall at most60s and complete harness wall at most90s, no retries. All algorithm/settings/runtime/source identities are frozen before launch; only output paths/session identities may differ.
- Hypothesis under test: independent continuous runs already exceed the unchanged tensor threshold due to floating CUDA accumulation sensitivity. A positive control result would establish a reproducibility confound, not prove the resume implementation correct or convert the existing failed gate to pass.
- Shared plan remains root-owned. This report is the diagnostic worker's ledger under the planning-with-files skill.

## Measured controls

Frozen protocol: `D:/Codex/validation/gsw-quality-upgrade-20261023/p5/mcmc-continuous-jitter-v1/protocol.json`, SHA-256 `e9cb40a74d1477ed069cb5f33a73a08a423f634da8a89db08c26dadba3349ee6`. It pins115 Python sources,18 runtime/proof files and9 owned-T0 input files; every identity was verified unchanged before and after both children. Runtime identity includes the actual CUDA rasterizer binaries and their source/build proofs, rather than claiming the current loose CUDA source was necessarily rebuilt.

Exactly two fresh continuous64 CUDA children completed:3.641s and3.437s, aggregate7.078s; entire control harness8.610s. No retries, resume children, real photographs, GUI, or production-source changes occurred. The diagnostic ran under the same MCMC plan, seed17, actual renderer/loss engine, cap256, noise LR500000, from-iteration1, interval16, and explicit capacity-storage-off contract as the preserved comparison.

The unchanged `rtol=1e-5, atol=1e-6` comparison also **fails for continuous versus continuous**:

| Measured value | Preserved continuous vs resumed | Fresh continuous A vs B |
|---|---:|---:|
| First loss difference, iteration2 |5.960464477539063e-8 |5.960464477539063e-8 |
| Final loss difference, iteration64 |2.950429916381836e-6 |3.516674041748047e-6 |
| XYZ maximum absolute difference |0.004190623760223389 |0.015291169285774231 |
| Log-scale maximum absolute difference |0.007494449615478516 |0.009127140045166016 |
| Quaternion maximum absolute difference |0.0081978440284729 |0.008558034896850586 |
| Opacity-logit maximum absolute difference |0.021887779235839844 |0.024319171905517578 |
| Largest Adam first-moment difference |5.03224873682484e-6 |3.762252163141966e-6 |

In both comparisons the complete64 train-camera trace, independent sampler/strategy RNG state, Gaussian IDs, next ID, step/event counters, strategy configuration, and last position LR are exact. Growth parent/new-ID events at16/32/48 are exact. Complete `last_event` is **not** exact because it contains a parameter-dependent floating noise norm; RNG equality must not be misreported as identical applied noise tensors or identical floating event summaries.

Specifically, final-step63 `noise.maximum_norm` is0.4666508436203003 vs0.4667450487613678 in the preserved resume comparison, and0.4666192829608917 vs0.4660959839820862 in fresh A/B. All other fields of those final events are equal, including external-Adam marker, preflight summary, event indices131/132, and position LR0.0002476526349165484.

Machine-readable evidence:

- `.../mcmc-continuous-jitter-v1/existing-v4-cpu-analysis.json`: CPU-only reanalysis of the original preserved checkpoints/logs.
- `.../mcmc-continuous-jitter-v1/result.json`: fresh A/B checkpoint deltas, loss history, camera/RNG/topology comparisons and exact child timing.
- `.../p5/mcmc_jitter_diagnostic.py`: standalone frozen diagnostic, SHA pinned in protocol. It does not import or execute an old experiment harness and refuses existing outputs/automatic retries.

## Interpretation and remaining gate

Observed: a tensor drift of this scale occurs without any resume boundary, while the original runs already differ at iteration2, before the pause at16. Therefore the existing independent-run final-state comparison cannot by itself distinguish a restore bug from ordinary CUDA execution variation. This is not a statistical equivalence bound: there is only one A/B pair, and no new tolerance or acceptance threshold has been inferred.

Source-level explanation, not isolated causal proof: the rasterizer backward uses floating `atomicAdd` for per-Gaussian gradient accumulation across contributing work (vendored `cuda_rasterizer/backward.cu:646-655`). MCMC covariance/noise and Adam then evolve from those parameters. Identical RNG draws can therefore yield nonidentical applied stochastic displacement once their covariance/opacities differ. The experiment did not replace the kernel, enforce a deterministic reduction, or attribute every observed bit difference to a specific CUDA atomic.

The actual pinned camera-rasterizer build copy at `D:/Codex/builds/gsw-pinhole-quality-20261023-v1/source/cuda_rasterizer/backward.cu` was also checked: its normalized-LF SHA-256 `90d872c04efef86469b11ffcb5fa8e4cfdfa556b74a6cbe486ea2f2c476fbd53` matches the build proof's source identity, and it contains those same floating atomic additions at646-655. The frozen Torch runtime reports2.0.1+cu118, CUDA11.8, deterministic algorithms off and cuDNN benchmark/deterministic flags off; those flags were not changed for the control.

No concrete native binding/rebinding/RNG restore defect was found in the read-only code trace. Successful v4 continuation implies its actual exact binding and optimizer-counter checks accepted the restored model/Adam/statistics before the controller resumed; it is not a proof that every possible checkpoint component has been independently exercised.

**The original strict CUDA resume gate remains failed/unresolved.** Next work must be separately proposed and frozen: either a single-tile numerical microfixture with the original tolerance, or an exact real-CUDA restore-boundary state audit joined to the already separate CPU trajectory evidence. Neither ran under the original A/B authorization; the subsequently authorized microfixture is recorded separately below. Full multi-tile strict end-state equivalence, quality improvement and default promotion remain unaccepted.

Root subsequently authorized a separate1x1-pixel/four-point canonical-locked microfixture, maximum3children each30s, to test continuous64 / pause near16 / full resume-to64 with the original tensor tolerance. CPU data/contract validation must precede its separate frozen protocol and GPU execution. It is not a replacement result for the original multi-tile gate.

CPU construction passed through the public `prepare_training_dataset` and `load_prepared_training_dataset` APIs at `p5/mcmc-single-pixel-v1`: one RGB pixel `[96,128,160]`, one train-only locked view, explicit identity source/canonical/training transforms, four literal points and four literal colors, all four inside and past the existing near clip. Prepared reload retains exact points/colors/K/1x1 size, and input bytes are unchanged. `cpu-validation.json` records all six source/prepared file hashes; `protocol-draft.json` preserved the explicitly **not frozen for GPU** preparation stage. At that stage GPU execution waited for root's replacement runtime/source-closure freeze; the subsequent frozen trial is below. No selector, minimum image-size rule or kernel was modified by this diagnostic.

### Separate microfixture execution: failed, stopped

After root supplied the frozen stock-v2 and quality-build3 runtimes, protocol `p5/mcmc-single-pixel-v1/protocol.json` was frozen with SHA-256 `d7b3fcbc36036c63577cae117394a2002046cdb3b8b2716fd3d58403ed43ee80`:121 source entries,12 runtime files and the six CPU-validated inputs. The first continuous child failed after17.703s, exit `3221226505` (`0xC0000409`), without timeout. Per the no-retry/stop-first-failure rule, **pause and resume children were never started**. `result.json` remains `passed:false`; there is no real restore-boundary observation in this failed trial.

The primary logged error is `RuntimeError: CUDA error: an illegal memory access was encountered`, surfaced at `gaussian_renderer/__init__.py:119` while clamping a rendered image. CUDA reports it may be asynchronous, so this stack does not identify the faulting kernel. Before the failure:

- Metrics, rendering, loss/backward and updates completed through iteration32.
- Actual camera audit reached iteration33, still the single locked train view.
- MCMC grew4→8 rows at16 and8→16 at32, both admitted by the unchanged resource guard. Last growth parents were `[0,0,7,6,7,0,3,0]`; no relocation was requested.
- The iteration32 stochastic displacement summary had `maximum_norm=3.739315986633301`, a possible coverage/culling-edge trigger, not a demonstrated cause.
- The actual loaded stock proof SHA was `3d377e0fc298f9c19184cec3323d320fe3ca220a00d95b67426adadb81c02604`, rasterizer binary SHA `6149315165c2cc99ceebc96bf49f7af319e6b8a504099c72424d12ab9b80d10c`. Actual camera size remained1x1, calibration `requires_grad:false`, renderer/loss reported stock, capacity storage explicitly disabled.
- The passive atexit record contains38 actually imported project source files, all matching the frozen closure.

Therefore this is not evidence that initial four-point `simple_knn`, one-pixel decoding/canonical preparation, imports/DLL loading, or the initial SSIM computation were rejected: all had progressed through32 real CUDA steps. It also does not prove these components are free of a latent memory bug. Read-only source tracing found guarded pixel/splat accesses and a possible zero-contributor/bucket edge worth instrumenting, but no unique faulting access was established. No minimum dimension, kernel, MCMC math or input was altered, and no additional GPU trial was authorized or run after the failure.

Postmortem runtime-closure limitation: the `stock` loss label does **not** identify its SSIM primitive. `train.py` uses the installed `fused_ssim` when importable, and the configured3.8 environment successfully imports that wrapper/CUDA binary on CPU without initializing CUDA. Those two installed SSIM files were not individually pinned among the12 preflight runtime entries. This is a diagnostic-closure omission, not a software failure cause or permission to rewrite the frozen protocol retroactively. A future protocol must pin the actual SSIM primitive/binary and passively record which callable executed; this trial cannot attribute its memory error uniquely to the rasterizer. The separately vendored SSIM source uses guarded pixel loads/stores, but an installed binary's provenance or tiny-input safety cannot be inferred merely from that source or from the `stock` label.

Postmortem-only SSIM file measurements (not retroactive preflight bindings): wrapper SHA-256 `528854ba7e40443cd17c89acb7e67bee6115709b1d8c2edafe067adf6660f437`; `fused_ssim_cuda.cp38-win_amd64.pyd` SHA-256 `86c36a9e20a67e41e1d2b3ee70165689e92ec653d39431d37197afbcf5bb24ec`. Wrapper defaults are `padding='same', train=True`.

An independently frozen future diagnostic could add synchronous CUDA error localization and passive pre-render state/coverage snapshots at32/33, or use separately declared preinitialized tiny scales. Such changes need a new trial/protocol; they cannot replace the preserved failed1x1 result or the original multi-tile trajectory failure.

### Prepared-only fault localization follow-up

Root authorized preparing, but **not yet executing**, one independent continuous64 fault-localization child with `CUDA_LAUNCH_BLOCKING=1`, maximum30s and no retry. It reuses the exact six1x1/four-point input hashes and unchanged MCMC mathematics, not a larger image or a changed threshold/loss/renderer.

Final prepared protocol: `p5/mcmc-single-pixel-fault-v3/protocol.json`, SHA-256 `e5fa5adcdc153afb25d4646983bbe05e35f15676d7dfc8e52494425d0e4ee895`. The CPU inventory imported the real trainer and observed modules with `CUDA_VISIBLE_DEVICES=-1`, exercised only CPU Adam/contract-loading dependencies, and confirmed `cuda_initialized:false`. It pins1738 actually imported module files plus a conservative5411-file source/binary closure for lazy package imports, including both installed SSIM files and the actual `FUSED_SSIM_AVAILABLE=True` selector. The child will record its complete actual import map and flag anything not prebound.

The private observer uses a selective Python call/exception/return trace for the original public render, `PhotometricLoss.compute`, installed `fused_ssim.fused_ssim`, Tensor.backward and MCMC post-optimizer methods. No method is replaced. Only a normally returned method is followed by `torch.cuda.synchronize`; exceptional unwinding preserves the original exception. Function-name/code-object filtering limits observation overhead. Stage events will identify the first observed failing layer, not automatically identify a unique CUDA instruction or certify resume.

The initial prepared-only fault-v1 observer was archived before execution as `fault-v1/observer-original.py`; v2 improves trace filtering overhead. Final API review caught that the photometric public method is `compute`, not `__call__`; v2 was likewise archived without GPU execution, and v3 corrects that binding and explicitly observes the installed SSIM sublayer. V1/v2 remain unexecuted. Root subsequently handed GPU ownership back and explicitly authorized the single v3 trial below.

### Fault-v3 executed once: first observable failure in render33

The one authorized child ran with `CUDA_LAUNCH_BLOCKING=1` and the original methods unchanged. All five observed phases at iteration32 returned normally and completed `torch.cuda.synchronize`:

`render → installed fused_ssim → PhotometricLoss.compute → Tensor.backward → MCMC_after_optimizer`

At iteration33, `render` entered and the **original render method** raised CUDA illegal memory access. The first error was not a post-return observer synchronization failure. This isolates the first observable failing layer to the next forward/render path, rather than unfinished asynchronous SSIM/backward/MCMC work from32. It does not identify a unique native instruction or exclude a latent earlier memory corruption that a synchronization barrier alone cannot detect.

The captured actual import map has1739 files and **zero missing/changed entries** against the preflight5411-file closure. The real installed SSIM primitive was observed executing through32 with successful return synchronization. There was no new pause/resume child, no observer-driven method replacement, and no GPU retry.

The parent diagnostic's JSON parser then failed because the trainer's existing `safe_state` appended human timestamps to all322 observer lines written with `print`. Raw stdout/stderr and the complete actual import map had already been preserved. `p5/analyze_fault_v3_logs.py` performed CPU-only recovery with `JSONDecoder.raw_decode`, retaining the timestamp suffix evidence and writing `fault-v3/analysis-result.json`. This repair did not execute another child or rewrite the frozen observer/protocol.

Child exit-code/timeout metadata had not yet been serialized when the parent parser failed, so those fields are explicitly unknown, not invented. The child used `subprocess.run(timeout=30)`; filesystem timestamps imply approximately30.149s including launch/termination/log-write overhead. GPU work had already reported its render33 error and atexit import audit earlier. This approximate wall measurement is not presented as a precise successful≤30s child completion. Future strict wall budgets should reserve termination overhead within the declared wall ceiling.

The original1x1 continuous/resume experiment remains failed before resume; the original multi-tile strict tensor gate also remains failed. The new result is **fault localization only**, not resume acceptance, quality acceptance or a kernel fix. GPU and source ownership were returned to root immediately after the one child ended.

## Subsequent explicitly authorized CPU fixes

Separate from the read-only MCMC diagnosis, root authorized two small lifecycle fixes while replacement kernels built:

- `QualityRandomStreams.image_sampler_state()` supplies an immutable snapshot to the existing read-only camera lookahead. Runtime image staging accepts that explicit state, and both3DGS/2DGS trainers pass it only for quality opt-in; the legacy path retains its original three-argument image call/global sampler.
- `reject_legacy_quality_checkpoint` rejects any explicit quality plan paired with a legacy tuple before model construction/restoration, using `quality_legacy_checkpoint_unsupported`. Absence of a quality plan retains the old path. Both actual trainer entry functions were exercised in CPU-only subprocesses and rejected before CUDA initialization.

Red→green evidence is in `native/worker/test_quality_sampler_resume_guards.py`. Combined sampler/lookahead/lifecycle/runtime regressions pass37/37 under both the configured3.8 and3.10 Torch runtimes; QA3.12 reports10 tests with3 explicit Torch-dependent skips. These fixes do not change the MCMC algorithm/admission policy and do not make any failed CUDA gate pass.

## Investigation errors

- An initial read requested nonexistent `quality_mcmc_runtime.py`; the actual adapter was traced to `quality_mcmc_model.py`. This read failure changed no data or process.
- A PowerShell evidence-only status check initially piped a bare `foreach` statement, producing a parser error; wrapping its results in `@(...)` corrected it. It executed no training or file mutation.
- Fault-v3 parent log parsing initially assumed an entire observer line was JSON; the trainer's timestamp suffix caused `JSONDecodeError: Extra data`. A separately saved CPU-only parser recovered the first JSON object from each original line. Raw logs, failed parent execution, and unknown child-status fields remain explicit.
