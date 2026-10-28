# Native UI upgrade: CI and delivery baseline

Read-only audit for the 2026-10-28 UI task. This is not an upgrade, test execution,
installation receipt or new acceptance approval. No local application, test,
launcher, training or diagnostic-capture command was executed by this auditor.

## Source and cloud identity

- Workspace: `D:/CodexProjects/Ishida_Lab/Documents/git`, maintained `main`.
- Audited main: `5183926e09f5b568379bb1f89ee1667a9bd38121`; the GitHub ref API
  returned that same full revision at audit time.
- The latest exact-revision run is [Native Security 40553331565](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40553331565), completed successfully.
- The newest full [Native Windows 40523593396](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40523593396)
  succeeded at `87a797bd2fe5072df1a72e5587414841f62ee494`. Its final jobs were
  `Qt 6 native preview`, `Upstream viewport acceptance`, and `Native C++ CodeQL`,
  all successful. The preview job was a later attempt; failed-attempt artifacts
  remain attached to the same run and must not be confused with final evidence.
- [Native Managed Updates 40523593355](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40523593355)
  succeeded at that same `87a797b` revision. Earlier failed runs are historical,
  not evidence that the fixed managed-update contract remains failed.
- `git diff 87a797b..5183926` changes only `AGENTS.md` and
  `docs/NATIVE_UPDATES.md` (2 files, 18 insertions/2 removals). Thus compiled
  application/renderer/test inputs are unchanged across these two revisions;
  the cloud results still belong to `87a797b`, not an exact-source full pass
  at `5183926` or any future UI candidate.

Raw API evidence is retained under
`D:/Codex/validation/gsw-ui-upgrade-20261028/ci-delivery-baseline-v1/`:
`remote-main.json`, `runs.json`, `full-run-jobs.json`, and
`full-run-artifacts.jsonl`.

## Installation recheck: concurrent external publication

**The installation changed while this audit was in progress.** At
`2026-10-28T05:13:28Z`, another task/user publication migrated the Desktop and
Start-menu links and activated a managed release. This auditor did not save
shortcuts, execute the publisher or start the launcher. Preserve both the
initial snapshot below and this later state; do not repeat initialization or
overwrite the new stable entry point.

The current rechecked managed root is **`D:/Apps/GSW`**, not the E-drive path
in the newly added documentation. Its identity is:

| Item | Rechecked value |
| --- | --- |
| `current.json` version | `v1-87a797bd2fe5-45e9cb901ee6` |
| Installed source commit | `87a797bd2fe5072df1a72e5587414841f62ee494` |
| Installed payload-manifest digest | `45e9cb901ee6b3536634fbad1fc1790d8d7bab5a71db2d937ae6868c296fa66a` |
| Active app SHA-256 | `b31d09e46d71f97e108e4eaed1e67e7b12e92ff56eda008ac68cec40626787dd` |
| Current pointer SHA-256 | `dccc0ba7440fd7d1d0c83b51623a0863fd27e8e23c6e20086cff4f5b26dd1258` |
| Installed receipt SHA-256 | `9c5d0c4b518dc575c763317763f49529db3c23cf4647ba9a1fbceb4f1a3e5118` |
| Bootstrap launcher SHA-256 | `74cb0c1765a55f49ac5cb684050320047c660981264e36c56661ba7bc2fb8109` |

The pointer's executable digest matches the actual active executable. The
installed receipt declares 2830 payload files; this audit checked the identity
fields and active app, not all 2830 hashes. The external publication has its
own complete acceptance/static/readback records under
`D:/Apps/GSW/validation/remote-sync-oct28-87a797b/`. Its
`publication/publication.json` has SHA-256
`8ee1ef3962331badb91bb3ffa42ace0f73cdea8a7d85fecdf07398e04e845014`,
records `previous:null`, both links' old hashes/backups, and zero application
starts/stops and old-version removals. These are the publisher's records, not
new actions or re-executed acceptance by this auditor.

Both recognized links now target
`D:/Apps/GSW/bin/Gaussian Scene Workbench Launcher.exe`, use the matching
`bin/` working directory and launcher icon, and retain empty arguments:

- Desktop raw digest: `08ef0943fa03b4ae87a6a388b606e3f8500cb79e9fd8e7f839bcde94c6217ce4`.
- Start-menu raw digest: `e4f551cf70d8b4481b59bb7d5bb9951ccaba0b9f08d407d34755803445322463`.

