#include "WindowUi.h"
#include "AppLanguage.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QKeyEvent>
#include <QLayout>
#include <QMainWindow>
#include <QListView>
#include <QMessageBox>
#include <QPointer>
#include <QPainter>
#include <QStandardPaths>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUrl>
#include <QVariant>

namespace gsw {
namespace {
constexpr auto controllerName = "gswWindowController";
QIcon windowIcon(bool restore, bool fullScreen = false) {
  QPixmap pixmap(36, 36);
  pixmap.setDevicePixelRatio(2);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setPen(QPen(QColor(210, 220, 225), 1.5));
  if (restore) {
    painter.drawLine(QPointF(6, 3), QPointF(15, 3));
    painter.drawLine(QPointF(15, 3), QPointF(15, 12));
    painter.drawRect(QRectF(3, 6, 9, 9));
  } else if (fullScreen) {
    for (const QPoint &corner : {QPoint(3, 3), QPoint(15, 3), QPoint(3, 15), QPoint(15, 15)}) {
      painter.drawLine(corner, corner + QPoint(corner.x() == 3 ? 4 : -4, 0));
      painter.drawLine(corner, corner + QPoint(0, corner.y() == 3 ? 4 : -4));
    }
  } else painter.drawRect(QRectF(3, 3, 12, 12));
  return QIcon(pixmap);
}
bool eligible(QWidget *w) {
  return w && w->isWindow() && (qobject_cast<QDialog *>(w) ||
      qobject_cast<QMainWindow *>(w) || qobject_cast<QDockWidget *>(w));
}

class WindowController final : public QObject {
public:
  explicit WindowController(QWidget *window) : QObject(window), mWindow(window) {
    setObjectName(QLatin1String(controllerName));
    mFullScreen = new QAction(this);
    mFullScreen->setObjectName(QStringLiteral("windowFullScreenAction"));
    mFullScreen->setShortcut(QKeySequence(QStringLiteral("F11")));
    mFullScreen->setShortcutContext(Qt::WindowShortcut);
    window->addAction(mFullScreen);
    connect(mFullScreen, &QAction::triggered, this, [this] { toggleFullScreen(); });
    if (auto *dock = qobject_cast<QDockWidget *>(window))
      connect(dock, &QDockWidget::topLevelChanged, this, [this] { refresh(); });
    window->installEventFilter(this);
    AppLanguage::onChanged(this, [this] { refresh(); });
    refresh();
  }

  QAction *action() const { return mFullScreen; }

  void decorate() {
    if (!eligible(mWindow)) return;
    // Do this before a native handle is shown: changing flags on a visible
    // dialog hides it and can prematurely end its modal event loop.
    // QDockWidget owns its flags and native drag state. Never recreate its
    // platform window while Qt is unplugging or dragging a panel.
    if (!mWindow->isVisible() && !qobject_cast<QDockWidget *>(mWindow)) {
      auto flags = mWindow->windowFlags();
      flags |= Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint;
      flags &= ~Qt::WindowContextHelpButtonHint;
      if (!qobject_cast<QDialog *>(mWindow)) flags |= Qt::WindowMinimizeButtonHint;
      if (flags != mWindow->windowFlags()) mWindow->setWindowFlags(flags);
    }
    if (auto *file = qobject_cast<QFileDialog *>(mWindow)) {
      auto urls = file->sidebarUrls();
      const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
      if (!desktop.isEmpty()) {
        const QUrl url = QUrl::fromLocalFile(desktop);
        urls.removeAll(url);
        urls.prepend(url);
        file->setSidebarUrls(urls);
      }
    }
    // The OS caption is the sole window-control bar. Desktop remains a
    // sidebar location; F11 is an action, not another row above the contents.
    refresh();
  }

  void toggleFullScreen() {
    ++mStateRevision;
    const QScopedValueRollback<bool> changingState(mChangingState, true);
    if (auto *dock = qobject_cast<QDockWidget *>(mWindow); dock && !dock->isFloating())
      dock->setFloating(true);
    if (mWindow->isFullScreen()) {
      mWindow->showNormal();
      if (mNormalGeometry.isValid()) mWindow->setGeometry(mNormalGeometry);
      if (mWasMaximized) mWindow->showMaximized();
    } else {
      mWasMaximized = mWindow->isMaximized();
      mNormalGeometry = mWasMaximized ? mWindow->normalGeometry() : mWindow->geometry();
      allowExpansion();
      mWindow->showFullScreen();
    }
    refresh();
  }

