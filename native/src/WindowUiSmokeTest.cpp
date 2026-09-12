#include "WindowUiSmokeTest.h"
#include "WindowUi.h"
#include "AppLanguage.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
void settle() {
  QEventLoop loop;
  QTimer::singleShot(100, &loop, &QEventLoop::quit);
  loop.exec();
}
void key(QWidget &target, int code) {
  QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
  QApplication::sendEvent(&target, &press);
  QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
  QApplication::sendEvent(&target, &release);
  settle();
}
void mouse(QWidget &target, QEvent::Type type, const QPoint &global, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QMouseEvent event(type, target.mapFromGlobal(global), global, button, buttons, modifiers);
  QApplication::sendEvent(&target, &event);
}
void doubleClick(QWidget &target) {
  const QPoint global = target.mapToGlobal(QPoint(40, target.height() / 2));
  mouse(target, QEvent::MouseButtonDblClick, global, Qt::LeftButton, Qt::LeftButton);
  mouse(target, QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton);
  settle();
}
void drag(QWidget &target, const QPoint &delta) {
  const QPoint start = target.mapToGlobal(QPoint(40, target.height() / 2));
  mouse(target, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
  mouse(target, QEvent::MouseMove, start + delta, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
  mouse(target, QEvent::MouseButtonRelease, start + delta, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
  settle();
}
void captionDoubleClick(QWidget &window) {
#ifdef Q_OS_WIN
  PostMessageW(reinterpret_cast<HWND>(window.winId()), WM_NCLBUTTONDBLCLK, HTCAPTION, 0);
  settle();
#else
  WindowUi::toggleMaximized(&window); settle();
#endif
}
} // namespace

bool runWindowUiSmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { qWarning() << "Window UI smoke:" << message; passed = false; }
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const auto locale = AppLanguage::current();
  const QString shots = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  if (!shots.isEmpty()) QDir().mkpath(shots);
  const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  const QByteArray dockState = workbench.saveState(5);
  for (const auto *id : {"projectDock", "inspectorDock", "taskDock"}) {
    auto *panel = workbench.findChild<QDockWidget *>(QLatin1String(id));
    check(panel && panel->features().testFlag(QDockWidget::DockWidgetMovable) &&
          panel->features().testFlag(QDockWidget::DockWidgetFloatable) &&
          panel->allowedAreas() == Qt::AllDockWidgetAreas, "all panels can move to every dock area");
    if (!panel) continue;
    auto *title = panel->titleBarWidget();
    check(title != nullptr, "dock title exists");
    if (!title) continue;
    doubleClick(*title);
    check(panel->isFloating() && !panel->isFullScreen(), "double-click dock title detaches without entering full screen");
    if (!panel->isFloating()) panel->setFloating(true);
    settle();
    check(panel->width() >= 280 && panel->height() >= 280 &&
          panel->width() < panel->height() * 3, "first floating panel has usable proportions, not a strip");
    const QPoint beforeDrag = panel->pos();
    drag(*title, QPoint(65, -45));
    check(panel->isFloating() && (panel->pos() - beforeDrag).manhattanLength() > 30,
          "dragging a floating title actually moves the panel");
    if (panel->objectName() == QStringLiteral("taskDock") && !shots.isEmpty())
      check(panel->grab().save(QDir(shots).filePath(locale + QStringLiteral("-floating-tasks.png"))), "floating task screenshot");
    panel->resize(450, 350); settle();
    const QSize userSize = panel->size();
    WindowUi::fullScreenAction(panel)->trigger(); settle();
    check(panel->isFullScreen(), "floating workbench panel enters full screen with F11 action");
    key(*panel, Qt::Key_Escape);
    check(!panel->isFullScreen() && panel->size() == userSize, "floating workbench panel restores its user size after full screen");
    AppLanguage::apply(locale == QStringLiteral("en_US") ? QStringLiteral("ja_JP") : QStringLiteral("en_US"), false);
    settle();
    check(title->toolTip() == QCoreApplication::translate("Workbench", "拖动标题栏移动面板；双击停靠或浮动；Ctrl 拖动保持浮动"), "dock gesture help switches language live");
    check(panel->size() == userSize, "language change preserves floating panel size");
    AppLanguage::apply(locale, false); settle();
    doubleClick(*title);
    check(!panel->isFloating(), "double-click floating title docks the panel");
    panel->setFloating(true); settle();
    check(panel->size() == userSize, "user floating size survives redocking");
    panel->setFloating(false); settle();
    drag(*title, QPoint(70, -70));
    check(panel->isFloating(), "dragging a docked title detaches the panel");
    panel->setFloating(false); settle();
    for (auto area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea, Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea}) {
      workbench.addDockWidget(area, panel); settle();
      check(workbench.dockWidgetArea(panel) == area, "panel can be placed in each dock area");
    }
  }
  workbench.restoreState(dockState, 5); settle();
  QMainWindow host;
  host.resize(640, 450);
  host.setCentralWidget(new QLineEdit(QStringLiteral("unchanged"), &host));
  host.show(); host.activateWindow(); settle();
  auto *hostAction = WindowUi::fullScreenAction(&host);
  check(hostAction && hostAction->shortcut() == QKeySequence(QStringLiteral("F11")), "main window full-screen action");
  const QRect original = host.geometry();
  key(*host.centralWidget(), Qt::Key_F11);
  check(host.isFullScreen(), "F11 enters full screen from a focused child");
  key(*host.centralWidget(), Qt::Key_Escape);
  check(!host.isFullScreen() && !host.isMaximized() && host.geometry() == original, "Esc restores original geometry");
  host.showMaximized(); settle();
  hostAction->trigger(); settle();
  hostAction->trigger(); settle();
  check(host.isMaximized() && !host.isFullScreen(), "full screen restores previous maximized state");
  host.showNormal(); settle();
  captionDoubleClick(host);
  check(host.isMaximized() && !host.isFullScreen(), "native title-bar double-click maximizes, not full screen");
  captionDoubleClick(host);
  check(!host.isMaximized() && !host.isFullScreen(), "native title-bar second double-click restores");
  // Content double-clicks must not be interpreted as window commands.
  auto *editor = static_cast<QLineEdit *>(host.centralWidget());
  QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(20, 10), QPointF(editor->mapToGlobal(QPoint(20,10))),
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(editor, &doubleClick);
  check(!host.isFullScreen() && editor->text() == QStringLiteral("unchanged"), "content double-click retains editing behavior");

  QFileDialog file(&host);
  file.setOption(QFileDialog::DontUseNativeDialog);
  file.setAcceptMode(QFileDialog::AcceptSave);
  file.setNameFilters({QStringLiteral("PLY (*.ply)"), QStringLiteral("GLB (*.glb)")});
  file.selectNameFilter(QStringLiteral("GLB (*.glb)"));
  file.setDirectory(temporary.path());
  file.setSidebarUrls({QUrl::fromLocalFile(temporary.path())});
  file.selectFile(QStringLiteral("模型_日本語.glb"));
  file.resize(800, 560);
  file.show(); file.activateWindow(); settle();
  check(file.findChildren<QWidget *>(QStringLiteral("windowControlsBar")).isEmpty(), "only the native title bar, no duplicate controls row");
  auto *name = file.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
  auto *sidebar = file.findChild<QListView *>(QStringLiteral("sidebar"));
  check(sidebar && name, "file dialog sidebar and filename available");
  if (!sidebar || !name) return false;
  check(file.windowFlags().testFlag(Qt::WindowMaximizeButtonHint), "dialog title bar enables maximize");
  check(file.sidebarUrls().contains(QUrl::fromLocalFile(desktop)) &&
        file.sidebarUrls().contains(QUrl::fromLocalFile(temporary.path())), "desktop added without removing existing sidebar places");
  const QString filename = name->text();
  const QPoint desktopPoint = sidebar->viewport()->mapToGlobal(sidebar->visualRect(sidebar->model()->index(0, 0)).center());
  mouse(*sidebar->viewport(), QEvent::MouseButtonPress, desktopPoint, Qt::LeftButton, Qt::LeftButton);
  mouse(*sidebar->viewport(), QEvent::MouseButtonRelease, desktopPoint, Qt::LeftButton, Qt::NoButton);
  settle();
  check(file.directory().absolutePath() == QDir(desktop).absolutePath(), "desktop sidebar click navigates to OS desktop path");
  check(name->text() == filename && file.selectedNameFilter() == QStringLiteral("GLB (*.glb)"), "desktop navigation preserves export filename and filter");
  file.setDirectory(temporary.path());
  WindowUi::fullScreenAction(&file)->trigger(); settle();
  check(file.isFullScreen(), "file dialog full screen");
  for (const auto &language : AppLanguage::supported()) {
    check(AppLanguage::apply(language, false), "change language while file dialog is full screen");
    settle();
    check(sidebar && sidebar->model()->index(0, 0).data().toString() == QCoreApplication::translate("Workbench", "桌面"), "desktop sidebar shortcut translated live");
    check(WindowUi::fullScreenAction(&file)->text() == QCoreApplication::translate("Workbench", "退出全屏"), "full-screen action translated live");
    check(file.isFullScreen() && name->text() == filename && file.selectedNameFilter() == QStringLiteral("GLB (*.glb)"), "language change preserves dialog state");
  }
  check(AppLanguage::apply(locale, false), "restore original UI language");
  key(*name, Qt::Key_Escape);
  check(!file.isFullScreen() && file.isVisible(), "first Esc leaves full screen without rejecting file dialog");
  captionDoubleClick(file);
  check(file.isMaximized() && !file.isFullScreen(), "dialog native caption double-click maximizes");
  captionDoubleClick(file);
  check(!file.isMaximized() && !file.isFullScreen() && name->text() == filename, "dialog restore retains filename");
  file.hide(); file.show(); settle();
  check(file.findChildren<QWidget *>(QStringLiteral("windowControlsBar")).isEmpty(), "reopening and translation never add duplicate controls");
  if (!shots.isEmpty()) {
    QDir().mkpath(shots);
    check(file.grab().save(QDir(shots).filePath(locale + QStringLiteral("-file-dialog.png"))), "file-dialog screenshot");
  }
  file.hide();
  // Covers Qt static directory/open dialogs as well as the save path.
  for (const auto mode : {QFileDialog::ExistingFiles, QFileDialog::Directory}) {
    QFileDialog open(&host);
    open.setFileMode(mode); open.setDirectory(temporary.path());
    open.show(); settle();
    check(open.sidebarUrls().contains(QUrl::fromLocalFile(desktop)), "open and folder dialog desktop shortcut");
    open.hide();
  }
  QInputDialog input(&host); input.setTextValue(QStringLiteral("unmodified"));
  QProgressDialog progress(&host); progress.setValue(37);
  QMessageBox message(QMessageBox::Information, QStringLiteral("QA"), QStringLiteral("QA"), QMessageBox::Ok, &host);
  for (QDialog *dialog : {static_cast<QDialog *>(&input), static_cast<QDialog *>(&progress), static_cast<QDialog *>(&message)}) {
    dialog->show(); settle();
    WindowUi::fullScreenAction(dialog)->trigger(); settle();
    if (!dialog->isFullScreen()) qWarning() << "Dialog full-screen state:" << dialog->metaObject()->className() << dialog->windowState() << dialog->maximumSize();
    check(dialog->isFullScreen(), "input/progress/message dialog can enter full screen");
    AppLanguage::apply(locale == QStringLiteral("en_US") ? QStringLiteral("ja_JP") : QStringLiteral("en_US"), false);
    settle();
    check(dialog->isFullScreen(), "full-screen message/input/progress survives language changes");
    AppLanguage::apply(locale, false); settle();
    key(*dialog, Qt::Key_Escape);
    check(dialog->isVisible() && !dialog->isFullScreen(), "input/progress/message dialog remains open after Esc");
    WindowUi::toggleMaximized(dialog); settle();
    check(dialog->isMaximized(), "input/progress/message maximize");
    WindowUi::toggleMaximized(dialog); settle();
    check(!dialog->isMaximized() && dialog->isVisible(), "input/progress/message restore");
    if (dialog == &message && !shots.isEmpty())
      check(dialog->grab().save(QDir(shots).filePath(locale + QStringLiteral("-message-dialog.png"))), "message-dialog screenshot");
    dialog->hide();
  }
  check(input.textValue() == QStringLiteral("unmodified") && progress.value() == 37 && !progress.wasCanceled(), "window changes do not reset input or cancel work");
  QDockWidget dock(&host);
  auto *dockContents = new QWidget;
  auto *dockLayout = new QVBoxLayout(dockContents);
  dockLayout->addWidget(new QLineEdit(QStringLiteral("dock data")));
  dockLayout->addStretch();
  dock.setWidget(dockContents);
  host.addDockWidget(Qt::LeftDockWidgetArea, &dock);
  WindowUi::toggleFullScreen(&dock); settle();
  check(dock.isFloating() && dock.isFullScreen(), "dock detaches into full screen");
  key(*dock.widget(), Qt::Key_Escape);
  check(!dock.isFullScreen() && dock.isFloating(), "dock returns to floating window");
  dock.setFloating(false); settle();
  check(WindowUi::fullScreenAction(&dock)->shortcut().isEmpty(), "docked panel does not steal main-window F11");
  check(AppLanguage::current() == locale, "test leaves language unchanged");
  qInfo().noquote() << "Window UI smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
