# Native desktop localization

## Language switching

Open **View → Language** (`视图 → 语言 / Language`, `ビュー → 言語 / Language`) and choose **简体中文**, **English**, or **日本語**. The language changes immediately in the current window and is remembered for future launches. No restart or confirmation is needed. Switching refreshes text and fonts without reloading the model, resetting its transforms/camera/selection, or interrupting processing and backup jobs. Dock layout, active tabs, edited dialog parameters, training progress and curve samples are retained.

The initial default is Simplified Chinese, independent of the Windows display language. Menus, panels, dialogs, viewport/transform hints, training-monitor labels, validation errors, and recovery/backup messages use the selected language. Qt file dialogs are used so their standard controls follow this choice too.

User filenames, entered project/scene names, paths, project data, JSON/protocol identifiers, and third-party raw logs are not translated. Existing project/task names and historical log records keep their original text; current status labels and newly generated messages use the new language. Units (`mm`, `cm`, `m`, `dB`) and established identifiers (`PLY`, `COLMAP`, `CUDA`, `PSNR`) remain stable. Language changes never rename user data. Command-line help and developer-only smoke-test output remain English.

## Mandatory feature maintenance

1. Use `QCoreApplication::translate("Workbench", "source text")` for dynamically formatted UI text. Bind persistent widget/action properties using `AppLanguage::text` / `AppLanguage::bind` with `AppLanguage::source("source text")`. Use `bindComboItem` for translated combo captions (it blocks signals so edited presets are not reset). Refresh dynamic labels and custom drawing with `AppLanguage::onChanged`; callbacks must change presentation only, never reload a scene or re-ingest telemetry. Connections are tied to the target QObject's lifetime. Never reverse-match rendered text to translate it. Prefer complete sentences and keep command/item identifiers separate from labels.
2. Add/update `zh_CN`, `en_US`, and `ja_JP` in `native/i18n/catalog.json` in the same change. Source keys may be Chinese or English. Preserve placeholders, keyboard shortcuts, wildcard file filters and HTML markup. Update the glossary when introducing terms.
3. Run `python scripts/native_i18n.py` and `python scripts/test_native_i18n.py`. Validation rejects missing entries/locales, duplicate keys, inconsistent placeholders, filters and markup. Literal auditing catches Chinese literals and English phrases in native implementation files; reviewers must also inspect short English labels and dynamically assembled UI text.
4. Run the native CTest suite, including `native_language_zh_CN`, `native_language_en_US`, and `native_language_ja_JP`. Inspect all three languages for clipping when changing forms, toolbars or overlays. Package and verify the installed build before delivery.

CMake runs validation, generates TS files in the build directory, compiles them with Qt Linguist `lrelease`, and embeds the QM catalogs. Qt base catalogs for Chinese and Japanese are embedded for standard controls. Qt Linguist Tools and Python 3 are build dependencies. Runtime translation is offline.

QA example: `"Gaussian Scene Workbench.exe" --smoke-test-language --language ja_JP`. The language override does not modify the user's preference. Language smoke tests isolate their settings and cycle all three languages twice in one window. They verify live actions/dialogs, Qt buttons, persistence, camera/transforms/selection, telemetry samples and unchanged backend/preset/user-name data. Build-tree tests also keep a real test-owned worker process running during the switches. Set `GSW_LANGUAGE_SCREENSHOT_DIR` to a non-system-drive directory for application-only QA images.

## Terminology

Scene and training-output names accept arbitrary Unicode text, spaces and symbols (including characters forbidden in Windows filenames). Only blank names are rejected. `ManagedName.h` maps display names to backend-safe ASCII storage identifiers; matching Python validation must stay in sync. Legacy safe ASCII names keep their paths, except the reserved `gsw-name-` namespace. Names requiring encoding use that prefix plus the full SHA-256 of the exact UTF-8 name. Do not trim, case-fold, translate or sanitize the display name. `.gsw-name.json` retains the original name alongside the dataset and travels with project migration/backup; import publishes it in the same transaction as the data. Existing job configurations without display metadata remain supported. Actual export filenames still obey operating-system filesystem rules.

| 简体中文 | English | 日本語 |
| --- | --- | --- |
| 工程 / 数据集 | Project / Dataset | プロジェクト / データセット |
| 显示名称 / 存储标识 | Display Name / Storage Identifier | 表示名 / 保存用識別子 |
| 点云 | Point Cloud | 点群 |
| 稀疏点云 | Sparse Point Cloud | 疎な点群 |
| 网格 / 三角网格 | Mesh / Triangle Mesh | メッシュ / 三角形メッシュ |
| 顶点 / 面 / 法线 | Vertex / Face / Normal | 頂点 / 面 / 法線 |
| 纹理 / UV 坐标 | Texture / UV Coordinates | テクスチャ / UV 座標 |
| 相机位姿 | Camera Pose | カメラ姿勢 |
| 重建 / 训练 | Reconstruction / Training | 再構築 / 学習 |
| 迭代 / 损失 | Iteration / Loss | 反復 / 損失 |
| 高斯点 / 增密 | Gaussian Splat / Densification | ガウシアンスプラット / 高密度化 |
| 移动 / 旋转 / 缩放 | Move / Rotate / Scale | 移動 / 回転 / スケール |
| 等比缩放 | Uniform Scale | 均等スケール |
| 全局 / 局部 | Global / Local | グローバル / ローカル |
| 轨迹球 / 吸附 | Trackball / Snap | トラックボール / スナップ |
| 锁定编辑工具 / 操作手柄 | Lock Editing Tools / Gizmo | 編集ツールをロック / ギズモ |
| 观察轨迹球 / 旋转中心 | View Trackball / Rotation Center | ビュートラックボール / 回転中心 |
| 导出模型 / 导出坐标 | Export Model / Export Coordinates | モデルをエクスポート / エクスポート座標 |
| 原始坐标 / 场景坐标 | Source Coordinates / Scene Coordinates | 元の座標 / シーン座標 |
| 烘焙变换 | Bake Transforms | トランスフォームをベイク |
| 坐标系 | Coordinate System | 座標系 |
| 透视 / 正交 | Perspective / Orthographic | 透視投影 / 平行投影 |
| 包围盒 | Bounding Box | バウンディングボックス |
| 快照 / 恢复 | Snapshot / Recovery | スナップショット / リカバリー |

