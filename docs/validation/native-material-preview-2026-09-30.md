# Native material-preview acceptance — 2026-09-30

Branch: `agent/native-desktop-0.3`. This follows the staged-mesh batch; HTML `main` and external algorithm sources are unchanged.

简体中文：生成网格的原始贴图经验证后自动显示，多材质逐面角 UV 保留，不简化几何；材质失败时保留已完成几何，不标为成功。

English: Generated mesh materials are validated and displayed automatically, preserving original geometry and face-corner UV seams. Material failure preserves completed geometry without marking the task successful.

日本語：生成メッシュの材質を検証して自動表示し、元の幾何と面のコーナー UV シームを保持します。材質処理の失敗時も完成済み幾何を保持し、成功扱いにしません。

## Delivered scope

- Shared `texture_preview` / `texture_ready` stages for bounded/unbounded 2DGS TSDF, 3DGS SuGaR and GS2Mesh. SuGaR supplies its own materials; other methods can use optional OpenMVS texturing. Geometry-only outputs remain supported.
- Native-owned triangle OBJ-to-PLY material adapter with source double XYZ, complete topology, face-corner UVs, disk-spooled geometry/UV tables and an 8 MiB UV lookup cache. Multiple diffuse image pages are packed at original resolution into one atlas; no welding, downsampling or simplification.
- Correct per-material image collection for OpenMVS and SuGaR, with collision-free output names and preservation of all original OBJ/MTL/images. OpenMVS's fixed legacy `Tr 1` is normalized only in its copied assets, not in generic or SuGaR material handling.
- Automatic validated textured-model adoption; missing material outputs or unreadable atlas images cannot silently complete a task. Validated geometry and original texture bundles survive later failure/cancellation.
- Existing resident/disk-paged PLY texture rendering and exports are reused. Model switching, original file bytes/transforms, live three-language changes and project reopening preserve the material association.
- New module staged and checked in the desktop package. No upstream source was copied or modified for this adapter.

## Test results

| Gate | Result |
| --- | --- |
| Complete native CTest suite | 43 / 43 passed |
| Translation catalog | 1235 UI source messages, 1261 catalog entries; zh_CN / en_US / ja_JP complete |
| Translation validator tests | 14 / 14 passed |
| Generic build worker suite | 108 discovered; 66 executed/passed, 42 optional-runtime skips |
| Material suites in Python 3.7 training runtime | 54 discovered; 53 executed/passed, 1 Windows symlink-privilege skip |
| Material suites in Python 3.10 2DGS runtime | 54 discovered; 53 executed/passed, 1 Windows symlink-privilege skip |
| Installed backend material suites | 54 discovered; 53 executed/passed, 1 Windows symlink-privilege skip; imported implementation paths confirmed inside package |
| Existing SuGaR/OpenMVS/GLB collection regressions plus collector tests | 17 / 17 passed in Python 3.10 |
| Staged Python syntax | 70 files compiled without bytecode output |
| Installed desktop smokes | 8 / 8 passed: language and material lifecycle in each locale, plus 3DGS and 2DGS training-resume smokes |

Material suites cover all four mesh-method contracts; valid single/multiple atlas pages and pixel/UV mapping; UV seams; positive/negative indices and double coordinates; no source modification; source-change detection; cancellation; invalid paths/materials/maps/indices; image budgets; retained geometry after invalid or missing material outputs; and the OpenMVS-only transparency-marker normalization. Ordinary path traversal, absolute/UNC and URI rejection are tested even where symlink creation is unavailable.

Desktop material smoke uses deterministic fixtures, including a UV seam and a real PNG, rather than external reconstruction. It checks GPU texture readiness, model changes, task/history state, immediate translations and save/reopen. The test-owned hidden window explicitly renders a framebuffer at the final readiness gate so queued GPU upload is not dependent on hidden-window paint scheduling. Production rendering behavior was not changed for that test timing issue. Chinese, English and Japanese mesh-dialog screenshots were visually checked for clipping.

The first test run with TEMP nested under the source repository encountered the backend-locator ancestor-search fixture collision. Final runs use `E:/GSW-validation-temp/material-preview` outside the source tree; the production locator was unchanged. Nonfatal syntax warnings originate from unchanged bundled upstream SIBR files.

## Installed package and evidence

Package: `E:/Gaussian-Scene-Workbench-Dev/native/dist/Gaussian-Scene-Workbench-0.3.1-native-material-preview-win-x64`.

- Executable SHA-256: `897BD2E421D70F6F3F34357F24D8A49690FC1ED4078379298E657BA3FF9B8506`.
- Packaged material adapter SHA-256: `5141652F84BDA9DBACA2FEF58A8E94C5F6F4FFE291F9F8D5323A7795B07525BB`.
- Final build log: `.tmp/native-material-qa/build-material-final.log`.
- Installed smoke helper: `.tmp/native-material-qa/verify-installed.ps1`; final logs: `.tmp/native-material-qa/installed/language-{locale}.err.log`, `material-{locale}.err.log`, `resume-{backend}.err.log`.
- Locale images: `.tmp/native-material-qa/installed/{locale}-meshing.png`. Older diagnostic/probe files in that directory are not final acceptance results.
- Installed smokes use package-local Qt libraries/plugins with PATH restricted to package `bin` and Windows system directories. No running user application was terminated.
- Desktop `Gaussian Scene Workbench.lnk` is updated to the new native package. Older packages remain intact.

Reproduction commands (use a training interpreter with NumPy, plyfile and Pillow for material tests):

```text
python -B -m unittest native.worker.test_mesh_generation native.worker.test_mesh_material_preview crop_editor.tests.test_texture_collection
python scripts/native_i18n.py
python scripts/test_native_i18n.py
ctest --test-dir native/build-unity-gizmo --output-on-failure
```

## Remaining boundaries and acceptance gates

The adapter accepts only opaque diffuse-textured triangle OBJ assets. Non-triangles, tiled/out-of-range UVs, map transforms, unsupported shader effects, non-white diffuse multipliers, transparency and animated/HDR/EXIF-rotated images fail explicitly. Atlas dimensions are at most 8192 px, with a conservative 256 MiB image-conversion working budget; over-budget assets retain geometry and original texture bundles rather than silently reducing image or geometry fidelity. GPU upload remains subject to the existing device texture-size limit and explicit warning.

This is not general OBJ/GLB import, PBR/transparent-material rendering, stage recovery or optimizer-state resume. Representative real-photo OpenMVS/SuGaR/GS2Mesh/unbounded-TSDF GPU and material quality was not newly accepted in this batch. The earlier official 2DGS bounded synthetic CUDA result (700 vertices / 1274 faces) remains a command/geometry integration check, not a new photo-quality or performance claim. Standalone OpenMVS dense reconstruction and fine-grained extraction previews remain outstanding.
