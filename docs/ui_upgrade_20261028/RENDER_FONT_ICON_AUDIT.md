# P0/P1 rendering, font and icon source audit

Status: **static audit complete; no visual/runtime acceptance**. Baseline read:
`5183926e09f5b568379bb1f89ee1667a9bd38121` on `main`, 2026-10-28. This report does not identify the cause of the
user's visible navigation jaggies before a remote red-capable feedback loop.

## Execution boundary

Only repository/reference files are read. No local automated test, application,
window, screenshot capture, training, model or original photograph is executed
or changed. Production source, catalogs, CMake and worktrees are not edited by
this audit. Root owns implementation and the shared plan.

## Confirmed source facts

- `NativeViewport.cpp` constructs `QPainter(this)` over its `QOpenGLWidget` and
  delegates the orientation overlay to `drawBlenderNavigationPresentation`.
  The navigation primitive implementation uses floating-point lines/ellipses,
  not a small PNG navigation image or polygonal cone mesh.
- `BlenderNavigationPresentation.h` requests `Antialiasing`, depth-sorts six
  handles, uses near/far background/axis interpolation and upright labels.
  This is an actual adapted subset of Blender v4.5.0, not Blender's complete
  GPU/font edge-coverage pipeline.
- `WorkbenchIcons.cpp` uses a custom `QIconEngine`. `scaledPixmap` allocates
  `qRound(logical size × requested scale)` pixels, assigns that scale as DPR,
  and draws SVG into the logical rectangle. Direct paint callers render SVG at
  their actual painter transform. This is not evidence that every runtime icon
  request or fractional DPR is correct.
- The icon cache stores parsed/color-resolved `QSvgRenderer` objects. Its key is
  icon enum index plus full RGBA, not DPR; there is no bitmap-size entry in this
  cache. Omitting DPR here is not itself a stale low-resolution bitmap defect.
- `AppTheme::apply` requests Chinese Microsoft YaHei UI; Japanese Yu Gothic UI,
  Meiryo and Microsoft YaHei UI; English Segoe UI and Microsoft YaHei UI. Body
  size is `10pt × UI scale`, with `PreferAntialias`. These are font requests,
  not observed resolved font families or per-glyph fallback.
- Dock/navigation/HUD and chart roles also use px or reduced relative sizes.
  Requested typography is not yet a single explicit role system.

## Required remote facts before a rendering conclusion

For each real window capture retain logical viewport size, physical framebuffer
size, actual DPR/logical DPI, requested and actual GL format, FBO samples,
Qt/GL vendor/renderer/driver, draw/composite path, font resolution and background
RGBA. A requested hint or `samples=4` is not a pixel-quality PASS.

No root cause, edge metric, font fallback, cross-screen behavior, visual gain or
performance result is claimed by this source-only report.

## Drawing and input chain at the baseline

| Layer | Actual source | Static observation | Required runtime evidence |
| --- | --- | --- | --- |
| Window/DPI request | `native/src/main.cpp:114` | High-DPI rounding is PassThrough; requested GL3.3 core/depth24/stencil8/MSAA4 | Actual window/screen DPR, context format and attachment samples, not the request |
| Physical GL viewport | `NativeViewport.cpp:3532` | `resizeGL` multiplies logical dimensions by `devicePixelRatioF` | FBO attachment pixel dimensions and GL viewport equal the recorded physical size |
| Navigation layout | `NavigationGizmo.cpp:70`, `NativeViewport.cpp:6639` | Layout consumes logical viewport dimensions and the current font height; ellipse/endpoint hit math is logical-coordinate based | Same real pointer positions still address the visible handles under each DPR |
| Overlay pass | `NativeViewport.cpp:3806` | Model/selection/transform/trackball and navigation use a QPainter over QOpenGLWidget; navigation follows general overlay/tool strip | Actual painter engine, transform and framebuffer samples at the draw |
| Navigation primitives | `BlenderNavigationPresentation.h:94` | Floating lines/ellipses and QPainter text; state carries hover/press/theme/custom backdrop | Matched orientation/size/background pixel evidence, normal-size review and independent edge oracle |
| Navigation forwarding | `NativeViewport.cpp:8930` | Passes actual `viewportColors().backdrop`, projection and camera availability | User-selected background RGBA remains unchanged, no fringe under light/dark/custom colors |
| State refresh | `NativeViewport.cpp:3538` | Palette/style events request presentation refresh and repaint; no navigation bitmap cache in this code | Same model/camera state retained and idle scheduling not converted into permanent redraw |

`RenderFrameDiagnostics.cpp` already records GL vendor/renderer/version, widget
DPR, framebuffer/attachment information and GL sample/blend state. Its desktop
capture path must **not** be invoked on this user's machine in this task. A
remote owned-window fixture can reuse appropriate metadata collection without
uploading the user's desktop or data. The existing recorder is not, by itself,
evidence that the new navigation matrix has run.

