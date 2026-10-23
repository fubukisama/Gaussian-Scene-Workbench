# Independent quality rasterizer, ABI 1

Status (2026-10-23): independently compiled on the recorded Windows/CUDA
toolchain, with seven real synthetic GPU numerical checks passing. Explicit
experimental 3DGS quality plans can select this separate runtime after
source/binary/ABI/acceptance validation. Actual bounded AbsGS, Pixel-GS, Full Mip
and mask/depth training evidence is recorded in
`docs/quality_upgrade/ACCEPTANCE_REPORT.json`; it is not final appearance,
geometry or generalization acceptance. Standard/off remains unchanged and
MCMC desktop admission remains blocked by its separate unresolved GPU gate.
An import, ABI marker, build or CPU test alone is never GPU acceptance.

## Interface and invariants

Import name: `diff_gaussian_rasterization_quality`. It never replaces the stock
`diff_gaussian_rasterization` module. Retains the full-pinhole covariance and
pixel-border convention of the copied GSW source. Binary markers are
`GSW_QUALITY_RASTERIZER_ABI=1` and `GSW_PINHOLE_COVARIANCE_VERSION=1`.

`GaussianRasterizationSettings` adds three default-false flags:
`enable_absgrad`, `enable_contributor_pixels`, `enable_alpha`. The public
`GaussianRasterizer` returns `QualityRasterizationResult`:

| Output | Shape / definition | Available |
| --- | --- | --- |
| image | `[3,H,W]`, unchanged stock RGB | Always |
| radii | `[N]` int, unchanged stock screen radius | Always |
| inverse_depth_moment | `[1,H,W]`, sum `T_i * alpha_i / cameraZ_i` | Always |
| alpha | `[1,H,W]`, `1 - final_T` | `enable_alpha` |
| contributor_pixels | `[N]` int64 exact count | `enable_contributor_pixels` |
| absolute_screen_gradient | `[N,2]`, sum of absolute per-pixel NDC derivatives | `enable_absgrad`, populated after backward |

Disabled outputs have zero elements, with no additional large image/Gaussian
buffer. Contributor and absolute-gradient tensors are diagnostics, not
differentiable outputs. The absolute buffer is updated in place by backward;
consume it only after backward completes. Each backward replaces that buffer's
values, so aggregate per-view statistics outside the renderer. No gradients are
replaced or modified by taking their absolute value. The signed `means2D.grad`
continues to feed the stock 3D optimizer/preprocess.

Absolute x/y units are derivatives with respect to NDC x/y, matching stock's
`0.5*W` and `0.5*H` conversion inside backward; they are NOT pixel-space or
world-space gradients. Accumulate `fabs` per accepted pixel BEFORE pixel/tile
reduction. Do not take abs of an already reduced signed gradient. No AbsGS
threshold, pruning change or density strategy is bundled here.

Contributor counts increment only after `power<=0`, `alpha>=1/255` and
`T*(1-alpha)>=1e-4` have passed the stock renderer's acceptance rules. The
accepted contribution uses `alpha=min(.99, opacity*exp(power))`; a Gaussian
rejected by opacity or early transmittance does not count. Counts are not tile
hits, raster radius or projected bounding-box area. Per-view camera-Z for a
later Pixel-GS density adapter must use the canonical view transform; no fixed
scene unit or millimetre threshold is introduced here.

Counts use 64-bit integer atomics, not float32 increments that stop being exact
above 2^24 covered pixels. A later density adapter can explicitly convert counts
to the floating accumulator dtype when applying pixel weights.

`inverse_depth_moment` is not camera-Z, ray range or alpha-normalized expected
inverse depth. When alpha is valid, a caller may form
`inverse_depth_moment / alpha` using ordinary autograd; quotient gradients must
propagate through BOTH outputs. Define the minimum valid alpha explicitly in
the supervision provider; do not interpret zero coverage as zero-depth GT.

## Gradient domain and retained limitations

All-off and abs/count-only paths retain stock RGB/depth/signed-gradient
arithmetic. The optional alpha backward adds `final_T/(1-alpha_i)` times the
incoming coverage gradient to the existing alpha derivative. Its new term is
zero for saturated `opacity*G >= .99` because the clipped alpha is constant.

The inherited stock RGB/depth backward uses approximate straight-through-like
derivatives in saturated alpha regions. This slice does NOT silently fix that
behavior. Therefore normalized-depth derivative acceptance is restricted to the
smooth, nonsaturated, nonthreshold domain. Coverage/visibility cutoffs and
ordering changes are nondifferentiable. Do not claim globally exact depth
supervision gradients from the smooth-domain finite-difference test.

