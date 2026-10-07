#include "WindowUi.h"
#include "AppLanguage.h"
#include "AppTheme.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLayout>
#include <QMainWindow>
#include <QListView>
#include <QMessageBox>
#include <QPointer>
#include <QPainter>
#include <QPalette>
#include <QStandardPaths>
#include <QScopedValueRollback>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace gsw {
namespace {
constexpr auto controllerName = "gswWindowController";
QIcon windowIcon(bool restore, bool fullScreen = false) {
  QIcon icon;
  const QPalette palette = qApp->palette();
  for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
    QPixmap pixmap(36, 36);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor foreground = mode == QIcon::Disabled
        ? palette.color(QPalette::Disabled, QPalette::ButtonText)
        : mode == QIcon::Selected ? palette.color(QPalette::Active, QPalette::HighlightedText)
                                 : palette.color(QPalette::Active, QPalette::ButtonText);
    painter.setPen(QPen(foreground, 1.5));
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
    icon.addPixmap(pixmap, mode);
  }
  return icon;
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
        if (urls != file->sidebarUrls()) file->setSidebarUrls(urls);
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
    if (watched == mWindow && event->type() == QEvent::Show &&
        qobject_cast<QFileDialog *>(mWindow)) {
      // QFileDialog restores its native saved header state in showEvent,
      // after application polish/show filters. Apply the layout contract once
      // that restoration has finished, rather than racing its initial setup.
      QTimer::singleShot(0, this, [this] {
        refreshFileLayout();
        applyInitialFileWidth();
      });
    }
    if (watched == mWindow && (event->type() == QEvent::LanguageChange ||
        event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)) {
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
  void applyInitialFileWidth() {
    auto *file = qobject_cast<QFileDialog *>(mWindow);
    if (!file || mInitialFileWidthApplied) return;
    mInitialFileWidthApplied = true;
    if (file->isMaximized() || file->isFullScreen() || !file->screen()) return;
    const QRect available = file->screen()->availableGeometry();
    const int scale = qApp->property("gswUiScalePercent").toInt();
    // A compact font must not reduce the user's requested filename area.
    // Apply after Qt restores its dialog geometry, once per dialog instance.
    // This is an initial size, not a minimum: later user resizing is untouched.
    const int preferred = std::max(640, AppTheme::scaled(640, scale));
    const int width = std::min(std::max(file->width(), preferred),
                               std::max(1, available.width() - 32));
    if (width == file->width()) return;
    const QPoint center = file->frameGeometry().center();
    file->resize(width, file->height());
    QRect frame = file->frameGeometry();
    frame.moveCenter(center);
    const QRect inset = available.adjusted(16, 16, -16, -16);
    const int left = std::clamp(frame.left(), inset.left(),
                               std::max(inset.left(), inset.right() - frame.width() + 1));
    const int top = std::clamp(frame.top(), inset.top(),
                              std::max(inset.top(), inset.bottom() - frame.height() + 1));
    file->move(left, top);
  }
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
    refreshFileLayout();
  }
  void refreshFileLayout() {
    auto *file = qobject_cast<QFileDialog *>(mWindow);
    if (!file) return;
    const int scale = qApp->property("gswUiScalePercent").toInt();
    // Qt's default detail columns keep their initial widths even when the
    // dialog is maximized. Give the spare width to filenames, not metadata.
    // Configure once; subsequent language/theme changes retain user widths,
    // sort order, selected files, directory, filename and filter.
    auto *tree = file->findChild<QTreeView *>(QStringLiteral("treeView"));
    if (tree && tree->header()->count() > 0) {
      auto *header = tree->header();
      const bool initialize = mFileTree != tree;
      QString metricsSignature = tree->font().key() + QString::number(scale);
      for (int column = 0; column < header->count(); ++column)
        metricsSignature += QChar(0x1f) + tree->model()->headerData(column, Qt::Horizontal).toString();
      const bool metricsChanged = initialize || metricsSignature != mFileMetricsSignature;
      if (initialize) {
        mFileTree = tree;
        for (int column = 1; column < header->count(); ++column) {
          header->setSectionResizeMode(column, QHeaderView::Interactive);
          tree->resizeColumnToContents(column);
        }
      }
      if (header->stretchLastSection()) header->setStretchLastSection(false);
      if (header->sectionResizeMode(0) != QHeaderView::Stretch)
        header->setSectionResizeMode(0, QHeaderView::Stretch);
      for (int column = 1; column < header->count(); ++column)
        if (header->sectionResizeMode(column) != QHeaderView::Interactive)
          header->setSectionResizeMode(column, QHeaderView::Interactive);
      header->setMinimumSectionSize(std::max(AppTheme::scaled(40, scale),
                                             header->fontMetrics().height() * 2));
      for (int column = 1; metricsChanged && column < header->count(); ++column) {
        const QFontMetrics metrics(tree->font());
        const int sampleWidth = column == 3
            ? metrics.horizontalAdvance(QStringLiteral("0000/00/00 00:00"))
            : column == 1
            ? metrics.horizontalAdvance(QStringLiteral("0000.00 MB"))
            : AppTheme::scaled(110, scale);
        const int minimum = std::max(header->sectionSizeHint(column),
                                      sampleWidth + AppTheme::scaled(16, scale));
        if (header->sectionSize(column) < minimum) header->resizeSection(column, minimum);
      }
      mFileMetricsSignature = metricsSignature;
    }
    // Navigation controls stay compact but follow the actual selected font
    // and UI scale. Their size is independent of the maximized window size.
    const int icon = std::max(AppTheme::scaled(20, scale), file->fontMetrics().height());
    for (auto *button : file->findChildren<QToolButton *>()) {
      button->setIconSize(QSize(icon, icon));
      const int minimum = icon + AppTheme::scaled(10, scale);
      button->setMinimumSize(minimum, minimum);
    }
    if (auto *sidebar = file->findChild<QListView *>(QStringLiteral("sidebar"))) {
      const int hint = sidebar->sizeHintForColumn(0) + AppTheme::scaled(16, scale);
      sidebar->setMinimumWidth(std::clamp(hint, AppTheme::scaled(120, scale),
                                                AppTheme::scaled(220, scale)));
    }
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
  QPointer<QTreeView> mFileTree;
  QString mFileMetricsSignature;
  QRect mNormalGeometry;
  bool mWasMaximized = false;
  bool mChangingState = false;
  bool mInitialFileWidthApplied = false;
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
