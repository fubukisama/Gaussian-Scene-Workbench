#include "TransformGizmoRenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QLinearGradient>

#include <algorithm>
#include <cmath>

namespace gsw {
namespace {

QColor alpha(QColor color, const int opacity) {
  color.setAlpha(std::clamp(opacity, 0, 255));
  return color;
}

struct RingStroke final {
  QLineF line;
  float depth = 0;
  int axis = 0;
  bool front = true;
};

QList<RingStroke> ringStrokes(const TransformGizmoLayout &layout) {
  QList<RingStroke> strokes;
  const auto append = [&](const QLineF &line, const float first, const float last,
                          const int axis) {
    const float depth = (first + last) * 0.5F;
    strokes.append({line, depth, axis, depth <= layout.pivotDepth});
  };
  for (int axis = 0; axis < 3; ++axis) {
    const auto &geometry = layout.axes[static_cast<std::size_t>(axis)];
    if (geometry.depthSegments.isEmpty()) {
      for (const auto &line : geometry.rotationSegments)
        append(line, layout.pivotDepth, layout.pivotDepth, axis);
      continue;
    }
    for (const auto &segment : geometry.depthSegments) {
      const float first = segment.startDepth - layout.pivotDepth;
      const float last = segment.endDepth - layout.pivotDepth;
      if (first * last < 0) {
        // NDC depth is linear on a projected edge. Split a depth-plane crossing
        // instead of assigning a whole edge to the wrong hemisphere.
        const qreal amount = first / (first - last);
        const QPointF crossing = segment.line.pointAt(amount);
        append(QLineF(segment.line.p1(), crossing), segment.startDepth,
               layout.pivotDepth, axis);
        append(QLineF(crossing, segment.line.p2()), layout.pivotDepth,
               segment.endDepth, axis);
      } else {
        append(segment.line, segment.startDepth, segment.endDepth, axis);
      }
    }
  }
  return strokes;
}

enum class PrimitiveKind { Ring, Shaft, Face, Plane };

struct Primitive final {
  PrimitiveKind type = PrimitiveKind::Ring;
  QLineF line;
  QPolygonF polygon;
  float depth = 0;
  float light = 1;
  TransformGizmoHandle handle;
  bool front = true;
};

QList<Primitive> primitives(const TransformGizmoLayout &layout, const TransformGizmoMode mode) {
  QList<Primitive> result;
  const bool move = mode == TransformGizmoMode::Move || mode == TransformGizmoMode::Transform;
  const bool scale = mode == TransformGizmoMode::Scale || mode == TransformGizmoMode::Transform;
  if (mode == TransformGizmoMode::Rotate || mode == TransformGizmoMode::Transform) {
    for (const auto &stroke : ringStrokes(layout))
      result.append({PrimitiveKind::Ring, stroke.line, {}, stroke.depth, 1,
                      {TransformGizmoHandleKind::RotateAxis, stroke.axis}, stroke.front});
  }
  for (int axis = 0; axis < 3; ++axis) {
    const auto &geometry = layout.axes[static_cast<std::size_t>(axis)];
    const auto addAxis = [&](const bool enabled, const TransformGizmoHandleKind kind,
                             const QLineF &line, const float endpointDepth,
                             const QList<TransformGizmoFace> &faces, const QPolygonF &fallback) {
      if (!enabled || !geometry.visible) return;
      result.append({PrimitiveKind::Shaft, line, {}, (endpointDepth + layout.pivotDepth) * 0.5F,
                      1, {kind, axis}});
      if (faces.isEmpty()) {
        if (!fallback.isEmpty())
          result.append({PrimitiveKind::Face, {}, fallback, endpointDepth, 1, {kind, axis}});
      } else {
        for (const auto &face : faces)
          result.append({PrimitiveKind::Face, {}, face.polygon, face.normalizedDepth,
                          face.light, {kind, axis}});
      }
    };
    addAxis(move, TransformGizmoHandleKind::MoveAxis, geometry.moveLine,
             geometry.moveEndpointDepth, geometry.moveFaces, geometry.moveArrow);
    // A multi-object selection intentionally has no per-axis scaling. The
    // viewport disables it by clearing the shared scale target, not its faces.
    addAxis(scale && !geometry.scaleHandle.isEmpty(), TransformGizmoHandleKind::ScaleAxis,
             geometry.scaleLine, geometry.scaleEndpointDepth, geometry.scaleFaces,
             QPolygonF(geometry.scaleHandle));
    if ((move || scale) && layout.planeHandles[static_cast<std::size_t>(axis)].size() >= 3)
      result.append({PrimitiveKind::Plane, {}, layout.planeHandles[static_cast<std::size_t>(axis)],
                      layout.planeDepths[static_cast<std::size_t>(axis)], 1,
                      {move ? TransformGizmoHandleKind::MovePlane : TransformGizmoHandleKind::ScalePlane, axis}});
  }
  std::stable_sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
    return left.depth > right.depth;
  });
  return result;
}

