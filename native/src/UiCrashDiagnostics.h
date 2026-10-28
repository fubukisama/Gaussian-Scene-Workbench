#pragma once

#include <QString>

namespace gsw {

// Remote synthetic-QA only. Requires QApplication on its UI thread, Windows,
// GITHUB_ACTIONS=true, an explicit --smoke-test* argument and an existing,
// non-link gswSmokeSettingsDirectory. The caller attests that this process
// contains only owned synthetic fixtures, never user projects/data/secrets.
//
// absoluteOwnedDir is a local absolute directory with non-link ancestors.
// Installation reserves CREATE_NEW qa-crash-<PID>.dmp/.txt paths; never overwrites.
// Only the first selected severe first-chance SEH code is observed. A later
// application handler may still handle it: a dump alone does not prove an
// unhandled exception. The VEH always returns EXCEPTION_CONTINUE_SEARCH and
// never edits the exception context or selects a process exit code.
//
// A prestarted Win32 worker writes MiniDumpNormal|MiniDumpWithThreadInfo;
// no full-memory/private-memory/handle/token streams. Even a normal dump can
// contain thread-stack data: upload only these explicitly owned QA processes.
// The VEH uses no Qt, CRT logging, symbol engine or heap containers. Its bounded
// 3-second worker handshake is best-effort; loader deadlocks, exhausted/corrupt
// stacks, fail-fast and process termination can prevent a complete dump.
// The <=16 KiB text has code/address/module offsets and at most 32 native
// handler-thread stack addresses (not a symbolized fault-context stack).
// Fault-thread EXCEPTION_POINTERS are copied into the dump exception stream.
//
// No installation or filesystem writes on ordinary launches. Returns false
// for any gate/setup failure or duplicate installation. No local test runs,
// downloads, process restarts, security-policy changes or error suppression.
bool installUiCrashDiagnostics(const QString &absoluteOwnedDir);

} // namespace gsw
