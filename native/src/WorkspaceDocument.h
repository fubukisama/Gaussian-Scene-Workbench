#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace gsw {

struct PlyMetadata {
  bool valid = false;
  QString format;
  qint64 vertexCount = 0;
  qint64 fileSize = 0;
  QStringList properties;

  [[nodiscard]] bool looksLikeGaussianSplat() const;
};

class WorkspaceDocument final : public QObject {
  Q_OBJECT

public:
  explicit WorkspaceDocument(QObject *parent = nullptr);

  [[nodiscard]] bool hasProject() const;
  [[nodiscard]] bool isUntitled() const;
  [[nodiscard]] bool isModified() const;
  [[nodiscard]] QString projectName() const;
  [[nodiscard]] QString rootPath() const;
  [[nodiscard]] QString projectFilePath() const;
  [[nodiscard]] QString datasetPath() const;
  [[nodiscard]] QString scenePath() const;
  [[nodiscard]] qint64 imageCount() const;
  [[nodiscard]] PlyMetadata sceneMetadata() const;

  bool create(const QString &rootPath, QString *errorMessage = nullptr);
  bool createUntitled(const QString &workingRoot,
                      const QString &displayName = {},
                      QString *errorMessage = nullptr);
  bool load(const QString &filePath, QString *errorMessage = nullptr);
  bool save(const QString &filePath = {}, QString *errorMessage = nullptr);
  bool setDatasetPath(const QString &path, QString *errorMessage = nullptr);
  bool setScenePath(const QString &path, QString *errorMessage = nullptr);

  static PlyMetadata inspectPly(const QString &filePath, QString *errorMessage = nullptr);
  static qint64 countDatasetImages(const QString &directoryPath);
  static QString projectDataRootForFile(const QString &projectFilePath);

signals:
  void changed();
  void modifiedChanged(bool modified);

private:
  void setModified(bool modified);
  QString resolvePortablePath(const QString &storedPath) const;

  QString mProjectName;
  QString mRootPath;
  QString mProjectFilePath;
  QString mDatasetPath;
  QString mScenePath;
  qint64 mImageCount = 0;
  PlyMetadata mSceneMetadata;
  bool mModified = false;
};

} // namespace gsw