New snapshots are `recognized-shortcuts-concurrent-v2.json` (SHA-256
`cdd44204fa2a1b792e9c03a50f7c79050596835428402826ff45ff22beb008b5`)
and `concurrent-managed-identity-v2.json` (SHA-256
`af9ebcfbea5a17245501678bcfc7122a002055f62334cf637efed3800cff4240`).
No local GSW process was returned at this recheck. Next UI publication must
reuse this managed root and ordinary version-slot publication, **without
repeating `-SynchronizeShortcuts`** unless a later independently established
entry-point defect requires repair.

## Initial classic-install snapshot (historical, superseded above)

The current device exposes C: and D:, not E:. The `E:/Apps/Gaussian Scene
Workbench` location in `docs/NATIVE_UPDATES.md` is therefore not a usable local
path and must not be copied into a publication command on this device.

At the initial read, the desktop `Gaussian Scene Workbench Native.lnk` pointed
directly to:

`D:/Apps/GSW/native/0.3.8-viewport-status-oct27-61d1654/bin/Gaussian Scene Workbench.exe`

It was **not yet** a stable-launcher entry. The target is a classic Setup
installation (`unins000.exe`, `gsw-installation.ini`, and
`installer-payload.json` are present). Its displayed product version is
`0.3.8-native-preview`; this version label alone does not identify source.

| Item | SHA-256 |
| --- | --- |
| Installed application | `762fc69d74bdacbe191c4665818e6e16316a0f5e7c89006aee7daa47a07c1cac` |
| Corresponding source manifest | `62d57ca727086d56d72954173753f90c343303eae58fac6046731efe8d265542` |
| Existing desktop shortcut bytes | `ce5f3e84ee088a54bde2de4a9b32e8a3587d802c4ea3b5d8d36e6e32e6ccd3c4` |

Initial readback used the Unicode WScript Shell COM properties without resolving
or launching the target. Arguments are empty; working directory and icon point
to the same existing version's `bin/` directory. This is a baseline observation,
not the independent native Unicode readback required after migration. A bounded
filename scan under `D:/Apps/GSW/native` found no `current.json` or
`installed-release.json`; no managed activation is claimed. No running GSW
process was returned by the audit's process-path query; that is an observation,
not permission to start one and not a guarantee of later session state.

The initial recognized Start-menu link was a different, older entry:
`C:/Users/Ishida_Lab/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Gaussian Scene Workbench/Gaussian Scene Workbench.lnk`
points to
`D:/Apps/GSW/native/0.3.2-camera-scale-p0-oct19-091a12e/bin/Gaussian Scene Workbench.exe`.
Its raw shortcut digest is
`03ec5f9675111fd57179c91468e4869a3b8b6e0319ed0443abf3a103943cef51`.
No filename-matching GSW Taskbar link was found; this limited filename scan is
not an exhaustive recognition/readback guarantee for renamed shell links.
The later external migration above updated both. A future update must not
regress to Desktop-only activation and claim all recognized entries agree.
The two recognized links, their preserved arguments/options and hashes are in
`recognized-shortcuts.json` (SHA-256
`3146c52ddb4b7c95eaa1090f5fb82587842aa3ebddc04a1c434ff741ebfe4300`).

Existing immutable `SOURCE_BINDING-8012184.json` and
`LOCAL_DELIVERY-8012184.json` under
`D:/Apps/GSW/validation/viewport-status-oct27/` were read and their actual hashes
match the previously recorded seals:

- Source binding: `866a9763e4259646aaaffca8408de04f15d41db781da2346b4d32e9497d69773`.
- Local delivery: `ed48d0b764e40c7ba5deaa5a7e6e74191e0a01c507604bf689d6d4810770ad37`.
- Previous independent readback: `8747888a6989696c533e4ed74b25ed97d3acb255090f2e2678c8f66c9d93045b`.
- Release binding: `5f1a1a71537467d118164528ffe9f4c8a8b925f1348ce2e87933e9492905510a`.

They bind installed source commit
`8012184fe196e4dc9da5fdf7e2af0e9c2bcdb788`, tree
`61d1654b45f9ed67c818bbdb751b8eb0f8a23177`, executable digest above, and the
current raw Desktop-link digest. Thus the source identity is receipt-derived,
not inferred from the directory name. The old receipt explicitly records
`fullCiPassed=false`; this auditor did not re-run its 1209 proof checks or
reinterpret it as current cloud acceptance.

