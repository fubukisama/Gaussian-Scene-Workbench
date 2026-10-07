#pragma once

#include <QString>

namespace gsw {
class NativeViewport;

// Compare the visible surface of the same PLY through the resident and paged
// loading policies, using only the viewport's public interface and framebuffer.
bool runMeshRenderingSmokeTest(NativeViewport &viewport, const QString &path);
}
