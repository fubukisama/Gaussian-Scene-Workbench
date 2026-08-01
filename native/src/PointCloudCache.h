#pragma once

#include <QString>
#include <QVector>
#include <QVector3D>

#include <memory>

namespace gsw {

struct PointPreviewVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  quint8 red = 184;
  quint8 green = 191;
  quint8 blue = 199;
  quint8 padding = 255;
};

static_assert(sizeof(PointPreviewVertex) == 16);

struct PointCloudCacheNode {
  int id = -1;
  int depth = 0;
  int parent = -1;
  QVector<int> children;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  qint64 dataOffset = 0;
  qint64 pointCount = 0;
  qint64 sourcePointCount = 0;

  [[nodiscard]] bool isLeaf() const { return children.isEmpty(); }
  [[nodiscard]] bool isValid() const {
    return id >= 0 && dataOffset >= 0 && pointCount > 0 &&
           sourcePointCount > 0;
  }
};

struct PointCloudCacheIndex {
  static constexpr int CurrentFormatVersion = 1;
  static constexpr int OctreeDepth = 3;

  QString indexPath;
  QString dataPath;
  QString sourcePath;
  qint64 sourceSize = 0;
  qint64 sourceModifiedMilliseconds = 0;
  qint64 fullPointCount = 0;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  QVector<PointCloudCacheNode> nodes;
  int rootNode = 0;
  int formatVersion = 0;

  [[nodiscard]] bool isValid() const;
};

struct PointCloudCachePage {
  int nodeId = -1;
  QVector<PointPreviewVertex> vertices;
  QString error;

  [[nodiscard]] bool isValid() const {
    return nodeId >= 0 && !vertices.isEmpty() && error.isEmpty();
  }
};

class PointCloudCache final {
public:
  [[nodiscard]] static PointCloudCacheIndex loadForSource(
      const QString &sourcePath, QString *errorMessage = nullptr);
  [[nodiscard]] static PointCloudCachePage readNode(
      const PointCloudCacheIndex &index, int nodeId);
  [[nodiscard]] static QString cacheDirectoryForSource(
      const QString &sourcePath);
  [[nodiscard]] static QString indexPathForSource(const QString &sourcePath);
};

class PointCloudCacheBuilder final {
public:
  PointCloudCacheBuilder(const QString &sourcePath,
                         const QVector3D &boundsMinimum,
                         const QVector3D &boundsMaximum);
  ~PointCloudCacheBuilder();

  PointCloudCacheBuilder(const PointCloudCacheBuilder &) = delete;
  PointCloudCacheBuilder &operator=(const PointCloudCacheBuilder &) = delete;

  [[nodiscard]] bool begin(QString *errorMessage = nullptr);
  [[nodiscard]] bool append(const PointPreviewVertex &vertex,
                            qint64 sourceIndex,
                            QString *errorMessage = nullptr);
  [[nodiscard]] PointCloudCacheIndex finish(QString *errorMessage = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace gsw