A fresh CRLF-to-LF-only source comparison shows the installed and current
`NavigationGizmo.cpp/.h`, `BlenderNavigationPresentation.h`,
`ViewportPresentation.h`, `NativeViewport.cpp`, `AppTheme.cpp` and
`WorkbenchIcons.cpp` agree exactly. `MainWindow.cpp` and `main.cpp` differ due
to subsequent lifecycle/managed-launcher changes. This limited equality check
is retained in `installed-source-slice.json` (SHA-256
`9db652dde3fc8304c67d20f1cbbba4804656858ff6afa3782dc060252cf24563`),
not promoted to whole-install or whole-build equivalence.

## Current remote seams and useful targets

| Scope | Existing remote path / targets | What it establishes |
| --- | --- | --- |
| Navigation layout/picking | `gsw_navigation_gizmo_tests`; CTest `navigation_gizmo` | Projected axes, signed axes, nearest-axis selection, aligned opposite view, control hit regions; not rendered edge quality |
| Actual viewport/camera | Application CTests `native_blender_viewport_presentation_{zh_CN,en_US,ja_JP}`, `native_upstream_navigation_*`, `native_persistent_orthographic_navigation`, edit/observation navigation | Real Windows `QOpenGLWidget`, frame grabs and public input/state assertions |
| Theme and controls | `gsw_app_theme_tests`; CTest `app_theme`; application `native_window_appearance_*`, `native_icon_ui_*` | Shared control/theme behavior and layout; not proof every icon edge is smooth at all DPRs |
| Idle/FPS | `gsw_frame_rate_counter_tests`, application `native_viewport_scheduling_{light,dark}` | Genuine presented-frame cadence and idle scheduling; preserve this independently of visual AA |
| Managed update | `GaussianSceneWorkbenchLauncher`, `gsw_native_installation_tests`, `gsw_native_shortcut_fixture`; `native_managed_installation`; `scripts/test_native_installation.ps1` | Immutable slot/publication, rollback and entry-point contracts on owned remote fixtures |

The full Native Windows workflow already builds the navigation target and runs
its CTest plus three real navigation presentation cases. A new UI target must
be added to the relevant **build target list and CTest selection**; defining a
CTest alone does not make a focused workflow execute it.

`RenderFrameDiagnostics` already records actual widget/screen DPR, physical
framebuffer dimensions, GL vendor/renderer/version, samples, attachments,
blend/state and staged images, without scheduling continuous redraw. Its
optional `native_render_diagnostics_capture` CTest is enabled only with
`GSW_ENABLE_RENDER_DIAGNOSTICS_CAPTURE_TEST=ON`. It requires a real active,
exposed owned window because the presented-image source is the corresponding
`QScreen::grabWindow(0)` region. That diagnostic can inform the **remote** P0
fixture; it is not permission to capture the user's local desktop or run the
launcher diagnostics mode locally. Its current 32-stage/256 MiB diagnostic
snapshot budget is not a proposed navigation-overlay resource budget.

Existing manual short workflows are `native-viewport-status.yml` and
`native-visibility-camera.yml`. Both install pinned Qt 6.8.3, QA Python 3.12,
codec dependencies, record exact checkout identity, prepare a real Windows
display/software OpenGL, then upload logs/screenshots. The status workflow's
choices are `all`, `project-only`, `ci-repair`; none presently means P0 edge/DPI
acceptance. The visibility workflow runs six existing locale/theme cases and
must not be relabelled a new navigation AA test.

## P0 remote red/green wiring recommendation (not implemented)

1. Add a narrow `native-ui-clarity` manual remote lane (or an explicit new scope
   in the existing manual lane), retaining full Native Windows separately.
   Build the actual application plus the existing navigation, theme and frame
   counter targets and the new P0 target. Use the same compiler/Qt/arch/codec
   identities as full CI; no stale executable reuse.
2. Capture the **actual public native viewport** navigation region and actual
   framebuffer/logical sizes, effective DPR, UI scale, font resolution,
   requested/actual GL format and samples, active framebuffer properties,
   renderer and background. Do not infer those values from an environment
   variable or filename. Record a raw full-window PNG and raw navigation ROI.
3. Freeze independent edge-coverage/reference, stroke/fringe thresholds and
   normal-size review criteria before the fix. Run the baseline once, retain
   its failure, and only then implement the production change. Existing
   `navigation_gizmo` asserts interaction geometry, not this visual defect.
