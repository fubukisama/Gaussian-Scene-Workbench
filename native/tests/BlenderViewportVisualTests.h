#pragma once

class QApplication;

namespace gsw {

// Must be called before any interactive application window is constructed or
// shown. Exercises the real viewport framebuffer without presenting a window.
// A missing OpenGL context is an explicit failure, never a passing skip.
int runBlenderViewportVisualTests(QApplication &application);

} // namespace gsw
