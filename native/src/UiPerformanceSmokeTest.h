#pragma once

namespace gsw {
class NativeViewport;

// Remote, test-owned real-window observation only. Zero means a structurally
// complete recording, not that the independent paired A/B acceptance passed.
// 2 = contract failure; 3 = unsuitable/unready environment (INCONCLUSIVE).
int runUiPerformanceSmokeTest(NativeViewport &viewport);
}
