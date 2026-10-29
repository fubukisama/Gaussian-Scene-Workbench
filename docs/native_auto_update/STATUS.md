# Auto-update delivery status

Status is intentionally split: implementation, isolated acceptance, local
delivery and public online operation. None is complete yet. On 2026-10-29 the
user selected **isolated acceptance and a local research preview; public
automatic updates remain disabled**. This does not authorize production keys,
secrets, public candidate uploads or channel promotion.

| Evidence | Exact source | Result | Scope |
| --- | --- | --- | --- |
| [First contracts](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40656542985) | `78579110cd54d533f7ff4c9f50eae54fe71b769e` | Expected RED: real HTTPS returns `download_not_implemented`; archive/gate CLIs not yet present | isolated, Windows Server2022, Qt6.8.3, Python3.12 |
| [Activation RED](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40658445549) | `9cc904a2a7576b93d31fe14f76a6aa1670013631` | `activation_not_implemented` asserted on the actual native call | isolated |
| [First download/archive/gate GREEN](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40658378285) | `547b2378f52c12241fbf48b99d4f469f9813dccb` | HTTPS succeeds; hidden source archive and unapproved-policy refusal pass; activation still RED | isolated |
| [Activation GREEN and resume RED](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40659572459) | `b4e46584b0601b6e2fcb372b66b60b683d868771` | Native full archive publishes B and preserves A/bootstrap; cancel-resume contract fails at retained-prefix assertion | isolated, no signed end-to-end claim |
| [First trust preparation](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40660699882) | `c50bfde` | Real fixture issuer succeeds; Rust dependency build hits deep Windows include path, before trust behavior | isolated infrastructure failure, not signature RED |
| [Five component groups GREEN](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40718734156) | `2e0cac42283619f96aa3864eacfe412608005d86` | TUF11 tests including real expiry; stable-entry, UI7 tests including no foreground change, native contracts and source/policy fixtures pass | isolated; not formal GSW installation/health or public channel acceptance |
| [Developer and downgrade regression](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40745705669) | `82e7132` | Six groups pass; actual source closure still fails, with a bounded metadata diagnostic | isolated; overall failure, not delivery approval |
| [Current confirmed source boundary](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40800050974) | `72472bb21c015c880b3e43af217c7afccfd8f35d` | Six groups pass. Actual registry closure reaches independent raw-archive checks but rejects the standard Cargo vendor projection; the original archive remains retained | isolated; source closure NOT passed |
| [Actual full source closure](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40811574832) | `8e9dcb8898343fae67e29dce6cffea3683264660` | Source closure PASS:188 registry archives,3209 exact Git files,12515 original registry files,12446 standard vendor files,69 filtered metadata files retained in raw archives. Normal approved redirect/UI/trust/developer tests pass. Health has genuine lifecycle failures under diagnosis; profile functionality passes but evidence upload needs workspace plumbing | isolated; not delivery approval or application acceptance |
| [Health and retry diagnostic boundary](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40817655279) | `9f939b3e25bc2a064a81aa5be615961651e932c7` | Original A-B-badC-A lifecycle passes; health alone is UNCONFIRMED. Retained traces show Qt construction at2.374s, painted readiness at6.004s and normal exit0 at6.157s, after the fixture-only1.5s pipe observation had expired. Actual HTTPS503 retry is RED with `download_http_error`; source/UI/trust/developer/policy groups pass | isolated diagnostic evidence; fixes require a new remote GREEN |
| [Compile-time profile evidence](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40817655279/job/123262413639) | `9f939b3e25bc2a064a81aa5be615961651e932c7` | Default/configured probes4tests and unsafe-input rejection35subcases pass, with the corrected in-workspace evidence upload | isolated developer probes only; no production package configured |
| [Official disabled engine component](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40817655291) | `9f939b3e25bc2a064a81aa5be615961651e932c7` | Official Helper default returns NOT_CONFIGURED; full component ZIP, exact Git/raw-crate source, fresh merge and badZIP/wrongsource/runtime-conflict rejection pass. Uploaded artifact12873258497 is retained | component acceptance only; merge uses a non-executable owned app fixture, not formal GSW startup |
| [Health/503 consistency GREEN](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40818699252) | `e48d535683d5d5269e422971e53c7ea104f9fd4e` | All8 component jobs pass, including authenticated fixture health, bounded real503 retry, source closure and profile evidence upload. Independent official disabled Engine run40818699154 also passes | isolated components; applicable packet interruption/resource/recovery/first-migration/live gaps remain |
| [Resource failures and interruption RED](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40820051072) | `7ca9d14d790d2a6f9f950199935b90d813b74891` | Real ACL refusal/retry, held-current refusal/retry and actual53,628,928B available on a fresh64MiB D-backedVHD pass without changing old files. Interrupted HTTPS body is RED at `download_network_error`; four process-fault cases are RED because the separately compiled fixture checkpoints are not yet connected | partial SPACE activation evidence; not download/rollback-space coverage or whole-packet acceptance |
| [Actual interruption recovery and next Range boundary](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40820732194) | `722ffd9e005cfe21d4af22f27119d3df4b9a0c82` | Four real process-termination/cross-process-lock/independent app-local console-slot-probe cases pass. Interrupted HTTPS retry passes.13 download rows pass; changed-ETag and invalid-Content-Range rows are semantic RED. Health positive plus missing/late/wrong-peer neutral fixture cases pass | shared-core developer-admission/Qt-fixture scope; not formal GSW, online first installation or GPU optimizer acceptance |

Health confirmation/recovery, normal redirect/retry and compile-time production
profile configuration now have independent first-RED slices. Their queued or
running checks are not PASS evidence. Production profile syntax probes are
separate developer products and never configure a production library/package.

The health consistency fix removes the fixture-only1.5s observation shortcut
and uses the existing product15s bound in both builds. It does not increase a
product limit, move the clock origin, change the fixture100/150ms lifecycle or
25s test bound, or weaken process-birth/image/window/nonce/ACK checks. Missing
ACK, startup beyond the bound and normal exit without proof remain neutral
UNCONFIRMED outcomes, not evidence of a bad version or automatic rollback.

The earlier Qt A/B lifecycle fixture uses real windows and the real stable
launcher/helper, but its scope is explicitly not the formal GSW renderer or
3DGS/2DGS full optimizer-state training acceptance. No such quality or backend
claim is inferred from a lifecycle or UI test.

Applicable isolated gaps remain separate from public authorization: independent
recovery UI, authenticated test-only first-install/bootstrap migration, actual
cross-process contention and kill-at-extract/slot/pointer recovery, ACL/occupied/
insufficient-space failures, and overlapping old application/owned worker/queue
during publication. Existing component success is not completion of these packet
contracts. A fresh local research-preview file bundle is not DEL-01 managed
delivery and does not upgrade existing current/shortcut entry points.

Production is not configured: no production root, approved complete release
policy, protected signing environment or public channel promotion. Ordinary
main pushes run isolated CI only. Local GSW/training has not been launched,
restarted, terminated or modified by these acceptance runs.
