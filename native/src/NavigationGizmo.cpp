#include "NavigationGizmo.h"

#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gsw {

namespace {
constexpr qreal kHandleSizeRatio = 0.20;
constexpr qreal kAxisExtentRatio = 0.80;

NavigationAxis axisDirection(const int axisIndex, const bool positive) {
  static constexpr std::array<NavigationAxis, 6> directions = {
      NavigationAxis::NegativeX, NavigationAxis::PositiveX,
      NavigationAxis::NegativeY, NavigationAxis::PositiveY,
      NavigationAxis::NegativeZ, NavigationAxis::PositiveZ,
  };
  return directions[static_cast<std::size_t>(axisIndex * 2 +
                                             (positive ? 1 : 0))];
}

QVector3D worldAxis(const int axisIndex) {
  if (axisIndex == 0) {
    return QVector3D(1.0F, 0.0F, 0.0F);
  }
  if (axisIndex == 1) {
    return QVector3D(0.0F, 1.0F, 0.0F);
  }
  return QVector3D(0.0F, 0.0F, 1.0F);
}

QRectF buttonRect(const qreal right, const qreal centerY,
                  const qreal buttonSize) {
  return QRectF(right - buttonSize, centerY - buttonSize * 0.5, buttonSize,
                buttonSize);
}
} // namespace

NavigationGizmoLayout navigationGizmoLayout(const QMatrix4x4 &viewMatrix,
                                            const QSizeF &viewportSize,
                                            const qreal fontHeight) {
  NavigationGizmoLayout layout;
  layout.radius = std::clamp(fontHeight * 2.65, 44.0, 68.0);
  const qreal margin = std::clamp(fontHeight * 0.65, 10.0, 18.0);
  qreal buttonSize = std::clamp(fontHeight * 1.7, 28.0, 44.0);
  qreal buttonGap = std::clamp(fontHeight * 0.25, 4.0, 8.0);
  const qreal requiredHeight =
      layout.radius * 2.0 + buttonSize * 4.0 + buttonGap * 4.0;
  const qreal availableHeight =
      std::max(1.0, viewportSize.height() - margin * 2.0);
  if (requiredHeight > availableHeight) {
    const qreal fit = availableHeight / requiredHeight;
    layout.radius *= fit;
    buttonSize *= fit;
    buttonGap *= fit;
  }
  layout.lineWidth = std::clamp(layout.radius / 20.0, 2.0, 3.4);
  layout.center = QPointF(viewportSize.width() - margin - layout.radius,
                          viewportSize.height() - margin - layout.radius);
  layout.rotateBounds = QRectF(layout.center.x() - layout.radius,
                               layout.center.y() - layout.radius,
                               layout.radius * 2.0, layout.radius * 2.0);

  const qreal buttonRight = viewportSize.width() - margin;
  qreal buttonCenterY =
      layout.center.y() - layout.radius - buttonGap - buttonSize * 0.5;
  layout.zoomButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  buttonCenterY -= buttonSize + buttonGap;
  layout.panButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  buttonCenterY -= buttonSize + buttonGap;
  layout.cameraButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  buttonCenterY -= buttonSize + buttonGap;
  layout.projectionButton = buttonRect(buttonRight, buttonCenterY, buttonSize);

  const qreal handleBaseRadius = layout.radius * kHandleSizeRatio;
  const qreal axisExtent = layout.radius * kAxisExtentRatio;
  int handleIndex = 0;
  for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
    const QVector3D viewAxis =
        (viewMatrix * QVector4D(worldAxis(axisIndex), 0.0F)).toVector3D();
    const qreal projectedLength = std::hypot(static_cast<qreal>(viewAxis.x()),
                                             static_cast<qreal>(viewAxis.y()));
    for (const bool positive : {false, true}) {
      const float sign = positive ? 1.0F : -1.0F;
      NavigationAxisHandle &handle =
          layout.handles[static_cast<std::size_t>(handleIndex++)];
      handle.axis = axisDirection(axisIndex, positive);
      handle.axisIndex = axisIndex;
      handle.positive = positive;
      handle.depth = viewAxis.z() * sign;
      handle.center =
          layout.center + QPointF(viewAxis.x() * sign * axisExtent,
                                  -viewAxis.y() * sign * axisExtent);
      const qreal depthScale =
          (static_cast<qreal>(handle.depth) + 1.0) * 0.08 + 0.92;
      handle.radius = handleBaseRadius * depthScale;
      handle.hidden = projectedLength < 1.0e-4 && handle.depth < 0.0F;
    }
  }
  return layout;
}

NavigationGizmoHit hitTestNavigationGizmo(const NavigationGizmoLayout &layout,
                                          const QPointF &position) {
  if (layout.zoomButton.contains(position)) {
    return {NavigationGizmoPart::Zoom, NavigationAxis::None};
  }
  if (layout.panButton.contains(position)) {
    return {NavigationGizmoPart::Pan, NavigationAxis::None};
  }
  if (layout.cameraButton.contains(position)) {
    return {NavigationGizmoPart::Camera, NavigationAxis::None};
  }
  if (layout.projectionButton.contains(position)) {
    return {NavigationGizmoPart::Projection, NavigationAxis::None};
  }

  const QPointF centerDelta = position - layout.center;
  if (QPointF::dotProduct(centerDelta, centerDelta) >
      layout.radius * layout.radius) {
    return {};
  }

  qreal bestDistanceSquared = std::numeric_limits<qreal>::max();
  NavigationAxis bestAxis = NavigationAxis::None;
  for (const NavigationAxisHandle &handle : layout.handles) {
    if (handle.hidden) {
      continue;
    }
    const QPointF delta = position - handle.center;
    const qreal distanceSquared = QPointF::dotProduct(delta, delta);
    if (distanceSquared < bestDistanceSquared) {
      bestDistanceSquared = distanceSquared;
      bestAxis = handle.axis;
    }
  }
  return {NavigationGizmoPart::Rotate, bestAxis};
}

std::optional<OrbitAngles> navigationAxisViewAngles(const NavigationAxis axis) {
  switch (axis) {
  case NavigationAxis::PositiveX:
    return OrbitAngles{90.0F, 0.0F};
  case NavigationAxis::NegativeX:
    return OrbitAngles{-90.0F, 0.0F};
  case NavigationAxis::PositiveY:
    return OrbitAngles{0.0F, 0.0F};
  case NavigationAxis::NegativeY:
    return OrbitAngles{180.0F, 0.0F};
  case NavigationAxis::PositiveZ:
    return OrbitAngles{0.0F, 90.0F};
  case NavigationAxis::NegativeZ:
    return OrbitAngles{0.0F, -90.0F};
  case NavigationAxis::None:
    break;
  }
  return std::nullopt;
}

} // namespace gsw
