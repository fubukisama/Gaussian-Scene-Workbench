# Compile-time profile configuration: developer tracer

The default library remains deliberately `NOT_CONFIGURED`. No production root,
private key, approved feed or signing/release authority is configured by this
change. A real public production profile, its review hash and any signing or
release infrastructure require separate human authorization.

`gsw-trust-profile-probe` is a developer-only observation tool. It calls the same
compiled-profile implementation as the trust library, takes no runtime input,
performs no network or installation operation, and is never installed or included
in a public package. Its machine JSON codes are intentionally unlocalized.

## Configuration mechanism

The default `cargo build --locked --release --lib` still embeds no profile,
regardless of environment variables. An explicitly selected `production-profile`
feature requires `GSW_UPDATER_PRODUCTION_PROFILE_PATH` to name an absolute build
input of at most 2 MiB. Cargo embeds those bytes; the running executable never
reads that path or any profile/feed/root/CA/time environment variable. Changing
the input requires a rebuild, not a runtime argument, GUI setting or adoption
request. `production-profile` and `fixture-profile` cannot be built together.

Before returning a configured profile, the same compiled-profile implementation
used by the library and probe requires strict, duplicate-free JSON with exact
known fields; product `Fubukisama.GaussianSceneWorkbench.Native`; platform
`windows-x64`; exactly `stable` and `preview`; unique, approved DNS hostnames; and
HTTPS bases with no credentials, query, fragment or non-443 port. IP literals,
numeric-IP spellings, localhost/local names and malformed DNS labels are rejected,
including unused entries in the approved-host list. Every `fixtureCaPem` field,
even JSON null, and the fixture product are rejected. Production continues using
the platform certificate verifier, with no custom-CA profile field.

The `seedRoot` must satisfy the existing supported-wire restrictions and have
root-role threshold at least two. Tough's `Signed<Root>` deserializer validates
the root structure and canonical key IDs, and its `verify_role` verifies the
distinct valid root signatures meeting that threshold. This is not a new crypto
implementation. Profile checking alone is not repository freshness or expiry
acceptance; the existing Safe TUF verification remains mandatory for refresh and
cached activation, while historical read-only inspection retains its semantics.

Malformed profile content is fail-closed `PROFILE_INVALID` at the compiled-profile
interface. Missing/nonabsolute/oversize build inputs and mixed features fail the
build itself. A future authorized production build must run the developer probe
as an explicit configuration gate before packaging. No such production build,
profile or packaging authorization is created here.

## Remote RED evidence and current acceptance gate

The initial feature was an intentional stub returning `None`. Actual remote run
`40804929618`, job `123221480977`, at source commit `ebb0e35` recorded the default
`NOT_CONFIGURED` positive and then failed the configured-profile assertion with
`NOT_CONFIGURED != CONFIGURED` at `02:16:47 UTC`. This was a semantic RED, not a
missing executable or build/environment failure. The current implementation still
requires its own exact-source remote positive/negative evidence.

Run only on remote CI with a separately prepared public, ephemeral test profile.
Never use a production-library/package Cargo target directory for these probes.

```powershell
$manifest = "$PWD/native/updater-trust/Cargo.toml"
$defaultTarget = 'D:/gu/profile-default'
$configuredTarget = 'D:/gu/profile-configured'
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

# After the positive probe contract, reuse only its isolated developer target.
# The base input is a CI ephemeral PUBLIC root with reserved .invalid hosts.
python -I -B native/updater-trust/tests/remote_profile_rejections.py `
  --cargo rustup --toolchain 1.99.1 --manifest $manifest `
  --profile $env:GSW_UPDATER_PRODUCTION_PROFILE_PATH `
  --target-dir $configuredTarget --work-root 'D:/gu/profile-rejection-evidence'
if ($LASTEXITCODE -ne 0) { throw 'Compiled-profile rejection contract failed.' }
```

The rejection tracer rebuilds only the developer probe for fixture contamination,
unknown/duplicate fields, unsafe hosts/URLs, wrong platform/channels, threshold
one, insufficient/invalid signatures, mixed features and missing/relative build
input. It generates no keys. It requires remote CI and a `profile-` named target;
its temporary public test profiles and build/probe logs are isolated evidence,
never public package inputs. Do not reuse the normal production-library target.
The positive tracer also confirms an already-compiled profile is unchanged by
runtime environment values and that the probe refuses runtime profile arguments.

Retain the exact source commit/tree, logs and remote assertions. Build success or
this isolated profile contract never establishes release, TUF refresh/activation,
package, application-health or visual/geometry acceptance. The public automatic
update rollout remains closed, including after this isolated contract passes.
