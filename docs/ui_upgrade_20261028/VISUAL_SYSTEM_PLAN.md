# P1 native visual system — implementation specification draft

**Status: SPECIFICATION / NOT_IMPLEMENTED / RUNTIME_NOT_RUN.** This document
defines the next design slice; it does not declare UI changes, font resolution,
interaction, contrast, DPI or whole-product acceptance complete. Production,
tests, CMake and workflows are unchanged by this author.

## 1. Three theses

### Visual thesis

GSW should read as a calm, precise 3D working instrument: the model viewport is
the dominant visual plane; neutral chrome and aligned text support it; one
action-blue family identifies operations without competing with domain colors.

### Content organization

1. **Primary workspace** — the actual model/grid/navigation and restrained
   selection/transform feedback, not a hero, welcome banner or dashboard cards.
2. **Navigation and actions** — familiar menus and two coherent toolbar levels:
   project/workflow actions, then display/view/edit context. Group related
   actions with spacing and a few separators, not a border around every group.
3. **Secondary inspector** — the project/scene tree and property context; names,
   values and units align, and full original values remain reachable.
4. **Activity** — the existing task/monitor/log dock and concise viewport status.
   Empty, running, paused, failed and cancelled states remain truthful; details
   carry diagnostics instead of duplicating long status strings everywhere.

There is no new brand slogan, enlarged product headline, hero illustration,
marketing copy, ornamental gradient, glass card stack or web navigation model.

### Interaction thesis

- Hover, keyboard focus, pressed and checked states change immediately on real
  input; the change clarifies an affordance, not decoration. No new animation
  timer, constant repaint, ease-out or artificial FPS motion is introduced.
- Existing camera/gizmo manipulation remains direct and numerically exact:
  preserve drag direction, axes, constraints, local/global interpretation,
  projection, snapping, shortcuts, numeric input, cancel and undo/redo.
- Existing details, dock resizing and form disclosure retain focus, selection
  and user layout. A presentation refresh does not rebuild a model, window,
  worker, queue, task or training request. No automatic expansion steals focus.

The installed `frontend-skill` Apps/Utility Copy principles inform restraint,
workspace hierarchy and operational wording. Its webpage hero, Framer Motion,
scroll choreography, imagery-generation and conversion rules do not apply to
this Qt utility. No web runtime or animation dependency is proposed.

## 2. Evidence and baseline boundaries

The four supplied local originals were inspected read-only:

- GSW navigation crop: `codex-clipboard-09d71a9c-b7a9-49ba-ae0a-b92f63813130.png`.
- Blender navigation reference: `codex-clipboard-ce9d1e59-0c4a-4490-ae71-b48580b96dfb.png`.
- GSW whole-window reference: `codex-clipboard-799183e6-0b2d-42b7-8f11-d906b43f2de2.png`.
- Blender whole-window reference: `codex-clipboard-9a6eda06-5bba-4ee6-ba0b-ddf77c8c476c.png`.

They remain under `D:/CodexData/Temp/`; no original was edited or uploaded.
Blender's whole-window image identifies **5.2.3 LTS**, whereas GSW's existing
source adapters pin Blender **4.5.0**. Different software, background, model,
scale and rendering conditions make those images visual references, **not pixel
goldens or evidence of a 5.2.3 source transplant**.

The already downloaded public `87a797b` synthetic screenshots were also viewed:
`acceptance-artifact/project-tree-chrome-en_US-{light,dark}/full-window.png`
inside the baseline artifact recorded in `CI_DELIVERY_BASELINE.md`. They show
the old implementation, not a new candidate. Visible issues suitable for the
next slice include crowded repeated footer information, strongly framed chrome
and a narrow inspector with long values; this is not a diagnosis of missing
fonts or a measured runtime root cause.

Preserve the user's white/light and dark viewport appearance as baseline.
Do not force Blender gray to improve perceived smoothness. The current surface
inventory separately identifies that arbitrary background/alpha controls are
not yet a verified existing capability; do not invent a saved preference or
claim custom-background acceptance from this specification.

## 3. Shared role system — one source of truth

