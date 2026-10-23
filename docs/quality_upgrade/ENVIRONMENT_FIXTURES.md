# P0 environment and fixture audit

Audit date: 2026-10-23 (Asia/Tokyo). Scope: read-only inventory for the independent quality upgrade; not training, renderer, or quality acceptance. No installed files, datasets, model state, security controls, or running processes were changed. No application was opened, imported, restarted, or brought forward.

## Audit status and plan

- Plan owner: root agent, `.planning/2026-10-23-quality-upgrade-20261023`; this worker writes only this ledger.
- Development checkout: `D:/CodexData/CodexHome/worktrees/windows-installer/git`, branch `codex/quality-upgrade-20261023`, base `161b5581d55ea0e501c903074ccbd381c1707927`.
- Input `AGENT.md`, fixture registry, and evidence snapshot were read as design/reference material. Historical measurements are not new acceptance evidence or authority to resume prior work.
- Root's initial experimental cap is 2 seeds x 64 steps, total GPU time <= 10 minutes and wall time <= 20 minutes, on newly generated owned fixtures only after a frozen protocol. Nothing in this audit executes that plan.
- Inventory complete within this read-only scope. Remaining gates are protocol/fixture binding and actual execution acceptance, not an instruction to run prior agents' tasks.

## Installed desktop identity

The live shortcut has moved beyond the input package's historical 2026-10-22 observation.

| Item | Read-only observation |
|---|---|
| Shortcut | `C:/Users/Ishida_Lab/Desktop/Gaussian Scene Workbench Native.lnk` |
| Shortcut last write UTC | `2026-10-23 03:07:58` |
| Target | `D:/Apps/GSW/native/0.3.7-remote-sync-oct23-c81d791/bin/Gaussian Scene Workbench.exe` |
| Working directory | `D:/Apps/GSW/native/0.3.7-remote-sync-oct23-c81d791` |
| Arguments | Empty |
| Build manifest version/channel | `0.3.7-native-preview` / `native-preview` |
| Build manifest release date | `2026-10-22` (manifest field, not inferred from directory name) |
| Executable SHA256 | `5f13ce108f4b005434be6552683648733a0fc0741d837448bade5e2927c66b57` |
| Build manifest SHA256 | `45269ab89e8bc1e4eb75e1fe6601dedb613ebac49639cf5ba648cf07969128d3` |
| Source manifest SHA256 | `258c292f384b057b733fda31b830cc0012b099abb53c53e4773bb8a058f7c019` |
| Backend manifest SHA256 | `24b9f47effd87993418d073e065ba6a08056f5c7d84b9d39a6157e8c2faeaa2d` |
| Corresponding-source manifest | `source-manifest.json`, schema `gsw-native-corresponding-source-v1`, license `GPL-3.0-or-later`, 1254 source-file records; its executable SHA256 agrees with the file |
| PE version resource | FileVersion/ProductVersion absent; identity above comes from manifest and file hash |

Only the executable hash field was cross-checked against the source manifest here; this is not a complete corresponding-source-file closure audit or launch verification. The installed package was not edited.

## GPU and running workload snapshot

Snapshot at `2026-10-23T12:49:37+09:00` using process inventory and `nvidia-smi`:

- GPU 0: NVIDIA GeForce RTX 4080 Laptop GPU; UUID `GPU-bb27a186-4e87-3833-7546-0c78b9c74d98`.
- Driver `617.42`; total VRAM 12282 MiB, used 1250 MiB, free 10746 MiB; GPU utilization 5%; compute mode Default.
- No Gaussian Scene Workbench, COLMAP, CUDA compiler/build, or identifiable training worker was present in the inspected process set. Existing Python processes were Blender MCP services; none was stopped or reused.
- WDDM process-memory reporting returned N/A, so this is not per-process VRAM accounting. Desktop/browser/Codex GPU clients exist. Free memory is a volatile sample, not a reserved execution budget; recheck immediately before any authorized GPU run.

## Candidate runtimes (metadata, not import/GPU acceptance)

Python identities below were obtained with `-I -S --version`; Torch/CUDA versions were read as text from `Lib/site-packages/torch/version.py`. No Torch, rasterizer, user module, or model was imported.

