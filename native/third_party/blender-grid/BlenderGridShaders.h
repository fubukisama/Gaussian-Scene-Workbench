/* SPDX-FileCopyrightText: 2017-2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BlenderGridSteps.h"

#include <QByteArray>

namespace gsw::blender_grid {

// Shader-only adapter. The caller owns its GL program, empty VAO, framebuffer,
// colors and immutable scene reference. Draw one triangle (three vertices).
// Coordinate/uniform and alpha/depth contracts are documented in README.gsw.md.
[[nodiscard]] QByteArray vertexShader();
[[nodiscard]] QByteArray fragmentShader();

} // namespace gsw::blender_grid