Use the current `AppTheme`, `WorkbenchIcons` and `WindowUi` seams. Callers should
consume semantic colors/metrics, not add independent literal-color QSS strings.
The role names below are **proposals**, not assertions that an API exists.

### Color roles

| Role | Rule / initial candidate | Application |
| --- | --- | --- |
| `viewportBackdrop` | Preserve actual viewport setting/theme clear behavior; do not alias to chrome | Scene canvas only; never change model data or training scale |
| `surfacePanel`, `surfaceChrome`, `surfaceField`, `surfaceMenu` | Begin with existing neutral values; separate through subtle tone and spacing rather than stacked outlines | Docks/forms/toolbars/inputs/menus |
| `textPrimary`, `textSecondary`, `textDisabled` | Preserve current text contrast baseline; secondary does not mean unreadably small or translucent | Labels, hints, disabled reasons |
| `lineSubtle` | Candidate light `#d5d9de`, dark `#34383c`; visual-review target, not measured contrast approval | Sparse section/header edges, not every blank column |
| `lineField` | Retain existing stronger field boundary initially (`#8a8a8a` light / `#41464b` dark) until real-state contrast checks | Editable controls |
| `actionBlue` | One blue family; candidate strong light `#2467a5`, strong dark `#2d6bb2` | Actual primary operation / strong active-tool fill when appropriate |
| `actionTint` | Candidate light `#e4effb`, dark `#243c56` | Checked passive toggles, selection and understated action state |
| `focusBlue` | Candidate light `#135f9b`, dark `#78aef1` | Keyboard focus, distinguishable from hover and selection |
| `hoverNeutral` | Current neutral hover family, not another saturated brand color | Pointer-only affordance |
| `warning`, `danger`, `success` | Preserve amber/red/green semantic roles plus text/icon distinctions | Actual risk, failure/destructive action and success |
| `axisX/Y/Z` | Preserve current domain mapping, labels, signs, depth and active feedback | 3D navigation/transform axes; Z blue is not an application action state |

Current source uses blue in the light palette but teal in dark `Highlight`,
`Accent`, focus and pressed QSS tokens. To implement action-blue consistently,
update the **same semantic roles across QPalette, QSS, icons and custom
QPainter callers**. Do not recolor XYZ, warning or danger to fake a one-color
product. Candidate hex values require real-theme/state review and are not an
accessibility certification.
Include action links and ordinary in-progress fill in that role audit; success
green must mean a successful result, not an accidental second dark-theme brand
color. Preserve visited-link and paused/error distinctions through deliberate
semantic roles rather than erasing meaningful state differences.

No generic "all checked buttons are white" rule: strongly filled active tools
need an on-accent foreground, whereas tinted passive toggles and selected rows
need their readable normal/accent foreground. Reserve the same border extent
in all states so hover/focus does not move contents.

### Typography roles — preserve families first

The static family declarations in `AppTheme::apply` are:

- zh_CN: `Microsoft YaHei UI`.
- en_US: `Segoe UI`, then `Microsoft YaHei UI`.
- ja_JP: `Yu Gothic UI`, then `Meiryo`, then `Microsoft YaHei UI`.
- Body: `10 pt × existing UI scale`; logs: existing `Cascadia Mono`/`Consolas`.

Keep these declarations as baseline; do not download fonts, copy Windows fonts
into packages or infer absent fonts from screenshots. Actual `QFontInfo`, glyph
runs/fallback, mixed CJK/Latin/Unicode and physical-DPI behavior remain remote
evidence requirements. Fallback families are language coverage, not extra
decorative brand faces.

| Role | Specification |
| --- | --- |
| Body/action/value | Existing body size/family/normal weight; preserve readable density |
| Panel title | Same family; align optical height with body without making another oversized heading. Keep current pixel-based dock helper as compatibility baseline until measured. |
| Section title/model title | Body family, restrained semibold and spacing; not a border under every label |
| Hint/status | Body-sized text with semantic secondary ink; do not fix crowding by making tiny text |
| Numeric value/unit | Align values and units; use available tabular figures only after actual font support is established. Do not change decimals, range, rounding, serialization or stored precision. |
| Raw log | Retain monospace policy and raw external log contents; UI captions remain localized |

