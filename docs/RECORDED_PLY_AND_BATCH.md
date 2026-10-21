# Recorded PLY and persistent batches / 记录坐标 PLY 与持久队列 / 記録座標 PLY と永続キュー

This incremental native capability is general-purpose. It does not depend on an
object size, capture layout, image count, experiment label or hardcoded dataset.
Local acceptance, actual installed delivery and matching full cloud CI remain
separate records. A version number or a short GPU run is not all-tests success,
long-run convergence, or physical geometric accuracy.

中文：普通点云、索引网格及两种高斯共用记录坐标导出，保留相机、单位、属性及资源哈希。场景编辑与被动坐标换算分别记录，再次导出不重复应用编辑。关闭批量观察窗口不停止队列；暂停或取消后保留完整状态，续训必须明确发起。未知单位不自动变成米；拟合残差、外观代理和短程训练均不是实体精度。

日本語：通常点群・インデックス付きメッシュ・両 Gaussian 形式は記録座標の書き出しを共有し、カメラ・単位・属性・リソースのハッシュを保持します。シーン編集と受動的な座標変換を分離し、再書き出しで編集を二重適用しません。バッチ観察を閉じてもキューは継続し、一時停止・キャンセルは完全状態を保持します。再開には明示操作が必要です。不明な単位をメートルと推定せず、短い学習や外観代理を実物精度としません。

## Export interfaces and support

`native.backend.camera_scale_export.export_recorded_ply` accepts
`representation=auto|gaussian|point_cloud|mesh`. The existing
`export_camera_scale_ply` remains the strict Gaussian interface; the CLI default
is still `--representation gaussian` for compatibility. Both share the same
coordinate implementation. Partial Gaussian fields are errors, not permission
to silently downgrade the file to ordinary points.

```powershell
python -B -m native.backend.camera_scale_export --input input.ply --output restored.ply --coordinates metric --representation point_cloud --unit m
```

| Actual representation | Shared recorded-frame route | Remaining boundary |
| --- | --- | --- |
| Ordinary XYZ point PLY | Source/canonical/training/preserve; metric with known scale; normals and custom fields | A recorded valid sidecar is required; a raw file does not acquire calibration or metric evidence |
| Indexed polygon PLY mesh | Same route, integral indices, deletion remapping, polygon order, UV/material/custom fields and recognized assets | Unknown relational-index/material conventions fail explicitly; export preservation does not imply every multi-material convention is rendered by the viewport |
| 3DGS / 2DGS PLY | Same route plus log dimensions, proper orientation/covariance and SH0–4 | Edited PLY is not a full optimizer checkpoint; nonuniform scale or reflection is rejected at this shared edit seam |
| Other native file formats | Existing format-specific exports remain | No claim that SPZ/OBJ/GLB now implement this full recorded-camera/asset contract |

XYZ uses double precision. Relative `TextureFile` references are copied as real
bytes into a new owned asset directory, in original reference order. Absolute,
URI, traversal and link/reparse references are rejected. Source assets are
rechecked before publication. ASCII-safe output stems use `<stem>.assets`;
other stems use `model-<UTF8-stem-SHA256-prefix>.assets` because PLY headers are
ASCII. The model filename itself may be Unicode. Existing outputs/resources
are never overwritten, and cancellation rolls back only this writer's files.

Version3 sidecars bind the model, model-specific cameras and ordered assets.
Assets carry relative path, byte count, source reference and actual SHA256;
their canonical UTF8 JSON digest joins the model/camera bundle digest. Native
readers verify actual bytes as well as frame, camera K/pose/split and hashes.
Version1/2 readers remain compatible; merely accepting a version number is not
validation. Original models, sidecars, photographs and checkpoints are retained.
Actual depth-derived products also retain `source_geometry_provenance`: the first
extraction's hashes, coordinate transform, depth definition and appearance-proxy/
unmeasured-accuracy declarations. Later exports inherit it without replacing the
first source. Original camera-depth semantics are not reinterpreted as the edited
file's axes or length unit.

## Explicit scene edit, not a second unit conversion

`--scene-transform` points to `gsw.scene_transform/1` with a proper positive
uniform Sim3 in the **current file frame/unit**. The native checkbox is separate
from coordinate mode. Pivot/rotation/translation arithmetic uses doubles; a
legacy display `sceneUnitScale` is not applied a second time. Equal absolute
scale components with an even number of negative signs can form a proper
rotation; zero/nonuniform scales and odd-negative reflections are refused.

The sidecar retains immutable training calibration/scale evidence and an active
canonical edit `W`. For frame-to-canonical maps `C_before`, `C_after` and a new
current-file edit `D`, the model applies `C_after^-1 C_before D`, while bound
cameras are generated from immutable root views using the accumulated `W`.
Re-export without a new `D` changes only frame/unit, never applies `W` twice or
automatically inverts it to undo a user's edit.

`scene_scale_multiplier`, `physical_scale_modified`,
`scale_evidence_scope=immutable_training_contract`,
`scene_edit_independent_validation=false` and
`full_optimizer_state_transformed=false` distinguish the edited artifact from
the original measured/declared scale and unmodified optimizer state. Camera
consistency is not independent validation of the dimensions of an edited model.

## Persistent batch observation and control

`native.worker.training_batch_connection` is separate from the fingerprinted
batch plan/executor. `inspect`/`list` and native attach/reopen are read-only:
they do not import compute, load checkpoints, recover queue state or start work.
Explicit `run`/`resume` starts an exclusive detached coordinator with batch/plan
dual hashes, host, PID plus creation token, session, heartbeat and child identity.
The original strict data/source/full-state validation is not bypassed.

```powershell
python -B -m native.worker.training_batch_connection --operation inspect --plan D:/owned/workspace/.gsw/training-batches/BATCH_ID/plan.json
python -B -m native.worker.training_batch_connection --operation resume --plan D:/owned/workspace/.gsw/training-batches/BATCH_ID/plan.json
```

Closing an observer detaches. Pause affects only the authenticated active child,
waits for its complete checkpoint and exit, and keeps pending runs. Cancellation
also preserves that checkpoint and the remainder: `coordinatorOutcome=cancelled`
is distinct from the underlying `queueState=paused`. Explicit resume creates a
new session, validates full state and skips already verified completed runs.
Damaged, stale or wrong-session commands are rejected rather than crashing the
coordinator and terminating its child. Legacy/foreign-host/unknown OS ownership
remains conservative and read-only; observing is never a lock-recovery action.
Ordinary nonpersistent tasks keep their owned-process cleanup behavior.

## Verification and scientific limits

Acceptance uses real PLY/texture bytes, independent transform expectations,
tampering/cancellation/collision tests, actual Qt controls in three languages,
and generated-data default-adapter queues for both backends. Real bounded
clone/split/prune events and complete Adam/statistics/RNG/camera restoration are
separate from CPU lifecycle fixtures. Installed-module origins, corresponding
source, EXE, Setup, DLL/CRT closure and the protected Native shortcut are audited.
Remote real-window/full regression and local delivery are reported separately.

Locked cameras, initialization identities, held-out image isolation, resolution
policy and scale provenance remain mandatory. Rendered-depth points are an
unfused appearance proxy, not a claim that Gaussian centers or transparent/
reflective boundaries are physical surfaces. External mesh routes and their
unsupported conditions remain unchanged; no additional backend or broad
CUDA/Torch upgrade is implied. Research measurements and quality thresholds
must still be established independently.
