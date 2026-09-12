#pragma once

class QAction;
class QWidget;

namespace gsw {
// One policy for the main window, dialogs (including Qt static file dialogs),
// and detached docks. Popup menus/tooltips are intentionally excluded.
class WindowUi {
public:
  static void install();
  static void prepare(QWidget *window);
  static QAction *fullScreenAction(QWidget *window);
  static void toggleFullScreen(QWidget *window);
  static void toggleMaximized(QWidget *window);
};
} // namespace gsw
