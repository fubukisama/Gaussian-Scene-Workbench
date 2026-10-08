#include "GenerationHistoryStore.h"

#include "CatalogPathGuard.h"
#include "WorkspaceDocument.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace gsw {
namespace {
constexpr qint64 kMetadataLimit = 1024 * 1024;

bool safeId(const QString &id) {
  static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,95}$"));
  return pattern.match(id).hasMatch();
}

QString absolutePath(const QString &path) {
  return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool noTraversal(const QString &path) {
  return !path.contains(QChar(0)) &&
      !QDir::fromNativeSeparators(path).split(QLatin1Char('/')).contains(QStringLiteral(".."));
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

bool samePath(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
  constexpr auto sensitivity = Qt::CaseInsensitive;
#else
  constexpr auto sensitivity = Qt::CaseSensitive;
#endif
  return absolutePath(left).compare(absolutePath(right), sensitivity) == 0;
}

void assignError(QString *target, const QString &message) { if (target) *target = message; }

QString archiveRoot(const QString &projectRoot) {
  return QDir(projectRoot).filePath(QStringLiteral(".gsw/experiments"));
}

QString archiveFile(const QString &projectRoot, const QString &id) {
  return QDir(archiveRoot(projectRoot)).filePath(id + QStringLiteral(".json"));
}

QString portablePath(const QString &projectRoot, const QString &path) {
  if (path.isEmpty()) return {};
  const QString absolute = QDir::isAbsolutePath(path) ? absolutePath(path)
      : QDir::cleanPath(QDir(projectRoot).absoluteFilePath(path));
  const QString relative = QDir(projectRoot).relativeFilePath(absolute);
  return noTraversal(relative) && !QDir::isAbsolutePath(relative) ? relative : absolute;
}

QString resolvedPath(const QString &projectRoot, const QString &path) {
  if (path.isEmpty() || !noTraversal(path)) return {};
  return QDir::isAbsolutePath(path) ? absolutePath(path)
      : QDir::cleanPath(QDir(projectRoot).absoluteFilePath(path));
}

QStringList configurationPathKeys() {
  return {QStringLiteral("projectRoot"), QStringLiteral("datasetPath"),
      QStringLiteral("datasetRoot"), QStringLiteral("outputRoot"), QStringLiteral("jobStore"),
      QStringLiteral("modelDirectory")};
}

QJsonObject mapConfigurationPaths(QJsonObject parameters, const QString &projectRoot, bool storing) {
  for (const auto &key : configurationPathKeys()) {
    if (!parameters.value(key).isString()) continue;
    const auto path = parameters.value(key).toString();
    parameters.insert(key, storing ? portablePath(projectRoot, path) : resolvedPath(projectRoot, path));
  }
  return parameters;
}

QJsonObject readObject(const QString &path, qint64 limit = kMetadataLimit) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return {};
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
      ? document.object() : QJsonObject{};
}

QJsonObject serialized(const GenerationExperiment &value, const QString &root) {
  return {{QStringLiteral("version"), 1}, {QStringLiteral("id"), value.id},
      {QStringLiteral("pipeline"), value.pipeline}, {QStringLiteral("backend"), value.backend},
      {QStringLiteral("displayName"), value.displayName}, {QStringLiteral("status"), value.status},
      {QStringLiteral("datasetPath"), portablePath(root, value.datasetPath)},
      {QStringLiteral("configurationPath"), portablePath(root, value.configurationPath)},
      {QStringLiteral("outputRoot"), portablePath(root, value.outputRoot)},
      {QStringLiteral("resultPath"), portablePath(root, value.resultPath)},
      {QStringLiteral("resultSceneId"), value.resultSceneId},
      {QStringLiteral("createdAt"), value.createdAt.toUTC().toString(Qt::ISODateWithMs)},
      {QStringLiteral("updatedAt"), value.updatedAt.toUTC().toString(Qt::ISODateWithMs)},
      {QStringLiteral("parameters"), mapConfigurationPaths(value.parameters, root, true)}};
}
} // namespace