The independent Abs analytic case uses black background. A nonzero-background
strict stock comparison is included separately; it certifies compatibility,
not correctness of inherited stock background-gradient arithmetic.

## Build and verification

Use explicit Python 3.8/Torch 2.0.1+cu118, CUDA 11.8/MSVC and architecture 8.9
for this machine. No dependencies are installed by the build. The command is
dry-run unless `-Compile` is explicitly supplied:

Toolchain recheck (2026-10-23): the pinned `setup_x64.bat` selects VCTools
14.34.31933, actual `cl /Bv` 19.34.31948 and SDK 10.0.22621.0; INCLUDE starts
with that VCTools include directory. Its `yvals_core.h` identifies STL 143,
update 202208 and requires CUDA >=11.6 / MSVC >=19.34. CUDA 11.8.89's
`crt/host_config.h` accepts `_MSC_VER` 1910 through 1939. The actual pair fits
both guards. No `-allow-unsupported-compiler`, version-mismatch macro or forged
compiler-version definition is supplied; `/Zc:preprocessor` is retained as a
syntax/preprocessor setting, not a compatibility bypass. These identity checks
are not a successful extension compilation.

```powershell
./scripts/build_quality_rasterizer.ps1 -OutputRoot D:/Codex/builds/quality-rasterizer-v1-example
# Only after the root freezes/authorizes the GPU protocol:
./scripts/build_quality_rasterizer.ps1 -OutputRoot D:/Codex/builds/quality-rasterizer-v1-approved -Compile
```

Every output root must be new and on D:. Source is copied into that root;
build/temp/runtime artifacts stay there. Stock sources and installed packages
are never edited. GLM headers/licenses are copied from the unchanged stock
third-party directory and included in the source hash closure. Build success
writes `quality-rasterizer-build.json` with `gpu_verified=false`, source/runtime
hashes and Python/Torch/CUDA/architecture identity; it never writes acceptance.

CPU public reference/rejection tests:

```powershell
& D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe -m unittest training_kit.quality_rasterizer.test_reference training_kit.quality_rasterizer.test_runtime -v
& D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe scripts/verify_quality_rasterizer.py --cpu-only --output D:/Codex/outputs/new-quality-cpu-result.json
```

The opt-in real GPU seam requires both isolated roots and a NEW output file:

```powershell
& D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe scripts/verify_quality_rasterizer.py --module-root D:/Codex/builds/quality-rasterizer-v1-approved/runtime --stock-module-root D:/Apps/GSW/runtime/kernels/camera-pinhole3d-v1 --output D:/Codex/builds/quality-rasterizer-v1-approved/runtime/quality-rasterizer-acceptance.json
```

It compares flags-off/stat-only outputs and signed gradients against stock,
checks analytic cancellation/contributor statistics, finite-differences alpha
and moment/alpha through opacity, world mean and covariance, and reuses the
existing independent full-pinhole numerical oracle. It reads no real dataset,
does no training and opens no GUI. CUDA failure stays failure. CPU-only output
sets `gpu_executed=false`, `accepted=false` even when its CPU reference passes.

`native.worker.quality_rasterizer_runtime.select_quality_rasterizer` rejects a
missing/changed build, ABI, source/binary hashes, Python mismatch, missing real
GPU acceptance or incomplete required numerical checks. Configuration further
checks Torch/CUDA and the imported package/binary location and markers. These
are local integrity records, not signed publisher attestation. Parser tests
use explicitly synthetic evidence; they are not GPU acceptance.

## License and scope

The copied/derived kernel, Python wrapper and recipe retain all Inria GRAPHDECO
notices and the complete research/evaluation license in `src/LICENSE.md`.
[PROVENANCE.json](PROVENANCE.json) records baseline hashes and modifications.
This is NOT a GPL/MIT/Apache relicensing, and no commercial permission is
implied. Original GSW runtime/proof/reference orchestration does not override
the derived kernel's license. No AbsGS extra pruning, GOF densification,
MCMC relocation, Full Mip 3D filtering, 2D surfel adaptation or default promotion
is implemented inside this kernel. Their separate controller/supervision/export
implementations and current acceptance boundaries are documented in
`docs/QUALITY_PLAN.md` and the acceptance report. This kernel's numerical gate
does not promote those higher-level features.