4. Cover controlled DPR 1/1.25/1.5/1.75/2 with measured physical/logical mapping,
   themes and user background; label the use of Qt scaling overrides honestly.
   A simulated/controlled DPR is not proof of OS settings changes or actual
   cross-monitor transitions. Actual multi-monitor transitions remain NOT_RUN
   when the runner has no corresponding hardware.
5. Re-run public clicks/drags, orthographic/perspective and real idle cadence;
   retain camera/model state around theme, resize and DPR changes. Preserve
   existing assertions and deadlines. No local GUI fallback, no long training.

## Fast delivery versus existing publisher gate

The supplied UI mandate allows an independently recorded short-gate delivery
before the full cloud workflow completes. The newly integrated managed publisher
currently **requires `fullAcceptance=true` at the exact source commit** in both
`write_native_release_manifest.ps1` and `publish_native_installation.ps1`, and
checks the accepted executable digest plus the proof hash before activation.
This is a real code gate, not just wording in the documentation.

Safe intersection without changing policy: continue UI work while the complete
CI runs; prepare a source-bound candidate/Setup on D and record short-gate
results, but activate through the managed publisher only when its existing
exact-source full proof is true. A doc-only equivalence note cannot be silently
turned into an exact-source full proof. Never manufacture full acceptance,
bypass the publisher with manual `current.json`, or point entry links directly
at a new version to evade the stable-launcher contract.

If an actual fast activation channel is required before full CI, its proof
schema/publisher policy needs an explicit reviewed change separating scoped
local delivery from cloud-full status; root should resolve that policy choice,
not weaken or relabel current proof fields. The classic Setup is a separate
adapter and must not accidentally replace stable taskbar/Desktop entry points.

The required D-drive managed migration was completed externally during this
audit as described above. Reuse `D:/Apps/GSW`, retain the classic install and
the external publisher's existing backups/readback, and publish ordinary later
slots without rewriting shortcuts. No migration or installation was performed
by this auditor.

## Baseline evidence limits

The available `87a797b` artifact is a historical same-production-code baseline,
not evidence of the new UI candidate, all five Windows DPI settings, a
fractional-DPR rendering fix, cross-screen behavior, or current local app state.
No user photos, models, desktop captures or projects were uploaded. Only public
CI-generated synthetic fixture evidence may be downloaded for inspection.

## Downloaded real-window baseline artifact

The public `native-upstream-viewport-acceptance` artifact (ID `12745155709`)
from run `40523593396` was downloaded with the authenticated GitHub CLI to:

`D:/Codex/validation/gsw-ui-upgrade-20261028/ci-delivery-baseline-v1/upstream-87a797b`

It contains 705 files, including 639 PNGs. Its official API archive digest is
`74ce74f000475ad85ec072c45b1b1e0f01104e22a367d2b9d125fc1dc8280cd8`;
the download command extracted the archive without retaining its ZIP, so an
independent raw-ZIP digest recheck was **not** performed. Ninety original
navigation/viewport PNGs have an independent extracted-file SHA-256/IHDR index
in `navigation-images-index.json` (SHA-256
`f563b9aac83b077ae050a60e91ffdb08200a137ebe57be380b7f8220bb49338e`).
All ninety are 960×720 physical pixels; PNG dimensions alone do not establish
their logical size, DPR, OS DPI, effective MSAA or alpha/composition state.

Useful untouched originals (repeat locale folder for `en_US`/`ja_JP`):

- `build-acceptance/blender-viewport-evidence/zh_CN/blender-background-light.png`
- `build-acceptance/blender-viewport-evidence/zh_CN/blender-perspective-initial-dark.png`
- `build-acceptance/blender-viewport-evidence/zh_CN/blender-plane-positive-z.png`
- `build-acceptance/blender-viewport-evidence/zh_CN/blender-model-{points,mesh,3dgs,2dgs}-{0,1,2,3}.png`

The actual artifact logs report `65/65` main selected CTests and `6/6`
path/preset tests passed. Those are the old workflow's selections, not the new
UI matrix's completion count. The selected texture-text JSON reports DPR1 for
its own 1089×689 frame; it must not be applied to every other artifact image.
No AA-specific independent reference/threshold or complete drawing-state
metadata was found in these navigation images; the new red-capable P0 loop
must add them rather than treating these successful semantic tests as an
edge-quality fix.

Two initial static `rg` calls passed Windows wildcard strings as literal path
arguments and failed before reading files; corrected directory plus `-g`
queries were used. No source, installation, shortcut or old evidence changed.
