#include "ExternalBackupStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUuid>

#include <algorithm>

namespace gsw {
namespace {

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

QString safeName(QString value) {
  value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*]+)")),
                QStringLiteral("_"));
  value = value.trimmed();
  return value.isEmpty() ? QStringLiteral("project") : value;
}

QString projectName(const QString &projectFilePath) {
  QString name = QFileInfo(projectFilePath).fileName();
  const QString suffix = QStringLiteral(".gsw.json");
  if (name.endsWith(suffix, Qt::CaseInsensitive)) {
    name.chop(suffix.size());
  } else {
    name = QFileInfo(projectFilePath).completeBaseName();
  }
  return safeName(name);
}

QString projectKey(const QString &projectFilePath) {
  const QByteArray digest =
      QCryptographicHash::hash(
          normalizedAbsolutePath(projectFilePath).toUtf8(),
          QCryptographicHash::Sha256)
          .toHex()
          .left(12);
  return QStringLiteral("%1-%2")
      .arg(projectName(projectFilePath), QString::fromLatin1(digest));
}

QString hashFile(const QString &path, QString *errorMessage) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to read backup source: %1")
                    .arg(file.errorString()));
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  constexpr qint64 chunkSize = 4LL * 1024LL * 1024LL;
  while (!file.atEnd()) {
    const QByteArray chunk = file.read(chunkSize);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
      assignError(errorMessage,
                  QStringLiteral("Unable to hash backup source: %1")
                      .arg(file.errorString()));
      return {};
    }
    hash.addData(chunk);
  }
  return QString::fromLatin1(hash.result().toHex());
}

QString storeObject(const QString &sourcePath, const QString &objectsRoot,
                    QString *errorMessage) {
  QFile input(sourcePath);
  if (!input.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open backup source: %1")
                    .arg(input.errorString()));
    return {};
  }
  QTemporaryFile temporary(
      QDir(objectsRoot).filePath(QStringLiteral(".partial-XXXXXX")));
  temporary.setAutoRemove(false);
  if (!temporary.open()) {
    assignError(errorMessage,
                QStringLiteral("Unable to create backup object: %1")
                    .arg(temporary.errorString()));
    return {};
  }
  const QString temporaryPath = temporary.fileName();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  constexpr qint64 chunkSize = 4LL * 1024LL * 1024LL;
  while (!input.atEnd()) {
    const QByteArray chunk = input.read(chunkSize);
    if ((chunk.isEmpty() && input.error() != QFileDevice::NoError) ||
        temporary.write(chunk) != chunk.size()) {
      const QString detail =
          input.error() != QFileDevice::NoError ? input.errorString()
                                                : temporary.errorString();
      temporary.close();
      QFile::remove(temporaryPath);
      assignError(errorMessage,
                  QStringLiteral("Unable to write backup object: %1")
                      .arg(detail));
      return {};
    }
    hash.addData(chunk);
  }
  input.close();
  if (!temporary.flush()) {
    temporary.close();
    QFile::remove(temporaryPath);
    assignError(errorMessage,
                QStringLiteral("Unable to flush backup object: %1")
                    .arg(temporary.errorString()));
    return {};
  }
  temporary.close();

  const QString sha256 = QString::fromLatin1(hash.result().toHex());
  QString verificationError;
  if (hashFile(sourcePath, &verificationError) != sha256) {
    QFile::remove(temporaryPath);
    assignError(errorMessage,
                verificationError.isEmpty()
                    ? QStringLiteral(
                          "Backup source changed while it was being copied.")
                    : verificationError);
    return {};
  }
  const QString objectPath =
      QDir(objectsRoot).filePath(sha256 + QStringLiteral(".object"));
  if (QFileInfo::exists(objectPath)) {
    if (hashFile(objectPath, &verificationError) == sha256) {
      QFile::remove(temporaryPath);
      return sha256;
    }
    QFile::remove(objectPath);
  }
  if (!temporary.rename(objectPath)) {
    QFile::remove(temporaryPath);
    assignError(errorMessage,
                QStringLiteral("Unable to publish backup object: %1")
                    .arg(objectPath));
    return {};
  }
  return sha256;
}