| Candidate | Python | Torch metadata | Python executable SHA256 |
|---|---|---|---|
| `D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe` | 3.8.20 | 2.0.1+cu118; CUDA 11.8 | `245168cea4dfca00d176b695d1e581c89df3e31b8c8005f7fa30d052fe911559` |
| `D:/Apps/GSW/runtime/envs/two_dgs/python.exe` | 3.10.21 | 2.0.1+cu118; CUDA 11.8 | `97493ee1fa62a377ff6077a1f8d3441eed7d3ea9dd91d63b09bda42c9763e835` |
| `D:/Apps/GSW/runtime/envs/sugar/python.exe` | 3.10.21 | 2.0.1+cu118; CUDA 11.8 | `97493ee1fa62a377ff6077a1f8d3441eed7d3ea9dd91d63b09bda42c9763e835` |
| `D:/Apps/GSW/runtime/envs/gs2mesh/python.exe` | 3.10.21 | 2.3.1+cu118; CUDA 11.8 | `97493ee1fa62a377ff6077a1f8d3441eed7d3ea9dd91d63b09bda42c9763e835` |
| `D:/Apps/GSW/runtime/envs/gsw_native/python.exe` | 3.12.14 | No Torch package observed in the selected metadata scan | `13d31fc5014b339e70c78add27d247d39542f84a2e8c0d7d9beb374bb34deb25` |

`D:/Apps/CUDA/11.8/bin/nvcc.exe --version` reports release 11.8, V11.8.89; SHA256 `3d25f98d0778a5c3dabdc170dd5dbc26508152f261c679ad73dead593a27f70a`.

Both 3DGS and 2DGS Torch `version.py` files have SHA256 `235d8190c848eb04086e71e4bdd080c805263ddaee1a7f6e5d6266f702ff44cb`. This identifies the inspected metadata only, not the full Torch binary closure.

The read-only `D:/Apps/GSW/runtime/gsw-environment.ps1` declares these 3DGS/2DGS environments, CUDA 11.8, `D:/Apps/PortableMSVC-2022/msvc/setup_x64.bat`, D-drive caches/temp, plus isolated experimental training-loss module paths. It was inspected, not sourced. Process and user variables agree with these 3DGS/2DGS/CUDA paths; inspected machine variables are unset. `GSW_CAMERA_RASTERIZER_ROOT` is unset, so the current known-camera selector derives its default kernel root from the selected Python prefix.

`GSW_TRAINING_LOSS_MODULE_PATH_3DGS` and `_2DGS` point to the existing `gsw_fastgs_experimental` and `gsw_loss_2dgs_experimental` site-packages. Their `pyvenv.cfg` files inherit the above main Python environments and enable system site-packages. These optional loss-module overlays must not be silently mistaken for a different Torch/runtime or newly certified kernel.

The unqualified `python` command resolves to the WindowsApps alias in this shell; `cl` and `nvcc` are not on this shell's PATH. Use the explicit candidates and task-owned process environment, not global PATH/runtime mutation.

### Existing compiler/native tools

| Tool | Identity | SHA256 |
|---|---|---|
| MSVC compiler | `D:/Apps/PortableMSVC-2022/msvc/VC/Tools/MSVC/14.34.31933/bin/Hostx64/x64/cl.exe`; PE FileVersion `19.34.31948.0`; setup script exists | `01586218610d4fabb0002026c2bba3ed1484f4bac38897b874c9c3acf8b53f77` |
| Windows SDK | Include directory `D:/Apps/PortableMSVC-2022/msvc/Windows Kits/10/Include/10.0.22621.0` exists | Not a full SDK closure check |
| CMake | `D:/Apps/CMake/4.3.3/bin/cmake.exe --version` reports 4.3.3 | `70fa92ce2ac9f54b0ae395b0b3790d9147ef2ebdbd7c4e0bb20852aac581baea` |
| Qt Core candidate | `D:/Apps/GSW/runtime/envs/gsw_native/Library/bin/Qt6Core.dll`; FileVersion `6.8.3.0` | `68a15353d192b735bd5411756a07b955f044bb1837e897016b986982e02ee943` |

The compiler setup was not sourced and no compile was attempted. Presence/version metadata is not proof that a new extension builds. D: had 749422981120 free bytes at the final inventory sample; this is not a space reservation.

### Existing rasterizer candidates and current-source integrity

| Runtime file | SHA256 |
|---|---|
| `envs/gaussian_splatting/Lib/site-packages/diff_gaussian_rasterization/_C.cp38-win_amd64.pyd` | `bd486f76b8bbf66d01d825e65847b1a5fe7e8e913c54ff9c3a9a2ab1bee0cada` |
| `envs/gaussian_splatting/Lib/site-packages/simple_knn/_C.cp38-win_amd64.pyd` | `ad12c5064db6948bca5e18f41a25707b0c46d647267a8e03466e4fe6655bcf95` |
| `envs/two_dgs/Lib/site-packages/diff_surfel_rasterization/_C.cp310-win_amd64.pyd` | `a04b2450ba70a8d30c0ca034b76b00d180aaf53cb27134889ffe49651096fff9` |
| `envs/two_dgs/Lib/site-packages/simple_knn/_C.cp310-win_amd64.pyd` | `87547f57a270ea8f0e62dfad4d5a6a3b337fa13286a8444d6fc6f554ddead562` |

