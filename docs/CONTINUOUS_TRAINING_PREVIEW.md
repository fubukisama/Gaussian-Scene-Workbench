# Continuous reconstruction and training preview

The native 3DGS and 2DGS workflows keep one navigable viewport throughout processing:

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
4. **Optimization / density control**: 3DGS prefers the existing CUDA/OpenGL shared
   memory path. Both adapters keep time-based PLY observations (also the 3DGS fallback).
   The default interval is 3 seconds, not an iteration checkpoint milestone.
   With a confirmed healthy shared-GPU consumer, CPU/PLY observations are
   reduced to one per 30 seconds. Resource pressure can temporarily skip
   observation work without changing the optimizer or training data.
5. **Result**: validate and associate the full final PLY. Keep the previous
   observation until it has loaded; retain the viewing direction, target and
   distance. Cancellation returns to the original scene when available; its
   last usable observation remains separately accessible. A safe pause publishes
   a separate validated task result, retaining all references; that PLY is an
   observation, not the full optimizer checkpoint used by Resume Training.

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
integrated into both native **3DGS** and **2DGS** training adapters; external-only
trainers retain their checkpoint-based fallback. A text-only COLMAP scene can train,
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
`--smoke-test-processing-completion` exercises the actual controller/owned-worker
handoff for 3DGS, 2DGS and all four mesh outputs, including a preceding COLMAP
job, explicit editing locks, select/delete/undo and live language changes.
Processing ends before source association; cached sparse previews cannot replace
model objects. Pending/failed final reads remain navigable but not editable.
Same-path adoption republishes readiness, and late snapshots cannot replace the
full source. See the trilingual handoff policy in [GENERATION_PIPELINES.md](GENERATION_PIPELINES.md).
`native.worker.test_training_preview` checks atomic output, PLY attributes, bounded
retention, write failures and backpressure; training telemetry tests check the
independent sequence domains and rejection of late observations.

`native/tests/validate_training_preview_cuda.py [packaged-backend-root]` runs in
the external CUDA training environment. It verifies the 512,001 → 256,001 bounded
observation path, all 14 Gaussian fields and four-file retention, including on
Python 3.7. The build stages the publisher explicitly and requires it in the
package's backend integrity check.

## Isolated tasks and training priority (2026-10-08)

中文：工程模型与任务观察层分离。“任务预览”切换原场景与任务画面，两层各自保留视角，原模型 ID、路径和变换不被快照覆盖。新的 3DGS/2DGS 成果使用任务独有的稳定对象 ID；同一任务暂停后续训只更新自己的成果。四种网格链路继续追加成果，不替换当前参考模型。已有模型不参与新训练，训练输入仍是数据集与相机重建。取消有原模型时回到原场景；没有原模型时保留只读观察。安全暂停会关联独立的任务成果，可在工程树中切回原模型；继续训练读取完整优化器检查点，不将这个 PLY 当作完整状态。

English: Project models and task observations are separate layers with separate camera views. Task Preview switches between them without overwriting reference IDs, source paths or transforms. New 3DGS/2DGS results have a stable per-task object ID; pause/resume updates only that result. All four mesh workflows continue appending their results. Imported reference models are not training inputs. Cancellation returns to available references, otherwise retaining a read-only observation. Safe pause associates a separate task result; references remain selectable in the project tree. Resume uses the full optimizer checkpoint, not this PLY as optimizer state.

日本語：工程のモデルとタスクの観察レイヤーを分離し、それぞれの視点を保持します。「タスクプレビュー」で切り替えても元の ID、パス、変換は上書きしません。3DGS/2DGS の成果はタスク専用の固定 ID を持ち、一時停止後の再開では同じ成果のみを更新します。四つのメッシュ工程は成果を追加します。参照モデル自体は学習入力ではありません。取消では元のモデルへ戻り、元のモデルがなければ読み取り専用の観察を保持します。安全な一時停止ではタスクの成果を別オブジェクトとして関連付け、工程ツリーから元のモデルへ切り替えられます。再開は完全な最適化器チェックポイントを使用し、この PLY を最適化器状態とは扱いません。

The observation policy probes free CUDA memory at most every 0.5 seconds.
GPU-intensive train, mesh and texture stages yield reference display buffers
before the first task frame. A CPU image retains the last reference geometry
while waiting, with live localized overlays drawn separately. Selecting the
original scene or finishing the task restores its display buffers; COLMAP and
preparation stages do not unnecessarily yield them. Explicit project changes
clear transient observations; ordinary same-project synchronization retains the
user's selected task view.
Point edits, spherical harmonics and object transforms remain CPU-owned. Large
resident mesh triangles and uploaded texture images can be discarded; restoring
those display buffers rereads their source PLY/texture paths, which must remain
available and unchanged. This is not a guarantee of offline residency after an
external source is moved or deleted.
Below `max(512 MiB, 5% of total)`, it skips tensor reads and packing. Recovery
requires `max(1 GiB, 10% of total)` continuously for ten seconds. This is
observation scheduling, not a hard optimizer quota or automatic density/quality
reduction. CUDA/OpenGL preview copy and cleanup failures do not abort training.
The native density policy and checkpoint identity are unchanged; full-state
resume remains available only for native 3DGS and 2DGS. TSDF, SuGaR refinement,
GS2Mesh depth and OpenMVS texture stages still have their documented cancellation
and result-preservation semantics, not a newly claimed optimizer resume.

`native_training_isolation` verifies reference retention, independent navigation,
actual GPU display allocation release/restoration, actual completed-frame rate
and live trilingual action behavior. Workspace tests verify stable result updates,
invalid-output atomicity and portable pause/resume identity. CPU adapter tests
verify that observation suppression and errors leave optimization running.
The pause controller writes the current session request atomically in the
validated project job store, with the process input channel as a secondary path.
It rejects ambiguous jobs, mismatched sessions, traversal and linked job paths.
This makes a successful UI pause request independent of stdin delivery; it still
waits for the optimizer boundary and does not force termination.
