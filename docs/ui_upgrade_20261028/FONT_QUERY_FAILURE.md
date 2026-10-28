# Font-query failure: direct evidence and repair boundary

This document records a **native metadata-query failure**, not proof of poor glyph quality, broken font files, or an invalid two-painter design. No Qt source is copied into the product. No local tests/application/debugger were run for this audit.

## Remote red and artifact binding

- Run: [40656943272](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40656943272), job `122710175134`, source `ffb8eb23679575ff55a3207b35c21e6f8225ceff`, tree `8d8ec309e578b7c5c9a43865abf4e62ac1ef137b`.
- Artifact: `native-ui-clarity-diagnostic-40656943272`, id `12802267229`, server compressed bytes `20,934,784`, server digest `sha256:112a9188893d4dd65b135c9d855be2aee926257b07cb70850541d515d1f47089`. The gh-extracted ZIP was not independently locally hashed.
- Raw data: `D:/Codex/validation/gsw-ui-upgrade-20261028/diagnostic-run40656943272`, 44 files / 68,037,694 extracted bytes, preserved unchanged. The sibling `diagnostic-run40656943272-review.json` records source/protocol/log/dump hashes and PID mapping methodology.
- Build and the 12 pixel-oracle / 19 performance / 12 surface **contract tests** passed. Both diagnostic arms failed 0/3 real navigation/lifecycle tests; this artifact is not a fresh 20-case rendered-pixel acceptance run.

| Arm | Test | Outcome / time | Dump PID | Mapping evidence |
|---|---|---|---|---|
| original | native_ui_overlay_lifecycle | SEGFAULT / 11.10 s | 2708 | Lifecycle function in CDB stack; last locale transition en_US |
| original | native_upstream_navigation_en_US | 0xc000041d / 1.57 s | 2404 | Serial CTest order and dump UTC08:36:25 |
| original | native_upstream_navigation_ja_JP | 0xc000041d / 1.69 s | 7932 | Serial CTest order and dump UTC08:36:27 |
| grayscale-probe | native_ui_overlay_lifecycle | SEGFAULT / 7.31 s | 1736 | Lifecycle function in CDB stack; last locale transition en_US |
| grayscale-probe | native_upstream_navigation_en_US | 0xc000041d / 1.24 s | 7368 | Serial CTest order and dump UTC08:36:35 |
| grayscale-probe | native_upstream_navigation_ja_JP | 0xc0000374 / 2.37 s | 2768 | Serial CTest order, UTC08:36:38, upstream function in stack |

The grayscale/NoSubpixel diagnostic treatment did **not** repair the failure. Japanese grayscale completed two render/composite cycles before failing; those frames do not establish a stable pass.

## Localized call site, not a glyph-AA guess

All six failing cycles stop at `NAV_PAINT_TRACE font-info-begin`, before its end marker and before that cycle's axis-text draw. The common own call chain is:

`NativeViewport::paintGL → drawAxisGizmo → UiOverlayRaster::paint → drawBlenderNavigationPresentation+0xaea → QFontInfo::exactMatch`

Five first-selected exceptions are access violations in family-name/string handling (`QtFontFamily::matchesFamilyName` and QString assignment/indexOf/lastIndexOf). The sixth detects heap damage while cleaning up within `QFontInfo::exactMatch+0x41e`, through `free_base/RtlFreeHeap`. The other five place exactMatch at `+0x1fe`.

The artifact's Qt6Gui export listing independently places exactMatch at RVA `0x294620` and family matching at `0x2a9430`. The own application PDB resolves its calling chain. Qt private source-line symbols are absent; CDB's executable-checksum warning is retained. These facts locate the failing metadata call but do not establish every earlier corruption write site.

This invalidates a simple inference that A32 glyph raster or image-to-GL upload caused the observed exception: the failing cycle had not reached them. It also does not make all nested QImage painting unsafe. Qt's [one-painter restriction](https://doc.qt.io/qt-6.8/qpainter.html#begin) is per paint device; the outer widget and inner QImage are distinct devices.

## Source-reviewed multi-family defect candidate

Official source is pinned to Qt `v6.8.3`, tag object `4606ef4436469d631d8f15f5a312c9af206ecc43`, commit `c07c2d5a527a644d36e7853d55132ae38921682f`. Source is reference-only under the headers' `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only` terms; no upstream implementation is compiled into GSW by this change.

| Relevant source | Exact source/hash and fact |
|---|---|
| [qfont.cpp](https://raw.githubusercontent.com/qt/qtbase/c07c2d5a527a644d36e7853d55132ae38921682f/src/gui/text/qfont.cpp) | SHA256 `6043c6f4a19c34640b8ae359641e22f091009ad4e668c57ee6ab9302263516d5`. QFontInfo::exactMatch calls requested-vs-realized QFontDef comparison (3315+). Its family loop (86–88) indexes the realized family list using the requested list's size without a second-size check. |
| [qwindowsfontengine.cpp](https://raw.githubusercontent.com/qt/qtbase/c07c2d5a527a644d36e7853d55132ae38921682f/src/gui/text/windows/qwindowsfontengine.cpp) | SHA256 `5f4dbcc73b40d605723a3a0a058cd55b5aea237c2fdc5063fb7e5b0b49a9dc9e`. GDI initFontInfo copies the request then replaces realized families with one GetTextFace result (1128–1136). |
| [qwindowsfontdatabase.cpp](https://raw.githubusercontent.com/qt/qtbase/c07c2d5a527a644d36e7853d55132ae38921682f/src/gui/text/windows/qwindowsfontdatabase.cpp) | SHA256 `b27769d1e58dc735ececc007f931da880f9fc24ec4127d674d4be3d2e9e03bf8`. DirectWrite construction also supplies one selected family (1250+). |

GSW requests one family in Chinese, two in English, and three in Japanese. The size-mismatch condition fits the observed locale split and exactMatch/string stacks. **Remaining research gate:** verify the installed Qt binary/source identity and a minimal remote non-GL multi-family exactMatch reproduction before describing a universally proven Qt-version defect. The application must not test this unsafe call in its ordinary full CTest process merely to reproduce an expected crash.

The complete 11-file read-only upstream reference set, URL prefix, hashes, license headers and two corrected unavailable paths are retained at `D:/Codex/validation/gsw-ui-upgrade-20261028/source-references/qtbase-v6.8.3-c07c2d5a/PROVENANCE.json`. Live Qt6.8 documentation currently identifies 6.8.9; implementation claims here use the pinned 6.8.3 source, not an assumed equivalence to that live documentation.

## Product repair boundary and acceptance

The scoped repair omits the unsafe **metadata query only**, retains requested fallback-family lists, default drawing/AA, font sizes/weights and scene semantics, and records:

- `requestedFamilies`, actual resolved family, point/pixel size and weight;
- `exactMatch: null`, not a fabricated true/false;
- `exactMatchQuery: "not_queried_qt683_multifamily_crash"`.

An unqueried exact match is **not** a font-availability, fallback-completeness or legibility pass. The independent mixed-script text remains 17 glyphs with zero missing glyphs; actual QImage ink/partial-alpha pixels and real-window locale/overlay gates remain required. `FontResolutionEvidenceTests.cpp` uses the public helper, a real QApplication/font backend and native-DPR QImages across zh/en/ja family lists, five DPRs and point/pixel roles. It never calls exactMatch itself and does not claim to prove all Qt releases have the defect.

At documentation creation, those new tests and the changed real-window gates are **NOT_RUN** for the repair. Prior red evidence does not establish a green repair, all-window visual acceptance, performance promotion or installed-package delivery.
