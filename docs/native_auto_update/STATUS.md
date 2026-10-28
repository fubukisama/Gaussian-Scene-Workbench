# Auto-update delivery status

Status is intentionally split: implementation, isolated acceptance, local
delivery and public online operation. None is complete yet.

| Evidence | Exact source | Result | Scope |
| --- | --- | --- | --- |
| [First contracts](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40656542985) | `78579110cd54d533f7ff4c9f50eae54fe71b769e` | Expected RED: real HTTPS returns `download_not_implemented`; archive/gate CLIs not yet present | isolated, Windows Server2022, Qt6.8.3, Python3.12 |
| [Activation RED](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40658445549) | `9cc904a2a7576b93d31fe14f76a6aa1670013631` | `activation_not_implemented` asserted on the actual native call | isolated |
| [First download/archive/gate GREEN](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40658378285) | `547b2378f52c12241fbf48b99d4f469f9813dccb` | HTTPS succeeds; hidden source archive and unapproved-policy refusal pass; activation still RED | isolated |
| [Activation GREEN and resume RED](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40659572459) | `b4e46584b0601b6e2fcb372b66b60b683d868771` | Native full archive publishes B and preserves A/bootstrap; cancel-resume contract fails at retained-prefix assertion | isolated, no signed end-to-end claim |
| [First trust preparation](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40660699882) | `c50bfde` | Real fixture issuer succeeds; Rust dependency build hits deep Windows include path, before trust behavior | isolated infrastructure failure, not signature RED |

Production is not configured: no production root, approved complete release
policy, protected signing environment or public channel promotion. Ordinary
main pushes run isolated CI only. Local GSW/training has not been launched,
restarted, terminated or modified by these acceptance runs.
