# Independent synthetic-Qt Alt menu-entry contract

Status: **IMPLEMENTED / NOT RUN / NOT ACCEPTED**. This additive probe does not
remove, waive, replace or reinterpret any original F10 assertion or receipt.
The three-locale Forms run 40828325789 observation remains
`NOT_RUN_F10_NOT_OBSERVED`; it is not a native Windows/physical F10 failure.
The independent native-input preflight draft remains unarmed and unintegrated.

## Scope and original requirements

The user-supplied instruction package at
`D:/Codex/deliveries/GSW_UI_Upgrade_Agent_20261028/` specifies general keyboard
and menu access, not F10 as its sole entry key:

- `ACCEPTANCE_MATRIX.md:28` (SHELL-01): actual wide/narrow menu operation and
  reachability of important actions.
- `ACCEPTANCE_MATRIX.md:40` (A11Y-01): keyboard focus, tooltips, accessible names
  and disabled reasons; Tab/shortcut/focus checks.
- `AGENTS.md:67` and `:106`: preserve familiar shortcuts, focus and keyboard
  operation. Adding this QA route does not add a production shortcut.
- `ACCEPTANCE_MATRIX.md:59`: do not remove old assertions or extend deadlines.

The repository's surface matrix also requires general menu access and shortcuts
(`ACCEPTANCE_STATUS.json:1048-1055`, SH-01). An Alt result only contributes the
specifically exercised menu paths and states. It does not close full A11Y-01,
all surface states, Windows native input, physical keyboard support or F10.

## Independent entry selection and evidence

The default entry route remains `qt_f10`, including the unchanged F10 attempt,
three existing `native_menu_keyboard_{zh_CN,en_US,ja_JP}` CTests, 30-second
individual CTest timeout and `menu-keyboard-results.json` filename.

Only the explicit environment value `GSW_UI_MENU_ENTRY_METHOD=qt_alt` selects the
additional `native_menu_alt_keyboard_{zh_CN,en_US,ja_JP}` CTests. These reuse the
original language smoke-test executable/flag and isolated remote form-capture
adapter, have their own 30-second timeouts, and save
`menu-alt-keyboard-results.json`. Unknown nonempty route values do not silently
fall back. Both routes retain the same per-case 2500 ms budget, 96 key-pair
bound, 15 ms fixed settling and reserved ESC cleanup budget. An Alt failure
does not invoke F10, retry Alt, extend a deadline or start another fixture.

All receipts identify **synthetic Qt widget key events**, record `entryRoute`,
and disclaim native OS, physical keyboard and full accessibility acceptance.
Alt evidence sets `f10Observed` to JSON null, including skipped cases: F10 was
not examined by that route. Old F10 records are never overwritten by Alt.

## Qt 6.8.3 source basis and modifier semantics

The remote workflow pins Qt 6.8.3. Qt's documented
[`SH_MenuBar_AltKeyNavigation`](https://doc.qt.io/qt-6.8/qstyle.html#StyleHint-enum)
describes Alt followed by arrow navigation. The version-pinned
[`QMenuBar::eventFilter` implementation](https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/widgets/qmenubar.cpp#L1463-L1501)
arms on visible, style-enabled `ShortcutOverride` for Alt with accessor modifiers
`AltModifier`, and enters keyboard mode on Alt release. This is an alternative
Qt menu-entry path, not evidence that native/physical F10 works.

The probe sends only `QKeyEvent` KeyPress/KeyRelease through public `sendEvent`
to the existing owned focus/popup receiver. It never directly sends/accepts a
ShortcutOverride, calls a private Qt helper, assigns focus, opens a menu or
triggers an action. The passive application event filter returns false for
every event and records receiver identity, ownership, event type/key, actual
modifiers, spontaneity and the active synchronous helper-dispatch phase.

Qt 6.8.3 has a modifier-key accessor transformation:
[`QKeyEvent::modifiers()`](https://github.com/qt/qtbase/blob/v6.8.3/src/gui/kernel/qevent.cpp#L1468-L1480)
XORs AltModifier for Key_Alt. The non-spontaneous QWidget KeyPress path in
[`QApplication::notify`](https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/kernel/qapplication.cpp#L2663-L2674)
passes that accessor to
[`qt_sendShortcutOverrideEvent`](https://github.com/qt/qtbase/blob/v6.8.3/src/gui/kernel/qwindowsysteminterface.cpp#L1175-L1199),
which constructs another QKeyEvent. Consequently, storing AltModifier in this
direct synthetic Alt press is expected to expose NoModifier in the press and
AltModifier in the automatically derived ShortcutOverride (two XORs). The
release stores AltModifier so its accessor exposes NoModifier. The receipt
records both construction parameters and actual accessor values; these details
are not claimed to reproduce physical/Win32 modifier semantics.

## Falsifiable acceptance gates

Starting from the already visible/active original owned main window and owned
focus control, with no other modal/popup, the following must all be observed:

1. The actual menu's Alt-navigation style hint is enabled. A passive observer
   sees the helper's non-spontaneous Alt KeyPress and correctly modified,
   automatically generated ShortcutOverride in the same synchronous dispatch.
2. The helper's non-spontaneous Alt KeyRelease is observed with NoModifier;
   afterward the real original menu bar naturally owns focus, its active action
   belongs to its existing action list, and no popup was forced open. Missing
   entry remains `NOT_RUN_ALT_ENTRY_NOT_OBSERVED`, not PASS.
3. Actual Right/Down events reach the originally specified existing menu/action
   graph and visible popup objects. The unchanged independent three-language
   QAccessible name/role/disabled/checked/explanation expectations must pass.
4. ESC closes all real owned menu popups and naturally restores the exact
   original focus object. No QAction is triggered, and the same public business
   snapshot is unchanged. Foreign receivers/popups are rejected, not operated.
5. The passive record is not truncated (fixed maximum 2048 event observations).
   Absence of a required event, wrong ownership, lost focus/state or exhausted
   bounds cannot be repaired by focus/show/trigger calls, retries or fallback.

It is a valid discriminating outcome for Alt to pass while the original F10
receipt remains NOT_RUN. Static source review predicts a testable path; it does
not establish that the remote runner will expose it or that the unchanged
assistive disabled-reason assertions will pass.

No application/test/GUI/GL process or keyboard input was run locally for this
change. Static/build verification and future remote execution are separate
acceptance gates, with source-bound receipts and logs still required.
