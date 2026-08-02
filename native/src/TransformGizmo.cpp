#include "TransformGizmo.h"

#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace gsw {
namespace {

constexpr qreal kPi = 3.14159265358979323846;

struct ProjectedPoint final {
  QPointF screen;
  float normalizedDepth = 0.0F;
};

std::optional<ProjectedPoint> projectPoint(const QVector3D &point,
                                           const QMatrix4x4 &viewProjection,
                                           const QSizeF &viewportSize) {
  if (viewportSize.width() <= 0.0 || viewportSize.height() <= 0.0) {
    return std::nullopt;
  }
  const QVector4D clip = viewProjection * QVector4D(point, 1.0F);
  if (!std::isfinite(clip.w()) || clip.w() <= 1.0e-5F) {
    return std::nullopt;
  }
  const QVector3D normalized = clip.toVector3DAffine();
  if (!std::isfinite(normalized.x()) || !std::isfinite(normalized.y()) ||
      !std::isfinite(normalized.z()) || normalized.z() < -1.0F ||
      normalized.z() > 1.0F) {
    return std::nullopt;
  }
  return ProjectedPoint{
      QPointF((normalized.x() * 0.5F + 0.5F) * viewportSize.width(),
              (1.0F - (normalized.y() * 0.5F + 0.5F)) * viewportSize.height()),
      normalized.z()};
}

std::optional<QVector3D> unprojectPoint(const QPointF &screen,
                                        const float normalizedDepth,
                                        const QMatrix4x4 &inverseViewProjection,
                                        const QSizeF &viewportSize) {
  if (viewportSize.width() <= 0.0 || viewportSize.height() <= 0.0) {
    return std::nullopt;
  }
  const float normalizedX =
      static_cast<float>(screen.x() / viewportSize.width() * 2.0 - 1.0);
  const float normalizedY =
      static_cast<float>(1.0 - screen.y() / viewportSize.height() * 2.0);
  QVector4D world = inverseViewProjection *
                    QVector4D(normalizedX, normalizedY, normalizedDepth, 1.0F);
  if (!std::isfinite(world.w()) || std::abs(world.w()) <= 1.0e-8F) {
    return std::nullopt;
  }
  world /= world.w();
  const QVector3D point = world.toVector3D();
  if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
      !std::isfinite(point.z())) {
    return std::nullopt;
  }
  return point;
}

QVector3D unitAxis(const int axis) {
  QVector3D result;
  if (axis >= 0 && axis < 3) {
    result[axis] = 1.0F;
  }
  return result;
}

qreal distanceToLineSegment(const QPointF &point, const QLineF &line) {
  const QPointF vector = line.p2() - line.p1();
  const qreal lengthSquared = vector.x() * vector.x() + vector.y() * vector.y();
  if (lengthSquared <= 1.0e-12) {
    return QLineF(point, line.p1()).length();
  }
  const QPointF offset = point - line.p1();
  const qreal amount = std::clamp(
      (offset.x() * vector.x() + offset.y() * vector.y()) / lengthSquared, 0.0,
      1.0);
  return QLineF(point, line.p1() + vector * amount).length();
}

qreal polygonArea(const QPolygonF &polygon) {
  if (polygon.size() < 3) {
    return 0.0;
  }
  qreal twiceArea = 0.0;
  for (qsizetype index = 0; index < polygon.size(); ++index) {
    const QPointF &current = polygon.at(index);
    const QPointF &next = polygon.at((index + 1) % polygon.size());
    twiceArea += current.x() * next.y() - current.y() * next.x();
  }
  return std::abs(twiceArea) * 0.5;
}

QPolygonF screenCircle(const QPointF &center, const qreal radius,
                       const int segmentCount = 96) {
  QPolygonF circle;
  circle.reserve(segmentCount);
  for (int segment = 0; segment < segmentCount; ++segment) {
    const qreal angle = static_cast<qreal>(segment) * 2.0 * kPi /
                        static_cast<qreal>(segmentCount);
    circle.append(center +
                  QPointF(std::cos(angle) * radius, -std::sin(angle) * radius));
  }
  return circle;
}

bool scaleHandleContains(const TransformGizmoAxisLayout &axis,
                         const QPointF &position) {
  return axis.visible &&
         axis.scaleHandle.adjusted(-2.0, -2.0, 2.0, 2.0).contains(position);
}

