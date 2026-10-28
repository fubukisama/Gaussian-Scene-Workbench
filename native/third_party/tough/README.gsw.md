# AWS tough provenance and GSW use

- Upstream: https://github.com/awslabs/tough
- Release: `tough-v0.24.0`, published 2026-07-10.
- Exact upstream Git commit: `98d8eb8b2ce63515d9b4981c938ef6453c5b5771`.
- Immutable registry source: https://crates.io/api/v1/crates/tough/0.24.0/download
- Registry archive SHA-256:
  `35b378d98765c2ae9cdc3e9963ea7e670da8cdd9ee39611b8d722083c7f1ac11`.
- The included `.cargo_vcs_info.json` independently records that exact commit
  and the `tough` repository subdirectory.
- Upstream license is `MIT OR Apache-2.0`. GSW selects the MIT option, preserves
  `LICENSE-MIT`, `LICENSE-APACHE`, copyright notices and complete source.

The published registry source is vendored here, including its normalized
Cargo manifest and tests. The explicit local changes below are storage/parser/
transport-error adaptations, not new TUF cryptographic algorithms. Dependency
versions are bound by the authoritative GSW updater trust crate's Cargo.lock.
The unused upstream development/test lock is retained unchanged at
`patches/upstream/Cargo.lock.reference`; it does not resolve this product build.

The library supplies TUF cryptographic/role verification. It does not supply
GSW's installation activation, archive policy, public release authorization,
UI, persistent cross-process lock or a first-install trust root. No claim of
complete TUF specification 1.0.37 conformance is made by vendoring it.

## Local changes

- `src/gsw_store.rs`: GSW-authored MIT adapter for bounded duplicate-rejecting
  JSON and a single atomic durable journal, including root history, clock and
  application accepted/revoked state. SHA-256 uses the existing AWS-LC library.
  Windows uses flushed non-temporary files and same-directory write-through
  atomic rename. Test-only fault hooks require an explicit Cargo feature.
- `src/datastore.rs`: routes role metadata/clock transactions through that
  journal; shares a global clock between channel namespaces; malformed saved
  time is an error instead of an implicit absent sample.
- `src/lib.rs`: loader namespace adapter, no remove-before-root-write window,
  and only actual next-root not-found terminates root discovery. Other transport
  failures abort. Root verification/rotation/role/digest mathematics is upstream.
- `Cargo.toml`: Windows filesystem API dependency and compile-time fixture
  storage-fault feature. The product's lock pins current audited TLS dependencies.
- `tests/interop.rs`: test-only short fixture path mapping and a Journal assertion
  for the changed durable-store layout. It does not change signed metadata.

Pristine modified upstream files are retained in `patches/upstream/`. The textual
local source patch is `patches/gsw-durable-store.patch`. Initial remote RED is
run 40656542985; dedicated security/Windows fault and root-rotation evidence must
be recorded separately before release. Neither local compilation nor GSW's
focused tests imply that the complete upstream test suite has passed. Some
upstream assertions depend on its old multi-file datastore layout and need
additional adaptation/remote validation before running that suite as a whole.

## Long upstream fixture storage mapping

The upstream logical path is
`tests/data/dubious-role-names/metadata/` followed by the percent-encoded filename
for `ᚩ os, ᚱ rad, ᚳ cen, ᚷ gyfu, ᚹ ƿynn, ᚻ hægl, ....json`. Its 146-character
component plus GitHub Windows checkout prefixes exceeded MAX_PATH. It is not
production source and does not participate in GSW's nondelegated wire profile.
The original bytes are preserved at `patches/upstream/long-role-name-fixture.json`
with SHA-256 `1f60219a41e50038468ab84e27a66e9c08674c6e064210acffe434a76de19dfb`.
`ShortFixtureTransport` only maps that logical test URL to the short physical
file, retaining the actual Unicode role, signatures and hashes. GSW's ordinary
dependency build does not compile/run upstream `[[test]]` targets. Do not assert
that an unmodified upstream suite is directly runnable against this layout.

