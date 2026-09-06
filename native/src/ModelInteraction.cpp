#include "ModelInteraction.h"

#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gsw {
namespace {

bool finiteVector(const QVector3D &value) {
  return std::isfinite(value.x()) && std::isfinite(value.y()) &&
         std::isfinite(value.z());
}

bool finiteQuaternion(const QQuaternion &value) {
  return std::isfinite(value.scalar()) && std::isfinite(value.x()) &&
         std::isfinite(value.y()) && std::isfinite(value.z());
}

QVector3D arcballVector(const QPointF &position, const QPointF &center,
                        const float radius) {
  QVector3D value(static_cast<float>((position.x() - center.x()) / radius),
                  static_cast<float>((center.y() - position.y()) / radius),
                  0.0F);
  const float planarLengthSquared =
      value.x() * value.x() + value.y() * value.y();
  if (planarLengthSquared <= 1.0F) {
    value.setZ(std::sqrt(std::max(0.0F, 1.0F - planarLengthSquared)));
  } else {
    value.normalize();
  }
  return value;
}

} // namespace

bool WorldRay::isValid() const {
  return finiteVector(origin) && finiteVector(direction) &&
         direction.lengthSquared() > 1.0e-12F;
}

bool ModelTransform::isValid() const {
  return finiteVector(translation) && finiteQuaternion(rotation) &&
         rotation.lengthSquared() > 1.0e-12F && finiteVector(scale) &&
         std::abs(scale.x()) >= 1.0e-4F &&
         std::abs(scale.y()) >= 1.0e-4F &&
         std::abs(scale.z()) >= 1.0e-4F;
}

QMatrix4x4 ModelTransform::matrix(const QVector3D &pivot) const {
  QMatrix4x4 result;
  if (!isValid() || !finiteVector(pivot)) {
    return result;
  }
  result.translate(pivot + translation);
  result.rotate(normalizedModelRotation(rotation));
  result.scale(normalizedModelScale(scale));
  result.translate(-pivot);
  return result;
}

QQuaternion normalizedModelRotation(const QQuaternion &rotation) {
  if (!finiteQuaternion(rotation) || rotation.lengthSquared() <= 1.0e-12F) {
    return QQuaternion();
  }
  QQuaternion normalized = rotation.normalized();
  // q and -q encode the same orientation. Keep one canonical sign so JSON,
  // undo history, and equality checks remain stable across long sessions.
  if (normalized.scalar() < 0.0F) {
    normalized = QQuaternion(-normalized.scalar(), -normalized.x(),
                             -normalized.y(), -normalized.z());
  }
  return normalized;
}

bool rotationsEquivalent(const QQuaternion &left, const QQuaternion &right,
                         const float tolerance) {
  const QQuaternion a = normalizedModelRotation(left);
  const QQuaternion b = normalizedModelRotation(right);
  const float dot = std::abs(QQuaternion::dotProduct(a, b));
  return std::isfinite(dot) &&
         1.0F - std::clamp(dot, 0.0F, 1.0F) <= std::max(tolerance, 0.0F);
}

QVector3D normalizedModelScale(const QVector3D &scale) {
  if (!finiteVector(scale) || std::abs(scale.x()) < 1.0e-4F ||
      std::abs(scale.y()) < 1.0e-4F || std::abs(scale.z()) < 1.0e-4F) {
    return QVector3D(1.0F, 1.0F, 1.0F);
  }
  QVector3D normalized = scale;
  for (int component = 0; component < 3; ++component) {
    normalized[component] =
        std::clamp(normalized[component], -1.0e4F, 1.0e4F);
  }
  return normalized;
}

bool scalesEquivalent(const QVector3D &left, const QVector3D &right,
                      const float tolerance) {
  const QVector3D a = normalizedModelScale(left);
  const QVector3D b = normalizedModelScale(right);
  const float comparisonScale =
      std::max({1.0F, std::abs(a.x()), std::abs(a.y()), std::abs(a.z()),
                std::abs(b.x()), std::abs(b.y()), std::abs(b.z())});
  return (a - b).lengthSquared() <=
         comparisonScale * comparisonScale *
             std::max(tolerance, 0.0F) * std::max(tolerance, 0.0F);
}

