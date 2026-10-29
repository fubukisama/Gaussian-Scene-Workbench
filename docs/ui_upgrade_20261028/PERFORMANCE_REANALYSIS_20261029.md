# Archived native UI performance reanalysis — 2026-10-29

Status: **PREPARED / REMOTE CONTRACTS AND REANALYSIS NOT RUN**. This file records
the candidate protocol, not a new performance PASS or full UI acceptance.

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
It downloads this artifact once, verifies ZIP CRCs during safe extraction, and
uses all 24 original receipts. No process is started and no timing is repeated.
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

## Execution boundary and remaining gate

Locally: source edits and static AST/YAML/JSON parsing only. No checker, automated
test, application, GUI, GPU command, or benchmark is run. The workflow needs only
Python 3.12 and read access to Actions artifacts; no Qt/build/CUDA dependency.
Remote execution is pending root review, commit, push, and one manual dispatch.
If the original artifact has expired or any pin differs, stop with that direct
failure; do not replace it with regenerated fixtures or fresh measurements.

The original Windows software-GL slice cannot close user NVIDIA hardware,
cross-monitor DPR, GPU milliseconds/VRAM, remaining full UI/installation, or
visual-quality acceptance. This reanalysis cannot activate the local version,
change a stable launcher, or make fullAcceptance true.
