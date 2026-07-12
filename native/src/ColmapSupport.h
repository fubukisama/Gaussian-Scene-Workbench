#pragma once

#include <QString>

namespace gsw {

[[nodiscard]] QString datasetImageDirectory(const QString &datasetPath);
[[nodiscard]] bool hasRecognizedColmapScene(const QString &datasetPath);
[[nodiscard]] bool hasRecognizedTrainingScene(const QString &datasetPath);
[[nodiscard]] bool hasColmapWorkingData(const QString &datasetPath);
[[nodiscard]] QString findColmapExecutable(const QString &repositoryRoot,
                                           const QString &preferredPath = {});

} // namespace gsw