std::optional<WorldRay>
screenRay(const QPointF &screenPosition, const QSize &viewportSize,
          const QMatrix4x4 &viewProjection) {
  if (viewportSize.width() <= 0 || viewportSize.height() <= 0 ||
      !std::isfinite(screenPosition.x()) ||
      !std::isfinite(screenPosition.y())) {
    return std::nullopt;
  }

  bool invertible = false;
  const QMatrix4x4 inverse = viewProjection.inverted(&invertible);
  if (!invertible) {
    return std::nullopt;
  }

  const float normalizedX =
      static_cast<float>(screenPosition.x() / viewportSize.width() * 2.0 - 1.0);
  const float normalizedY = static_cast<float>(
      1.0 - screenPosition.y() / viewportSize.height() * 2.0);
  QVector4D nearPoint = inverse * QVector4D(normalizedX, normalizedY, -1.0F,
                                            1.0F);
  QVector4D farPoint =
      inverse * QVector4D(normalizedX, normalizedY, 1.0F, 1.0F);
  if (std::abs(nearPoint.w()) <= 1.0e-8F ||
      std::abs(farPoint.w()) <= 1.0e-8F) {
    return std::nullopt;
  }
  nearPoint /= nearPoint.w();
  farPoint /= farPoint.w();
  WorldRay ray{nearPoint.toVector3D(),
               farPoint.toVector3D() - nearPoint.toVector3D()};
  if (!ray.isValid()) {
    return std::nullopt;
  }
  ray.direction.normalize();
  return ray;
}

std::optional<float> rayAabbDistance(const WorldRay &ray,
                                     const QVector3D &minimum,
                                     const QVector3D &maximum) {
  if (!ray.isValid() || !finiteVector(minimum) || !finiteVector(maximum) ||
      minimum.x() > maximum.x() || minimum.y() > maximum.y() ||
      minimum.z() > maximum.z()) {
    return std::nullopt;
  }

  float entry = 0.0F;
  float exit = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    const float origin = ray.origin[axis];
    const float direction = ray.direction[axis];
    if (std::abs(direction) <= 1.0e-8F) {
      if (origin < minimum[axis] || origin > maximum[axis]) {
        return std::nullopt;
      }
      continue;
    }
    float nearDistance = (minimum[axis] - origin) / direction;
    float farDistance = (maximum[axis] - origin) / direction;
    if (nearDistance > farDistance) {
      std::swap(nearDistance, farDistance);
    }
    entry = std::max(entry, nearDistance);
    exit = std::min(exit, farDistance);
    if (entry > exit) {
      return std::nullopt;
    }
  }
  return exit >= 0.0F ? std::optional<float>(entry) : std::nullopt;
}

std::optional<QVector3D>
rayPlaneIntersection(const WorldRay &ray, const QVector3D &planePoint,
                     const QVector3D &planeNormal) {
  if (!ray.isValid() || !finiteVector(planePoint) ||
      !finiteVector(planeNormal) || planeNormal.lengthSquared() <= 1.0e-12F) {
    return std::nullopt;
  }
  const QVector3D normal = planeNormal.normalized();
  const float denominator = QVector3D::dotProduct(ray.direction, normal);
  if (std::abs(denominator) <= 1.0e-7F) {
    return std::nullopt;
  }
  const float distance =
      QVector3D::dotProduct(planePoint - ray.origin, normal) / denominator;
  if (!std::isfinite(distance) || distance < 0.0F) {
    return std::nullopt;
  }
  const QVector3D intersection = ray.origin + ray.direction * distance;
  return finiteVector(intersection) ? std::optional<QVector3D>(intersection)
                                    : std::nullopt;
}

std::optional<WorldRay> rayInModelSpace(const WorldRay &worldRay,
                                        const QMatrix4x4 &modelMatrix) {
  if (!worldRay.isValid()) {
    return std::nullopt;
  }
  bool invertible = false;
  const QMatrix4x4 inverse = modelMatrix.inverted(&invertible);
  if (!invertible) {
    return std::nullopt;
  }
  const QVector4D localOrigin = inverse * QVector4D(worldRay.origin, 1.0F);
  const QVector4D localDirection = inverse * QVector4D(worldRay.direction, 0.0F);
  WorldRay localRay{localOrigin.toVector3D(), localDirection.toVector3D()};
  if (!localRay.isValid()) {
    return std::nullopt;
  }
  localRay.direction.normalize();
  return localRay;
}

