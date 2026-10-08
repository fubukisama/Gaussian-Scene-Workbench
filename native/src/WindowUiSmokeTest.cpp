#include "WindowUiSmokeTest.h"
#include "WindowUi.h"
#include "AppLanguage.h"
#include "AppTheme.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFile>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressDialog>
#include <QScreen>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <UIAutomation.h>
#include <OleAcc.h>
#include <wrl/client.h>
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

#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;

struct MinimizeButtonResult {
  std::atomic_bool done{false};
  std::atomic_bool canceled{false};
  HRESULT result = E_PENDING;
};

HRESULT invokeOwnedMinimizeButton(HWND hwnd, const std::atomic_bool &canceled) {
  // UIA must run on a windowless MTA thread when exercising our own UI.
  // All COM objects stay in this apartment; only the HWND crosses threads.
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return initialized;
  struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
  const auto stillOwned = [&] {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    return !canceled.load() && IsWindow(hwnd) && process == GetCurrentProcessId();
  };
  if (!stillOwned()) return E_ABORT;
  ComPtr<IUIAutomation2> automation;
  HRESULT result = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(automation.GetAddressOf()));
  if (FAILED(result)) return result;
  if (FAILED(result = automation->put_ConnectionTimeout(1000)) ||
      FAILED(result = automation->put_TransactionTimeout(1000))) return result;
  ComPtr<IUIAutomationElement> owned;
  if (FAILED(result = automation->ElementFromHandle(hwnd, owned.GetAddressOf()))) return result;
  int process = 0;
  if (FAILED(result = owned->get_CurrentProcessId(&process))) return result;
  if (process != static_cast<int>(GetCurrentProcessId()) || !stillOwned()) return E_ACCESSDENIED;
  VARIANT type{};
  type.vt = VT_I4;
  type.lVal = UIA_TitleBarControlTypeId;
  ComPtr<IUIAutomationCondition> titleBarCondition;
  if (FAILED(result = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type,
      titleBarCondition.GetAddressOf()))) return result;
  ComPtr<IUIAutomationElement> titleBar;
  if (FAILED(result = owned->FindFirst(TreeScope_Children, titleBarCondition.Get(),
      titleBar.GetAddressOf()))) return result;
  if (!titleBar) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  type.lVal = UIA_ButtonControlTypeId;
  ComPtr<IUIAutomationCondition> buttonCondition;
  if (FAILED(result = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type,
      buttonCondition.GetAddressOf()))) return result;
  ComPtr<IUIAutomationElementArray> buttons;
  if (FAILED(result = titleBar->FindAll(TreeScope_Children, buttonCondition.Get(),
      buttons.GetAddressOf()))) return result;
  int count = 0;
  if (FAILED(result = buttons->get_Length(&count))) return result;
  ComPtr<IUIAutomationElement> minimize;
  for (int index = 0; index < count; ++index) {
    if (!stillOwned()) return E_ABORT;
    ComPtr<IUIAutomationElement> button;
    if (FAILED(result = buttons->GetElement(index, button.GetAddressOf()))) return result;
    BSTR rawId = nullptr;
    button->get_CurrentAutomationId(&rawId);
    const QString id = rawId ? QString::fromWCharArray(rawId) : QString();
    SysFreeString(rawId);
    int childId = 0;
    DWORD role = 0;
    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(button->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
        __uuidof(IUIAutomationLegacyIAccessiblePattern),
        reinterpret_cast<void **>(legacy.GetAddressOf())))) {
      legacy->get_CurrentChildId(&childId);
      legacy->get_CurrentRole(&role);
    }
    qInfo() << "Native minimize UIA candidate:" << "AutomationId" << id
            << "MSAA child" << childId << "role" << Qt::hex << role << Qt::dec;
    // Do not select by a localized caption or invoke an arbitrary button.
    // MSAA's minimize child (2) is accepted only when AutomationId is absent.
    if (id != QStringLiteral("Minimize") && id != QStringLiteral("Minimize-Restore") &&
        !(id.isEmpty() && childId == 2 && role == ROLE_SYSTEM_PUSHBUTTON)) continue;
    if (minimize) return E_UNEXPECTED;
    minimize = button;
  }
  if (!minimize) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  BOOL enabled = FALSE;
  BOOL offscreen = TRUE;
  if (FAILED(result = minimize->get_CurrentProcessId(&process)) ||
      FAILED(result = minimize->get_CurrentIsEnabled(&enabled)) ||
      FAILED(result = minimize->get_CurrentIsOffscreen(&offscreen))) return result;
  qInfo() << "Native minimize UIA selected:" << "PID" << process << "enabled" << bool(enabled)
          << "offscreen" << bool(offscreen);
  if (process != static_cast<int>(GetCurrentProcessId()) || !enabled || offscreen || !stillOwned())
    return E_ACCESSDENIED;
  ComPtr<IUIAutomationInvokePattern> action;
  if (FAILED(result = minimize->GetCurrentPatternAs(UIA_InvokePatternId,
      __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void **>(action.GetAddressOf())))) return result;
  if (!stillOwned()) return E_ABORT;
  return action->Invoke(); // Real title-bar Button action, not WindowPattern::SetWindowVisualState.
}

