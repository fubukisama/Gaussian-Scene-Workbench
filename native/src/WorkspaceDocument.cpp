#include "WorkspaceDocument.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>

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

bool finiteVector(const QVector3D &value) {
  return std::isfinite(value.x()) && std::isfinite(value.y()) &&
         std::isfinite(value.z());
}

QJsonObject sceneTransformJson(const QVector3D &translation) {
  return {{QStringLiteral("translation"),
           QJsonArray{translation.x(), translation.y(), translation.z()}}};
}

QVector3D sceneTranslationFromJson(const QJsonValue &value) {
  if (!value.isObject()) {
    return {};
  }
  const QJsonArray translation =
      value.toObject().value(QStringLiteral("translation")).toArray();
  if (translation.size() != 3) {
    return {};
  }
  const QVector3D parsed(static_cast<float>(translation.at(0).toDouble()),
                         static_cast<float>(translation.at(1).toDouble()),
                         static_cast<float>(translation.at(2).toDouble()));
  return finiteVector(parsed) ? parsed : QVector3D();
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

QStringList reconstructionArtifactPaths(const QString &datasetPath) {
  if (datasetPath.isEmpty() || !QFileInfo(datasetPath).isDir()) {
    return {};
  }
  const QDir dataset(datasetPath);
  QStringList artifacts;
  const auto appendExisting = [&artifacts](const QString &path) {
    const QString cleanPath = QDir::cleanPath(path);
    if (QFileInfo::exists(cleanPath) && !artifacts.contains(cleanPath)) {
      artifacts.append(cleanPath);
    }
  };
  for (const QString &name :
       {QStringLiteral("distorted"), QStringLiteral("sparse"),
        QStringLiteral("stereo"), QStringLiteral("database.db"),
        QStringLiteral(".alignment_cache"),
        QStringLiteral(".colmap-undistorted"),
        QStringLiteral(".gsw/previews/colmap")}) {
    appendExisting(dataset.filePath(name));
  }
  const QFileInfoList transientArtifacts = dataset.entryInfoList(
      {QStringLiteral(".colmap-undistorted-*"),
       QStringLiteral(".colmap-backup-*")},
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
          QDir::System,
      QDir::Name);
  for (const QFileInfo &artifact : transientArtifacts) {
    appendExisting(artifact.absoluteFilePath());
  }
  return artifacts;
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
QVector3D WorkspaceDocument::sceneTranslation() const {
  return mSceneTranslation;
}
qint64 WorkspaceDocument::imageCount() const { return mImageCount; }
PlyMetadata WorkspaceDocument::sceneMetadata() const { return mSceneMetadata; }
bool WorkspaceDocument::hasPendingDataMigration() const {
  return !mPendingDataRoot.isEmpty();
}

bool WorkspaceDocument::isDatasetManaged() const {
  if (!hasProject() || mDatasetPath.isEmpty()) {
    return false;
  }
  const QString managedDatasetsRoot =
      QDir(mRootPath).filePath(QStringLiteral("datasets"));
  QString relative;
  return relativePathInside(managedDatasetsRoot, mDatasetPath, &relative) &&
         relative != QStringLiteral(".");
}

bool WorkspaceDocument::hasManagedReconstructionData() const {
  return isDatasetManaged() &&
         !reconstructionArtifactPaths(mDatasetPath).isEmpty();
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
  mSceneTranslation = {};
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
  mSceneTranslation = {};
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
  mSceneTranslation = sceneTranslationFromJson(
      root.value(QStringLiteral("sceneTransform")));
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
  root.insert(QStringLiteral("sceneTransform"),
              sceneTransformJson(mSceneTranslation));
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
  root.insert(QStringLiteral("sceneTransform"),
              sceneTransformJson(mSceneTranslation));
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
  root.insert(QStringLiteral("sceneTransform"),
              sceneTransformJson(mSceneTranslation));
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
  const QString normalizedPath = normalizedAbsolutePath(path);
  if (mScenePath.isEmpty() || !pathsEqual(mScenePath, normalizedPath)) {
    mSceneTranslation = {};
  }
  mScenePath = normalizedPath;
  mSceneMetadata = metadata;
  setModified(true);
  emit changed();
  return true;
}

bool WorkspaceDocument::setSceneTranslation(const QVector3D &translation,
                                            QString *errorMessage) {
  if (!hasProject()) {
    assignError(errorMessage, tr("No project is open."));
    return false;
  }
  if (!finiteVector(translation)) {
    assignError(errorMessage, tr("Scene translation must be finite."));
    return false;
  }
  const float comparisonScale =
      std::max({1.0F, mSceneTranslation.length(), translation.length()});
  if ((mSceneTranslation - translation).lengthSquared() <=
      comparisonScale * comparisonScale * 1.0e-12F) {
    return true;
  }
  mSceneTranslation = translation;
  setModified(true);
  emit changed();
  return true;
}

bool WorkspaceDocument::clearImportedData(
    const ImportCleanupOptions &options, ImportCleanupResult *result,
    QString *errorMessage) {
  if (result != nullptr) {
    *result = {};
  }
  if (!hasProject()) {
    assignError(errorMessage, tr("No project is open."));
    return false;
  }
  if (hasPendingDataMigration()) {
    assignError(errorMessage,
                tr("Finish the pending project data migration before "
                   "cleaning imported data."));
    return false;
  }

  const bool clearDataset = options.clearDataset && !mDatasetPath.isEmpty();
  const bool clearScene = options.clearScene && !mScenePath.isEmpty();
  if (!clearDataset && !clearScene) {
    assignError(errorMessage, tr("There is no selected imported data to clear."));
    return false;
  }

  const bool managedDataset = clearDataset && isDatasetManaged();
  if (managedDataset && !clearScene && !mScenePath.isEmpty() &&
      relativePathInside(mDatasetPath, mScenePath)) {
    assignError(errorMessage,
                tr("The current scene is stored inside the managed dataset. "
                   "Clear the scene association as well, or move the scene "
                   "outside the dataset first."));
    return false;
  }

  const QString previousDatasetPath = mDatasetPath;
  const QString previousScenePath = mScenePath;
  const qint64 previousImageCount = mImageCount;
  const PlyMetadata previousSceneMetadata = mSceneMetadata;
  const QVector3D previousSceneTranslation = mSceneTranslation;
  const bool previousModified = mModified;
  QString stagedDatasetPath;
  QString stagingRoot;

  if (managedDataset && QFileInfo(mDatasetPath).exists()) {
    const QFileInfo projectRootInfo(mRootPath);
    stagingRoot =
        QDir(projectRootInfo.absolutePath())
            .filePath(QStringLiteral(".%1.gsw-cleanup-%2")
                          .arg(projectRootInfo.fileName(),
                               QUuid::createUuid().toString(
                                   QUuid::WithoutBraces)));
    if (!QDir().mkpath(stagingRoot)) {
      assignError(errorMessage,
                  tr("Unable to prepare the managed dataset cleanup area: %1")
                      .arg(stagingRoot));
      return false;
    }
    stagedDatasetPath =
        QDir(stagingRoot)
            .filePath(QStringLiteral("%1-%2")
                          .arg(QUuid::createUuid().toString(
                                   QUuid::WithoutBraces),
                               QFileInfo(mDatasetPath).fileName()));
    if (!QDir().rename(mDatasetPath, stagedDatasetPath)) {
      QDir().rmdir(stagingRoot);
      assignError(errorMessage,
                  tr("Unable to stage the managed dataset for cleanup: %1")
                      .arg(mDatasetPath));
      return false;
    }
  }

  if (clearDataset) {
    mDatasetPath.clear();
    mImageCount = 0;
  }
  if (clearScene) {
    mScenePath.clear();
    mSceneMetadata = {};
    mSceneTranslation = {};
  }

  QString saveError;
  bool committed = true;
  if (!mProjectFilePath.isEmpty()) {
    committed = saveManifest({}, &saveError);
  } else {
    setModified(true);
    emit changed();
  }
  if (!committed) {
    mDatasetPath = previousDatasetPath;
    mScenePath = previousScenePath;
    mImageCount = previousImageCount;
    mSceneMetadata = previousSceneMetadata;
    mSceneTranslation = previousSceneTranslation;
    setModified(previousModified);
    bool restored = true;
    if (!stagedDatasetPath.isEmpty()) {
      restored = QDir().rename(stagedDatasetPath, previousDatasetPath);
      if (restored) {
        QDir().rmdir(stagingRoot);
      }
    }
    emit changed();
    assignError(
        errorMessage,
        restored
            ? tr("Unable to save the cleaned project state: %1").arg(saveError)
            : tr("Unable to save the cleaned project state, and the managed "
                 "dataset could not be restored from %1: %2")
                  .arg(stagedDatasetPath, saveError));
    return false;
  }

  bool managedDatasetRemoved = false;
  QString cleanupPendingPath;
  if (!stagedDatasetPath.isEmpty()) {
    if (QDir(stagedDatasetPath).removeRecursively()) {
      managedDatasetRemoved = true;
      QDir().rmdir(stagingRoot);
    } else {
      cleanupPendingPath = stagedDatasetPath;
    }
  }

  if (result != nullptr) {
    result->datasetCleared = clearDataset;
    result->sceneCleared = clearScene;
    result->managedDatasetRemoved = managedDatasetRemoved;
    result->previousDatasetPath = previousDatasetPath;
    result->previousScenePath = previousScenePath;
    result->cleanupPendingPath = cleanupPendingPath;
  }
  return true;
}

bool WorkspaceDocument::clearReconstructionData(
    ReconstructionCleanupResult *result, QString *errorMessage) {
  if (result != nullptr) {
    *result = {};
  }
  if (!hasProject()) {
    assignError(errorMessage, tr("No project is open."));
    return false;
  }
  if (hasPendingDataMigration()) {
    assignError(errorMessage,
                tr("Finish the pending project data migration before "
                   "cleaning reconstruction data."));
    return false;
  }
  if (mDatasetPath.isEmpty()) {
    assignError(errorMessage, tr("No dataset is attached."));
    return false;
  }
  if (!isDatasetManaged()) {
    assignError(errorMessage,
                tr("Reconstruction data belongs to an external linked "
                   "dataset and will not be modified."));
    return false;
  }

  const QStringList artifacts = reconstructionArtifactPaths(mDatasetPath);
  if (artifacts.isEmpty()) {
    assignError(errorMessage, tr("No managed reconstruction data was found."));
    return false;
  }

  const QFileInfo projectRootInfo(mRootPath);
  const QString stagingRoot =
      QDir(projectRootInfo.absolutePath())
          .filePath(QStringLiteral(".%1.gsw-reconstruction-cleanup-%2")
                        .arg(projectRootInfo.fileName(),
                             QUuid::createUuid().toString(
                                 QUuid::WithoutBraces)));
  if (!QDir().mkpath(stagingRoot)) {
    assignError(errorMessage,
                tr("Unable to prepare the reconstruction cleanup area: %1")
                    .arg(stagingRoot));
    return false;
  }

  QList<QPair<QString, QString>> stagedArtifacts;
  for (const QString &artifact : artifacts) {
    const QString stagedPath =
        QDir(stagingRoot).filePath(QFileInfo(artifact).fileName());
    if (!QDir().rename(artifact, stagedPath)) {
      bool restored = true;
      for (auto iterator = stagedArtifacts.crbegin();
           iterator != stagedArtifacts.crend(); ++iterator) {
        restored = QDir().rename(iterator->second, iterator->first) && restored;
      }
      if (restored) {
        QDir().rmdir(stagingRoot);
      }
      assignError(
          errorMessage,
          restored
              ? tr("Unable to stage reconstruction data for cleanup: %1")
                    .arg(artifact)
              : tr("Unable to stage reconstruction data for cleanup, and "
                   "some files could not be restored from: %1")
                    .arg(stagingRoot));
      return false;
    }
    stagedArtifacts.append(qMakePair(artifact, stagedPath));
  }

  QString cleanupPendingPath;
  if (!QDir(stagingRoot).removeRecursively()) {
    cleanupPendingPath = stagingRoot;
  }
  if (result != nullptr) {
    result->removedPaths = artifacts;
    result->cleanupPendingPath = cleanupPendingPath;
  }
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
      } else if (parts.size() >= 3 &&
                 parts.at(1).compare(QStringLiteral("face"),
                                     Qt::CaseInsensitive) == 0) {
        bool ok = false;
        const qint64 count = parts.at(2).toLongLong(&ok);
        if (ok) {
          metadata.faceCount = count;
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
      metadata.format != QStringLiteral("binary_little_endian") &&
      metadata.format != QStringLiteral("binary_big_endian")) {
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
