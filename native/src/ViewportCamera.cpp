#include "ViewportCamera.h"

#include <cmath>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;

float radians(const float degrees) { return degrees * kPi / 180.0F; }

float wrapDegrees(const float degrees) {
  float wrapped = std::fmod(degrees + 180.0F, 360.0F);
  if (wrapped < 0.0F) {
    wrapped += 360.0F;
  }
  return wrapped - 180.0F;
}
} // namespace

OrbitAngles orbitAnglesAfterLeftDrag(const OrbitAngles current,
                                     const QPoint &delta) {
  constexpr float kYawDegreesPerPixel = 0.32F;
  constexpr float kPitchDegreesPerPixel = 0.28F;

  return {
      wrapDegrees(current.yawDegrees -
                  static_cast<float>(delta.x()) * kYawDegreesPerPixel),
      wrapDegrees(current.pitchDegrees +
                  static_cast<float>(delta.y()) * kPitchDegreesPerPixel),
  };
}

OrbitFrame orbitFrame(const OrbitAngles angles) {
  const float yaw = radians(angles.yawDegrees);
  const float pitch = radians(angles.pitchDegrees);
  const float sinYaw = std::sin(yaw);
  const float cosYaw = std::cos(yaw);
  const float sinPitch = std::sin(pitch);
  const float cosPitch = std::cos(pitch);

  return {
      QVector3D(cosPitch * sinYaw, sinPitch, cosPitch * cosYaw),
      QVector3D(-sinPitch * sinYaw, cosPitch, -sinPitch * cosYaw),
  };
}

} // namespace gsw
