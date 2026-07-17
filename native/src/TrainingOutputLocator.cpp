#include "TrainingOutputLocator.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace gsw {

TrainingOutputScene findLatestTrainingOutputScene(const QString &outputSceneRoot) {
  const QDir pointCloudRoot(
      QDir(outputSceneRoot).filePath(QStringLiteral("point_cloud")));
  if (!pointCloudRoot.exists()) {
    return {};
  }

  static const QRegularExpression iterationPattern(
      QStringLiteral("^iteration_([0-9]+)$"));
  TrainingOutputScene result;
  const QFileInfoList iterationDirectories = pointCloudRoot.entryInfoList(
      QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
  for (const QFileInfo &directory : iterationDirectories) {
    const QRegularExpressionMatch match =
        iterationPattern.match(directory.fileName());
    if (!match.hasMatch()) {
      continue;
    }

    bool parsed = false;
    const int iteration = match.captured(1).toInt(&parsed);
    if (!parsed || iteration < result.iteration) {
      continue;
    }

    const QFileInfo sceneFile(
        QDir(directory.absoluteFilePath()).filePath(QStringLiteral("point_cloud.ply")));
    if (!sceneFile.exists() || !sceneFile.isFile()) {
      continue;
    }

    result.path = QDir::cleanPath(sceneFile.absoluteFilePath());
    result.iteration = iteration;
  }
  return result;
}

} // namespace gsw
