# SPZ codec source provenance

- Upstream: https://github.com/nianticlabs/spz
- Pinned commit: `affd0ecea7fbb4c265ee119475af7ee5b2997482` (retrieved 2026-09-19).
- License: MIT; see LICENSE and the Niantic Labs / Adobe copyright notices in each source file.
- Vendored subset: `src/cc/load-spz.{cc,h}`, `splat-types.{cc,h}`, `splat-c-types.h`, `splat-utils.h`.
- Source files are unmodified (line-ending normalization only). Built directly into `gsw_spz` by the native CMake target, with the existing toolchain's zlib and Zstandard. No upstream CMake downloads, C API implementation, Python/WASM bindings or optional extensions are enabled.
- GSW's adapter is `native/src/SpzIO.cpp`. It validates headers, memory budget, exact stream sizes, finite raw PLY parameters, coordinate range and cancellation/atomic output before/around calls into the actual upstream codec. Unsupported SPZ extensions are rejected, not silently discarded.
- `spz::saveSpz`, `loadSpzPacked`, `PackedGaussians::unpack` and `coordinateConverter` are used directly. PLY RDF ↔ SPZ RUB conversions include rotations and SH signs, not only positions.
- Export v4 and v3; import standard v1–v4. This is lossy delivery, not optimizer checkpoint storage. Resident native rendering supports SH 0–4 via the separately licensed gsplat evaluator; retained higher-degree SH remains available for re-export regardless of the display quality cap.
- This directory, including source copyright notices, is included in desktop packages under `licenses/spz`.
- Dependency notices are reproduced in `dependency-licenses/` from the official [Zstandard v1.5.7 license](https://github.com/facebook/zstd/blob/v1.5.7/LICENSE) and [zlib v1.3.2 license](https://github.com/madler/zlib/blob/v1.3.2/LICENSE), including for CI's statically linked codec dependencies.
