#include "ViewportCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kMinimumGridStep = 0.001F;
constexpr float kMaximumGridStep = 1000000.0F;
constexpr float kAxisViewAlignmentThreshold = 0.9995F;

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
      current.rollDegrees,
  };
}

bool isTemporaryOrbitShortcut(const Qt::MouseButton button,
                              const Qt::KeyboardModifiers modifiers) {
  return button == Qt::LeftButton &&
         modifiers.testFlag(Qt::ControlModifier);
}

OrbitFrame orbitFrame(const OrbitAngles angles) {
  const float yaw = radians(angles.yawDegrees);
  const float pitch = radians(angles.pitchDegrees);
  const float sinYaw = std::sin(yaw);
  const float cosYaw = std::cos(yaw);
  const float sinPitch = std::sin(pitch);
  const float cosPitch = std::cos(pitch);

  const QVector3D offset(cosPitch * sinYaw, cosPitch * cosYaw, sinPitch);
  const QVector3D up(-sinPitch * sinYaw, -sinPitch * cosYaw, cosPitch);
  return {offset, QQuaternion::fromAxisAndAngle(offset, angles.rollDegrees).rotatedVector(up)};
}

OrbitAngles orbitAnglesAfterRotation(const OrbitAngles current,
                                      const QQuaternion &rotation) {
  if (!std::isfinite(rotation.scalar()) || !std::isfinite(rotation.x()) ||
      !std::isfinite(rotation.y()) || !std::isfinite(rotation.z()) ||
      rotation.lengthSquared() < 1.0e-12F) return current;
  const auto before = orbitFrame(current);
  const auto q = rotation.normalized();
  const QVector3D offset = q.rotatedVector(before.cameraOffsetDirection).normalized();
  const QVector3D up = q.rotatedVector(before.upDirection).normalized();
  OrbitAngles result;
  result.yawDegrees = offset.x() * offset.x() + offset.y() * offset.y() < 1.0e-10F
      ? current.yawDegrees : std::atan2(offset.x(), offset.y()) * 180.0F / kPi;
  result.pitchDegrees = std::asin(std::clamp(offset.z(), -1.0F, 1.0F)) * 180.0F / kPi;
  const QVector3D baseUp = orbitFrame(result).upDirection;
  result.rollDegrees = std::atan2(QVector3D::dotProduct(offset, QVector3D::crossProduct(baseUp, up)),
                                  QVector3D::dotProduct(baseUp, up)) * 180.0F / kPi;
  return result;
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
  const float dominantAxis = std::max({absoluteX, absoluteY, absoluteZ});

  // Toggling projection preserves an arbitrary orbit angle. Keep that view on
  // the world ground plane; only axis-snapped orthographic views use a
  // screen-parallel principal plane.
  if (dominantAxis < kAxisViewAlignmentThreshold) {
    return ReferenceGridPlane::XY;
  }

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

std::optional<ViewportCameraFrame>
viewportFrameForPoints(const std::span<const QVector3D> points,
                       const OrbitAngles angles, const float aspectRatio,
                       const bool orthographic, const float margin) {
  if (points.empty() || !std::isfinite(aspectRatio) || aspectRatio <= 0.0F ||
      !std::isfinite(margin) || margin < 1.0F) {
    return std::nullopt;
  }

  QVector3D minimum(std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max());
  QVector3D maximum(std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest());
  for (const QVector3D &point : points) {
    if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
        !std::isfinite(point.z())) {
      return std::nullopt;
    }
    minimum.setX(std::min(minimum.x(), point.x()));
    minimum.setY(std::min(minimum.y(), point.y()));
    minimum.setZ(std::min(minimum.z(), point.z()));
    maximum.setX(std::max(maximum.x(), point.x()));
    maximum.setY(std::max(maximum.y(), point.y()));
    maximum.setZ(std::max(maximum.z(), point.z()));
  }

  const QVector3D target = (minimum + maximum) * 0.5F;
  float radius = 0.0F;
  for (const QVector3D &point : points) {
    radius = std::max(radius, (point - target).length());
  }
  const float safeRadius = std::max(radius, 1.0e-4F);
  // Capping an extremely wide viewport is conservative. Do not raise a very
  // narrow aspect ratio: doing so would underestimate its horizontal fit.
  const float safeAspect = std::min(aspectRatio, 20.0F);
  const float safeMargin = std::clamp(margin, 1.0F, 2.0F);
  const float verticalTangent = std::tan(radians(23.0F));
  const float horizontalTangent = verticalTangent * safeAspect;

  const OrbitFrame frame = orbitFrame(angles);
  const QVector3D forward = -frame.cameraOffsetDirection;
  QVector3D right =
      QVector3D::crossProduct(forward, frame.upDirection);
  if (right.lengthSquared() <= 1.0e-12F) {
    return std::nullopt;
  }
  right.normalize();

  const float nearPadding = std::max(safeRadius * 0.02F, 1.0e-4F);
  float distance = nearPadding;
  for (const QVector3D &point : points) {
    const QVector3D offset = point - target;
    const float horizontal =
        std::abs(QVector3D::dotProduct(offset, right));
    const float vertical =
        std::abs(QVector3D::dotProduct(offset, frame.upDirection));
    const float forwardOffset = QVector3D::dotProduct(offset, forward);

    const float horizontalFit =
        horizontal * safeMargin / horizontalTangent;
    const float verticalFit = vertical * safeMargin / verticalTangent;
    distance = std::max(distance, -forwardOffset + nearPadding);
    if (orthographic) {
      distance = std::max({distance, horizontalFit, verticalFit});
    } else {
      distance = std::max(
          {distance, horizontalFit - forwardOffset,
           verticalFit - forwardOffset});
    }
  }

  if (!std::isfinite(distance)) {
    return std::nullopt;
  }
  return ViewportCameraFrame{target, std::max(distance, nearPadding),
                             safeRadius};
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

ReferenceGridDrawSpans
referenceGridDrawSpans(const ReferenceGridScale &scale) {
  const float fineHalfSpan = scale.displayMajorStep * 0.1F * 80.0F;
  const float majorHalfSpan = scale.displayMajorStep * 40.0F;
  return {fineHalfSpan, majorHalfSpan, majorHalfSpan};
}

float pointPreviewDiameterPixels(const float devicePixelRatio) {
  constexpr float kMetashapeStyleLogicalPointDiameter = 1.0F;
  const float safeRatio =
      std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0F
          ? devicePixelRatio
          : 1.0F;
  return kMetashapeStyleLogicalPointDiameter * safeRatio;
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