TransformGizmoHandle closestAxisLine(const TransformGizmoLayout &layout,
                                     const QPointF &position,
                                     const TransformGizmoHandleKind kind,
                                     const qreal tolerance) {
  qreal bestDistance = tolerance;
  int bestAxis = -1;
  for (int axis = 0; axis < 3; ++axis) {
    const TransformGizmoAxisLayout &axisLayout =
        layout.axes[static_cast<std::size_t>(axis)];
    if (!axisLayout.visible) {
      continue;
    }
    const qreal distance = distanceToLineSegment(position, axisLayout.line);
    if (distance <= bestDistance) {
      bestDistance = distance;
      bestAxis = axis;
    }
  }
  return bestAxis >= 0 ? TransformGizmoHandle{kind, bestAxis}
                       : TransformGizmoHandle{};
}

TransformGizmoHandle closestRotationRing(const TransformGizmoLayout &layout,
                                         const QPointF &position,
                                         const qreal tolerance) {
  qreal bestDistance = tolerance;
  int bestAxis = -1;
  for (int axis = 0; axis < 3; ++axis) {
    const QPolygonF &ring =
        layout.axes[static_cast<std::size_t>(axis)].rotationRing;
    if (ring.size() < 3) {
      continue;
    }
    const qreal distance = pointPolylineDistance(position, ring, true);
    if (distance <= bestDistance) {
      bestDistance = distance;
      bestAxis = axis;
    }
  }
  return bestAxis >= 0
             ? TransformGizmoHandle{TransformGizmoHandleKind::RotateAxis,
                                    bestAxis}
             : TransformGizmoHandle{};
}

TransformGizmoHandle planeHandleAt(const TransformGizmoLayout &layout,
                                   const QPointF &position,
                                   const TransformGizmoHandleKind kind) {
  for (int excludedAxis = 0; excludedAxis < 3; ++excludedAxis) {
    const QPolygonF &polygon =
        layout.planeHandles[static_cast<std::size_t>(excludedAxis)];
    if (polygon.size() >= 3 &&
        polygon.containsPoint(position, Qt::OddEvenFill)) {
      return {kind, excludedAxis};
    }
  }
  return {};
}

} // namespace

