#include "TransformGizmo.h"

#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

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
  if (!axis.visible || axis.scaleHandle.isEmpty()) return false;
  if (axis.scaleFaces.isEmpty())
    return axis.scaleHandle.adjusted(-padding, -padding, padding, padding).contains(position);
  for (const auto &face : axis.scaleFaces) {
    if (face.polygon.containsPoint(position, Qt::OddEvenFill) ||
        pointPolylineDistance(position, face.polygon, true) <= padding)
      return true;
  }
  return false;
}

bool moveHandleContains(const TransformGizmoAxisLayout &axis,
                        const QPointF &position) {
  if (!axis.visible || axis.moveArrow.size() < 3) return false;
  const qreal padding = std::min(3.0, axis.moveArrow.boundingRect().width() * 0.25);
  if (axis.moveFaces.isEmpty())
    return axis.moveArrow.containsPoint(position, Qt::OddEvenFill) ||
           pointPolylineDistance(position, axis.moveArrow, true) <= padding;
  for (const auto &face : axis.moveFaces) {
    if (face.polygon.containsPoint(position, Qt::OddEvenFill) ||
        pointPolylineDistance(position, face.polygon, true) <= padding)
      return true;
  }
  return false;
}

struct HandleHit final {
  qreal distance = 0;
  float depth = 0;
};

std::optional<HandleHit> solidHandleHit(const TransformGizmoAxisLayout &axis,
                                      const QPointF &position,
                                      const TransformGizmoHandleKind kind) {
  const bool move = kind == TransformGizmoHandleKind::MoveAxis;
  if (!(move ? moveHandleContains(axis, position)
             : scaleHandleContains(axis, position))) return std::nullopt;
  const QPolygonF legacy = move ? axis.moveArrow : QPolygonF(axis.scaleHandle);
  const auto &faces = move ? axis.moveFaces : axis.scaleFaces;
  if (faces.isEmpty()) {
    const qreal distance = legacy.containsPoint(position, Qt::OddEvenFill) ? 0
        : pointPolylineDistance(position, legacy, true);
    return HandleHit{distance, move ? axis.moveEndpointDepth : axis.scaleEndpointDepth};
  }
  const qreal padding = std::min(3.0, (move ? axis.moveArrow.boundingRect().width() :
                                              axis.scaleHandle.width()) * 0.25);
  std::optional<HandleHit> hit;
  for (const auto &face : faces) {
    const qreal faceDistance = face.polygon.containsPoint(position, Qt::OddEvenFill) ? 0
        : pointPolylineDistance(position, face.polygon, true);
    if (faceDistance > padding) continue;
    if (!hit || faceDistance < hit->distance - 0.5 ||
        (std::abs(faceDistance - hit->distance) <= 0.5 && face.normalizedDepth < hit->depth))
      hit = HandleHit{faceDistance, face.normalizedDepth};
  }
  return hit;
}

TransformGizmoHandle closestSolidHandle(const TransformGizmoLayout &layout,
                                       const QPointF &position,
                                       const TransformGizmoHandleKind kind) {
  std::optional<HandleHit> best;
  int bestAxis = -1;
  const qreal overlapTolerance = std::min(0.5, layout.radius * 0.01);
  for (int axis = 0; axis < 3; ++axis) {
    const auto hit = solidHandleHit(layout.axes[static_cast<std::size_t>(axis)],
                                    position, kind);
    if (!hit) continue;
    if (!best || hit->distance < best->distance - overlapTolerance ||
        (std::abs(hit->distance - best->distance) <= overlapTolerance &&
         (hit->depth < best->depth ||
          (hit->depth == best->depth &&
           hit->distance < best->distance)))) {
      best = hit;
      bestAxis = axis;
    }
  }
  return bestAxis < 0 ? TransformGizmoHandle{} : TransformGizmoHandle{kind, bestAxis};
}

bool centerHandleContains(const TransformGizmoLayout &layout,
                           const QPointF &position, bool diamond = false) {
  const qreal radius = layout.centerHitRadius;
  const qreal paddedRadius = radius + std::min(2.0, radius * 0.25);
  const QPointF delta = position - layout.center;
  return diamond ? std::abs(delta.x()) + std::abs(delta.y()) <= paddedRadius
                 : std::hypot(delta.x(), delta.y()) <= paddedRadius;
}