bool GenerationExperiment::isValid() const {
  static const QSet<QString> states{QStringLiteral("queued"), QStringLiteral("running"),
      QStringLiteral("paused"), QStringLiteral("completed"), QStringLiteral("cancelled"),
      QStringLiteral("failed"), QStringLiteral("interrupted")};
  return safeId(id) && !pipeline.isEmpty() && !displayName.trimmed().isEmpty() && states.contains(status);
}

GenerationHistoryStore::GenerationHistoryStore(const QString &projectRoot)
    : mProjectRoot(absolutePath(projectRoot)) {}

bool GenerationHistoryStore::upsert(const GenerationExperiment &experiment, QString *errorMessage) const {
  if (errorMessage) errorMessage->clear();
  if (!experiment.isValid() || !QFileInfo(mProjectRoot).isDir() || !noLinks(mProjectRoot)) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案数据不完整或包含不安全路径。"));
    return false;
  }
  for (const auto &path : {experiment.datasetPath, experiment.configurationPath,
                          experiment.outputRoot, experiment.resultPath}) {
    if (!noTraversal(path)) {
      assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案数据不完整或包含不安全路径。"));
      return false;
    }
  }
  const QString directory = archiveRoot(mProjectRoot);
  if (!noLinks(directory) || !QDir().mkpath(directory)) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案目录不安全。"));
    return false;
  }
  GenerationExperiment value = experiment;
  const QString path = archiveFile(mProjectRoot, value.id);
  if (QFileInfo::exists(path)) {
    QString previousError;
    const auto previous = record(value.id, &previousError);
    if (!previous.isValid()) { assignError(errorMessage, previousError); return false; }
    value.createdAt = previous.createdAt;
    if (value.parameters.isEmpty()) value.parameters = previous.parameters;
  }
  if (value.parameters.isEmpty() && !value.configurationPath.isEmpty())
    value.parameters = readObject(value.configurationPath);
  if (!value.createdAt.isValid()) value.createdAt = QDateTime::currentDateTimeUtc();
  value.updatedAt = QDateTime::currentDateTimeUtc();
  const QByteArray bytes = QJsonDocument(serialized(value, mProjectRoot)).toJson(QJsonDocument::Indented);
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (bytes.size() > kMetadataLimit || !noLinks(path) || !file.open(QIODevice::WriteOnly) ||
      file.write(bytes) != bytes.size() || !file.commit()) {
    file.cancelWriting();
    assignError(errorMessage, QCoreApplication::translate("Workbench", "无法保存实验档案：%1").arg(path));
    return false;
  }
  return true;
}

GenerationExperiment GenerationHistoryStore::record(const QString &id, QString *errorMessage) const {
  if (errorMessage) errorMessage->clear();
  if (!safeId(id)) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案数据不完整或包含不安全路径。"));
    return {};
  }
  const QString path = archiveFile(mProjectRoot, id);
  if (!QFileInfo::exists(path)) return {};
  if (!safeCatalogChild(mProjectRoot, path) || !noLinks(mProjectRoot)) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案目录不安全。"));
    return {};
  }
  const auto object = readObject(path);
  GenerationExperiment result;
  result.id = object.value(QStringLiteral("id")).toString();
  result.pipeline = object.value(QStringLiteral("pipeline")).toString();
  result.backend = object.value(QStringLiteral("backend")).toString();
  result.displayName = object.value(QStringLiteral("displayName")).toString();
  result.status = object.value(QStringLiteral("status")).toString();
  result.resultSceneId = object.value(QStringLiteral("resultSceneId")).toString();
  result.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
  result.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updatedAt")).toString(), Qt::ISODateWithMs);
  result.parameters = mapConfigurationPaths(object.value(QStringLiteral("parameters")).toObject(), mProjectRoot, false);
  result.datasetPath = resolvedPath(mProjectRoot, object.value(QStringLiteral("datasetPath")).toString());
  result.configurationPath = resolvedPath(mProjectRoot, object.value(QStringLiteral("configurationPath")).toString());
  result.outputRoot = resolvedPath(mProjectRoot, object.value(QStringLiteral("outputRoot")).toString());
  result.resultPath = resolvedPath(mProjectRoot, object.value(QStringLiteral("resultPath")).toString());
  bool unsafePath = false;
  for (const auto &key : {QStringLiteral("datasetPath"), QStringLiteral("configurationPath"),
                         QStringLiteral("outputRoot"), QStringLiteral("resultPath")})
    unsafePath = unsafePath || !noTraversal(object.value(key).toString());
  if (object.value(QStringLiteral("version")).toInt() != 1 || result.id != id || !result.isValid() ||
      !result.createdAt.isValid() || !result.updatedAt.isValid() || unsafePath) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案已损坏或版本不受支持：%1").arg(path));
    return {};
  }
  return result;
}

