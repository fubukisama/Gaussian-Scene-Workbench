# P0 candidate017f78c: split result, not a delivery

Remote run40649920338 used source017f78cfbf0c35873fccc7029a6a7a056707cacb.
The unchanged independent edge checker measured20real Windows captures/60rings:
**20/20cases PASS**, max normalized RGB RMSE.0264389274, maxP95.0621412327,
maxouterhalo.0117647059. Frozen limits remain.06/.16/.04. This reverses the
baseline's14/20failures, including all requested0sample cases. Normal-size raw
d1-s0-light was reviewed read-only: hollow circles and diagonal strokes are
visibly smoother, without changing logical layout or the white background.

The run overall **FAILED**, and the candidate is not installed/promoted.
ui_overlay_raster and navigation_gizmo passed; actual zh_CN navigation and
Blender-presentation tests passed. The new20cycle lifecycle test crashed at7.68s,
and en_US/ja_JP navigation/presentation tests crashed at roughly1–2seconds
(including0xc000041d). This is not a30second timeout. All20pixel captures used
the default Chinese locale; they cannot establish three-language acceptance.
Original images/results and the direct failure log remain onD under
`validation/gsw-ui-upgrade-20261028/p0-candidate-run40649920338`.

Ranked follow-up hypotheses are separated from facts. Official Qt6.8.3 source
(c07c2d5a527a644d36e7853d55132ae38921682f) distinguishes raster A32/native
alphaRGB glyph rendering from GL's possible A8 path. The new cache also invokes
QFontInfo on every miss, whereas the old path only did so during diagnostics.
Two painters on different devices is not itself a Qt contract violation, and
glyph caches are context-keyed. Trace phases and first-chance fault context are
therefore needed before choosing a font, metadata or compositing fix.
A remote-only grayscale-text treatment is diagnostic, not a promoted default.

Paired performance run40649923711 also **FAILED its evidence contract**, not a
valid measured regression verdict. All24processes have receipts;18exit0 and
6DPR2empty processes areINCONCLUSIVE(not fully exposed). Actual idle counters
were zero where the raw checker rejected waits shorter than600/1000ms. Qt timer
requests are not a monotonic minimum-duration guarantee. Preserve those results;
fix the producer equally in both arms, record any common QA overlay explicitly,
and retain every frozen budget. No valid three-round comparison was produced.

All local activity was static reading, PNG viewing, translation validation and
compilation/linking. No local app/test/window/installer/training was executed.
No user project, photograph, model, checkpoint or running session was changed.
