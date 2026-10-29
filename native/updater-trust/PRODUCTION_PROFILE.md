# Compile-time profile configuration: developer tracer

The default library remains deliberately `NOT_CONFIGURED`. No production root,
private key, approved feed or signing/release authority is configured by this
change. A real public production profile, its review hash and any signing or
release infrastructure require separate human authorization.

`gsw-trust-profile-probe` is a developer-only observation tool. It calls the same
compiled-profile implementation as the trust library, takes no runtime input,
performs no network or installation operation, and is never installed or included
in a public package. Its machine JSON codes are intentionally unlocalized.

## First RED remote slice

The initial `production-profile` feature is intentionally a stub: the current
non-fixture implementation still returns `None`. The configured-profile tracer
must therefore fail on `NOT_CONFIGURED` instead of failing to compile or having a
missing executable. This is not an authorized production-enabled build.

Run only on remote CI with a separately prepared public, ephemeral test profile.
Never use a production-library/package Cargo target directory for these probes.

```powershell
$manifest = "$PWD/native/updater-trust/Cargo.toml"
$defaultTarget = "$PWD/native/profile-probe-default"
$configuredTarget = "$PWD/native/profile-probe-configured"
$env:GSW_UPDATER_PRODUCTION_PROFILE_PATH = '<absolute ephemeral CI-only public profile path>'
cargo build --manifest-path $manifest --locked --release --bin gsw-trust-profile-probe `
  --features profile-probe --target-dir $defaultTarget
if ($LASTEXITCODE -ne 0) { throw 'Default profile probe build failed.' }
cargo build --manifest-path $manifest --locked --release --bin gsw-trust-profile-probe `
  --features profile-probe,production-profile --target-dir $configuredTarget
if ($LASTEXITCODE -ne 0) { throw 'Configured profile probe build failed.' }
python -I -B native/updater-trust/tests/remote_profile_contract.py `
  --default-probe "$defaultTarget/release/gsw-trust-profile-probe.exe" `
  --configured-probe "$configuredTarget/release/gsw-trust-profile-probe.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compiled-profile contract failed.' }
```

Retain the exact source commit/tree, build logs and failing assertion from the
remote RED run before implementing production-profile acceptance. A passing
compile or this isolated profile contract never establishes release, TUF
refresh/activation, package, application-health or visual/geometry acceptance.