  void toggleMaximized() {
    ++mStateRevision;
    const QScopedValueRollback<bool> changingState(mChangingState, true);
    if (mWindow->isFullScreen()) {
      // The restore button always returns to a normal, resizable window.
      mWasMaximized = false;
      toggleFullScreen();
    } else if (mWindow->isMaximized()) {
      mWindow->showNormal();
    } else {
      allowExpansion();
      mWindow->showMaximized();
    }
    refresh();
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched == mWindow && (event->type() == QEvent::LanguageChange ||
        event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)) {
      const Qt::WindowStates state = mWindow->windowState();
      const auto revision = mStateRevision;
      QTimer::singleShot(0, this, [this, state, revision] {
        decorate();
        // Some Qt dialogs rebuild their layout and call setFixedSize while
        // translating/repolishing. Restore presentation after that work,
        // unless the user has explicitly requested a different window state.
        if (mStateRevision == revision && mWindow->isVisible() && qobject_cast<QMessageBox *>(mWindow) &&
            (state.testFlag(Qt::WindowFullScreen) || state.testFlag(Qt::WindowMaximized))) {
          const QScopedValueRollback<bool> changing(mChangingState, true);
          allowExpansion();
          if (state.testFlag(Qt::WindowFullScreen)) mWindow->showFullScreen();
          else mWindow->showMaximized();
        }
        refresh();
      });
    }
    if (watched == mWindow && event->type() == QEvent::Show && mChangingState &&
        qobject_cast<QMessageBox *>(mWindow)) {
      // A visible message box receives another Show when changing window
      // state; its override would call setFixedSize and undo full screen.
      // Its initial show (escape/default buttons, accessibility) is untouched.
      return true;
    }
    if (watched == mWindow && event->type() == QEvent::LayoutRequest &&
        qobject_cast<QMessageBox *>(mWindow) && (mChangingState || mWindow->isFullScreen() || mWindow->isMaximized())) {
      // QMessageBox normally re-applies setFixedSize on every layout request.
      // Activate its layout without shrinking a user-expanded window.
      allowExpansion();
      mWindow->layout()->activate();
      return true;
    }
    if (watched == mWindow && event->type() == QEvent::WindowStateChange) refresh();
    return false;
  }

private:
  void allowExpansion() {
    if (auto *layout = mWindow->layout(); layout && layout->sizeConstraint() == QLayout::SetFixedSize)
      layout->setSizeConstraint(QLayout::SetMinimumSize);
    mWindow->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  }
  void refresh() {
    const bool full = mWindow->isFullScreen();
    AppLanguage::bind(mFullScreen, "text", full ? AppLanguage::source("退出全屏") : AppLanguage::source("全屏"));
    AppLanguage::bind(mFullScreen, "toolTip", AppLanguage::source("F11 切换全屏；Esc 退出全屏；双击系统标题栏最大化或还原"));
    mFullScreen->setIcon(windowIcon(full, true));
    if (auto *dock = qobject_cast<QDockWidget *>(mWindow)) mFullScreen->setShortcut(dock->isFloating() ? QKeySequence(QStringLiteral("F11")) : QKeySequence());
    refreshDesktopLabel();
  }
  void refreshDesktopLabel() {
    if (auto *file = qobject_cast<QFileDialog *>(mWindow)) {
      // Qt's URL sidebar stores filesystem display names. Translate only the
      // well-known Desktop shortcut, never arbitrary folders or user files.
      if (auto *sidebar = file->findChild<QListView *>(QStringLiteral("sidebar")); sidebar && sidebar->model()) {
        if (mSidebarModel != sidebar->model()) {
          mSidebarModel = sidebar->model();
          connect(mSidebarModel, &QAbstractItemModel::dataChanged, this, [this] { refreshDesktopLabel(); });
          connect(mSidebarModel, &QAbstractItemModel::modelReset, this, [this] { refreshDesktopLabel(); });
          connect(mSidebarModel, &QAbstractItemModel::rowsInserted, this, [this] { refreshDesktopLabel(); });
        }
        const QUrl desktop = QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        constexpr int urlRole = Qt::UserRole + 1; // Qt QFileDialog's URL model
        for (int row = 0; row < sidebar->model()->rowCount(); ++row) {
          const auto index = sidebar->model()->index(row, 0);
          const QString label = QCoreApplication::translate("Workbench", "桌面");
          if (index.data(urlRole).toUrl() == desktop) {
            if (index.data().toString() != label)
              sidebar->model()->setData(index, label, Qt::DisplayRole);
            const QString tip = QCoreApplication::translate("Workbench", "转到桌面，保留当前文件名和文件类型");
            if (index.data(Qt::ToolTipRole).toString() != tip)
              sidebar->model()->setData(index, tip, Qt::ToolTipRole);
          }
        }
      }
    }
  }
  QWidget *mWindow;
  QAction *mFullScreen;
  QPointer<QAbstractItemModel> mSidebarModel;
  QRect mNormalGeometry;
  bool mWasMaximized = false;
  bool mChangingState = false;
  quint64 mStateRevision = 0;
};

WindowController *controller(QWidget *w) {
  if (!w) return nullptr;
  auto *existing = w->findChild<QObject *>(QLatin1String(controllerName), Qt::FindDirectChildrenOnly);
  return existing ? static_cast<WindowController *>(existing) : new WindowController(w);
}

class WindowPolicy final : public QObject {
public:
  explicit WindowPolicy(QObject *parent) : QObject(parent) {}
  bool eventFilter(QObject *watched, QEvent *event) override {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget) return false;
    if ((event->type() == QEvent::Polish || event->type() == QEvent::Show) && eligible(widget))
      WindowUi::prepare(widget);
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) &&
        eligible(widget->window()) && widget->window()->isFullScreen()) {
      auto *key = static_cast<QKeyEvent *>(event);
      if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
        if (event->type() == QEvent::KeyPress) WindowUi::toggleFullScreen(widget->window());
        event->accept();
        return true;
      }
    }
    return false;
  }
};
} // namespace

void WindowUi::install() {
  static QPointer<WindowPolicy> policy;
  if (policy) return;
  policy = new WindowPolicy(qApp);
  qApp->installEventFilter(policy);
}
void WindowUi::prepare(QWidget *window) { if (eligible(window)) controller(window)->decorate(); }
QAction *WindowUi::fullScreenAction(QWidget *window) { return controller(window)->action(); }
void WindowUi::toggleFullScreen(QWidget *window) { controller(window)->toggleFullScreen(); }
void WindowUi::toggleMaximized(QWidget *window) { controller(window)->toggleMaximized(); }
} // namespace gsw
