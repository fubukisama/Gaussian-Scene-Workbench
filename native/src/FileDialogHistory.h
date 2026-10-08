#pragma once

class QFileDialog;

namespace gsw {
// Common history behavior for the application's widget-based file dialogs.
// Attaching is idempotent and never accepts a dialog or changes its filter.
class FileDialogHistory {
public:
  static void attach(QFileDialog *dialog);
};
} // namespace gsw
