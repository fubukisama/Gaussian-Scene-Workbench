# Update UI integration boundary

The new dialog is a separate acceptance surface, not one of the existing
baseline dialog captures. `native/updater-ui` builds both the actual production
UI code and a distinct fixture-only variant. The fixture locator is compiled
out of production and is not a runtime option of the helper or application.
Its process fixture validates widgets/state/language behavior, not TLS or TUF;
the separate native stable-entry/HTTPS tests validate those boundaries.

The UI never raises or activates a window automatically. Background checks
default to enabled and are throttled to once per day; automatic preparation is
opt-in. Download/apply operate on the next launch pointer, not the current
window or training process. Release notes use QPlainTextEdit, never remote HTML.
Language changes retranslate widgets and current state without restarting the
helper. Preferences, skipped build IDs, channel and deferral use QSettings.

## Shared-file integration

The MainWindow Help hook and native application source list are now integrated.
The normal workbench language smoke checks the actual menu/controller and the
absence of a startup update dialog. The separate update-UI acceptance still
checks its real widgets and helper protocol. These code hooks do not configure
production trust or authorize bootstrap deployment.

1. `native/src/MainWindow.cpp` includes `NativeUpdateUi.h`.
2. Immediately after creating `helpMenu`, it calls
   `installNativeUpdateUi(helpMenu, this);`.
3. `src/NativeUpdateUi.cpp` and `src/NativeUpdateUi.h` are in the existing native
   application source list. They use the already-linked AppLanguage,
   NativeInstallation and Qt Widgets; the app does not link Rust/TUF itself.
4. Package the separately built native helper only through the verified
   bootstrap/managed delivery procedure. An ordinary app-slot update must not
   overwrite bootstrap binaries. Missing helper or production root is an
   actionable failure, never an "up to date" result.

Health/startup-guard/recovery hooks require their own explicit integration and
acceptance. This dialog does not turn process creation into a health claim.
