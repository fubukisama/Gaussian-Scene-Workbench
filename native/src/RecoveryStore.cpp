#include "RecoveryStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace gsw {
namespace {

constexpr auto kRecoveryStatePath = ".gsw/recovery-state.json";
constexpr auto kProjectHistoryPath = ".gsw/history";

QString normalizedAbsolutePath(const QString &path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void assignError(QString *target, const QString &message) {
  if (target != nullptr) {
    *target = message;
  }
}

bool pathInside(const QString &root, const QString &candidate) {
  const QString relative =
      QDir(normalizedAbsolutePath(root))
          .relativeFilePath(normalizedAbsolutePath(candidate));
  return !QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
         !relative.startsWith(QStringLiteral("../")) &&
         !relative.startsWith(QStringLiteral("..\\"));
}

QString optionalAbsolutePath(const QString &path) {
  return path.isEmpty() ? QString() : normalizedAbsolutePath(path);
}

std::optional<RecoveryWorkspace>
readRecoveryWorkspace(const QString &workspaceRoot, QString *errorMessage) {
  QFile stateFile(
      QDir(workspaceRoot).filePath(QString::fromLatin1(kRecoveryStatePath)));
  if (!stateFile.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open recovery state: %1")
                    .arg(stateFile.errorString()));
    return std::nullopt;
  }

  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(stateFile.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                QStringLiteral("Invalid recovery state: %1")
                    .arg(parseError.errorString()));
    return std::nullopt;
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
    assignError(errorMessage,
                QStringLiteral("Unsupported recovery state version."));
    return std::nullopt;
  }

  RecoveryWorkspace workspace;
  workspace.sessionId = root.value(QStringLiteral("sessionId")).toString();
  workspace.displayName = root.value(QStringLiteral("displayName")).toString();
  workspace.rootPath = normalizedAbsolutePath(workspaceRoot);
  workspace.projectFilePath = optionalAbsolutePath(
      root.value(QStringLiteral("projectFilePath")).toString());
  workspace.datasetPath =
      optionalAbsolutePath(root.value(QStringLiteral("datasetPath")).toString());
  workspace.scenePath =
      optionalAbsolutePath(root.value(QStringLiteral("scenePath")).toString());
  workspace.updatedUtc = QDateTime::fromString(
      root.value(QStringLiteral("updatedUtc")).toString(), Qt::ISODate);
  if (!workspace.isValid() || !workspace.updatedUtc.isValid()) {
    assignError(errorMessage,
                QStringLiteral("Recovery state is incomplete."));
    return std::nullopt;
  }
  return workspace;
}

std::optional<ProjectSnapshot>
readProjectSnapshot(const QString &snapshotPath, QJsonObject *project,
                    QString *errorMessage) {
  QFile snapshotFile(snapshotPath);
  if (!snapshotFile.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open project snapshot: %1")
                    .arg(snapshotFile.errorString()));
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(snapshotFile.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                QStringLiteral("Invalid project snapshot: %1")
                    .arg(parseError.errorString()));
    return std::nullopt;
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
      !root.value(QStringLiteral("project")).isObject()) {
    assignError(errorMessage,
                QStringLiteral("Unsupported project snapshot."));
    return std::nullopt;
  }
  ProjectSnapshot snapshot;
  snapshot.snapshotId =
      root.value(QStringLiteral("snapshotId")).toString();
  snapshot.snapshotPath = normalizedAbsolutePath(snapshotPath);
  snapshot.sourceProjectFilePath = optionalAbsolutePath(
      root.value(QStringLiteral("sourceProjectFilePath")).toString());
  snapshot.createdUtc = QDateTime::fromString(
      root.value(QStringLiteral("createdUtc")).toString(), Qt::ISODate);
  if (!snapshot.isValid()) {
    assignError(errorMessage,
                QStringLiteral("Project snapshot is incomplete."));
    return std::nullopt;
  }
  if (project != nullptr) {
    *project = root.value(QStringLiteral("project")).toObject();
  }
  return snapshot;
}

} // namespace

RecoveryStore::RecoveryStore(QString workspaceBase)
    : mWorkspaceBase(normalizedAbsolutePath(workspaceBase)) {}

