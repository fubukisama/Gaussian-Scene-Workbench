#pragma once

#include "SceneObject.h"
#include <QJsonObject>

#include <QByteArray>
#include <QObject>
#include <QQuaternion>
#include <QString>
#include <QStringList>
#include <QVector3D>

namespace gsw {

struct PlyMetadata {
  bool valid = false;
  QString format;
  qint64 vertexCount = 0;
  qint64 faceCount = 0;
  qint64 fileSize = 0;
  QStringList properties;

  [[nodiscard]] bool looksLikeGaussianSplat() const;
  [[nodiscard]] bool looksLikeMesh() const { return faceCount > 0; }
};

struct ImportCleanupOptions {
  bool clearDataset = true;
  bool clearScene = false;
};

struct ImportCleanupResult {
  bool datasetCleared = false;
  bool sceneCleared = false;
  bool managedDatasetRemoved = false;
  QString previousDatasetPath;
  QString previousScenePath;
  QString cleanupPendingPath;
};

struct ReconstructionCleanupResult {
  QStringList removedPaths;
  QString cleanupPendingPath;
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
  [[nodiscard]] QVector3D sceneTranslation() const;
  [[nodiscard]] QQuaternion sceneRotation() const;
  [[nodiscard]] QVector3D sceneScale() const;
  [[nodiscard]] qint64 imageCount() const;
  [[nodiscard]] PlyMetadata sceneMetadata() const;
  [[nodiscard]] QList<SceneObject> sceneObjects() const;
  [[nodiscard]] QString activeSceneId() const { return mActiveSceneId; }
  [[nodiscard]] QJsonObject sceneCollectionJson(const QString &rootPath = {}) const;
  void restoreSceneCollection(const QJsonObject &state);
  bool addScenePath(const QString &path, QString *errorMessage = nullptr);
  bool activateSceneObject(const QString &id);
  bool setSceneObjectTransforms(const QList<SceneObject> &objects,
                                QString *errorMessage = nullptr);
  [[nodiscard]] bool hasPendingDataMigration() const;
  [[nodiscard]] bool isDatasetManaged() const;
  [[nodiscard]] bool hasManagedReconstructionData() const;

  bool create(const QString &rootPath, QString *errorMessage = nullptr);
  bool createUntitled(const QString &workingRoot,
                      const QString &displayName = {},
                      QString *errorMessage = nullptr);
  bool load(const QString &filePath, QString *errorMessage = nullptr);
  bool save(const QString &filePath = {}, QString *errorMessage = nullptr);
  bool saveManifest(const QString &filePath = {},
                    QString *errorMessage = nullptr);
  bool finalizeDataMigration(QString *errorMessage = nullptr);
  [[nodiscard]] QByteArray recoveryManifestJson() const;
  bool setDatasetPath(const QString &path, QString *errorMessage = nullptr);
  bool setScenePath(const QString &path, QString *errorMessage = nullptr);
  bool setSceneTranslation(const QVector3D &translation,
                           QString *errorMessage = nullptr);
  bool setSceneTransform(const QVector3D &translation,
                         const QQuaternion &rotation,
                         QString *errorMessage = nullptr);
  bool setSceneTransform(const QVector3D &translation,
                         const QQuaternion &rotation,
                         const QVector3D &scale,
                         QString *errorMessage = nullptr);
  bool clearImportedData(const ImportCleanupOptions &options,
                         ImportCleanupResult *result = nullptr,
                         QString *errorMessage = nullptr);
  bool clearReconstructionData(ReconstructionCleanupResult *result = nullptr,
                               QString *errorMessage = nullptr);

  static PlyMetadata inspectPly(const QString &filePath,
                                QString *errorMessage = nullptr);
  static qint64 countDatasetImages(const QString &directoryPath);
  static QString projectDataRootForFile(const QString &projectFilePath);

signals:
  void changed();
  void modifiedChanged(bool modified);

private:
  void setModified(bool modified);
  QString resolvePortablePath(const QString &storedPath) const;
  void storeActiveScene();

  QString mProjectName;
  QString mRootPath;
  QString mProjectFilePath;
  QString mDatasetPath;
  QString mScenePath;
  QList<SceneObject> mSceneObjects;
  QString mActiveSceneId;
  QVector3D mSceneTranslation;
  QQuaternion mSceneRotation;
  QVector3D mSceneScale = QVector3D(1.0F, 1.0F, 1.0F);
  QString mPendingDataRoot;
  qint64 mImageCount = 0;
  PlyMetadata mSceneMetadata;
  bool mModified = false;
};

} // namespace gsw