QList<GenerationExperiment> GenerationHistoryStore::records(QString *errorMessage) const {
  if (errorMessage) errorMessage->clear();
  QList<GenerationExperiment> result;
  const QString directory = archiveRoot(mProjectRoot);
  if (!QFileInfo::exists(directory)) return result;
  if (!safeCatalogChild(mProjectRoot, directory) || !noLinks(mProjectRoot)) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案目录不安全。"));
    return result;
  }
  const auto files = QDir(directory).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  for (const auto &file : files) {
    QString error;
    const auto value = record(file.completeBaseName(), &error);
    if (value.isValid()) result.append(value);
    else if (!error.isEmpty()) assignError(errorMessage, error);
  }
  std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
    return left.updatedAt == right.updatedAt ? left.id < right.id : left.updatedAt > right.updatedAt;
  });
  return result;
}
bool GenerationHistoryStore::migrateActiveTraining(QString *errorMessage) const {
  if (errorMessage) errorMessage->clear();
  QString error;
  const auto active = loadActiveTrainingJob(mProjectRoot, &error);
  if (!active.isValid()) { assignError(errorMessage, error); return error.isEmpty(); }
  // Experimental identity outlives a displayed result object's identity. A
  // user may replace that slot, so resume/navigation can safely create a new
  // result object without creating another experimental parameter archive.
  for (const auto &archived : records())
    if (archived.pipeline == QStringLiteral("training") && samePath(archived.outputRoot, active.outputSceneRoot))
      return true;
  GenerationExperiment value;
  value.id = active.resultSceneId;
  if (value.id.isEmpty()) {
    const QByteArray identity = active.configurationPath.toUtf8() + '\n' + active.outputSceneRoot.toUtf8();
    value.id = QStringLiteral("legacy-") + QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32));
  }
  // A saved archive remains browseable even if its former configuration has
  // since disappeared. Resume availability will still reject that missing file.
  const auto existing = record(value.id, &error);
  if (existing.isValid()) return true;
  if (!error.isEmpty()) { assignError(errorMessage, error); return false; }
  const QString jobsRoot = QDir(mProjectRoot).filePath(QStringLiteral(".gsw/jobs"));
  if (!safeCatalogChild(jobsRoot, active.configurationPath) ||
      !noLinks(active.configurationPath) || !noLinks(active.outputSceneRoot)) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "实验档案数据不完整或包含不安全路径。"));
    return false;
  }
  auto parameters = readObject(active.configurationPath);
  if (parameters.isEmpty()) {
    assignError(errorMessage, QCoreApplication::translate("Workbench", "配置或数据集已缺失、损坏或路径不安全。"));
    return false;
  }
  const QString oldRoot = parameters.value(QStringLiteral("projectRoot")).toString();
  if (!oldRoot.isEmpty()) {
    for (const auto &key : configurationPathKeys()) {
      const auto parameterValue = parameters.value(key);
      if (!parameterValue.isString() || parameterValue.toString().isEmpty()) continue;
      const QString relative = QDir(oldRoot).relativeFilePath(parameterValue.toString());
      if (noTraversal(relative) && !QDir::isAbsolutePath(relative))
        parameters.insert(key, QDir(mProjectRoot).absoluteFilePath(relative));
    }
  }
  value.pipeline = QStringLiteral("training");
  value.backend = parameters.value(QStringLiteral("backend")).toString();
  value.displayName = parameters.value(QStringLiteral("outputDisplayName")).toString();
  if (value.displayName.trimmed().isEmpty()) value.displayName = parameters.value(QStringLiteral("outputScene")).toString();
  if (value.displayName.trimmed().isEmpty()) value.displayName = QFileInfo(active.outputSceneRoot).fileName();
  value.datasetPath = parameters.value(QStringLiteral("datasetPath")).toString();
  value.configurationPath = active.configurationPath;
  value.outputRoot = active.outputSceneRoot;
  value.resultSceneId = active.resultSceneId;
  value.parameters = parameters;
  value.resultPath = findLatestTrainingOutputScene(active.outputSceneRoot).path;
  const int pausedIteration = nativeResumeIteration(active.outputSceneRoot);
  if (pausedIteration > 0) {
    QString group = QFileInfo(active.outputSceneRoot).fileName();
    // Match the existing durable publisher's filename mapping, including
    // legacy names that retain spaces or Unicode.
    group.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*]+)")), QStringLiteral("_"));
    group = group.trimmed();
    if (group.isEmpty()) group = QStringLiteral("gaussian-scene");
    QStringList groups;
    if (!active.resultSceneId.isEmpty()) {
      const QString identity = QString::fromLatin1(QCryptographicHash::hash(
          active.resultSceneId.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
      groups.append(group + QLatin1Char('-') + identity);
    }
    // New copies are isolated by result identity even when different output
    // directories have the same basename. Keep legacy copies as read-only
    // fallback; neither migration nor browsing rewrites either artifact.
    groups.append(group);
    for (const auto &candidate : groups) {
      const QString protectedPath = QDir(mProjectRoot).filePath(
          QStringLiteral(".gsw/checkpoints/training/%1/iteration_%2/point_cloud.ply")
              .arg(candidate).arg(pausedIteration));
      if (safeCatalogChild(mProjectRoot, protectedPath) && noLinks(protectedPath)) {
        const auto metadata = WorkspaceDocument::inspectPly(protectedPath);
        if (metadata.valid && metadata.vertexCount > 0 && metadata.looksLikeGaussianSplat()) {
          value.resultPath = protectedPath;
          break;
        }
      }
    }
  }
  value.status = pausedIteration > 0
      ? QStringLiteral("paused") : QStringLiteral("interrupted");
  return upsert(value, errorMessage);
}

