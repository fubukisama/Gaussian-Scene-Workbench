# Windows native desktop runtime

简体中文：桌面程序、四套独立计算环境、编译工具、权重和缓存均放在 D 盘。安装验收必须运行真实计算及渲染检查；不能把模块导入、PLY 预览或热启动当成完整状态续训。

English: Keep the desktop application, four independent compute environments, compiler tools, weights and caches on D:. Acceptance requires real compute and rendering checks; importing a module or loading a PLY is not optimizer-state resume.

日本語：デスクトップアプリ、4 つの独立した計算環境、ビルドツール、モデル重み、キャッシュを D ドライブに保存します。実際の計算と描画を検証し、モジュールの読み込みや PLY のプレビューを完全な学習状態の再開と混同しないでください。

## Storage and process environment

The October 2026 installation uses `D:\Apps\GSW` for the application, runtimes,
validation artifacts and external sources; `D:\CodexData` holds downloads,
package caches and temporary files. The small `.lnk` file stays in the user's
Windows Desktop known folder and points to the D: executable. No Windows system
files, GPU driver or application-control policy are changed.

Do not add every Python/Qt/CUDA DLL directory to the global PATH: different
backends require different PyTorch ABIs. Persist the application locator
variables in the user environment; prepend only the selected environment's
directories in a development shell. Restart an already-running application
after changing runtime locator variables.

| Runtime | Python | PyTorch / CUDA wheel | Main source |
| --- | --- | --- | --- |
| Native build/checks | 3.12.14 | Not required | This desktop branch; Qt 6.8.3 |
| 3DGS | 3.8.20 | 2.0.1+cu118 | Packaged `gaussian-splatting` |
| 2DGS | 3.10.21 | 2.0.1+cu118 | hbb1/2d-gaussian-splatting `f3e3b9fa67bbd1c75e05167ff37391d8dab2a678` |
| SuGaR | 3.10.21 | 2.0.1+cu118 | Anttwo/SuGaR `7c10c4ae4a267dece512f5c7f40ed212a0a2ab44` |
| GS2Mesh | 3.10.21 | 2.3.1+cu118 | yanivw12/gs2mesh `560f3e8a2349aaad24fe9444ef1ca2770aeec5ab` |

Independent prefixes are `D:\Apps\GSW\runtime\envs\gsw_native`,
`gaussian_splatting`, `two_dgs`, `sugar`, and `gs2mesh`.
The matching `scripts/requirements-native-*.txt` explicitly pin CUDA-enabled
PyTorch/torchvision versions. Do not run two installers against one prefix.
Install a prefix's requirements first; build its own source extensions with
`--no-build-isolation --no-deps`. Never reuse a `.pyd` from another Python or
PyTorch environment, or from an older CUDA installation.
On Windows, `pip install --no-compile` avoids eagerly creating thousands of
optional Plotly bytecode cache files; Python still compiles used modules on
first import. This does not disable any operating-system security checks.

Use the PyPI NumPy wheels pinned in these files. The tested 3DGS environment
encountered a duplicate OpenMP runtime when mixing conda-forge NumPy and the
PyTorch wheel. Reinstalling NumPy 1.24.4 from its official PyPI wheel resolved
the conflict; do not mask it with `KMP_DUPLICATE_LIB_OK`.

Required locators:

```text
GAUSSIAN_SPLATTING_CONDA_PREFIX / GS_CONDA_PREFIX = .../envs/gaussian_splatting
TWO_DGS_DIR = D:/Apps/GSW/runtime/2dgs
TWO_DGS_PYTHON = .../envs/two_dgs/python.exe
SUGAR_DIR = D:/Apps/GSW/runtime/SuGaR
SUGAR_PYTHON / SUGAR_CONDA_PREFIX = the independent sugar prefix
GS2MESH_DIR = D:/Apps/GSW/runtime/gs2mesh
GS2MESH_PYTHON / GS2MESH_CONDA_PREFIX = the independent gs2mesh prefix
COLMAP_EXE / COLMAP_PATH = D:/Apps/COLMAP/4.2.1/bin/colmap.exe
FFMPEG_PATH = the installed imageio-ffmpeg executable
OPENMVS_BIN = D:/Apps/OpenMVS/2.4.0/vc17/x64/Release
CUDA_HOME / CUDA_PATH / TWO_DGS_CUDA_PATH = D:/Apps/CUDA/11.8
GSW_UNTITLED_ROOT = D:/Apps/GSW/workspaces/untitled
```

Set `TEMP`, `TMP`, `CONDA_ENVS_PATH`, `CONDA_PKGS_DIRS`, `PIP_CACHE_DIR`,
`TORCH_HOME`, `HF_HOME`, `XDG_CACHE_HOME`, and `MPLCONFIGDIR` to D: directories.
The installed `D:\Apps\GSW\runtime\gsw-environment.ps1` supplies these paths
for development shells. Micromamba and a portable conda installation use
`D:\Apps\Micromamba` and `D:\Apps\Conda` respectively.

