/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BlenderGridSteps.h"

#include <cmath>
#include <limits>

namespace gsw::blender_grid {

std::optional<std::array<float, StepCount>>
subdivisionSteps(const double baseSpacing, const int subdivisions) {
  if (!std::isfinite(baseSpacing) || baseSpacing <= 0.0 || subdivisions < 1) {
    return std::nullopt;
  }

  // Actual adaptation of the unitless subdivision loop in Blender v4.5.0:
  // source/blender/editors/space_view3d/view3d_draw.cc, view3d_grid_steps_ex,
  // commit 8cb6b388974a817afedf1317ce26f0c75aa5f181.
  // Full upstream file SHA256:
  // 6a6d5edb9021116cab87b6a9e2299c39b0cf4b950c3ccf50e78eb854b6fd4b48.
  // GSW takes the caller's base directly, omits Blender view/unit objects and
  // the axis-view division by N^3, and uses double rather than an overflowing
  // integer multiplier. All eight results are validated before publication.
  std::array<float, StepCount> result{};
  double subdivisionFactor = 1.0;
  for (int index = 0;; ++index) {
    const double spacing = baseSpacing * subdivisionFactor;
    // Normal floats also avoid a positive CPU subnormal becoming zero in a
    // GPU implementation that flushes denormals. This is a representation
    // boundary, not a physical minimum grid size or an automatic rescaling.
    if (!std::isfinite(spacing) ||
        spacing < static_cast<double>(std::numeric_limits<float>::min()) ||
        spacing > static_cast<double>(std::numeric_limits<float>::max())) {
      return std::nullopt;
    }
    const float narrowed = static_cast<float>(spacing);
    if (!std::isfinite(narrowed) || narrowed <= 0.0F ||
        (index > 0 && subdivisions > 1 && narrowed <= result[index - 1])) {
      return std::nullopt;
    }
    result[index] = narrowed;
    if (index == StepCount - 1) {
      break;
    }
    subdivisionFactor *= static_cast<double>(subdivisions);
  }
  return result;
}

} // namespace gsw::blender_grid
