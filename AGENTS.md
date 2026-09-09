# Native desktop development

- Work directly on `agent/native-desktop-0.3` in this checkout. Do not modify the legacy HTML `main` branch.
- Keep builds, generated files, fixtures and packages on a non-system drive.
- Every new or changed user-facing feature must include Simplified Chinese (`zh_CN`), English (`en_US`) and Japanese (`ja_JP`) text in the same change. This applies to menus, dialogs, tooltips, viewport overlays, validation messages and status/monitor labels.
- Use the native translation catalog and its validation command; never introduce unlocalized UI literals. Preserve placeholders, keyboard shortcuts, numeric units and machine-facing identifiers. Do not translate user filenames, project data, JSON keys, CLI flags or external raw logs.
- Use consistent photogrammetry, point-cloud, mesh and 3D transformation terminology. Consult `docs/LOCALIZATION.md` and its glossary; reference official CloudCompare, Metashape and Blender terminology when adding terms.
- Run translation validation and relevant native tests, package and verify the installed desktop build, then commit and push authorized changes to the configured desktop remote before reporting completion.