QColor lit(QColor base, const float intensity, const int opacity) {
  const qreal amount = std::clamp(static_cast<qreal>(intensity), 0.3, 1.0);
  return QColor::fromRgbF(base.redF() * amount, base.greenF() * amount,
                          base.blueF() * amount, opacity / 255.0);
}

void paintRingUnderlay(QPainter &painter, const TransformGizmoLayout &layout,
                        const TransformGizmoMode mode, const TransformGizmoStyle &style,
                        const TransformGizmoHandle highlighted, const bool dragging) {
  if (mode != TransformGizmoMode::Rotate && mode != TransformGizmoMode::Transform) return;
  const auto strokes = ringStrokes(layout); // Natural connected order, not paint-depth order.
  for (int axis = 0; axis < 3; ++axis) {
    QPainterPath path;
    bool connected = false;
    QPointF previous;
    for (const auto &stroke : strokes) {
      if (stroke.axis != axis) continue;
      const bool context = mode == TransformGizmoMode::Transform &&
          QLineF(layout.center, stroke.line.center()).length() < layout.rotationHitInnerRadius;
      if (!stroke.front || context) { connected = false; continue; }
      if (!connected || QLineF(previous, stroke.line.p1()).length() > 0.001)
        path.moveTo(stroke.line.p1());
      path.lineTo(stroke.line.p2());
      previous = stroke.line.p2();
      connected = true;
    }
    const bool active = highlighted == TransformGizmoHandle{TransformGizmoHandleKind::RotateAxis, axis};
    const bool subdued = dragging && highlighted.isValid() && !active;
    const qreal width = layout.lineWidth + (active ? 0.8 : 0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(8, 12, 17, subdued ? 25 : (style.light ? 100 : 170)),
                        width + 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
  }
}

void paintRing(QPainter &painter, const Primitive &stroke,
                const TransformGizmoLayout &layout, const TransformGizmoMode mode,
                const TransformGizmoStyle &style, const bool active, const bool subdued) {
  const bool context = mode == TransformGizmoMode::Transform &&
      QLineF(layout.center, stroke.line.center()).length() < layout.rotationHitInnerRadius;
  const int opacity = active ? (stroke.front ? 255 : 155) :
      context ? (stroke.front ? 70 : 30) :
      subdued ? (stroke.front ? 65 : 30) :
      stroke.front ? 255 : (style.light ? 64 : 82);
  const qreal width = stroke.front ? layout.lineWidth + (active ? 0.8 : 0) :
                                      std::max(0.8, layout.lineWidth * 0.55);
  const QColor color = alpha(active ? style.highlight : style.axes[static_cast<std::size_t>(stroke.handle.axis)],
                              opacity);
  painter.setBrush(Qt::NoBrush);
  // The halo is one continuous underlay. Per-edge halos painted after adjacent
  // edges can cut tiny dark teeth into otherwise smooth rotation arcs.
  painter.setPen(QPen(color, width, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
  painter.drawLine(stroke.line);
}

} // namespace

void paintTransformGizmo(QPainter &painter, const TransformGizmoLayout &layout,
                         const TransformGizmoMode mode, const TransformGizmoStyle &style,
                         const TransformGizmoHandle highlighted, const bool dragging) {
  if (!layout.valid) return;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (mode == TransformGizmoMode::Rotate || mode == TransformGizmoMode::Transform) {
    const bool outerActive = highlighted.kind == TransformGizmoHandleKind::RotateView;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(alpha(outerActive ? style.highlight : style.neutral,
                              outerActive ? 245 : (style.light ? 110 : 125)),
                        outerActive ? 2.5 : 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolygon(layout.viewRing);
    if (highlighted.kind == TransformGizmoHandleKind::RotateTrackball) {
      QPainterPath trackball;
      trackball.setFillRule(Qt::OddEvenFill);
      trackball.addEllipse(layout.trackballBounds);
      trackball.addEllipse(QRectF(layout.center - QPointF(layout.trackballInnerRadius, layout.trackballInnerRadius),
                                   QSizeF(layout.trackballInnerRadius * 2, layout.trackballInnerRadius * 2)));
      painter.setPen(QPen(alpha(style.highlight, 165), 1.0, Qt::DashLine));
      painter.setBrush(alpha(style.highlight, 18));
      painter.drawPath(trackball);
    }
  }
  paintRingUnderlay(painter, layout, mode, style, highlighted, dragging);
  for (const auto &primitive : primitives(layout, mode)) {
    const bool active = highlighted == primitive.handle;
    const bool subdued = dragging && highlighted.isValid() && !active;
    const QColor base = active ? style.highlight : style.axes[static_cast<std::size_t>(primitive.handle.axis)];
    if (primitive.type == PrimitiveKind::Ring) {
      paintRing(painter, primitive, layout, mode, style, active, subdued);
    } else if (primitive.type == PrimitiveKind::Face) {
      const int opacity = subdued ? 100 : 255;
      painter.setPen(QPen(alpha(base.darker(170), subdued ? 45 : 130),
                          primitive.handle.kind == TransformGizmoHandleKind::MoveAxis ? 0.45 : 0.8,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.setBrush(lit(base, primitive.light, opacity));
      painter.drawPolygon(primitive.polygon);
    } else if (primitive.type == PrimitiveKind::Shaft) {
      const qreal width = primitive.handle.kind == TransformGizmoHandleKind::MoveAxis ?
                              layout.lineWidth : std::max(1.5, layout.lineWidth * 0.7);
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor(8, 12, 17, subdued ? 25 : 90), width + 1,
                          Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(primitive.line);
      QLinearGradient shade(primitive.line.p1(), primitive.line.p2());
      shade.setColorAt(0, alpha(base.darker(130), subdued ? 80 : 245));
      shade.setColorAt(1, alpha(base.lighter(108), subdued ? 80 : 255));
      painter.setPen(QPen(QBrush(shade), width + (active ? 0.7 : 0), Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(primitive.line);
    } else {
      const int excluded = primitive.handle.axis;
      const QColor first = style.axes[static_cast<std::size_t>((excluded + 1) % 3)];
      const QColor second = style.axes[static_cast<std::size_t>((excluded + 2) % 3)];
      const QColor tint = active ? style.highlight :
          QColor((first.red() + second.red()) / 2, (first.green() + second.green()) / 2,
                  (first.blue() + second.blue()) / 2);
      painter.setPen(QPen(alpha(tint, active ? 240 : subdued ? 65 : 170), active ? 1.7 : 1));
      painter.setBrush(alpha(tint, active ? 65 : subdued ? 10 : 26));
      painter.drawPolygon(primitive.polygon);
    }
  }
  if (mode == TransformGizmoMode::Move || mode == TransformGizmoMode::Transform) {
    const bool active = highlighted.kind == TransformGizmoHandleKind::MoveView;
    painter.setPen(QPen(active ? style.highlight : style.centerEdge,
                        active ? 2.2 : 1.5, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(layout.centerHandle);
  } else if (mode == TransformGizmoMode::Scale) {
    const qreal radius = layout.centerHandle.width() * 0.5;
    QPolygonF diamond;
    diamond << layout.center + QPointF(0, -radius) << layout.center + QPointF(radius, 0)
            << layout.center + QPointF(0, radius) << layout.center + QPointF(-radius, 0);
    const bool active = highlighted.kind == TransformGizmoHandleKind::ScaleUniform;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(active ? style.highlight : style.centerEdge, active ? 2.2 : 1.5,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolygon(diamond);
  }
  painter.restore();
}

} // namespace gsw
