#include "TrainingPauseController.h"
#include "CatalogPathGuard.h"
#include "TrainingOutputLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace gsw {
namespace {
bool incomplete(QString *errorMessage) {
  if (errorMessage) *errorMessage = QCoreApplication::translate(
      "Workbench", "Active training recovery data is incomplete.");
  return false;
}

QString absolute(const QString &path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool samePath(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
  constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
  constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
  return absolute(left).compare(absolute(right), sensitivity) == 0;
}

bool noTraversal(const QString &path) {
  const QString normalized = QDir::fromNativeSeparators(path);
  return QDir::isAbsolutePath(normalized) &&
      !normalized.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

bool noLinks(const QString &path) {
  QFileInfo node(path);
  while (true) {
    if (node.isSymLink() || node.isJunction()) return false;
    const QString parent = node.absolutePath();
    if (parent == node.absoluteFilePath()) return true;
    node = QFileInfo(parent);
  }
}

QJsonObject readObject(const QString &path, qint64 limit = 16 * 1024 * 1024) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return {};
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
      ? document.object() : QJsonObject{};
}
} // namespace

bool requestActiveTrainingPause(const QString &projectRoot, QString *errorMessage) {
  if (errorMessage) errorMessage->clear();
  if (projectRoot.isEmpty() || !QFileInfo(projectRoot).isDir() || !noLinks(projectRoot))
    return incomplete(errorMessage);
  const QDir project(absolute(projectRoot));
  const QString jobsRoot = project.filePath(QStringLiteral(".gsw/jobs"));
  const QString expectedStore = QDir(jobsRoot).filePath(QStringLiteral("training"));
  const QString marker = QDir(jobsRoot).filePath(QStringLiteral("active-training.json"));
  if (!safeCatalogChild(project.path(), marker) || !QFileInfo(expectedStore).isDir() ||
      !noLinks(expectedStore)) return incomplete(errorMessage);
  const auto active = loadActiveTrainingJob(project.path());
  if (!active.isValid() || !safeCatalogChild(jobsRoot, active.configurationPath) ||
      !QFileInfo(active.configurationPath).isFile()) return incomplete(errorMessage);
  const auto config = readObject(active.configurationPath, 1024 * 1024);
  const QString backend = config.value(QStringLiteral("backend")).toString();
  const QString store = config.value(QStringLiteral("jobStore")).toString();
  const QString outputRoot = config.value(QStringLiteral("outputRoot")).toString();
  const QString outputName = config.value(QStringLiteral("outputScene")).toString();
  if (!config.value(QStringLiteral("nativeCheckpoint")).toBool() ||
      (backend != QStringLiteral("3dgs") && backend != QStringLiteral("2dgs")) ||
      !noTraversal(store) || !samePath(store, expectedStore) ||
      !noTraversal(config.value(QStringLiteral("projectRoot")).toString()) ||
      !samePath(config.value(QStringLiteral("projectRoot")).toString(), project.path()) ||
      !noTraversal(config.value(QStringLiteral("datasetPath")).toString()) ||
      !noTraversal(outputRoot) || outputName.isEmpty() || outputName == QStringLiteral(".") ||
      outputName == QStringLiteral("..") || outputName.contains(QLatin1Char('/')) ||
      outputName.contains(QLatin1Char('\\')) ||
      !samePath(QDir(outputRoot).filePath(outputName), active.outputSceneRoot) ||
      !QFileInfo(active.outputSceneRoot).isDir() || !noLinks(active.outputSceneRoot))
    return incomplete(errorMessage);

  static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{32}$"));
  QJsonObject control;
  int matching = 0;
  const QDir jobStore(expectedStore);
  const auto entries = jobStore.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  for (const auto &entry : entries) {
    if (!idPattern.match(entry.completeBaseName()).hasMatch()) continue;
    if (!safeCatalogChild(expectedStore, entry.absoluteFilePath())) return incomplete(errorMessage);
    const auto job = readObject(entry.absoluteFilePath());
    if (job.value(QStringLiteral("status")).toString() != QStringLiteral("running") ||
        job.value(QStringLiteral("stage")).toString() != QStringLiteral("train") ||
        job.value(QStringLiteral("output_scene")).toString() != outputName) continue;
    if (job.value(QStringLiteral("id")).toString() != entry.completeBaseName() ||
        job.value(QStringLiteral("backend")).toString() != backend ||
        !samePath(job.value(QStringLiteral("dataset_path")).toString(),
                  config.value(QStringLiteral("datasetPath")).toString()))
      return incomplete(errorMessage);
    control = job.value(QStringLiteral("native_control")).toObject();
    if (++matching > 1) return incomplete(errorMessage);
  }
  const QString session = control.value(QStringLiteral("session")).toString();
  const QString expectedRequest = jobStore.filePath(QStringLiteral("pause-%1.json").arg(session));
  const QString requestPath = control.value(QStringLiteral("request")).toString();
  if (matching != 1 || !idPattern.match(session).hasMatch() ||
      control.value(QStringLiteral("backend")).toString() != backend ||
      !noTraversal(control.value(QStringLiteral("output")).toString()) ||
      !samePath(control.value(QStringLiteral("output")).toString(), active.outputSceneRoot) ||
      !noTraversal(requestPath) || !samePath(requestPath, expectedRequest) ||
      !noLinks(expectedRequest)) return incomplete(errorMessage);
  if (QFileInfo::exists(expectedRequest)) {
    const auto previous = readObject(expectedRequest, 4096);
    return previous.value(QStringLiteral("session")).toString() == session && previous.size() == 1
        ? true : incomplete(errorMessage);
  }

  QSaveFile request(expectedRequest);
  request.setDirectWriteFallback(false);
  const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("session"), session}}).toJson();
  if (!request.open(QIODevice::WriteOnly) || request.write(bytes) != bytes.size()) {
    request.cancelWriting();
    if (errorMessage) *errorMessage = QCoreApplication::translate(
        "Workbench", "Unable to write training recovery record: %1").arg(request.errorString());
    return false;
  }
  if (!request.commit()) {
    if (errorMessage) *errorMessage = QCoreApplication::translate(
        "Workbench", "Unable to commit training recovery record: %1").arg(request.errorString());
    return false;
  }
  return true;
}
} // namespace gsw
