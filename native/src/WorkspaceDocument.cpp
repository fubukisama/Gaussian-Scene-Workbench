#include "WorkspaceDocument.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace gsw {

namespace {
QString normalizedAbsolutePath(const QString &path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

Qt::CaseSensitivity pathCaseSensitivity() {
#ifdef Q_OS_WIN
  return Qt::CaseInsensitive;
#else
  return Qt::CaseSensitive;
#endif
}

bool pathsEqual(const QString &left, const QString &right) {
  return normalizedAbsolutePath(left).compare(normalizedAbsolutePath(right),
                                              pathCaseSensitivity()) == 0;
}

bool relativePathInside(const QString &rootPath, const QString &candidatePath,
                        QString *relativePath = nullptr) {
  if (rootPath.isEmpty() || candidatePath.isEmpty()) {
    return false;
  }
  const QString relative =
      QDir(rootPath).relativeFilePath(normalizedAbsolutePath(candidatePath));
  const bool inside = !QDir::isAbsolutePath(relative) &&
                      relative != QStringLiteral("..") &&
                      !relative.startsWith(QStringLiteral("../")) &&
                      !relative.startsWith(QStringLiteral("..\\"));
  if (inside && relativePath != nullptr) {
    *relativePath = relative;
  }
  return inside;
}

QString portablePathForRoot(const QString &absolutePath,
                            const QString &rootPath) {
  QString relative;
  return relativePathInside(rootPath, absolutePath, &relative) ? relative
                                                               : absolutePath;
}

QString remapManagedPath(const QString &absolutePath,
                         const QString &oldRootPath,
                         const QString &newRootPath) {
  if (absolutePath.isEmpty()) {
    return {};
  }
  QString relative;
  if (!relativePathInside(oldRootPath, absolutePath, &relative)) {
    return absolutePath;
  }
  return relative == QStringLiteral(".")
             ? newRootPath
             : QDir::cleanPath(QDir(newRootPath).filePath(relative));
}

QString projectStem(const QString &projectFilePath) {
  QString name = QFileInfo(projectFilePath).fileName();
  const QString suffix = QStringLiteral(".gsw.json");
  if (name.endsWith(suffix, Qt::CaseInsensitive)) {
    name.chop(suffix.size());
  } else {
    name = QFileInfo(projectFilePath).completeBaseName();
  }
  return name.isEmpty() ? QStringLiteral("Gaussian Scene Project") : name;
}

void assignError(QString *target, const QString &message) {
  if (target != nullptr) {
    *target = message;
  }
}

constexpr auto kDataMigrationMarker = ".gsw-data-migration.json";

bool writeDataMigrationMarker(const QString &dataRoot,
                              const QString &projectFilePath,
                              const QString &sourceRoot,
                              QString *errorMessage) {
  const QString markerPath =
      QDir(dataRoot).filePath(QString::fromLatin1(kDataMigrationMarker));
  QSaveFile marker(markerPath);
  if (!marker.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QObject::tr("Unable to create data migration marker: %1")
                    .arg(marker.errorString()));
    return false;
  }

  const QJsonObject markerJson{
      {QStringLiteral("version"), 1},
      {QStringLiteral("projectFilePath"),
       normalizedAbsolutePath(projectFilePath)},
      {QStringLiteral("sourceRoot"), normalizedAbsolutePath(sourceRoot)}};
  const QByteArray serialized =
      QJsonDocument(markerJson).toJson(QJsonDocument::Compact);
  if (marker.write(serialized) != serialized.size()) {
    marker.cancelWriting();
    assignError(errorMessage,
                QObject::tr("Unable to write data migration marker: %1")
                    .arg(marker.errorString()));
    return false;
  }
  if (!marker.commit()) {
    assignError(errorMessage,
                QObject::tr("Unable to commit data migration marker: %1")
                    .arg(marker.errorString()));
    return false;
  }
  return true;
}

bool dataMigrationMarkerMatches(const QString &dataRoot,
                                const QString &projectFilePath,
                                const QString &sourceRoot) {
  QFile marker(
      QDir(dataRoot).filePath(QString::fromLatin1(kDataMigrationMarker)));
  if (!marker.open(QIODevice::ReadOnly)) {
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(marker.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return false;
  }
  const QJsonObject markerJson = document.object();
  return markerJson.value(QStringLiteral("version")).toInt() == 1 &&
         pathsEqual(
             markerJson.value(QStringLiteral("projectFilePath")).toString(),
             projectFilePath) &&
         pathsEqual(markerJson.value(QStringLiteral("sourceRoot")).toString(),
                    sourceRoot);
}

bool copyDirectoryTree(const QString &sourceRoot, const QString &targetRoot,
                       const QStringList &excludedPaths,
                       QString *errorMessage) {
  if (!QDir().mkpath(targetRoot)) {
    assignError(errorMessage,
                QObject::tr("Unable to create project data directory: %1")
                    .arg(targetRoot));
    return false;
  }

  QDirIterator iterator(sourceRoot,
                        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                            QDir::System,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString sourcePath = iterator.next();
    bool excluded = false;
    for (const QString &excludedPath : excludedPaths) {
      if (!excludedPath.isEmpty() && pathsEqual(sourcePath, excludedPath)) {
        excluded = true;
        break;
      }
    }
    if (excluded) {
      continue;
    }

    const QFileInfo sourceInfo = iterator.fileInfo();
    if (sourceInfo.isSymLink()) {
      assignError(
          errorMessage,
          QObject::tr("Project data contains an unsupported symbolic link: %1")
              .arg(sourcePath));
      return false;
    }
    const QString relativePath = QDir(sourceRoot).relativeFilePath(sourcePath);
    const QString targetPath = QDir(targetRoot).filePath(relativePath);
    if (sourceInfo.isDir()) {
      if (!QDir().mkpath(targetPath)) {
        assignError(errorMessage,
                    QObject::tr("Unable to create project data directory: %1")
                        .arg(targetPath));
        return false;
      }
      continue;
    }
    if (!QDir().mkpath(QFileInfo(targetPath).absolutePath()) ||
        !QFile::copy(sourcePath, targetPath)) {
      assignError(
          errorMessage,
          QObject::tr("Unable to copy project data: %1").arg(sourcePath));
      return false;
    }
  }
  return true;
}

qint64 countImageFiles(const QString &rootPath) {
  static const QSet<QString> extensions = {
      QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
      QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("bmp"),
      QStringLiteral("webp"), QStringLiteral("exr")};

  qint64 count = 0;
  const QFileInfoList files =
      QDir(rootPath).entryInfoList(QDir::Files | QDir::NoSymLinks);
  for (const QFileInfo &info : files) {
    if (extensions.contains(info.suffix().toLower())) {
      ++count;
    }
  }
  return count;
}
} // namespace

bool PlyMetadata::looksLikeGaussianSplat() const {
  const QSet<QString> propertySet(properties.cbegin(), properties.cend());
  return propertySet.contains(QStringLiteral("opacity")) &&
         propertySet.contains(QStringLiteral("scale_0")) &&
         propertySet.contains(QStringLiteral("rot_0"));
}

WorkspaceDocument::WorkspaceDocument(QObject *parent) : QObject(parent) {}

bool WorkspaceDocument::hasProject() const { return !mRootPath.isEmpty(); }
bool WorkspaceDocument::isUntitled() const {
  return hasProject() && mProjectFilePath.isEmpty();
}
bool WorkspaceDocument::isModified() const { return mModified; }
QString WorkspaceDocument::projectName() const { return mProjectName; }
QString WorkspaceDocument::rootPath() const { return mRootPath; }
QString WorkspaceDocument::projectFilePath() const { return mProjectFilePath; }
QString WorkspaceDocument::datasetPath() const { return mDatasetPath; }
QString WorkspaceDocument::scenePath() const { return mScenePath; }
qint64 WorkspaceDocument::imageCount() const { return mImageCount; }
PlyMetadata WorkspaceDocument::sceneMetadata() const { return mSceneMetadata; }
bool WorkspaceDocument::hasPendingDataMigration() const {
  return !mPendingDataRoot.isEmpty();
}

bool WorkspaceDocument::create(const QString &rootPath, QString *errorMessage) {
  const QFileInfo rootInfo(rootPath);
  if (!rootInfo.exists() || !rootInfo.isDir()) {
    assignError(errorMessage,
                tr("Project directory does not exist: %1").arg(rootPath));
    return false;
  }

  mRootPath = normalizedAbsolutePath(rootPath);
  mProjectName = QDir(mRootPath).dirName();
  if (mProjectName.isEmpty()) {
    mProjectName = QStringLiteral("Gaussian Scene Project");
  }
  mProjectFilePath.clear();
  mDatasetPath.clear();
  mScenePath.clear();
  mPendingDataRoot.clear();
  mImageCount = 0;
  mSceneMetadata = {};
  setModified(true);
  emit changed();
  return true;
}

bool WorkspaceDocument::createUntitled(const QString &workingRoot,
                                       const QString &displayName,
                                       QString *errorMessage) {
  const QFileInfo rootInfo(workingRoot);
  if (!rootInfo.exists() || !rootInfo.isDir()) {
    assignError(
        errorMessage,
        tr("Temporary project directory does not exist: %1").arg(workingRoot));
    return false;
  }

  mRootPath = normalizedAbsolutePath(workingRoot);
  mProjectName = displayName.trimmed().isEmpty() ? tr("Untitled Project")
                                                 : displayName.trimmed();
  mProjectFilePath.clear();
  mDatasetPath.clear();
  mScenePath.clear();
  mPendingDataRoot.clear();
  mImageCount = 0;
  mSceneMetadata = {};
  setModified(false);
  emit changed();
  return true;
}

bool WorkspaceDocument::load(const QString &filePath, QString *errorMessage) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                tr("Unable to open project: %1").arg(file.errorString()));
    return false;
  }

  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                tr("Invalid project file: %1").arg(parseError.errorString()));
    return false;
  }

  const QJsonObject root = document.object();
  const int schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt();
  if (schemaVersion != 1) {
    assignError(
        errorMessage,
        tr("Unsupported project schema version: %1").arg(schemaVersion));
    return false;
  }

  mProjectFilePath = normalizedAbsolutePath(filePath);
  const QString storedRoot = root.value(QStringLiteral("rootPath")).toString();
  const QString projectDirectory = QFileInfo(mProjectFilePath).absolutePath();
  if (storedRoot.isEmpty()) {
    mRootPath = projectDirectory;
  } else if (QDir::isAbsolutePath(storedRoot)) {
    mRootPath = normalizedAbsolutePath(storedRoot);
  } else {
    mRootPath =
        QDir::cleanPath(QDir(projectDirectory).absoluteFilePath(storedRoot));
  }
  mProjectName = root.value(QStringLiteral("projectName"))
                     .toString(QDir(mRootPath).dirName());
  mDatasetPath =
      resolvePortablePath(root.value(QStringLiteral("datasetPath")).toString());
  mScenePath =
      resolvePortablePath(root.value(QStringLiteral("scenePath")).toString());
  const QString storedPendingDataRoot =
      root.value(QStringLiteral("pendingDataRoot")).toString();
  mPendingDataRoot =
      storedPendingDataRoot.isEmpty() ? QString()
      : QDir::isAbsolutePath(storedPendingDataRoot)
          ? normalizedAbsolutePath(storedPendingDataRoot)
          : normalizedAbsolutePath(
                QDir(projectDirectory).filePath(storedPendingDataRoot));
  mImageCount = countDatasetImages(mDatasetPath);
  mSceneMetadata = inspectPly(mScenePath);
  setModified(false);
  emit changed();
  return true;
}

