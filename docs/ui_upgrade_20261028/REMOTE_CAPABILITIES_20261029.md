# Remote Windows / GPU capability audit — 2026-10-29

This is a capability inventory and a proposed read-only probe, **not** a new UI,
performance, NVIDIA, native OS-scaling, or full-acceptance result. No local
application, detector, GPU query, test or GUI was run. No local GSW process,
stable entry point, user data, driver or Windows display setting was touched.

## Identity and current findings

Audit observation: `2026-10-29T04:52:35.8516463Z`. The shared checkout was
`e48d535683d5d5269e422971e53c7ea104f9fd4e`. This does not bind historical
acceptance to that commit. Machine-readable findings are in
`REMOTE_CAPABILITIES_20261029.json`.

| Resource / test boundary | Actual evidence | Meaning / remaining limit |
| --- | --- | --- |
| Repository-visible self-hosted runners | Authenticated repository API returned `total_count: 0`, `runners: []` | No registered self-hosted test runner is available through this repository at observation time. This is not an inventory of the whole account or every cloud service. |
| Codex saved execution context | 34 project metadata entries: 17 local projects with local host; 17 ChatGPT projects with unspecified host; 0 nonlocal host entries | No configured nonlocal Windows execution machine was established. An unspecified/cloud project is not evidence of a usable remote GPU machine. Names, host display names and project paths were omitted. |
| Hosted Windows / software GL | Existing exact historical candidate produced native-window captures using Mesa llvmpipe | Available for bounded Qt/software-GL regression tests; not NVIDIA hardware acceptance. |
| Actual Windows 100% scaling | Historical captures report window DPI `96` | Observed only for that historical candidate. Not proof that latest main passed. |
| Actual Windows 125/150/175/200% | No such window DPI in the inspected evidence | Untested. Qt DPR values alone cannot close these gates. |
| Actual mixed-DPI cross-monitor movement | No confirmed remote monitor topology or transition evidence | Untested; two painter metadata records are not two monitors. |
| NVIDIA provider / actual resident VRAM | No confirmed authorized remote NVIDIA environment | Unknown and untested, not zero, unlimited, or passed. |
| GitHub GPU/larger-runner entitlement | Not queried or provisioned | No billing, rental, account-wide resource discovery or new runner registration was attempted. |

