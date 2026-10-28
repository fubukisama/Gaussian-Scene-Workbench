# miniz provenance

Upstream: https://github.com/richgel999/miniz

- Version: 3.1.2, commit `77d0dce8627735138c51770d1799a1ef48f2117d`.
- Amalgamation: https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
- Archive SHA-256: `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`.
- Unmodified `miniz.c`: `e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30`.
- Unmodified `miniz.h`: `b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f`.
- License: MIT, retained in `LICENSE`.

Used only for bounded ZIP decompression. It does not authorize an update or
choose filesystem paths. The activation core validates all names, file sets,
expanded lengths and digests before publishing a slot. ZIP64 is supported by
the upstream reader; GSW applies its own stricter resource limits.