std::optional<ExternalBackupSnapshot>
readSnapshot(const QString &manifestPath, QJsonObject *manifestRoot,
             QString *errorMessage) {
  QFile manifest(manifestPath);
  if (!manifest.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open backup manifest: %1")
                    .arg(manifest.errorString()));
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(manifest.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                QStringLiteral("Invalid backup manifest: %1")
                    .arg(parseError.errorString()));
    return std::nullopt;
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
      !root.value(QStringLiteral("files")).isArray()) {
    assignError(errorMessage,
                QStringLiteral("Unsupported backup manifest."));
    return std::nullopt;
  }
  ExternalBackupSnapshot snapshot;
  snapshot.snapshotId =
      root.value(QStringLiteral("snapshotId")).toString();
  snapshot.manifestPath = normalizedAbsolutePath(manifestPath);
  snapshot.projectKey =
      root.value(QStringLiteral("projectKey")).toString();
  snapshot.projectName =
      root.value(QStringLiteral("projectName")).toString();
  snapshot.createdUtc = QDateTime::fromString(
      root.value(QStringLiteral("createdUtc")).toString(), Qt::ISODate);
  snapshot.fileCount =
      root.value(QStringLiteral("files")).toArray().size();
  snapshot.totalBytes =
      root.value(QStringLiteral("totalBytes")).toInteger();
  if (!snapshot.isValid()) {
    assignError(errorMessage,
                QStringLiteral("Backup manifest is incomplete."));
    return std::nullopt;
  }
  if (manifestRoot != nullptr) {
    *manifestRoot = root;
  }
  return snapshot;
}

} // namespace

ExternalBackupStore::ExternalBackupStore(QString backupRoot)
    : mBackupRoot(
          QDir(normalizedAbsolutePath(backupRoot))
              .filePath(QStringLiteral("Gaussian Scene Workbench Backups"))) {}

