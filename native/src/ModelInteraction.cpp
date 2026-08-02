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

} // namespace

bool WorldRay::isValid() const {
  return finiteVector(origin) && finiteVector(direction) &&
         direction.lengthSquared() > 1.0e-12F;
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

} // namespace gsw