std::optional<RecoveryWorkspace>
RecoveryStore::beginWorkspace(const QString &displayName,
                              QString *errorMessage) const {
  if (!QDir().mkpath(mWorkspaceBase)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create recovery workspace base: %1")
                    .arg(mWorkspaceBase));
    return std::nullopt;
  }

  RecoveryWorkspace workspace;
  workspace.sessionId =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  workspace.displayName =
      displayName.trimmed().isEmpty() ? QStringLiteral("Untitled Project")
                                      : displayName.trimmed();
  workspace.rootPath =
      QDir(mWorkspaceBase)
          .filePath(QStringLiteral("Untitled-%1").arg(workspace.sessionId));
  workspace.updatedUtc = QDateTime::currentDateTimeUtc();
  if (!QDir().mkpath(workspace.rootPath) ||
      !checkpoint(workspace, errorMessage)) {
    QDir(workspace.rootPath).removeRecursively();
    return std::nullopt;
  }
  return workspace;
}

bool RecoveryStore::checkpoint(const RecoveryWorkspace &workspace,
                               QString *errorMessage) const {
  if (!workspace.isValid() ||
      !pathInside(mWorkspaceBase, workspace.rootPath) ||
      !QFileInfo(workspace.rootPath).isDir()) {
    assignError(errorMessage,
                QStringLiteral("Recovery workspace is outside the catalog."));
    return false;
  }

  const QString statePath =
      QDir(workspace.rootPath)
          .filePath(QString::fromLatin1(kRecoveryStatePath));
  if (!QDir().mkpath(QFileInfo(statePath).absolutePath())) {
    assignError(errorMessage,
                QStringLiteral("Unable to create recovery state directory."));
    return false;
  }

  QJsonObject root;
  root.insert(QStringLiteral("schemaVersion"), 1);
  root.insert(QStringLiteral("sessionId"), workspace.sessionId);
  root.insert(QStringLiteral("displayName"), workspace.displayName);
  root.insert(QStringLiteral("projectFilePath"),
              optionalAbsolutePath(workspace.projectFilePath));
  root.insert(QStringLiteral("datasetPath"),
              optionalAbsolutePath(workspace.datasetPath));
  root.insert(QStringLiteral("scenePath"),
              optionalAbsolutePath(workspace.scenePath));
  root.insert(QStringLiteral("updatedUtc"),
              QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

  QSaveFile stateFile(statePath);
  if (!stateFile.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create recovery state: %1")
                    .arg(stateFile.errorString()));
    return false;
  }
  const QByteArray serialized =
      QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (stateFile.write(serialized) != serialized.size()) {
    stateFile.cancelWriting();
    assignError(errorMessage,
                QStringLiteral("Unable to write recovery state: %1")
                    .arg(stateFile.errorString()));
    return false;
  }
  if (!stateFile.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to commit recovery state: %1")
                    .arg(stateFile.errorString()));
    return false;
  }
  return true;
}

QList<RecoveryWorkspace>
RecoveryStore::recoverableWorkspaces(QString *errorMessage) const {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QList<RecoveryWorkspace> result;
  const QDir base(mWorkspaceBase);
  if (!base.exists()) {
    return result;
  }

  const QFileInfoList workspaces =
      base.entryInfoList({QStringLiteral("Untitled-*")},
                         QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDir::NoSort);
  QStringList invalidStates;
  for (const QFileInfo &workspaceInfo : workspaces) {
    QString readError;
    const std::optional<RecoveryWorkspace> workspace =
        readRecoveryWorkspace(workspaceInfo.absoluteFilePath(), &readError);
    if (workspace.has_value()) {
      result.append(*workspace);
    } else if (QFileInfo::exists(
                   QDir(workspaceInfo.absoluteFilePath())
                       .filePath(QString::fromLatin1(kRecoveryStatePath)))) {
      invalidStates.append(
          QStringLiteral("%1: %2").arg(workspaceInfo.fileName(), readError));
    }
  }
  std::sort(result.begin(), result.end(),
            [](const RecoveryWorkspace &left,
               const RecoveryWorkspace &right) {
              return left.updatedUtc > right.updatedUtc;
            });
  if (!invalidStates.isEmpty()) {
    assignError(errorMessage,
                QStringLiteral("Some recovery states are invalid:\n%1")
                    .arg(invalidStates.join(QLatin1Char('\n'))));
  }
  return result;
}

bool RecoveryStore::discardWorkspace(const RecoveryWorkspace &workspace,
                                     QString *errorMessage) const {
  if (!workspace.isValid() ||
      !pathInside(mWorkspaceBase, workspace.rootPath)) {
    assignError(errorMessage,
                QStringLiteral("Refusing to discard a workspace outside the "
                               "recovery catalog."));
    return false;
  }
  QString readError;
  const std::optional<RecoveryWorkspace> stored =
      readRecoveryWorkspace(workspace.rootPath, &readError);
  if (!stored.has_value() || stored->sessionId != workspace.sessionId) {
    assignError(errorMessage,
                readError.isEmpty()
                    ? QStringLiteral("Recovery workspace identity mismatch.")
                    : readError);
    return false;
  }
  if (!QDir(workspace.rootPath).removeRecursively()) {
    assignError(errorMessage,
                QStringLiteral("Unable to discard recovery workspace: %1")
                    .arg(workspace.rootPath));
    return false;
  }
  return true;
}

