#include "TrainingOutputLocator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace gsw {

namespace {
QString activeTrainingJobPath(const QString &projectRoot) {
  return QDir(projectRoot).filePath(
      QStringLiteral(".gsw/jobs/active-training.json"));
}

QString normalizedAbsolutePath(const QString &path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void assignError(QString *target, const QString &message) {
  if (target != nullptr) {
    *target = message;
  }
}
} // namespace

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

bool saveActiveTrainingJob(const QString &projectRoot,
                           const ActiveTrainingJob &job,
                           QString *errorMessage) {
  if (projectRoot.isEmpty() || !job.isValid()) {
    assignError(errorMessage,
                QStringLiteral("Active training recovery data is incomplete."));
    return false;
  }

  const QString markerPath = activeTrainingJobPath(projectRoot);
  if (!QDir().mkpath(QFileInfo(markerPath).absolutePath())) {
    assignError(errorMessage,
                QStringLiteral("Unable to create training recovery directory: "
                               "%1")
                    .arg(QFileInfo(markerPath).absolutePath()));
    return false;
  }

  QSaveFile marker(markerPath);
  if (!marker.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create training recovery record: %1")
                    .arg(marker.errorString()));
    return false;
  }
  const QJsonObject root{
      {QStringLiteral("version"), 1},
      {QStringLiteral("configurationPath"),
       normalizedAbsolutePath(job.configurationPath)},
      {QStringLiteral("outputSceneRoot"),
       normalizedAbsolutePath(job.outputSceneRoot)}};
  const QByteArray serialized =
      QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (marker.write(serialized) != serialized.size()) {
    marker.cancelWriting();
    assignError(errorMessage,
                QStringLiteral("Unable to write training recovery record: %1")
                    .arg(marker.errorString()));
    return false;
  }
  if (!marker.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to commit training recovery record: %1")
                    .arg(marker.errorString()));
    return false;
  }
  return true;
}

ActiveTrainingJob loadActiveTrainingJob(const QString &projectRoot,
                                        QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QFile marker(activeTrainingJobPath(projectRoot));
  if (!marker.exists()) {
    return {};
  }
  if (!marker.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open training recovery record: %1")
                    .arg(marker.errorString()));
    return {};
  }

  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(marker.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                QStringLiteral("Invalid training recovery record: %1")
                    .arg(parseError.errorString()));
    return {};
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("version")).toInt() != 1) {
    assignError(errorMessage,
                QStringLiteral("Unsupported training recovery record."));
    return {};
  }

  ActiveTrainingJob job{
      root.value(QStringLiteral("configurationPath")).toString(),
      root.value(QStringLiteral("outputSceneRoot")).toString()};
  if (!job.isValid()) {
    assignError(errorMessage,
                QStringLiteral("Training recovery record is incomplete."));
    return {};
  }
  job.configurationPath = normalizedAbsolutePath(job.configurationPath);
  job.outputSceneRoot = normalizedAbsolutePath(job.outputSceneRoot);
  return job;
}

bool clearActiveTrainingJob(const QString &projectRoot,
                            QString *errorMessage) {
  const QString markerPath = activeTrainingJobPath(projectRoot);
  if (!QFileInfo::exists(markerPath) || QFile::remove(markerPath)) {
    if (errorMessage != nullptr) {
      errorMessage->clear();
    }
    return true;
  }
  assignError(errorMessage,
              QStringLiteral("Unable to remove training recovery record: %1")
                  .arg(markerPath));
  return false;
}

} // namespace gsw