// Clip individual world-space edges before perspective division. Near-camera
// rings can cross the eye/near plane; neither discard the whole ring nor join
// disconnected visible arcs with an artificial closing line.
std::optional<TransformGizmoDepthSegment>
projectSegment(const QVector3D &start, const QVector3D &end,
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
  return TransformGizmoDepthSegment{
      QLineF(screen(ca), screen(cb)), std::clamp(ca.z() / ca.w(), -1.0F, 1.0F),
      std::clamp(cb.z() / cb.w(), -1.0F, 1.0F)};
}

// The same homogeneous clipping as the ring strokes, applied to solid
// handle faces before dividing by W. In particular, a cone or box crossing
// the near plane keeps its visible portion rather than disappearing.
std::optional<TransformGizmoFace>
projectFace(const QList<QVector3D> &vertices, const QVector3D &normal,
            const QVector3D &lightDirection, const QMatrix4x4 &vp,
            const QMatrix4x4 &inverseVp, const QSizeF &size) {
  QList<QVector4D> clipped;
  clipped.reserve(vertices.size());
  for (const auto &vertex : vertices) clipped.append(vp * QVector4D(vertex, 1));
  const auto planeDistance = [](const QVector4D &point, int plane) {
    switch (plane) {
    case 0: return point.w() + point.x();
    case 1: return point.w() - point.x();
    case 2: return point.w() + point.y();
    case 3: return point.w() - point.y();
    case 4: return point.w() + point.z();
    default: return point.w() - point.z();
    }
  };
  for (int plane = 0; plane < 6 && !clipped.isEmpty(); ++plane) {
    QList<QVector4D> output;
    QVector4D previous = clipped.constLast();
    float previousDistance = planeDistance(previous, plane);
    for (const auto &point : clipped) {
      const float distance = planeDistance(point, plane);
      if (!std::isfinite(distance) || !std::isfinite(previousDistance))
        return std::nullopt;
      const bool inside = distance >= 0;
      if (inside != (previousDistance >= 0)) {
        const float amount = previousDistance / (previousDistance - distance);
        output.append(previous + (point - previous) * amount);
      }
      if (inside) output.append(point);
      previous = point;
      previousDistance = distance;
    }
    clipped = std::move(output);
  }
  if (clipped.size() < 3) return std::nullopt;
  TransformGizmoFace face;
  QPointF screenCenter;
  for (const auto &point : clipped) {
    if (!std::isfinite(point.w()) || point.w() <= 1.0e-5F)
      return std::nullopt;
    const QPointF screen((point.x() / point.w() + 1) * size.width() * 0.5,
                         (1 - point.y() / point.w()) * size.height() * 0.5);
    face.polygon.append(screen);
    screenCenter += screen;
    face.normalizedDepth += std::clamp(point.z() / point.w(), -1.0F, 1.0F);
  }
  if (polygonArea(face.polygon) < 1.0e-6) return std::nullopt;
  const float count = static_cast<float>(face.polygon.size());
  face.normalizedDepth /= count;
  screenCenter /= count;
  const auto nearPoint = unprojectPoint(screenCenter, -1, inverseVp, size);
  const auto farPoint = unprojectPoint(screenCenter, 1, inverseVp, size);
  if (!nearPoint || !farPoint ||
      QVector3D::dotProduct(normal, *nearPoint - *farPoint) <= 0)
    return std::nullopt;
  // Camera-relative upper-left headlight, shared by cones and boxes. A
  // substantial ambient term keeps even their unlit visible facets readable.
  face.light = std::clamp(0.45F + 0.55F *
      std::max(0.0F, QVector3D::dotProduct(normal, lightDirection)), 0.45F, 1.0F);
  return face;
}