bool RecoveryStore::completeWorkspace(
    const RecoveryWorkspace &workspace, const QString &managedProjectRoot,
    QString *errorMessage) const {
  const QString managedRoot = normalizedAbsolutePath(managedProjectRoot);
  if (!QFileInfo(managedRoot).isDir() ||
      pathInside(workspace.rootPath, managedRoot)) {
    assignError(errorMessage,
                QStringLiteral("Managed project root is not independent from "
                               "the recovery workspace."));
    return false;
  }
  const QString copiedState =
      QDir(managedRoot).filePath(QString::fromLatin1(kRecoveryStatePath));
  if (QFileInfo::exists(copiedState) && !QFile::remove(copiedState)) {
    assignError(errorMessage,
                QStringLiteral("Unable to remove copied recovery metadata: %1")
                    .arg(copiedState));
    return false;
  }
  return discardWorkspace(workspace, errorMessage);
}

std::optional<ProjectSnapshot> RecoveryStore::createProjectSnapshot(
    const QString &projectFilePath, const QString &projectDataRoot,
    const int maximumSnapshots, QString *errorMessage) const {
  QFile projectFile(projectFilePath);
  if (!projectFile.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open project for snapshot: %1")
                    .arg(projectFile.errorString()));
    return std::nullopt;
  }
  return createProjectSnapshot(projectFile.readAll(), projectFilePath,
                               projectDataRoot, maximumSnapshots,
                               errorMessage);
}

std::optional<ProjectSnapshot> RecoveryStore::createProjectSnapshot(
    const QByteArray &projectStateJson,
    const QString &sourceProjectFilePath, const QString &projectDataRoot,
    const int maximumSnapshots, QString *errorMessage) const {
  if (maximumSnapshots < 1) {
    assignError(errorMessage,
                QStringLiteral("Snapshot retention must be at least one."));
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument projectDocument =
      QJsonDocument::fromJson(projectStateJson, &parseError);
  if (parseError.error != QJsonParseError::NoError ||
      !projectDocument.isObject()) {
    assignError(errorMessage,
                QStringLiteral("Unable to snapshot invalid project JSON: %1")
                    .arg(parseError.errorString()));
    return std::nullopt;
  }

  QString listError;
  QList<ProjectSnapshot> existing =
      projectSnapshots(projectDataRoot, &listError);
  if (!listError.isEmpty()) {
    assignError(errorMessage, listError);
    return std::nullopt;
  }
  if (!existing.isEmpty()) {
    QJsonObject latestProject;
    QString latestError;
    const std::optional<ProjectSnapshot> latest = readProjectSnapshot(
        existing.constFirst().snapshotPath, &latestProject, &latestError);
    QJsonObject currentProject = projectDocument.object();
    latestProject.remove(QStringLiteral("updatedUtc"));
    currentProject.remove(QStringLiteral("updatedUtc"));
    if (latest.has_value() && latestProject == currentProject) {
      return latest;
    }
  }
  QDateTime createdUtc = QDateTime::currentDateTimeUtc();
  if (!existing.isEmpty() &&
      createdUtc <= existing.constFirst().createdUtc) {
    createdUtc = existing.constFirst().createdUtc.addMSecs(1);
  }

  const QString snapshotId =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString historyRoot =
      QDir(projectDataRoot)
          .filePath(QString::fromLatin1(kProjectHistoryPath));
  if (!QDir().mkpath(historyRoot)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create project history directory: "
                               "%1")
                    .arg(historyRoot));
    return std::nullopt;
  }
  const QString snapshotPath =
      QDir(historyRoot)
          .filePath(QStringLiteral("%1-%2.snapshot.json")
                        .arg(createdUtc.toString(
                                 QStringLiteral("yyyyMMdd-HHmmsszzz")),
                             snapshotId));
  const QJsonObject envelope{
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("snapshotId"), snapshotId},
      {QStringLiteral("createdUtc"),
       createdUtc.toString(Qt::ISODateWithMs)},
      {QStringLiteral("sourceProjectFilePath"),
       normalizedAbsolutePath(sourceProjectFilePath)},
      {QStringLiteral("project"), projectDocument.object()}};
  QSaveFile snapshotFile(snapshotPath);
  snapshotFile.setDirectWriteFallback(false);
  if (!snapshotFile.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create project snapshot: %1")
                    .arg(snapshotFile.errorString()));
    return std::nullopt;
  }
  const QByteArray serialized =
      QJsonDocument(envelope).toJson(QJsonDocument::Indented);
  if (snapshotFile.write(serialized) != serialized.size()) {
    snapshotFile.cancelWriting();
    assignError(errorMessage,
                QStringLiteral("Unable to write project snapshot: %1")
                    .arg(snapshotFile.errorString()));
    return std::nullopt;
  }
  if (!snapshotFile.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to commit project snapshot: %1")
                    .arg(snapshotFile.errorString()));
    return std::nullopt;
  }

  const ProjectSnapshot created{
      snapshotId, normalizedAbsolutePath(snapshotPath),
      normalizedAbsolutePath(sourceProjectFilePath), createdUtc};
  existing.prepend(created);
  std::sort(existing.begin(), existing.end(),
            [](const ProjectSnapshot &left, const ProjectSnapshot &right) {
              return left.createdUtc > right.createdUtc;
            });
  for (qsizetype index = maximumSnapshots; index < existing.size();
       ++index) {
    QFile::remove(existing.at(index).snapshotPath);
  }
  return created;
}

