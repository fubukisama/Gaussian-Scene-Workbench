#pragma once

#include <QPoint>
#include <QVector3D>

namespace gsw {

struct OrbitAngles {
  float yawDegrees = 0.0F;
  float pitchDegrees = 0.0F;

  [[nodiscard]] bool operator==(const OrbitAngles &) const = default;
};

struct OrbitFrame {
  QVector3D cameraOffsetDirection;
  QVector3D upDirection;
};

[[nodiscard]] OrbitAngles orbitAnglesAfterLeftDrag(OrbitAngles current,
                                                   const QPoint &delta);

[[nodiscard]] OrbitFrame orbitFrame(OrbitAngles angles);

} // namespace gsw
