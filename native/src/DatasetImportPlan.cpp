#include <QCoreApplication>
#include "DatasetImportPlan.h"
#include "ManagedName.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <cmath>

namespace gsw {
namespace {

const QSet<QString> kImageExtensions = {
    QStringLiteral("jpg"),  QStringLiteral("jpeg"),
    QStringLiteral("png"),  QStringLiteral("bmp"),
    QStringLiteral("tif"),  QStringLiteral("tiff"),
    QStringLiteral("webp"),
};

const QSet<QString> kVideoExtensions = {
    QStringLiteral("mp4"),  QStringLiteral("mov"),
    QStringLiteral("avi"),  QStringLiteral("mkv"),
    QStringLiteral("webm"), QStringLiteral("m4v"),
};

QString deduplicationKey(const QFileInfo &fileInfo) {
  QString path = fileInfo.canonicalFilePath();
  if (path.isEmpty()) {
    path = fileInfo.absoluteFilePath();
  }
  return QDir::cleanPath(path).toCaseFolded();
}

QString absoluteCleanPath(const QString &path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

bool DatasetImportPlan::supportsFile(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return kImageExtensions.contains(suffix) || kVideoExtensions.contains(suffix);
}

std::optional<DatasetImportPlan>
DatasetImportPlan::create(const DatasetImportRequest &request,
                          QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }

  if (request.sceneName.trimmed().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Invalid scene name");
    }
    return std::nullopt;
  }

  if (!std::isfinite(request.framesPerSecond) ||
      request.framesPerSecond <= 0.0) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "Frames per second must be finite and greater than zero");
    }
    return std::nullopt;
  }

  DatasetImportPlan plan;
  plan.mSceneName = request.sceneName;
  plan.mFramesPerSecond = request.framesPerSecond;
  plan.mOverwrite = request.overwrite;
  plan.mSourceCount = request.sourcePaths.size();

  if (!request.cameraInput.isEmpty()) {
    const QString adapter = request.cameraInput.value(QStringLiteral("adapter")).toString();
    const QString policy = request.cameraInput.value(QStringLiteral("posePolicy")).toString();
    const QFileInfo source(request.cameraInput.value(QStringLiteral("sourcePath")).toString());
    const QSet<QString> adapters{QStringLiteral("auto"), QStringLiteral("blender_transforms"),
        QStringLiteral("colmap"), QStringLiteral("generic_json"), QStringLiteral("research_json"),
        QStringLiteral("trajectory_dataset")};
    if (!adapters.contains(adapter) || policy != QStringLiteral("locked")) {
      if (errorMessage) *errorMessage = QCoreApplication::translate(
          "Workbench", "已知相机导入需要受支持的适配器和锁定位姿策略；不会回退到 SfM。");
      return std::nullopt;
    }
    if (request.cameraInput.value(QStringLiteral("sourcePath")).toString().trimmed().isEmpty() ||
        !source.exists() || (!source.isFile() && !source.isDir()) || !source.isReadable()) {
      if (errorMessage) *errorMessage = QCoreApplication::translate(
          "Workbench", "相机来源不存在或不可读取；请重新选择文件或目录。不会运行 COLMAP。");
      return std::nullopt;
    }
    plan.mCameraInput = request.cameraInput;
    plan.mCameraInput.insert(QStringLiteral("sourcePath"), absoluteCleanPath(source.absoluteFilePath()));
    plan.mSourceCount = 1;
    // Formats, image mappings, projection, splits and units are validated once
    // by the shared Python adapter, not guessed again by the desktop planner.
    return plan;
  }

  QSet<QString> seenPaths;
  QSet<QString> usedDirectoryLabels;
  const QMimeDatabase mimeDatabase;
  const auto uniqueDirectoryLabel = [&](const QString &requestedLabel) {
    const QString baseLabel = requestedLabel.isEmpty()
                                  ? QStringLiteral("selected-directory")
                                  : requestedLabel;
    QString candidate = baseLabel;
    int suffix = 2;
    while (usedDirectoryLabels.contains(candidate.toCaseFolded())) {
      candidate = QStringLiteral("%1_%2").arg(baseLabel).arg(suffix);
      ++suffix;
    }
    usedDirectoryLabels.insert(candidate.toCaseFolded());
    return candidate;
  };
  const auto collectFile = [&](const QFileInfo &fileInfo,
                               const QString &relativePath) {
    if (!fileInfo.exists() || !fileInfo.isFile()) {
      return;
    }

    const QString extension = fileInfo.suffix().toLower();
    const bool isImage = kImageExtensions.contains(extension);
    const bool isVideo = kVideoExtensions.contains(extension);
    if (!isImage && !isVideo) {
      return;
    }

    const QString key = deduplicationKey(fileInfo);
    if (seenPaths.contains(key)) {
      return;
    }
    seenPaths.insert(key);

    QString absolutePath = fileInfo.canonicalFilePath();
    if (absolutePath.isEmpty()) {
      absolutePath = fileInfo.absoluteFilePath();
    }
    plan.mFiles.append({
        QDir::cleanPath(absolutePath),
        fileInfo.fileName(),
        mimeDatabase.mimeTypeForFile(fileInfo, QMimeDatabase::MatchExtension)
            .name(),
        QDir::fromNativeSeparators(QDir::cleanPath(relativePath)),
        fileInfo.size(),
        fileInfo.lastModified().toMSecsSinceEpoch(),
    });
    if (isImage) {
      ++plan.mImageCount;
    } else {
      ++plan.mVideoCount;
    }
    plan.mTotalBytes += fileInfo.size();
  };

  for (const QString &sourcePath : request.sourcePaths) {
    const QFileInfo sourceInfo(sourcePath);
    if (sourceInfo.isDir()) {
      const QDir sourceDirectory(sourceInfo.absoluteFilePath());
      const QString directoryLabel =
          uniqueDirectoryLabel(sourceDirectory.dirName());
      QDirIterator iterator(sourceInfo.absoluteFilePath(), QDir::Files,
                            QDirIterator::Subdirectories);
      while (iterator.hasNext()) {
        iterator.next();
        collectFile(
            iterator.fileInfo(),
            QDir(directoryLabel)
                .filePath(sourceDirectory.relativeFilePath(
                    iterator.fileInfo().absoluteFilePath())));
      }
    } else {
      collectFile(sourceInfo, sourceInfo.fileName());
    }
  }

  if (plan.mFiles.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("No supported image or video files were found");
    }
    return std::nullopt;
  }

  return plan;
}

