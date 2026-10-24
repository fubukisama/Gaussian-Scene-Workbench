/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Navigation drawing adapted from Blender v4.5.0, commit
 * 8cb6b388974a817afedf1317ce26f0c75aa5f181:
 * source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc,
 * gizmo_axis_draw(), lines 53-287. SHA256:
 * 6c1e71395e46bd8dbf6da5bced22f0bdfe3e68a460a74a8207e1be0f242604eb.
 * source/blender/editors/space_view3d/view3d_gizmo_navigate.cc,
 * WIDGETGROUP_navigate_setup(), lines 163-209. SHA256:
 * 2cac9cfc0ac73f31a26f8ff31c362a84f8550778784a72e313fd7ff4a5da570e.
 *
 * Ported subset: depth ordering, background/axis color interpolation,
 * positive/negative circular axis handles, upright labels and hover backdrop.
 * Qt replaces Blender's GPU immediate drawing and font API. Axis projection
 * and aligned-front visibility follow the source. Axis picking uses Blender's
 * nearest projected endpoint; GSW distinguishes clicks from orbit drags.
 * Projection is a separate mini
 * button below the camera button, matching Blender's navigation placement.
 * Mini buttons retain GSW's shared semantic artwork. No coordinate or model
 * selection state is changed by this drawing adapter.
 * GSW selects GPL-3.0-or-later for the combined desktop; see COPYING.
 */
#pragma once

#include "NavigationGizmo.h"
#include "ViewportPresentation.h"
#include "WorkbenchIcons.h"

#include <QFontMetricsF>
#include <QCoreApplication>
#include <QLinearGradient>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>

namespace gsw {

struct BlenderNavigationPresentationState final {
  NavigationGizmoHit hover;
  NavigationGizmoHit press;
  bool interactionActive = false;
  bool light = true;
  bool orthographic = false;
  bool cameraActive = false;
  bool cameraAvailable = true;
  QColor backdrop;
};

namespace blender_navigation_detail {

inline QColor mix(const QColor &background, const QColor &foreground,
                   const qreal amount) {
  const qreal weight = std::clamp(amount, 0.0, 1.0);
  return QColor::fromRgbF(background.redF() * (1.0 - weight) + foreground.redF() * weight,
                           background.greenF() * (1.0 - weight) + foreground.greenF() * weight,
                           background.blueF() * (1.0 - weight) + foreground.blueF() * weight,
                           background.alphaF() * (1.0 - weight) + foreground.alphaF() * weight);
}

inline bool active(const BlenderNavigationPresentationState &state,
                    const NavigationGizmoPart part) {
  return state.hover.part == part ||
      (state.interactionActive && state.press.part == part);
}

inline bool highlighted(const BlenderNavigationPresentationState &state,
                         const NavigationAxisHandle &handle) {
  const auto matches = [&handle](const NavigationAxis axis) {
    if (axis == handle.axis) return true;
    if (!handle.aligned || axis == NavigationAxis::None) return false;
    // Aligned-center picking selects the opposite view; highlight the visible
    // near disc, as Blender's aligned-front/back mapping does.
    return (static_cast<int>(axis) - 1) / 2 == handle.axisIndex;
  };
  return (state.hover.part == NavigationGizmoPart::Rotate && matches(state.hover.axis)) ||
      (state.interactionActive && state.press.part == NavigationGizmoPart::Rotate &&
       matches(state.press.axis));
}

} // namespace blender_navigation_detail

inline void drawBlenderNavigationPresentation(
    QPainter &painter, const NavigationGizmoLayout &layout,
    const BlenderNavigationPresentationState &state) {
  using namespace blender_navigation_detail;
  if (!std::isfinite(layout.radius) || layout.radius <= 0.0 ||
      !std::isfinite(layout.center.x()) || !std::isfinite(layout.center.y())) return;
  const QColor backdrop = state.backdrop.isValid() ? state.backdrop :
      state.light ? QColor(Qt::white) : QColor(12, 13, 14);
  const QColor ink = state.light ? QColor(37, 42, 49) : QColor(222, 225, 228);
  const qreal lineWidth = std::clamp(layout.radius / 20.0, 1.0, 3.4);
  const qreal ringWidth = std::clamp(layout.radius / 30.0, 0.8, 2.0);
  const bool rotationActive = active(state, NavigationGizmoPart::Rotate);

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (rotationActive) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(state.light ? QColor(94, 101, 111, 30) : QColor(154, 160, 168, 45));
    painter.drawEllipse(layout.rotateBounds);
  }

  std::array<const NavigationAxisHandle *, 6> order;
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = &layout.handles[i];
  std::stable_sort(order.begin(), order.end(), [](const auto *first, const auto *second) {
    return first->depth < second->depth;
  });
  const bool axisAligned = std::any_of(order.begin(), order.end(),
                                       [](const auto *handle) { return handle->hidden; });
  QFont axisFont = painter.font();
  axisFont.setBold(true);
  axisFont.setPixelSize(std::clamp(qRound(layout.radius * 0.20 * 1.25), 9, 18));
  painter.setFont(axisFont);

