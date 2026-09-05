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

// Distance-responsive decoration anchored to the real reference origin, with
// soft screen-size limits for legibility. referenceLength is in scene units
// and must not depend on camera distance or the adaptive grid step. Positions
// are NDC, including the original scene depth; render with an identity matrix
// and depth writes so opaque geometry and Gaussian compositing can occlude it.
[[nodiscard]] QVector<ReferenceAxisVertex>
referenceAxisGeometry(const QMatrix4x4 &viewProjection,
                      const QVector3D &origin, const QSizeF &viewport,
                      float uiScale = 1.0F, float referenceLength = 1.2F);

} // namespace gsw
