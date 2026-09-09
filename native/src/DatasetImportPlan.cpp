#include <QCoreApplication>
#include "DatasetImportPlan.h"

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

bool isSafeSceneName(const QString &name) {
  static const QRegularExpression pattern(
      QStringLiteral("^[A-Za-z0-9_.-]+$"));
  static const QSet<QString> reservedWindowsNames = {
      QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
      QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
      QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
      QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
      QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
      QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
      QStringLiteral("LPT9"),
  };

  if (name.isEmpty() || name.size() > 120 || name.startsWith(QLatin1Char('.')) ||
      name.endsWith(QLatin1Char('.')) || name.contains(QStringLiteral("..")) ||
      !pattern.match(name).hasMatch()) {
    return false;
  }
  const QString deviceBaseName = name.section(QLatin1Char('.'), 0, 0).toUpper();
  return !reservedWindowsNames.contains(deviceBaseName);
}

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

std::optional<DatasetImportPlan>
DatasetImportPlan::create(const DatasetImportRequest &request,
                          QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }

  if (!isSafeSceneName(request.sceneName)) {
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

QString DatasetImportPlan::managedDatasetPath(const QString &datasetRoot) const {
  return QDir(datasetRoot).filePath(mSceneName);
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

  const QJsonObject config{
      {QStringLiteral("task"), QStringLiteral("import")},
      {QStringLiteral("repositoryRoot"), absoluteCleanPath(repositoryRoot)},
      {QStringLiteral("projectRoot"),
       QDir::cleanPath(QFileInfo(datasetRoot).absolutePath())},
      {QStringLiteral("datasetRoot"), absoluteCleanPath(datasetRoot)},
      {QStringLiteral("scene"), mSceneName},
      {QStringLiteral("fps"), mFramesPerSecond},
      {QStringLiteral("overwrite"), mOverwrite},
      {QStringLiteral("files"), files},
  };
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
