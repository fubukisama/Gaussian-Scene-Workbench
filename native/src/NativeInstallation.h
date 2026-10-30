#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace gsw {

inline constexpr char kNativeLauncherName[] = "Gaussian Scene Workbench Launcher.exe";
inline constexpr char kNativeApplicationName[] = "Gaussian Scene Workbench.exe";

// Immutable versions plus a single atomically published current.json pointer.
// This validates local installation integrity, not publisher authenticity.
struct NativeLaunchPlan final {
  QString root;
  QString version;
  QString program;
  QString executableSha256;
  QString workingDirectory;
  QStringList arguments;
  QProcessEnvironment environment;
  QString error;
  [[nodiscard]] bool valid() const { return error.isEmpty() && !program.isEmpty(); }
};

[[nodiscard]] NativeLaunchPlan resolveNativeInstallation(
    const QString &root, bool diagnostics, const QStringList &arguments,
    const QProcessEnvironment &inheritedEnvironment);
// Protocol 1 remains a local integrity-only entry. Protocol 2 delegates to a
// fixed compiled helper, which revalidates trusted history and the whole slot.
// Inspection never creates the application; protocol 2 may create a headless
// verifier child. Starting holds the helper's installation lock through spawn.
[[nodiscard]] bool verifyNativeInstallationLaunch(const NativeLaunchPlan &, QString *error);
[[nodiscard]] bool startNativeInstallation(const NativeLaunchPlan &, bool diagnostics, QString *error);
// Explicit independent entry; does not parse current.json or start a version.
[[nodiscard]] bool startNativeRecovery(const QString &installationRoot, QString *error);
[[nodiscard]] QString nativeRecoveryInstallationForApplication(const QString &applicationPath);
[[nodiscard]] QString nativeInstallationErrorText(const QString &code);
[[nodiscard]] QString managedInstallationForApplication(const QString &applicationPath);
// Pinning keeps the stable launch command/icon/ID, while display resource 201
// belongs to the running application, even with a retained pre-rename bootstrap.
[[nodiscard]] bool setManagedTaskbarRelaunch(void *windowHandle,
                                          const QString &applicationPath,
                                          const QString &appUserModelId);
void clearManagedTaskbarRelaunch(void *windowHandle, const QString &applicationPath);
struct NativeShortcutInfo final {
  QString target, arguments, workingDirectory, description, error;
  int showCommand = 1;
  unsigned short hotkey = 0;
  [[nodiscard]] bool valid() const { return error.isEmpty(); }
};
[[nodiscard]] NativeShortcutInfo readNativeShortcut(const QString &path);
// Installation action only: never pins/unpins, reorders, or executes a link.
[[nodiscard]] bool writeNativeLauncherShortcut(const QString &path, const QString &launcher,
                                              const QString &arguments, const QString &appUserModelId,
                                              QString *diagnostic = nullptr);
} // namespace gsw
