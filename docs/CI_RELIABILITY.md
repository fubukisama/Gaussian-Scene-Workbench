# Result-path and persistent-control reliability

These incremental native fixes apply to shared 3DGS/2DGS workflows. The preview version label is not a substitute for the exact source commit, installed payload and independent CI receipts.

## Equivalent Windows output names

Windows can expose an existing directory through both a long name and an 8.3 alias. Completed-result delivery now checks the output directory and its ancestors without following reparse points, then uses its canonical spelling only for containment checks. Recorded model and receipt paths retain their original spelling. The raw training PLY, camera/scale metadata and optimizer checkpoints are not reinitialized or rewritten.

Import/removal containment rules are unchanged. Root/ancestor links and linked model directories remain rejected; recursive removal unlinks a reparse entry rather than following it into another tree. No volume short-name policy, registry setting or blanket extended-path prefix is required. A volume that supplies no distinct 8.3 alias is reported as such in acceptance, not silently treated as a real-alias test pass.

## Pause while metadata is being published

Persistent observers still validate the live OS process identity, session, plan/data hashes and fresh heartbeat for each command. On Windows, their bounded JSON reader opens one no-follow handle with read/write/delete sharing so an atomic metadata publisher's rename/delete access does not by itself make a valid lease unreadable. It rejects reparse entries, directories, changed or oversized contents, invalid JSON and invalid/closed sessions. It does not cache an earlier valid lease, retry an invalid command, extend its validity window or weaken acknowledgment deadlines.

The public regression uses a real detached CPU coordinator and keeps a fresh, complete lease open with replacement-compatible access across the pause call. A closed lease must still reject the command; restoring a valid lease must produce the normal pause acknowledgment and owned-process exit. Existing pause, checkpoint-preserving cancellation, detach, shutdown and destruction assertions remain.

The mechanism is grounded in [Qt 6.8.3 file-open sharing](https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/io/qfsfileengine_win.cpp#L63-L91), [Windows sharing compatibility](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew), and [QSaveFile atomic publication](https://doc.qt.io/qt-6.8/qsavefile.html). A deterministic reproduction is evidence for this failure mechanism; the exact refusal branch in an older CI log cannot be recovered if that log did not record it.

## Acceptance boundaries

Local affected tests, genuine Setup/source/runtime validation, local shortcut delivery and complete exact-commit cloud CI are separate records. Existing CUDA training, densification and Adam/RNG full-state proofs remain historical unless rerun; these CPU path/control tests do not establish long-run convergence, physical scale accuracy or material surface accuracy. Cameras, declared units, held-out image isolation, training algorithms and original research files are outside these changes.
