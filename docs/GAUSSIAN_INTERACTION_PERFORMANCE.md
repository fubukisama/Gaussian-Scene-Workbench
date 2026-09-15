# Gaussian interaction performance

## Change

Static imported Gaussians use GPU-resident attributes when their count fits the
OpenGL texture-buffer limit. All positions, colors, opacity, scales and rotations
remain float32. Camera rotation updates a uint32 instance-order buffer rather
than rebuilding and uploading the 60-byte vertex record for every Gaussian.
There is no new point-count cap, quantization, delayed sort or navigation LOD.

`GaussianDepthSorter` computes each depth once and uses a stable four-pass radix
sort of float32 depth keys. It preserves the previous back-to-front ordering and
source-index tie-breaker, including signed-zero equality. Non-finite depths have
deterministic ordering. Scratch arrays are reused per scene. Selection and
deletion rebuild attributes; camera/model orientation changes only update order.
Pan/zoom/hover reuse the order when direction has not changed. Camera-release and
snap completion no longer force a duplicate whole-scene rebuild.

The compatibility instanced-attribute renderer retains the same exact sorter for
devices/data exceeding texture-buffer limits. `GSW_DISABLE_INDEXED_GAUSSIANS=1`
forces this path for developer comparison. Training CUDA/shared-GPU-memory VAOs
continue to use their existing attribute layout, not the static-scene indirection.

## Reproduce and guard

Run the packaged or build-tree executable with:

```powershell
$env:GSW_GAUSSIAN_ORBIT_BUDGET_MS = '35'
$env:GSW_EXPECT_INDEXED_GAUSSIANS = '1'
& '.\Gaussian Scene Workbench.exe' --smoke-test-gaussian-performance --smoke-scene 'E:\model.ply'
```

Without `--smoke-scene`, the smoke test creates a deterministic 512,202-Gaussian
fixture in a temporary directory beside the executable and removes it afterward.
Keep the executable/build on a non-system drive. Smoke settings are isolated.
`GSW_GAUSSIAN_BENCHMARK_DIR` optionally saves QA frames to a chosen directory.

The benchmark replays 40 frames each of idle, orbit, alternating wheel zoom and
hover. Timing includes input dispatch, completed rendering and framebuffer
readback, not just CPU submission. It is not a claim about every user's sustained
FPS. The optional budget fails when orbit median exceeds idle median + the given
milliseconds; ordinary CTest uses structural checks without hardware-dependent
timing thresholds.

Regression checks cover full rendered count, no attribute uploads during
navigation, fresh order on rotation, reused order for unchanged direction,
resident/compatibility image agreement (maximum one 8-bit color level rounding),
point/Gaussian mode switching, selection, deletion, undo and scene cleanup.
The mixed-scene test also exercises picking and double-click recentering on
inactive Gaussian layers. Its point-picking VAO is refreshed on load/edits, not on
camera motion. Model rotation/nonuniform scaling must refresh order without
reuploading attributes.
`gaussian_depth_sorter` additionally compares every field of 512,202 sorted
records against the former comparison sort across four directions, tied depths,
reordered inputs, signed zero, infinities, NaN and empty/resized input.

## Measured on the development workstation

User model: 512,202 Gaussians; framebuffer: 1674 × 1032 physical pixels;
RelWithDebInfo; same camera/input replay. Original input file was not modified.

| Renderer | Idle median | Orbit median | Orbit p95 |
| --- | ---: | ---: | ---: |
| Original comparison sort + whole-record uploads | 5.08 ms | 98.76 ms | 101.39 ms |
| Exact radix sort + whole-record uploads | 5.12 ms | 58.91 ms | 63.72 ms |
| Exact radix order + resident attributes | 5.27 ms | 9.69 ms | 10.89 ms |

Per-rotation upload for this model changes from 30,732,120 bytes of attributes to
2,048,808 bytes of indices. The new initial attribute buffer is 32,780,928 bytes
(four RGBA32F texels per Gaussian). CPU filtering, gather and driver vertex-data
updates no longer run for each camera rotation. Timed CPU upload calls alone did
not account for the entire old frame stall, so the measurements above report the
complete rendering path rather than attributing all improvement to bus bandwidth.
The existing 60-byte point VAO is retained alongside the new buffer for picking;
total static vertex/index storage in this example is 65,561,856 bytes (about
35 MB more than the old path), excluding framebuffers and other scene resources.

Very large on-screen splats can still be GPU fill-rate limited. This change does
not alter the rasterizer, SH representation, splat size or source data.
