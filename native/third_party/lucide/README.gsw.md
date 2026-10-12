# Lucide artwork used by Gaussian Scene Workbench

- Upstream: https://github.com/lucide-icons/lucide
- Pinned commit: `70562c1ee1c4fdcf736fe97bc893fb8511927934`
- Source files: upstream `icons/*.svg`; original SVG geometry is unmodified.
- License: ISC, with the upstream Feather-derived subset under MIT. See the complete `LICENSE` file distributed with this directory and installed in `licenses/lucide`.
- Integration: `native/lucide_icons.qrc` embeds the selected assets. `WorkbenchIcons` resolves `currentColor` to the current application palette at render time and caches the resolved SVG renderers. Default viewbox is 24 units with a 2-unit stroke.
- Scope: semantic desktop operation icons. The existing GSW application identity, native Windows caption controls, file-browser provider icons, and colored 3D axes remain unchanged.
- Blender, CloudCompare, and Metashape are layout/interaction references only. No proprietary or GPL application artwork is included in this directory.