std::optional<ModelPickViewport>
modelPickViewport(const QPointF &screenPosition, const QSize &viewportSize,
                  const qreal devicePixelRatio,
                  const qreal tolerancePixels) {
  if (viewportSize.isEmpty() || !std::isfinite(screenPosition.x()) ||
      !std::isfinite(screenPosition.y()) ||
      !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0 ||
      !std::isfinite(tolerancePixels) || tolerancePixels <= 0.0) {
    return std::nullopt;
  }
  const int physicalWidth =
      std::max(1, qRound(viewportSize.width() * devicePixelRatio));
  const int physicalHeight =
      std::max(1, qRound(viewportSize.height() * devicePixelRatio));
  const int radius =
      std::clamp(qCeil(tolerancePixels * devicePixelRatio), 1, 96);
  const int diameter = radius * 2 + 1;
  const int clickX = std::clamp(
      qRound(screenPosition.x() * devicePixelRatio), 0, physicalWidth - 1);
  const int clickYFromTop = std::clamp(
      qRound(screenPosition.y() * devicePixelRatio), 0, physicalHeight - 1);
  const int clickY = physicalHeight - 1 - clickYFromTop;
  return ModelPickViewport{QSize(diameter, diameter),
                           QSize(physicalWidth, physicalHeight),
                           QPoint(radius - clickX, radius - clickY),
                           QPoint(radius, radius)};
}

bool modelPickBufferHasCoverage(const std::span<const quint8> redSamples) {
  return std::any_of(redSamples.begin(), redSamples.end(),
                     [](const quint8 sample) { return sample != 0; });
}

std::optional<float> rayAxisParameter(const WorldRay &ray,
                                      const QVector3D &axisOrigin,
                                      const QVector3D &axisDirection) {
  if (!ray.isValid() || !finiteVector(axisOrigin) ||
      !finiteVector(axisDirection) ||
      axisDirection.lengthSquared() <= 1.0e-12F) {
    return std::nullopt;
  }
  const QVector3D axis = axisDirection.normalized();
  const QVector3D offset = ray.origin - axisOrigin;
  const float rayAxisDot = QVector3D::dotProduct(ray.direction, axis);
  const float denominator = 1.0F - rayAxisDot * rayAxisDot;
  if (std::abs(denominator) <= 1.0e-7F) {
    return std::nullopt;
  }
  const float parameter =
      (QVector3D::dotProduct(offset, axis) -
       QVector3D::dotProduct(offset, ray.direction) * rayAxisDot) /
      denominator;
  return std::isfinite(parameter) ? std::optional<float>(parameter)
                                  : std::nullopt;
}

float signedAngleDegrees(const QVector3D &from, const QVector3D &to,
                         const QVector3D &axis) {
  if (!finiteVector(from) || !finiteVector(to) || !finiteVector(axis) ||
      from.lengthSquared() <= 1.0e-12F || to.lengthSquared() <= 1.0e-12F ||
      axis.lengthSquared() <= 1.0e-12F) {
    return 0.0F;
  }
  const QVector3D a = from.normalized();
  const QVector3D b = to.normalized();
  const QVector3D normal = axis.normalized();
  const float sine = QVector3D::dotProduct(normal, QVector3D::crossProduct(a, b));
  const float cosine = std::clamp(QVector3D::dotProduct(a, b), -1.0F, 1.0F);
  return std::atan2(sine, cosine) * 180.0F /
         3.14159265358979323846F;
}