GenerationResumeAvailability GenerationHistoryStore::resumeAvailability(const QString &id) const {
  const auto value = record(id);
  GenerationResumeAvailability result;
  if (!value.isValid()) {
    result.reason = QCoreApplication::translate("Workbench", "实验档案数据不完整或包含不安全路径。");
    return result;
  }
  if (value.pipeline != QStringLiteral("training") ||
      (value.backend != QStringLiteral("3dgs") && value.backend != QStringLiteral("2dgs"))) {
    result.reason = QCoreApplication::translate("Workbench", "此生成阶段不支持完整优化器状态续训。");
    return result;
  }
  if (value.status == QStringLiteral("completed")) {
    result.reason = QCoreApplication::translate("Workbench", "该实验已完成；未提供自动重新训练。");
    return result;
  }
  const QString jobsRoot = QDir(mProjectRoot).filePath(QStringLiteral(".gsw/jobs"));
  if (value.configurationPath.isEmpty() || !QFileInfo(value.configurationPath).isFile() ||
      !safeCatalogChild(jobsRoot, value.configurationPath) ||
      !noLinks(value.configurationPath) || readObject(value.configurationPath).isEmpty() ||
      value.datasetPath.isEmpty() || !QFileInfo(value.datasetPath).isDir() || !noLinks(value.datasetPath) ||
      value.outputRoot.isEmpty() || !QFileInfo(value.outputRoot).isDir() || !noLinks(value.outputRoot)) {
    result.reason = QCoreApplication::translate("Workbench", "配置或数据集已缺失、损坏或路径不安全。");
    return result;
  }
  const auto &parameters = value.parameters;
  const QString configuredDataset = parameters.value(QStringLiteral("datasetPath")).toString();
  const QString configuredOutputParent = parameters.value(QStringLiteral("outputRoot")).toString();
  const QString configuredOutputName = parameters.value(QStringLiteral("outputScene")).toString();
  const QString configuredControlStore = parameters.value(QStringLiteral("jobStore")).toString();
  if (parameters.value(QStringLiteral("backend")).toString() != value.backend ||
      !parameters.value(QStringLiteral("nativeCheckpoint")).toBool() ||
      !samePath(configuredDataset, value.datasetPath) || configuredOutputParent.isEmpty() ||
      (!configuredControlStore.isEmpty() &&
       (!samePath(configuredControlStore, QDir(jobsRoot).filePath(QStringLiteral("training"))) ||
        !noLinks(configuredControlStore))) ||
      configuredOutputName.isEmpty() || configuredOutputName == QStringLiteral(".") ||
      !noTraversal(configuredOutputName) ||
      configuredOutputName.contains(QLatin1Char('/')) || configuredOutputName.contains(QLatin1Char('\\')) ||
      !samePath(QDir(configuredOutputParent).filePath(configuredOutputName), value.outputRoot)) {
    result.reason = QCoreApplication::translate("Workbench", "完整状态元数据与实验参数不一致。");
    return result;
  }
  const QString checkpointRoot = QDir(value.outputRoot).filePath(QStringLiteral(".gsw-resume"));
  const QString manifestPath = QDir(checkpointRoot).filePath(QStringLiteral("ready.json"));
  const auto metadata = readObject(manifestPath, 16 * 1024);
  static const QRegularExpression filename(QStringLiteral("^state-[0-9a-f]{32}\\.pth$"));
  static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
  const QString name = metadata.value(QStringLiteral("file")).toString();
  const QFileInfo state(QDir(checkpointRoot).filePath(name));
  const int iteration = metadata.value(QStringLiteral("iteration")).toInt(-1);
  const int total = metadata.value(QStringLiteral("total")).toInt(-1);
  if (!QFileInfo(checkpointRoot).isDir() || !noLinks(manifestPath) || !noLinks(checkpointRoot) ||
      metadata.value(QStringLiteral("version")).toInt() != 1 || !filename.match(name).hasMatch() ||
      !digest.match(metadata.value(QStringLiteral("sha256")).toString()).hasMatch() ||
      !digest.match(metadata.value(QStringLiteral("identity")).toString()).hasMatch() ||
      iteration <= 0 || iteration >= total || !state.isFile() || state.size() == 0 ||
      !noLinks(state.absoluteFilePath()) || QDir(state.canonicalPath()) != QDir(QFileInfo(checkpointRoot).canonicalFilePath())) {
    result.reason = QCoreApplication::translate("Workbench", "尚无可用的完整优化器状态。预览 PLY 不能用于完整续训。");
    return result;
  }
  if (metadata.value(QStringLiteral("backend")).toString(QStringLiteral("3dgs")) != value.backend ||
      total != parameters.value(QStringLiteral("trainOptions")).toObject().value(QStringLiteral("iterations")).toInt(-1)) {
    result.reason = QCoreApplication::translate("Workbench", "完整状态元数据与实验参数不一致。");
    return result;
  }
  result.available = true;
  result.iteration = iteration;
  result.reason = QCoreApplication::translate("Workbench", "完整状态元数据就绪：迭代 %1 / %2；续训前仍需校验内容和参数。")
      .arg(iteration).arg(total);
  return result;
}

ActiveTrainingJob GenerationHistoryStore::resumeJob(const QString &id, QString *errorMessage) const {
  if (errorMessage) errorMessage->clear();
  const auto availability = resumeAvailability(id);
  if (!availability.available) { assignError(errorMessage, availability.reason); return {}; }
  const auto value = record(id, errorMessage);
  return {value.configurationPath, value.outputRoot, true, value.resultSceneId};
}
} // namespace gsw
