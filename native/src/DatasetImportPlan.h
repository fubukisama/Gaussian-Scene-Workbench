#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gsw {

struct DatasetImportRequest {
  QString sceneName;
  QStringList sourcePaths;
  double framesPerSecond = 1.0;
  bool overwrite = false;
};

class DatasetImportPlan final {
public:
  [[nodiscard]] static bool supportsFile(const QString &path);
  [[nodiscard]] static std::optional<DatasetImportPlan>
  create(const DatasetImportRequest &request, QString *errorMessage = nullptr);

  [[nodiscard]] QString sceneName() const;
  [[nodiscard]] qsizetype sourceCount() const;
  [[nodiscard]] qsizetype imageCount() const;
  [[nodiscard]] qsizetype videoCount() const;
  [[nodiscard]] qint64 totalBytes() const;
  [[nodiscard]] QString managedDatasetPath(const QString &datasetRoot) const;
  bool writeWorkerConfiguration(const QString &configPath,
                                const QString &repositoryRoot,
                                const QString &datasetRoot,
                                QString *errorMessage = nullptr) const;

private:
  struct MediaFile {
    QString path;
    QString name;
    QString type;
    QString relativePath;
    qint64 size = 0;
    qint64 lastModified = 0;
  };

  QString mSceneName;
  QList<MediaFile> mFiles;
  double mFramesPerSecond = 1.0;
  bool mOverwrite = false;
  qsizetype mSourceCount = 0;
  qsizetype mImageCount = 0;
  qsizetype mVideoCount = 0;
  qint64 mTotalBytes = 0;
};

} // namespace gsw