float axisRotationDragDegrees(
    const QPointF &startPosition, const QPointF &currentPosition,
    const QVector3D &pivot, const QVector3D &axisDirection,
    const QMatrix4x4 &viewProjection, const QSize &viewportSize,
    const float radiusPixels) {
  if (!finiteVector(pivot) || !finiteVector(axisDirection) ||
      axisDirection.lengthSquared() <= 1.0e-12F || viewportSize.isEmpty() ||
      !std::isfinite(startPosition.x()) || !std::isfinite(startPosition.y()) ||
      !std::isfinite(currentPosition.x()) || !std::isfinite(currentPosition.y()) ||
      !std::isfinite(radiusPixels) || radiusPixels <= 1.0F) {
    return 0.0F;
  }
  const QVector3D axis = axisDirection.normalized();
  const QVector4D clip = viewProjection * QVector4D(pivot, 1.0F);
  if (!std::isfinite(clip.w()) || clip.w() <= 1.0e-7F) {
    return 0.0F;
  }
  const QPointF center((clip.x() / clip.w() * 0.5 + 0.5) * viewportSize.width(),
                       (0.5 - clip.y() / clip.w() * 0.5) * viewportSize.height());
  const auto centerRay = screenRay(center, viewportSize, viewProjection);
  if (!centerRay) {
    return 0.0F;
  }
  const QVector3D towardCamera = -centerRay->direction;
  const float facing = std::abs(QVector3D::dotProduct(axis, towardCamera));
  // Do not wait for an exact zero denominator. Thin, almost edge-on ellipses
  // are ill-conditioned as well. The camera and starting ray are fixed during
  // a gesture; above/below the horizon use the same tangent and drag direction.
  const auto startRay = screenRay(startPosition, viewportSize, viewProjection);
  if (facing >= 0.2F && startRay &&
      std::abs(QVector3D::dotProduct(startRay->direction, axis)) >= 0.1F) {
    const auto currentRay = screenRay(currentPosition, viewportSize, viewProjection);
    const auto start = rayPlaneIntersection(*startRay, pivot, axis);
    const auto current = currentRay
        ? rayPlaneIntersection(*currentRay, pivot, axis) : std::nullopt;
    if (start && current && (*start - pivot).lengthSquared() > 1.0e-12F &&
        (*current - pivot).lengthSquared() > 1.0e-12F) {
      return signedAngleDegrees(*start - pivot, *current - pivot, axis);
    }
  }

  // The front of the rotation ring moves along axis x towardCamera. Project
  // that tangent at the pivot using a derivative, avoiding subtraction of
  // large scene coordinates. Never divide by the edge-on plane denominator.
  const QVector3D tangent = QVector3D::crossProduct(axis, towardCamera);
  const QVector4D direction = viewProjection * QVector4D(tangent, 0.0F);
  const double w = clip.w();
  QPointF screenTangent(
      (direction.x() * w - clip.x() * direction.w()) / (w * w) * viewportSize.width(),
      -(direction.y() * w - clip.y() * direction.w()) / (w * w) * viewportSize.height());
  const qreal length = std::hypot(screenTangent.x(), screenTangent.y());
  if (!std::isfinite(length) || length <= 1.0e-12) {
    return 0.0F;
  }
  screenTangent /= length;
  const double radians = QPointF::dotProduct(currentPosition - startPosition,
                                              screenTangent) / radiusPixels;
  return std::isfinite(radians) ? static_cast<float>(radians * 180.0 / 3.141592653589793)
                               : 0.0F;
}

QQuaternion trackballRotationDelta(const QPointF &startPosition,
                                    const QPointF &currentPosition,
                                    const QPointF &screenCenter,
                                    const float radiusPixels,
                                    const QMatrix4x4 &viewMatrix) {
  if (!std::isfinite(radiusPixels) || radiusPixels <= 1.0F) {
    return QQuaternion();
  }
  bool invertible = false;
  const QMatrix4x4 inverseView = viewMatrix.inverted(&invertible);
  if (!invertible) {
    return QQuaternion();
  }
  QVector3D start = inverseView.mapVector(
      arcballVector(startPosition, screenCenter, radiusPixels));
  QVector3D current = inverseView.mapVector(
      arcballVector(currentPosition, screenCenter, radiusPixels));
  if (start.lengthSquared() <= 1.0e-12F ||
      current.lengthSquared() <= 1.0e-12F) {
    return QQuaternion();
  }
  start.normalize();
  current.normalize();
  return normalizedModelRotation(QQuaternion::rotationTo(start, current));
}

} // namespace gsw
