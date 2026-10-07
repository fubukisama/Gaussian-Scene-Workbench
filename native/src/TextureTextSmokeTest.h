#pragma once

#include <QString>

namespace gsw {
class NativeViewport;

// Capture newly introduced overlay glyphs only after the actual mesh renderer
// has presented the requested textured or untextured scene. Pixel comparison is
// performed between independent application processes by the companion script.
bool runTextureTextSmokeTest(NativeViewport &viewport, const QString &scenePath);
}
