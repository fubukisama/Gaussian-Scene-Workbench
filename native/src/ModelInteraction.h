#pragma once

#include <QMatrix4x4>
#include <QPoint>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QVector3D>

#include <optional>
#include <span>

namespace gsw {

struct WorldRay final {
  QVector3D origin;
  QVector3D direction;

  [[nodiscard]] bool isValid() const;
};

struct ModelTransform final {
  QVector3D translation;
  QQuaternion rotation;
  QVector3D scale = QVector3D(1.0F, 1.0F, 1.0F);

  [[nodiscard]] bool isValid() const;
  [[nodiscard]] QMatrix4x4 matrix(const QVector3D &pivot) const;
};

struct ModelPickViewport final {
  QSize framebufferSize;
  QSize sourceViewportSize;
  QPoint viewportOrigin;
  QPoint sampleCenter;

  [[nodiscard]] bool isValid() const {
    return !framebufferSize.isEmpty() && !sourceViewportSize.isEmpty();
  }
};

[[nodiscard]] QQuaternion
normalizedModelRotation(const QQuaternion &rotation);

[[nodiscard]] bool rotationsEquivalent(const QQuaternion &left,
                                       const QQuaternion &right,
                                       float tolerance = 1.0e-6F);

[[nodiscard]] QVector3D normalizedModelScale(const QVector3D &scale);

[[nodiscard]] bool scalesEquivalent(const QVector3D &left,
                                    const QVector3D &right,
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

[[nodiscard]] std::optional<ModelPickViewport>
modelPickViewport(const QPointF &screenPosition, const QSize &viewportSize,
                  qreal devicePixelRatio, qreal tolerancePixels = 6.0);

[[nodiscard]] bool
modelPickBufferHasCoverage(std::span<const quint8> redSamples);

[[nodiscard]] std::optional<float>
rayAxisParameter(const WorldRay &ray, const QVector3D &axisOrigin,
                 const QVector3D &axisDirection);

[[nodiscard]] float signedAngleDegrees(const QVector3D &from,
                                       const QVector3D &to,
                                       const QVector3D &axis);

// Axis-constrained mouse rotation. Near edge-on planes use a screen tangent
// fixed by the camera/axis, so parallel ray-plane intersections cannot freeze
// rotation. radiusPixels sets drag sensitivity, not the model's world scale.
[[nodiscard]] float axisRotationDragDegrees(
    const QPointF &startPosition, const QPointF &currentPosition,
    const QVector3D &pivot, const QVector3D &axis,
    const QMatrix4x4 &viewProjection, const QSize &viewportSize,
    float radiusPixels);

[[nodiscard]] QQuaternion
trackballRotationDelta(const QPointF &startPosition,
                       const QPointF &currentPosition,
                       const QPointF &screenCenter, float radiusPixels,
                       const QMatrix4x4 &viewMatrix);

} // namespace gsw
