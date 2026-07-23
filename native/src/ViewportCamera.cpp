#include "ViewportCamera.h"

#include <algorithm>

namespace gsw {

OrbitAngles orbitAnglesAfterLeftDrag(const OrbitAngles current,
                                     const QPoint &delta) {
  constexpr float kYawDegreesPerPixel = 0.32F;
  constexpr float kPitchDegreesPerPixel = 0.28F;
  constexpr float kMinimumPitchDegrees = -86.0F;
  constexpr float kMaximumPitchDegrees = 86.0F;

  return {
      current.yawDegrees -
          static_cast<float>(delta.x()) * kYawDegreesPerPixel,
      std::clamp(current.pitchDegrees -
                     static_cast<float>(delta.y()) * kPitchDegreesPerPixel,
                 kMinimumPitchDegrees, kMaximumPitchDegrees),
  };
}

} // namespace gsw
