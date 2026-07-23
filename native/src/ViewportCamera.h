#pragma once

#include <QPoint>

namespace gsw {

struct OrbitAngles {
  float yawDegrees = 0.0F;
  float pitchDegrees = 0.0F;

  [[nodiscard]] bool operator==(const OrbitAngles &) const = default;
};

[[nodiscard]] OrbitAngles orbitAnglesAfterLeftDrag(OrbitAngles current,
                                                   const QPoint &delta);

} // namespace gsw
