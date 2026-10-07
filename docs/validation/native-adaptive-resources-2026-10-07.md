# Native adaptive model resources — 2026-10-07

Desktop branch: `agent/native-desktop-0.3`. This change affects the native mesh-loading/display path, not the legacy HTML branch or training-worker parameters. Original research assets are read-only.

## Policy and scope

Automatic mode uses observed available RAM/commit and GPU headroom, retaining 10% of that headroom with minimum reserves of 512 MiB RAM and 256 MiB GPU memory. Manual model ceilings are live and persistent. The policy is GSW's own, informed by the separation of cache, interaction-quality and display-acceleration settings in Blender, CloudCompare and Metashape; it is not an attribution of their allocation algorithms. See [Resource budgets](../RESOURCE_BUDGETS.md) for primary documentation and all three UI languages.

Managed mesh display data and pending mesh imports/page reads share a thread-safe reservation ledger. A worker's lease survives removal/stale requests until its actual work ends. Full mesh admission uses estimated CPU peaks and actual mesh-buffer/atlas GPU storage, not an absent point buffer. Geometry paging does not page texture atlases. Unknown GPU probes are labeled explicitly; manual ceilings are never presented as measured free VRAM. Independent packed-Gaussian, depth-order and SH GPU buffers are not part of the manual mesh ceiling.

## Executed public regressions

| Boundary | Evidence | Result |
|---|---|---|
| Actual settings entry and immediate controls | Entry RED 3.02s then GREEN 3.30s; controls RED 3.09s then GREEN 3.42s | Passed |
| Live paged-to-resident transition | Grid1024 at 64 MiB → 512 MiB; source, camera and transform retained; independent two-triangle reference 377,503 interior pixels, 100% coverage; GREEN 4.29s | Passed |
| Unknown/manual wording and narrow UI | Real dialog in zh_CN/en_US/ja_JP, light/dark and 150% narrow layout; RED 6.83s then GREEN 6.75s, fifteen screenshots; three worst-case images visually reviewed | Passed |
| Atomic ledger/lifecycle semantics | Twelve public behavior cases plus Qt init/cleanup; known zero vs unknown, concurrent admission, stale observations, component release, worker lifetime, shortfall/commit bounds | Passed |
| Shared serial and same-batch imports | Grid768 red/blue, total 64 MiB GPU; 66,865,536 managed bytes, leases returned; both actual colored surfaces visible (292,640 / 122,739 pixels) | Passed |
| Removal and replacement admission | RED 25.61s: old page cache occupied 66,410,496 bytes and starved a 42,541,104-byte replacement. Physical release plus fresh atomic admission; shared GREEN 6.73s includes automatic remaining-model promotion | Passed |
| Indivisible texture/geometry page progress | Portable UV256 +4096-pixel atlas, 94 MiB GPU. RED 19.95s: atlas present but zero surface pixels. One page may exceed the prefetch quarter only if it fits the real shared GPU allowance and CPU lease. GREEN 4.81s: 139,228 real surface pixels, 94,590,292 GPU bytes below 98,566,144, leases returned | Passed |
| Minimum drawable page and live recovery | RED 9.16s at 86 MiB: atlas alone fit, successful import was still blank. Actual root-page admission now returns a localized resource error and all leases; only raising to 94 MiB recovers the same source and 139,305 real surface pixels. GREEN 2.73s | Passed |
| Cache-first budget reduction | Actual two-model 80→64 MiB public settings. RED 7.65s: resident model unnecessarily reloaded and swapped residency. Trim pages first and use the captured probe target; GREEN 11.03s: 76,603,584→66,996,288 GPU bytes, no reload/loading event, original resident stays complete and both surfaces visible | Passed |
| Texture alone exceeds the ceiling | Portable 4096-pixel atlas at 64 MiB: explicit failure, original dimensions retained and all leases returned; only raising to 94 MiB restores a real textured frame | Passed |
| Genuinely insufficient-budget demotion | Complete grid1024 exceeds 64 MiB; live 512→64 MiB produces nonempty paging within RAM/GPU budgets, source/camera/transform unchanged; explicit clear returns worker/page leases | Passed |
| Above the former face-count limit | Grid1600: 2,563,201 source vertices, 5,120,000 valid faces/triangles. Complete display 1.732s, actual GPU 184,473,648 bytes / managed RAM 399,782,532 bytes, 384,027 colored pixels, resident 1 / paged 0 | Passed |
| Language change between worker completion and UI delivery | True RED 24.69s: English worker error delivered after switching to Japanese could not be retried at the raised cap. Locale-independent admission outcome and typed failure now deliver the current Japanese UI error; only raising 86→94 MiB recovers the same source. GREEN 5.12s includes later 64→94 atlas recovery | Passed |
| Adaptive ASCII short records and long-row boundaries | Public loader RED: 4,003 records in a 32,175-byte file took 13,632ms. Bounded 8 KiB segments admit cumulative scratch before growth; GREEN 3ms, plus 36 KiB cross-segment blank/CRLF/final-EOF coverage. Original ASCII SuGaR mesh complete display 193ms with 100% reference coverage | Passed |
| Unknown-count resident mesh HUD | Real eight-vertex/twelve-face cube RED 3.59s: source signal was correct, but rendered digit matched zero (IoU 0.955224) rather than eight (0.595506). Independent source-vertex metadata, without changing Gaussian preview semantics; GREEN 3.80s, eight IoU 0.945578 versus zero 0.608187 | Passed |

Final targeted regression: all nine resource tests passed in 34.37s, including twelve ledger cases plus Qt setup/cleanup. Cache-first reduction also passed while a bounded 25,158,528-byte background page-read lease was active; the explicit clear then returned every managed allocation and reservation to zero.

