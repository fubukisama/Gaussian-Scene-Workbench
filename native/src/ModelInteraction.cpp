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

bool RigidModelTransform::isValid() const {
  return finiteVector(translation) && finiteQuaternion(rotation) &&
         rotation.lengthSquared() > 1.0e-12F;
}

QMatrix4x4 RigidModelTransform::matrix(const QVector3D &pivot) const {
  QMatrix4x4 result;
  if (!isValid() || !finiteVector(pivot)) {
    return result;
  }
  result.translate(pivot + translation);
  result.rotate(normalizedModelRotation(rotation));
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
