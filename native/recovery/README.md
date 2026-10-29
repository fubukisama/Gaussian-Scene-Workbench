# Native version recovery

The independent window derives its installation from its own fixed `bin/`
location. Opening it does not require a valid `current.json`. Production has no
runtime root, feed, key or time override; an unconfigured production Helper
continues to return `NOT_CONFIGURED` without an unsigned fallback.

The GUI only asks the fixed, ordinary, non-hardlinked Helper to list or recover
a build. The read-only catalog holds the existing installation lock, reads the
independent accepted/non-revoked ledger, mints historical admission and verifies
each complete installed slot. Local directories or receipts alone never grant
admission. Selecting and explicitly confirming a build invokes a new Helper
rollback request, which repeats history and complete-slot checks before the
atomic pointer operation. The GUI never writes a pointer or claims that process
creation, a catalog result or a historical build is application health.

Catalog display labels preserve normal Unicode. URI-like/control labels fall
back to the immutable build ID at the presentation boundary only. This does not
change the original admitted offer/descriptor and is not a trust decision.
URLs, IPC nonces and raw Helper stderr are not displayed or logged by the GUI.

AppLanguage refreshes existing text and controls without reconstructing the
list or the active Helper process. Cancel at confirmation starts no action.
Normal Close/Esc while a Helper command is active defers window destruction
until that command finishes. The GUI never starts a GSW application or training
process as a recovery result, and never kills or automatically retries a Helper
transaction.

## Explicit availability limit

The 120-second GUI command timer is **advisory**, not a cancellation guarantee.
It reports an unconfirmed outcome and does not kill/retry a possibly committed
transaction. If the Helper permanently hangs, the current safe-close deferral
also remains pending. Therefore this version must not be described as allowing
immediate close/cancel after timeout. A separate real-process safe-detach or
cooperative-cancellation seam and tests are required to close that P2 lifecycle
gap; they are not implied by a small successful recovery fixture.

The first chooser RED is run `40834717543`, commit `0ec86155`: the real signed
fixture root was prepared, but the placeholder window had no
`recoveryCandidates`. Current Green source/build checks do not replace a fresh
remote result. The private QtTest target uses the actual fixture Helper and
signed A/B history, signed B withdrawal, post-listing source tampering, explicit
confirmation/cancel, three languages and protected-state checks. It is excluded
from production targets and is not formal GSW renderer/training acceptance.