## Exact upstream navigation drawing that was not wholesale ported

Official immutable revision: Blender v4.5.0,
`8cb6b388974a817afedf1317ce26f0c75aa5f181`. Original files were retrieved to
`D:/Codex/validation/gsw-ui-upgrade-20261028/source-references/blender-8cb6b388/`.
Raw file hashes and paths are in `RENDER_REFERENCES.json`.

1. [`view3d_gizmo_navigate_type.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc#L187)
   chooses an alpha blend and the smooth-color polyline shader for axis stems.
   Its circular endpoints call `UI_draw_roundbox_4fv_ex` at line242.
2. [`interface_draw.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/interface/interface_draw.cc#L69)
   binds widget geometry/parameters to `GPU_SHADER_2D_WIDGET_BASE`, draws, then
   restores Blender's selected blend mode.
3. [`gpu_shader_2D_widget_base_frag.glsl`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/gpu/shaders/gpu_shader_2D_widget_base_frag.glsl#L11)
   builds pixel-space signed-distance masks, separates interior/border/emboss
   coverage with a half-pixel transition, composes alpha and converts into the
   selected framebuffer space. This is additional behavior beyond choosing
   ellipse positions/colors in the current Qt adapter.
4. [`gpu_shader_3D_polyline_frag.glsl`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/gpu/shaders/gpu_shader_3D_polyline_frag.glsl#L11)
   applies an explicit smooth-line alpha factor. Its vertex/interface setup
   supplies the distance coordinates and is not reproduced by this report.
5. Blender BLF handles font size/style and upright alignment separately. GSW
   replaces that with Qt text. No Blender font or font rasterizer has been
   added by this audit.

All named candidate files carry GPL-2.0-or-later, with Blender Authors or NaN
Holding BV copyright as stated in each file. The pinned root `COPYING` points
to the retained GPLv2 text; the individual later-version grants must not be
discarded. The existing original GSW native program chooses GPL3-or-later for
the combined application. These are reviewed **reference candidates**, not new
ports: this audit changed no production file and added no source to a binary.
Any actual adaptation still needs its retained subset, dependency/coordinate/
color-space changes, notices and acceptance recorded by the implementing owner.

## Font roles and unresolved runtime facts

| Role | Current request / owner | Not established by static source |
| --- | --- | --- |
| Base widgets/forms | `AppTheme.cpp:300-316`, 10pt×UI scale; language-specific family lists above | Actual font, fallback, hinting, glyph coverage and raster result for every form |
| Dock titles | `AppTheme::dockTitleFontPixelSize` → scaled15px; `MainWindow.cpp:304-307`, normal weight | Optical hierarchy versus body text and clipping at fractional DPR |
| Section titles | Global QSS, weight600 and scaled padding | Baseline/line height with long English/Japanese labels |
| Navigation axis labels | `BlenderNavigationPresentation.h:115-118`, bold9–18px based on radius | Actual font substitution and stroke placement on the final GL painter |
| Navigation projection label | Same header:169-172, regular9–13px | Tight available height and actual Japanese/English layout |
| Viewport title/count | `NativeViewport.cpp:8853-8861`, base−1pt with8pt floor (or px−1/11px floor); title DemiBold | Minimum normal-size readability and contrast over real model colors |
| Viewport status label | `NativeViewport.cpp:4666`, inherits viewport font | Full text/tooltip and actual glyph fallback in each locale |
| Training chart | `TrainingMonitorWidget.cpp:178-182`, base×0.82 with7pt floor | Actual label readability, metrics alignment and unchanged chart data |
| Training metric/title emphasis | `TrainingMonitorWidget.cpp:75-77,303-305`, inherited font with bold emphasis | Tabular-number behavior and real layout under narrow widths |
| File/history category groups | `FileDialogHistory.cpp:308-310`, inherited tree font with emphasis | Runtime file-provider/Qt family and Unicode filenames |
| Logs | `AppTheme.cpp:472`, Cascadia Mono/Consolas family list in QSS | CJK/emoji fallback may differ from the Latin monospaced face |

The source contains no general per-role resolved-font telemetry. Existing
`FileDialogHistorySmokeTest.cpp:664-675` uses QFontInfo for its own file-history
scope only; that is not proof for all other surfaces. Qt distinguishes a
requested QFont from the effective QFontInfo returned at painting time.
[Qt QFontInfo](https://doc.qt.io/qt-6.8/qfontinfo.html#details)

Remote FONT-01 evidence should capture `QWidget::fontInfo()` or
`QPainter::fontInfo()` after each live language/scale change. A mixed-string
glyph-run check is needed if claiming per-glyph fallback rather than only a
primary family. Do not infer that a requested Windows font is missing, or
copy Windows fonts into the package, from a serif-like screenshot.

## Icon/resource inventory

- There are **67 semantic IDs**, **63 unique SVG names** and **63 vendored SVG
  files**. Every statically referenced SVG exists. Reuse pairs are `box`,
  `globe`, `images` and `info`; reuse is not automatically a visual defect.
  `RENDER_REFERENCES.json` preserves the complete ID→asset mapping.
- Lucide revision `70562c1ee1c4fdcf736fe97bc893fb8511927934`; 24-unit viewbox,
  2-unit round strokes for the inspected artwork. Original ISC/Feather MIT
  notices remain in `native/third_party/lucide/LICENSE`; resource aliases are
  embedded by `native/lucide_icons.qrc`.
- Shared consumer groups: main actions/dock buttons/project tree/task tabs;
  dataset-import source rows/buttons; file-history tree/clear action;
  experiment result/resume/history rows; multi-item list actions; task details;
  window/fullscreen controls; viewport transform strip/status-detail button;
  navigation mini buttons. Native file-provider icons remain framework-owned.
- Main toolbar icons request scaled20px, tree/task tabs scaled16px;
  `MainWindow.cpp:4188-4203` also preserves scaled32px tool-button minimums.
  File-dialog navigation uses max(scaled20px, actual font metrics height),
  `WindowUi.cpp:273-278`. Other form/list owners scale through AppTheme.
- State resolution: disabled uses disabled WindowText; selected uses ordinary
  foreground for tinted container selection; checked uses the light highlight
  or dark text role. Active/hover also depend on the actual container/painter.
  It needs contact-sheet and real-control evidence, not merely an enum test.
- The parsed SVG renderer map is a static QHash with no explicit capacity or
  eviction. Its cardinality can grow with new RGBA requests. This is a bounded-
  resource **risk to measure**, not an observed leak or proof of degradation.
- Source search of production UI (excluding smoke/test filenames) did not find
  an identified emoji/geometric-symbol substitute for a semantic operation icon.
  Mathematical arrows and multiplication signs in explanatory text are not
  icon placeholders and must not be removed. The camera emoji in language
  smoke fixtures is deliberate user-filename data.
- The separate application-identity PNG is **1024×1024**, not a tiny operation
  icon. The ICO contains32-bit entries at16/20/24/32/40/48/64/96/128/256px.
  Branding/caption/taskbar rendering is a separate route from Lucide actions.

## Public documentation cross-check

- Qt's [High DPI model](https://doc.qt.io/qt-6.8/highdpi.html) distinguishes
  logical coordinates and physical pixels. Widget/event geometry is logical;
  low-level OpenGL needs explicit density handling. A QT_SCALE_FACTOR test
  changes effective DPR, but is not proof of a real multi-monitor transition.
- [QPainter rendering quality](https://doc.qt.io/qt-6.8/qpainter.html#rendering-quality)
  describes floating-point geometry and implementation-dependent render hints.
  A requested flag is therefore not a verdict on the user's real GL window.
- [QIconEngine::scaledPixmap](https://doc.qt.io/qt-6.8/qiconengine.html#scaledPixmap)
  receives device-independent size and a scale normally matching DPR. Current
  WorkbenchIconEngine follows that declared shape; fractional rounding and the
  actual caller's request still need recorded pixel evidence.

## Suggested narrow interfaces, not an unapproved refactor

1. Keep layout and hit testing in the existing navigation interface. Isolate
   edge coverage behind one drawing module receiving the same layout/state
   plus actual DPR. If a pixel cache is introduced, its key must cover every
   rendered input and its memory/extent/lifetime must be bounded.
2. Deepen existing AppTheme into shared metric and typography roles, rather
   than scattering new point/pixel arithmetic among callers. Language/scale
   refresh must remain presentation-only.
3. Keep WorkbenchIcons as the one semantic artwork interface; do not create a
   parallel icon manager. If profiling warrants bounded caching, compare the
   exact same public icon/painter output and state behavior.
4. Metadata can be exposed through the existing diagnostic seam in a remote
   owned fixture. No permanent repaint loop, renderer resolution increase,
   training change or user desktop capture is required for this audit.

## Open acceptance and handoff

`NAV-01/02/03`, `DPI-01`, `FONT-01`, `ICON-01` and related theme/performance
rows remain **NOT_RUN for this audit**. Current files/versions and declarations
are an implementation starting point, not source-equivalent runtime success.
All production choices and remote tests remain with Root.

Audit-only command issues retained: an external `rg` call incorrectly received
PowerShell's `-ErrorAction`; a broad optional cache search met WinSAT access
denial; and a PowerShell foreach pipeline initially had a parser error. Those
calls produced no production changes. Subsequent targeted reads used existing
paths; official references were fetched directly without an access bypass.
