# c3e Native Windows — scoped Qt / package-readback evidence

Root follow-up at **2026-10-29 07:18 UTC**: the exact same c3e run40823880544
has now completed with **all three jobs SUCCESS**, including CodeQL123282342250.
The original 06:49 audit snapshot below is retained, not rewritten. This closes
that exact candidate's full CI gate, not later-source/full-UI/hardware/performance
or local-delivery acceptance; no additional large package was downloaded.

Audit snapshot: **2026-10-29 06:49 UTC**. Status is
**QT_JOB_SUCCESS / UPSTREAM_SUCCESS / CODEQL_PENDING_AT_OBSERVATION /
NOT_FULL_ACCEPTANCE / NOT_DELIVERED**. This is a historical exact-candidate
record, not acceptance of later `b646ab5`, updater integrations, or latest HEAD.

## Exact source and remote scope

- Source: `c3e667a249e0de1b4384aa3d97d29172e08a474b`.
- Tree: `59d195a88b50ee9e2685c81998f5a74f302dd0a6`, independently resolved locally
  and also recorded by the remote grid trace request.
- [Native Windows run 40823880544](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40823880544).
- [Qt job 123282342457](https://github.com/fubukisama/GaussianSplattingWorkbench/actions/runs/40823880544/job/123282342457)
  is SUCCESS. Upstream job `123282342524` is SUCCESS. CodeQL job
  `123282342250` was still in progress when inspected; the overall run was not
  reported complete.
- Current canonical repository is `fubukisama/GaussianSplattingWorkbench`, same
  immutable repository ID `1286130219`. Original build paths inside the
  receipts still contain `Gaussian-Scene-Workbench`; they were not rewritten.

## Results actually present in the direct Qt log

| Boundary | Observed result | Limit |
| --- | --- | --- |
| Original graphics bootstrap | 4/4 PASS, 247.21 s total | Same original CTest selection; not a new benchmark. |
| Original complete native CTest suite | 175/175 PASS, 1863.28 s total | Exact c3e Qt job only. |
| Separate `NativeInstallationTests` | 10 QtTest rows PASS, 0 failed/skipped | Includes init/cleanup rows; owned fixture behavior, not this user's installation. |
| PowerShell managed-installation adapter | Actual invocation and terminal `Native managed installation contracts PASS.` | No per-contract JSON was retained in these two artifacts. Older focused 13-contract JSON is not relabelled as this run. |
| Actual product upload/download readback | PASS with `baselineCompared=true` | Remote staging-to-downloaded-byte comparison, not a local product-package validation. |

The real readback receipt records **3044 files, 1351 backend records, 1583
source records, and 28 hidden files**. Its source identity is c3e. EXE SHA256 is
`91e21f70a869e3a6fa78db27ffb5dfb97eeaef5a5be7548c741408a231151b8e`,
matching the grid trace request. The EXE hash here is read from those two
source-bound remote receipts; the EXE itself was **not downloaded or rehashed
locally** in this audit. Both staged and readback receipts say
`applicationExecuted=false`, `authenticityClaim=false`; only the readback has
`baselineCompared=true`.

## Bounded grid trace was actually collected

The small full-test artifact contains the actual `trace-request.json`,
`grid-phase.jsonl`, original `bootstrap.log`, and original temporary PNGs in
`_temp/gsw-grid-bootstrap/`. The trace is **present**, not inferred from a green
test. It has **37 rows, 37 unique phase IDs, contiguous sequence numbers, and
nondecreasing elapsed microseconds**, from `main.entry` to `event-loop.end`.
There is no `grid.timeout-detected` phase in this one successful invocation.

The request binds c3e/source tree/run/EXE, retains CTest **15 s** and internal
grid **10 s**, and records `newCapturesAdded=false`, `retriesAdded=false`,
`localExecution=false`, and `applicationInvocations="original CTest selection only"`.

Selected observations from this **single diagnostic invocation**, not a
performance/no-regression result:

| Phase boundary | Elapsed / interval |
| --- | --- |
| MainWindow constructor | 86,774 → 4,234,939 µs; interval 4,148,165 µs |
| Existing `show()` | 4,235,080 → 4,976,742 µs; interval 741,662 µs |
| Internal grid deadline starts | 4,976,918 µs after `main.entry` |
| First grid poll / ready frame | 5,417,327 / 5,417,391 µs |
| Final framebuffer | 5,829,706 → 5,853,898 µs |
| Existing grid verdict / event-loop end | 5,888,905 / 5,889,167 µs |
| CTest's original grid test duration | 6.00 s |

These facts show that this run's old-name opt-in guard did enable the trace.
They do not validate the later immutable-ID guard, nor prove the root cause of
historical ac5/6bfc 15-second timeouts. No old failed binary was replayed and no
deadline, rendering operation, capture count, or assertion was changed for this
audit. A successful trace is not a timeout root-cause fix.

## Exactly two small artifacts downloaded once

Local owned evidence directory:
`D:/Codex/validation/gsw-ui-upgrade-20261028/full-c3e-run40823880544-evidence/`.
Authenticated metadata verified both artifacts belong to run40823880544/c3e
before download. Each ZIP's complete SHA256 was checked before fresh extraction.

| Artifact | Bytes | ZIP SHA256 |
| --- | ---: | --- |
| `12878186229`, `native-full-test-evidence` | 3331239 | `6d4530a9e9db17398c6fe9253f4bfc437c5857baaa5ddc89a5c1944365e1a070` |
| `12877949285`, `native-artifact-roundtrip-receipts` | 1021 | `e98f0748dd3719d6d13d8db1889a74744a7ff645e9eb9246bc348fde840893c4` |

Selected extracted evidence hashes:

| File | SHA256 |
| --- | --- |
| `trace-request.json` | `c79f834ef379db9d614004bcb0abedf45e41a97cfbd7c1f6a340383f8a3a429d` |
| `grid-phase.jsonl` | `e1cff4ad7ee6e38a0f4d604fc577715ae106d501f49d262fb42dd926aeb0620a` |
| `bootstrap.log` | `e866db43968b4287287902d5e65468c86fd1270145659ec7b5047f0f68d546f9` |
| `full-tests.log` | `5bcc32622a930a8ccf3290fc13a9e98e05318e99e0b8ee09ad47f0d6c7e6408e` |
| `native-managed-installation.log` | `010146013f6f8c38f396048836a71647c5e44b769ca928aba730548a775a4d5e` |
| `gsw-native-artifact-roundtrip.json` | `057dc6254e5bbc942a3f86ea300d33694f4bfe071ee11817d784cd3ba1785ac2` |
| `gsw-native-artifact-staged.json` | `f440d20d8594f8e87897d6b2593f7762a3922b0f34b7eecacee618729f1b7c87` |

The direct Qt log was collected once, only after verifying it did not already
exist, at `D:/Codex/validation/gsw-ui-upgrade-20261028/full-run40823880544-native-direct.log`;
SHA256 `d3b6699c7cd0b6307f947c20cdd8a4c846fbadbf24a091e763e9a91511c8c413`.

No payload, Setup, or 106-MB upstream artifact was downloaded. No GSW application,
installer, test executable, CTest, verifier script, compiler, publisher, or
training process was run locally. No user data, active installation, stable
entry point, settings, shortcut, or running session was changed. Later source
packages and full UI/hardware/performance acceptance remain separate gates;
`fullAcceptance=false`, `promotionAllowed=false`.
