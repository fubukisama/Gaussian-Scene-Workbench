# GSW native trust profile and evidence boundary

The C++ helper owns `.publish.lock` across trust, download and activation. This
library adds only an in-process serialization guard; it does not compete for a
second installation lock. Requests contain intent, a helper-derived installation
root and channel, never a runtime root/key/feed/CA/time/unsafe-expiry override.
Production is deliberately unconfigured until separately authorized provisioning.

## Supported wire profile

AWS tough 0.24.0 performs the TUF signature thresholds, sequential root rotation,
role versions, role hashes/lengths, target digest and Safe expiration checks.
GSW does not reimplement these algorithms or claim complete TUF 1.0.37 conformance.
The explicit GSW profile supports top-level Ed25519 root/timestamp/snapshot/targets
roles, SHA-256, a single `release.json` target, and TUF 1.0 patch versions through
37. Delegations, extra critical fields, unsupported schemes/hashes, duplicate
decoded JSON keys, non-integer/unsafe JSON numbers and excessive nesting/sizes
are rejected. Every metadata/descriptor response is bounded before Tough sees it.
No partially read target is parsed as an Offer: the full stream must finish its
Tough digest verification first.

Metadata transport is HTTPS-only, bounded to compiled approved hostnames and
three redirects. It sends identity encoding, no credentials/cookies, uses real
certificate validation, has bounded connect/read/whole-request timeouts and polls
cancellation every 100 ms. A refresh is bounded to 90 seconds and 64 root updates.
Only an actual not-found next root ends discovery; TLS, parser, cancellation and
other errors abort. Error JSON contains codes, not raw URLs or network log text.
The compiled fixture profile may contain its public CA; production cannot load
one from runtime input. The trusted `transportPolicy` response supplies this
compiled host policy to the native archive downloader; it is not a GUI override.

## Durable state

`<installation>/updates/trust/` lies outside every application slot. It contains
an initialization sentinel and `trust-journal.json`; the stable installation
root additionally holds `.update-trust-anchor`, outside the entire updates tree.
The outer anchor is written first on a genuinely fresh initialization and is
never cleared by update/rollback/cleanup. Its presence forbids fresh-store
fallback even if the whole updates/trust directory has disappeared. A missing half,
bad journal digest, malformed data, missing root, root-history gap or profile
identity mismatch fails closed, never silently starts a new trust history.

The local Tough storage patch keeps channel-namespaced role data, original
verified metadata/target bytes, global clock samples, complete accepted root
history, release high-water marks and accepted/revoked build records in one
journal map. Each create/remove/accept is one atomic replacement. Files are
flushed, cleared of the Windows temporary attribute, then renamed in the same
directory with `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`. The journal SHA-256
detects accidental corruption; it is not publisher authentication. Complete
same-user installation/state rewrite is outside this filesystem threat model.
OS flush/atomic rename does not establish a hardware power-loss guarantee.

Each accepted new root is persisted immediately with its immutable history entry.
Later network, descriptor, download or activation failure cannot restore an older
root. At startup the chain is replayed from the compiled public anchor using
Tough's old-root AND new-root threshold verification. Role/key rotation may
legitimately remove old timestamp/snapshot entries, within the same journal.

The shared clock high-water sample prevents backward wall-clock sampling. Safe
expiry still assumes the OS clock is correctly maintained; this is not a hardware
secure clock or a new authenticated time protocol. No runtime clock override is
provided. An expired chain cannot authorize a new download/activation.

Application high-water marks are channel-scoped. Revoked build IDs are a global
monotonic union. Immutable build identities cannot be reused for different
archives/source/manifests. Metadata revisions cannot equivocate, and a release
sequence cannot be reused by a different build. Up to 1024 build identities,
64 accepted descriptor digests per build, 8192 revoked IDs and a 3 MiB accepted
ledger are retained; reaching a bound fails closed without pruning safety state.
Root histories and the full journal are separately bounded.

## Resolve, cached verification and explicit rollback

`resolve` obtains fresh TUF metadata and verifies the complete descriptor, then
durably accepts the descriptor, high-water marks and revocations. A caller's
mismatching expected build ID or a self-revoked candidate does not undo this
newly accepted security state.

`verify_cached` is the post-download activation recheck: it uses the original
metadata bytes (not Tough's reserialized version-comparison cache), the highest
accepted root, full Safe expiration, descriptor digest, expected build ID and
revocation/sequence checks. It does not fetch a caller-selected remote old offer.

`inspect_accepted` is strictly read-only. It reports accepted history and current
high-water/revocation state without requiring an old historical timestamp to
remain fresh. This does NOT authorize a new network installation. The C++ core
must independently match the existing slot's immutable receipt/descriptor digest
against `descriptorSha256s`, revalidate its complete files and reject revoked
builds before an explicit installed-slot rollback. Application rollback never
resets the trust journal.

## Acceptance status

The initial remote stub failure is recorded in run 40656542985. Local checks
are limited to static validation, dependency advisory analysis and compilation.
Remote run 40663657869, isolated-trust job 122732631637, commit
`66ca2462f6a73ea063e8f1417f252f02b57b7443` / tree
`16adf983206980a4fa7c10fbf6ff9d22a32abfbb`, executed all 10 public C-ABI test
groups successfully with no skips (`Ran 10 tests in 3.418s`). This covered the
real HTTPS A/B/cache/replay tracer, invalid signatures/expiry/mix/descriptors,
runtime-override rejection, cancellation, Windows accepted-state fault points,
whole-directory loss/corruption, root dual thresholds/exact version/adoption
retention, fresh-TUF application counters/immutable IDs and persistent global
revocations. It does not validate later source automatically.

The additional real 60-second expiry slice runs without OS
clock changes or runtime overrides: a bounded remote issuer keeps ephemeral
keys only in memory, receives a public trigger after compilation, signs the
same-root short timestamp, then exits. Cached activation rejects actual expiry
while historical inspection remains available. This gate passed in run
40745705669, job 123018803603, commit
`82e71320a3604eedc0f804cefc050a446f502e30`: all 11 trust groups ran with no
skips in 63.455 seconds, including the signed timestamp's real 60-second wait.

The same source's real Qt fixture stable-entry job 123018803552 passed all four
groups in 30.734 seconds: authenticated A/B, bad-C retention, explicit safe
rollback, complete installed-file verification, known-revoked startup refusal,
and refusal to bypass existing trust by downgrading bootstrap to protocol 1.
Protocol-1 local developer startup without any trust history remains explicitly
unsigned and is not public publisher authentication. Startup verification and
process creation use the independent fixed helper under the same installation
lock; they do not change trust state or kill an already-running application.

These are isolated fixtures, not formal GSW health/rendering acceptance. The
same run still had an independent source-closure job failure, so these results
do NOT authorize production delivery or establish a complete release gate.
No production root, signing key, public release or channel promotion is configured.
