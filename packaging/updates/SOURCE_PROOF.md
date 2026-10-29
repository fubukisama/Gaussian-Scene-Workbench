# Independent Cargo registry source proof

This is a developer/CI source-archive contract, not production signing authority.
Until its remote acceptance passes, generated dependency source remains
ineligible. A crate's own `.cargo-checksum.json` is **not** its independent origin
proof.

The existing `gsw.native_update_source_provenance/1` schema retains its exact
tracked-file set and extends `generatedDependencies` with a second known kind:

```json
{
  "path": "native/updater-trust/vendor-archives/NAME-VERSION.crate",
  "size": 1234,
  "sha256": "64 lowercase hexadecimal digits",
  "kind": "cargo-crate-archive"
}
```

`cargo-vendor` continues to classify files under `native/updater-trust/vendor/`
and the one generated portable `.cargo/config.toml`. Unknown kinds and paths are
rejected. All generated files must still be enumerated by the complete native
payload and corresponding-source manifests; no hidden/license exception exists.

The independent chain is:

1. The accepted exact Git tree binds the actual archived `Cargo.lock` bytes.
2. Each crates.io registry package in that lockfile requires exactly one original
   `NAME-VERSION.crate`; the actual archive SHA-256 must equal the lock checksum.
3. A bounded, streaming standard-library tar reader validates and hashes every
   regular file inside that original gzip archive. It then compares Cargo's
   pinned metadata-only vendor projection with the actual vendored file bytes.
   The sole new vendor metadata file is `.cargo-checksum.json`, which is
   separately checked against the lock and the independently verified file set.
4. Registry package, raw archive and vendored crate sets are exact matches,
   including packages for platforms not selected by the current Windows build.

Source preparation uses `cargo vendor --locked` and the ordinary configured
Cargo cache. Original `.crate` files are read from `CARGO_HOME/registry/cache`,
verified against the exact lock checksum, then copied into the fresh source
staging tree. Cache directory names and local checksum declarations grant no
trust. No private key, public Release, production environment, user project or
running application is involved.

Cargo 1.99.1 emits an advisory `$comment` alongside `package` and `files` in
vendor checksum metadata. Only this optional bounded text field is accepted;
it is never an authority input. All other unknown fields still fail closed.
See the [exact Cargo source used by Rust 1.99.1](https://github.com/rust-lang/cargo/blob/5f94df4789f005f9a352888e8355ffc645b7ed0e/src/ops/cargo_vendor.rs#L315).

The `cargo-vendor-1.99.1` projection follows only the exact
[`vendor_this` predicate](https://github.com/rust-lang/cargo/blob/5f94df4789f005f9a352888e8355ffc645b7ed0e/src/ops/cargo_vendor.rs#L604):
it omits paths with a component exactly `.git`, files whose basename is exactly
`.gitignore` or `.gitattributes`, and the root-relative `.cargo-ok` marker.
Matching is case-sensitive; other hidden files, nested `.cargo-ok`, source code
and licenses are never implicitly omitted. All original TAR entries first pass
the same safety checks, including entries omitted from the vendor projection.
Their complete original bytes remain in the SHA-256 lock-bound `.crate` in the
source bundle; source preparation does not restore Git metadata into vendor
directories. Extra vendor copies, unexplained missing files and changed bytes
remain failures even if local checksum metadata is changed coherently.

The source evidence separates `originalRegistryFileCount`,
`vendoredRegistryFileCount` and `filteredRegistryMetadataFileCount`, records
`vendorProjectionId`, and requires original count = vendor count + filtered
count. The counts describe two representations of the same retained original
source, not permission to discard files from the original registry archives.

The reader must never extract tar paths into the filesystem. Only regular files
and directories are permitted under the exact `NAME-VERSION/` prefix. Links,
devices, sparse files, traversal, Windows aliases, duplicate files and unknown
source mappings are rejected. Extended headers are bounded before their data is
allocated; file size, total data, count, path length/depth and trailing padding
are also bounded. Original archives and input files are not rewritten.
Gzip expansion is bounded before TAR parsing, including metadata and padding:
512 MiB per crate and 2 GiB for the dependency closure. Individual archive/source
metadata reads are capped at 64 MiB. Unsupported dependency resolution or an
exceeded bound is a failed acceptance gate, not permission to relax protection
silently. Local path dependencies must also be independently present in the Git
tree; workspace/patch/replacement or unknown source semantics fail closed.

Acceptance is through `verify_release_archive`: first a complete independent
fixture, then coherent-manifest tampering and hostile archive cases, and finally
the real locked GSW dependency closure on remote CI. This proves source origin
and byte completeness, not reproducible compilation, training quality, a legal
license opinion or production publisher readiness.

The implementation uses documented Python interfaces:
[tarfile streaming and TarInfo hooks](https://docs.python.org/3.12/library/tarfile.html)
and [gzip file objects](https://docs.python.org/3.12/library/gzip.html).
