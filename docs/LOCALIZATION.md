# Native desktop localization

## Language switching

Open **View → Language** (`视图 → 语言 / Language`, `ビュー → 言語 / Language`) and choose **简体中文**, **English**, or **日本語**. The choice is saved immediately and applied on the next application launch. Save your work and close normally when ready: language selection does not close the project, rebuild widgets, or interrupt processing and backup jobs.

The initial default is Simplified Chinese, independent of the Windows display language. Menus, panels, dialogs, viewport/transform hints, training-monitor labels, validation errors, and recovery/backup messages use the selected language. Qt file dialogs are used so their standard controls follow this choice too.

User filenames, entered project/scene names, paths, project data, JSON/protocol identifiers, and third-party raw logs are not translated. Units (`mm`, `cm`, `m`, `dB`) and established identifiers (`PLY`, `COLMAP`, `CUDA`, `PSNR`) remain stable. Language changes never rename user data. Command-line help and developer-only smoke-test output remain English.

## Mandatory feature maintenance

1. Use `QCoreApplication::translate("Workbench", "source text")` for UI text. Prefer complete sentences. Keep command identifiers and combo-box item data separate from labels; never use translated text for state, paths, object names or protocol keys.
2. Add/update `zh_CN`, `en_US`, and `ja_JP` in `native/i18n/catalog.json` in the same change. Source keys may be Chinese or English. Preserve placeholders, keyboard shortcuts, wildcard file filters and HTML markup. Update the glossary when introducing terms.
3. Run `python scripts/native_i18n.py` and `python scripts/test_native_i18n.py`. Validation rejects missing entries/locales, duplicate keys, inconsistent placeholders, filters and markup. Literal auditing catches Chinese literals and English phrases in native implementation files; reviewers must also inspect short English labels and dynamically assembled UI text.
4. Run the native CTest suite, including `native_language_zh_CN`, `native_language_en_US`, and `native_language_ja_JP`. Inspect all three languages for clipping when changing forms, toolbars or overlays. Package and verify the installed build before delivery.

CMake runs validation, generates TS files in the build directory, compiles them with Qt Linguist `lrelease`, and embeds the QM catalogs. Qt base catalogs for Chinese and Japanese are embedded for standard controls. Qt Linguist Tools and Python 3 are build dependencies. Runtime translation is offline.

QA example: `"Gaussian Scene Workbench.exe" --smoke-test-language --language ja_JP`. The language override does not modify the user's preference. Language smoke tests isolate their settings and verify actions, labels, Qt buttons, persistence, restart-only activation, and unchanged backend/preset/user-name data. Set `GSW_LANGUAGE_SCREENSHOT_DIR` to a non-system-drive directory for application-only QA images.

## Terminology

| 简体中文 | English | 日本語 |
| --- | --- | --- |
| 工程 / 数据集 | Project / Dataset | プロジェクト / データセット |
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
| 坐标系 | Coordinate System | 座標系 |
| 透视 / 正交 | Perspective / Orthographic | 透視投影 / 平行投影 |
| 包围盒 | Bounding Box | バウンディングボックス |
| 快照 / 恢复 | Snapshot / Recovery | スナップショット / リカバリー |

## References

Terminology references: [CloudCompare official translations](https://github.com/CloudCompare/CloudCompare/tree/master/qCC/translations), [Agisoft official manuals](https://www.agisoft.com/downloads/user-manuals/), and [Blender Japanese manual](https://docs.blender.org/manual/ja/latest/). Application messages are original translations, not wholesale copies of another application's language library. Loading follows the official [QTranslator documentation](https://doc.qt.io/qt-6/qtranslator.html). Qt base catalogs remain upstream Qt assets; see `THIRD_PARTY_LICENSES.md`.
