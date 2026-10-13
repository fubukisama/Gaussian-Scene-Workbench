#pragma once

#include "TransformGizmo.h"

#include <QColor>

class QPainter;

namespace gsw {

// Presentation only: geometry, hit targets and transform semantics are supplied
// by the same layout used by the viewport's interaction code.
struct TransformGizmoStyle final {
  std::array<QColor, 3> axes{QColor(191, 26, 42), QColor(27, 74, 184),
                            QColor(18, 132, 53)};
  QColor highlight{168, 108, 14};
  QColor neutral{48, 55, 64};
  QColor centerEdge{38, 43, 50};
  bool light = true;
};

void paintTransformGizmo(QPainter &painter, const TransformGizmoLayout &layout,
                         TransformGizmoMode mode, const TransformGizmoStyle &style,
                         TransformGizmoHandle highlighted = {}, bool dragging = false);

} // namespace gsw
