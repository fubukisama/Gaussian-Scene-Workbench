# Archived native UI performance reanalysis — 2026-10-29

Status: **ARCHIVED REANALYSIS COMPLETED / INCONCLUSIVE / NOT FULL ACCEPTANCE**.
Both remote runs replayed the same 24 original receipts. The result is 45
comparisons: **41 PASS, 4 INCONCLUSIVE, 0 FAIL**. This is a contract/provenance
reanalysis, not new timing, a blanket “no performance regression” claim, or
acceptance of the current product binary.

## Executed analysis identity and retained first failure

| Remote run / exact source | Job | Observed outcome |
|---|---|---|
| [40821150392](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40821150392), `0d3f381746726835b38e7c0709f985f021f2593c` | [123273671106](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40821150392/job/123273671106) | 26 live + 13 archive CLI contracts OK; all reanalysis assertions completed; job FAIL due to PowerShell propagating the expected checker exit code 3 |
| [40821404151](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40821404151), `afe668eb8cacaf6b06ba0ae15794467584df8eab` | [123274473165](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40821404151/job/123274473165) | 26 live + 13 archive CLI contracts OK; same reanalysis result; job SUCCESS after an explicit final `exit 0` for completed replay |

The second source change only fixes the workflow's completed-INCONCLUSIVE exit
handling: structural/numerical FAILs still throw before that exit. No evaluator,
threshold, noise rule, receipt, original result, or timing producer changed.
The first failed run and direct log are retained; it is not relabelled SUCCESS.

Both `reanalysis.json` files have SHA256
`115e5ef9f79f6c4d1a1a46a0269cfd8caa9500a971350f2b8147e81ce7c2b3b1`
and retain 24 raw summaries, zero failures, 45 comparisons, and exactly the four
observed original medium INCONCLUSIVE metrics. Identical analysis bytes do not
make their run/source identities interchangeable: each request receipt binds
its own analysis commit/run to the same checker and adapter bytes.

| Binding | Run 40821150392 | Run 40821404151 |
|---|---|---|
| Evidence artifact ID | `12874064675` | `12873764727` |
| Request receipt SHA256 | `1857a9bbe82df57ea1687412488d94d21d3328b1563f7129a693256830d2bc16` | `f45c9111494fc2ba0d7fdb308aa02fcb1224a3874b2bda2fd8aef13beac822b4` |
| Direct job log SHA256 | `6b0dc5255d150edc6e0b9f3e4b63f392b6b24246d73d8087dd095ff61822e514` | `e886731d370ad8e68aee903bd337da421d62b95c44f0a8a1cb5374bc27220b95` |

Shared checker SHA256:
`32f67fb57d9be58f93d914f1a8fbdd5148e3d849d13f7d9b6193c84c344cfe53`;
archive adapter SHA256:
`8529ad2a0644a16c326f5168880b8d3777a2216e008f5c07e099c804c5aae4f7`.
Read-only local copies and direct logs are in
`D:/Codex/validation/gsw-ui-upgrade-20261028/performance-reanalysis-run40821150392/`
and `.../performance-reanalysis-run40821404151/`, with each payload in `evidence/`.
No checker or test was rerun locally and these artifacts were not redownloaded
for this documentation update.

## Immutable input and distinct new identity

The sole input is the original artifact `12806465500`,
`native-ui-performance-40663280091`, from
[run 40663280091](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40663280091).
Its server ZIP SHA256 is
`80eb95e991828f9ed733539f981925b1bc014323d4f475e443068473acbd3b86`
(6,646,944 bytes). Baseline production commit:
`b0e9fae5c53156b74e247e9c56ab3d0bd1e4a0ff`; original candidate commit:
`de764f0a5ef3a8e813ec38ed19e3f232e4c83fbe`.

The manual `Native UI Archived Performance Reanalysis` workflow verifies these
fixed GitHub artifact/run metadata values and the exact downloaded ZIP digest.
Each replay downloads this artifact once, verifies ZIP CRCs during safe extraction,
and uses all 24 original receipts. No GSW/measurement process is started and no
timing is repeated.
The new result and request receipt carry the new analysis run/source/checker
identity; they do not rename the original source or promote a newer product.