bool captionMinimizeAndRestore(QWidget &window, const char *scenario) {
  const HWND hwnd = reinterpret_cast<HWND>(window.winId());
  DWORD ownedProcess = 0;
  const DWORD ownedThread = GetWindowThreadProcessId(hwnd, &ownedProcess);
  if (ownedProcess != GetCurrentProcessId() || ownedThread != GetCurrentThreadId()) return false;
  // Only raise the isolated fixture; never manipulate a user's other windows.
  SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  BringWindowToTop(hwnd);
  window.raise();
  window.activateWindow();
  SetForegroundWindow(hwnd);
  if (GetForegroundWindow() != hwnd) {
    const HWND previous = GetForegroundWindow();
    const DWORD foregroundThread = previous ? GetWindowThreadProcessId(previous, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();
    bool inputHeld = false;
    for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_SHIFT, VK_CONTROL, VK_MENU})
      inputHeld = inputHeld || (GetAsyncKeyState(key) & 0x8000);
    if (foregroundThread && foregroundThread != currentThread && !inputHeld &&
        AttachThreadInput(currentThread, foregroundThread, TRUE)) {
      SetForegroundWindow(hwnd);
      BOOL detached = AttachThreadInput(currentThread, foregroundThread, FALSE);
      if (!detached) detached = AttachThreadInput(currentThread, foregroundThread, FALSE);
      if (!detached) {
        qWarning() << "Native minimize:" << scenario << "input-queue detach failed";
        return false;
      }
    }
  }
  for (int attempt = 0; attempt < 15 && GetForegroundWindow() != hwnd; ++attempt) settle();
  settle();
  const int observeMs = std::clamp(qEnvironmentVariableIntValue("GSW_MINIMIZE_QA_OBSERVE_MS"), 0, 60000);
  if (observeMs) {
    qInfo() << "Native minimize observation:" << scenario << "PID" << ownedProcess
            << "title" << window.windowTitle() << "milliseconds" << observeMs;
    QEventLoop loop;
    QTimer::singleShot(observeMs, &loop, &QEventLoop::quit);
    loop.exec();
  }
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const HMENU menu = GetSystemMenu(hwnd, FALSE);
  const UINT menuState = menu ? GetMenuState(menu, SC_MINIMIZE, MF_BYCOMMAND) : UINT(-1);
  const bool enabled = (style & WS_MINIMIZEBOX) && menuState != UINT(-1) &&
      !(menuState & (MF_DISABLED | MF_GRAYED)) && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd);
  qInfo() << "Native minimize caption:" << scenario << "style" << Qt::hex << style
          << "system menu" << menuState << Qt::dec << "enabled" << enabled
          << "owned foreground" << (GetForegroundWindow() == hwnd);
  if (!enabled || GetForegroundWindow() != hwnd) return false;

  // UIA's own-window calls need GUI message pumping. Do not leave a detached
  // worker able to invoke a caption button after this fixture is restored.
  auto state = std::make_shared<MinimizeButtonResult>();
  std::thread worker([hwnd, state] {
    state->result = invokeOwnedMinimizeButton(hwnd, state->canceled);
    state->done.store(true, std::memory_order_release);
  });
  QElapsedTimer deadline;
  deadline.start();
  while (!state->done.load(std::memory_order_acquire) && deadline.elapsed() < 5000) settle();
  const bool completed = state->done.load(std::memory_order_acquire);
  if (!completed) {
    state->canceled.store(true);
    qWarning() << "Native minimize:" << scenario << "UIA deadline exceeded; canceling and waiting for worker cleanup";
    QElapsedTimer cleanupDeadline;
    cleanupDeadline.start();
    while (!state->done.load(std::memory_order_acquire) && cleanupDeadline.elapsed() < 2000) settle();
    if (!state->done.load(std::memory_order_acquire)) {
      // This helper is reached only by --smoke-test-window-minimize in its
      // independent QA process. Fail fast before destroying/reusing its HWND;
      // no other application or user instance is terminated.
      qCritical() << "Native minimize:" << scenario
                  << "UIA worker did not stop after cancellation; terminating only this smoke QA process";
      std::_Exit(2);
    }
  }
  worker.join();
  const HRESULT actionResult = completed ? state->result : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  for (int attempt = 0; attempt < 30 && (!IsIconic(hwnd) || !window.isMinimized()); ++attempt) settle();
  const bool minimized = completed && SUCCEEDED(actionResult) && IsIconic(hwnd) && window.isMinimized();
  qInfo() << "Native minimize result:" << scenario << "UIA Invoke HRESULT" << Qt::hex << actionResult
          << Qt::dec << "completed" << completed << "OS iconic" << bool(IsIconic(hwnd))
          << "Qt minimized" << window.isMinimized();
  // Taskbar-equivalent restore keeps the same window and its in-progress state.
  if (IsIconic(hwnd) || window.isMinimized()) PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
  for (int attempt = 0; attempt < 30 && (IsIconic(hwnd) || window.isMinimized() || !window.isVisible()); ++attempt) settle();
  settle();
  const bool restored = !IsIconic(hwnd) && !window.isMinimized() && window.isVisible();
  qInfo() << "Native restore result:" << scenario << "restored" << restored;
  return minimized && restored;
}
#endif
} // namespace

