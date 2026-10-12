# SPZ codec source provenance

- Upstream: https://github.com/nianticlabs/spz
- Pinned commit: `affd0ecea7fbb4c265ee119475af7ee5b2997482` (retrieved 2026-09-19).
- License: MIT; see LICENSE and the Niantic Labs / Adobe copyright notices in each source file.
- Vendored subset: `src/cc/load-spz.{cc,h}`, `splat-types.{cc,h}`, `splat-c-types.h`, `splat-utils.h`.
- Base source is pinned to the commit above; `load-spz.cc` carries the narrowly scoped validation patches documented below. Copyright and MIT license notices are retained. Built directly into `gsw_spz` by the native CMake target, with the existing toolchain's zlib and Zstandard. No upstream CMake downloads, C API implementation, Python/WASM bindings or optional extensions are enabled.
- GSW's adapter is `native/src/SpzIO.cpp`. It validates headers, memory budget, exact stream sizes, finite raw PLY parameters, coordinate range and cancellation/atomic output before/around calls into the actual upstream codec. Unsupported SPZ extensions are rejected, not silently discarded.
- `spz::saveSpz`, `loadSpzPacked`, `PackedGaussians::unpack` and `coordinateConverter` are used directly. PLY RDF ↔ SPZ RUB conversions include rotations and SH signs, not only positions.
- Export v4 and v3; import standard v1–v4. This is lossy delivery, not optimizer checkpoint storage. Resident native rendering supports SH 0–4 via the separately licensed gsplat evaluator; retained higher-degree SH remains available for re-export regardless of the display quality cap.
- This directory, including source copyright notices, is included in desktop packages under `licenses/spz`.
- Dependency notices are reproduced in `dependency-licenses/` from the official [Zstandard v1.5.7 license](https://github.com/facebook/zstd/blob/v1.5.7/LICENSE) and [zlib v1.3.2 license](https://github.com/madler/zlib/blob/v1.3.2/LICENSE), including for CI's statically linked codec dependencies.

## Local validation patches (2026-10-12)

- Counts are validated before multiplication; attribute sizes and offsets use `size_t`, with representability checks for narrower targets. Invalid cloud metadata is rejected without replacing the caller's output.
- `PackedGaussians::at` throws `std::out_of_range` for invalid indices, metadata or incomplete buffers. Direct unpacking also validates fractional-bit counts before shifting. Degree-zero SH does not address an empty vector.
- Legacy gzip decoding validates the complete attribute payload against the known decompressed byte length before allocation; it no longer depends on seeking the read-only memory stream. Both legacy and NGSP headers reject unsupported SH degrees and fractional-bit counts.
- SH quantization saturates in floating point before integer conversion, preserving ordinary quantization and safely handling extreme finite coefficients.
- Regression coverage uses one/two-point round trips, tiny malformed headers, metadata-only large declared counts, invalid indices and truncated buffers. These tests do not allocate a declared large model or run training; MSVC tests are not claimed as UBSan coverage.

中文：本地补丁仅加固计数、长度、索引和数值边界，保留普通 SPZ 读写兼容性及 MIT 版权声明。旧版 gzip 在分配前检查实际解压长度；空 SH、越界索引和异常声明计数使用微型样本验证，不申请大型模型内存、不启动训练。

日本語：ローカル修正は点数・長さ・インデックス・数値の境界検証に限定し、通常の SPZ 読み書きの互換性と MIT の著作権表示を維持します。旧 gzip は確保前に実際の展開サイズを検証し、空の SH、不正なインデックス、異常な点数宣言を小さなサンプルで確認します。大規模モデルのメモリ確保や学習は行いません。