## Compiler and CUDA development components

The verified compiler is MSVC 14.34.31933 (compiler 19.34.31948) plus Windows SDK
10.0.22621.0 under `D:\Apps\PortableMSVC-2022\msvc`. Its Microsoft payloads
were obtained with hash verification. The CUDA toolkit is 11.8.89, supplied by
NVIDIA's `nvidia/label/cuda-11.8.0` packages at `D:\Apps\CUDA\11.8`.

The development components must include `cuda-nvcc`, `cuda-cudart-dev`,
`cuda-cccl`, `libcublas-dev`, `libcusparse-dev`, `libcusolver-dev`,
`libcurand-dev`, `libcufft-dev`, and `cuda-nvtx`. A toolkit sufficient for the
Gaussian rasterizer may still lack headers required by PyTorch3D and SAM 2.
For example, a missing `cusparse.h` in `ATen/cuda/CUDAContext*.h` is a toolkit
development-component problem, not a CUDA driver or signature problem.

Import the compiler's setup script into the current process before building.
For extensions set `DISTUTILS_USE_SDK=1`, `TORCH_CUDA_ARCH_LIST=8.9` on this RTX
4080 Laptop GPU, and a bounded `MAX_JOBS` (tested: 2 or 4). Other devices need
their own compute capability. Keep setup/build output and wheel caches on D:.

`scripts/build_native.ps1` accepts `-MsvcEnvironmentScript`, or reads
`GSW_NATIVE_MSVC_SETUP`, in addition to its Visual Studio installation search.
The script must set PATH, INCLUDE and LIB; it may ignore the usual VsDevCmd
arguments. App-local CRT DLLs can come from the Qt runtime when a portable
compiler does not expose `VCToolsRedistDir`.

```powershell
./scripts/build_native.ps1 -Configuration Release -Package `
  -QtRoot D:/Apps/GSW/runtime/envs/gsw_native `
  -CMakeRoot D:/Apps/CMake/4.3.3 `
  -MsvcEnvironmentScript D:/Apps/PortableMSVC-2022/msvc/setup_x64.bat `
  -BuildDirectory D:/Apps/GSW/build/native
```

## Research mesh environments

SuGaR requires PyTorch3D 0.7.4, source commit
`297020a4b1d7492190cb4a909cafbd2c81a12cb5`, plus its own vendored Gaussian
rasterizer and simple-knn. On Windows with CUDA 11.8, the Windows RPC `small`
macro collides with a CUB storage member in Pulsar. Apply
`scripts/patches/pytorch3d-0.7.4-windows-cub.patch` to that pinned source before
building. This scopes the macro suppression around the CUB include; it does
not modify CUDA, Windows headers or numerical kernels. NVIDIA nvdiffrast is
optional; upstream SuGaR provides the PyTorch3D mesh-rasterization fallback.

GS2Mesh requires four separately built source packages from its pinned tree:
its Gaussian rasterizer, simple-knn, `segment-anything-2`, and GroundingDINO.
Build SAM 2 with `SAM2_BUILD_ALLOW_ERRORS=0` so a failed CUDA extension is not
silently accepted. Verify connected components and GroundingDINO CUDA
deformable-attention operations as well as loading their Python packages.
GroundingDINO's BERT backbone uses the D: Hugging Face cache.
The verified `bert-base-uncased` cache revision is
`86b5e0934494bd15c9632b12f734a8a67f723594`; its config, tokenizer, vocabulary
and `model.safetensors` are present. Offline checkpoint inference passed.

The native backend previously prepended SuGaR's source extension paths even
when GS2Mesh selected an independent prefix. A real mesh job reproduced the
wrong-module import. `gs2mesh_env()` now preserves that compatibility path only
for the legacy shared prefix; an independent prefix uses its own CUDA wheels.

The downloaded, upstream-provided checkpoints are not part of the Git/package
payload. Preserve their upstream licenses and attribution.

| Checkpoint | SHA-256 |
| --- | --- |
| DLNR_Middlebury.pth | `57e46b8fed66b685f732b730ebffd71998dcabea49f8443ca77e015419f6ae18` |
| DLNR_SceneFlow.pth | `76e5e66694919133db9cfc8f663cde540b4edf339db37040631befc6fb046e52` |
| sam2_hiera_large.pt | `7442e4e9b732a508f80e141e7c2913437a3610ee0c77381a66658c3a445df87b` |
| groundingdino_swint_ogc.pth | `3b3ca2563c77c69f651d7bd133e97139c186df06231157a64c507099c52bc799` |