The original `results.json` remains **FAIL**, SHA256
`3831ebe7393f1b2ae89fbae000efb9cefa54fdac9fc67d4e1bb8163855b3f9f4`.
It records the original `light-dpr1` cross-process `3dgs/model` mismatch and four
`medium-dpr1` cadence/noise INCONCLUSIVE metrics. Its original status/failures
are retained separately in `previousResult`, never overwritten or reclassified.

## Corrected contract, unchanged measurements and numerical policy

The existing fixed evaluator separates process-lifetime upload totals from
cross-process model identity. Independent fresh processes may have different
preparation totals; every raw before/after value remains in the result.
Within each measured operation the complete raw model state must still be
exactly unchanged, including all three upload counters. Geometry, renderer,
SH/fidelity, fixture bytes, camera sequence, environment, resources, timing
windows, deadlines, and idle-zero-work checks are unchanged.

An old public CLI test contradicted that contract: it added the same `+1`
attribute/order total to both boundaries in 3DGS and 2DGS, then expected a
cross-process failure. The original input scenario is retained, but its name
and assertions now require raw totals `2` at both boundaries and zero operation
deltas. Separate After-only attribute, SH, and order upload tests remain hard
FAILs. This is a contradictory QA-contract correction, not threshold relaxation.

Archived and live paths call the **same** `validate_process` and group/numerical
comparison evaluator. Only archive-contained receipt/fixture reads are relocated
from the original absolute root. Fixtures are actually re-hashed and their
headers/counts checked. No arbitrary host path, basename search, source-tree
fabrication, or EXE fallback is allowed. The live CLI still requires real source
trees, actual EXE digests, and common-overlay production-byte verification.

The frozen acceptance and plan files are not edited. Their SHA256 values remain
`fd80215e7b0bcb48ed28b7c70d1bc7675ddb2e8b18d8cd034f25b5fa27f46376`
and `2e8046da5cfd68ad8a047bdee8ffac7501f6cf51d5058d4f976faafa7ae5bff6`.
The original four medium noise outcomes must remain INCONCLUSIVE. Correcting
the light group can expose previously unevaluated legitimate timing failures
or noise; every such outcome must also be retained. There is no golden target
of “no FAIL” or “exactly four INCONCLUSIVE”. A successful workflow only means
the reanalysis completed; its JSON verdict remains authoritative.

## Provenance limitation

The original artifact does not include its source trees or built EXEs. The new
adapter imports the original live checker's source/binary verification record
only after binding the original result to the unchanged source receipt,
overlay/diff receipt, and pinned ZIP. It explicitly emits
`currentSourceAndBinaryFiles: NOT_REVERIFIED` and the import status. Original
source identity records and EXE digests are historical verified identities,
not newly computed file hashes. No new binary, GPU, hardware, installation,
visual, or full acceptance is claimed (`fullAcceptance: false`).

Generic CLI caller pins alone establish byte identity, not authenticity; the
dedicated workflow supplies independent GitHub metadata checks. Synthetic CLI
contracts use their own explicit test-only pins, never the original production
identity. Modified raw receipts, fixture bytes/counts, source labels or source
receipts, After-only uploads, bad ZIP pins and traversal entries are rejected.

## Execution boundary and remaining gates

Locally: source/docs edits, static parsing, and read-only file/receipt/digest
inspection only. No checker, automated test, application, GUI, GPU command, or
benchmark ran. The remote workflow used Python 3.12 and read access to Actions
artifacts; it did not build Qt/GSW or invoke CUDA. Both remote requests and results
record `applicationExecuted: false` and `newMeasurementsTaken: false`.

The contract correction removes the original cross-process lifetime-counter
identity failure, but does **not** establish an overall performance PASS: four
medium cadence metrics remain INCONCLUSIVE under the unchanged frozen baseline
noise policy. No retiming or selection of favourable rounds was used. Numerical
PASS comparisons are confined to these original, source-bound scheduling
receipts; they are not new measurements of the latest main/EXE.

The original Windows software-GL slice cannot close user NVIDIA hardware,
cross-monitor DPR, GPU milliseconds/VRAM, remaining full UI/installation, or
visual-quality acceptance. This reanalysis cannot activate the local version,
change a stable launcher, or make fullAcceptance true.
