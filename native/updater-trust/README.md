# GSW native update trust adapter

This static library implements the private-to-the-native-helper C seam in
`include/gsw_update_trust.h`. No end-user Python, shell, Git or Rust runtime is
required. The C++ owner must hold the shared installation `.publish.lock`.

Production has no provisioned trust root and returns `NOT_CONFIGURED`, without
network access or a runtime bypass. The separately compiled `fixture-profile`
build implements the isolated trust path after remote first-red run
`40656542985`. Build success is not functional acceptance: remote trust,
fault-injection and integrated activation evidence must be retained separately.

The trust implementation is pinned to AWS tough `0.24.0`, source commit
`98d8eb8b2ce63515d9b4981c938ef6453c5b5771`, under its MIT license option. GSW
does not reimplement TUF cryptographic or role-verification algorithms. Required
GSW adaptations include bounded strict JSON, approved HTTPS transport, atomic
trust-state persistence and a top-level nondelegated release-descriptor profile.

The test profile is a compile-time public fixture input selected using
`GSW_UPDATER_FIXTURE_PROFILE_PATH`. Production does not read this environment
variable, accept test roots, or accept runtime trust/feed overrides. Fixture
private keys must never be embedded or packaged.

## Build and remote tracer

Pin Rust `1.99.1` and use `--locked`. No user-facing binary needs Rust installed.
Use different Cargo target directories for production and fixture builds.

```powershell
# Production: no root, no fixture CA, no test CLI or fault switches.
cargo build --manifest-path native/updater-trust/Cargo.toml --locked --release --lib

# REMOTE CI ONLY: start the real HTTPS server and issue ephemeral public roots.
python -I -B scripts/prepare_native_update_trust_ci.py --root <fresh-fixture-dir> `
  --source-commit <HEAD> --source-tree <HEAD-tree> --with-short-expiry
# Consume profile/fixture; retain serverPid/issuerPid for test-owned cleanup.
$env:GSW_UPDATER_FIXTURE_PROFILE_PATH = '<fixture>/issued/profile.json'
cargo build --manifest-path native/updater-trust/Cargo.toml --locked --release --features fixture-cli
python -I -B native/updater-trust/tests/remote_trust_contract.py `
  --cli <cargo-target>/release/gsw-trust-fixture-cli.exe `
  --fixture <fixture>/issued/fixture.json --work-root <fresh-test-dir>
```

With `--with-short-expiry`, the remote issuer stays alive with ephemeral keys
only in memory until the last test triggers a same-root 60-second timestamp.
It exits after signing and times out after ten minutes without a trigger. CI
cleanup must stop its owned `issuerPid` as well as the HTTPS `serverPid` on errors.
There is no OS clock change, runtime time override or additional production input.

The fixture CLI accepts one request JSON filename, or `-` for stdin. It invokes
the exact C ABI, prints its bounded JSON and returns `0` on success or `2` on
rejection. It is a test adapter, never part of a public installation. Its
synthetic payload does not prove real application launch/activation acceptance.

`tests/compile-only-invalid-profile.json` is deliberately invalid (`{}`). It
only permits local fixture-feature type/link builds without generating keys or
running software; it cannot pass profile validation or constitute test evidence.

See [SECURITY.md](SECURITY.md) for state/expiry/rollback semantics and remaining
acceptance gates, and [DEPENDENCY_AUDIT.md](DEPENDENCY_AUDIT.md) for the scoped
static advisory review.

