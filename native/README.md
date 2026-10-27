# Gaussian Scene Workbench Native

The `main` branch is the single maintained native desktop source branch. `native/` contains the Qt 6/C++ desktop and its supervised Python compute backend. The retired web/Electron implementation has been removed from this branch. The native target does not load a browser engine, start a local HTTP server, or require Node.js at runtime. Shared workflow code lives in `native/backend/compute.py`; see [migration and compatibility](../docs/NATIVE_MIGRATION.md).

Documentation updated: `2026-10-23`. Product version is `0.3.8-native-preview`; the build manifest records the release date. See [result-path and persistent-control reliability](../docs/CI_RELIABILITY.md) for equivalent Windows names and bounded metadata reads during atomic publication. See [recorded PLY and persistent batches](../docs/RECORDED_PLY_AND_BATCH.md) for shared restoration, scene-edit provenance and persistent queue controls; [runtime decoupling](../docs/RUNTIME_DECOUPLING.md) covers independent resource/execution controls, old-policy migration and existing-job observation. [General reconstruction](../docs/GENERAL_RECONSTRUCTION.md) covers initialization, rendered-depth point clouds, independent geometry evaluation and named comparison recipes; [camera/scale P1](../docs/CAMERA_SCALE_P1.md) covers constraints, independent checks and trajectory association. Local acceptance, installed delivery and complete cloud CI are separately recorded; this version label is not a claim that every gate has passed.

Experimental quality plans / 实验性质量计划 / 実験的品質プラン: [three-language guide](../docs/QUALITY_PLAN.md). Default off; final quality/geometry and MCMC desktop admission remain gated. 默认关闭，不把短测当画质提升。初期状態では無効で、最終品質の受入とは区別します。

## Current preview

- [Managed native updates](../docs/NATIVE_UPDATES.md): desktop, Start menu and taskbar use a stable native entry point. Verified releases switch atomically for the next launch, retaining running and previous versions; this is not live patching or an automatic downloader.

中文：[原生版本更新](../docs/NATIVE_UPDATES.md)让桌面、开始菜单和任务栏统一使用固定入口。验证通过后下次启动自动使用新版，保留运行中的版本和旧版；不属于热更新或后台自动下载。

日本語：[ネイティブ版の更新](../docs/NATIVE_UPDATES.md)ではデスクトップ・スタートメニュー・タスクバーを固定入口に統一します。検証後の次回起動から新版になり、起動中の版と旧版を保持します。ホットパッチや自動ダウンロードではありません。

- Ordinary 3DGS/2DGS PLY transform baking reads the complete source, preserves field types/custom values, transforms covariance/tangents and SH through degree 4, and rejects nonuniform directional-SH appearance rather than approximating it. Recorded-camera exports retain their proper-Sim3 provenance constraints; original files/checkpoints remain unchanged. See [Gaussian PLY baking routes and fidelity limits](../docs/GAUSSIAN_PLY_BAKING.md).

中文：普通 3DGS／2DGS PLY 变换烘焙读取完整源数据，保留字段类型和自定义属性，处理协方差／切向与 0–4 阶 SH；方向 SH 的非等比缩放明确拒绝，不近似外观。记录相机路线保持 proper-Sim3 来源约束，原件和检查点不变。见[烘焙路线与保真边界](../docs/GAUSSIAN_PLY_BAKING.md)。

日本語：通常の 3DGS／2DGS PLY ベイクは完全な元データを読み、フィールド型・独自属性を保持して共分散／接線と 0–4 次 SH を変換します。方向依存 SH の不均等スケールは近似せず拒否します。記録付きカメラの proper-Sim3 出所制約を維持し、元ファイルとチェックポイントは変更しません。[ルートと忠実性の限界](../docs/GAUSSIAN_PLY_BAKING.md)を参照してください。