TransformGizmoLayout transformGizmoLayout(const QVector3D &pivot,
                                          const QQuaternion &orientation,
                                          const QMatrix4x4 &viewProjection,
                                          const QSizeF &viewportSize,
                                          const qreal radiusPixels) {
  TransformGizmoLayout layout;
  const auto projectedCenter =
      projectPoint(pivot, viewProjection, viewportSize);
  if (!projectedCenter.has_value()) {
    return layout;
  }
  bool invertible = false;
  const QMatrix4x4 inverseViewProjection = viewProjection.inverted(&invertible);
  if (!invertible) {
    return layout;
  }

  layout.center = projectedCenter->screen;
  layout.radius = std::clamp(radiusPixels, 58.0, 118.0);
  layout.lineWidth = std::clamp(layout.radius / 36.0, 1.8, 3.2);
  const auto worldAtCenter =
      unprojectPoint(layout.center, projectedCenter->normalizedDepth,
                     inverseViewProjection, viewportSize);
  const auto worldAtRadius = unprojectPoint(
      layout.center + QPointF(layout.radius, 0.0),
      projectedCenter->normalizedDepth, inverseViewProjection, viewportSize);
  if (!worldAtCenter.has_value() || !worldAtRadius.has_value()) {
    return layout;
  }
  const float worldRadius = (*worldAtRadius - *worldAtCenter).length();
  if (!std::isfinite(worldRadius) || worldRadius <= 1.0e-8F) {
    return layout;
  }

  QQuaternion normalizedOrientation = orientation;
  if (!std::isfinite(normalizedOrientation.scalar()) ||
      !std::isfinite(normalizedOrientation.x()) ||
      !std::isfinite(normalizedOrientation.y()) ||
      !std::isfinite(normalizedOrientation.z()) ||
      normalizedOrientation.lengthSquared() <= 1.0e-12F) {
    normalizedOrientation = QQuaternion();
  } else {
    normalizedOrientation.normalize();
  }

  std::array<QPointF, 3> screenDirections;
  for (int axis = 0; axis < 3; ++axis) {
    TransformGizmoAxisLayout &axisLayout =
        layout.axes[static_cast<std::size_t>(axis)];
    axisLayout.direction =
        normalizedOrientation.rotatedVector(unitAxis(axis)).normalized();
    const auto endpoint =
        projectPoint(pivot + axisLayout.direction * worldRadius * 0.86F,
                     viewProjection, viewportSize);
    if (!endpoint.has_value()) {
      continue;
    }
    QPointF screenDirection = endpoint->screen - layout.center;
    const qreal length = std::hypot(screenDirection.x(), screenDirection.y());
    if (!std::isfinite(length) || length < 7.0) {
      continue;
    }
    screenDirection /= length;
    screenDirections[static_cast<std::size_t>(axis)] = screenDirection;
    axisLayout.visible = true;
    axisLayout.endpoint = endpoint->screen;
    axisLayout.line =
        QLineF(layout.center + screenDirection * 13.0, endpoint->screen);
    axisLayout.scaleHandle =
        QRectF(endpoint->screen - QPointF(6.0, 6.0), QSizeF(12.0, 12.0));
  }

  for (int excludedAxis = 0; excludedAxis < 3; ++excludedAxis) {
    const int firstAxis = (excludedAxis + 1) % 3;
    const int secondAxis = (excludedAxis + 2) % 3;
    if (!layout.axes[static_cast<std::size_t>(firstAxis)].visible ||
        !layout.axes[static_cast<std::size_t>(secondAxis)].visible) {
      continue;
    }
    const QPointF first = screenDirections[static_cast<std::size_t>(firstAxis)];
    const QPointF second =
        screenDirections[static_cast<std::size_t>(secondAxis)];
    QPolygonF polygon;
    polygon << layout.center + first * 18.0 + second * 18.0
            << layout.center + first * 35.0 + second * 18.0
            << layout.center + first * 35.0 + second * 35.0
            << layout.center + first * 18.0 + second * 35.0;
    if (polygonArea(polygon) >= 24.0) {
      layout.planeHandles[static_cast<std::size_t>(excludedAxis)] = polygon;
    }
  }

  constexpr int ringSegments = 96;
  for (int axis = 0; axis < 3; ++axis) {
    const int firstAxis = (axis + 1) % 3;
    const int secondAxis = (axis + 2) % 3;
    const QVector3D firstDirection =
        normalizedOrientation.rotatedVector(unitAxis(firstAxis)).normalized();
    const QVector3D secondDirection =
        normalizedOrientation.rotatedVector(unitAxis(secondAxis)).normalized();
    QPolygonF ring;
    ring.reserve(ringSegments);
    bool complete = true;
    for (int segment = 0; segment < ringSegments; ++segment) {
      const qreal angle = static_cast<qreal>(segment) * 2.0 * kPi /
                          static_cast<qreal>(ringSegments);
      const QVector3D point =
          pivot + (firstDirection * static_cast<float>(std::cos(angle)) +
                   secondDirection * static_cast<float>(std::sin(angle))) *
                      worldRadius * 0.78F;
      const auto projected = projectPoint(point, viewProjection, viewportSize);
      if (!projected.has_value()) {
        complete = false;
        break;
      }
      ring.append(projected->screen);
    }
    if (complete) {
      layout.axes[static_cast<std::size_t>(axis)].rotationRing = ring;
    }
  }

  layout.viewRing = screenCircle(layout.center, layout.radius * 1.04);
  layout.centerHandle =
      QRectF(layout.center - QPointF(10.0, 10.0), QSizeF(20.0, 20.0));
  const qreal trackballRadius = layout.radius * 0.68;
  layout.trackballBounds =
      QRectF(layout.center - QPointF(trackballRadius, trackballRadius),
             QSizeF(trackballRadius * 2.0, trackballRadius * 2.0));
  layout.valid = true;
  return layout;
}

qreal pointPolylineDistance(const QPointF &point, const QPolygonF &polyline,
                            const bool closed) {
  if (polyline.size() < 2) {
    return std::numeric_limits<qreal>::infinity();
  }
  qreal best = std::numeric_limits<qreal>::infinity();
  for (qsizetype index = 1; index < polyline.size(); ++index) {
    best = std::min(best,
                    distanceToLineSegment(point, QLineF(polyline.at(index - 1),
                                                        polyline.at(index))));
  }
  if (closed) {
    best = std::min(
        best, distanceToLineSegment(
                  point, QLineF(polyline.constLast(), polyline.constFirst())));
  }
  return best;
}