The live query used the repository
[self-hosted runner listing endpoint](https://docs.github.com/en/rest/actions/self-hosted-runners#list-self-hosted-runners-for-a-repository).
Only runner OS/status/busy and a narrow generic label allowlist were selected;
runner names and credentials were not emitted. No results existed to classify.
GitHub documents standard Windows VM runners separately from the larger-runner
feature that can include GPUs. That feature's existence does not establish this
repository's entitlement or current availability.
[GitHub runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).

## Exact existing native-window evidence

Inspected existing evidence, without rerunning or downloading it:

- Source `de764f0a5ef3a8e813ec38ed19e3f232e4c83fbe`,
  [run 40663272343](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40663272343).
- Root: `D:/Codex/validation/gsw-ui-upgrade-20261028/candidate-run40663272343`.
- 20 `capture.json` records: actual Windows window DPI is **96 in every record**.
  Requested and measured Qt widget DPR values are `1, 1.25, 1.5, 1.75, 2`.
  Every record explicitly identifies this as controlled DPR, not real
  cross-monitor evidence.
- Actual GL: vendor `Mesa`, renderer `llvmpipe (LLVM 23.1.2, 256 bits)`,
  version `4.6 (Core Profile) Mesa 26.2.4 (git-9a0169f2a7)`, Qt `6.8.3`.
- Representative `d1-s0-dark/capture.json` has a `1600×936` physical native
  window capture at DPR 1. That image size is not the desktop resolution, the
  monitor count, or proof of multiple physical monitors. These remain unknown.

SHA-256 evidence binding:

| Evidence | SHA-256 |
| --- | --- |
| `source-run.json` | `52d9b6cc5ad617aa72a21d5a663fa5547db9c32f548a6ba9fb65c13cd34fa841` |
| `d1-s0-dark/capture.json` | `4669e99833193b35b26af8baf4e15ebfc0bfe54a7cba8adb2015e5345e9b27a4` |
| Its `captures/capture-20261028-092641-405-b94d55a7-bc17-4460-9410-c9cc4350d0ab/manifest.json` | `74ed1ca63497e0bc1deafcb0cbffc9adfddf58e561034234525550eb2f1e759e` |

The existing `setup_native_ci_display.ps1` only chooses a primary physical
resolution from supported modes after `CDS_TEST`: preferred `1920×1080`,
minimum `1600×900`. It does not set Windows scale, create monitors, or supply a
GPU. Those are code capabilities, not an observation that every runner has
those modes. The existing graphics helper deliberately selects the pinned Mesa
26.2.4 `llvmpipe` software provider. Its successful use does not prove that a
physical GPU is absent; it proves which provider the tested process used.
Helper file hashes are recorded in the JSON.

## Prepared remote-only, read-only snapshot

New files, awaiting parent review before commit/push/manual dispatch:

- `scripts/probe_native_ci_capabilities.ps1`
- `.github/workflows/native-ui-capabilities.yml`

The workflow is **manual only**, has `contents: read`, and uses the existing
`windows-2022` runner label. It neither builds nor launches GSW/Qt. The only
created files are its isolated D-drive evidence directory, temporary compiler
files, and `resource-snapshot.json`; it installs no dependencies. It never
invokes the current resolution/Mesa setup helpers.

The script refuses non-Windows, non-CI, unrelated-repository, 32-bit, existing
output, non-D-drive output, and reparse-point output paths. It observes:

1. Native monitor rectangles/counts, with local indices instead of display
   device identifiers. `EnumDisplayMonitors` may include mirroring
   pseudo-monitors, so this is not a physical-monitor certificate.
2. Native `GetDpiForWindow` and awareness for an owned one-pixel **hidden**
   `STATIC` HWND inside each enumerated monitor. The probe temporarily switches
   only its own thread to per-monitor awareness and restores it. No visible,
   activated, moved product window is involved; OS scale and display modes do
   not change.
3. Narrow CIM video-controller model/driver candidate metadata. It deliberately
   does not label `Win32_VideoController.AdapterRAM` as real resident or usable
   VRAM.
4. Only if already available, a one-shot, five-second bounded `nvidia-smi`
   query for index/model/device-reported total memory/driver. No UUID, PCI,
   processes, power, clocks, reset, stress or allocation operations are used.
5. Process-visible CPU count and current host memory observations, explicitly
   not an effective allocation, reservation or promise that an application can
   use that amount.

Output omits hostnames, paths, project labels, device identifiers, raw
exceptions/stderr, credentials and broad environment dumps. Missing/failed
queries remain explicit status plus null unknown values. Management-visible
GPU candidates never become a runtime-usable GPU count or a successful VRAM
test. This is a task-specific schema, not a claim to implement every field of
the general resource-detector schema.

Why these distinctions matter: Microsoft documents that an unaware window
gets DPI 96, a system-aware window gets system DPI, and a per-monitor-aware
window gets its monitor's DPI. The probe therefore records awareness and an
owned valid window rather than treating any value 96 as reliable monitor
scale. [GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow).
The thread API's returned prior context supports restoration.
[SetThreadDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddpiawarenesscontext).
Pseudo-monitor semantics are explicit in
[EnumDisplayMonitors](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaymonitors).
NVIDIA supports selective CSV queries and reports unsupported values as `N/A`;
the selected management fields cannot establish GSW OpenGL/CUDA compatibility.
[nvidia-smi manual](https://docs.nvidia.com/deploy/nvidia-smi/index.html).

Snapshot success is **not** a green product gate. The snapshot does not execute
OS scale transitions, cross-monitor GSW movement, OpenGL/CUDA workloads or VRAM
measurement. `fullAcceptance=false` and promotion remains disallowed while the
related gates are open. Original performance FAIL and four INCONCLUSIVE results
are unaffected by this audit.

## Native scaling / RDP: documented possibility, not available proof

Microsoft currently documents RDP `desktopscalefactor` values including
100/125/150/175/200%, but also warns that the property is being deprecated.
`smart sizing` is client-side scaling and is not a substitute. `use multimon`
uses a client's available displays; it does not magically supply independently
scaled output hardware to a standard single-display CI VM.
[Supported RDP properties](https://learn.microsoft.com/en-us/azure/virtual-desktop/rdp-properties).

No existing authenticated remote desktop session or verified CI recipe was
found/configured in the authorized scope. No RDP service, local account,
credentials, firewall rule, display driver, virtual monitor or registry scale
override was created. We therefore cannot claim these gates are now runnable.

If an **already authorized isolated** RDP test machine/session is provided,
the exact proposed proof sequence, to review before any session/display change,
is:

1. Capture the read-only baseline first; preserve its monitor topology and
   driver/provider facts. Do not operate on the user's local Windows session.
2. On the isolated remote session only, request each documented RDP desktop
   scale with fixed 1920×1080 dimensions and client smart sizing disabled.
   A requested `.rdp` value is merely a request, not success.
3. Require an owned per-monitor-aware HWND to actually report
   `96/120/144/168/192` DPI respectively. Unset Qt scale-factor overrides for
   native-OS cases; retain exact candidate identity, actual Qt screen/widget
   DPR, native-window physical/logical sizes, fonts and screenshots. If the
   server/client ignores a request, that case is INCONCLUSIVE, not PASS.
4. Cross-monitor acceptance additionally requires at least two confirmed
   independent displays with differing actual DPI, plus the product's own
   move/`WM_DPICHANGED`/Qt-screen transition evidence. A single RDP scale value
   or two synthetic painter entries cannot replace this.
5. NVIDIA acceptance separately requires the exact GSW process to report the
   intended NVIDIA GL provider and driver with baseline/bounded-workload
   resident-memory observations. `nvidia-smi` inventory alone is insufficient;
   an RDP session could still select a different rendering provider.

This sequence is a proposal, not authorization to configure RDP or acquire a
machine. No resource purchase or account-wide discovery is needed for the
first read-only hosted-runner snapshot.

## Consolidated environment question (for parent, not sent to user)

是否提供并授权一台隔离的 Windows NVIDIA 远端测试机，具备真实
100/125/150/175/200% OS 缩放和至少两个可独立缩放的显示输出，用于一次
有界 UI/GPU/VRAM 验收？如暂无此环境，NVIDIA、原生非 100% 缩放和跨屏
保持未验，不激活本地候选。

No request for passwords/tokens is appropriate. If provided, access must use
the product's approved connection/secret mechanism. Local RTX hardware is not
an authorized substitute for remote acceptance.

## Static validation and method reference

Local validation is limited to PowerShell AST parsing, JSON/YAML parsing, and
file/diff checks. The probe, hidden HWND creation, CIM calls, and `nvidia-smi`
have **not run locally or remotely** in this change. No commit/push/dispatch has
been performed by this capability subtask.

The `get-available-resources` skill materially guided the distinction between
inventory, effective resource limits and runtime usability, along with
redaction and unknown-value handling. Its requested method reference was
verified against the current arXiv record on 2026-10-29 (record currently v2,
revised 2026-09-02; no published journal reference shown):

Kassis, T., Agarwal, V., He, Y., Patel, D., & Brueckner, A. M. (2026).
*Scientific Agent Skills: A Library of Procedural Knowledge for Research Agents.*
[arXiv:2609.00065](https://doi.org/10.48550/arXiv.2609.00065).
This method citation is not evidence of GSW correctness or remote hardware
availability.