- Shared recorded-frame PLY export covers ordinary points, indexed polygon meshes and 3DGS/2DGS. Safe referenced texture bytes, face-corner UVs, custom fields and model-specific cameras remain bound to their actual hashes. Proper uniform scene TRS is an explicit active edit, not a second unit conversion; repeat exports preserve the accumulated edit. Persistent batch observers detach without owning the coordinator; pause/cancel preserve complete-state continuation and completed experiments are not relaunched. See [support matrix and limits](../docs/RECORDED_PLY_AND_BATCH.md).
- Native 3DGS/2DGS runtime settings separate resource protection from execution features; requested options, dependency-resolved settings and observed loss/renderer history stay distinct. Legacy disabled settings remain effectively disabled. Full-state resume supports explicit guard-only changes after unchanged data/source/optimizer validation; runtime hot switching is not advertised. Probe failure, estimate failure, typed CUDA OOM, inferred legacy CUDA OOM text and normal exceptions retain different causes. Guard OFF never promises that CUDA will fit or that a short run reaches geometric accuracy. See [schema and capability matrix](../docs/RUNTIME_DECOUPLING.md).
- The native existing-job dialog lists and observes already recorded jobs without recovery mutations or starting training. A live OS-bound worker session is required for explicit pause/stop. Closing the observer cancels only its own inspection helper, not the observed trainer. Windows persistent training can continue after the desktop closes; background lifecycles and package acceptance remain separately tested, not inferred from a unit test.

中文：显存保护与各项执行优化分开，请求值、依赖解析值和实际执行分开记录；旧版 OFF 不重新开启子功能。只改保护策略通过严格完整状态续训生效，不宣称热切换。现有任务观察不启动训练，明确控制须验证会话；关闭观察不停止训练。详见[运行设置与验收边界](../docs/RUNTIME_DECOUPLING.md)。

日本語：メモリ保護と実行最適化を分離し、要求・依存解決・実測を区別します。旧 OFF 設定を再有効化せず、保護のみの変更は厳密な完全状態再開で適用します。既存タスクの観察は学習を開始せず、明示制御にはセッション確認が必要です。観察終了は学習停止ではありません。[設定と検証範囲](../docs/RUNTIME_DECOUPLING.md)を参照してください。

The new geometry route uses only training views and preserves original Gaussian models/checkpoints. Camera-Z/masks/scale/frame/backend identities are saved with a separate, unfused point cloud. It is an appearance proxy, not a certified material surface. Native display defaults to the training frame; canonical/source products are saved without guessing viewport alignment. Short actual training/resume tests do not establish reconstruction accuracy.

Windows has a real setup wizard definition, not only a portable ZIP. See
[Windows Installer](../docs/WINDOWS_INSTALLER.md) for build and acceptance.
The complete native/Qt/backend/source payload, shortcuts and Windows uninstall
entry are installed without auto-starting GSW or training; research runtimes
remain separate.

中文：Setup 安装向导支持目录选择、快捷方式和系统卸载，保留用户新增数据与独立训练环境。
日本語：Setup は導入先、ショートカット、アンインストールに対応し、追加データと独立した学習環境を保持します。

- Source-ported Blender cursor wheel zoom (persistent View → Zoom to Cursor) and projection-correct pan cover all model displays; zoom anchors the view-target plane, not an automatically picked surface. Source-ported LichtFeld whole-page upload budgeting is shared by paged points/meshes, with first-page progress and unchanged actual RAM/VRAM admission. See [fixed sources, capability matrix and acceptance limits](../docs/UPSTREAM_NATIVE_UPGRADES.md).

