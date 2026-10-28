#pragma once

class QWidget;

namespace gsw {
class NativeViewport;

// Remote isolated QA only. Call before show and before enabling observation;
// prepares the same owned 640x360 viewport/chrome in both measurement arms.
bool prepareUiPerformanceWindow(QWidget &window);

// Remote, test-owned real-window observation only. Zero means a structurally
// complete recording, not that the independent paired A/B acceptance passed.
// 2 = contract failure; 3 = unsuitable/unready environment (INCONCLUSIVE).
int runUiPerformanceSmokeTest(NativeViewport &viewport);
}