bool WorkspaceDocument::save(const QString &filePath, QString *errorMessage) {
  if (!hasProject()) {
    assignError(errorMessage, tr("No project is open."));
    return false;
  }

  const QString targetPath =
      filePath.isEmpty() ? mProjectFilePath : normalizedAbsolutePath(filePath);
  if (targetPath.isEmpty()) {
    assignError(errorMessage, tr("A project file path is required."));
    return false;
  }

  const QFileInfo targetInfo(targetPath);
  if (!QFileInfo(targetInfo.absolutePath()).isDir()) {
    assignError(errorMessage, tr("Project save directory does not exist: %1")
                                  .arg(targetInfo.absolutePath()));
    return false;
  }

  const bool saveAs =
      !filePath.isEmpty() &&
      (mProjectFilePath.isEmpty() || !pathsEqual(targetPath, mProjectFilePath));
  const QString oldRootPath = mRootPath;
  QString savedRootPath = mRootPath;
  QString savedDatasetPath = mDatasetPath;
  QString savedScenePath = mScenePath;
  QString savedProjectName = mProjectName;
  bool createdDataRoot = false;

  if (saveAs) {
    const QString targetDataRoot = projectDataRootForFile(targetPath);
    if (!pathsEqual(oldRootPath, targetDataRoot)) {
      if (relativePathInside(oldRootPath, targetDataRoot)) {
        assignError(errorMessage, tr("Choose a project file outside the "
                                     "current working data directory."));
        return false;
      }
      if (QFileInfo::exists(targetDataRoot)) {
        assignError(errorMessage,
                    tr("Project data directory already exists: %1")
                        .arg(targetDataRoot));
        return false;
      }

      const QString stagingRoot =
          QDir(targetInfo.absolutePath())
              .filePath(
                  QStringLiteral(".%1.gsw-stage-%2")
                      .arg(QFileInfo(targetDataRoot).fileName(),
                           QUuid::createUuid().toString(QUuid::WithoutBraces)));
      const QStringList exclusions = {mProjectFilePath, targetPath};
      if (!copyDirectoryTree(oldRootPath, stagingRoot, exclusions,
                             errorMessage)) {
        QDir(stagingRoot).removeRecursively();
        return false;
      }
      if (!QDir().rename(stagingRoot, targetDataRoot)) {
        QDir(stagingRoot).removeRecursively();
        assignError(errorMessage,
                    tr("Unable to finalize project data directory: %1")
                        .arg(targetDataRoot));
        return false;
      }
      createdDataRoot = true;
      savedRootPath = normalizedAbsolutePath(targetDataRoot);
      savedDatasetPath =
          remapManagedPath(mDatasetPath, oldRootPath, savedRootPath);
      savedScenePath = remapManagedPath(mScenePath, oldRootPath, savedRootPath);
    }
    savedProjectName = projectStem(targetPath);
  }

  QJsonObject root;
  root.insert(QStringLiteral("schemaVersion"), 1);
  root.insert(QStringLiteral("application"),
              QStringLiteral("Gaussian Scene Workbench"));
  root.insert(QStringLiteral("projectName"), savedProjectName);
  const QString projectDirectory = QFileInfo(targetPath).absolutePath();
  QString relativeRoot;
  const QString storedRoot =
      relativePathInside(projectDirectory, savedRootPath, &relativeRoot)
          ? relativeRoot
          : savedRootPath;
  root.insert(QStringLiteral("rootPath"), storedRoot);
  root.insert(QStringLiteral("datasetPath"),
              portablePathForRoot(savedDatasetPath, savedRootPath));
  root.insert(QStringLiteral("scenePath"),
              portablePathForRoot(savedScenePath, savedRootPath));
  root.insert(QStringLiteral("updatedUtc"),
              QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

  QSaveFile output(targetPath);
  if (!output.open(QIODevice::WriteOnly)) {
    if (createdDataRoot) {
      QDir(savedRootPath).removeRecursively();
    }
    assignError(errorMessage,
                tr("Unable to save project: %1").arg(output.errorString()));
    return false;
  }
  const QByteArray serialized =
      QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (output.write(serialized) != serialized.size()) {
    output.cancelWriting();
    if (createdDataRoot) {
      QDir(savedRootPath).removeRecursively();
    }
    assignError(
        errorMessage,
        tr("Unable to write project file: %1").arg(output.errorString()));
    return false;
  }
  if (!output.commit()) {
    if (createdDataRoot) {
      QDir(savedRootPath).removeRecursively();
    }
    assignError(
        errorMessage,
        tr("Unable to commit project file: %1").arg(output.errorString()));
    return false;
  }

  mProjectName = savedProjectName;
  mRootPath = savedRootPath;
  mProjectFilePath = targetPath;
  mDatasetPath = savedDatasetPath;
  mScenePath = savedScenePath;
  mPendingDataRoot.clear();
  setModified(false);
  emit changed();
  return true;
}

bool WorkspaceDocument::saveManifest(const QString &filePath,
                                     QString *errorMessage) {
  if (!hasProject()) {
    assignError(errorMessage, tr("No project is open."));
    return false;
  }

  const QString targetPath =
      filePath.isEmpty() ? mProjectFilePath : normalizedAbsolutePath(filePath);
  if (targetPath.isEmpty()) {
    assignError(errorMessage, tr("A project file path is required."));
    return false;
  }

  const QFileInfo targetInfo(targetPath);
  if (!QFileInfo(targetInfo.absolutePath()).isDir()) {
    assignError(errorMessage, tr("Project save directory does not exist: %1")
                                  .arg(targetInfo.absolutePath()));
    return false;
  }

  const bool saveAs =
      !filePath.isEmpty() &&
      (mProjectFilePath.isEmpty() || !pathsEqual(targetPath, mProjectFilePath));
  QString pendingDataRoot = mPendingDataRoot;
  if (saveAs) {
    pendingDataRoot = projectDataRootForFile(targetPath);
    if (!pathsEqual(mRootPath, pendingDataRoot)) {
      if (relativePathInside(mRootPath, pendingDataRoot)) {
        assignError(errorMessage, tr("Choose a project file outside the "
                                     "current working data directory."));
        return false;
      }
      if (QFileInfo::exists(pendingDataRoot)) {
        assignError(errorMessage,
                    tr("Project data directory already exists: %1")
                        .arg(pendingDataRoot));
        return false;
      }
    } else {
      pendingDataRoot.clear();
    }
  }

  const QString savedProjectName =
      saveAs ? projectStem(targetPath) : mProjectName;
  const QString projectDirectory = targetInfo.absolutePath();
  QJsonObject root;
  root.insert(QStringLiteral("schemaVersion"), 1);
  root.insert(QStringLiteral("application"),
              QStringLiteral("Gaussian Scene Workbench"));
  root.insert(QStringLiteral("projectName"), savedProjectName);
  QString relativeRoot;
  root.insert(QStringLiteral("rootPath"),
              relativePathInside(projectDirectory, mRootPath, &relativeRoot)
                  ? relativeRoot
                  : mRootPath);
  root.insert(QStringLiteral("datasetPath"),
              portablePathForRoot(mDatasetPath, mRootPath));
  root.insert(QStringLiteral("scenePath"),
              portablePathForRoot(mScenePath, mRootPath));
  if (!pendingDataRoot.isEmpty()) {
    QString relativePendingRoot;
    root.insert(QStringLiteral("pendingDataRoot"),
                relativePathInside(projectDirectory, pendingDataRoot,
                                   &relativePendingRoot)
                    ? relativePendingRoot
                    : pendingDataRoot);
  }
  root.insert(QStringLiteral("updatedUtc"),
              QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

  QSaveFile output(targetPath);
  if (!output.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                tr("Unable to save project: %1").arg(output.errorString()));
    return false;
  }
  const QByteArray serialized =
      QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (output.write(serialized) != serialized.size()) {
    output.cancelWriting();
    assignError(
        errorMessage,
        tr("Unable to write project file: %1").arg(output.errorString()));
    return false;
  }
  if (!output.commit()) {
    assignError(
        errorMessage,
        tr("Unable to commit project file: %1").arg(output.errorString()));
    return false;
  }

  mProjectName = savedProjectName;
  mProjectFilePath = targetPath;
  mPendingDataRoot = pendingDataRoot;
  setModified(false);
  emit changed();
  return true;
}

QByteArray WorkspaceDocument::recoveryManifestJson() const {
  if (!hasProject()) {
    return {};
  }
  QJsonObject root;
  root.insert(QStringLiteral("schemaVersion"), 1);
  root.insert(QStringLiteral("application"),
              QStringLiteral("Gaussian Scene Workbench"));
  root.insert(QStringLiteral("projectName"), mProjectName);
  root.insert(QStringLiteral("rootPath"), mRootPath);
  root.insert(QStringLiteral("datasetPath"),
              portablePathForRoot(mDatasetPath, mRootPath));
  root.insert(QStringLiteral("scenePath"),
              portablePathForRoot(mScenePath, mRootPath));
  if (!mPendingDataRoot.isEmpty()) {
    root.insert(QStringLiteral("pendingDataRoot"), mPendingDataRoot);
  }
  root.insert(QStringLiteral("updatedUtc"),
              QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool WorkspaceDocument::finalizeDataMigration(QString *errorMessage) {
  if (!hasPendingDataMigration()) {
    return true;
  }
  if (mProjectFilePath.isEmpty()) {
    assignError(
        errorMessage,
        tr("A project file is required before migrating project data."));
    return false;
  }

  const QString oldRootPath = mRootPath;
  const QString targetDataRoot = mPendingDataRoot;
  if (relativePathInside(oldRootPath, targetDataRoot)) {
    assignError(errorMessage, tr("Choose a project file outside the current "
                                 "working data directory."));
    return false;
  }
  bool publishedByThisCall = false;
  if (QFileInfo::exists(targetDataRoot)) {
    if (!QFileInfo(targetDataRoot).isDir() ||
        !dataMigrationMarkerMatches(targetDataRoot, mProjectFilePath,
                                    oldRootPath)) {
      assignError(
          errorMessage,
          tr("Project data directory already exists and does not belong to "
             "this pending save: %1")
              .arg(targetDataRoot));
      return false;
    }
  } else {
    const QFileInfo targetInfo(targetDataRoot);
    const QString stagingRoot =
        QDir(targetInfo.absolutePath())
            .filePath(
                QStringLiteral(".%1.gsw-stage-%2")
                    .arg(targetInfo.fileName(),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QStringList exclusions = {mProjectFilePath, targetDataRoot};
    if (!copyDirectoryTree(oldRootPath, stagingRoot, exclusions,
                           errorMessage)) {
      QDir(stagingRoot).removeRecursively();
      return false;
    }
    if (!writeDataMigrationMarker(stagingRoot, mProjectFilePath, oldRootPath,
                                  errorMessage)) {
      QDir(stagingRoot).removeRecursively();
      return false;
    }
    if (!QDir().rename(stagingRoot, targetDataRoot)) {
      QDir(stagingRoot).removeRecursively();
      assignError(errorMessage,
                  tr("Unable to finalize project data directory: %1")
                      .arg(targetDataRoot));
      return false;
    }
    publishedByThisCall = true;
  }

  const QString oldDatasetPath = mDatasetPath;
  const QString oldScenePath = mScenePath;
  mRootPath = normalizedAbsolutePath(targetDataRoot);
  mDatasetPath = remapManagedPath(oldDatasetPath, oldRootPath, mRootPath);
  mScenePath = remapManagedPath(oldScenePath, oldRootPath, mRootPath);
  mPendingDataRoot.clear();

  if (!saveManifest({}, errorMessage)) {
    mRootPath = oldRootPath;
    mDatasetPath = oldDatasetPath;
    mScenePath = oldScenePath;
    mPendingDataRoot = targetDataRoot;
    if (publishedByThisCall) {
      QDir(targetDataRoot).removeRecursively();
    }
    return false;
  }
  QFile::remove(
      QDir(targetDataRoot).filePath(QString::fromLatin1(kDataMigrationMarker)));
  return true;
}

bool WorkspaceDocument::setDatasetPath(const QString &path,
                                       QString *errorMessage) {
  const QFileInfo info(path);
  if (!info.exists() || !info.isDir()) {
    assignError(errorMessage,
                tr("Dataset directory does not exist: %1").arg(path));
    return false;
  }
  mDatasetPath = normalizedAbsolutePath(path);
  mImageCount = countDatasetImages(mDatasetPath);
  setModified(true);
  emit changed();
  return true;
}

bool WorkspaceDocument::setScenePath(const QString &path,
                                     QString *errorMessage) {
  const PlyMetadata metadata = inspectPly(path, errorMessage);
  if (!metadata.valid) {
    return false;
  }
  mScenePath = normalizedAbsolutePath(path);
  mSceneMetadata = metadata;
  setModified(true);
  emit changed();
  return true;
}

PlyMetadata WorkspaceDocument::inspectPly(const QString &filePath,
                                          QString *errorMessage) {
  PlyMetadata metadata;
  if (filePath.isEmpty()) {
    return metadata;
  }

  QFile file(filePath);
  const QFileInfo info(filePath);
  metadata.fileSize = info.size();
  if (!file.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                tr("Unable to open PLY file: %1").arg(file.errorString()));
    return metadata;
  }

  constexpr qint64 kMaximumHeaderBytes = 1024 * 1024;
  qint64 consumed = 0;
  bool firstLine = true;
  bool foundEndHeader = false;
  bool readingVertexProperties = false;

  while (!file.atEnd() && consumed < kMaximumHeaderBytes) {
    const QByteArray rawLine = file.readLine();
    consumed += rawLine.size();
    const QString line = QString::fromLatin1(rawLine).trimmed();

    if (firstLine) {
      firstLine = false;
      if (line != QStringLiteral("ply")) {
        assignError(errorMessage, tr("The selected file is not a PLY file."));
        return metadata;
      }
      continue;
    }

    if (line.startsWith(QStringLiteral("format "))) {
      metadata.format = line.section(QLatin1Char(' '), 1, 1);
    } else if (line.startsWith(QStringLiteral("element "))) {
      const QStringList parts =
          line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
      readingVertexProperties =
          parts.size() >= 3 && parts.at(1) == QStringLiteral("vertex");
      if (readingVertexProperties) {
        bool ok = false;
        const qint64 count = parts.at(2).toLongLong(&ok);
        if (ok) {
          metadata.vertexCount = count;
        }
      }
    } else if (readingVertexProperties &&
               line.startsWith(QStringLiteral("property "))) {
      const QString propertyName = line.section(QLatin1Char(' '), -1);
      if (!propertyName.isEmpty()) {
        metadata.properties.append(propertyName);
      }
    } else if (line == QStringLiteral("end_header")) {
      foundEndHeader = true;
      break;
    }
  }

  if (!foundEndHeader || metadata.format.isEmpty()) {
    assignError(errorMessage,
                tr("The PLY header is incomplete or unsupported."));
    return metadata;
  }
  if (metadata.format != QStringLiteral("ascii") &&
      metadata.format != QStringLiteral("binary_little_endian")) {
    assignError(errorMessage,
                tr("Unsupported PLY format: %1").arg(metadata.format));
    return metadata;
  }
  metadata.valid = true;
  return metadata;
}

qint64 WorkspaceDocument::countDatasetImages(const QString &directoryPath) {
  if (directoryPath.isEmpty() || !QFileInfo(directoryPath).isDir()) {
    return 0;
  }

  const QDir datasetRoot(directoryPath);
  for (const QString &standardDirectory :
       {QStringLiteral("input"), QStringLiteral("images")}) {
    const QString candidate = datasetRoot.filePath(standardDirectory);
    if (QFileInfo(candidate).isDir()) {
      const qint64 count = countImageFiles(candidate);
      if (count > 0) {
        return count;
      }
    }
  }

  return countImageFiles(directoryPath);
}

QString
WorkspaceDocument::projectDataRootForFile(const QString &projectFilePath) {
  if (projectFilePath.isEmpty()) {
    return {};
  }
  const QFileInfo info(normalizedAbsolutePath(projectFilePath));
  return QDir::cleanPath(QDir(info.absolutePath())
                             .filePath(projectStem(info.absoluteFilePath()) +
                                       QStringLiteral(".files")));
}

void WorkspaceDocument::setModified(const bool modified) {
  if (mModified == modified) {
    return;
  }
  mModified = modified;
  emit modifiedChanged(mModified);
}

QString
WorkspaceDocument::resolvePortablePath(const QString &storedPath) const {
  if (storedPath.isEmpty() || QDir::isAbsolutePath(storedPath)) {
    return storedPath;
  }
  return QDir::cleanPath(QDir(mRootPath).absoluteFilePath(storedPath));
}

} // namespace gsw
