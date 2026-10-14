#pragma once

namespace gsw {
class NativeViewport;
// Real-window acceptance for the isolated GitHub runner, never a local
// background/offscreen substitute for actual viewport input and rendering.
[[nodiscard]] bool runUpstreamNavigationSmokeTest(NativeViewport &viewport);
} // namespace gsw
