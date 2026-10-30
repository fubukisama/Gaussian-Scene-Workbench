<div lang="en">

# Changelog

[English](CHANGELOG.md) · [简体中文](CHANGELOG.zh-CN.md) · [日本語](CHANGELOG.ja.md)

User-facing changes to Gaussian Splatting Workbench, newest first. This recovered
history covers 2026-10-23–30; it is a curated summary, not a reconstruction of every
older change. Dates use Japan time. [Complete source history](https://github.com/fubukisama/GaussianSplattingWorkbench/commits/main/).

**Status key:** **Implemented** means the linked source change exists. It does not
mean full verification, a public Release or installation on a particular computer.
Those require their own exact-version evidence. As of 2026-10-30, the source version
is `0.3.9-native-preview` and no public native Release has been published. See the
[download guide](README_RELEASE.md) and [update/rollback guide](docs/NATIVE_UPDATES.md)
for distribution status and requirements. CI artifacts and local preview deliveries
are not automatically public Releases.

Future user-facing additions, changes and fixes must update all three changelogs
in the same change, with affected capabilities, limitations and verification or
delivery status. Implementation, verification, publication and installation remain
separate claims.

## 2026-10-30

Status: **Implemented**; this section is a source record, not a release or installation receipt.

- **Changed:** restored dated English, Simplified Chinese and Japanese update
  records and their homepage entry points; made ongoing changelog maintenance a
  shared project rule, including Codex project forks. Documentation checks now
  guard localized history, dates, source references and homepage entry points.
- **Fixed:** taskbar relaunch metadata takes its display branding from the running
  application resource while keeping the stable launcher as the launch target.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/e1ec20a69f7a44523bc747fbbb71000984864684).
- **Improved:** automatic appearance and idle pause/unavailable resume menu items
  expose their explanations through accessibility, rather than leaving disabled
  actions unexplained.
  [Appearance](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/aa09ab26ba637387f3b1049dc11492916cce2b11) · [Pause/resume](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d87343fa215389815c6cf037aef85153a5bf6b91).

## 2026-10-29

Status: **Implemented**; verification and delivery remain bound to their separately recorded source versions.

- **Changed:** the display product name is now **Gaussian Splatting Workbench**.
  Existing executable names, settings keys and application identity remain
  compatible. [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b0aacf85bb5e87b82b6bfcde0ce0a6ba5728b93b).
- **Improved:** initialization and batch forms handle bounded windows more
  consistently; field accessibility and job-state presentation were clarified.
  [Form layout](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c170af833aa8b35bed560da0a403d1160b4657c0) · [Accessible fields and jobs](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c3e667a249e0de1b4384aa3d97d29172e08a474b).
- **Added:** an independent recovery entry and explicitly reverified recovery
  choices; downloading checks available capacity before starting. These updater
  components do not enable a public signed update channel or prove the complete
  first-install/update workflow. [Recovery](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/3ed5a4857ded622e9c188847494bc4d2c59ed650) · [Capacity checks](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/9db10d37bb0e9d2ce60d38d91c573dab530a6d2e).
- **Documentation:** added a real 3DGS/2DGS training-stage and native-operation
  video. Saved-stage replay and synthetic inputs are identified; the demonstration
  is not a reconstruction-quality, accuracy or performance benchmark.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/7222c1203ebdc791d89d02768baf81cf27275915).

## 2026-10-28

Status: **Implemented**; isolated component checks are not whole-product or public-channel acceptance.

- **Improved:** initialization controls scroll within a bounded window while
  keeping the action row accessible.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/fa1c39fe2fcf5cacd3bb39e266a2b0bc10dc9b7b).
- **Added:** a silent-by-default **Help → Software Updates** interface with live
  three-language text. Online use still requires a compatible trusted bootstrap
  and configured signed source; it does not announce a deployed public service.
  [Interface](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/55064f8c5c5b26d99f6feab1d063423e20af1e7e) · [Application integration](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/ce3e4e044647dfd59a9508674f77c2ed5db74320).
- **Security:** strengthened complete archive/source verification, long Unicode
  update paths, offline startup checks and explicit rollback verification. These
  safeguards do not constitute production signing or update-feed approval.
  [Paths and rollback](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/fa1c39fe2fcf5cacd3bb39e266a2b0bc10dc9b7b) · [Offline startup and source proof](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/ce3e4e044647dfd59a9508674f77c2ed5db74320).

## 2026-10-27