std::optional<ExternalBackupSnapshot>
ExternalBackupStore::backupProject(const QString &projectFilePath,
                                   const QString &projectDataRoot,
                                   QString *errorMessage,
                                   const QString &linkedDatasetPath,
                                   const QString &linkedScenePath) const {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  const QString projectFile = normalizedAbsolutePath(projectFilePath);
  const QString dataRoot = normalizedAbsolutePath(projectDataRoot);
  if (!QFileInfo(projectFile).isFile() || !QFileInfo(dataRoot).isDir()) {
    assignError(errorMessage,
                QStringLiteral("Project file or data directory is missing."));
    return std::nullopt;
  }
  if (pathInside(dataRoot, mBackupRoot)) {
    assignError(errorMessage,
                QStringLiteral("Choose a backup location outside the project "
                               "data directory."));
    return std::nullopt;
  }

  const QString key = projectKey(projectFile);
  const QString projectBackupRoot = QDir(mBackupRoot).filePath(key);
  const QString objectsRoot =
      QDir(projectBackupRoot).filePath(QStringLiteral("objects"));
  const QString snapshotsRoot =
      QDir(projectBackupRoot).filePath(QStringLiteral("snapshots"));
  if (!QDir().mkpath(objectsRoot) || !QDir().mkpath(snapshotsRoot)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create external backup store: %1")
                    .arg(projectBackupRoot));
    return std::nullopt;
  }

  struct SourceFile {
    QString path;
    QString restorePath;
  };
  QList<SourceFile> sourceFiles = {
      {projectFile, QFileInfo(projectFile).fileName()}};
  const QString dataDirectoryName = QFileInfo(dataRoot).fileName();
  QDirIterator iterator(dataRoot,
                        QDir::Files | QDir::Hidden | QDir::System |
                            QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString source = iterator.next();
    const QString relative = QDir(dataRoot).relativeFilePath(source);
    sourceFiles.append(
        {source, QDir(dataDirectoryName).filePath(relative)});
  }
  QString datasetPathOverride;
  QString scenePathOverride;
  const QString linkedDataset =
      linkedDatasetPath.isEmpty()
          ? QString()
          : normalizedAbsolutePath(linkedDatasetPath);
  const QString linkedScene =
      linkedScenePath.isEmpty() ? QString()
                                : normalizedAbsolutePath(linkedScenePath);
  if (!linkedDataset.isEmpty() && QFileInfo(linkedDataset).isDir() &&
      !pathInside(dataRoot, linkedDataset)) {
    datasetPathOverride = QStringLiteral("external/dataset");
    QDirIterator datasetIterator(
        linkedDataset,
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    while (datasetIterator.hasNext()) {
      const QString source = datasetIterator.next();
      const QString relative =
          QDir(linkedDataset).relativeFilePath(source);
      sourceFiles.append(
          {source,
           QDir(dataDirectoryName)
               .filePath(QDir(datasetPathOverride).filePath(relative))});
    }
  }
  if (!linkedScene.isEmpty() && QFileInfo(linkedScene).isFile() &&
      !pathInside(dataRoot, linkedScene)) {
    if (!linkedDataset.isEmpty() && !datasetPathOverride.isEmpty() &&
        pathInside(linkedDataset, linkedScene)) {
      scenePathOverride =
          QDir(datasetPathOverride)
              .filePath(QDir(linkedDataset).relativeFilePath(linkedScene));
    } else {
      scenePathOverride =
          QDir(QStringLiteral("external/scene"))
              .filePath(QFileInfo(linkedScene).fileName());
      sourceFiles.append(
          {linkedScene,
           QDir(dataDirectoryName).filePath(scenePathOverride)});
    }
  }
  std::sort(sourceFiles.begin(), sourceFiles.end(),
            [](const SourceFile &left, const SourceFile &right) {
              return left.restorePath < right.restorePath;
            });

  QJsonArray files;
  qint64 totalBytes = 0;
  for (const SourceFile &source : sourceFiles) {
    const QString sha256 =
        storeObject(source.path, objectsRoot, errorMessage);
    if (sha256.isEmpty()) {
      return std::nullopt;
    }
    const qint64 size = QFileInfo(source.path).size();
    files.append(QJsonObject{
        {QStringLiteral("restorePath"),
         QDir::fromNativeSeparators(source.restorePath)},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("size"), size}});
    totalBytes += size;
  }

  const QString snapshotId =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QDateTime createdUtc = QDateTime::currentDateTimeUtc();
  const QString manifestPath =
      QDir(snapshotsRoot)
          .filePath(QStringLiteral("%1-%2.backup.json")
                        .arg(createdUtc.toString(
                                 QStringLiteral("yyyyMMdd-HHmmsszzz")),
                             snapshotId));
  const QJsonObject manifest{
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("snapshotId"), snapshotId},
      {QStringLiteral("projectKey"), key},
      {QStringLiteral("projectName"), projectName(projectFile)},
      {QStringLiteral("createdUtc"),
       createdUtc.toString(Qt::ISODateWithMs)},
      {QStringLiteral("projectRestorePath"),
       QFileInfo(projectFile).fileName()},
      {QStringLiteral("dataDirectoryName"), dataDirectoryName},
      {QStringLiteral("datasetPathOverride"), datasetPathOverride},
      {QStringLiteral("scenePathOverride"), scenePathOverride},
      {QStringLiteral("totalBytes"), totalBytes},
      {QStringLiteral("files"), files}};
  QSaveFile output(manifestPath);
  output.setDirectWriteFallback(false);
  if (!output.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create backup manifest: %1")
                    .arg(output.errorString()));
    return std::nullopt;
  }
  const QByteArray serialized =
      QJsonDocument(manifest).toJson(QJsonDocument::Indented);
  if (output.write(serialized) != serialized.size()) {
    output.cancelWriting();
    assignError(errorMessage,
                QStringLiteral("Unable to write backup manifest: %1")
                    .arg(output.errorString()));
    return std::nullopt;
  }
  if (!output.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to commit backup manifest: %1")
                    .arg(output.errorString()));
    return std::nullopt;
  }
  return ExternalBackupSnapshot{
      snapshotId, normalizedAbsolutePath(manifestPath), key,
      projectName(projectFile), createdUtc, files.size(), totalBytes};
}