Download provenance: [DLNR releases](https://github.com/David-Zhao-1997/High-frequency-Stereo-Matching-Network/releases/tag/v1.0.0),
[SAM 2 checkpoints](https://github.com/facebookresearch/segment-anything-2),
and [GroundingDINO releases](https://github.com/IDEA-Research/GroundingDINO/releases/tag/v0.1.0-alpha).

## Acceptance record: 2026-10-05

Validation artifacts are test-owned synthetic multi-view scenes, not user
projects or real-photo quality benchmarks. The installed binary is based on
desktop source `4454d98`; this change adds runtime pins, portable-toolchain
support and reliable acceptance-script behavior, not a new UI version.

| Capability | Verified result / remaining gate |
| --- | --- |
| Native desktop | Release packaging; 45/45 native tests; Simplified Chinese, English and Japanese installed-language checks |
| Shared backend | Native check environment: 154 tests, 146 passed / 8 skips; Python 3.10 CUDA prefix: 153 passed / 1 Windows symlink-permission skip; configured-environment isolation suite: 157 passed / 1 skip |
| 3DGS photos | Strict E2E rerun: 16 images registered; 12,596 sparse points; 1,000 training iterations; 35,058 Gaussians; installed render process waited for and passed |
| 3DGS video | Strict E2E rerun: 16 frames imported, reconstructed and trained; 34,839 Gaussians; installed render process waited for and passed |
| 2DGS training | 16 views; 1,000 iterations; 21,269 surfels; real installed surfel render benchmark passed |
| Original resolution | Both backends retained 1,928 x 1,084 input frames; uncapped COLMAP image undistortion passed |
| Full training-state resume | Both backends restore actual model, Adam and RNG tensors; 2DGS long-run numerical caveat below |
| Bounded TSDF | Real CUDA surfel rendering and TSDF; trained fixture: 6,690 vertices / 12,888 faces |
| Unbounded TSDF | Official 512 grid; 1,324,447 vertices / 2,666,688 faces |
| OpenMVS photo texturing | InterfaceCOLMAP and TextureMesh 2.4.0; 2,048 atlas; OBJ/MTL/PNG/ZIP and installed native material render passed |
| SuGaR | Own Gaussian/KNN and PyTorch3D wheels installed; GPU KNN and mesh-rasterizer backward passed; native surface-constrained training running; final mesh/refinement acceptance in progress |
| GS2Mesh | Own four CUDA wheels installed; connected components and deformable-attention forward/backward passed; both DLNR checkpoints, SAM 2 Large and GroundingDINO ran real GPU inference; native mesh job completed with 25,454 vertices / 45,093 triangles |

2DGS 1,200-iteration resume comparison initially exceeded the existing 0.05 dB
acceptance threshold (0.134813 dB). An unchanged rerun passed with 0.025623 dB,
while two independent continuous runs differed by 0.103058 dB. Exact serialized
state restore passed. The long-run CUDA numerical comparison remains flaky;
do not relax the assertion or describe this as bitwise-deterministic training.

OpenMVS rejected the 512 texture test because a 116 x 625 image patch cannot fit
that atlas limit. The default 2,048 configuration passed. Unbounded 2DGS uses
512-sized blocks and requires a resolution divisible by 512; a bounded-grid
smoke value such as 64 is not a supported unbounded resolution.

SuGaR/GS2Mesh remain separate generation pipelines: their native optimizer
checkpoint adapters are not implemented. Completed PLY meshes, stage artifacts
and 3DGS warm starts do not provide their optimizer-state resume. Standalone
OpenMVS dense reconstruction is likewise not a native generation entry;
OpenMVS's verified native role is photo-texturing an existing mesh.

The photo/video E2E script now runs `--smoke-test-gaussian-performance` with the
actual output PLY and waits for its own Windows GUI process exit code.
`--smoke-test` asserts an empty workspace and cannot verify model loading;
PowerShell's stale `$LASTEXITCODE` cannot prove a GUI process succeeded.
Geometry-specific reference-axis/picking tests are separate regression gates,
not substitutes for the general output-load/render benchmark.
The reconstructed photo fixture passed axis visibility and model transforms
but did not satisfy the reference-axis test's center-point precise-picking
assertion. Its full Gaussian renderer/residency/edit/undo benchmark passed;
do not claim that this particular picking fixture passed.
The GS2Mesh output was also loaded and visually inspected in the installed
native renderer. Its combined reference-axis/picking/gizmo fixture did not
pass (axis sampling region, center-point picking and one orthographic gizmo
ratio); this is not a mesh-generation or CUDA-environment success criterion,
and remains a separate interaction/fixture acceptance gate.

The first strict-photo rerun hit the backend's 20-second runtime probe while
the two research-prefix installers were writing bytecode caches. A direct cold
runtime import took 43.99 seconds and successfully accessed the GPU. After
dependency installation completed, an unchanged complete photo E2E rerun
passed. Do not relabel a cold-import timeout as an application-control block.

Worker unit tests use Python 3.10 or newer (their parenthesized multi-context
syntax is not a Python 3.8 context manager). The isolated upstream 3DGS runtime
remains Python 3.8 and passed its real photo/video training and checkpoint
workloads. COLMAP/OpenMVS locator tests explicitly isolate the relevant
environment variables, without removing the application's persisted locators
or weakening their original assertions.