Status: **Implemented**; a source commit alone does not show that an existing desktop was updated.

- **Added:** project-persistent automatic/fixed grid spacing, grid/axis visibility,
  display units and read-only viewport details. Idle/waiting states replace stale
  FPS claims; display settings do not change model scale or reconstruction accuracy.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d9e1d361fa5daee23617852bcacf11cd64156d50).
- **Fixed:** hiding the last visible model retains the canvas layout and reference
  plane instead of resizing the viewport or resetting its basis.
  [Canvas layout](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/6ea81a50c0f8641c03056d9f5fa0922096ec8d70) · [Reference bounds](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/17cef1e1222b07c7e4109de59deab5a3e4294d7d).
- **Added:** a stable native launcher and verified atomic version-slot publication,
  with subsequent fixes to preserve shortcut options and atomic pointer replacement.
  Running and rollback versions remain separate from the newly prepared slot.
  [Launcher and publication](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/8653293e4ce7209ce9396cc597a89e199ead4da4) · [Shortcut preservation](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/32f8e8d904ea462d5ace2717211170fb54466045) · [Atomic replacement](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/87a797bd2fe5072df1a72e5587414841f62ee494).
- **Fixed:** transient Windows file-sharing conflicts during atomic worker-state
  publication receive bounded retries rather than immediate failure.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b87448ece4bb5664b713881a00ebd553bebb085e).

## 2026-10-26

Status: **Implemented**; background rendering checks do not replace the recorded foreground-bug recheck.

- **Fixed:** static Gaussian object picking uses a read-only path; mixed-model
  selection and overlays isolate and restore OpenGL state/readback so selection
  does not intentionally change model data or peer rendering.
  [Gaussian picking](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/c8901e2ff70845d23f97a724da34afe8d6552bb6) · [Mixed-model state isolation](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/14be3f9aba02d166a5f25db817f67887c5c6cb0f).
- **Fixed:** translation generation has one shared resource owner, avoiding
  parallel consumers reading unfinished catalogs during language switching.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/d8df7867e475e4f271eb9c1a505895c6c47b8d90).

## 2026-10-25

Status: **Implemented**; no universal speedup or memory-limit guarantee is inferred.

- **Improved:** hidden models retain their coordinate identity while viewport
  rendering and resource scheduling become demand-driven; hidden sources defer
  unnecessary GPU admission. [Scheduling](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/253b35fec2ebea58822066ce2b280129745f8207) · [Hidden-source resources](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/68186326266ef9cd6abc7a3a0294e4599cc630e9).
- **Fixed:** disk-paged source export keeps the original source available; package
  startup checks isolate residue from earlier runs instead of accepting stale files.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/bd521628558d62612c8d077df0bd6a655fd70f03).

## 2026-10-24

Status: **Implemented**; these viewing controls do not register or merge separate reconstructions.

- **Added:** per-model eye controls in the project tree, with visibility persisted
  in the project. Name selection and visibility updates remain separate operations.
  [Visibility controls](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/b7c91307813c46b9258d48e2fdb3c9c365cee3a0) · [Selection preservation](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/3552eb20374f321cf19a60dbb352681d05761597).
- **Changed:** Blender-derived axis navigation enters the selected orthographic
  view and restores perspective when freely orbiting from an axis view; panning
  and zooming preserve projection, and the central projection toggle is removed.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/4627ae3db353f027dbb01e695d0563f3e8ac6ea5).

## 2026-10-23

Status: **Implemented**; experimental method execution is not final visual-quality or independent geometry acceptance.

- **Added:** full-source 3DGS/2DGS PLY transform baking with attribute fidelity.
  Unsupported nonuniform transforms with directional spherical harmonics are
  rejected instead of silently approximated; two-scale 2DGS gains no invented
  thickness. [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/0a7116bba76ee83ad53b0567df91e60e5f079dba).
- **Added:** opt-in experimental quality plans and Mip-aware delivery. Shared
  workflow identity and mask/depth supervision cover 3DGS/2DGS, but volumetric methods are
  not presented as interchangeable surfel algorithms; MCMC remains unavailable
  in the desktop selector pending its own acceptance.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/bc09c7dbe3b103c7b54617b123bd5a89ee1d4120).
- **Fixed:** equivalent Windows result paths and persistent metadata sharing no
  longer cause the documented result-publication/pause failures.
  [Source](https://github.com/fubukisama/GaussianSplattingWorkbench/commit/0bbea50994a0dea6431672977887d6fc5b9bc470).

</div>
