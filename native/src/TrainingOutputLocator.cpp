#include <QCoreApplication>
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
                QCoreApplication::translate("Workbench", "Active training recovery data is incomplete."));
    return false;
  }

  const QString markerPath = activeTrainingJobPath(projectRoot);
  if (!QDir().mkpath(QFileInfo(markerPath).absolutePath())) {
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Unable to create training recovery directory: "
                               "%1")
                    .arg(QFileInfo(markerPath).absolutePath()));
    return false;
  }

  QSaveFile marker(markerPath);
  if (!marker.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Unable to create training recovery record: %1")
                    .arg(marker.errorString()));
    return false;
  }
  const auto portablePath = [&projectRoot](const QString &path) {
    const QString relative = QDir(projectRoot).relativeFilePath(normalizedAbsolutePath(path));
    return relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)
        ? normalizedAbsolutePath(path) : relative;
  };
  const QJsonObject root{
      {QStringLiteral("version"), 1},
      {QStringLiteral("configurationPath"),
       portablePath(job.configurationPath)},
      {QStringLiteral("outputSceneRoot"),
       portablePath(job.outputSceneRoot)},
      {QStringLiteral("previewRecovered"), job.previewRecovered}};
  const QByteArray serialized =
      QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (marker.write(serialized) != serialized.size()) {
    marker.cancelWriting();
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Unable to write training recovery record: %1")
                    .arg(marker.errorString()));
    return false;
  }
  if (!marker.commit()) {
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Unable to commit training recovery record: %1")
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
                QCoreApplication::translate("Workbench", "Unable to open training recovery record: %1")
                    .arg(marker.errorString()));
    return {};
  }

  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(marker.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Invalid training recovery record: %1")
                    .arg(parseError.errorString()));
    return {};
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("version")).toInt() != 1) {
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Unsupported training recovery record."));
    return {};
  }

  ActiveTrainingJob job{
      root.value(QStringLiteral("configurationPath")).toString(),
      root.value(QStringLiteral("outputSceneRoot")).toString(),
      root.value(QStringLiteral("previewRecovered")).toBool()};
  if (!job.isValid()) {
    assignError(errorMessage,
                QCoreApplication::translate("Workbench", "Training recovery record is incomplete."));
    return {};
  }
  job.configurationPath = normalizedAbsolutePath(QDir(projectRoot).absoluteFilePath(job.configurationPath));
  job.outputSceneRoot = normalizedAbsolutePath(QDir(projectRoot).absoluteFilePath(job.outputSceneRoot));
  return job;
}

int nativeResumeIteration(const QString &outputSceneRoot) {
  if (outputSceneRoot.isEmpty()) return -1;
  const QDir root(QDir(outputSceneRoot).filePath(QStringLiteral(".gsw-resume")));
  QFile manifest(root.filePath(QStringLiteral("ready.json")));
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 16384) return -1;
  const QJsonObject data = QJsonDocument::fromJson(manifest.readAll()).object();
  const QString name = data.value(QStringLiteral("file")).toString();
  static const QRegularExpression pattern(QStringLiteral("^state-[0-9a-f]{32}\\.pth$"));
  const int iteration = data.value(QStringLiteral("iteration")).toInt(-1);
  const QFileInfo state(root.filePath(name));
  if (data.value(QStringLiteral("version")).toInt() != 1 || !pattern.match(name).hasMatch() ||
      iteration <= 0 || iteration >= data.value(QStringLiteral("total")).toInt() ||
      !state.isFile() || state.size() == 0 || state.isSymLink() ||
      QDir(state.canonicalPath()) != QDir(root.canonicalPath())) return -1;
  return iteration;
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
              QCoreApplication::translate("Workbench", "Unable to remove training recovery record: %1")
                  .arg(markerPath));
  return false;
}

} // namespace gsw