## Editing guard

**Lock Editing Tools** is a persistent view preference, available beside **Inspect** in the selection toolbar and in the **View** menu (`Ctrl+Shift+L`). Locking cancels uncommitted transforms and selection gestures, hides transform handles, the transform strip, model bounds and model-selection tint, and disables transform/trim/delete/undo/redo actions. It preserves committed transforms, selection, gizmo mode and undo history. Camera orbit/pan/zoom, axis views and Find Model remain available; viewport geometry clicks navigate without changing selection. Unlocking restores the tools in Inspect mode without resuming an unfinished edit. The action and locked-state labels update immediately in all three languages without changing the lock state. This is a viewport editing guard, not a project/file permission lock: explicit import, project-tree selection and background processing remain available.

## References

### Model file export

**File → Export Model** (`Ctrl+E`) and the labeled toolbar button are separate from **Save Project**. Export works without cropping and while editing tools are locked. It exports the active model only, reading complete source records rather than preview/LOD buffers. Model selection, transforms, undo history and the original source remain unchanged. Point deletions are applied to exports. Current processing must finish first; live GPU-only training state is not exported as if it were a complete file.

Formats: PLY preserves source vertex attributes, topology and face UVs; unchanged sources stream in large blocks. XYZ/CSV contain source-unit XYZ and 8-bit RGB. OBJ contains full triangulated geometry, normals, vertex colors and face UVs plus a companion MTL/texture directory. Binary STL contains triangles only (no colors, texture or unit metadata). GLB 2.0 stores meshes or points, embeds PNG textures and uses Y-up/metres; undeclared source units explicitly default to one metre. GLB writes local float coordinates with a separate translation for large-coordinate models; the format's 32-bit file size bounds are checked. Conversion formats do not carry arbitrary PLY fields or CRS as a standardized coordinate system. GLB includes source-coordinate metadata in application-specific extras.

The dialog offers original coordinates or baked translation/rotation/scale about the model pivot; temporary viewport shifts are never exported. Gaussian PLY preserves its source coordinates and full Gaussian/SH attributes; baking Gaussian transforms remains unsupported and is disabled with an explanation. Gaussian XYZ/CSV/GLB exports are explicitly labeled as center points/base colors, not splat appearance or reconstructed surfaces. OBJ/STL require real mesh faces. Mirrored mesh transforms reverse winding in GLB/OBJ/STL; PLY explicitly requires original coordinates for mirrored meshes. Texture export currently supports the loader's single-texture, face-UV model; missing textures fail rather than silently disappearing.

Export runs off the UI thread with progress and cancellation. Temporary geometry resides beside the destination on its chosen drive with a bounded 64 MiB vertex-page cache. The final model uses atomic replacement; unique companion asset directories are retained only on success. Failed or cancelled exports keep an existing destination unchanged and remove only their own temporary files. Saving over a loaded source/project is blocked. Export does not mark crop history as saved in the project or turn conversion into an import.

Format references: [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html), [Wavefront OBJ specification](https://paulbourke.net/dataformats/obj/obj_spec.pdf), and [STL structure](https://paulbourke.org/dataformats/stl/). Explicit attribute-loss notices, source-coordinate handling and round-trip tests guard export fidelity.

Observation navigation follows the [Metashape Model-view navigation controls](https://www.agisoft.com/pdf/metashape_2_3_en.pdf) and Agisoft's [surface double-click centering guidance](https://www.agisoft.com/forum/index.php?topic=7955.0): while editing is locked, left-drag rotates with a full 3-axis trackball, Ctrl+left-drag pans, Shift+left-drag zooms, and the wheel zooms. Right/middle drag remain pan aliases. Colored front arcs constrain rotation about the corresponding world axis; the sphere interior rotates freely and dragging outside it rolls the view. Double-clicking model geometry sets the rotation center and recenters the view without changing model transforms, selection, zoom scale or projection. Empty space does not move the center. The GPU-only pick pass reads the nearest covered pixel's depth and unprojects that exact physical pixel (HiDPI included), considering all model layers and their display shifts/transforms. Meshes use triangle depth; point clouds and Gaussians use their rendered point centers, including resident paged data and live preview buffers. It does not load full source datasets or target grid/transform overlays. The navigation sphere is a screen-space guide (24% of the shorter viewport dimension), separate from world-space model gizmos; **View Trackball** toggles its visibility immediately and persistently. No proprietary Metashape code or assets are copied.

Terminology references: [CloudCompare official translations](https://github.com/CloudCompare/CloudCompare/tree/master/qCC/translations), [Agisoft official manuals](https://www.agisoft.com/downloads/user-manuals/), and [Blender Japanese manual](https://docs.blender.org/manual/ja/latest/). Application messages are original translations, not wholesale copies of another application's language library. Loading follows the official [QTranslator documentation](https://doc.qt.io/qt-6/qtranslator.html). Qt base catalogs remain upstream Qt assets; see `THIRD_PARTY_LICENSES.md`.
