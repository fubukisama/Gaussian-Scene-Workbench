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
Cargo manifest and tests. At this checkpoint no upstream implementation file
has been changed. Any later GSW storage patch must be listed below with its
reason and remote regression evidence. Dependency versions are additionally
bound by the GSW updater trust crate's Cargo.lock.

The library supplies TUF cryptographic/role verification. It does not supply
GSW's installation activation, archive policy, public release authorization,
UI, persistent cross-process lock or a first-install trust root. No claim of
complete TUF specification 1.0.37 conformance is made by vendoring it.

## Local changes

- `README.gsw.md`: provenance notice only. Implementation patch pending remote
  first-red acceptance; this directory is not by itself a usable GSW updater.

