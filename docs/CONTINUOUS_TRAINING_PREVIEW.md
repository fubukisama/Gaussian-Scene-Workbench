# Continuous reconstruction and training preview

The native 3DGS workflow keeps one navigable viewport throughout processing:

1. **Prepare images/data**: the stage panel is visible immediately. Until actual
   geometry is available, retain the previous view and explicitly say that the
   first 3D result is pending. No simulated reconstruction is shown.
2. **Camera alignment / sparse point cloud**: display COLMAP's stable mapper
   snapshots. Camera frustums become available once the trainer writes its
   `cameras.json` (the reconstruction snapshots themselves contain points only).
   Starting from an existing binary COLMAP
   reconstruction also publishes a first sparse snapshot.
3. **Initial Gaussians**: publish an observation PLY from initialized tensors at
   iteration 0 (or the restored iteration when resuming).
4. **Optimization / density control**: prefer the existing CUDA/OpenGL shared
   memory path. Independently keep time-based PLY observations as a fallback.
   The default interval is 3 seconds, not an iteration checkpoint milestone.
5. **Result**: validate and associate the full final PLY. Keep the previous
   observation until it has loaded; retain the viewing direction, target and
   distance. Cancellation/failure retains the last usable observation and is
   labeled interrupted, never complete.

## Fidelity and responsiveness

Training snapshots contain XYZ, DC SH, opacity, log-scale and quaternion rotation.
They are limited to 500,000 evenly strided Gaussians, keep four completed files
per training session, and are explicitly observation data, not resumable optimizer
checkpoints or full export models. Full training tensors and checkpoint/export
attributes remain unchanged. Disk writes use a worker thread and atomic rename;
slow storage drops intermediate snapshots rather than building a queue. The UI
coalesces pending reads and replaces visible buffers only after successful parsing.
Shared GPU preview retains its existing capacity and frame-rate limits.

Sparse snapshot sequence numbers and Gaussian iteration numbers use separate
counters. Initial iteration 0 must not be rejected because sparse snapshot 40
was previously displayed. Late observation events cannot replace a newer frame
or a durable checkpoint at the same iteration. Snapshot loading runs even when
GPU preview is attached so disconnecting the producer has a usable fallback.
Live observations are read-only; viewport camera navigation remains available.

Preparation/COLMAP display is progress plus discrete real reconstruction snapshots,
not every internal optimization step. The periodic Gaussian tensor publisher is
integrated into this repository's **3DGS** trainer; the external **2DGS** trainer
continues to use its existing checkpoint events. A text-only COLMAP scene can train,
but the pre-training sparse publisher currently reads binary `points3D.bin`.

## Design references

- [Original 3DGS](https://repo-sam.inria.fr/fungraph/3d-gaussian-splatting/): sparse
  initialization followed by parameter optimization and adaptive density control;
  a dense mesh is not a prerequisite.
- [Postshot Getting Started](https://activation.jawset.com/docs/d/Postshot%2BUser%2BGuide/Getting%2BStarted):
  a navigable viewport showing emerging points, camera positions and radiance field.
  No official public Postshot source repository was identified; no proprietary code
  or assets were copied.
- [LichtFeld training manager, inspected revision a010028](https://github.com/MrNeRF/LichtFeld-Studio/blob/a010028d5c882dd9372ac1591468cf8706f623f0/src/visualizer/training/training_manager.cpp):
  separate training state, guarded live-model access, and rollback for failed scene
  initialization. Its source is GPLv3. This change references the architecture;
  it does not copy implementation or assets into the Qt/OpenGL application.

## Verification

`--smoke-test-processing-preview` tests sparse → initial → updated → final handoffs,
nonempty buffers during reads, coalescing, failed-file retention, camera continuity,
read-only previews, cancellation and live zh_CN/en_US/ja_JP labels.
`native.worker.test_training_preview` checks atomic output, PLY attributes, bounded
retention, write failures and backpressure; training telemetry tests check the
independent sequence domains and rejection of late observations.

`native/tests/validate_training_preview_cuda.py [packaged-backend-root]` runs in
the external CUDA training environment. It verifies the 512,001 → 256,001 bounded
observation path, all 14 Gaussian fields and four-file retention, including on
Python 3.7. The build stages the publisher explicitly and requires it in the
package's backend integrity check.