bool runWindowMinimizeSmokeTest(QMainWindow &workbench) {
  Q_UNUSED(workbench);
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { qWarning() << "Window minimize smoke:" << message; passed = false; }
  };
#ifdef Q_OS_WIN
  // Keep this seam independent of language/appearance/dock QA: unrelated
  // modal windows in those suites can prevent safe native-caption clicks.
  QTemporaryDir temporary;
  check(temporary.isValid(), "create isolated file-dialog fixture directory");
  if (!temporary.isValid()) return false;
  const QString fixtureName = QStringLiteral("模型_日本語.glb");
  const QString fixturePath = QDir(temporary.path()).filePath(fixtureName);
  QFile fixture(fixturePath);
  check(fixture.open(QIODevice::WriteOnly), "create the known Unicode file-dialog selection fixture");
  if (!fixture.isOpen()) return false;
  check(fixture.write("native caption fixture\n") == 23, "write the complete file-dialog selection fixture");
  fixture.close();

  QMainWindow host;
  host.setWindowTitle(QStringLiteral("GSW minimize QA main"));
  host.setCentralWidget(new QLineEdit(QStringLiteral("unchanged"), &host));
  host.resize(640, 450);
  host.show();
  host.activateWindow();
  settle();
  const QRect hostGeometry = host.geometry();
  check(captionMinimizeAndRestore(host, "isolated main window"),
        "main-window enabled native minimize button actually minimizes and restores through Windows");
  check(host.isVisible() && host.geometry() == hostGeometry &&
        static_cast<QLineEdit *>(host.centralWidget())->text() == QStringLiteral("unchanged"),
        "main-window native minimize and restore retain geometry and in-progress content");

  int finishedCount = 0;
  QFileDialog file(&host);
  file.setOption(QFileDialog::DontUseNativeDialog);
  file.setWindowModality(Qt::WindowModal);
  file.setAcceptMode(QFileDialog::AcceptOpen);
  file.setFileMode(QFileDialog::ExistingFile);
  file.setViewMode(QFileDialog::Detail);
  file.setNameFilters({QStringLiteral("PLY (*.ply)"), QStringLiteral("GLB (*.glb)")});
  file.selectNameFilter(QStringLiteral("GLB (*.glb)"));
  file.setDirectory(temporary.path());
  file.selectFile(fixturePath);
  file.setWindowTitle(QStringLiteral("GSW minimize QA file"));
  file.resize(AppTheme::fitWindowResolution(QSize(900, 640), file.screen()->availableGeometry().size(),
                                           QSize(640, 460)));
  QObject::connect(&file, &QDialog::finished, &file, [&](int) { ++finishedCount; });
  file.show();
  file.activateWindow();
  settle();
  auto *name = file.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
  auto *tree = file.findChild<QTreeView *>(QStringLiteral("treeView"));
  check(name && tree, "real file dialog exposes its filename editor and filesystem view");
  if (!name || !tree) return false;
  QModelIndex selectedFixture;
  for (int attempt = 0; attempt < 30 && !selectedFixture.isValid(); ++attempt) {
    for (int row = 0; row < tree->model()->rowCount(tree->rootIndex()); ++row) {
      const auto index = tree->model()->index(row, 0, tree->rootIndex());
      if (index.data().toString() == fixtureName) { selectedFixture = index; break; }
    }
    if (!selectedFixture.isValid()) settle();
  }
  check(selectedFixture.isValid(), "filesystem view actually loaded the known Unicode fixture");
  if (!selectedFixture.isValid()) return false;
  tree->selectionModel()->setCurrentIndex(selectedFixture,
      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  settle();
  const auto selectedNames = [&] {
    QStringList names;
    for (const auto &index : tree->selectionModel()->selectedRows(0)) names.append(index.data().toString());
    names.sort();
    return names;
  };
  const QString expectedDirectory = QDir(temporary.path()).absolutePath();
  const QStringList expectedSelection{QStringLiteral("模型_日本語.glb")};
  const QStringList expectedPaths{QDir::cleanPath(fixturePath)};
  const QRect expectedGeometry = file.geometry();
  check(file.directory().absolutePath() == expectedDirectory && name->text() == fixtureName &&
        file.selectedNameFilter() == QStringLiteral("GLB (*.glb)") && selectedNames() == expectedSelection &&
        file.selectedFiles() == expectedPaths && file.windowModality() == Qt::WindowModal && finishedCount == 0,
        "file-dialog minimize regression begins with the known directory, filename, selection, filter and modality");
  qInfo() << "File-dialog state before caption minimize:" << file.directory().absolutePath() << name->text()
          << file.selectedNameFilter() << selectedNames() << file.selectedFiles() << file.windowModality()
          << "geometry" << file.geometry() << "finished count" << finishedCount;
  check(captionMinimizeAndRestore(file, "isolated modal file dialog"),
        "file-dialog enabled native minimize button actually minimizes and restores through Windows");
  qInfo() << "File-dialog state after caption restore:" << file.directory().absolutePath() << name->text()
          << file.selectedNameFilter() << selectedNames() << file.selectedFiles() << file.windowModality()
          << "geometry" << file.geometry() << "finished count" << finishedCount;
  check(file.isVisible() && file.directory().absolutePath() == expectedDirectory && name->text() == fixtureName &&
        file.selectedNameFilter() == QStringLiteral("GLB (*.glb)") && selectedNames() == expectedSelection &&
        file.selectedFiles() == expectedPaths && file.windowModality() == Qt::WindowModal &&
        file.geometry() == expectedGeometry && finishedCount == 0,
        "native minimize and restore retain file-dialog directory, filename, filter, actual selection, modality and geometry without accepting or rejecting");
  file.reject();
  host.hide();
#else
  qInfo() << "Window minimize smoke: Windows native-caption seam unavailable on this platform";
#endif
  qInfo().noquote() << "Window minimize smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runWindowUiAppearanceSmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const QString locale = AppLanguage::current();
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { qWarning() << "Window appearance smoke:" << message; passed = false; }
  };
  const QString shots = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  if (!shots.isEmpty()) QDir().mkpath(shots);
  auto *import = workbench.findChild<QAction *>(QStringLiteral("importSceneAction"));
  check(import && import->isEnabled(), "real model-import action is available");
  if (!import || !import->isEnabled()) return false;

  const auto checkImportDialog = [&](const QString &suffix, const int requestedScale) {
    bool observed = false;
    QElapsedTimer deadline;
    deadline.start();
    QTimer inspect;
    inspect.setInterval(20);
    QObject::connect(&inspect, &QTimer::timeout, &workbench, [&] {
      auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
      if (!file || !file->isVisible()) {
        if (deadline.elapsed() > 2500) {
          check(false, "model-import action presents a real Qt file dialog");
          if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
          inspect.stop();
        }
        return;
      }
      // Let the dialog's own queued show/layout work finish before observing it.
      inspect.stop();
      QTimer::singleShot(80, file, [&, file] {
        observed = true;
        const int scale = qApp->property("gswUiScalePercent").toInt();
        const QRect available = file->screen()->availableGeometry();
        // Independent worked examples from the requested 640 logical-pixel
        // baseline, rather than the production AppTheme sizing helper.
        const int specifiedWidth = requestedScale == 150 ? 960 : 640;
        const int expectedWidth = std::min(specifiedWidth, available.width() - 32);
        qInfo() << "Window appearance geometry:" << file->size() << "DPR" << file->devicePixelRatioF()
                << "scale" << scale << "minimum width" << expectedWidth;
        check(file->width() >= expectedWidth,
              "model-import dialog initially has the requested wide filename area without manual resizing");
        check(available.contains(file->frameGeometry()), "initial model-import dialog stays inside the current screen");
        if (!shots.isEmpty())
          check(file->grab().save(QDir(shots).filePath(suffix + QStringLiteral("-import-default.png"))),
                "initial import-dialog screenshot");
        // The default must not become a forced minimum or reset an in-progress
        // dialog when presentation changes. Observe only public dialog state.
        const QSize userSize(std::max(file->minimumWidth(), expectedWidth - 120), file->height());
        file->resize(userSize);
        settle();
        const QSize actualUserSize = file->size();
        AppLanguage::apply(locale == QStringLiteral("en_US")
                               ? QStringLiteral("ja_JP") : QStringLiteral("en_US"), false);
        settle();
        check(file->size() == actualUserSize, "language change preserves user-resized import dialog dimensions");
        AppLanguage::apply(locale, false);
        settle();
        check(file->size() == actualUserSize, "restoring language retains the user's narrower import dialog size");
        const UiTheme originalTheme = AppTheme::currentTheme();
        auto *otherTheme = workbench.findChild<QAction *>(originalTheme == UiTheme::Light
            ? QStringLiteral("darkThemeAction") : QStringLiteral("lightThemeAction"));
        auto *restoreTheme = workbench.findChild<QAction *>(originalTheme == UiTheme::Light
            ? QStringLiteral("lightThemeAction") : QStringLiteral("darkThemeAction"));
        check(otherTheme && restoreTheme, "live appearance actions are available while importing");
        if (otherTheme && restoreTheme) {
          otherTheme->trigger(); settle();
          check(file->size() == actualUserSize, "appearance change preserves user-resized import dialog dimensions");
          restoreTheme->trigger(); settle();
          check(file->size() == actualUserSize, "restoring appearance retains the user's narrower import dialog size");
        }
        file->reject();
      });
    });
    inspect.start();
    import->trigger();
    inspect.stop();
    check(observed, "initial import-dialog geometry was observed through the real action");
  };

  auto *toolbar = workbench.findChild<QToolBar *>(QStringLiteral("selectionToolbar"));
  auto *radius = toolbar ? toolbar->findChild<QLabel *>(QStringLiteral("mutedLabel")) : nullptr;
  check(toolbar && radius, "real selection toolbar exposes its brush-radius label");
  const auto checkToolbar = [&](const QString &suffix) {
    if (toolbar && radius) {
      toolbar->show();
      workbench.showMaximized();
      settle();
      check(radius->isVisible(), "brush-radius label is visible in the actual toolbar");
      check(radius->text() == QCoreApplication::translate("Workbench", "半径"),
            "brush-radius label retains the selected live language");
      const QImage toolbarImage = toolbar->grab().toImage();
      check(!toolbarImage.isNull(), "actual toolbar and label paint into screenshots");
      if (!toolbarImage.isNull()) {
        // Observe the composed toolbar, not QLabel::grab(): a correctly
        // transparent label's isolated image legitimately has alpha zero.
        const QPoint corner = radius->mapTo(toolbar, QPoint(1, 1));
        const qreal dpr = toolbarImage.devicePixelRatio();
        const QPoint pixel(qRound(corner.x() * dpr), qRound(corner.y() * dpr));
        check(toolbarImage.rect().contains(pixel), "visible brush-radius background lies inside the real toolbar image");
        if (!toolbarImage.rect().contains(pixel)) return;
        const QColor labelBackground = toolbarImage.pixelColor(pixel);
        const QColor toolbarBackground = toolbarImage.pixelColor(3, 3);
        const int difference = std::max({std::abs(labelBackground.red() - toolbarBackground.red()),
                                         std::abs(labelBackground.green() - toolbarBackground.green()),
                                         std::abs(labelBackground.blue() - toolbarBackground.blue())});
        qInfo() << "Window appearance backgrounds:" << labelBackground << toolbarBackground << "difference" << difference;
        check(difference <= 6, "brush-radius label paints the toolbar background, not a contrasting rectangle");
        if (!shots.isEmpty())
          check(toolbarImage.save(QDir(shots).filePath(suffix + QStringLiteral("-selection-toolbar.png"))),
                "selection-toolbar background screenshot");
      }
    }
  };
  auto *light = workbench.findChild<QAction *>(QStringLiteral("lightThemeAction"));
  auto *dark = workbench.findChild<QAction *>(QStringLiteral("darkThemeAction"));
  check(light && dark, "actual light and dark appearance menu actions are available");
  if (light && dark) for (auto *theme : {light, dark}) {
    theme->trigger();
    for (const int scale : {90, 100, 150}) {
      QAction *scaleAction = nullptr;
      for (auto *action : workbench.findChildren<QAction *>())
        if (action->isCheckable() && action->data().toInt() == scale) { scaleAction = action; break; }
      check(scaleAction != nullptr, "real manual UI-scale command is available");
      if (!scaleAction) continue;
      scaleAction->trigger(); settle();
      check(qApp->property("gswUiScalePercent").toInt() == scale, "appearance matrix uses the real manual font scale");
      const QString suffix = locale + QLatin1Char('-') + theme->data().toString() + QLatin1Char('-') + QString::number(scale);
      qInfo().noquote() << "Window appearance scenario:" << suffix;
      checkImportDialog(suffix, scale);
      checkToolbar(suffix);
    }
  }
  qInfo().noquote() << "Window appearance smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}

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
  file.setViewMode(QFileDialog::Detail);
  file.setNameFilters({QStringLiteral("PLY (*.ply)"), QStringLiteral("GLB (*.glb)")});
  file.selectNameFilter(QStringLiteral("GLB (*.glb)"));
  file.setDirectory(temporary.path());
  file.setSidebarUrls({QUrl::fromLocalFile(temporary.path())});
  file.selectFile(QStringLiteral("模型_日本語.glb"));
  for (const QString &fixtureName : {QStringLiteral("模型_日本語.glb"), QStringLiteral("another.glb")}) {
    QFile fixture(QDir(temporary.path()).filePath(fixtureName));
    check(fixture.open(QIODevice::WriteOnly), "create real file-dialog selection fixture");
    fixture.write("file dialog fixture\n");
  }
  const QSize available = file.screen()->availableGeometry().size();
  const QSize normalFileSize = AppTheme::fitWindowResolution(QSize(1100, 680), available, QSize(640, 460));
  file.resize(normalFileSize);
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
  // Exercise the real QFileDialog detail view after asynchronous filesystem
  // loading, not a synthetic table with unrelated header defaults.
  auto *tree = file.findChild<QTreeView *>(QStringLiteral("treeView"));
  check(tree && tree->header()->count() >= 4, "file dialog exposes its four real detail columns");
  if (tree && tree->header()->count() >= 4) {
    const int originalScale = qApp->property("gswUiScalePercent").toInt();
    const UiThemeMode originalThemeMode = AppTheme::currentThemeMode();
    auto *header = tree->header();
    auto *detailButton = file.findChild<QToolButton *>(QStringLiteral("detailModeButton"));
    check(detailButton != nullptr, "file dialog detail-view button exists for scale checks");
    tree->sortByColumn(0, Qt::DescendingOrder);
    file.selectFile(QStringLiteral("模型_日本語.glb"));
    settle();
    QModelIndex selectedFixture;
    for (int attempt = 0; attempt < 15 && !selectedFixture.isValid(); ++attempt) {
      for (int row = 0; row < tree->model()->rowCount(tree->rootIndex()); ++row) {
        const auto index = tree->model()->index(row, 0, tree->rootIndex());
        if (index.data().toString() == QStringLiteral("模型_日本語.glb")) {
          selectedFixture = index;
          break;
        }
      }
      if (!selectedFixture.isValid()) settle();
    }
    check(selectedFixture.isValid(), "filesystem model loaded the Unicode GLB selection fixture");
    if (selectedFixture.isValid()) {
      tree->selectionModel()->setCurrentIndex(selectedFixture,
          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      settle();
    }
    const auto selectedNames = [tree] {
      QStringList values;
      for (const auto &index : tree->selectionModel()->selectedRows(0))
        values.append(index.data().toString());
      values.sort();
      return values;
    };
    const QStringList expectedSelection = selectedNames();
    check(!expectedSelection.isEmpty(), "selection preservation checks start with a real selected file");
    const QString expectedFileName = name->text();
    const QString expectedFilter = file.selectedNameFilter();
    const QString expectedDirectory = file.directory().absolutePath();
    const int expectedSortColumn = header->sortIndicatorSection();
    const auto expectedSortOrder = header->sortIndicatorOrder();
    // A deliberate user width above the 150% font minimum must not be reset by
    // repolishing, resizing or translating the dialog.
    header->resizeSection(3, 400);
    const int userDateWidth = header->sectionSize(3);
    const auto checkFileState = [&] {
      check(name->text() == expectedFileName && file.selectedNameFilter() == expectedFilter &&
            file.directory().absolutePath() == expectedDirectory && selectedNames() == expectedSelection,
            "file layout/theme/scale/language changes preserve filename, filter, directory and selection");
      check(header->sortIndicatorSection() == expectedSortColumn &&
            header->sortIndicatorOrder() == expectedSortOrder && header->sectionSize(3) == userDateWidth,
            "file layout/theme/scale/language changes preserve sorting and the user-sized date column");
    };
    const auto checkFileColumns = [&] {
      const int viewportWidth = tree->viewport()->width();
      const int minimumTotal = header->minimumSectionSize() * header->count();
      int metadataWidth = 0;
      for (int column = 1; column < header->count(); ++column)
        metadataWidth += header->sectionSize(column);
      const bool columnsShouldFit = viewportWidth >= metadataWidth + header->minimumSectionSize() &&
                                    viewportWidth >= minimumTotal;
      if (header->length() < viewportWidth || (columnsShouldFit && header->length() != viewportWidth))
        qWarning() << "File detail geometry:" << file.windowState() << qApp->property("gswUiScalePercent")
                   << header->length() << viewportWidth << header->sectionSize(0)
                   << header->sectionSize(1) << header->sectionSize(2) << header->sectionSize(3);
      check(header->sectionResizeMode(0) == QHeaderView::Stretch && !header->stretchLastSection(),
            "file name column, not the date column, absorbs available dialog width");
      for (int column = 1; column < header->count(); ++column)
        check(header->sectionResizeMode(column) == QHeaderView::Interactive,
              "file metadata columns remain user-resizable");
      // Small screens may legitimately need horizontal scrolling for the
      // explicit user width. A larger viewport must never leave a white gap.
      check(header->length() >= viewportWidth,
            "detail header covers the full file-list viewport without a trailing blank gap");
      if (columnsShouldFit)
        check(header->length() == viewportWidth && tree->horizontalScrollBar()->maximum() == 0,
              "file details use all available width without an unnecessary horizontal scrollbar");
      check(header->sectionSize(0) >= header->minimumSectionSize(), "file-name column remains usable");
      checkFileState();
    };
    QSize smallIconSize;
    for (const int scale : {90, 150, 90}) {
      file.showNormal(); file.resize(normalFileSize); settle();
      AppTheme::apply(*qApp, scale, false); settle();
      checkFileColumns();
      if (detailButton) {
        if (smallIconSize.isEmpty()) smallIconSize = detailButton->iconSize();
        else check(scale == 150 ? detailButton->iconSize().height() > smallIconSize.height()
                                : detailButton->iconSize() == smallIconSize,
                   "file navigation icons scale up at 150% and return to their 90% size");
      }
      const int normalViewportWidth = tree->viewport()->width();
      const int normalNameWidth = header->sectionSize(0);
      const bool normalColumnsFit = header->length() == normalViewportWidth;
      captionDoubleClick(file);
      check(file.isMaximized(), "file detail matrix enters maximized mode through the native caption");
      checkFileColumns();
      if (normalColumnsFit && tree->viewport()->width() > normalViewportWidth)
        check(header->sectionSize(0) > normalNameWidth, "maximizing expands the file-name column with the viewport");
      WindowUi::fullScreenAction(&file)->trigger(); settle();
      check(file.isFullScreen(), "file detail matrix enters full screen");
      checkFileColumns();
      key(*name, Qt::Key_Escape);
      check(file.isMaximized() && !file.isFullScreen(), "file detail matrix restores its maximized state after full screen");
      checkFileColumns();
      captionDoubleClick(file);
      check(!file.isMaximized() && !file.isFullScreen(), "file detail matrix returns to normal mode");
      checkFileColumns();
    }
    // A user may intentionally prefer a compact metadata column. With the
    // same font, window-state or palette changes must not silently widen it.
    const int retainedTypeWidth = header->sectionSize(2);
    const QString narrowFontKey = tree->font().key();
    const int narrowScale = qApp->property("gswUiScalePercent").toInt();
    header->resizeSection(2, 80);
    settle();
    const int narrowTypeWidth = header->sectionSize(2);
    check(narrowTypeWidth == 80 && narrowTypeWidth < retainedTypeWidth,
          "narrow-column regression starts with a real user-sized type column below the automatic width");
    captionDoubleClick(file);
    check(file.isMaximized() && header->sectionSize(2) == narrowTypeWidth,
          "same-font maximization preserves the user's narrow type column");
    checkFileColumns();
    captionDoubleClick(file);
    check(!file.isMaximized() && !file.isFullScreen() && header->sectionSize(2) == narrowTypeWidth,
          "same-font restoration preserves the user's narrow type column");
    checkFileColumns();
    for (const UiTheme theme : {UiTheme::Dark, UiTheme::Light}) {
      AppTheme::applyTheme(*qApp, theme, false); settle();
      check(tree->font().key() == narrowFontKey && qApp->property("gswUiScalePercent").toInt() == narrowScale &&
            header->sectionSize(2) == narrowTypeWidth,
            "day/night palette changes preserve the user's narrow type column without changing font or scale");
      checkFileColumns();
    }
    header->resizeSection(2, retainedTypeWidth);
    settle();
    for (const QString &language : AppLanguage::supported()) {
      AppLanguage::apply(language, false); settle();
      checkFileColumns();
      file.showMaximized(); settle();
      checkFileColumns();
      if (!shots.isEmpty())
        check(file.grab().save(QDir(shots).filePath(language + QStringLiteral("-file-dialog-maximized-light.png"))),
              "maximized light file dialog screenshot for each language");
      file.showNormal(); settle();
    }
    AppLanguage::apply(locale, false);
    AppTheme::applyThemeMode(*qApp, originalThemeMode, false);
    if (auto *themeTimer = workbench.findChild<QTimer *>(QStringLiteral("automaticThemeTimer"))) {
      check(QMetaObject::invokeMethod(themeTimer, "timeout", Qt::DirectConnection),
            "restore the appearance schedule after non-persistent palette QA overrides");
      check(themeTimer->isActive() == (originalThemeMode == UiThemeMode::Automatic),
            "file-dialog palette QA retains the original automatic/manual schedule policy");
    }
    AppTheme::apply(*qApp, originalScale, false);
    settle();
    checkFileState();
  }
  if (!shots.isEmpty()) {
    QDir().mkpath(shots);
    check(file.grab().save(QDir(shots).filePath(locale + QStringLiteral("-file-dialog.png"))), "file-dialog screenshot");
  }
  file.hide();
  // Covers Qt static directory/open dialogs as well as the save path.
  for (const auto mode : {QFileDialog::ExistingFiles, QFileDialog::Directory}) {
    QFileDialog open(&host);
    open.setOption(QFileDialog::DontUseNativeDialog);
    open.setViewMode(QFileDialog::Detail);
    open.setFileMode(mode); open.setDirectory(temporary.path());
    open.resize(normalFileSize);
    open.show(); settle();
    check(open.sidebarUrls().contains(QUrl::fromLocalFile(desktop)), "open and folder dialog desktop shortcut");
    auto *openTree = open.findChild<QTreeView *>(QStringLiteral("treeView"));
    check(openTree && openTree->header()->count() >= 4, "open and folder dialogs expose the same detail columns");
    if (openTree && openTree->header()->count() >= 4) {
      auto *header = openTree->header();
      header->resizeSection(3, 400);
      const int userWidth = header->sectionSize(3);
      openTree->sortByColumn(0, Qt::DescendingOrder);
      const auto directory = open.directory().absolutePath();
      const auto filter = open.selectedNameFilter();
      const int initialScale = qApp->property("gswUiScalePercent").toInt();
      const auto checkDetails = [&] {
        const int viewportWidth = openTree->viewport()->width();
        int metadataWidth = 0;
        for (int column = 1; column < header->count(); ++column) {
          metadataWidth += header->sectionSize(column);
          check(header->sectionResizeMode(column) == QHeaderView::Interactive,
                "open/folder metadata columns remain user-resizable");
        }
        check(header->sectionResizeMode(0) == QHeaderView::Stretch && !header->stretchLastSection() &&
              header->length() >= viewportWidth,
              "open/folder name column expands without a trailing header gap");
        if (viewportWidth >= metadataWidth + header->minimumSectionSize())
          check(header->length() == viewportWidth && openTree->horizontalScrollBar()->maximum() == 0,
                "open/folder details avoid unnecessary horizontal scrolling");
        check(header->sectionSize(3) == userWidth && header->sortIndicatorSection() == 0 &&
              header->sortIndicatorOrder() == Qt::DescendingOrder &&
              open.directory().absolutePath() == directory && open.selectedNameFilter() == filter,
              "open/folder resizing and scale changes preserve user column widths, sorting, directory and filter");
      };
      for (const int scale : {90, 150, 90}) {
        open.showNormal(); open.resize(normalFileSize); settle();
        AppTheme::apply(*qApp, scale, false); settle();
        checkDetails();
        const int normalViewport = openTree->viewport()->width();
        const int normalFirstColumn = header->sectionSize(0);
        const bool columnsFit = header->length() == normalViewport;
        open.showMaximized(); settle();
        check(open.isMaximized(), "open/folder details maximize");
        checkDetails();
        if (columnsFit && openTree->viewport()->width() > normalViewport)
          check(header->sectionSize(0) > normalFirstColumn, "open/folder file-name column follows viewport expansion");
        WindowUi::fullScreenAction(&open)->trigger(); settle();
        check(open.isFullScreen(), "open/folder details enter full screen");
        checkDetails();
        WindowUi::fullScreenAction(&open)->trigger(); settle();
        check(open.isMaximized() && !open.isFullScreen(), "open/folder details restore their maximized state");
        checkDetails();
      }
      open.showNormal();
      AppTheme::apply(*qApp, initialScale, false); settle();
      checkDetails();
    }
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
