#pragma once

#include "ViewportCamera.h"

#include <QMatrix4x4>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include <array>
#include <optional>

namespace gsw {

enum class NavigationAxis {
  None,
  PositiveX,
  NegativeX,
  PositiveY,
  NegativeY,
  PositiveZ,
  NegativeZ,
};

enum class NavigationGizmoPart {
  None,
  Rotate,
  Zoom,
  Pan,
  Camera,
  Projection,
};

struct NavigationAxisHandle {
  NavigationAxis axis = NavigationAxis::None;
  QPointF center;
  qreal radius = 0.0;
  float depth = 0.0F;
  int axisIndex = 0;
  bool positive = false;
  bool hidden = false;
};

struct NavigationGizmoLayout {
  QPointF center;
  qreal radius = 0.0;
  qreal lineWidth = 0.0;
  QRectF rotateBounds;
  QRectF zoomButton;
  QRectF panButton;
  QRectF cameraButton;
  QRectF projectionButton;
  std::array<NavigationAxisHandle, 6> handles;
};

struct NavigationGizmoHit {
  NavigationGizmoPart part = NavigationGizmoPart::None;
  NavigationAxis axis = NavigationAxis::None;

  [[nodiscard]] bool operator==(const NavigationGizmoHit &) const = default;
};

[[nodiscard]] NavigationGizmoLayout
navigationGizmoLayout(const QMatrix4x4 &viewMatrix, const QSizeF &viewportSize,
                      qreal fontHeight);

[[nodiscard]] NavigationGizmoHit
hitTestNavigationGizmo(const NavigationGizmoLayout &layout,
                       const QPointF &position);

[[nodiscard]] std::optional<OrbitAngles>
navigationAxisViewAngles(NavigationAxis axis);

} // namespace gsw
