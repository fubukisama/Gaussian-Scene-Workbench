# P0 real-window baseline evidence review

Status: **remote pixel-edge failure reproduced under the frozen oracle; normal-size visual review remains with Root**. This is a raw-artifact inspection, not a local test or a rendering fix.

## Identity and retained evidence

- [Remote baseline run40641681307](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40641681307), candidate/instrumentation commit `4f1dbb8f2ad086b3d855e112b835d6b641f81bf9`, tree `dcab400dae1d7a3b026ad18c8bafbe58a33d6bad`.
- Run source record, exact protocol and original result/log file hashes are retained in [P0_BASELINE_RESULTS.json](P0_BASELINE_RESULTS.json). Protocol SHA `c1d3f422abeac3cebb4d5e3bf2965c7989762e71a0ad45e9fb141b2bc287586e` matches the source-run record.
- The sole artifact `native-ui-clarity-baseline-40641681307` is retained unchanged under `D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307`:585files/316,938,868bytes. GitHub reports a3,405,565byte compressed archive and server digest `cd071964e0b404e7107e904429484096faf5402c66e3ffb66455ac2ad9b46cf5`; this audit downloaded via gh extraction and does not claim an independently hashed ZIP.
- Before edge evaluation, the original log records12oracle contract tests passed, native build succeeded, and20actual native-window captures completed. The only failed workflow step is `Evaluate navigation edges`.
- No local application, tests, checker, image decoding/filtering, metric recomputation, training, settings or original-data change occurred. PNG inspection is limited to signature/IHDR size/bit-depth/color-type and raw SHA; metrics below are the original remote output, not regenerated.

## Signal and validity

**14cases fail,6pass;60negative rings measured,39fail/21pass.** All failure codes are the frozen RGB RMSE/P95 limits. There are no input-contract errors, missing/invalid PNG errors, empty-ROI failures, serial mismatches or reported color-stage GL errors in these20cases.

All20raw manifests are complete, request/stage serial matches are true, `noRerender` is true, the after-painter color is valid and both source/destination framebuffer statuses are complete. Manifest/capture/painter DPRs agree; PNG physical sizes match stage sizes and the frozen logical×DPR rounding tolerance. Native-window PNG dimensions match their capture metadata. Camera/transform preservation is true throughout. Source paths are empty and source counts0: this is the owned empty-view fixture, not a user model or training project.

This is therefore **not merely an oracle schema/metadata failure**. It is a measured navigation-edge discrepancy in real Windows QOpenGLWidget frames. It still does not uniquely identify a root cause or replace inspection of the original images for the user's exact staircase symptom.

## Actual sample-count distinction

Do not substitute `context.samples` for attachment evidence:

- All20context-format sample values are0.
- All10requested4cases bind an actual color **GL_RENDERBUFFER(36161)** with `sourceColorSamples=4`, and source-bound `GL_SAMPLES=4`.
- Requested0cases use color **GL_TEXTURE(5890)**. Their `sourceColorSamples` field was not recorded and remains null/NR in the review; source-bound `GL_SAMPLES=0` is separately observed. This is not an invented color-attachment query.
- The sample-sensitive control is real: requested0fails at every measured DPR/theme. With actual4sample color attachments, DPR1/1.25still fail, while DPR1.5/1.75/2pass. At each fixed DPR/theme, the0-versus4pair has matching logical/physical size and UI scale.
- Hardware scope is Mesa/llvmpipe `LLVM23.1.2`, GL4.6Core, Mesa26.2.4. This is not evidence for the user's NVIDIA driver.

All native Windows window-DPI values are96. Qt screen and widget DPRs both follow the controlled1/1.25/1.5/1.75/2values. These are real controlled-DPR windows, **not** proof of five native OS display-scale settings or cross-monitor migration.

## Per-case ranges over its three rings

Frozen limits remain RMSE≤.06, P95≤.16, halo≤.04. The sample column is requested / color-attachment field / source-bound GL state. NR means not recorded, never context-derived.