Do not apply DPR twice. UI percentage is widget/font density, **not** viewport
render-resolution reduction. Avoid a global pt→px conversion or weight increase
without actual font/baseline evidence at the affected DPRs.

### Metrics and states

Values below are 100% logical-pixel compatibility targets, consumed through the
existing scaling policy once and bounded by actual font metrics. They are not
hard fixed physical sizes or permission to shrink existing verified hit areas.

| Metric | Starting rule |
| --- | --- |
| Spacing family | 4 small / 8 normal / 12 group; prefer alignment/space to extra lines |
| Toolbar artwork | Existing 20 logical icon family; optical adjustments must remain within the semantic view box |
| Tool hit region | At least current verified hit extent and a 32-logical starting target; glyph size and hit size are distinct |
| Field/row | Preserve existing 26-control / 25-row baseline, expand for actual font/long content rather than clipping |
| Dock title | Candidate minimum24 and `fontHeight + 6`; content must not force wider persistent panels |
| Dock title controls | Candidate glyph14, hit22 minimum, never below existing effective area; one title bar only |
| Resize handles | Preserve at least existing effective8-logical draggable extent in both themes; neutral hover, real resize cursor |
| Corners | Existing small4 control radius / restrained menu2–4; no large rounded panel-card frames |
| Focus/border | Thin reserved boundary, clear focus-blue cue; no geometry jump, no erased keyboard focus |

States are not interchangeable: hover is neutral; pressed is momentary;
active tool is persistent and exclusive where the existing QActionGroup says
so; checked passive settings are a subtle tint/indicator; keyboard focus is a
separate visible cue; disabled actions retain a discoverable localized reason.
Because disabled widgets may not receive focus/tooltips normally, the reason
must also be available through the existing surrounding hint/accessibility
path, not only as unreachable disabled tooltip text.

## 4. Owner-specific implementation rules

### AppTheme

- Centralize role colors/metrics/type choices and derive QPalette/QSS/custom
  painter inputs from them. Keep `ui/theme`, automatic06:00–18:00 behavior,
  manual scale and persisted user choices intact.
- Preserve transparent layout-label/checkbox/radio/button-box backgrounds.
  Inputs, selected rows and progress widgets keep their intentional surfaces.
- Reduce redundant decorative borders, not input boundaries, keyboard focus,
  column resizing or split/dock drag handles. Keep the prior empty-column-line
  fix: item borders do not continue through blank tree/table space.
- Existing item-view `outline:0` must not become permission to lose keyboard
  focus. Provide a stable visible focus cue on the actual focused item/control,
  distinguishable from mere selection, without reintroducing blank-column rules.
- Use role properties/typed helpers without renaming existing object names used
  by actions, settings, accessibility or regression contracts.

### WorkbenchIcons

- Retain pinned Lucide vectors, semantic IDs, original asset bytes and notices.
  The current 24-unit/2-unit-stroke artwork is not a low-resolution PNG set.
- The cached object keyed by icon ID/ink is a **vector QSvgRenderer**; reusing it
  across sizes/DPR is not inherently a stale bitmap defect. Any raster cache
  added later must include logical size, actual DPR, mode/state and resolved
  ink/palette, be bounded, and invalidate appropriately.
- Existing `QIconEngine` stores a palette copy: refresh persistent action and
  dock icons on appearance changes rather than recreate the action/state.
- Resolve on-accent versus tinted-row foreground explicitly. Do not globally
  force checked/selected icons to white or substitute emoji/Unicode artwork.
- Make normal/hover/pressed/checked/disabled/focus contact sheets for both themes
  and all five requested DPRs. A stroke being fractional in physical pixels is
  not itself a fault; optical thickness and coverage must be measured.

### WindowUi

- Keep the OS caption as the only window-control bar; preserve minimize,
  maximize, restore, floating docks, F11/Esc and double-caption behavior.
- Do not change flags/recreate a visible native window to restyle it; preserve
  the existing modal, language-change and user-state-revision safeguards.
- File dialogs retain their current provider, accepted/cancelled result,
  directory, filename, filter, selection, history and user column widths. Do
  not replace them with a web/custom file browser to achieve visual sameness.
- Consume shared form/toolbar metrics, not a second color system. Keep once-only
  initial width behavior separate from later user resizing or theme changes.

