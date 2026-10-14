# Native desktop main-program license

Copyright (C) 2026 fubukisama

Effective licensing change: 2026-10-14.

The original Gaussian Scene Workbench native desktop main program is free
software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. See [COPYING](../COPYING), or <https://www.gnu.org/licenses/>.

## Scope and preserved grants

The GPL-3.0-or-later grant covers original code in `native/src/`,
`native/tests/`, `native/i18n/`, `native/CMakeLists.txt`, and original native
build resources, except files or materials carrying a separate license notice.
It is not a blanket relicensing of the repository or of every file below
`native/`.

- `native/third_party/` retains each upstream license and copyright notice.
  The Blender v4.5.0 geometry adaptation in `native/third_party/blender-gizmo/`
  retains GPL-2.0-or-later upstream notices; GPL version 3 is selected when
  combining it with the main program. See that directory's provenance records.
- Separate `native/worker/` compute components retain their existing terms.
  In particular, upstream-derived 2DGS training and density-control code keeps
  the Gaussian-Splatting research/evaluation license. It is not granted GPL
  rights by this document.
- Original repository code outside this main-program scope remains under
  the root [LICENSE](../LICENSE), unless another notice applies.
- MIT rights already granted for older versions remain available. This
  change does not retroactively revoke those grants or make a previous MIT
  version contain the new Blender adaptation.

## Distribution and third-party boundary

Redistribution of the combined desktop program must satisfy the GPL,
including its corresponding-source requirements and applicable copyright,
license, change and warranty notices. Binary redistributors must provide
the source and build/install materials corresponding to the distributed
binary through a method permitted by the GPL. A moving branch link alone
does not identify the exact source of a binary.

Qt, SPZ, zlib, Zstandard, Lucide and gsplat SH retain their own notices.
The original `native/src/shaders/SurfelProjection.glsl` implementation is
part of the GPL main program. The previous research-licensed derivative
`native/third_party/2dgs-surfel/evaluate_surfel.glsl` is preserved under its
original license but is not embedded in or loaded by the GPL desktop program.

Research training components run separately and exchange task information,
files and supported preview frames with the desktop. Separate processes
alone do not determine whether a particular distribution is legally an
aggregate. Packaging or integration changes require a fresh assessment of
the actual communication and dependency boundary. No commercial-use rights
to Graphdeco training components, models or other restricted dependencies
are granted by the main program's GPL license. This document records the
project's licensing scope and engineering boundary, not a legal guarantee
for every downstream distribution. See [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

## License-text provenance

`COPYING` is the unmodified GPL version 3 text from
<https://www.gnu.org/licenses/gpl-3.0.txt>, retrieved on 2026-10-14.
SHA-256: `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`.
The program's "or any later version" permission is stated above; the license
document itself is not edited to add it.

## 简体中文

自 2026-10-14 本次变更起，原生桌面主程序的原创代码按 GPL-3.0-or-later
提供，不提供担保。范围及例外见上方；完整正式英文条款见根目录 `COPYING`。
第三方及独立计算 worker 保留各自许可，不代表 Graphdeco 等研究组件获准商用。
此前授予的 MIT 权利不撤销。分发新版主程序须按 GPL 提供对应源码、构建材料、
版权与许可声明；“不同进程”并不自动证明整个分发包的法律兼容性。
中文说明仅供理解，正式授权以上方英文声明和 GPL 原文为准。

## 日本語

2026-10-14 の変更以降、独自のネイティブ主プログラムは GPL-3.0-or-later
で提供し、保証しません。対象と例外は上記、正式な英語条項はルートの
`COPYING` を参照してください。第三者と独立した計算 worker は各ライセンスを
維持し、Graphdeco などの研究コンポーネントの商用許諾を意味しません。
過去の MIT の権利は撤回しません。新版を配布する場合、GPL に従って対応する
ソース、ビルド資料、著作権・ライセンス通知を提供する必要があります。
別プロセスという事実だけで、配布全体の法的互換性が保証されるわけではありません。
日本語は説明であり、正式な許諾は上記の英語通知と GPL 原文に従います。
