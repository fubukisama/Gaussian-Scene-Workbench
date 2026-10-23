#pragma once

#include <QProcessEnvironment>
#include <QStringList>

namespace gsw {

struct RuntimeComponentEnvironmentResult {
  QProcessEnvironment environment;
  QString errorCode;
  QString errorDetail;
  QStringList inserted;
  QStringList overridden;
  [[nodiscard]] bool ready() const { return errorCode.isEmpty(); }
};

// For the trusted executable directory only, never for component JSON paths.
[[nodiscard]] QString runtimeComponentInstallationRoot(const QString &applicationDirectory);
// Missing per-install configuration preserves every inherited value. Existing
// keys (including explicit empty values) take precedence; no global mutation.
[[nodiscard]] RuntimeComponentEnvironmentResult resolveRuntimeComponents(
    const QString &installationRoot, const QProcessEnvironment &inherited);
[[nodiscard]] QString runtimeComponentEnvironmentError(
    const RuntimeComponentEnvironmentResult &result);

} // namespace gsw