## 5. Surface treatment and invariants

- **Menus/toolbars:** preserve menu names/order, actions and shortcuts. Use
  spacing for workflow/display/edit groups. Overflow must expose all existing
  actions, including save, train and cancel; no clipped icon guessing game.
- **Project tree/task lists:** full readable rows, subdued headers, no blanket
  vertical grid through empty content; keep selection, visibility-eye hit area,
  sorting, scrolling and column/viewport separation functional. Never make an
  eye click select/recenter a different model.
- **Inspector:** shared two-column rhythm; section hierarchy by spacing/weight.
  Long paths can wrap/elide with full tooltip/copy, but original filenames and
  exact values remain accessible. Do not truncate numeric precision or change
  units to fit. Avoid forcing the entire inspector to scroll horizontally for
  an ordinary label; special unbreakable values need an explicit usable path.
- **Activity/status:** keep one concise viewport performance/source-of-truth
  presentation; global footer emphasizes project/activity without repeating all
  diagnostic numbers. Existing details retain historical FPS/GL/grid metadata.
  Empty and idle are real states, not fabricated progress or moving numbers.
- **Navigation/transform overlays:** consume common ink/edge quality, but keep
  hit geometry and state contracts independent. Cover move/rotate/scale/combined
  modes, continuous shaft/head/cap appearance, front/back/edge views and both
  projections. A smaller center artwork must not make the free-drag target
  harder to hit or steal axis/plane selection. No white giant center or debug
  outlines that obscure the model.
- **Selection/grid/crop:** remain restrained and legible against actual chosen
  backgrounds; preserve deletion/selection data, grid spacing/units/reference,
  model transforms and camera. Visual polish is not a geometry-quality claim.
- **Forms/P2:** reuse one label/value/help/error/button rhythm, scrolling and
  progressive existing details; preserve every research parameter and advanced
  entry. No empty buttons, changed requests, silent defaults or reduced precision.
- **Layout:** preserve saved docks/tabs/scale. The existing "重置工作区布局"
  action remains an explicit presentation-only recovery entry; do not repurpose
  it to clear projects/settings for training. Do not auto-collapse user panes
  or jump the camera when a selection, language or task state changes.

## 6. Migration order and stop gates

1. **Token foundation:** add shared roles/metrics without changing backend or
   saved contracts; compare palette/QSS/custom paint consumers and declared
   font policy. Do not mark this visual PASS from static code presence.
2. **Shell vertical slice:** menus + two toolbars + dock title/resize + footer,
   with native icons and actual narrow/wide three-language windows. Stop if a
   critical action/focus/hit area becomes unreachable.
3. **Tree/inspector/activity:** close clipping/empty-column/repeated-status
   issues using the surface inventory's actual entry points. Preserve all
   public selection/visibility/renderer/idle assertions.
4. **All manipulation modes/overlays:** reuse the verified P0 edge module only
   after separate transform geometry/picking/constraint/undo checks; upgrading
   combined mode alone is insufficient.
5. **P2 form families:** migrate common form wrappers then each of the inventoried
   concrete dialogs/special states, not just the training dialog. Review normal,
   empty, busy, failed, unsupported, cancelled and long/Unicode content.
6. **Evidence/packaging:** bind actual source, normal-size originals, actual
   font/glyph/DPR/render-path records, interaction and the separate frozen
   performance plan. Preserve stable managed-launcher delivery and old sessions.

Required matrix scopes: `FONT-01`, `ICON-01`, `SHELL-01`, `TREE-01`,
`GIZMO-01/02`, `MODEL-01/02`, `OVERLAY-01`, `GRID-01`, `FPS-01`,
`FORM-01`, `STATE-01`, `LOCALE-01`, `A11Y-01`, `LIFE-01`, `PERF-01/02`.
They remain **NOT_RUN for this proposed new visual system** until matching
remote evidence exists. Historical screenshot inspection does not close them.

No new brand/font/icon-library/web/runtime dependency or copied Blender asset
is introduced by this plan. Any actual future source/asset port still needs
its own accurate revision/file/license/modification record; stylistic reference
alone must not be labelled source transplantation.