| Case | Measured DPR | UI% | Samples req/attachment/state | RGB RMSE range | P95 range | Halo range | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| d1-s0-dark | 1 | 100 | 0 / NR(texture) / 0 | 0.078158–0.098794 | 0.182772–0.234369 | 0.011765–0.011765 | FAIL |
| d1-s0-light | 1 | 100 | 0 / NR(texture) / 0 | 0.101262–0.105462 | 0.235604–0.248516 | 0.000000–0.000000 | FAIL |
| d1-s4-dark | 1 | 100 | 4 / 4 / 4 | 0.051719–0.064911 | 0.108224–0.147092 | 0.011765–0.011765 | FAIL |
| d1-s4-light | 1 | 100 | 4 / 4 / 4 | 0.063004–0.069992 | 0.125331–0.154998 | 0.000000–0.000000 | FAIL |
| d1.25-s0-dark | 1.25 | 90 | 0 / NR(texture) / 0 | 0.085793–0.104071 | 0.190872–0.247729 | 0.011765–0.011765 | FAIL |
| d1.25-s0-light | 1.25 | 90 | 0 / NR(texture) / 0 | 0.101326–0.121657 | 0.262115–0.292414 | 0.000000–0.000000 | FAIL |
| d1.25-s4-dark | 1.25 | 90 | 4 / 4 / 4 | 0.057929–0.074502 | 0.124838–0.168478 | 0.011765–0.011765 | FAIL |
| d1.25-s4-light | 1.25 | 90 | 4 / 4 / 4 | 0.074371–0.077283 | 0.167516–0.168665 | 0.000000–0.000000 | FAIL |
| d1.5-s0-dark | 1.5 | 90 | 0 / NR(texture) / 0 | 0.074664–0.086711 | 0.168708–0.213568 | 0.011765–0.011765 | FAIL |
| d1.5-s0-light | 1.5 | 90 | 0 / NR(texture) / 0 | 0.087472–0.100568 | 0.206607–0.226812 | 0.000000–0.000000 | FAIL |
| d1.5-s4-dark | 1.5 | 90 | 4 / 4 / 4 | 0.043169–0.055096 | 0.087285–0.120775 | 0.011765–0.011765 | PASS |
| d1.5-s4-light | 1.5 | 90 | 4 / 4 / 4 | 0.054040–0.058299 | 0.118019–0.123171 | 0.000000–0.000000 | PASS |
| d1.75-s0-dark | 1.75 | 90 | 0 / NR(texture) / 0 | 0.067411–0.080422 | 0.154277–0.180439 | 0.011765–0.011765 | FAIL |
| d1.75-s0-light | 1.75 | 90 | 0 / NR(texture) / 0 | 0.083538–0.090389 | 0.189100–0.221225 | 0.000000–0.000000 | FAIL |
| d1.75-s4-dark | 1.75 | 90 | 4 / 4 / 4 | 0.036010–0.042560 | 0.079490–0.093213 | 0.011765–0.011765 | PASS |
| d1.75-s4-light | 1.75 | 90 | 4 / 4 / 4 | 0.044731–0.048839 | 0.098576–0.113406 | 0.000000–0.000000 | PASS |
| d2-s0-dark | 2 | 90 | 0 / NR(texture) / 0 | 0.062134–0.080634 | 0.136303–0.188615 | 0.011765–0.011765 | FAIL |
| d2-s0-light | 2 | 90 | 0 / NR(texture) / 0 | 0.080678–0.083474 | 0.180538–0.190502 | 0.000000–0.000000 | FAIL |
| d2-s4-dark | 2 | 90 | 4 / 4 / 4 | 0.030443–0.041085 | 0.067003–0.094378 | 0.011765–0.011765 | PASS |
| d2-s4-light | 2 | 90 | 4 / 4 / 4 | 0.036854–0.041295 | 0.085184–0.094552 | 0.000000–0.000000 | PASS |

The JSON appendix preserves every one of the60rings by case/handle/axis, raw metric values, edge/halo/exclusion counts and physical ROI, plus the20manifest/environment and40PNG hashes. Aggregate RMSE spans0.030443–0.121657; P95 spans0.067003–0.292414. Halo never exceeds0.011765:light is0, and the small dark delta is consistent with the known declared(12,15,17) versus actual clear near(12,13,14). No background fit or threshold adjustment occurred.

## Original images for immediate review and minimization

1. **First minimal case: `d1-s0-light`** — high-contrast white background, integer DPR, no model/grid contribution, halo0. Its ring RMSE0.101262–0.105462 and P950.235604–0.248516 provide a strong red signal without fractional-DPR or dark-background ambiguity.
   - [Raw after-painter PNG](D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307/d1-s0-light/captures/capture-20261028-061031-244-f231b067-4cd8-46f7-a2c0-d7992a494f6e/stages/04-after-painter-color.png)
   - [Owned native-window PNG](D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307/d1-s0-light/window-native.png)
2. **Default-MSAA/fractional control: `d1.25-s4-light`** — actual4sample color attachment but still fails. This demonstrates that “request4samples” or “actual4samples exists” alone is not complete pixel acceptance.
   - [Raw after-painter PNG](D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307/d1.25-s4-light/captures/capture-20261028-061042-324-de10ec3d-9159-404d-9ef9-60166bd46c8b/stages/04-after-painter-color.png)
   - [Owned native-window PNG](D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307/d1.25-s4-light/window-native.png)
3. **Passing real-renderer control: `d1.5-s4-light`** — same oracle can pass a real captured renderer at this combination, rather than rejecting every real input.
   - [Raw after-painter PNG](D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307/d1.5-s4-light/captures/capture-20261028-061049-797-e1c5ba94-7671-4df7-a705-688b21582374/stages/04-after-painter-color.png)
   - [Owned native-window PNG](D:/Codex/validation/gsw-ui-upgrade-20261028/baseline-run40641681307/d1.5-s4-light/window-native.png)

Preserve these originals. Crops or enlarged previews, if later needed, must be separately labeled and cannot replace normal-size review. No Blender comparator or user screenshot has been uploaded to this remote evidence.

## Typography/support boundary

All20captures are zh_CN UI. Their97or99visible widget font records report0missing glyphs for the supplied mixed-script probe; this is useful actual font evidence, not a full three-language form/layout matrix. UI auto-scale is100% at controlled DPR1 and90% at the higher controlled DPRs; it is distinct from the framebuffer rendering resolution. Do not reinterpret that90% as downsampled model rendering.

P0 navigation click/drag, real cross-screen changes, all model types, performance distributions, custom RGB/alpha feature, complete locales/dialogs and final installed package remain separate gates. The present data supports ranked falsifiable rendering hypotheses; it does not establish that the driver, font, scale policy or a particular GL state is the cause.

## Inspection notes

An initial attempt to serialize every repeated field exceeded tool-output length and was discarded without writing or changing evidence. The final review read the immutable artifact in bounded chunks and generated only these two review documents. All original585artifact files remain untouched; no checker was executed locally.