All relative runtime paths above are under `D:/Apps/GSW/runtime`. The 3DGS installation's direct-url metadata points to `D:/Apps/GSW/build/extensions-3dgs/diff-gaussian-rasterization`; 2DGS points to `D:/Apps/GSW/runtime/2dgs/submodules/diff-surfel-rasterization`. These are provenance hints, not proof of current source/binary equivalence.

The separately selected known-camera 3DGS kernel has stronger local integrity evidence:

- Root `D:/Apps/GSW/runtime/kernels/camera-pinhole3d-v1`; proof schema `gsw.camera_rasterizer_runtime/1`.
- Proof SHA256 `b49622855e64cc9bee439b21f6c08d7c76b30bdff8bf05b670a6791e0e4e837b`.
- Declared Python 3.8, Torch 2.0.1+cu118, CUDA 11.8, CUDA architecture 8.9, pinhole covariance version 1, built `2026-10-19T07:14:09.8706518Z`.
- All 515 proof-listed source files match this audit's current development checkout using the declared `text-lf-v1` CRLF normalization. Both proof-listed runtime files match exact bytes; no missing or mismatch entries.
- Runtime `diff_gaussian_rasterization/__init__.py` SHA256 `69b576acb68ca1ede3541db273cc9b21e282f02b7608d4a3e724570a557b0d40`; `_C.cp38-win_amd64.pyd` SHA256 `a7fae95e9561fef3a448cde5d3c2ceb741270e5dc1dadd11e17deb51236caaa6`.
- The current selector additionally imports Torch/the kernel and checks ABI attributes during actual locked-camera execution. That stage was deliberately not invoked here. Hash agreement supplies local integrity, not publisher attestation, loadability, numerical correctness, or new quality-kernel support.

## Fixture inventory and binding limits

The input registry contains requirements, not bundled data: all T0-T4 `dataset_root`, identities, split manifests, ground-truth roles, thresholds, and supervision sources are unbound. No existing measurements were copied into candidate results.

| Group | Inventory result | Remaining acceptance gate |
|---|---|---|
| T0 known synthetic | Current repo has deterministic textured multi-depth planes (`native/tests/generate_colmap_training_fixture.py`) and direct known-camera analytic plane construction (`scripts/verify_general_reconstruction.py:plane_images`) | Create a new owned, protocol-bound scene including checkerboard/fine lines/L structure; hash image/camera/initializer/splits; old smoke fixtures alone do not cover all T0 mechanisms |
| T1 mixed scale, rectangular/multiple K | `scripts/verify_camera_scale_gpu.py:fixture` and the analytic plane generator already cover non-square images, varied K and disjoint train/val/test | Separate distance/focal/resolution perturbations; randomized rectangular cases; freeze m/cm/mm equivalence and exact initializer/sampler identities |
| T2 real non-cube | Existing 59-photo architectural data read-only bound as `local:T2-architecture-59`; human authorization for local stress testing confirmed by root | Verify real non-cube visual suitability, choose a new version and split/ROI/protocol; stress-test authorization is not a quality-acceptance result |
| T3 sparse interior | Optional registry C2 paths exist; manifest SHA256 `05e77dc1375e6d5df82ba0e5d464aee859e518bc1ef148cf7359e5e25b780d51`; image SHA256 `d7568d2805ebc9be57ad5cac613e88e4d7957bfde75ce34a68f83ecb42656ffa` | Read-only optional inventory only; no train/load/write. Previously inspected 130 mm image is validation, not pristine final test; C2 is not T2 |
| T4 thin/transparent/reflective | No eligible bound dataset established by the scoped scan | Generated thin-structure failure fixture plus licensed/authorized material cases and declared depth-validity/mask semantics; transparent rear depth must not become surface GT |

The architectural candidate is privately registered as `local:T2-architecture-59`. Its source metadata records 59 images, with 59 image files and 59 archived originals observed; one `sparse/0` COLMAP model exists, and archiving uses hardlinks. Exact user-photo paths are kept only in `D:/Codex/validation/gsw-quality-upgrade-20261023/p0/local_asset_bindings.json`, not this public document. Treat originals and links as immutable. Root confirms direct human authorization for local stress testing; real non-cube benchmark suitability and quality acceptance remain pending. No camera/model binary was parsed or loaded to claim reconstruction validity.

