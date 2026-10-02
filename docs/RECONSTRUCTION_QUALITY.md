# Reconstruction admission and 2DGS regression

## Shared workflow

COLMAP output components are not ordered by quality. The native compute backend now inspects every component before undistortion. A viable candidate needs at least 3 registered views, 100 finite sparse points supported by at least two registered images, at most 4 px reprojection error for those points, and at least 50% input-image registration. At least 95% of the sparse points must meet the point checks. Among viable components, registration coverage takes priority, then valid point count and reprojection error. Registration below 80% is reported as partial coverage, not hidden as success.

These are minimum viability thresholds, not guarantees of camera-baseline geometry, metric accuracy, complete surface coverage, held-out image quality or good appearance from arbitrary viewpoints. Intrinsics remain the responsibility of COLMAP and the training loader. Binary/text model consistency and finite poses are checked, but this gate is not a complete photogrammetric bundle-adjustment audit.

When the preferred coverage is unavailable, recovery makes at most two additional attempts: deterministic lower-angle initialization with the same calibration, then an optional SIMPLE_RADIAL single-camera reconstruction with a new database. Explicit camera models are not changed unless the user enables fallback. The reconstruction dialog has an immediately localized quality-recovery switch. Disabling retries does not disable admission checks.

Original input frames are retained in `input` before a registered/undistorted subset is published to `images`. Legacy datasets recover from their archived original photos/video; the recorded extraction rate or one unambiguous matching import configuration is required. Unknown rates, missing archives and frame-count mismatches stop recovery without publishing partial input. Existing training output is not removed until reconstruction admission succeeds. Optimizer-state resume never rebuilds or substitutes the original dataset; an invalid reconstruction blocks resume.

| Pipeline | COLMAP-based source gate | Remaining acceptance boundary |
| --- | --- | --- |
| 3DGS | Shared reconstruction recovery and admission before training | Representative held-out and geometric quality, not just training PSNR |
| 2DGS | Same gate; quick defaults 10,000 iterations / resolution 2; perspective-correct surfel snapshots and final viewport | Native center-sorted OpenGL compositing is not pixel-identical to the upstream CUDA tile renderer; held-out coverage remains untested |
| Bounded / unbounded TSDF | Re-check the available source dataset before mesh work | Source surface/depth quality; no voxel-state resume |
| SuGaR / GS2Mesh | Re-check the available source dataset before refinement/fusion | Independent real refinement/depth-fusion runtime acceptance |
| OpenMVS / COLMAP photo texture | Shared source admission before photo texturing; also inherited from native mesh jobs | Representative photo bake and material accuracy |

External, relocated or synthetic inputs without an available COLMAP dataset cannot receive a fabricated quality pass. Their existing backend-specific validation remains necessary. PLY warm starts and mesh previews are not full optimizer-state resume.

## 2DGS preset and native camera view

2DGS quick defaults are now 10,000 iterations and half-resolution (`-r 2`). Same-size CUDA/native comparisons show that quarter-resolution training still leaves fine needle artifacts at desktop resolution, so acceptance must not rely on thumbnail appearance. The upstream normal-consistency phase starts after iteration 7,000; the previous 7,000-iteration quick run never entered it. Explicit manual iteration/resolution settings remain unchanged. More Gaussians are not a quality target in themselves.

Protected training results retain `cameras.json`, `training_backend.json` and `reconstruction_quality.json` beside their PLY. Source-camera view respects the camera's image-up/roll, calibrated vertical field of view and focal-axis ratio, and applies the same display/model coordinate transform as the model. Returning from source-camera view restores the prior orbit/projection settings. Orbiting away remains possible. A different viewport aspect ratio changes framing; this is not a pixel-identical photo viewer.

