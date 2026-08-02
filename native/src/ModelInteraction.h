#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QVector3D>

#include <optional>

namespace gsw {

struct WorldRay final {
  QVector3D origin;
  QVector3D direction;

  [[nodiscard]] bool isValid() const;
};

struct RigidModelTransform final {
  QVector3D translation;
  QQuaternion rotation;

  [[nodiscard]] bool isValid() const;
  [[nodiscard]] QMatrix4x4 matrix(const QVector3D &pivot) const;
};

[[nodiscard]] QQuaternion
normalizedModelRotation(const QQuaternion &rotation);

[[nodiscard]] bool rotationsEquivalent(const QQuaternion &left,
                                       const QQuaternion &right,
                                       float tolerance = 1.0e-6F);

[[nodiscard]] std::optional<WorldRay>
screenRay(const QPointF &screenPosition, const QSize &viewportSize,
          const QMatrix4x4 &viewProjection);

[[nodiscard]] std::optional<float>
rayAabbDistance(const WorldRay &ray, const QVector3D &minimum,
                const QVector3D &maximum);

[[nodiscard]] std::optional<QVector3D>
rayPlaneIntersection(const WorldRay &ray, const QVector3D &planePoint,
                     const QVector3D &planeNormal);

[[nodiscard]] std::optional<WorldRay>
rayInModelSpace(const WorldRay &worldRay, const QMatrix4x4 &modelMatrix);

[[nodiscard]] std::optional<float>
rayAxisParameter(const WorldRay &ray, const QVector3D &axisOrigin,
                 const QVector3D &axisDirection);

[[nodiscard]] float signedAngleDegrees(const QVector3D &from,
                                       const QVector3D &to,
                                       const QVector3D &axis);

[[nodiscard]] QQuaternion
trackballRotationDelta(const QPointF &startPosition,
                       const QPointF &currentPosition,
                       const QPointF &screenCenter, float radiusPixels,
                       const QMatrix4x4 &viewMatrix);

} // namespace gsw
