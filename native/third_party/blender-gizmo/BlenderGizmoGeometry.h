/* SPDX-FileCopyrightText: 2014-2016 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QList>
#include <QVector3D>

namespace gsw {

// Qt geometry adapter for Blender 4.5's normal arrow and box handles.
// Local Z is the handle direction. The arrow base is at Z=0, tip at Z=height.
[[nodiscard]] QList<QList<QVector3D>> blenderGizmoCone(float radius, float height);
[[nodiscard]] QList<QList<QVector3D>> blenderGizmoCube(float halfSize);

// Blender arrow3d normal style uses a stem of 1, head length .25 and radius .06.
// Express them relative to the fixed total axis extent, preserving GSW pivots.
inline constexpr float blenderGizmoHeadFraction = 0.25F / 1.25F;
inline constexpr float blenderGizmoHeadRadiusFraction = 0.06F / 1.25F;
inline constexpr float blenderGizmoBoxFraction = 0.10F / 1.10F;

} // namespace gsw
