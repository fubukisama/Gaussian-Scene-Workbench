/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <optional>

namespace gsw::blender_grid {

inline constexpr int StepCount = 8;

// Adapted from Blender's unitless view3d_grid_steps_ex subdivision loop.
// baseSpacing is already in the caller's reference/display coordinate unit.
// N > 1 produces increasing levels; N == 1 repeats the base for fixed mode.
// Invalid inputs or values outside the finite normal-float shader range fail
// explicitly. No physical unit, minimum scene scale or clamping is implied.
[[nodiscard]] std::optional<std::array<float, StepCount>>
subdivisionSteps(double baseSpacing, int subdivisions);

} // namespace gsw::blender_grid