- Simplified Chinese, English and Japanese UI: choose View → Language to switch immediately, without restarting or interrupting running tasks. The preference is remembered; model, camera, selection, dialog parameters and training curves are preserved. New/changed features must maintain all three catalogs and live text bindings, and pass the build-time translation gate. See [localization and terminology](../docs/LOCALIZATION.md).
- Scene and training-output display names support all scripts, spaces, emoji and punctuation, without the former 120-character limit. Blank names are rejected. Names are preserved in project data while safe ASCII storage identifiers protect backend paths; import name metadata participates in the atomic publish/rollback transaction.
- Native Qt Widgets application with a GPU-backed OpenGL viewport.
- Dockable project tree, inspector, task queue, and process log.
- Shared Lucide SVG icons across workbench commands, project/history lists and viewport controls, with palette-aware Normal/Disabled/Selected/On states and HiDPI rendering. The compact editing rail preserves 32 px minimum targets and adapts to short viewports with horizontal/wrapped layouts; native Windows caption controls and filesystem-provider icons remain native. See [icon system and provenance](../docs/ICON_SYSTEM.md).
- Light, dark and automatic local-time appearance: automatic mode uses light from 06:00 inclusive to 18:00 exclusive and dark otherwise. Theme/language/scale changes refresh presentation without rebuilding model or worker state. File dialogs remember accepted file/folder visits, retain state across minimize/restore, and omit cancelled selections from history.
- Independent task-preview layers and per-experiment settings/results/resume records preserve existing scene objects and transforms. Pause Preview freezes display without pausing optimization; Pause Training saves supported full state and exits the worker. Reference-model display caches can yield GPU resources during training and recover afterward without deleting objects. Native 3DGS and 2DGS support compatible full-state resume; meshing/texturing stages do not. Logs and historical curves remain session-only. See [training resume and experiment history](../docs/TRAINING_RESUME.md).
- Multi-model selection: Ctrl-click toggles objects in the tree/viewport, Shift selects a tree range or adds a viewport object, and Ctrl+A selects all loaded models. Selected objects share a G/R/RR/S transform gizmo and group-bounds pivot, with atomic cancel/undo/redo and per-object persistence. F frames the whole selection. Group scaling is uniform to preserve relative shape; point trimming and export remain single-object operations. See [multi-object controls](../docs/MULTI_SCENE_IMPORT.md#同时选中与整组变换).
- Multi-object PLY import: importing into a populated scene asks whether to append (default), replace only the active object, or cancel. Point clouds, indexed meshes and Gaussian models can coexist in one depth-tested viewport, retain independent transforms, and be selected through the scene tree or visible geometry. Project Save/Save As, pending data migration, snapshots and untitled recovery retain the complete object collection; legacy single-scene projects still load. See [multi-scene import](../docs/MULTI_SCENE_IMPORT.md) for behavior and limits.
- Metashape-style untitled projects that can import, reconstruct, and train before the user chooses a save location. First save writes a portable `.gsw.json` document beside a linked `<name>.files` data directory; Save As copies managed data to a newly selected location while external datasets remain linked in place.
- Metashape-style Add Photos/Add Folder entry points that select media first and prefill the import plan without forcing project setup. Managed import includes recursive discovery, metadata manifest, video frame extraction, structured progress, and crash-safe journaled publish/recovery (including project reopen); existing image/COLMAP datasets can also be linked without copying. Dataset, reconstruction, scene, and task-history cleanup are separate menu and project-tree actions. Managed dataset copies are removed transactionally; reconstruction cleanup preserves photos, the dataset association, and the loaded scene; linked external datasets are never modified; scene cleanup never deletes the PLY/training output.
- Native COLMAP reconstruction dialog with standard, robust, and sequential presets, automatic newest-version discovery on the application drive, cache overwrite protection, live logs, cancellation, and sparse-model validation. COLMAP's native mapper snapshots are sampled into atomic PLY previews so the sparse point cloud grows in the viewport while camera registration is still running.
- Exit confirmation defaults to Cancel and offers to save project/crop progress. Foreground tasks owned by the desktop stop through the Windows Job Object/PID cleanup path. Persistent training instead has its own durable worker/session: the exit prompt explicitly states that it continues, the observer detaches without stopping it, and its output/workspace remains protected. Existing-job attachment does not acquire process ownership.
- Native OpenGL point-cloud rendering plus depth-sorted screen-space Gaussian splats using activated scale, normalized rotation, sigmoid opacity, and view-dependent SH degrees 0–4. The SH Display menu changes the display degree immediately without modifying source/export data; unavailable SH uses an explicitly labeled DC fallback.
- Blender-derived orientation navigation at the upper right uses front/back-shaded circular axis ends and thin shafts. Direction discs snap to signed views without changing projection; the center projection target and `Persp`/`Iso` label toggle projection, and Shift-clicking the center restores the default perspective. Free orbit, pan and zoom preserve the chosen projection. Circular hit regions follow the visible discs and hover buttons, with independent center and axis targets.
- Live 3DGS training frames use a bounded double-buffered CUDA VMM allocation exported as a Windows KMT object and imported directly by OpenGL. PyTorch packs the current Gaussian attributes on the GPU, CUDA performs only a device-to-device copy into the shared allocation, and OpenGL renders that same physical memory after per-slot event/fence handoff. The same shared frame can be switched between Gaussian splats and center-point rendering to expose densification in real time. Training never waits for the viewport; a busy or unsupported interop path drops the preview frame and retains periodic PLY checkpoints as the fallback.
- Blender v4.5 source-derived native viewport: antialiased multilevel decimal grids with continuous detail transitions and horizon fade, consistent red-X/green-Y/blue-Z axes, upper-right circular-axis navigation and unframed captions. Blender's 3D cursor is not treated as a world-origin or model transform handle and is not added by default. GSW's configured light/dark background is preserved rather than replaced by Blender gray. Shared source coordinates, fixed reference-plane elevation, model data, editing and training previews remain unchanged. See [port scope, licenses and verification](../docs/BLENDER_VIEWPORT.md).
- Optional camera visualization that walks upward from the loaded scene to find a standard 3DGS `cameras.json`, with one camera-trajectory toggle for the frustums and capture path.
- Automatic Gaussian/point/mesh mode selection, scene-bounds camera fitting, and a manual diagnostic fallback. Point-only PLY files above the 10-million-point editable threshold are converted once into a reusable disk-resident depth-3 octree under the source folder's `.gsw-cache`: fixed-width binary input is streamed in bounded blocks, while ASCII input uses an 8 MiB sliding-buffer, allocation-free scalar parser and one compact temporary spool instead of splitting millions of lines. Leaf pages preserve every finite source XYZ/RGB point in an exact 16-byte record, while bounded deterministic reservoirs provide parent LODs. The viewport performs frustum and projected-size selection, reads at most two point pages concurrently off the UI thread, keeps the nearest resident parent visible while detail arrives, and evicts least-recently-used GPU pages within the shared adaptive viewport budget. The source file is never simplified, rewritten, or held wholly in CPU/GPU memory; editing is disabled for this out-of-core path.
- Native mesh loading uses measured RAM/commit and GPU headroom instead of normal fixed vertex/face-count thresholds. View → Resource Budget offers live, persistent automatic/aggressive and manual ceilings; all models and pending mesh work share atomic reservations. Complete meshes remain resident when estimated CPU peaks and actual GPU buffers/atlases fit; otherwise a separate exact triangle cache uses a depth-4 spatial hierarchy, deterministic sampled parent LODs, pages of at most 65,536 triangles, smooth global area-weighted normals and local indexed vertices. Budgets can promote or demote existing mesh residency without changing transforms or camera. `GSW_MESH_RESIDENT_VERTEX_LIMIT` and `GSW_MESH_RESIDENT_FACE_LIMIT` still force paging for diagnostics. Legacy `GSW_MESH_CACHE_GPU_BUDGET_MB` (or `GSW_POINT_CACHE_GPU_BUDGET_MB` when the mesh override is absent) sets the same shared manual GPU ceiling, not independent pools. See [three-language resource policy and limits](../docs/RESOURCE_BUDGETS.md). PLY polygon meshes support ASCII and binary little-/big-endian data and fan-triangulate indexed faces. Original geometry remains unchanged and every non-degenerate source triangle is retained in leaf pages; sampled previews can still be incomplete when exact visible pages do not fit. Metashape-style `comment TextureFile` images and per-face-corner `texcoord` UVs survive both resident and disk-paged paths. Seam-aware indexed vertices, background image decoding, vertical texture-space conversion, edge-clamped mipmaps and supported anisotropic filtering provide textured rendering. Geometry paging does not page texture atlases; insufficient texture/working-set budgets report an error without resizing the original image. Multi-material and multi-texture PLY conventions remain unsupported.
- Original PLY coordinates are scanned and retained as double-precision bounds independently from compact GPU coordinates. If a coordinate magnitude reaches 100,000 source units, a CloudCompare-style reversible display shift is selected before float conversion and persisted in point/Mesh cache indexes; source files and lossless crop export remain untouched. The inspector exposes source precision, declared unit, CRS, global minimum/maximum/center/size, display shift/scale, and active reference plane. PLY has no mandatory unit/CRS field, so missing metadata is reported honestly as raw unit `u` instead of being silently labeled metres; recognized header metadata and a same-name `.prj` supply available context. Scene > Export Coordinate and Size Report writes JSON or CSV with the original values and `local = (global + shift) * scale` transform.
- Object-level selection works consistently for resident and disk-paged point clouds, triangle meshes, and Gaussian scenes. Clicking the model or its scene-tree entry selects the complete object without moving it. `查找模型` in the scene menu and visible selection toolbar (shortcut `F`) selects the object and frames its transformed full-source bounds from the current viewing direction. Horizontal and vertical FOV, viewport aspect ratio, model depth, projection mode, rotation, scale, and local display shifts all participate in the fit, so elongated or out-of-core models are brought close without clipping. Its dashed world-space bounding box uses the same depth buffer as the model, preserving visible front edges while hiding rear edges instead of floating over the scene. A Blender-style in-viewport strip offers Move, Rotate, Scale, and combined Transform gizmos: direct handles cover three axes, three planes, view-plane movement, axis/view rotation (including an edge-on screen-tangent fallback when a rotation plane is parallel to the view), an internal free trackball, axis/plane scale, and uniform scale. All model gizmos use fixed world dimensions derived from source bounds (radius 0.3 times the source scene radius, or 0.5 for the combined tool). Their centre balls, arrows, plane handles, scale blocks, and rings grow and shrink with perspective distance and orthographic zoom, without screen-size clamps. Combined controls retain separate proportional zones; drawing and picking share the projected geometry, and near-plane ring clipping preserves visible arcs; guarded hit priorities prevent a nearby ring or trackball from stealing a direct handle, and the shortcut hint stays outside the gizmo. Move and Rotate expose a real Global/Local orientation toggle. Scale and combined Transform instead show a disabled Local icon with a lock badge and translated explanation because their non-uniform scale handles must remain on the model basis to preserve the persisted shear-free TRS representation; clicking the control or pressing comma reports the reason without mutating the remembered Move/Rotate preference. Modal `G`, `R`, `R R`, and `S` remain available without a held mouse button. `X`/`Y`/`Z` constrain globally or locally where representable, `Shift` + axis selects a plane, `Ctrl` snaps, `Shift` provides precision, and numeric input is accepted. Left-click/Enter confirms while right-click/Escape restores the exact starting transform. A pivoted TRS matrix is shared by points, meshes, splats, disk-cache culling, point editing, picking, and camera trajectories without rewriting the source PLY; inverse-transpose mesh normals and scale-aware cache bounds preserve non-uniform scaling. Source-unit translation, normalized quaternion rotation, and local XYZ scale survive normal saves, untitled crash recovery, project snapshots, and transform undo/redo. Full-source rectangle/lasso selection remains available alongside a persistent 4-256 px continuous brush, with optional visible-point depth filtering, replace/add/subtract, clear, and invert actions; `Ctrl` + left-drag temporarily orbits from every trim mode.
- Original-index delete history with undo/redo and atomic cropped PLY export that preserves all vertex fields.
- Existing PowerShell/Python backend execution through `QProcess`.
- Qt high-DPI support with readable 90%-150% manual scaling, automatic screen/window adaptation, common window-resolution presets, and scene-only contextual render/edit toolbars that keep an empty workspace uncluttered.

The current viewport includes a native Gaussian preview, but it is not yet a production tile rasterizer. It projects each 3D covariance to a 2D EWA ellipse, sorts preview splats by camera depth after navigation, and composites premultiplied alpha. View-dependent SH 0–4 uses the pinned gsplat evaluator adapted to GLSL. Coefficient buffers remain resident while navigating or switching display degree. GPU tile sorting/culling and GPU timing remain pending; displayed FPS is viewport frame cadence, not a GPU timing query. Live shared-GPU training previews still supply DC only; PLY snapshots can supply full SH. See `docs/STUDIO_COMPARISON.md` for memory/device limits and remaining priorities.

Camera metadata is optional. When a scene is loaded, the desktop searches the scene directory and its parents for the nearest standard 3DGS `cameras.json`; a missing sidecar does not prevent the scene from opening, while an unreadable or malformed sidecar is reported to the user. Loading runs outside the UI thread, invalid entries are counted and skipped, and very long trajectories are evenly decimated for display while retaining the full source count. Reopening the same scene refreshes a repaired sidecar. The camera-trajectory control shows or hides the camera frustums and ordered capture path together. Pressing `Home` resets the view around the loaded scene center.

Rectangle, lasso, and brush selection operate on every source vertex even when display rendering is sampled. The brush shows its exact screen-space radius, persists the chosen size, and uses the same visible-only filter and original-index edit model; GPU ID picking remains pending.

Object selection keeps the source-accurate bounds only as a broad-phase check, then renders the currently resident point, Gaussian-center, or mesh primitives into a small device-pixel-aware GPU mask around the pointer. Empty volume inside a loose scan AABB therefore no longer selects the model. Selected bounds use subdued corner brackets while idle and expand to the complete dashed box only during an active transform, so the coordinate extent stays truthful without covering the viewport.

Crop export supports ASCII and binary little-/big-endian point/Gaussian PLY files. It copies retained vertex records without re-encoding custom Gaussian fields, updates the vertex count, writes atomically, and refuses indexed mesh PLY files whose face indices would become invalid.

COLMAP is an external native dependency and is never assumed to live on the system drive. The application checks the saved path, `COLMAP_PATH`/`COLMAP_EXE`, repository-local tool folders, the newest semantic version under `<application-drive>:\Tools\COLMAP`, legacy locations, and `PATH`; if none exists, the reconstruction dialog requires the user to select `colmap.exe` before a task can start. Official COLMAP 4.1.0 CUDA has been exercised through the complete native worker pipeline; see `docs/COLMAP_SETUP.md`.

Feature parity and release gates are tracked in `docs/NATIVE_PARITY.md` (packaged as `NATIVE_PARITY.md`).

The Windows preview package includes the native worker, GPU-preview publisher, and compute source needed to launch training. A compatible external Conda/CUDA environment is still required and may be located outside the system drive. Before 3DGS training starts, the desktop validates the selected dataset, sparse reconstruction, CUDA device, PyTorch, and both native 3DGS extensions. Iteration progress is streamed into the task table; on compatible NVIDIA/Windows drivers the viewport also consumes the live shared-GPU-memory frames, otherwise it reports the reason and stays on the checkpoint-based PLY path. A successful process is accepted only after the requested iteration contains a non-empty Gaussian `point_cloud.ply`; that result is then attached to the project, saved, and loaded into the viewport automatically. Cancelled and failed jobs report the latest usable checkpoint instead of discarding it. Windows Application Control failures are reported explicitly and are never bypassed by the application.

Before managed media import starts, the desktop probes the exact selected Python against the packaged `native/backend/compute.py` and requires `numpy` plus `plyfile`. Video import additionally requires at least one verified extraction route: FFmpeg, OpenCV in that Python, or the existing `gaussian_splatting` Conda `video_extract.py` fallback.

Python discovery prefers `GAUSSIAN_SPLATTING_CONDA_PREFIX`, then `GS_CONDA_PREFIX`, an active `gaussian_splatting` Conda environment, and conventional Miniforge/Conda/Anaconda environment locations on the install drive or in the user profile. An arbitrary `python.exe` on `PATH` is not accepted as a Gaussian environment.

The desktop application only auto-discovers a backend beside its executable. Source-tree development can explicitly select another trusted checkout with `GSW_BACKEND_ROOT`; the current working directory is never treated as a backend implicitly.

## Local Windows build

The build script discovers Qt/CMake/Ninja from `GSW_NATIVE_QT_ROOT`, the active Conda environment, common Miniforge/Miniconda locations, and the repository drive. Visual Studio 2022 C++ Build Tools are discovered automatically. Python 3.10 or newer is required for the worker test and staged-source syntax gates; these checks run for normal builds and `-Package` builds.

With `BUILD_TESTING=ON` (the default), the CMake-selected Python also needs
NumPy and Pillow for the real-framebuffer texture/text regression. Configure
fails with an explicit dependency diagnostic rather than silently skipping it:

```powershell
& <Python3_EXECUTABLE> -m pip install numpy Pillow
```

`native_texture_text_*` checks all three UI languages in both themes using
independent textured/untextured application processes. The title and tool-state
glyphs must retain at least 98% matching pixels; the tiny model must still show
the texture. `scripts/test_native_texture_text.py` additionally supports
`--source <single-atlas.ply>`, `--paging resident|paged`, and
`--texture-format ppm|png|jpg`. Real fixtures use independent copies and verify
that the original model and texture hashes are unchanged. This common native
mesh path covers imported Metashape models and the material previews generated
by OpenMVS/SuGaR; untextured 2DGS, TSDF, and GS2Mesh outputs retain their existing
rendering behavior. No training or optimizer-state capability is changed.

中文：默认构建测试需要 NumPy 和 Pillow。纹理模型回归在三种界面语言、
深浅两种主题中对比真实视口字形，同时确认贴图仍被绘制。真实模型仅使用
独立副本，校验原模型和贴图未变；训练及优化器状态能力不受影响。

日本語：既定のビルドテストには NumPy と Pillow が必要です。テクスチャ付き
モデルの回帰テストは、3言語・明暗2テーマの実際のビューポート文字を比較し、
テクスチャの描画も確認します。実モデルは独立したコピーのみを使用し、元の
モデルと画像が変更されていないことを検証します。学習やオプティマイザーの
状態に関する機能は変更しません。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1
```

Build and collect a runnable directory:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

When an installed package is still running, use `-PackageDirectoryName <safe-name>` to build a side-by-side package without terminating or partially replacing the active session.

Set `GSW_NATIVE_QT_ROOT` or pass `-QtRoot` when Qt is installed elsewhere.

SPZ interchange additionally requires zlib development files and the Zstandard CMake package. They are already supplied by the local Conda Qt toolchain. Other toolchains can provide `ZLIB_ROOT` and `zstd_DIR`; Windows CI installs `zlib` and `zstd` with the `x64-windows-static-md` vcpkg triplet. The pinned MIT SPZ source is vendored in `native/third_party/spz`, compiled locally, and included with its license in packages. CMake does not download codec source at configure time. See [studio comparison and SPZ limits](../docs/STUDIO_COMPARISON.md).

On a managed Windows workstation, configure an organization-approved code-signing certificate before building so the application and native test executables are signed before launch:

```powershell
$env:GSW_WINDOWS_SIGNING_CERTIFICATE_THUMBPRINT = "40_HEX_CHARACTER_THUMBPRINT"
$env:GSW_WINDOWS_SIGNING_CERTIFICATE_STORE_LOCATION = "LocalMachine"
$env:GSW_WINDOWS_SIGNING_TIMESTAMP_URL = "https://organization.example/rfc3161"
$env:GSW_NATIVE_CMAKE_ROOT = "C:\Program Files\CMake"
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

`GSW_NATIVE_CMAKE_ROOT` (or `-CMakeRoot`) selects a CMake distribution containing both `cmake.exe` and `ctest.exe`; this is useful when a managed policy rejects the copy bundled with Conda. The certificate or publisher must also be permitted by the active Windows Application Control policy. The project does not create or trust self-signed certificates and does not disable the policy. See `docs/WINDOWS_APPLICATION_CONTROL.md` for certificate checks, explicit CUDA-extension signing, verification, and the information to send to endpoint management.

Install or verify COLMAP on a non-system drive:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_colmap.ps1 -InstallRoot E:\Tools\COLMAP -Variant cuda
```

Generate the deterministic Gaussian renderer QA project:

```powershell
powershell -ExecutionPolicy Bypass -File native\tests\generate_gaussian_visual_fixture.ps1
```

Run the complete media-to-model smoke test through the same worker used by the
native desktop application:

```powershell
# Photo import -> COLMAP -> 1000-step 3DGS -> PLY/checkpoint -> native load
powershell -ExecutionPolicy Bypass -File native\tests\run_training_e2e_smoke.ps1

# Video import -> frame extraction -> the same reconstruction/training checks
powershell -ExecutionPolicy Bypass -File native\tests\run_training_e2e_smoke.ps1 -MediaMode Video
```

Each run uses a new directory below `.tools/training-e2e/`. It generates 16
translated multi-depth views, imports them transactionally, validates the
Gaussian PLY fields and checkpoint, and opens the resulting project with the
native executable when a local build is available. Use `-WorkRoot` to choose a
different empty directory; the script never deletes an existing run.

## Complete mesh loading, resource budgets and compact UI

Model-import and other Qt file dialogs start at 640 logical pixels wide (960 at 150% UI scale), bounded by the current screen. Later manual resizing is preserved. Toolbar labels reveal their containing surface in both themes and all three live languages.

The shared native mesh loader uses measured RAM/commit and GPU headroom, not ordinary fixed five-million-face or 1 GiB admission thresholds. **View → Resource Budget** defaults to automatic/aggressive mode: retain 10% of available headroom, with minimum reserves of 512 MiB RAM and 256 MiB GPU memory. Manual settings take effect immediately and persist. Mesh display data, in-flight mesh imports and page caches share reservations; estimated peak costs include geometry workspaces, UV seams, source coordinates, picking data and texture decoding, while GPU costs include uploaded buffers and RGBA mip levels.

When the full mesh fits, all valid triangles are loaded. Otherwise the disk-backed cache preserves exact leaf triangles and sampled parent previews. Higher budgets can promote an existing model to residency; lower budgets or pressure can demote it without changing its transform or camera. Requests keep their reservation until the worker actually stops, not merely until a request is abandoned. Integer/index/format guards and explicit diagnostic count overrides remain.

These are mesh-management targets, not hard whole-program or training quotas. Independent Gaussian/SH buffers are not all in the mesh ledger. Geometry paging does not page textures: a texture plus minimum working set that cannot fit produces an explicit error rather than silently exceeding the budget or changing the source image. Sampled previews can remain incomplete when exact visible pages do not fit. This is not a new surface-preserving LOD algorithm or a guarantee of generated mesh quality.

This path is shared by imported meshes and supported TSDF, SuGaR, GS2Mesh and OpenMVS-textured outputs; it does not alter original files, training parameters or optimizer-state capabilities. See [resource policy and measurement limits](../docs/RESOURCE_BUDGETS.md).

Public regression commands include `--smoke-test-window-appearance`, `--smoke-test-mesh-rendering` and `--smoke-test-resource-budget-ui`/`--smoke-test-resource-budget-mesh`/`--smoke-test-resource-budget-shared`. `--smoke-test-icon-ui` and CTest `native_icon_ui_*` exercise real toolbar/viewport clicks, all vector states, two themes, three scales, DPR 2, short-view layouts and source/transform preservation in isolated test settings. Render-performance FPS is completed viewport-frame cadence, not GPU timing or an estimated inverse CPU duration.

中文：文件窗口默认加宽，工具栏标签保持连续底色；统一图标随三语、主题和缩放即时更新。网格默认采用激进自适应预算，按实际余量完整加载或分页，预算改变不重置模型与视角。普通固定面数/1 GiB 门槛已取消，保留格式与整数保护；纹理不分页，预算不足时明确报错。预算不是整个程序或训练的硬配额，不更改原始数据或完整状态续训能力。实际按钮、高 DPI、小视口及源文件保持均有原生界面回归。

日本語：ファイルダイアログを広くし、ツールバーのラベル背景を連続させます。統一アイコンは3言語・テーマ・スケール変更に即時追従します。メッシュは実際の余量に応じた積極的な自動予算で完全常駐またはページングし、予算変更でモデルや視点をリセットしません。通常の固定面数/1 GiB 制限は廃止し、形式・整数の保護は残します。テクスチャはページングせず不足時に明示します。全体/学習の強制的な上限ではなく、元データや完全状態再開の機能を変えません。実際の操作、高 DPI、小さなビュー、ソース保持をネイティブ UI 回帰で検証します。

## License boundary

The original native desktop main program is GPL-3.0-or-later from the
2026-10-14 licensing change. See [LICENSE.md](LICENSE.md) for scope and exceptions,
[COPYING](../COPYING) for the full GPL text, and
[THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md) for upstream components.
Earlier MIT grants are not revoked; original code outside the main-program
scope retains the root terms. Separate worker and third-party files keep
their existing licenses, including Graphdeco's research/evaluation restrictions.

`third_party/blender-gizmo/` directly adapts Blender v4.5.0 cone generation and
cube vertex/index geometry, with upstream copyright, GPL-2.0-or-later notices
and modification records retained. The combined main program selects GPL
version 3. This is a native Qt geometry/presentation adapter, not Blender's
complete operator or windowing system: GSW retains its own drag, constraints,
numeric input, undo and model/project logic. Blender's infinite-grid behavior
remains a visual/algorithmic reference; that viewport grid was independently
implemented for GSW's Z-up coordinates and is not a newly copied Blender shader.

The GPL main program embeds the independently implemented
`src/shaders/SurfelProjection.glsl`. The old research-licensed
`third_party/2dgs-surfel/evaluate_surfel.glsl` stays preserved under its original
license but is neither embedded nor loaded by the GPL desktop. This display
replacement does not relicense training components or change optimizer-state
resume capabilities. The native desktop now also adapts a reviewed LichtFeld
whole-page upload policy and Blender projected-navigation code; see
[the scoped source-port and acceptance record](../docs/UPSTREAM_NATIVE_UPGRADES.md).
No complete LichtFeld training/rendering engine is linked by these CPU/Qt ports.

Redistributors must satisfy corresponding-source, license and notice duties
for the actual binaries and dependencies they supply. Separate compute
processes do not automatically guarantee that every distribution is legally
an aggregate. This scope statement is not a blanket legal assurance or a
commercial license for restricted training backends.

中文：自本次变更起，原创原生桌面主程序为 GPL-3.0-or-later，旧 MIT 权利不撤销。
Blender v4.5.0 移植是圆锥/立方体几何与 Qt 展示适配，不是完整操作器系统；
交互、约束和撤销仍由 GSW 管理。独立实现的二维高斯 shader 替代旧研究许可衍生
shader 的主程序嵌入，旧源码保留原许可。独立 worker 及其他第三方仍遵循各自条款，
不因 GPL 主程序而获准商用；分发须提供对应源码及声明，进程边界不等于法律保证。

日本語：本変更以降、独自のネイティブ主プログラムは GPL-3.0-or-later とし、
過去の MIT の権利は維持します。Blender v4.5.0 の移植は円錐/立方体と Qt 表示で、
操作器全体ではなく、操作・制約・取り消しは GSW が管理します。独自実装の 2DGS
shader が従来の研究ライセンス派生 shader の組込みを置き換え、旧ソースは
元の許諾を維持します。独立 worker と第三者は各条件に従い、主プログラムの GPL
で商用許諾を得るわけではありません。配布時は対応ソースと通知が必要であり、
プロセスを分けるだけで法的な互換性を保証するものではありません。
