/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QSizeF>
#include <QVector3D>

#include <optional>

namespace gsw {

// Adapted Blender view navigation for Qt camera matrices and top-left logical
// pixels. Inputs are value-only: no model, project, GPU or window state changes.
// The zoom frames have the same target and orientation, with the caller's
// actual before/after distances and projections already applied. This anchors
// the target-depth plane, not an arbitrary model surface. nullopt means the
// input cannot safely define that plane; the caller keeps its old target.
[[nodiscard]] std::optional<QVector3D> blenderViewZoomTarget(
    const QVector3D &target, const QMatrix4x4 &viewBefore,
    const QMatrix4x4 &projectionBefore, const QMatrix4x4 &viewAfter,
    const QMatrix4x4 &projectionAfter, const QSizeF &viewport,
    const QPointF &cursor);

// The displayed scene follows logicalPixelDelta exactly on the target-depth
// plane. Uses the actual projection, including orthographic extents and FOV.
[[nodiscard]] std::optional<QVector3D> blenderViewPanTarget(
    const QVector3D &target, const QMatrix4x4 &view,
    const QMatrix4x4 &projection, const QSizeF &viewport,
    const QPointF &logicalPixelDelta);

} // namespace gsw