QList<ExternalBackupSnapshot>
ExternalBackupStore::snapshots(QString *errorMessage) const {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QList<ExternalBackupSnapshot> result;
  if (!QDir(mBackupRoot).exists()) {
    return result;
  }
  QStringList invalid;
  QDirIterator iterator(mBackupRoot, {QStringLiteral("*.backup.json")},
                        QDir::Files | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString path = iterator.next();
    QString readError;
    const std::optional<ExternalBackupSnapshot> snapshot =
        readSnapshot(path, nullptr, &readError);
    if (snapshot.has_value()) {
      result.append(*snapshot);
    } else {
      invalid.append(QStringLiteral("%1: %2").arg(path, readError));
    }
  }
  std::sort(result.begin(), result.end(),
            [](const ExternalBackupSnapshot &left,
               const ExternalBackupSnapshot &right) {
              return left.createdUtc > right.createdUtc;
            });
  if (!invalid.isEmpty()) {
    assignError(errorMessage,
                QStringLiteral("Some external backups are invalid:\n%1")
                    .arg(invalid.join(QLatin1Char('\n'))));
  }
  return result;
}

bool ExternalBackupStore::restore(
    const ExternalBackupSnapshot &snapshot, const QString &destinationRoot,
    QString *errorMessage) const {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QJsonObject manifest;
  const std::optional<ExternalBackupSnapshot> stored =
      readSnapshot(snapshot.manifestPath, &manifest, errorMessage);
  if (!stored.has_value() ||
      stored->snapshotId != snapshot.snapshotId) {
    if (stored.has_value()) {
      assignError(errorMessage,
                  QStringLiteral("Backup snapshot identity mismatch."));
    }
    return false;
  }
  const QJsonArray files =
      manifest.value(QStringLiteral("files")).toArray();
  const QString destination = normalizedAbsolutePath(destinationRoot);
  if (!QDir().mkpath(destination)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create backup restore directory."));
    return false;
  }
  const QString projectBackupRoot =
      QFileInfo(QFileInfo(snapshot.manifestPath).absolutePath())
          .absolutePath();
  const QString objectsRoot =
      QDir(projectBackupRoot).filePath(QStringLiteral("objects"));
  for (const QJsonValue &value : files) {
    const QJsonObject file = value.toObject();
    const QString relative =
        file.value(QStringLiteral("restorePath")).toString();
    const QString sha256 =
        file.value(QStringLiteral("sha256")).toString();
    const qint64 size = file.value(QStringLiteral("size")).toInteger(-1);
    const QString target = QDir(destination).filePath(relative);
    const QString object =
        QDir(objectsRoot).filePath(sha256 + QStringLiteral(".object"));
    if (relative.isEmpty() || sha256.size() != 64 || size < 0 ||
        !pathInside(destination, target) ||
        QFileInfo(object).size() != size) {
      assignError(errorMessage,
                  QStringLiteral("Backup manifest contains an invalid file."));
      return false;
    }
    QString hashError;
    if (hashFile(object, &hashError) != sha256) {
      assignError(errorMessage,
                  hashError.isEmpty()
                      ? QStringLiteral("Backup object checksum mismatch: %1")
                            .arg(object)
                      : hashError);
      return false;
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
      assignError(errorMessage,
                  QStringLiteral("Unable to create restored directory: %1")
                      .arg(QFileInfo(target).absolutePath()));
      return false;
    }
    QFile input(object);
    QSaveFile output(target);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) ||
        !output.open(QIODevice::WriteOnly)) {
      assignError(errorMessage,
                  QStringLiteral("Unable to restore backup file: %1")
                      .arg(relative));
      return false;
    }
    while (!input.atEnd()) {
      const QByteArray chunk = input.read(4LL * 1024LL * 1024LL);
      if ((chunk.isEmpty() && input.error() != QFileDevice::NoError) ||
          output.write(chunk) != chunk.size()) {
        output.cancelWriting();
        assignError(errorMessage,
                    QStringLiteral("Unable to write restored file: %1")
                        .arg(relative));
        return false;
      }
    }
    if (!output.commit()) {
      assignError(errorMessage,
                  QStringLiteral("Unable to publish restored file: %1")
                      .arg(relative));
      return false;
    }
  }
  const QString datasetPathOverride =
      manifest.value(QStringLiteral("datasetPathOverride")).toString();
  const QString scenePathOverride =
      manifest.value(QStringLiteral("scenePathOverride")).toString();
  const QString dataDirectoryName =
      manifest.value(QStringLiteral("dataDirectoryName")).toString();
  if (!dataDirectoryName.isEmpty() || !datasetPathOverride.isEmpty() ||
      !scenePathOverride.isEmpty()) {
    const QString restoredProject =
        QDir(destination)
            .filePath(
                manifest.value(QStringLiteral("projectRestorePath"))
                    .toString());
    QFile projectFile(restoredProject);
    if (!projectFile.open(QIODevice::ReadOnly)) {
      assignError(errorMessage,
                  QStringLiteral("Unable to open restored project for link "
                                 "repair: %1")
                      .arg(projectFile.errorString()));
      return false;
    }
    QJsonParseError parseError;
    QJsonDocument projectDocument =
        QJsonDocument::fromJson(projectFile.readAll(), &parseError);
    projectFile.close();
    if (parseError.error != QJsonParseError::NoError ||
        !projectDocument.isObject()) {
      assignError(errorMessage,
                  QStringLiteral("Restored project JSON is invalid: %1")
                      .arg(parseError.errorString()));
      return false;
    }
    QJsonObject project = projectDocument.object();
    if (!dataDirectoryName.isEmpty()) {
      project.insert(QStringLiteral("rootPath"), dataDirectoryName);
    }
    if (!datasetPathOverride.isEmpty()) {
      project.insert(QStringLiteral("datasetPath"),
                     datasetPathOverride);
    }
    if (!scenePathOverride.isEmpty()) {
      project.insert(QStringLiteral("scenePath"), scenePathOverride);
    }
    QSaveFile repairedProject(restoredProject);
    repairedProject.setDirectWriteFallback(false);
    if (!repairedProject.open(QIODevice::WriteOnly)) {
      assignError(errorMessage,
                  QStringLiteral("Unable to repair restored project links: %1")
                      .arg(repairedProject.errorString()));
      return false;
    }
    const QByteArray repaired =
        QJsonDocument(project).toJson(QJsonDocument::Indented);
    if (repairedProject.write(repaired) != repaired.size()) {
      repairedProject.cancelWriting();
      assignError(errorMessage,
                  QStringLiteral("Unable to write restored project links: %1")
                      .arg(repairedProject.errorString()));
      return false;
    }
    if (!repairedProject.commit()) {
      assignError(errorMessage,
                  QStringLiteral("Unable to commit restored project links: %1")
                      .arg(repairedProject.errorString()));
      return false;
    }
  }
  return true;
}

QString ExternalBackupStore::rootPath() const { return mBackupRoot; }

} // namespace gsw
