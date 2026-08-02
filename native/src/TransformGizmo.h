#pragma once

#include <QLineF>
#include <QMatrix4x4>
#include <QPointF>
#include <QPolygonF>
#include <QQuaternion>
#include <QRectF>
#include <QSizeF>
#include <QVector3D>

#include <array>

namespace gsw {

enum class TransformGizmoMode { Move, Rotate, Scale, Transform };

[[nodiscard]] bool
transformOrientationLocked(TransformGizmoMode mode);

[[nodiscard]] bool
transformUsesLocalOrientation(TransformGizmoMode mode,
                              bool localPreference);

enum class TransformGizmoHandleKind {
  None,
  MoveAxis,
  MovePlane,
  MoveView,
  RotateAxis,
  RotateView,
  RotateTrackball,
  ScaleAxis,
  ScalePlane,
  ScaleUniform
};

struct TransformGizmoHandle final {
  TransformGizmoHandleKind kind = TransformGizmoHandleKind::None;
  int axis = -1;

  [[nodiscard]] bool isValid() const {
    return kind != TransformGizmoHandleKind::None;
  }

  friend bool operator==(const TransformGizmoHandle &left,
                         const TransformGizmoHandle &right) = default;
};

struct TransformGizmoAxisLayout final {
  QVector3D direction;
  QLineF moveLine;
  QLineF scaleLine;
  QPointF moveEndpoint;
  QPointF scaleEndpoint;
  QPolygonF moveArrow;
  QRectF scaleHandle;
  QPolygonF rotationRing;
  bool visible = false;
};

struct TransformGizmoLayout final {
  bool valid = false;
  QPointF center;
  qreal radius = 84.0;
  qreal lineWidth = 2.2;
  qreal rotationHitInnerRadius = 0.0;
  qreal trackballInnerRadius = 14.0;
  qreal trackballOuterRadius = 0.0;
  std::array<TransformGizmoAxisLayout, 3> axes;
  std::array<QPolygonF, 3> planeHandles;
  QPolygonF viewRing;
  QRectF centerHandle;
  QRectF trackballBounds;
};

struct TransformToolStripLayout final {
  QRectF background;
  std::array<QRectF, 4> buttons;
  QRectF orientationButton;
};

[[nodiscard]] TransformGizmoLayout
transformGizmoLayout(const QVector3D &pivot, const QQuaternion &orientation,
                     const QMatrix4x4 &viewProjection,
                     const QSizeF &viewportSize,
                     TransformGizmoMode mode = TransformGizmoMode::Move,
                     qreal radiusPixels = 84.0);

[[nodiscard]] TransformGizmoHandle
hitTestTransformGizmo(const TransformGizmoLayout &layout,
                      const QPointF &position, TransformGizmoMode mode);

[[nodiscard]] qreal pointPolylineDistance(const QPointF &point,
                                          const QPolygonF &polyline,
                                          bool closed);

[[nodiscard]] TransformToolStripLayout
transformToolStripLayout(const QSizeF &viewportSize, qreal fontHeight);

[[nodiscard]] int
hitTestTransformToolStrip(const TransformToolStripLayout &layout,
                          const QPointF &position);

[[nodiscard]] QRectF
transformGizmoHintRect(const TransformGizmoLayout &layout,
                       const QSizeF &hintSize, const QSizeF &viewportSize,
                       qreal gap = 12.0, qreal margin = 8.0);

} // namespace gsw