  for (const NavigationAxisHandle *handle : order) {
    if (handle->hidden || handle->axisIndex < 0 || handle->axisIndex >= 3 ||
        handle->radius <= 0.0) continue;
    const QColor axis = blenderAxisColor(handle->axisIndex, state.light);
    // Blender's actual front/back interpolation: half the view background at
    // the far end, full axis ink at the near end; the center uses 75% axis ink.
    const QColor fading = mix(backdrop, axis, ((handle->depth + 1.0) * 0.25) + 0.5);
    const QColor middle = mix(backdrop, axis, 0.75);
    const bool highlight = highlighted(state, *handle);
    const QPointF delta = handle->center - layout.center;
    const qreal extent = std::hypot(delta.x(), delta.y());
    if ((handle->positive || axisAligned) && extent > handle->radius) {
      const QPointF start = layout.center -
          delta * (lineWidth / std::max(layout.radius, 1.0) * 0.66);
      const QPointF end = handle->center -
          delta * ((handle->radius - 0.5) / extent);
      QLinearGradient lineInk(start, end);
      lineInk.setColorAt(0.0, middle);
      lineInk.setColorAt(1.0, fading);
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QBrush(lineInk), lineWidth, Qt::SolidLine, Qt::FlatCap));
      painter.drawLine(start, end);
    }

    QColor fill = fading;
    QColor outline = fading;
    if (!handle->positive) {
      if (handle->aligned) {
        outline = mix(QColor(Qt::white), axis, 0.5);
      } else {
        fill = mix(backdrop, axis, 0.25);
        fill.setAlphaF(std::clamp(handle->depth + 1.0F, 0.0F, 1.0F));
      }
    }
    painter.setPen(QPen(highlight ? QColor(Qt::white) : outline,
                        highlight ? ringWidth + 0.5 : ringWidth));
    painter.setBrush(fill);
    painter.drawEllipse(handle->center, handle->radius, handle->radius);

    if (handle->positive || highlight || handle->aligned) {
      QString label(QChar('X' + handle->axisIndex));
      if (!handle->positive) label.prepend(QChar('-'));
      painter.setPen(highlight ? QColor(Qt::white) : QColor(0, 0, 0, rotationActive ? 255 : 230));
      const QRectF labelBounds(handle->center - QPointF(handle->radius, handle->radius),
                               QSizeF(handle->radius * 2.0, handle->radius * 2.0));
      painter.drawText(labelBounds, Qt::AlignCenter, label);
    }
  }

  QFont labelFont = axisFont;
  labelFont.setBold(false);
  labelFont.setPixelSize(std::clamp(qRound(layout.projectionLabel.height() * 0.5), 9, 13));
  painter.setFont(labelFont);
  painter.setPen(mix(backdrop, ink, 0.75));
  painter.drawText(layout.projectionLabel, Qt::AlignCenter,
                    state.orthographic
                        ? QCoreApplication::translate("Workbench", "正交")
                        : QCoreApplication::translate("Workbench", "透视"));

  const auto miniButton = [&](const QRectF &bounds, const NavigationGizmoPart part,
                              const WorkbenchIconId icon, const bool selected = false,
                              const bool enabled = true) {
    const bool over = enabled && active(state, part);
    painter.setPen(Qt::NoPen);
    if (over || selected) {
      painter.setBrush(selected ? state.light ? QColor(52, 100, 163, 85) : QColor(122, 164, 216, 90)
                                : state.light ? QColor(99, 107, 117, 40) : QColor(179, 186, 194, 55));
      painter.drawEllipse(bounds);
    }
    const qreal iconSize = bounds.width() * 0.57;
    QColor buttonInk = enabled ? ink : mix(backdrop, ink, 0.35);
    if (over) buttonInk = state.light ? QColor(13, 20, 30) : QColor(Qt::white);
    paintWorkbenchIcon(painter, icon,
        QRectF(bounds.center() - QPointF(iconSize * 0.5, iconSize * 0.5),
                 QSizeF(iconSize, iconSize)), buttonInk);
  };
  miniButton(layout.zoomButton, NavigationGizmoPart::Zoom, WorkbenchIconId::Zoom);
  miniButton(layout.panButton, NavigationGizmoPart::Pan, WorkbenchIconId::Pan);
  miniButton(layout.cameraButton, NavigationGizmoPart::Camera, WorkbenchIconId::Camera,
               state.cameraActive, state.cameraAvailable);
  miniButton(layout.projectionButton, NavigationGizmoPart::Projection,
               WorkbenchIconId::Projection, state.orthographic);
  painter.restore();
}

} // namespace gsw
