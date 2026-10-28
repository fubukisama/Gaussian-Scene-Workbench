# GSW native update trust adapter

This static library implements the private-to-the-native-helper C seam in
`include/gsw_update_trust.h`. No end-user Python, shell, Git or Rust runtime is
required. The C++ owner must hold the shared installation `.publish.lock`.

Current tracer status: **intentional compile stub, not an updater**. The
production build has no provisioned trust root and returns `NOT_CONFIGURED`.
The separately compiled `fixture-profile` build returns
`OPERATION_NOT_IMPLEMENTED` until the real remote A -> B tracer has recorded
its expected red result. It cannot be used as production release evidence.

The planned trust implementation is pinned to AWS tough `0.24.0`, source commit
`98d8eb8b2ce63515d9b4981c938ef6453c5b5771`, under its MIT license option. GSW
does not reimplement TUF cryptographic or role-verification algorithms. Required
GSW adaptations include bounded strict JSON, approved HTTPS transport, atomic
trust-state persistence and a top-level nondelegated release-descriptor profile.

The test profile is a compile-time public fixture input selected using
`GSW_UPDATER_FIXTURE_PROFILE_PATH`. Production does not read this environment
variable, accept test roots, or accept runtime trust/feed overrides. Fixture
private keys must never be embedded or packaged.

