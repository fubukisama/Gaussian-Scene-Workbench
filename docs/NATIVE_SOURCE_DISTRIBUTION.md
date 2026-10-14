# Native GPL corresponding source

The original desktop main program is GPL-3.0-or-later; see `native/LICENSE.md`
and `COPYING`. Third-party and separate training components keep their own
licenses. No Graphdeco commercial rights are granted.

Native binary packages contain `source/native/`: the matching C++ sources,
shaders, resource files, translations, tests, build manifest, CMake description
and directly used third-party source subsets. `source/scripts/` contains the
native build and translation tools. This source tree is copied from the same
checkout as the binary, not reconstructed from a moving branch URL. The
package's `source-manifest.json` records the executable hash and source-file
hashes; preserve it with the corresponding package when redistributing.

## Rebuild the main program

Use a C++20 compiler (MSVC 2022 on Windows), CMake >=3.24, Python 3.12,
Qt 6.8.3 Core/Gui/Widgets/OpenGL/OpenGLWidgets/Concurrent/Svg/LinguistTools,
zlib and Zstandard development packages. Qt must include the base translation
catalogs. Build dependencies retain their own upstream licenses. Match the
runtime versions used by your package; do not substitute a restrictive Qt
license for the open-source one when redistributing.

```powershell
$date = (Get-Content source/native/build_manifest.json -Raw | ConvertFrom-Json).releaseDate
cmake -S source/native -B rebuild -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release "-DGSW_RELEASE_DATE=$date" -DCMAKE_PREFIX_PATH="<Qt and codec development prefix>"
cmake --build rebuild --target GaussianSceneWorkbench
```

Run from the package root. The application dynamically links Qt; its DLLs are
replaceable. To rebuild the embedded source without the optional training
backend, use this CMake path rather than automatically installing research
training environments. The supported frontend/worker boundary and optional
training configuration remain described in `native/LICENSE.md` and the native
runtime documentation. To run the complete tests, use the exact repository
revision and QA scripts, with the dependencies recorded by `docs/NATIVE_CI.md`.

Qt source corresponding to 6.8.3 is available from the official release archive:
https://download.qt.io/official_releases/qt/6.8/6.8.3/single/ . Qt libraries and
their bundled dependencies require their own source/notices compliance; the
original-application source bundle is not a claim to relicense all dependency
binaries. Redistributors must check the actual dependency build and their
chosen GPL/LGPL compliance method.

中文：安装包同附主程序对应源码、构建资源和逐文件哈希；第三方及训练许可分别
保留，不能把整个包宣称为可自由商用。重新构建主程序不需要自动安装训练环境。

日本語：配布物に主プログラムの対応ソース・ビルド資料・ファイルごとのハッシュ
を同梱します。第三者と学習のライセンスは別に維持し、配布全体の商用許諾を
意味しません。主プログラムの再ビルド時に学習環境を自動導入する必要はありません。
