#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QVector3D>

#include <optional>

namespace gsw {

struct WorldRay final {
  QVector3D origin;
  QVector3D direction;

  [[nodiscard]] bool isValid() const;
};

[[nodiscard]] std::optional<WorldRay>
screenRay(const QPointF &screenPosition, const QSize &viewportSize,
          const QMatrix4x4 &viewProjection);

[[nodiscard]] std::optional<float>
rayAabbDistance(const WorldRay &ray, const QVector3D &minimum,
                const QVector3D &maximum);

[[nodiscard]] std::optional<QVector3D>
rayPlaneIntersection(const WorldRay &ray, const QVector3D &planePoint,
                     const QVector3D &planeNormal);

} // namespace gsw