The first grid512 shared fixture was invalid as a limit oracle: combined storage was only 37,847,136 bytes and legitimately fit 64 MiB. The later wait-helper double evaluation was a harness issue, not a production failure. Neither is counted as a product RED. Tests do not intentionally exhaust system memory or VRAM.

## Initial regression and candidate installation

- Complete native CTest suite: 74/74 passed, 411.54s.
- Native translation catalog: 1,338 UI messages and 1,369 entries complete in zh_CN, en_US and ja_JP; validator regression 16/16 passed.
- Python worker regression: 154 executed, 146 passed and 8 conditional skips, 1.717s.
- Package app-local launch, backend staging, 73-file Python syntax gate and isolated import preflight passed.
- Installed in `D:/Apps/GSW/native/0.3.1-adaptive-resources-oct7`; every one of 1,365 package files was checked against its installed SHA-256. Build/package/installed executable SHA-256: `C1B6E55312019FAA5FF4132AE712E4D7193A567D45B1EB7064AF2C9A055EF388`.

## Installation findings and closed regressions

- Initial installed matrix was 27/29: ASCII SuGaR light/dark source loading timed out; every other case passed and all original source hashes remained unchanged. Investigation identified per-record 256 MiB temporary allocation in the matching [Qt 6.8.3 readLine implementation](https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/io/qiodevice.cpp#L1335-L1371); a bounded sixteen-record diagnostic measured 46ms versus 0ms for the normal incremental overload. The minimal segmented-read fix passed the permanent public loader regression and both original-model installed viewport gates.
- Visual inspection found zero source vertices in the resident mesh HUD when `setScene(path, 0)` supplies an unknown initial count, despite correct parsed vertex counts. The public glyph regression now passes after recording parsed mesh counts independently; this is not claimed to have affected every MainWindow import path.

## Final post-fix acceptance

- Complete native CTest suite: **75/75 passed**, 407.82s, including translation catalog/validator regressions, public resource controls, unknown-count HUD, ASCII loader and all prior native tests.
- Python worker regression: 154 executed, 146 passed and 8 conditional skips, 1.729s. App-local package launch, dependency/backend staging, 73-file Python syntax gate and isolated import preflight passed.
- Fresh final installation: `D:/Apps/GSW/native/0.3.1-adaptive-resources-oct7-r2`. All 1,365 package files matched installed SHA-256; build/package/installed executable SHA-256: `1B5053C58C66A3062931D6D5AAC7F0B4E09FC582BDEDC8D8416FE77ADC7FB527`. Prior installations were not overwritten.
- Installed public matrix: **30/30 passed**. Seven real mesh sources each passed in light/dark themes; two GS2Mesh readiness cases, live settings, three-language/narrow UI, rendered HUD, full/paged transitions, shared serial/concurrent imports, texture-page progress, minimum-page rejection/recovery, cache-first reduction and the 5.12-million-triangle case passed. No intentional hardware exhaustion was used.
- Original 2DGS mesh: complete display 1,089ms light / 1,076ms dark, 1,343,524 source vertices and 2,594,994 valid triangles. Original Metashape texture mesh: 1,274ms / 1,303ms, 525,238 vertices and 1,049,902 triangles; original 8192×8192 texture unchanged. ASCII SuGaR: 174ms / 172ms, 47,621 vertices and 91,099 triangles. These are this-machine test-run measurements, not cold-cache guarantees.
- All fourteen real continuous-surface cases retained 100% of their corresponding reference interior pixels. Final large fixture displayed all 5,120,000 triangles in 1,755ms, with 399,782,532 managed RAM bytes and 184,473,648 GPU bytes, zero pending reservations. Default-policy full admission and controlled low-budget paging are tested separately.
- Read-only source hashes remained unchanged. Original Metashape/2DGS light framebuffers were visually reviewed: complete reference geometry, accurate source counts and clean text. Existing application processes were not stopped; only test-owned processes were managed.
- Only `Gaussian Scene Workbench Native.lnk` was retargeted. Target, working directory and icon persistently point to the final D: installation, existing arguments are preserved, actual launched executable path matched, exit code was zero and no test-owned application process remained.

Local evidence: `D:/Apps/GSW/validation/adaptive-final-r2-build-oct7.log`, `D:/Apps/GSW/validation/adaptive-installed-oct7/20261007T100630Z-4a777a60/report.json`, and `D:/Apps/GSW/validation/adaptive-shortcut-oct7/report.json`. Desktop-branch publication and exact Git-tree verification are recorded by the scoped branch history and final handoff, separately from runtime test results.

## Remaining capability boundaries

The renderer is shared by imported meshes, 2DGS TSDF/OpenMVS and 3DGS SuGaR/GS2Mesh outputs. Reduced GS2Mesh fixtures test import readiness only; dense continuous-surface generation quality remains unverified. This work is not a new continuous-surface LOD algorithm, a training resource scheduler or optimizer-state resume. Sampled paging can still be incomplete when exact visible geometry cannot fit.

The low-budget demotion fixture can continue bounded page prefetch while visible. Its acceptance is nonempty geometry, budget and pose preservation, followed by complete lease return on clearing; it is not proof of a fully settled continuous-surface LOD at that cap. An initial demand for zero prefetch leases while the model remained displayed was a test overconstraint, not a production-memory-leak result.

Actual driver texture-allocation failures are reported, but this change does not add automatic atlas retry for that separate driver-error path. Budget-admission failures do retry after the ceiling is raised. Hardware allocation exhaustion was not deliberately injected; actual driver failures may still require reimporting the model.
