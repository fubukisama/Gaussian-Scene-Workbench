#include "ViewportCamera.h"

#include <cmath>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kMinimumGridStep = 0.001F;
constexpr float kMaximumGridStep = 1000000.0F;

float radians(const float degrees) { return degrees * kPi / 180.0F; }

float wrapDegrees(const float degrees) {
  float wrapped = std::fmod(degrees + 180.0F, 360.0F);
  if (wrapped < 0.0F) {
    wrapped += 360.0F;
  }
  return wrapped - 180.0F;
}

float nextDecimalGridStep(const float step) {
  const float exponent = std::floor(std::log10(step));
  const float decade = std::pow(10.0F, exponent);
  const float normalized = step / decade;
  if (normalized < 1.999F) {
    return 2.0F * decade;
  }
  if (normalized < 4.999F) {
    return 5.0F * decade;
  }
  return 10.0F * decade;
}

float lowerDecimalGridStep(const float desiredStep) {
  const float exponent = std::floor(std::log10(desiredStep));
  const float decade = std::pow(10.0F, exponent);
  const float normalized = desiredStep / decade;
  if (normalized < 2.0F) {
    return decade;
  }
  if (normalized < 5.0F) {
    return 2.0F * decade;
  }
  return 5.0F * decade;
}
} // namespace

OrbitAngles orbitAnglesAfterLeftDrag(const OrbitAngles current,
                                     const QPoint &delta) {
  constexpr float kYawDegreesPerPixel = 0.32F;
  constexpr float kPitchDegreesPerPixel = 0.28F;

  return {
      wrapDegrees(current.yawDegrees +
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
      QVector3D(cosPitch * sinYaw, cosPitch * cosYaw, sinPitch),
      QVector3D(-sinPitch * sinYaw, -sinPitch * cosYaw, cosPitch),
  };
}

ReferenceGridPlane referenceGridPlane(const OrbitAngles angles,
                                      const bool orthographic) {
  if (!orthographic) {
    return ReferenceGridPlane::XY;
  }

  const QVector3D viewNormal = orbitFrame(angles).cameraOffsetDirection;
  const float absoluteX = std::abs(viewNormal.x());
  const float absoluteY = std::abs(viewNormal.y());
  const float absoluteZ = std::abs(viewNormal.z());
  if (absoluteX >= absoluteY && absoluteX >= absoluteZ) {
    return ReferenceGridPlane::YZ;
  }
  if (absoluteY >= absoluteZ) {
    return ReferenceGridPlane::XZ;
  }
  return ReferenceGridPlane::XY;
}

ViewportZoomLimits viewportZoomLimits(const float sceneRadius) {
  const float safeRadius =
      std::max(std::isfinite(sceneRadius) ? std::abs(sceneRadius) : 1.0F,
               0.001F);
  const float minimumDistance =
      std::clamp(safeRadius * 0.001F, 0.00001F, 10.0F);
  const float maximumDistance =
      std::clamp(safeRadius * 250.0F, 100.0F, 10000000.0F);
  return {minimumDistance, std::max(maximumDistance, minimumDistance * 10.0F)};
}

float clampViewportDistance(const float distance, const float sceneRadius) {
  const ViewportZoomLimits limits = viewportZoomLimits(sceneRadius);
  if (std::isinf(distance)) {
    return distance > 0.0F ? limits.maximumDistance : limits.minimumDistance;
  }
  if (!std::isfinite(distance)) {
    return limits.minimumDistance;
  }
  return std::clamp(distance, limits.minimumDistance, limits.maximumDistance);
}

ReferenceGridScale referenceGridScale(const float cameraDistance,
                                      const int viewportPixelHeight) {
  const float safeDistance =
      std::max(std::isfinite(cameraDistance) ? std::abs(cameraDistance) : 1.0F,
               0.000001F);
  const float safeHeight = static_cast<float>(std::max(viewportPixelHeight, 1));
  constexpr float kVerticalHalfFovRadians = 23.0F * kPi / 180.0F;
  constexpr float kTargetMinorSpacingPixels = 36.0F;
  const float worldPerPixel =
      2.0F * safeDistance * std::tan(kVerticalHalfFovRadians) / safeHeight;
  const float desiredStep = std::clamp(
      worldPerPixel * kTargetMinorSpacingPixels, kMinimumGridStep,
      kMaximumGridStep);

  if (desiredStep <= kMinimumGridStep) {
    return {kMinimumGridStep, kMinimumGridStep, kMinimumGridStep, 0.0F,
            kMinimumGridStep * 10.0F,
            std::max(safeDistance * 8.0F, kMinimumGridStep * 40.0F)};
  }

  const float lower =
      std::clamp(lowerDecimalGridStep(desiredStep), kMinimumGridStep,
                 kMaximumGridStep);
  const float upper =
      std::clamp(nextDecimalGridStep(lower), lower, kMaximumGridStep);
  const float blend = upper > lower
                          ? std::clamp(std::log(desiredStep / lower) /
                                           std::log(upper / lower),
                                       0.0F, 1.0F)
                          : 0.0F;
  const float displayMinor = blend >= 0.5F ? upper : lower;
  return {kMinimumGridStep,
          lower,
          upper,
          blend,
          displayMinor * 10.0F,
          std::max(safeDistance * 8.0F, upper * 40.0F)};
}

QString formatMetricDistance(const float metres) {
  if (!std::isfinite(metres)) {
    return QStringLiteral("—");
  }

  const float absoluteMetres = std::abs(metres);
  float factor = 1.0F;
  QString suffix = QStringLiteral("m");
  if (absoluteMetres < 0.01F) {
    factor = 1000.0F;
    suffix = QStringLiteral("mm");
  } else if (absoluteMetres < 1.0F) {
    factor = 100.0F;
    suffix = QStringLiteral("cm");
  }
  return QStringLiteral("%1 %2")
      .arg(QString::number(static_cast<double>(metres * factor), 'g', 4),
           suffix);
}

} // namespace gsw
