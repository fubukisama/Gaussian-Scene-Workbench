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
  const qreal padding = std::min(3.0, axis.scaleHandle.width() * 0.25);
  return axis.visible &&
         axis.scaleHandle.adjusted(-padding, -padding, padding, padding).contains(position);
}

bool moveHandleContains(const TransformGizmoAxisLayout &axis,
                        const QPointF &position) {
  return axis.visible && axis.moveArrow.size() >= 3 &&
         (axis.moveArrow.containsPoint(position, Qt::OddEvenFill) ||
          pointPolylineDistance(position, axis.moveArrow, true) <=
              std::min(3.0, axis.moveArrow.boundingRect().size().width() * 0.25));
}

bool centerHandleContains(const TransformGizmoLayout &layout,
                           const QPointF &position, bool diamond = false) {
  const qreal radius = layout.centerHandle.width() * 0.5;
  const qreal paddedRadius = radius + std::min(2.0, radius * 0.25);
  const QPointF delta = position - layout.center;
  return diamond ? std::abs(delta.x()) + std::abs(delta.y()) <= paddedRadius
                 : std::hypot(delta.x(), delta.y()) <= paddedRadius;
}

// Clip individual world-space edges before perspective division. Near-camera
// rings can cross the eye/near plane; neither discard the whole ring nor join
// disconnected visible arcs with an artificial closing line.
std::optional<QLineF> projectSegment(const QVector3D &start, const QVector3D &end,
                                      const QMatrix4x4 &vp, const QSizeF &size) {
  const QVector4D a = vp * QVector4D(start, 1), b = vp * QVector4D(end, 1);
  const std::array<float, 6> da{a.w() + a.x(), a.w() - a.x(), a.w() + a.y(),
                               a.w() - a.y(), a.w() + a.z(), a.w() - a.z()};
  const std::array<float, 6> db{b.w() + b.x(), b.w() - b.x(), b.w() + b.y(),
                               b.w() - b.y(), b.w() + b.z(), b.w() - b.z()};
  float first = 0, last = 1;
  for (std::size_t plane = 0; plane < da.size(); ++plane) {
    if (!std::isfinite(da[plane]) || !std::isfinite(db[plane]) ||
        (da[plane] < 0 && db[plane] < 0)) {
      return std::nullopt;
    }
    if (da[plane] < 0) {
      first = std::max(first, da[plane] / (da[plane] - db[plane]));
    } else if (db[plane] < 0) {
      last = std::min(last, da[plane] / (da[plane] - db[plane]));
    }
  }
  if (first > last) {
    return std::nullopt;
  }
  const QVector4D ca = a + (b - a) * first, cb = a + (b - a) * last;
  if (ca.w() <= 1.0e-5F || cb.w() <= 1.0e-5F) {
    return std::nullopt;
  }
  const auto screen = [&size](const QVector4D &clip) {
    return QPointF((clip.x() / clip.w() + 1) * size.width() * 0.5,
                   (1 - clip.y() / clip.w()) * size.height() * 0.5);
  };
  return QLineF(screen(ca), screen(cb));
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
    const QLineF line = kind == TransformGizmoHandleKind::MoveAxis
                            ? axisLayout.moveLine
                            : axisLayout.scaleLine;
    const qreal distance = distanceToLineSegment(position, line);
    if (distance <= bestDistance) {
      bestDistance = distance;
      bestAxis = axis;
    }
  }
  return bestAxis >= 0 ? TransformGizmoHandle{kind, bestAxis}
                       : TransformGizmoHandle{};
}

TransformGizmoHandle moveArrowAt(const TransformGizmoLayout &layout,
                                 const QPointF &position) {
  for (int axis = 0; axis < 3; ++axis) {
    if (moveHandleContains(layout.axes[static_cast<std::size_t>(axis)],
                           position)) {
      return {TransformGizmoHandleKind::MoveAxis, axis};
    }
  }
  return {};
}

