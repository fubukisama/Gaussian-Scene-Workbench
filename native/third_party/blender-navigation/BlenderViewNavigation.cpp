/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "BlenderViewNavigation.h"

#include <cmath>

namespace gsw {

namespace {
bool finiteVector(const QVector3D &value) {
  return std::isfinite(value.x()) && std::isfinite(value.y()) &&
         std::isfinite(value.z());
}

bool finitePoint(const QPointF &point) {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool finiteMatrix(const QMatrix4x4 &matrix) {
  for (int index = 0; index < 16; ++index) {
    if (!std::isfinite(matrix.constData()[index])) return false;
  }
  return true;
}

// Qt adaptation of Blender's ED_view3d_win_to_delta (view3d_project.cc).
// Blender scales the pixel delta by zfac before applying persinv. The target's
// clip W is that depth scale (1 for an orthographic camera). Applying the two
// inverse matrices to a direction expresses ED_view3d_win_to_delta in Qt,
// avoiding cancellation in a near/far homogeneous depth division. Y is flipped
// for Qt pixels. Actual projection extents, not requested distance, are used.
std::optional<QVector3D> targetPlaneOffset(
    const QVector3D &target, const QMatrix4x4 &view,
    const QMatrix4x4 &projection, const QSizeF &viewport,
    const QPointF &pixel) {
  if (!finiteVector(target) || !finiteMatrix(view) ||
      !finiteMatrix(projection) || !finitePoint(pixel) ||
      !std::isfinite(viewport.width()) || !std::isfinite(viewport.height()) ||
      viewport.width() <= 0 || viewport.height() <= 0) return std::nullopt;

  const QVector4D clip = projection * view * QVector4D(target, 1);
  if (!std::isfinite(clip.w()) || clip.w() <= 1.0e-8F) return std::nullopt;
  bool viewInvertible = false;
  bool projectionInvertible = false;
  const QMatrix4x4 inverseView = view.inverted(&viewInvertible);
  const QMatrix4x4 inverseProjection = projection.inverted(&projectionInvertible);
  if (!viewInvertible || !projectionInvertible ||
      !finiteMatrix(inverseView) || !finiteMatrix(inverseProjection) ||
      !std::isfinite(clip.x()) || !std::isfinite(clip.y()) ||
      !std::isfinite(clip.z()))
    return std::nullopt;
  const float dx = static_cast<float>(2 * pixel.x() / viewport.width() - 1) *
                       clip.w() - clip.x();
  const float dy = static_cast<float>(1 - 2 * pixel.y() / viewport.height()) *
                       clip.w() - clip.y();
  const QVector4D direction = inverseView * inverseProjection *
                             QVector4D(dx, dy, 0, 0);
  // Standard perspective/orthographic camera projections map this clip-plane
  // delta to a vector. Reject arbitrary projective matrices that do not.
  if (!std::isfinite(direction.w()) || std::abs(direction.w()) > 1.0e-6F)
    return std::nullopt;
  const QVector3D offset = direction.toVector3D();
  return finiteVector(offset) ? std::optional<QVector3D>(offset) : std::nullopt;
}
} // namespace

bool blenderViewOrbitOrthographic(const bool orthographic, const bool axisView) {
  // Blender v4.5.0 view3d_utils.cc, ED_view3d_persp_ensure, lines 534-559.
  // The host resolves scene-camera exit separately. Keep Blender's distinction
  // between RV3D_VIEW_IS_AXIS and a manually chosen user orthographic view.
  if (orthographic && axisView) return false;
  return orthographic;
}

std::optional<QVector3D> blenderViewZoomTarget(
    const QVector3D &target, const QMatrix4x4 &viewBefore,
    const QMatrix4x4 &projectionBefore, const QMatrix4x4 &viewAfter,
    const QMatrix4x4 &projectionAfter, const QSizeF &viewport,
    const QPointF &cursor) {
  const auto before = targetPlaneOffset(target, viewBefore, projectionBefore,
                                        viewport, cursor);
  const auto after = targetPlaneOffset(target, viewAfter, projectionAfter,
                                       viewport, cursor);
  if (!before || !after) return std::nullopt;
  // Adapted view_zoom_to_window_xy_3d: target + dvec - dvec * dfac.
  // Using the actual after-projection offset instead of the requested dfac
  // preserves the anchor when distance or orthographic extent was clamped.
  const QVector3D result = target + *before - *after;
  return finiteVector(result) ? std::optional<QVector3D>(result) : std::nullopt;
}

std::optional<QVector3D> blenderViewPanTarget(
    const QVector3D &target, const QMatrix4x4 &view,
    const QMatrix4x4 &projection, const QSizeF &viewport,
    const QPointF &logicalPixelDelta) {
  if (!finitePoint(logicalPixelDelta)) return std::nullopt;
  const QPointF center(viewport.width() * 0.5, viewport.height() * 0.5);
  const auto before = targetPlaneOffset(target, view, projection, viewport, center);
  const auto after = targetPlaneOffset(target, view, projection, viewport,
                                       center + logicalPixelDelta);
  if (!before || !after) return std::nullopt;
  // Blender's viewmove_apply subtracts ED_view3d_win_to_delta from the view
  // offset. GSW stores the positive target rather than Blender's negated ofs.
  const QVector3D result = target - (*after - *before);
  return finiteVector(result) ? std::optional<QVector3D>(result) : std::nullopt;
}

} // namespace gsw