Two-scale PLYs select a native perspective-correct surfel shader. The bounds and per-fragment ray/surface intersection are directly adapted from the official 2DGS rasterizer with its research-use license retained. This removes the affine thin-disk approximation; 3DGS retains its existing covariance shader. Live 2DGS snapshots keep the original two-scale contract rather than fabricating a third scale. GPU tests exercise off-center perspective inversion, orthographic inversion, projected support bounds, projection-pole guards, and tilted-surface occlusion against opaque geometry. Native blending still uses center depth order and does not reproduce CUDA tile early termination; outside captured camera coverage no new-view quality guarantee is made.

COLMAP inputs containing only `points3D.ply` lack point tracks and reprojection errors needed by this admission gate. They cannot receive a quality pass by counts alone. New training may reconstruct from the original images; full-state resume does not do so and is blocked until a complete original binary/text reconstruction is supplied.

Chinese, English and Japanese monitoring show registered/input view counts and valid sparse-point counts, plus repairing, rejected, accepted or partial status. Language changes retain worker and model state. Historical/raw backend logs remain untranslated.

Media-import and training preflight entrypoints explicitly add the selected installed backend root to Python's module search path before loading the staged server. This applies to both 3DGS and 2DGS training checks and to the shared media-import path used by all generation pipelines. It does not depend on the developer checkout, launch directory or inherited `PYTHONPATH`. Isolated subprocess regression tests cover both loaders, and packaging runs the installed media-preflight entrypoint with Python isolated mode so a repository import cannot hide a missing staged dependency.

## Reproduced user regression (2026-10-01)

The supplied C0001 video produced 9 extracted frames, but the selected old component contained only 2 registered images and 31 sparse points. Another component contained 5 images but only 2 points. The original published training-images folder contained only two frames, so a second reconstruction could silently lose coverage. The 7,000-iteration result contained 16,845 Gaussians but did not represent a reliable multi-view model.

In an independent source-preserving replay of the original archived video, the corrected selection/recovery registered all 9 frames, produced 2,330 usable undistorted sparse points at mean reprojection error about 0.43 px, and generated 81,630 Gaussians at 10,000 iterations. Official CUDA renders visibly recover the exhibit. Mean training-view PSNR was 39.10 dB over 9 training views, with **zero held-out views**. This is regression evidence, not a novel-view quality benchmark. A nine-frame short video does not establish complete 360-degree coverage.

The final packaged-backend replay on 2026-10-02 used the corrected quick preset (10,000 iterations, resolution 2) and the same archived source video. It registered 9/9 frames, admitted 2,509 valid sparse points with mean reprojection error 0.424 px, and produced 111,689 Gaussians. Official CUDA evaluation at 964 x 542 measured mean training-view PSNR 36.44 dB over 9 training views and **zero held-out views**. The earlier quarter-resolution PSNR is not directly comparable to this half-resolution run. Training took about 6 minutes 32 seconds on the local validation device; this is not a general performance promise.

The installed native renderer was exercised with all 111,689 Gaussians at 1674 x 1032, using the calibrated source-camera view. The exhibit is visibly coherent in this view; fine background artifacts remain. Median frame times were 4.67 ms at rest and 5.77 ms while orbiting on the validation device. The resident/indexed and compatibility paths differed by at most one color-channel level; editing, delete/undo and residency cleanup passed. This evidence is from the real native viewport, not only a CUDA thumbnail. Moving the camera outside the captured views can still expose unobserved or poorly constrained surfaces. The source-camera control provides a reproducible starting view without automatically replacing the user's orbit at processing completion.

Development checks use Python with NumPy, plyfile, Pillow and trimesh; on Python 3.13+ the legacy server also needs `legacy-cgi`. See `scripts/requirements-native-checks.txt`. These check dependencies do not replace the pinned CUDA training runtimes.

Primary references: [COLMAP camera-model guidance](https://colmap.github.io/faq.html#camera-models), [COLMAP reconstruction format](https://colmap.github.io/format.html), and the [official 2DGS implementation](https://github.com/hbb1/2d-gaussian-splatting). Official 2DGS remains under its upstream research-use license, included in the package; no upstream renderer code is relicensed as MIT.
