/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Axis projection, endpoint scaling and aligned front/back handling adapted
 * from Blender v4.5.0, commit 8cb6b388974a817afedf1317ce26f0c75aa5f181,
 * source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc,
 * gizmo_axis_draw(), lines 53-287; aligned-axis reversal follows
 * gizmo_axis_test_select(), lines 289-340. Qt supplies layout and hit testing.
 * Picking follows Blender's nearest projected axis over the whole unit circle;
 * GSW's input handler distinguishes axis clicks from orbit drags.
 */
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
// Blender's circular axis endpoints leave room for the complete round handle
// inside the rotation sphere, rather than ending at the old cone's tip.
constexpr qreal kAxisExtentRatio = 0.80;
constexpr qreal kAlignedAxisProjectionSquared = 1.0e-6;

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

bool ellipseContains(const QRectF &bounds, const QPointF &position) {
  if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
    return false;
  }
  const QPointF delta = position - bounds.center();
  const qreal normalizedX = delta.x() / (bounds.width() * 0.5);
  const qreal normalizedY = delta.y() / (bounds.height() * 0.5);
  return normalizedX * normalizedX + normalizedY * normalizedY <= 1.0;
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
  qreal projectionLabelHeight = std::clamp(fontHeight * 1.25, 20.0, 30.0);
  qreal projectionLabelGap = std::clamp(fontHeight * 0.20, 3.0, 6.0);
  const qreal requiredHeight = layout.radius * 2.0 + projectionLabelHeight +
                               projectionLabelGap + buttonSize * 4.0 +
                               buttonGap * 4.0;
  const qreal availableHeight =
      std::max(1.0, viewportSize.height() - margin * 2.0);
  const qreal availableWidth =
      std::max(1.0, viewportSize.width() - margin * 2.0);
  const qreal fit = std::min({1.0, availableHeight / requiredHeight,
                             availableWidth / (layout.radius * 2.0)});
  if (fit < 1.0) {
    layout.radius *= fit;
    buttonSize *= fit;
    buttonGap *= fit;
    projectionLabelHeight *= fit;
    projectionLabelGap *= fit;
  }
  layout.lineWidth = std::clamp(layout.radius / 20.0, 2.0, 3.4);
  layout.center =
      QPointF(viewportSize.width() - margin - layout.radius,
              margin + layout.radius);
  layout.rotateBounds = QRectF(layout.center.x() - layout.radius,
                               layout.center.y() - layout.radius,
                               layout.radius * 2.0, layout.radius * 2.0);

  const qreal buttonRight = viewportSize.width() - margin;
  qreal buttonCenterY =
      layout.center.y() + layout.radius + projectionLabelGap +
          projectionLabelHeight + buttonGap + buttonSize * 0.5;
  layout.zoomButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  buttonCenterY += buttonSize + buttonGap;
  layout.panButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  buttonCenterY += buttonSize + buttonGap;
  layout.cameraButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  buttonCenterY += buttonSize + buttonGap;
  layout.projectionButton = buttonRect(buttonRight, buttonCenterY, buttonSize);
  const qreal projectionLabelWidth =
      std::min(availableWidth, std::max(layout.radius * 1.35, fontHeight * 3.0 * fit));
  layout.projectionLabel =
      QRectF(layout.center.x() - projectionLabelWidth * 0.5,
             layout.center.y() + layout.radius + projectionLabelGap,
             projectionLabelWidth, projectionLabelHeight);

  const qreal handleBaseRadius = layout.radius * kHandleSizeRatio;
  const qreal axisExtent = layout.radius * kAxisExtentRatio;
  int handleIndex = 0;
  for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
    const QVector3D viewAxis =
        (viewMatrix * QVector4D(worldAxis(axisIndex), 0.0F)).toVector3D();
    const qreal projectedLengthSquared =
        static_cast<qreal>(viewAxis.x()) * viewAxis.x() +
        static_cast<qreal>(viewAxis.y()) * viewAxis.y();
    for (const bool positive : {false, true}) {
      const float sign = positive ? 1.0F : -1.0F;
      NavigationAxisHandle &handle =
          layout.handles[static_cast<std::size_t>(handleIndex++)];
      handle.axis = axisDirection(axisIndex, positive);
      handle.axisIndex = axisIndex;
      handle.positive = positive;
      handle.depth = viewAxis.z() * sign;
      const qreal depthScale =
          (static_cast<qreal>(handle.depth) + 1.0) * 0.08 + 0.92;
      handle.radius = handleBaseRadius * depthScale;
      handle.aligned = projectedLengthSquared < kAlignedAxisProjectionSquared;
      handle.hidden = handle.aligned && handle.depth < 0.0F;
      handle.center = layout.center +
          QPointF(viewAxis.x(), -viewAxis.y()) * static_cast<qreal>(sign) * axisExtent;
    }
  }
  return layout;
}

NavigationGizmoHit hitTestNavigationGizmo(const NavigationGizmoLayout &layout,
                                          const QPointF &position) {
  if (ellipseContains(layout.projectionButton, position)) {
    return {NavigationGizmoPart::Projection, NavigationAxis::None};
  }
  if (ellipseContains(layout.zoomButton, position)) {
    return {NavigationGizmoPart::Zoom, NavigationAxis::None};
  }
  if (ellipseContains(layout.panButton, position)) {
    return {NavigationGizmoPart::Pan, NavigationAxis::None};
  }
  if (ellipseContains(layout.cameraButton, position)) {
    return {NavigationGizmoPart::Camera, NavigationAxis::None};
  }
  const QPointF centerDelta = position - layout.center;
  if (QPointF::dotProduct(centerDelta, centerDelta) >
      layout.radius * layout.radius) {
    return {};
  }

  if (layout.radius <= 0.0) return {};
  const QPointF normalizedPosition = centerDelta / layout.radius;
  qreal bestDistanceSquared = std::numeric_limits<qreal>::max();
  NavigationAxis bestAxis = NavigationAxis::None;
  for (const NavigationAxisHandle &handle : layout.handles) {
    if (handle.radius <= 0.0) continue;
    // Blender excludes the currently facing endpoint only when the view is
    // axis-aligned. The hidden far endpoint remains selectable to flip sides.
    if (handle.aligned && !handle.hidden) continue;
    // Drawing insets endpoints to 0.8 of the sphere radius; selection uses the
    // raw projected unit axis, as gizmo_axis_test_select() does upstream.
    const QPointF projectedAxis =
        (handle.center - layout.center) / (layout.radius * kAxisExtentRatio);
    const QPointF fromCenter = normalizedPosition - projectedAxis;
    const qreal distanceSquared = QPointF::dotProduct(fromCenter, fromCenter);
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
