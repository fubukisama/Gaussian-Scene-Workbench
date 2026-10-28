# Font metadata and small-screen repair candidate

Status: IMPLEMENTED / COMPILE_ONLY_PASS / REMOTE_REPAIR_NOT_RUN.

The six real crash contexts in40656943272 all identify
`QFontInfo::exactMatch()` from navigation diagnostics. The grayscale treatment
did not fix any of the three failing tests. The product repair therefore leaves
font families, fallbacks, sizes, weights, style strategy, alpha blending and
actual text rendering intact. A shared `fontResolutionEvidence` helper records
safe actual family/size/weight facts, and explicitly records `exactMatch:null`
with `exactMatchQuery=not_queried_qt683_multifamily_crash`. It does not fabricate
a match boolean. All product call sites of the unsafe predicate are removed.
The separate missing-glyph,17-glyph, physical-size and original behavior gates
remain unchanged.30real-font regression rows cover Chinese/English/Japanese
fallback lists,5DPRs and point/pixel font roles without calling the unsafe API.

Foundation40656947039 independently passed AppTheme28 and UiRoles151 assertions,
but all English/Japanese actual viewports failed. Chinese oldI passed all5DPRs,
yet native screen captures at1.75/2were refused: recorded client sizes were
1009×620 and940×620, exactly the production hard minimum. The oldI test does not
enlarge the top-level window. A theoretical fitted size could not override the
actual QWidget minimum of620logical pixels.

The new shared minimum policy preserves the940×620design where it fits and
bounds it by actual logical screen availability, nonnegative native frame
edges and a16logical-pixel safety gap where it cannot fit. Restore/explicit-fit
and coalesced show/screen/DPR events use the same policy. Normal windows that
fit are not moved; intentionally spanning windows are not snapped merely for
moving between screens. Maximized/fullscreen/minimized state and saved docks
are retained. This does not shrink fonts, hide actions, change camera/model
state or loosen the native occlusion guard. The isolated performance harness
retains its separately frozen640×360geometry and is not readapted by this policy.

The surface checker now continues indexing valid evidence and all16coverage
gaps even after behavior failures; its overall result remains FAIL. It accepts
the explicit unqueried font fact only with the exact reason and complete font
fields; unknown reasons, fake booleans, missing glyphs or invalid image metadata
still fail. This improves reporting, not tolerance.

Local v9 compilation/linking passed the application and all affected test
executables. Only static validation and compilation ran locally, never a test,
application, installer or GUI. Actual repair, high-DPR screen visibility,
four-mode input, valid paired performance, complete native CI and managed
installation remain separate remote/delivery gates.