void addSolidHandleFaces(TransformGizmoAxisLayout &axisLayout,
                        const QVector3D &pivot,
                        const QQuaternion &orientation, const int axis,
                        const float moveExtent, const float coneHeight,
                        const float coneRadius, const float scaleExtent,
                        const float cubeSize, const QVector3D &lightDirection,
                        const QMatrix4x4 &vp, const QMatrix4x4 &inverseVp,
                        const QSizeF &size) {
  const QVector3D direction = axisLayout.direction;
  const QVector3D first = orientation.rotatedVector(unitAxis((axis + 1) % 3));
  const QVector3D second = orientation.rotatedVector(unitAxis((axis + 2) % 3));
  const QVector3D tip = pivot + direction * moveExtent;
  const QVector3D baseCenter = tip - direction * coneHeight;
  constexpr int coneSegments = 12;
  QList<QVector3D> base;
  for (int segment = 0; segment < coneSegments; ++segment) {
    const float angle = static_cast<float>(segment * 2 * kPi / coneSegments);
    base.append(baseCenter + (first * std::cos(angle) +
                             second * std::sin(angle)) * coneRadius);
  }
  for (int segment = 0; segment < coneSegments; ++segment) {
    const auto &start = base[segment];
    const auto &end = base[(segment + 1) % coneSegments];
    const QVector3D normal = QVector3D::crossProduct(start - tip, end - tip).normalized();
    if (const auto face = projectFace({tip, start, end}, normal, lightDirection,
                                     vp, inverseVp, size))
      axisLayout.moveFaces.append(*face);
  }
  std::reverse(base.begin(), base.end());
  if (const auto face = projectFace(base, -direction, lightDirection,
                                   vp, inverseVp, size))
    axisLayout.moveFaces.append(*face);

  const QVector3D cubeCenter = pivot + direction * scaleExtent;
  const float half = cubeSize * 0.5F;
  for (int component = 0; component < 3; ++component) {
    const auto outward = orientation.rotatedVector(unitAxis(component));
    const auto u = orientation.rotatedVector(unitAxis((component + 1) % 3)) * half;
    const auto v = orientation.rotatedVector(unitAxis((component + 2) % 3)) * half;
    for (const float side : {-1.0F, 1.0F}) {
      const auto center = cubeCenter + outward * (side * half);
      QList<QVector3D> faceVertices{center - u - v, center + u - v,
                                   center + u + v, center - u + v};
      if (side < 0) std::reverse(faceVertices.begin(), faceVertices.end());
      if (const auto face = projectFace(faceVertices, outward * side,
                                       lightDirection, vp, inverseVp, size))
        axisLayout.scaleFaces.append(*face);
    }
  }
  const auto farToNear = [](const TransformGizmoFace &left,
                            const TransformGizmoFace &right) {
    return left.normalizedDepth > right.normalizedDepth;
  };
  std::stable_sort(axisLayout.moveFaces.begin(), axisLayout.moveFaces.end(), farToNear);
  std::stable_sort(axisLayout.scaleFaces.begin(), axisLayout.scaleFaces.end(), farToNear);
}

TransformGizmoHandle closestAxisLine(const TransformGizmoLayout &layout,
                                     const QPointF &position,
                                     const TransformGizmoHandleKind kind,
                                     const qreal tolerance) {
  qreal bestDistance = tolerance;
  float bestDepth = std::numeric_limits<float>::infinity();
  int bestAxis = -1;
  const qreal overlapTolerance = std::min(0.5, tolerance * 0.25);
  for (int axis = 0; axis < 3; ++axis) {
    const TransformGizmoAxisLayout &axisLayout =
        layout.axes[static_cast<std::size_t>(axis)];
    if (!axisLayout.visible) {
      continue;
    }
    const QLineF line = kind == TransformGizmoHandleKind::MoveAxis
                            ? axisLayout.moveLine
                            : axisLayout.scaleLine;
    if (line.isNull()) continue;
    const qreal distance = distanceToLineSegment(position, line);
    if (distance > tolerance) continue;
    const QPointF endpoint = kind == TransformGizmoHandleKind::MoveAxis
        ? axisLayout.moveEndpoint : axisLayout.scaleEndpoint;
    const float endpointDepth = kind == TransformGizmoHandleKind::MoveAxis
        ? axisLayout.moveEndpointDepth : axisLayout.scaleEndpointDepth;
    const QPointF direction = endpoint - layout.center;
    const qreal squaredLength = QPointF::dotProduct(direction, direction);
    const qreal amount = squaredLength > 1.0e-12
        ? std::clamp(QPointF::dotProduct(position - layout.center, direction) /
                     squaredLength, 0.0, 1.0) : 0.0;
    const float depth = layout.pivotDepth + (endpointDepth - layout.pivotDepth) *
        static_cast<float>(amount);
    if (bestAxis < 0 || distance < bestDistance - overlapTolerance ||
        (std::abs(distance - bestDistance) <= overlapTolerance &&
         (depth < bestDepth ||
          (depth == bestDepth && distance <= bestDistance)))) {
      bestDistance = distance;
      bestDepth = depth;
      bestAxis = axis;
    }
  }
  return bestAxis >= 0 ? TransformGizmoHandle{kind, bestAxis}
                       : TransformGizmoHandle{};
}

