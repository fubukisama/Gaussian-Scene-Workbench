#pragma once

#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>

#include <memory>

namespace gsw {

struct MeshVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float red = 0.72F;
  float green = 0.75F;
  float blue = 0.78F;
  float normalX = 0.0F;
  float normalY = 0.0F;
  float normalZ = 1.0F;
  float textureU = 0.0F;
  float textureV = 0.0F;
  float textureWeight = 0.0F;
};

static_assert(sizeof(MeshVertex) == 48);

struct MeshCacheNode {
  int id = -1;
  int depth = 0;
  int parent = -1;
  QVector<int> children;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  qint64 dataOffset = 0;
  qint64 vertexCount = 0;
  qint64 indexCount = 0;
  qint64 sourceTriangleCount = 0;

  [[nodiscard]] bool isLeaf() const { return children.isEmpty(); }
  [[nodiscard]] bool isValid() const {
    return id >= 0 && dataOffset >= 0 && vertexCount > 0 &&
           indexCount >= 3 && indexCount % 3 == 0 &&
           sourceTriangleCount > 0;
  }
  [[nodiscard]] qint64 byteCount() const {
    return vertexCount * static_cast<qint64>(sizeof(MeshVertex)) +
           indexCount * static_cast<qint64>(sizeof(quint32));
  }
};

struct MeshCacheIndex {
  static constexpr int CurrentFormatVersion = 2;
  static constexpr int SpatialOctreeDepth = 4;

  QString indexPath;
  QString dataPath;
  QString sourcePath;
  qint64 sourceSize = 0;
  qint64 sourceModifiedMilliseconds = 0;
  qint64 fullVertexCount = 0;
  qint64 fullFaceCount = 0;
  qint64 fullTriangleCount = 0;
  qint64 renderableTriangleCount = 0;
  bool hasTextureCoordinates = false;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  QVector<MeshCacheNode> nodes;
  int rootNode = 0;
  int formatVersion = 0;

  [[nodiscard]] bool isValid() const;
};

struct MeshCachePage {
  int nodeId = -1;
  QVector<MeshVertex> vertices;
  QVector<quint32> indices;
  QString error;

  [[nodiscard]] bool isValid() const {
    return nodeId >= 0 && !vertices.isEmpty() && indices.size() >= 3 &&
           indices.size() % 3 == 0 && error.isEmpty();
  }
};

class MeshCache final {
public:
  [[nodiscard]] static MeshCacheIndex loadForSource(
      const QString &sourcePath, QString *errorMessage = nullptr);
  [[nodiscard]] static MeshCachePage readNode(
      const MeshCacheIndex &index, int nodeId);
  [[nodiscard]] static QString cacheDirectoryForSource(
      const QString &sourcePath);
  [[nodiscard]] static QString indexPathForSource(const QString &sourcePath);
};

class MeshCacheBuilder final {
public:
  MeshCacheBuilder(const QString &sourcePath, qint64 sourceVertexCount);
  ~MeshCacheBuilder();

  MeshCacheBuilder(const MeshCacheBuilder &) = delete;
  MeshCacheBuilder &operator=(const MeshCacheBuilder &) = delete;

  [[nodiscard]] bool begin(QString *errorMessage = nullptr);
  [[nodiscard]] bool appendVertex(const MeshVertex &vertex,
                                  QString *errorMessage = nullptr);
  [[nodiscard]] bool finishVertices(QString *errorMessage = nullptr);
  [[nodiscard]] bool appendTriangle(quint32 a, quint32 b, quint32 c,
                                    qint64 sourceTriangleIndex,
                                    QString *errorMessage = nullptr);
  [[nodiscard]] bool appendTexturedTriangle(
      quint32 a, quint32 b, quint32 c, qint64 sourceTriangleIndex,
      const QVector2D &textureA, const QVector2D &textureB,
      const QVector2D &textureC, QString *errorMessage = nullptr);
  [[nodiscard]] MeshCacheIndex finish(qint64 sourceFaceCount,
                                      qint64 sourceTriangleCount,
                                      QString *errorMessage = nullptr);

  [[nodiscard]] QVector3D boundsMinimum() const;
  [[nodiscard]] QVector3D boundsMaximum() const;

private:
  [[nodiscard]] bool appendTriangleInternal(
      quint32 a, quint32 b, quint32 c, qint64 sourceTriangleIndex,
      const QVector2D &textureA, const QVector2D &textureB,
      const QVector2D &textureC, bool textured,
      QString *errorMessage = nullptr);

  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace gsw