The optional C2 camera manifest reports schema `gsw.camera_dataset/1`, identity `65429e038e4d7a18893f5f31717be2e1afb4ece01418ffb936cb78d107665112`, locked poses, and 135 views split 99 train / 12 val / 24 test. Its canonical unit is m; its scale evidence explicitly records geometry accuracy as `not_measured`. The optional historical `evidence/QUICKTEST_VALIDATION.json` exists and hashes to the package snapshot's recorded `e9e895935cf93f4a8300886e75c8ed7e0ba695deac589f34a6a75f4a6ef7043d`. This confirms the reference's identity, not the historical measurement methodology or new quality acceptance. All other C2 data remain untouched.

## Existing runner cautions

- `native/tests/run_training_e2e_smoke.ps1` discovers Python/COLMAP, creates media/project/job/output roots, executes import and training, and can launch the native app. It is not a read-only environment check and was not run.
- `scripts/verify_camera_scale_gpu.py` is an explicit GPU acceptance runner. It has small known-camera fixture generation but its full execution launches jobs; not run.
- `scripts/verify_general_reconstruction.py` has a reusable analytic plane fixture and both 3DGS/2DGS paths, but its existing full runner requests 1000 iterations and geometry extraction, exceeding this task's initial 64-step baseline cap. Do not run it unchanged for the quality baseline.
- Existing generated validation datasets may aid code understanding, but use newly owned output roots and freshly frozen identities rather than mutating or silently reusing prior runs.

Inspected source identities at base 161b558:

| Source | SHA256 |
|---|---|
| `native/tests/generate_colmap_training_fixture.py` | `f26674e32b2c4991ace71ba2ca7a5c51fdf3954dd4c619dff582d6697db8a164` |
| `scripts/verify_camera_scale_gpu.py` | `12ba4e903dd41497154e51794d3d94fcbf1f394f8b7924b7a0969fa020e7dbdf` |
| `scripts/verify_general_reconstruction.py` | `0d103e798f76e0463a4ee236534f95b84d5d076e3d1543361f6365b245898e53` |

The COLMAP media generator accepts `--output-dir`, `--views` (default 16, minimum 8), `--width` (default 960, minimum 640), `--height` (default 720, minimum 480), and `--seed` (default 20260718). It generates translated pinhole views of six textured planes, photos/contact sheet and fixture metadata; it does not provide a complete normalized known-camera dataset, frozen split, depth-validity contract, or T0-T4 quality protocol by itself. The direct analytic generator supplies explicit train/val/test poses and multiple rectangular K, but also needs the additional quality-specific structures and evaluation contracts.

## Remaining prerequisites before execution

1. Freeze a task-owned T0/T1 registry and protocol, seeds, exact camera/image/initializer/split hashes, schedule, diagnostic outputs, metric thresholds, and stop rules before candidate results. Freeze a baseline runner that actually respects the root's 64-step budget; do not run the existing 1000-step script unchanged.
2. Re-sample workload/VRAM; select explicit Python and D-only temp/cache/output paths. Import/ABI/security-policy validation and real tiny GPU training/render/checkpoint-resume tests remain `not_run` for both 3DGS and 2DGS. Do not infer them from this inventory or prior proofs.
3. Any changed CUDA rasterizer source requires a separately identified compatible build and real numerical/GPU evidence. The existing known-camera proof covers the present source bytes only and cannot certify future AbsGS/Pixel-GS/Full Mip/MCMC kernels.
4. Verify T2 real non-cube suitability and freeze its quality protocol; local stress-test authorization is confirmed, but no real-world quality benchmark was accepted by this audit. Declare T3/T4 supervision/ground-truth roles and thin/transparent/reflective failure cases. Missing real-world evidence blocks promotion, not unrelated implementation work.
5. Native view/GUI/installed-candidate acceptance requires its own non-intrusive authorized execution path. No local visible application launch is implied by successful CLI or CPU tests.

## Audit errors and resolution

- A broad `rg` request included nonexistent `native/worker/tests`; no files were changed. Subsequent searches use actual repo paths.
- Two PowerShell display commands used a pipeline directly after `foreach`, causing parse errors before execution. Both were reissued using explicitly collected arrays; no runtime configuration was sourced or changed.

## What this does not certify

Executable/version/hash inventory does not certify module import, ABI compatibility, GPU execution, numerical correctness, optimizer-state resume, rendering, GUI behavior, quality improvement, or default promotion. Those require separately recorded real tests under the frozen protocol.
