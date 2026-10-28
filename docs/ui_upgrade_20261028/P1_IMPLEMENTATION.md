# Shared native visual system candidate

Status: IMPLEMENTED / RUNTIME_ACCEPTANCE_PENDING / NOT_DELIVERED.

The55shared AppTheme color roles now drive QPalette and QSS. Light/dark actions
share one blue family; neutral hover, tinted checked, pressed, keyboard focus,
disabled, warning, danger and successful-result roles remain distinct. Layout
labels retain transparent backgrounds. Existing body/CJK/Latin font families,
scale/time policies, saved settings, field surfaces and resize hit areas remain.
Tabs reserve their selected border in every state, and focus is an inset cue
rather than a geometry-changing border. No new animation/repaint timer exists.

WorkbenchIcons retains the pinned Lucide vectors and semantic IDs. Tinted
selection uses readable ordinary ink; an explicit OnAccent surface supports
white ink only for solid action fills. The actual DPR-aware QIcon engine is
unchanged. Widget and viewport action roles now agree; XYZ/domain colors and
the actual white/dark scene clear values are untouched. Task running versus
success colors are distinct, without changing task state.

The inspector's existing label/value forms wrap long rows instead of forcing
ordinary labels to determine horizontal width. Original values, precision,
copyability and transforms remain unchanged. ModelExport/ResourceBudget dialogs
have real Qt meta-object identities so evidence does not misidentify them as a
generic QDialog; no request behavior changes.

Move/Rotate/Scale/Combined use the same bounded native-DPR vector coverage layer
as navigation, without altering their actual geometry, depth ordering, picking,
constraints, units, cancel/commit or undo/redo. A streamed full-content display
key is limited to256KiB serialized input and32retained hash bytes. The visible
tile has a logical clip matching the renderer's stem/face depth mask. Extreme
close-ups exceeding the8MiB/4096edge bound retain the original vector fallback;
this is explicit, not a promise that every possible close-up uses a raster tile.

Added independent public palette/icon/font/alpha/focus/geometry contracts and a
real-viewport test that discovers actual painted handles through pixels and
public hover feedback, then uses real mouse/key input. These tests are awaiting
remote execution. No local automated test or application was run.

P2 evidence is opt-in for remote isolated synthetic smoke processes only. It
does not show/raise/activate windows itself. It records already-active exposed
owned native windows with an occlusion guard, original PNG/hash/DPR and visible
widget-default font/glyph facts. Missing/short-lived/occluded surfaces are
SKIP/NOT_RUN. Existing deep request and lifecycle assertions remain responsible
for business behavior; screenshots do not replace them. New real-Windows modes
reuse the old initializer/runtime tests while retaining their default offscreen
contracts and all old assertions. Nested trust prompts/custom-painter font roles
and native OS providers remain separately identified, not falsely covered.

Local compilation and three-language catalog validation passed; actual focused
style/forms, all-model regressions, language-crash diagnosis, valid paired
performance, complete native CI and source-bound managed delivery are still
required. No new CUDA/Torch/training algorithm/default or user-data change.
