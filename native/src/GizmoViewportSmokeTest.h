#pragma once

namespace gsw {
class NativeViewport;

// Explicit remote real-Windows smoke only: --smoke-test-gizmo-viewport,
// GITHUB_ACTIONS=true, main's isolated gswSmokeSettingsDirectory,
// GSW_UI_SOURCE_COMMIT (40 hex), and GSW_GIZMO_VIEWPORT_EVIDENCE (absolute,
// absent or empty). The supplied viewport must have an empty scene and no
// processing/training preview. Never opens, activates or raises a window.
// Writes an owned gray cube once under evidence/fixtures, preserves its digest,
// and records raw grabFramebuffer PNGs/public state in gizmo-viewport.json.
// False includes unsupported/unpainted/no-visible-handle cases, not a fake pass.
bool runGizmoViewportSmokeTest(NativeViewport &viewport);
} // namespace gsw