TransformGizmoHandle moveArrowAt(const TransformGizmoLayout &layout,
                                 const QPointF &position) {
  return closestSolidHandle(layout, position, TransformGizmoHandleKind::MoveAxis);
}

TransformGizmoHandle closestRotationRing(const TransformGizmoLayout &layout,
                                         const QPointF &position,
                                         const qreal tolerance) {
  const qreal centerDistance = QLineF(layout.center, position).length();
  if (centerDistance < layout.rotationHitInnerRadius) {
    return {};
  }
  qreal bestDistance = tolerance;
  float bestDepth = std::numeric_limits<float>::infinity();
  int bestAxis = -1;
  const qreal overlapTolerance = std::min(0.5, tolerance * 0.25);
  const auto consider = [&](const QLineF &line, const float startDepth,
                            const float endDepth, const int axis) {
    const qreal distance = distanceToLineSegment(position, line);
    if (distance > tolerance) return;
    const QPointF direction = line.p2() - line.p1();
    const qreal squaredLength = QPointF::dotProduct(direction, direction);
    const qreal amount = squaredLength > 1.0e-12
        ? std::clamp(QPointF::dotProduct(position - line.p1(), direction) /
                         squaredLength, 0.0, 1.0) : 0.0;
    const float depth = startDepth + (endDepth - startDepth) * static_cast<float>(amount);
    const bool equallyClose = std::abs(distance - bestDistance) <= overlapTolerance;
    if (bestAxis < 0 || distance < bestDistance - overlapTolerance ||
        (equallyClose && (depth < bestDepth ||
                         (depth == bestDepth &&
                          distance <= bestDistance)))) {
      bestDistance = distance;
      bestDepth = depth;
      bestAxis = axis;
    }
  };
  for (int axis = 0; axis < 3; ++axis) {
    const auto &axisLayout = layout.axes[static_cast<std::size_t>(axis)];
    if (!axisLayout.depthSegments.isEmpty()) {
      for (const auto &stroke : axisLayout.depthSegments)
        consider(stroke.line, stroke.startDepth, stroke.endDepth, axis);
    } else {
      for (const auto &stroke : axisLayout.rotationSegments)
        consider(stroke, layout.pivotDepth, layout.pivotDepth, axis);
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
  float bestDepth = std::numeric_limits<float>::infinity();
  int bestAxis = -1;
  for (int excludedAxis = 0; excludedAxis < 3; ++excludedAxis) {
    const QPolygonF &polygon =
        layout.planeHandles[static_cast<std::size_t>(excludedAxis)];
    if (polygon.size() >= 3 &&
        polygon.containsPoint(position, Qt::OddEvenFill) &&
        layout.planeDepths[static_cast<std::size_t>(excludedAxis)] < bestDepth) {
      bestDepth = layout.planeDepths[static_cast<std::size_t>(excludedAxis)];
      bestAxis = excludedAxis;
    }
  }
  return bestAxis < 0 ? TransformGizmoHandle{} : TransformGizmoHandle{kind, bestAxis};
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
  layout.pivotDepth = projectedCenter->normalizedDepth;
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
  const auto worldAtUp = unprojectPoint(
      layout.center - QPointF(0, viewportSize.height() * 0.5),
      projectedCenter->normalizedDepth, inverseViewProjection, viewportSize);
  const auto worldAtNear = unprojectPoint(layout.center, -1,
                                         inverseViewProjection, viewportSize);
  const auto worldAtFar = unprojectPoint(layout.center, 1,
                                        inverseViewProjection, viewportSize);
  if (!worldAtUp || !worldAtNear || !worldAtFar) return layout;
  const QVector3D up = (*worldAtUp - *worldAtCenter).normalized();
  const QVector3D towardCamera = (*worldAtNear - *worldAtFar).normalized();
  const QVector3D lightDirection =
      (-right * 0.35F + up * 0.55F + towardCamera * 0.75F).normalized();
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
    axisLayout.moveEndpointDepth = moveEnd->normalizedDepth;
    axisLayout.scaleEndpointDepth = scaleEnd->normalizedDepth;
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
    addSolidHandleFaces(axisLayout, pivot, normalizedOrientation, axis,
        static_cast<float>(worldRadius * moveExtent / layout.radius),
        static_cast<float>(worldRadius * (combined ? 12.0 / 148.0 : 11.0 / 84.0)),
        static_cast<float>(worldRadius * (combined ? 6.0 / 148.0 : 5.0 / 84.0)),
        static_cast<float>(worldRadius * scaleExtent / layout.radius),
        static_cast<float>(worldRadius * (combined ? 15.0 / 148.0 : 12.0 / 84.0)),
        lightDirection, viewProjection, inverseViewProjection, viewportSize);
  }

  for (int excludedAxis = 0; excludedAxis < 3; ++excludedAxis) {
    const int firstAxis = (excludedAxis + 1) % 3;
    const int secondAxis = (excludedAxis + 2) % 3;
    const QVector3D first = layout.axes[static_cast<std::size_t>(firstAxis)].direction;
    const QVector3D second = layout.axes[static_cast<std::size_t>(secondAxis)].direction;
    QPolygonF polygon;
    float depth = 0.0F;
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
      depth += point->normalizedDepth;
    }
    if (polygonArea(polygon) >= 1.0e-6) {
      layout.planeHandles[static_cast<std::size_t>(excludedAxis)] = polygon;
      layout.planeDepths[static_cast<std::size_t>(excludedAxis)] =
          depth / static_cast<float>(polygon.size());
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
        auto &axisLayout = layout.axes[static_cast<std::size_t>(axis)];
        axisLayout.rotationSegments.append(edge->line);
        axisLayout.depthSegments.append(*edge);
      }
      previous = point;
    }
    if (complete) {
      layout.axes[static_cast<std::size_t>(axis)].rotationRing = ring;
    }
  }

  layout.viewRing = screenCircle(layout.center, viewRingRadius);
  layout.centerHitRadius = (combined ? 12.0 : 10.0) * shapeScale;
  const qreal centerHandleSize = (combined ? 10.0 : 8.0) * shapeScale;
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
    if (const auto head = closestSolidHandle(layout, position,
                                            TransformGizmoHandleKind::ScaleAxis);
        head.isValid()) return head;
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
  if (!std::isfinite(viewportSize.width()) || !std::isfinite(viewportSize.height()) ||
      !std::isfinite(fontHeight) || viewportSize.width() <= 0 || viewportSize.height() <= 0)
    return layout;
  // A single compact tool rail, with the same logical-pixel rectangles used
  // for painting and hit testing. Keep scaling tied to the actual UI font.
  const qreal buttonSize = std::clamp(fontHeight * 1.90, 32.0, 60.0);
  const qreal gap = std::clamp(fontHeight * 0.11, 2.0, 4.0);
  const qreal padding = std::clamp(fontHeight * 0.22, 3.0, 6.0);
  const qreal sectionGap = std::clamp(fontHeight * 0.40, 6.0, 12.0);
  const qreal margin = std::clamp(fontHeight * 0.55, 8.0, 14.0);
  const qreal top = std::clamp(fontHeight * 4.2, 70.0, 104.0);
  const qreal outerMargin = std::max(0.0, margin - padding);
  const qreal stripLength = buttonSize * 5 + gap * 2 + sectionGap * 2 + padding * 2;
  const qreal stripBreadth = buttonSize + padding * 2;
  const auto setButton = [&layout](const int index, const QRectF &rect) {
    if (index < 4) layout.buttons[static_cast<std::size_t>(index)] = rect;
    else layout.orientationButton = rect;
  };
  const auto placeLinear = [&](const bool horizontal) {
    layout.horizontal = horizontal;
    const QSizeF extent(horizontal ? stripLength : stripBreadth,
                         horizontal ? stripBreadth : stripLength);
    const qreal backgroundTop = std::clamp(top - padding, outerMargin,
        std::max(outerMargin, viewportSize.height() - outerMargin - extent.height()));
    layout.background = QRectF(QPointF(outerMargin, backgroundTop), extent);
    for (int index = 0; index < 5; ++index) {
      const qreal advance = index * (buttonSize + gap) +
          (index >= 3 ? sectionGap - gap : 0) +
          (index == 4 ? sectionGap - gap : 0);
      const QPointF origin = layout.background.topLeft() +
          QPointF(padding + (horizontal ? advance : 0),
                   padding + (horizontal ? 0 : advance));
      setButton(index, QRectF(origin, QSizeF(buttonSize, buttonSize)));
    }
  };
  const QSizeF available = viewportSize - QSizeF(outerMargin * 2, outerMargin * 2);
  if (stripBreadth <= available.width() && stripLength <= available.height()) {
    placeLinear(false);
    return layout;
  }
  if (stripLength <= available.width() && stripBreadth <= available.height()) {
    placeLinear(true);
    return layout;
  }
  // A short, narrow viewport wraps the five targets. Try normal spacing first,
  // then compact spacing; preserve the 32 px target minimum even at the edge.
  // The final flush layout is used only when decorative spacing cannot fit.
  struct Spacing { qreal outer; qreal padding; qreal gap; };
  const std::array<Spacing, 3> spacings{{
      {outerMargin, padding, gap}, {2.0, 2.0, 2.0}, {0.0, 0.0, 0.0}}};
  for (const auto &spacing : spacings) {
    int bestColumns = 0;
    qreal bestSize = 0;
    for (int columns = 1; columns <= 5; ++columns) {
      const int rows = (5 + columns - 1) / columns;
      const qreal widthLimit = (viewportSize.width() - spacing.outer * 2 -
          spacing.padding * 2 - spacing.gap * (columns - 1)) / columns;
      const qreal heightLimit = (viewportSize.height() - spacing.outer * 2 -
          spacing.padding * 2 - spacing.gap * (rows - 1)) / rows;
      const qreal fittedSize = std::min({buttonSize, widthLimit, heightLimit});
      if (fittedSize >= 32 && fittedSize >= bestSize) {
        bestColumns = columns;
        bestSize = fittedSize;
      }
    }
    if (bestColumns == 0) continue;
    const int rows = (5 + bestColumns - 1) / bestColumns;
    const QSizeF extent(bestColumns * bestSize + (bestColumns - 1) * spacing.gap +
                            spacing.padding * 2,
                        rows * bestSize + (rows - 1) * spacing.gap + spacing.padding * 2);
    const qreal backgroundTop = std::clamp(top - spacing.padding, spacing.outer,
        std::max(spacing.outer, viewportSize.height() - spacing.outer - extent.height()));
    layout.horizontal = bestColumns > 1;
    layout.background = QRectF(QPointF(spacing.outer, backgroundTop), extent);
    for (int index = 0; index < 5; ++index) {
      const QPointF origin = layout.background.topLeft() + QPointF(
          spacing.padding + (index % bestColumns) * (bestSize + spacing.gap),
          spacing.padding + (index / bestColumns) * (bestSize + spacing.gap));
      setButton(index, QRectF(origin, QSizeF(bestSize, bestSize)));
    }
    return layout;
  }
  // If five 32 px targets cannot fit in any grid, omit the rail rather than
  // publishing clipped/inaccessible controls. Menus and shortcuts still exist.
  return layout;
}

int hitTestTransformToolStrip(const TransformToolStripLayout &layout,
                              const QPointF &position) {
  if (layout.background.isEmpty()) return -1;
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
