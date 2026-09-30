# Native staged mesh acceptance — 2026-09-30

Branch: `agent/native-desktop-0.3`. Desktop-only batch; no changes to HTML `main` or installed external algorithm sources.

## Delivered scope

- Native Workflow / toolbar Generate Mesh dialog, source/backend compatibility checks, latest complete source iteration, TSDF/components/SuGaR/GS2Mesh options and optional OpenMVS photo texturing.
- Typed isolated mesh jobs for bounded/unbounded TSDF, SuGaR and GS2Mesh. Atomic stage journal, complete XYZ/triangle validation, preview after geometry validation, cancellation and preservation of completed geometry after later failure/cancellation.
- Appended model associations rather than replacement. Original model selection, file bytes and transforms persist; successful and partial results survive saved project reopen.
- Fixed preview adoption so a retained original and a newly generated result cannot share a mutable viewport/GPU SceneState.
- Chinese/English/Japanese immediate translations for dialog, actions, stages and mesh task history, without restarting workers/models.
- Literal-only validation of external `cfg_args` before upstream renderers evaluate it. A code-execution payload is rejected without creating its marker or launching meshing.
- Reuses existing shared backend commands and official installed 2DGS source. Upstream licenses retained unchanged.

## Test results

| Gate | Result |
| --- | --- |
| Complete native CTest suite | 43 / 43 passed, 135.47 s |
| Catalog validation | 1232 UI messages, 1258 catalog entries, all three locales complete |
| Catalog validator unit tests | 14 / 14 passed |
| General worker suite after final Python change | 78 discovered; 66 executed/passed, 12 optional-runtime tests skipped |
| Real NumPy/plyfile mesh orchestration | 10 / 10 passed separately in Python 3.7 and Python 3.10 training environments |
| Installed application locale smoke | zh_CN, en_US, ja_JP passed; mesh form images visually inspected for clipping |
| Installed mesh lifecycle smoke | Passed: successful and failed-texture result append, original reselection, transform/file preservation, locale/history updates, save/reopen |
| Installed existing training lifecycle | 3DGS and 2DGS pause/resume desktop smokes passed |
| Installed backend real CUDA mesh test | Passed: official 2DGS bounded TSDF, 700 vertices / 1274 faces; source PLY SHA-256 unchanged |

The generic build Python has no numeric training runtime: its 10 mesh test skips were subsequently covered with actual NumPy/plyfile interpreters. The other two optional tensor tests belong to the existing checkpoint suite, whose real CUDA acceptance was recorded in the previous batch. The CUDA mesh test uses four synthetic cameras and a 289-surfel analytic plane, not a real-photo quality benchmark. The installed worker was launched with the desktop's existing Python 3.7 interpreter, while the official renderer ran in isolated Python 3.10 / PyTorch 2.0.1+cu118 on RTX 4070 Laptop GPU. Final test duration: 11.738 s, not a comparative performance claim.

## Installed package and evidence

Package: `E:/Gaussian-Scene-Workbench-Dev/native/dist/Gaussian-Scene-Workbench-0.3.1-native-mesh-stages-win-x64`.

App-local Qt smoke checks used only package `bin` plus Windows system directories in PATH. The staged worker, mesh module, capability matrix and shared backend hashes match source. Packaged executable matches the tested build executable.

- Executable SHA-256: `F17E3301E49B6D423DA4323CA49A7FC596BC2E38ED58AA936E9502D7E62038C2`.
- Mesh worker SHA-256: `71FE25FEB780BD878A4A240D6CAC0774DFA6546BB8620EC725FB8CE61DB2D40E`.
- Desktop `Gaussian Scene Workbench.lnk` points to this native package. Previous native packages and the legacy HTML application remain untouched.
- Local logs: `E:/GSW-validation-temp/native-mesh-stages-build.log`, `mesh-generic-worker-tests.log`, `mesh-workflow-legacy-python.log`, `mesh-workflow-2dgs-python.log`, `mesh-cuda-installed.log`, `mesh-installed-qa.log`.
- Locale images: `E:/GSW-validation-temp/mesh-stages-installed-qa/*-meshing.png`.

## Remaining acceptance gates

Bounded CUDA smoke proves command/geometry integration, not representative-data quality. Unbounded contracted-space quality and real SuGaR refinement, GS2Mesh stereo-depth and OpenMVS photo/material runtimes remain unverified. Their missing runtimes fail explicitly. OpenMVS is only an optional native texturing stage; standalone dense reconstruction is not implemented. Generated OBJ/MTL/PNG/ZIP assets (optional GLB conversion) are saved separately; automatic generated-texture viewport loading is not implemented. Per-voxel/depth-frame previews, stage retry/recovery and mesh optimizer-state resume remain outstanding. None is presented as Gaussian full-state resume.
