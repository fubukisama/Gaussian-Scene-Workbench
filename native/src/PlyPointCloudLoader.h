#pragma once

#include "MeshCache.h"
#include "PointCloudCache.h"
#include "SceneCoordinates.h"

#include <QBitArray>
#include <QImage>
#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <functional>

namespace gsw {

struct ModelExportOptions;
struct ModelExportResult;

struct PlySourceGeometry {
  qint64 vertexCount = 0;
  qint64 faceCount = 0;
  bool gaussian = false;
  bool normals = false;
  bool colors = false;
  bool textureCoordinates = false;
  QString texturePath;
};

struct PlySourceVertex {
  SceneCoordinate3D position;
  QVector3D normal;
  QVector3D color{0.72F, 0.75F, 0.78F};
};

struct PlyGeometryVisitor {
  std::function<bool(const PlySourceGeometry &)> begin;
  std::function<bool(qint64, const PlySourceVertex &)> vertex;
  std::function<bool(const QVector<quint32> &, const QVector<QVector2D> &)> face;
  std::function<bool(int)> cancelled;
};

struct PointCloudVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float red = 0.72F;
  float green = 0.75F;
  float blue = 0.78F;
  float opacity = 1.0F;
  float scaleX = 1.0F;
  float scaleY = 1.0F;
  float scaleZ = 1.0F;
  float rotationW = 1.0F;
  float rotationX = 0.0F;
  float rotationY = 0.0F;
  float rotationZ = 0.0F;
  quint32 sourceIndex = 0;
};

struct PointPosition {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;

  [[nodiscard]] QVector3D toVector3D() const { return QVector3D(x, y, z); }
};

struct PointCloudData {
  QVector<PointCloudVertex> vertices;
  QVector<PointPosition> sourcePositions;
  PointCloudCacheIndex pointCache;
  MeshCacheIndex meshCache;
  QVector<MeshVertex> meshVertices;
  QVector<quint32> meshIndices;
  QVector<QVector2D> meshCornerTextureCoordinates;
  QVector<quint8> meshCornerTextured;
  QImage meshTextureImage;
  QString meshTexturePath;
  QString meshTextureError;
  bool meshHasTextureCoordinates = false;
  SceneCoordinateInfo coordinates;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  qint64 sourceVertexCount = 0;
  qint64 sourceFaceCount = 0;
  qint64 sourceTriangleCount = 0;
  bool hasGaussianAttributes = false;
  bool meshPreviewDecimated = false;
  bool previewOnly = false;
  QString error;

  [[nodiscard]] bool isValid() const;
  [[nodiscard]] bool hasMesh() const;
  [[nodiscard]] qsizetype previewPointCount() const;
  [[nodiscard]] QVector3D center() const;
  [[nodiscard]] float radius() const;
};

class PlyPointCloudLoader final {
public:
  static constexpr qsizetype DefaultMaximumPreviewPoints = 1'500'000;
  static constexpr qint64 DefaultMaximumEditablePoints = 10'000'000;
  static constexpr qint64 DefaultMaximumResidentMeshVertices = 5'000'000;
  static constexpr qint64 DefaultMaximumResidentMeshFaces = 2'000'000;

  [[nodiscard]] static PointCloudData load(
      const QString &filePath,
      qsizetype maximumPreviewPoints = DefaultMaximumPreviewPoints,
      qint64 maximumEditablePoints = DefaultMaximumEditablePoints,
      qint64 maximumResidentMeshVertices =
          DefaultMaximumResidentMeshVertices,
      qint64 maximumResidentMeshFaces = DefaultMaximumResidentMeshFaces);

  [[nodiscard]] static bool writeFiltered(
      const QString &sourceFilePath, const QString &destinationFilePath,
      const QBitArray &deletedVertices, QString *errorMessage = nullptr);

  // Full source records, never the resident/LOD preview. Visitor false aborts.
  [[nodiscard]] static bool visitSourceGeometry(const QString &sourcePath,
      const PlyGeometryVisitor &visitor, QString &error);
  [[nodiscard]] static ModelExportResult exportSourcePly(
      const ModelExportOptions &options);
};

} // namespace gsw
