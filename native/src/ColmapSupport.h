#pragma once

#include <QString>
#include <QStringList>

namespace gsw {

[[nodiscard]] QString datasetImageDirectory(const QString &datasetPath);
[[nodiscard]] bool hasRecognizedColmapScene(const QString &datasetPath);
[[nodiscard]] bool hasRecognizedTrainingScene(const QString &datasetPath);
[[nodiscard]] bool hasColmapWorkingData(const QString &datasetPath);
[[nodiscard]] QString findVersionedColmapExecutable(
    const QString &installRoot);
[[nodiscard]] QString findColmapExecutable(const QString &repositoryRoot,
                                           const QString &preferredPath = {},
                                           const QStringList &searchVolumeRoots = {});

} // namespace gsw