QList<ProjectSnapshot>
RecoveryStore::projectSnapshots(const QString &projectDataRoot,
                                QString *errorMessage) const {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QList<ProjectSnapshot> snapshots;
  const QDir history(
      QDir(projectDataRoot)
          .filePath(QString::fromLatin1(kProjectHistoryPath)));
  if (!history.exists()) {
    return snapshots;
  }
  QStringList invalidSnapshots;
  const QFileInfoList files = history.entryInfoList(
      {QStringLiteral("*.snapshot.json")}, QDir::Files | QDir::NoSymLinks,
      QDir::NoSort);
  for (const QFileInfo &file : files) {
    QString readError;
    const std::optional<ProjectSnapshot> snapshot =
        readProjectSnapshot(file.absoluteFilePath(), nullptr, &readError);
    if (snapshot.has_value()) {
      snapshots.append(*snapshot);
    } else {
      invalidSnapshots.append(
          QStringLiteral("%1: %2").arg(file.fileName(), readError));
    }
  }
  std::sort(snapshots.begin(), snapshots.end(),
            [](const ProjectSnapshot &left, const ProjectSnapshot &right) {
              return left.createdUtc > right.createdUtc;
            });
  if (!invalidSnapshots.isEmpty()) {
    assignError(errorMessage,
                QStringLiteral("Some project snapshots are invalid:\n%1")
                    .arg(invalidSnapshots.join(QLatin1Char('\n'))));
  }
  return snapshots;
}

bool RecoveryStore::restoreProjectSnapshot(
    const ProjectSnapshot &snapshot, const QString &targetProjectFilePath,
    QString *errorMessage) const {
  QJsonObject project;
  const std::optional<ProjectSnapshot> stored =
      readProjectSnapshot(snapshot.snapshotPath, &project, errorMessage);
  if (!stored.has_value() ||
      stored->snapshotId != snapshot.snapshotId) {
    if (stored.has_value()) {
      assignError(errorMessage,
                  QStringLiteral("Project snapshot identity mismatch."));
    }
    return false;
  }
  const QString target = normalizedAbsolutePath(targetProjectFilePath);
  if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
    assignError(errorMessage,
                QStringLiteral("Unable to create snapshot restore directory."));
    return false;
  }
  QSaveFile output(target);
  output.setDirectWriteFallback(false);
  if (!output.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create restored project: %1")
                    .arg(output.errorString()));
    return false;
  }
  const QByteArray serialized =
      QJsonDocument(project).toJson(QJsonDocument::Compact);
  if (output.write(serialized) != serialized.size()) {
    output.cancelWriting();
    assignError(errorMessage,
                QStringLiteral("Unable to write restored project: %1")
                    .arg(output.errorString()));
    return false;
  }
  if (!output.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to commit restored project: %1")
                    .arg(output.errorString()));
    return false;
  }
  return true;
}

QByteArray RecoveryStore::projectSnapshotJson(
    const ProjectSnapshot &snapshot, QString *errorMessage) const {
  QJsonObject project;
  const std::optional<ProjectSnapshot> stored =
      readProjectSnapshot(snapshot.snapshotPath, &project, errorMessage);
  if (!stored.has_value() ||
      stored->snapshotId != snapshot.snapshotId) {
    if (stored.has_value()) {
      assignError(errorMessage,
                  QStringLiteral("Project snapshot identity mismatch."));
    }
    return {};
  }
  return QJsonDocument(project).toJson(QJsonDocument::Indented);
}

} // namespace gsw