QString DatasetImportPlan::sceneName() const { return mSceneName; }

qsizetype DatasetImportPlan::sourceCount() const { return mSourceCount; }

qsizetype DatasetImportPlan::imageCount() const { return mImageCount; }

qsizetype DatasetImportPlan::videoCount() const { return mVideoCount; }

qint64 DatasetImportPlan::totalBytes() const { return mTotalBytes; }

bool DatasetImportPlan::hasCameraInput() const { return !mCameraInput.isEmpty(); }

QJsonObject DatasetImportPlan::cameraInput() const { return mCameraInput; }

QString DatasetImportPlan::managedDatasetPath(const QString &datasetRoot) const {
  return QDir(datasetRoot).filePath(managedStorageName(mSceneName));
}

bool DatasetImportPlan::writeWorkerConfiguration(
    const QString &configPath, const QString &repositoryRoot,
    const QString &datasetRoot, QString *errorMessage) const {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }

  const QFileInfo configInfo(configPath);
  if (!QDir().mkpath(configInfo.absolutePath())) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "Could not create the worker configuration directory");
    }
    return false;
  }

  QJsonArray files;
  for (const MediaFile &file : mFiles) {
    files.append(QJsonObject{
        {QStringLiteral("path"), file.path},
        {QStringLiteral("name"), file.name},
        {QStringLiteral("type"), file.type},
        {QStringLiteral("relativePath"), file.relativePath},
        {QStringLiteral("size"), file.size},
        {QStringLiteral("lastModified"), file.lastModified},
    });
  }

  QJsonObject config{
      {QStringLiteral("task"), QStringLiteral("import")},
      {QStringLiteral("repositoryRoot"), absoluteCleanPath(repositoryRoot)},
      {QStringLiteral("projectRoot"),
       QDir::cleanPath(QFileInfo(datasetRoot).absolutePath())},
      {QStringLiteral("datasetRoot"), absoluteCleanPath(datasetRoot)},
      {QStringLiteral("scene"), managedStorageName(mSceneName)},
      {QStringLiteral("displayName"), mSceneName},
      {QStringLiteral("fps"), mFramesPerSecond},
      {QStringLiteral("overwrite"), mOverwrite},
      {QStringLiteral("files"), files},
  };
  if (!mCameraInput.isEmpty()) config.insert(QStringLiteral("cameraInput"), mCameraInput);
  const QByteArray data = QJsonDocument(config).toJson(QJsonDocument::Indented);

  QSaveFile output(configInfo.absoluteFilePath());
  if (!output.open(QIODevice::WriteOnly)) {
    if (errorMessage != nullptr) {
      *errorMessage = output.errorString();
    }
    return false;
  }
  if (output.write(data) != data.size()) {
    if (errorMessage != nullptr) {
      *errorMessage = output.errorString();
    }
    output.cancelWriting();
    return false;
  }
  if (!output.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = output.errorString();
    }
    return false;
  }
  return true;
}

} // namespace gsw
