# Scoped updater Rust dependency audit

Refreshed 2026-10-29 (JST) against the exact updater `Cargo.lock`, not the retired web
backend or the repository-wide historical alert count. This is static dependency
advisory analysis, not a test execution or a guarantee of absence of defects.

- Auditor: official RustSec `cargo-audit 0.22.2` release.
- Official Windows asset SHA-256:
  `0a7316540862c13d954f648917ceacca593747baed6eec180fafa590be2710ab`.
- Advisory database commit:
  `40f5be6fb12ed93fc591bf4ab23b7d793b3f2239` (1381 advisories).
- Scanned lockfile: `native/updater-trust/Cargo.lock`.
- Lockfile SHA-256:
  `3b66bf597b844c720bf5bf86c0ed3cf14eb31a89a70e93ac9256cd17fdbee1cc`.
- Current result: 190 packages, 0 matched vulnerabilities, no informational
  warnings. No advisory ignore list is used.

The initial upstream-compatible lock selected rustls 0.23.43 and webpki 0.103.13.
It matched RUSTSEC-2026-0285, -0346, -0348, -0349 and -0382. Tokio 1.52.3 also
matched the Unix `AsyncFd` unsoundness notice RUSTSEC-2026-0386. Although the
product target is Windows, that version was upgraded as well. The active lock
now pins rustls 0.23.46, rustls-webpki 0.103.16, aws-lc-rs 1.18.2 and Tokio 1.54.1.
The original upstream library development lock is preserved as a provenance
reference, not used as the updater dependency resolution.

Official references:

- https://github.com/RustSec/rustsec/releases/tag/cargo-audit%2Fv0.22.2
- https://github.com/RustSec/advisory-db/tree/40f5be6fb12ed93fc591bf4ab23b7d793b3f2239
- https://github.com/rustls/rustls/releases/tag/v%2F0.23.46
- https://github.com/rustls/webpki/releases/tag/v%2F0.103.16
- https://rustsec.org/advisories/RUSTSEC-2026-0386.html

Re-run the advisory check remotely on the final exact-source lock before release.
An older clean report is not proof about a later lock or advisory database.
