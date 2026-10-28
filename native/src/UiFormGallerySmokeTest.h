#pragma once

namespace gsw {
class MainWindow;

// Remote-only, isolated, synthetic gallery. Root wires --smoke-test-ui-form-gallery;
// GSW_UI_FORM_GALLERY_GROUP must be exactly1 or2 (inventory DLG01-08/09-16).
// Opens production dialogs without accepting them or starting generation.
// Uses public getter freezes; UI_ONLY_OPEN rows are explicitly not request or
// deep-semantic acceptance. Existing families remain responsible for those.
// Reuses the already installed UiSurfaceEvidence adapter; no local UI execution.
bool runUiFormGallerySmokeTest(MainWindow &window);
} // namespace gsw
