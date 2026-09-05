#pragma once

#include <QMatrix4x4>
#include <QSizeF>
#include <QVector>
#include <QVector3D>

namespace gsw {

struct ReferenceAxisVertex {
  float x, y, z;
  float red, green, blue, alpha;
};

// Screen-sized decoration anchored to the real reference origin. Positions
// are NDC, including the original scene depth; render with an identity matrix
// and depth writes so opaque geometry and Gaussian compositing can occlude it.
[[nodiscard]] QVector<ReferenceAxisVertex>
referenceAxisGeometry(const QMatrix4x4 &viewProjection,
                      const QVector3D &origin, const QSizeF &viewport,
                      float uiScale = 1.0F);

} // namespace gsw