TransformGizmoHandle hitTestTransformGizmo(const TransformGizmoLayout &layout,
                                           const QPointF &position,
                                           const TransformGizmoMode mode) {
  if (!layout.valid) {
    return {};
  }
  const qreal axisTolerance = std::max(6.0, layout.lineWidth * 2.5);
  const qreal ringTolerance = std::max(5.5, layout.lineWidth * 2.2);

  if (mode == TransformGizmoMode::Scale ||
      mode == TransformGizmoMode::Transform) {
    for (int axis = 0; axis < 3; ++axis) {
      if (scaleHandleContains(layout.axes[static_cast<std::size_t>(axis)],
                              position)) {
        return {TransformGizmoHandleKind::ScaleAxis, axis};
      }
    }
    if (mode == TransformGizmoMode::Scale) {
      if (const TransformGizmoHandle plane = planeHandleAt(
              layout, position, TransformGizmoHandleKind::ScalePlane);
          plane.isValid()) {
        return plane;
      }
    }
  }

  if (mode == TransformGizmoMode::Move ||
      mode == TransformGizmoMode::Transform) {
    if (const TransformGizmoHandle plane = planeHandleAt(
            layout, position, TransformGizmoHandleKind::MovePlane);
        plane.isValid()) {
      return plane;
    }
    if (const TransformGizmoHandle axis =
            closestAxisLine(layout, position,
                            TransformGizmoHandleKind::MoveAxis, axisTolerance);
        axis.isValid()) {
      return axis;
    }
    if (layout.centerHandle.adjusted(-2.0, -2.0, 2.0, 2.0).contains(position)) {
      return {TransformGizmoHandleKind::MoveView, -1};
    }
  }

  if (mode == TransformGizmoMode::Rotate ||
      mode == TransformGizmoMode::Transform) {
    if (const TransformGizmoHandle ring =
            closestRotationRing(layout, position, ringTolerance);
        ring.isValid()) {
      return ring;
    }
    if (pointPolylineDistance(position, layout.viewRing, true) <=
        ringTolerance) {
      return {TransformGizmoHandleKind::RotateView, -1};
    }
    const qreal distance = QLineF(layout.center, position).length();
    if (distance <= layout.radius * 0.68 && distance >= 14.0) {
      return {TransformGizmoHandleKind::RotateTrackball, -1};
    }
  }

  if (mode == TransformGizmoMode::Scale) {
    if (const TransformGizmoHandle axis =
            closestAxisLine(layout, position,
                            TransformGizmoHandleKind::ScaleAxis, axisTolerance);
        axis.isValid()) {
      return axis;
    }
    if (layout.centerHandle.adjusted(-3.0, -3.0, 3.0, 3.0).contains(position)) {
      return {TransformGizmoHandleKind::ScaleUniform, -1};
    }
  }

  return {};
}

TransformToolStripLayout transformToolStripLayout(const QSizeF &viewportSize,
                                                  const qreal fontHeight) {
  TransformToolStripLayout layout;
  const qreal buttonSize = std::clamp(fontHeight * 2.05, 34.0, 46.0);
  const qreal gap = std::clamp(fontHeight * 0.16, 2.0, 4.0);
  const qreal margin = std::clamp(fontHeight * 0.55, 8.0, 14.0);
  const qreal top = std::clamp(fontHeight * 4.2, 70.0, 104.0);
  const qreal left = margin;
  for (int index = 0; index < 4; ++index) {
    layout.buttons[static_cast<std::size_t>(index)] =
        QRectF(left, top + index * (buttonSize + gap), buttonSize, buttonSize);
  }
  const qreal orientationTop = top + 4.0 * (buttonSize + gap) + gap * 1.5;
  layout.orientationButton =
      QRectF(left, orientationTop, buttonSize, buttonSize * 0.62);
  layout.background = QRectF(
      left - gap, top - gap, buttonSize + gap * 2.0,
      orientationTop + layout.orientationButton.height() - top + gap * 2.0);
  if (layout.background.bottom() > viewportSize.height() - margin) {
    const qreal offset =
        layout.background.bottom() - (viewportSize.height() - margin);
    layout.background.translate(0.0, -offset);
    layout.orientationButton.translate(0.0, -offset);
    for (QRectF &button : layout.buttons) {
      button.translate(0.0, -offset);
    }
  }
  return layout;
}

int hitTestTransformToolStrip(const TransformToolStripLayout &layout,
                              const QPointF &position) {
  for (int index = 0; index < 4; ++index) {
    if (layout.buttons[static_cast<std::size_t>(index)].contains(position)) {
      return index;
    }
  }
  return layout.orientationButton.contains(position) ? 4 : -1;
}

} // namespace gsw
