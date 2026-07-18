#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace gsw {

struct MediaProjectBootstrapPlan {
  QString displayName;
  QString rootPath;
  QString projectFilePath;
};

[[nodiscard]] QString suggestedMediaProjectName(
    const QStringList &sourcePaths);
[[nodiscard]] QString suggestedMediaSceneName(
    const QStringList &sourcePaths);
[[nodiscard]] std::optional<MediaProjectBootstrapPlan>
planMediaProjectBootstrap(const QStringList &sourcePaths,
                          const QString &displayName = {},
                          QString *errorMessage = nullptr);
bool materializeMediaProjectBootstrap(
    const MediaProjectBootstrapPlan &plan,
    QString *errorMessage = nullptr);

} // namespace gsw
