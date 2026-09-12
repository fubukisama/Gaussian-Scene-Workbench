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
} // namespace

bool runWindowUiSmokeTest() {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { qWarning() << "Window UI smoke:" << message; passed = false; }
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const auto locale = AppLanguage::current();
  const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
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
#ifdef Q_OS_WIN
  // Real mouse input passes through the native message queue. SendMessage
  // bypasses Qt's application-level native input filter.
  PostMessageW(reinterpret_cast<HWND>(host.winId()), WM_NCLBUTTONDBLCLK, HTCAPTION, 0);
  settle();
  check(host.isFullScreen(), "native title-bar double-click enters full screen");
  if (host.isFullScreen()) WindowUi::toggleFullScreen(&host);
#endif
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
  auto *desktopButton = file.findChild<QToolButton *>(QStringLiteral("fileDialogDesktopButton"));
  auto *name = file.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
  auto *maxButton = file.findChild<QToolButton *>(QStringLiteral("windowMaximizeButton"));
  check(desktopButton && name && maxButton, "file dialog controls installed automatically");
  if (!desktopButton || !name || !maxButton) return false;
  check(file.windowFlags().testFlag(Qt::WindowMaximizeButtonHint), "dialog title bar enables maximize");
  check(file.sidebarUrls().contains(QUrl::fromLocalFile(desktop)) &&
        file.sidebarUrls().contains(QUrl::fromLocalFile(temporary.path())), "desktop added without removing existing sidebar places");
  const QString filename = name->text();
  desktopButton->click(); settle();
  check(file.directory().absolutePath() == QDir(desktop).absolutePath(), "desktop button navigates to OS desktop path");
  check(name->text() == filename && file.selectedNameFilter() == QStringLiteral("GLB (*.glb)"), "desktop navigation preserves export filename and filter");
  file.setDirectory(temporary.path());
  WindowUi::fullScreenAction(&file)->trigger(); settle();
  check(file.isFullScreen(), "file dialog full screen");
  for (const auto &language : AppLanguage::supported()) {
    check(AppLanguage::apply(language, false), "change language while file dialog is full screen");
    settle();
    check(desktopButton->text() == QCoreApplication::translate("Workbench", "桌面"), "desktop text translated live");
    auto *sidebar = file.findChild<QListView *>(QStringLiteral("sidebar"));
    check(sidebar && sidebar->model()->index(0, 0).data().toString() == QCoreApplication::translate("Workbench", "桌面"), "desktop sidebar shortcut translated live");
    check(WindowUi::fullScreenAction(&file)->text() == QCoreApplication::translate("Workbench", "退出全屏"), "full-screen action translated live");
    check(file.isFullScreen() && name->text() == filename && file.selectedNameFilter() == QStringLiteral("GLB (*.glb)"), "language change preserves dialog state");
  }
  check(AppLanguage::apply(locale, false), "restore original UI language");
  key(*name, Qt::Key_Escape);
  check(!file.isFullScreen() && file.isVisible(), "first Esc leaves full screen without rejecting file dialog");
  maxButton->click(); settle();
  check(file.isMaximized(), "dialog maximize button");
  maxButton->click(); settle();
  check(!file.isMaximized() && !file.isFullScreen() && name->text() == filename, "dialog restore retains filename");
  file.hide(); file.show(); settle();
  check(file.findChildren<QWidget *>(QStringLiteral("windowControlsBar")).size() == 1, "reopening does not duplicate controls");
  const QString shots = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
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
    check(open.findChild<QToolButton *>(QStringLiteral("fileDialogDesktopButton")), "open and folder dialog desktop shortcut");
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
