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
  QString workingDirectory;
  QStringList arguments;
  QProcessEnvironment environment;
  QString error;
  [[nodiscard]] bool valid() const { return error.isEmpty() && !program.isEmpty(); }
};

[[nodiscard]] NativeLaunchPlan resolveNativeInstallation(
    const QString &root, bool diagnostics, const QStringList &arguments,
    const QProcessEnvironment &inheritedEnvironment);
[[nodiscard]] QString nativeInstallationErrorText(const QString &code);
[[nodiscard]] QString managedInstallationForApplication(const QString &applicationPath);
// Ensures pinning the running version keeps pointing to the stable launcher.
[[nodiscard]] bool setManagedTaskbarRelaunch(void *windowHandle,
                                          const QString &applicationPath,
                                          const QString &appUserModelId);
void clearManagedTaskbarRelaunch(void *windowHandle, const QString &applicationPath);
// Installation action only: never pins/unpins, reorders, or executes a link.
[[nodiscard]] bool writeNativeLauncherShortcut(const QString &path, const QString &launcher,
                                              const QString &arguments, const QString &appUserModelId,
                                              QString *diagnostic = nullptr);
} // namespace gsw
