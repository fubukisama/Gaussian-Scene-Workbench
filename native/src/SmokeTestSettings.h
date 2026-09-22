#pragma once

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <memory>

namespace gsw {
// Keep the returned owner alive until all application widgets/settings die.
// PIDs are reused on Windows; a PID-named persistent directory is not isolated.
inline std::unique_ptr<QTemporaryDir> isolateSmokeTestSettings(const QString &root) {
  if (!QDir().mkpath(root)) return {};
  auto directory = std::make_unique<QTemporaryDir>(QDir(root).filePath(QStringLiteral("run-XXXXXX")));
  if (!directory->isValid()) return {};
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory->path());
  return directory;
}
} // namespace gsw
