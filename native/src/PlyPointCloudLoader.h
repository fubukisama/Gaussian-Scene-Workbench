#pragma once

#include <QString>
#include <QVector>
#include <QVector3D>

namespace gsw {

struct PointCloudVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float red = 0.72F;
  float green = 0.75F;
  float blue = 0.78F;
};

struct PointCloudData {
  QVector<PointCloudVertex> vertices;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  qint64 sourceVertexCount = 0;
  QString error;

  [[nodiscard]] bool isValid() const;
  [[nodiscard]] QVector3D center() const;
  [[nodiscard]] float radius() const;
};

class PlyPointCloudLoader final {
public:
  static constexpr qsizetype DefaultMaximumPreviewPoints = 1'500'000;

  [[nodiscard]] static PointCloudData load(
      const QString &filePath,
      qsizetype maximumPreviewPoints = DefaultMaximumPreviewPoints);
};

} // namespace gsw
