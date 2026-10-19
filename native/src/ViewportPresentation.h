/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Blender v4.5.0, commit 8cb6b388974a817afedf1317ce26f0c75aa5f181:
 * release/datafiles/userdef/userdef_default_theme.c, lines 237-239.
 * SHA256 1871145a25268d3310f3e0d8ae54ed6892f4ebb0a5d57399b899bef7dc0adbd3.
 * GSW adaptation: Qt colors, with darker inks for the configured white backdrop.
 * This is display color only, never a coordinate swap or a Blender 3D cursor.
 * The combined desktop selects GPL-3.0-or-later; see COPYING.
 */
#pragma once
#include <QColor>
#include <array>

namespace gsw {
[[nodiscard]] inline std::array<QColor, 3> blenderAxisColors(const bool light) {
  if (light) {
    return {QColor(191, 38, 61), QColor(80, 134, 0), QColor(26, 96, 184)};
  }
  return {QColor(255, 51, 82), QColor(139, 220, 0), QColor(40, 144, 255)};
}

[[nodiscard]] inline QColor blenderAxisColor(const int axis, const bool light) {
  if (axis < 0 || axis >= 3) return light ? QColor(48, 55, 64) : QColor(225, 229, 232);
  return blenderAxisColors(light)[static_cast<std::size_t>(axis)];
}
} // namespace gsw