TransformGizmoHandle closestRotationRing(const TransformGizmoLayout &layout,
                                         const QPointF &position,
                                         const qreal tolerance) {
  const qreal centerDistance = QLineF(layout.center, position).length();
  if (centerDistance < layout.rotationHitInnerRadius) {
    return {};
  }
  qreal bestDistance = tolerance;
  int bestAxis = -1;
  for (int axis = 0; axis < 3; ++axis) {
    qreal distance = std::numeric_limits<qreal>::infinity();
    for (const QLineF &segment : layout.axes[static_cast<std::size_t>(axis)].rotationSegments) {
      distance = std::min(distance, distanceToLineSegment(position, segment));
    }
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

bool transformOrientationLocked(const TransformGizmoMode mode) {
  return mode == TransformGizmoMode::Scale ||
         mode == TransformGizmoMode::Transform;
}

bool transformUsesLocalOrientation(const TransformGizmoMode mode,
                                   const bool localPreference) {
  return transformOrientationLocked(mode) || localPreference;
}

TransformGizmoLayout transformGizmoLayout(const QVector3D &pivot,
                                          const QQuaternion &orientation,
                                          const QMatrix4x4 &viewProjection,
                                          const QSizeF &viewportSize,
                                          const TransformGizmoMode mode,
                                          const qreal worldRadius) {
  TransformGizmoLayout layout;
  const auto projectedCenter =
      projectPoint(pivot, viewProjection, viewportSize);
  if (!projectedCenter.has_value() || !std::isfinite(worldRadius) || worldRadius <= 0.0) {
    return layout;
  }
  bool invertible = false;
  const QMatrix4x4 inverseViewProjection = viewProjection.inverted(&invertible);
  if (!invertible) {
    return layout;
  }

  layout.center = projectedCenter->screen;
  const bool combined = mode == TransformGizmoMode::Transform;
  // Measure the camera's projection, not a desired screen-space size. The
  // caller supplies a stable world radius; zoom must never resize it in world
  // space to compensate for camera distance or orthographic scale.
  const auto worldAtCenter =
      unprojectPoint(layout.center, projectedCenter->normalizedDepth,
                     inverseViewProjection, viewportSize);
  const auto worldAtRight = unprojectPoint(
      layout.center + QPointF(viewportSize.width() * 0.5, 0.0),
      projectedCenter->normalizedDepth, inverseViewProjection, viewportSize);
  if (!worldAtCenter || !worldAtRight) {
    return layout;
  }
  const QVector3D right = (*worldAtRight - *worldAtCenter).normalized();
  const auto projectedRadius = projectPoint(
      pivot + right * static_cast<float>(worldRadius), viewProjection, viewportSize);
  if (!projectedRadius) {
    return layout;
  }
  layout.radius = QLineF(layout.center, projectedRadius->screen).length();
  if (!std::isfinite(layout.radius) || layout.radius <= 1.0e-6) {
    return layout;
  }
  const qreal shapeScale = layout.radius / (combined ? 148.0 : 84.0);
  layout.lineWidth = std::clamp(layout.radius / 36.0, 1.8, 3.2);
  const qreal moveExtent = layout.radius * (combined ? 0.43 : 0.86);
  const qreal scaleExtent = layout.radius * (combined ? 0.67 : 0.86);
  const qreal rotationExtent = layout.radius * (combined ? 0.84 : 0.78);
  const qreal viewRingRadius = layout.radius * (combined ? 1.0 : 1.04);
  const qreal centerClearance = (combined ? 15.0 : 13.0) * shapeScale;
  layout.rotationHitInnerRadius = combined ? layout.radius * 0.72 : 0.0;
  layout.trackballInnerRadius = combined ? layout.radius * 0.29 : 14.0 * shapeScale;
  layout.trackballOuterRadius =
      layout.radius * (combined ? 0.41 : 0.68);

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

  const qreal pivotClipW = (viewProjection * QVector4D(pivot, 1.0F)).w();
  for (int axis = 0; axis < 3; ++axis) {
    TransformGizmoAxisLayout &axisLayout =
        layout.axes[static_cast<std::size_t>(axis)];
    axisLayout.direction =
        normalizedOrientation.rotatedVector(unitAxis(axis)).normalized();
    const auto worldOnAxis = [&](qreal extent) {
      return pivot + axisLayout.direction *
                         static_cast<float>(worldRadius * extent / layout.radius);
    };
    const qreal scaleLineStart =
        combined ? moveExtent + 15.0 * shapeScale : centerClearance;
    const auto moveEnd = projectPoint(worldOnAxis(moveExtent), viewProjection, viewportSize);
    const auto scaleEnd = projectPoint(worldOnAxis(scaleExtent), viewProjection, viewportSize);
    const auto moveStart = projectPoint(worldOnAxis(centerClearance), viewProjection, viewportSize);
    const auto scaleStart = projectPoint(worldOnAxis(scaleLineStart), viewProjection, viewportSize);
    const auto scaleLineEnd = projectPoint(
        worldOnAxis(scaleExtent - (combined ? 8.5 * shapeScale : 0.0)), viewProjection, viewportSize);
    if (!moveEnd || !scaleEnd || !moveStart || !scaleStart || !scaleLineEnd) {
      continue;
    }
    QPointF screenDirection = moveEnd->screen - layout.center;
    const qreal length = std::hypot(screenDirection.x(), screenDirection.y());
    if (!std::isfinite(length) || length < 1.0e-6) {
      continue;
    }
    screenDirection /= length;
    axisLayout.visible = true;
    axisLayout.moveEndpoint = moveEnd->screen;
    axisLayout.scaleEndpoint = scaleEnd->screen;
    axisLayout.moveLine = QLineF(moveStart->screen, moveEnd->screen);
    axisLayout.scaleLine = QLineF(scaleStart->screen, scaleLineEnd->screen);
    const QPointF normal(-screenDirection.y(), screenDirection.x());
    const qreal moveShapeScale = shapeScale * pivotClipW /
        (viewProjection * QVector4D(worldOnAxis(moveExtent), 1.0F)).w();
    const qreal scaleShapeScale = shapeScale * pivotClipW /
        (viewProjection * QVector4D(worldOnAxis(scaleExtent), 1.0F)).w();
    const qreal arrowLength = (combined ? 12.0 : 11.0) * moveShapeScale;
    const qreal arrowHalfWidth = (combined ? 6.0 : 5.0) * moveShapeScale;
    axisLayout.moveArrow
        << axisLayout.moveEndpoint
        << axisLayout.moveEndpoint - screenDirection * arrowLength +
               normal * arrowHalfWidth
        << axisLayout.moveEndpoint - screenDirection * arrowLength -
               normal * arrowHalfWidth;
    const qreal scaleHandleSize = (combined ? 15.0 : 12.0) * scaleShapeScale;
    axisLayout.scaleHandle =
        QRectF(axisLayout.scaleEndpoint -
                   QPointF(scaleHandleSize * 0.5, scaleHandleSize * 0.5),
               QSizeF(scaleHandleSize, scaleHandleSize));
  }

  for (int excludedAxis = 0; excludedAxis < 3; ++excludedAxis) {
    const int firstAxis = (excludedAxis + 1) % 3;
    const int secondAxis = (excludedAxis + 2) % 3;
    const QVector3D first = layout.axes[static_cast<std::size_t>(firstAxis)].direction;
    const QVector3D second = layout.axes[static_cast<std::size_t>(secondAxis)].direction;
    QPolygonF polygon;
    const float inner = static_cast<float>((combined ? 22.0 : 18.0) *
                                          worldRadius / (combined ? 148.0 : 84.0));
    const float outer = static_cast<float>((combined ? 40.0 : 35.0) *
                                          worldRadius / (combined ? 148.0 : 84.0));
    for (const QVector3D &offset : {first * inner + second * inner,
                                    first * outer + second * inner,
                                    first * outer + second * outer,
                                    first * inner + second * outer}) {
      const auto point = projectPoint(pivot + offset, viewProjection, viewportSize);
      if (!point) {
        polygon.clear();
        break;
      }
      polygon << point->screen;
    }
    if (polygonArea(polygon) >= 1.0e-6) {
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
    QVector3D previous = pivot + firstDirection *
        static_cast<float>(worldRadius * rotationExtent / layout.radius);
    for (int segment = 0; segment < ringSegments; ++segment) {
      const qreal angle = static_cast<qreal>(segment + 1) * 2.0 * kPi /
                          static_cast<qreal>(ringSegments);
      const QVector3D point =
          pivot + (firstDirection * static_cast<float>(std::cos(angle)) +
                   secondDirection * static_cast<float>(std::sin(angle))) *
                      worldRadius * static_cast<float>(rotationExtent /
                                                       layout.radius);
      const auto projected = projectPoint(point, viewProjection, viewportSize);
      if (!projected.has_value()) {
        complete = false;
      } else {
        ring.append(projected->screen);
      }
      const auto edge = projectSegment(previous, point, viewProjection, viewportSize);
      if (edge) {
        layout.axes[static_cast<std::size_t>(axis)].rotationSegments.append(*edge);
      }
      previous = point;
    }
    if (complete) {
      layout.axes[static_cast<std::size_t>(axis)].rotationRing = ring;
    }
  }

  layout.viewRing = screenCircle(layout.center, viewRingRadius);
  const qreal centerHandleSize = (combined ? 24.0 : 20.0) * shapeScale;
  layout.centerHandle =
      QRectF(layout.center -
                 QPointF(centerHandleSize * 0.5, centerHandleSize * 0.5),
             QSizeF(centerHandleSize, centerHandleSize));
  layout.trackballBounds =
      QRectF(layout.center - QPointF(layout.trackballOuterRadius,
                                    layout.trackballOuterRadius),
             QSizeF(layout.trackballOuterRadius * 2.0,
                    layout.trackballOuterRadius * 2.0));
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
  const qreal axisTolerance = std::min(std::max(6.0, layout.lineWidth * 2.5),
                                       layout.radius * 0.08);
  const qreal ringTolerance = std::min(std::max(5.5, layout.lineWidth * 2.2),
                                       layout.radius * 0.06);

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
    } else if (const TransformGizmoHandle axis = closestAxisLine(
                   layout, position, TransformGizmoHandleKind::ScaleAxis,
                   axisTolerance);
               axis.isValid()) {
      return axis;
    }
  }

  if (mode == TransformGizmoMode::Move ||
      mode == TransformGizmoMode::Transform) {
    if (centerHandleContains(layout, position)) {
      return {TransformGizmoHandleKind::MoveView, -1};
    }
    if (const TransformGizmoHandle plane = planeHandleAt(
            layout, position, TransformGizmoHandleKind::MovePlane);
        plane.isValid()) {
      return plane;
    }
    if (const TransformGizmoHandle arrow = moveArrowAt(layout, position);
        arrow.isValid()) {
      return arrow;
    }
    if (const TransformGizmoHandle axis =
            closestAxisLine(layout, position,
                            TransformGizmoHandleKind::MoveAxis, axisTolerance);
        axis.isValid()) {
      return axis;
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
    if (distance <= layout.trackballOuterRadius &&
        distance >= layout.trackballInnerRadius) {
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
    if (centerHandleContains(layout, position, true)) {
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
  const qreal orientationHeight =
      std::clamp(fontHeight * 1.65, 28.0, 38.0);
  layout.orientationButton =
      QRectF(left, orientationTop, buttonSize, orientationHeight);
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

QRectF transformGizmoHintRect(const TransformGizmoLayout &layout,
                              const QSizeF &hintSize,
                              const QSizeF &viewportSize, const qreal gap,
                              const qreal margin) {
  if (!layout.valid || hintSize.isEmpty() || viewportSize.isEmpty()) {
    return {};
  }
  const QRectF viewport(QPointF(), viewportSize);
  const QRectF available = viewport.adjusted(margin, margin, -margin, -margin);
  const QRectF gizmoBounds =
      layout.viewRing.boundingRect().adjusted(-gap, -gap, gap, gap);
  const std::array<QRectF, 4> candidates = {
      QRectF(QPointF(layout.center.x() - hintSize.width() * 0.5,
                     gizmoBounds.top() - hintSize.height()),
             hintSize),
      QRectF(QPointF(gizmoBounds.right(),
                     layout.center.y() - hintSize.height() * 0.5),
             hintSize),
      QRectF(QPointF(gizmoBounds.left() - hintSize.width(),
                     layout.center.y() - hintSize.height() * 0.5),
             hintSize),
      QRectF(QPointF(layout.center.x() - hintSize.width() * 0.5,
                     gizmoBounds.bottom()),
             hintSize)};
  for (const QRectF &candidate : candidates) {
    if (available.contains(candidate) && !candidate.intersects(gizmoBounds)) {
      return candidate;
    }
  }

  QRectF fallback = candidates.front();
  const qreal maximumLeft =
      std::max(available.left(), available.right() - fallback.width());
  const qreal maximumTop =
      std::max(available.top(), available.bottom() - fallback.height());
  fallback.moveLeft(
      std::clamp(fallback.left(), available.left(), maximumLeft));
  fallback.moveTop(std::clamp(fallback.top(), available.top(), maximumTop));
  return fallback;
}

} // namespace gsw
