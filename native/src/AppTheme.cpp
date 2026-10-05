#include "AppTheme.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QtMath>

#include <algorithm>
#include <initializer_list>

namespace gsw {

namespace {
constexpr int kMinimumScale = 90;
constexpr int kMaximumScale = 150;

int clampScale(const int value) {
  return std::clamp(value, kMinimumScale, kMaximumScale);
}

QString themeName(const UiTheme theme) {
  return theme == UiTheme::Light ? QStringLiteral("light")
                               : QStringLiteral("dark");
}
} // namespace

UiTheme AppTheme::loadTheme() {
  const QString value =
      QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("dark"))
          .toString();
  return value == QStringLiteral("light") ? UiTheme::Light : UiTheme::Dark;
}

UiTheme AppTheme::currentTheme() {
  if (QCoreApplication::instance() != nullptr) {
    const QString value = QCoreApplication::instance()->property("gswUiTheme").toString();
    if (value == QStringLiteral("light")) {
      return UiTheme::Light;
    }
    if (value == QStringLiteral("dark")) {
      return UiTheme::Dark;
    }
  }
  return loadTheme();
}

QPalette AppTheme::palette(const UiTheme theme) {
  const bool light = theme == UiTheme::Light;
  QPalette result;
  const QColor window(light ? "#ffffff" : "#1b1d1f");
  const QColor base(light ? "#ffffff" : "#151719");
  const QColor text(light ? "#171717" : "#e7e9ea");
  const QColor muted(light ? "#444444" : "#a2aaaf");
  const QColor disabled(light ? "#6b6b6b" : "#929a9f");
  const QColor disabledBase(light ? "#ededed" : "#242629");
  const QColor button(light ? "#f3f3f3" : "#2a2e31");
  const QColor accent(light ? "#2467a5" : "#327e74");
  for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive,
                                         QPalette::Disabled}) {
    const bool isDisabled = group == QPalette::Disabled;
    const QColor foreground = isDisabled ? disabled : text;
    result.setColor(group, QPalette::Window, window);
    result.setColor(group, QPalette::WindowText, foreground);
    result.setColor(group, QPalette::Base, isDisabled ? disabledBase : base);
    result.setColor(group, QPalette::AlternateBase,
                    QColor(light ? "#f5f5f5" : "#1e2123"));
    result.setColor(group, QPalette::Text, foreground);
    result.setColor(group, QPalette::Button, isDisabled ? disabledBase : button);
    result.setColor(group, QPalette::ButtonText, foreground);
    result.setColor(group, QPalette::BrightText, QColor(Qt::white));
    result.setColor(group, QPalette::Highlight,
                    isDisabled ? QColor(light ? "#666666" : "#445b54") : accent);
    result.setColor(group, QPalette::HighlightedText, QColor(Qt::white));
    result.setColor(group, QPalette::ToolTipBase,
                    QColor(light ? "#fffff5" : "#2c3033"));
    result.setColor(group, QPalette::ToolTipText, foreground);
    result.setColor(group, QPalette::PlaceholderText, isDisabled ? disabled : muted);
    result.setColor(group, QPalette::Link,
                    QColor(light ? "#135f9b" : "#70cdb4"));
    result.setColor(group, QPalette::LinkVisited,
                    QColor(light ? "#64518a" : "#c7a9e9"));
    result.setColor(group, QPalette::Light,
                    QColor(light ? "#ffffff" : "#5a6268"));
    result.setColor(group, QPalette::Midlight,
                    QColor(light ? "#e0e0e0" : "#41464b"));
    result.setColor(group, QPalette::Mid,
                    QColor(light ? "#a0a0a0" : "#34383c"));
    result.setColor(group, QPalette::Dark,
                    QColor(light ? "#8a8a8a" : "#111315"));
    result.setColor(group, QPalette::Shadow,
                    QColor(light ? "#656565" : "#090b0d"));
    result.setColor(group, QPalette::Accent, accent);
  }
  return result;
}

void AppTheme::applyTheme(QApplication &application, const UiTheme theme,
                          const bool persist) {
  application.setProperty("gswUiTheme", themeName(theme));
  if (persist) {
    QSettings().setValue(QStringLiteral("ui/theme"), themeName(theme));
  }
  const QVariant scale = application.property("gswUiScalePercent");
  apply(application, scale.isValid() ? scale.toInt() : 100, false);
}

UiScaleMode AppTheme::loadScaleMode() {
  const QString mode =
      QSettings().value(QStringLiteral("ui/scaleMode"),
                        QStringLiteral("automatic"))
          .toString();
  return mode == QStringLiteral("manual") ? UiScaleMode::Manual
                                           : UiScaleMode::Automatic;
}

int AppTheme::recommendedScalePercent(const QScreen *screen) {
  if (screen == nullptr) {
    return 100;
  }
  const QSize availableSize = screen->availableGeometry().size();
  return recommendedScalePercent(availableSize, availableSize,
                                 screen->devicePixelRatio());
}

int AppTheme::recommendedScalePercent(const QSize &availableSize,
                                      const QSize &windowSize) {
  return recommendedScalePercent(availableSize, windowSize, 1.0);
}

int AppTheme::recommendedScalePercent(const QSize &availableSize,
                                      const QSize &windowSize,
                                      const double devicePixelRatio) {
  QSize basis = windowSize.isValid() ? windowSize : availableSize;
  if (availableSize.isValid()) {
    basis = basis.boundedTo(availableSize);
  }
  if (!basis.isValid()) {
    return 100;
  }

  int scale = 125;
  if (basis.width() <= 1280 || basis.height() <= 720) {
    scale = 90;
  } else if (basis.width() < 1500 || basis.height() < 820) {
    scale = 95;
  } else if (basis.width() < 2200 || basis.height() < 1200) {
    scale = 100;
  } else if (basis.width() < 3200 || basis.height() < 1750) {
    scale = 110;
  }

  if (devicePixelRatio >= 1.75) {
    scale -= 15;
  } else if (devicePixelRatio >= 1.4) {
    scale -= 10;
  } else if (devicePixelRatio >= 1.2) {
    scale -= 5;
  }
  return clampScale(scale);
}

QSize AppTheme::fitWindowResolution(const QSize &requestedSize,
                                    const QSize &availableSize,
                                    const QSize &minimumSize) {
  if (!availableSize.isValid()) {
    return requestedSize.expandedTo(minimumSize);
  }

  const QSize maximumSize(
      std::max(1, qFloor(availableSize.width() * 0.96)),
      std::max(1, qFloor(availableSize.height() * 0.96)));
  QSize fitted = requestedSize.isValid() ? requestedSize : maximumSize;
  if (fitted.width() > maximumSize.width() ||
      fitted.height() > maximumSize.height()) {
    fitted.scale(maximumSize, Qt::KeepAspectRatio);
  }

  const QSize boundedMinimum = minimumSize.boundedTo(maximumSize);
  fitted = fitted.expandedTo(boundedMinimum).boundedTo(maximumSize);
  return fitted;
}

int AppTheme::rescaledDockExtent(const int currentExtent,
                                 const int fromScalePercent,
                                 const int toScalePercent,
                                 const int minimumExtent) {
  if (fromScalePercent <= 0) {
    return std::max(currentExtent, minimumExtent);
  }
  const int target = qRound(
      static_cast<double>(currentExtent) * clampScale(toScalePercent) /
      clampScale(fromScalePercent));
  return std::max(target, minimumExtent);
}

int AppTheme::dockTitleFontPixelSize(const int scalePercent) {
  return scaled(15, scalePercent);
}

int AppTheme::dockTitleHeight(const int scalePercent) {
  return scaled(21, scalePercent);
}

int AppTheme::dockTitleButtonSize(const int scalePercent) {
  return scaled(15, scalePercent);
}

int AppTheme::loadScalePercent(const QScreen *screen) {
  Q_UNUSED(screen);
  QSettings settings;
  return clampScale(
      settings.value(QStringLiteral("ui/manualScalePercent"), 100).toInt());
}

void AppTheme::saveScaleMode(const UiScaleMode mode) {
  QSettings().setValue(QStringLiteral("ui/scaleMode"),
                       mode == UiScaleMode::Manual
                           ? QStringLiteral("manual")
                           : QStringLiteral("automatic"));
}

int AppTheme::scaled(const int value, const int scalePercent) {
  return std::max(1, qRound(static_cast<double>(value) * clampScale(scalePercent) / 100.0));
}

void AppTheme::apply(QApplication &application, const int scalePercent, const bool persist) {
  const int scale = clampScale(scalePercent);
  const UiTheme theme = currentTheme();
  application.setProperty("gswUiTheme", themeName(theme));
  application.setPalette(palette(theme));
  QFont font(QStringLiteral("Microsoft YaHei UI"));
  const QString language = application.property("gswUiLanguage").toString();
  if (language == QStringLiteral("ja_JP")) {
    font.setFamilies({QStringLiteral("Yu Gothic UI"), QStringLiteral("Meiryo"),
                      QStringLiteral("Microsoft YaHei UI")});
  } else if (language == QStringLiteral("en_US")) {
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI")});
  }
  font.setPointSizeF(10.0 * scale / 100.0);
  font.setStyleStrategy(QFont::PreferAntialias);
  application.setFont(font);
  application.setStyleSheet(styleSheet(scale, theme));
  application.setProperty("gswUiScalePercent", scale);

  if (persist) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/manualScalePercent"), scale);
  }
}

QString AppTheme::styleSheet(const int scalePercent, const UiTheme theme) {
  QString css = QStringLiteral(R"CSS(
QWidget {
  background: @SURFACE@;
  color: @TEXT@;
  selection-background-color: @SELECTION@;
  selection-color: #ffffff;
}
QWidget:disabled { color: @DISABLED_TEXT@; }
QMainWindow, QDialog { background: @WINDOW@; }
QMenuBar {
  background: @BAR@;
  border-bottom: 1px solid @BORDER@;
  padding: @MENU_PAD@px;
}
QMenuBar::item { padding: @MENU_ITEM_V@px @MENU_ITEM_H@px; background: transparent; }
QMenuBar::item:selected { background: @HOVER@; }
QMenuBar::item:disabled { color: @DISABLED_TEXT@; }
QMenu {
  background: @MENU@;
  border: 1px solid @FIELD_BORDER@;
  padding: 4px;
}
QMenu::item { padding: @MENU_ROW_V@px @MENU_ROW_H@px; border-radius: 3px; }
QMenu::item:selected { background: @MENU_SELECTION@; }
QMenu::item:disabled { color: @DISABLED_TEXT@; }
QMenu::separator { height: 1px; background: @SEPARATOR@; margin: 4px 8px; }
QToolBar {
  background: @BAR@;
  border: 0;
  border-bottom: 1px solid @BORDER@;
  spacing: @TOOL_GAP@px;
  padding: @TOOL_PAD@px;
}
QToolBar::separator { width: 1px; background: @SEPARATOR@; margin: 4px 6px; }
QToolButton {
  background: transparent;
  border: 1px solid transparent;
  border-radius: 4px;
  padding: @BUTTON_PAD@px;
}
QToolButton:hover { background: @HOVER@; border-color: @HOVER_BORDER@; }
QToolButton:pressed, QToolButton:checked { background: @PRESSED@; border-color: @ACCENT@; }
QToolButton:focus { border-color: @FOCUS@; }
QToolButton:disabled { color: @DISABLED_TEXT@; }
QPushButton {
  min-height: @CONTROL_HEIGHT@px;
  padding: 0 @CONTROL_PAD@px;
  background: @BUTTON@;
  border: 1px solid @BUTTON_BORDER@;
  border-radius: 4px;
}
QPushButton:hover { background: @BUTTON_HOVER@; border-color: @BUTTON_HOVER_BORDER@; }
QPushButton:pressed, QPushButton:checked { background: @PRESSED@; border-color: @ACCENT@; }
QPushButton:focus, QPushButton:default { border-color: @FOCUS@; }
QPushButton:disabled { color: @DISABLED_TEXT@; background: @DISABLED_SURFACE@; border-color: @DISABLED_BORDER@; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
  min-height: @CONTROL_HEIGHT@px;
  background: @FIELD@;
  border: 1px solid @FIELD_BORDER@;
  border-radius: 4px;
  padding: 0 @INPUT_PAD@px;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
  border-color: @FOCUS@;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
  color: @DISABLED_TEXT@;
  background: @DISABLED_SURFACE@;
  border-color: @DISABLED_BORDER@;
}
QComboBox::drop-down { border: 0; width: @COMBO_ARROW@px; }
QAbstractItemView {
  background: @ITEM_VIEW@;
  alternate-background-color: @ALTERNATE@;
  border: 0;
  outline: 0;
}
QTreeView::item, QTableView::item { min-height: @ROW_HEIGHT@px; }
QTreeView::item:hover, QTableView::item:hover { background: @ITEM_HOVER@; }
QTreeView::item:selected, QTableView::item:selected { background: @PRESSED@; color: @TEXT@; }
QListView::item:hover { background: @ITEM_HOVER@; }
QListView::item:selected { background: @PRESSED@; color: @TEXT@; }
QHeaderView::section {
  background: @HEADER@;
  color: @HEADER_TEXT@;
  border: 0;
  border-right: 1px solid @HEADER_BORDER@;
  border-bottom: 1px solid @HEADER_BORDER@;
  padding: @HEADER_PAD@px;
}
QDockWidget { color: @TITLE_TEXT@; }
QDockWidget::title {
  background: @TITLE@;
  border-bottom: 1px solid @TITLE_BORDER@;
  padding: @DOCK_FALLBACK_V@px @DOCK_FALLBACK_H@px;
  font-size: @DOCK_FALLBACK_FONT@px;
  font-weight: 400;
  text-align: left;
}
QWidget#dockTitleBar {
  background: @TITLE@;
  border: 0;
  border-bottom: 1px solid @TITLE_BORDER@;
}
QLabel#dockTitleLabel {
  background: transparent;
  color: @TITLE_TEXT@;
  border: 0;
  padding: 0;
}
QToolButton#dockTitleButton {
  background: transparent;
  border: 0;
  border-radius: 2px;
  padding: 0;
}
QToolButton#dockTitleButton:hover { background: @BUTTON_HOVER@; }
QToolButton#dockTitleButton:pressed { background: @PRESSED@; }
QTabWidget::pane { border: 0; border-top: 1px solid @TAB_BORDER@; }
QTabBar::tab {
  background: @BAR@;
  color: @TAB_TEXT@;
  padding: @TAB_V@px @TAB_H@px;
  border: 0;
  border-right: 1px solid @BORDER@;
}
QTabBar::tab:selected { color: @ACTIVE_TAB_TEXT@; background: @BUTTON@; border-top: 2px solid @FOCUS@; }
QTabBar::tab:hover:!selected { background: @TAB_HOVER@; }
QTabBar::tab:disabled { color: @DISABLED_TEXT@; }
QPlainTextEdit {
  background: @LOG@;
  color: @LOG_TEXT@;
  border: 0;
  font-family: "Cascadia Mono", "Consolas";
}
QScrollBar:vertical { background: @ITEM_VIEW@; width: @SCROLL@px; margin: 0; }
QScrollBar::handle:vertical { background: @SCROLL_HANDLE@; min-height: 24px; border-radius: 4px; }
QScrollBar::handle:vertical:hover { background: @ACCENT@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: @ITEM_VIEW@; height: @SCROLL@px; margin: 0; }
QScrollBar::handle:horizontal { background: @SCROLL_HANDLE@; min-width: 24px; border-radius: 4px; }
QScrollBar::handle:horizontal:hover { background: @ACCENT@; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QStatusBar { background: @BAR@; border-top: 1px solid @BORDER@; color: @TAB_TEXT@; }
QStatusBar::item { border: 0; }
QLabel#sectionTitle {
  color: @SECTION_TEXT@;
  font-weight: 600;
  padding-top: @SECTION_TOP@px;
  padding-bottom: @SECTION_BOTTOM@px;
  border-bottom: 1px solid @SECTION_BORDER@;
}
QLabel#mutedLabel, QLabel#uiScaleStatus { color: @MUTED_TEXT@; }
QLabel#statusGood { color: @GOOD_TEXT@; }
QLabel#statusWarn, QLabel#densityGuardWarning { color: @WARN_TEXT@; }
QFrame#inspectorPanel { background: @SURFACE@; }
QSplitter::handle { background: @BORDER@; }
QProgressBar {
  background: @FIELD@;
  color: @TEXT@;
  border: 1px solid @FIELD_BORDER@;
  border-radius: 3px;
  text-align: center;
}
QProgressBar::chunk { background: @PROGRESS@; border-radius: 2px; }
QToolTip { background: @TOOLTIP@; color: @TOOLTIP_TEXT@; border: 1px solid @TOOLTIP_BORDER@; padding: 4px; }
)CSS");

  // Both modes share the widget/font template, retaining text density and the
  // selected language's font. Only light dock-resize borders add a compact,
  // scale-aware separator rule below; dark separators retain native styling.
  struct ColorToken {
    const char *token;
    const char *dark;
    const char *light;
  };
  const ColorToken colors[] = {
      {"@SURFACE@", "#1b1d1f", "#ffffff"},
      {"@TEXT@", "#e7e9ea", "#171717"},
      {"@SELECTION@", "#327e74", "#2467a5"},
      {"@PROGRESS@", "#327e74", "#aecbe8"},
      {"@WINDOW@", "#17191b", "#f3f3f3"},
      {"@BAR@", "#202326", "#e7e7e7"},
      {"@BORDER@", "#34383c", "#a0a0a0"},
      {"@HOVER@", "#303438", "#d8e5f2"},
      {"@MENU@", "#24272a", "#ffffff"},
      {"@HEADER@", "#24272a", "#dedede"},
      {"@FIELD_BORDER@", "#41464b", "#8a8a8a"},
      {"@MENU_SELECTION@", "#356e67", "#cddff3"},
      {"@SEPARATOR@", "#3b3f43", "#aaaaaa"},
      {"@HOVER_BORDER@", "#444a4f", "#6e8ead"},
      {"@PRESSED@", "#315d58", "#cddff3"},
      {"@ACCENT@", "#4a9a8f", "#2467a5"},
      {"@DISABLED_TEXT@", "#929a9f", "#6b6b6b"},
      {"@BUTTON@", "#2a2e31", "#f3f3f3"},
      {"@BUTTON_BORDER@", "#454a4f", "#8a8a8a"},
      {"@BUTTON_HOVER@", "#34383c", "#e0e8f1"},
      {"@BUTTON_HOVER_BORDER@", "#5a6268", "#135f9b"},
      {"@DISABLED_SURFACE@", "#242629", "#ededed"},
      {"@DISABLED_BORDER@", "#34373a", "#b0b0b0"},
      {"@FIELD@", "#151719", "#ffffff"},
      {"@FOCUS@", "#55b2a5", "#135f9b"},
      {"@ITEM_VIEW@", "#191b1d", "#ffffff"},
      {"@ALTERNATE@", "#1e2123", "#f5f5f5"},
      {"@ITEM_HOVER@", "#282c2f", "#e5edf7"},
      {"@HEADER_TEXT@", "#b9bec2", "#252525"},
      {"@HEADER_BORDER@", "#35393d", "#a0a0a0"},
      {"@TITLE_TEXT@", "#dfe2e4", "#171717"},
      {"@TITLE@", "#222528", "#dedede"},
      {"@TITLE_BORDER@", "#363a3e", "#a0a0a0"},
      {"@TAB_BORDER@", "#373b3f", "#a0a0a0"},
      {"@TAB_TEXT@", "#aeb4b8", "#444444"},
      {"@ACTIVE_TAB_TEXT@", "#ffffff", "#171717"},
      {"@TAB_HOVER@", "#272a2d", "#d8e5f2"},
      {"@LOG@", "#111315", "#ffffff"},
      {"@LOG_TEXT@", "#ced3d6", "#171717"},
      {"@SCROLL_HANDLE@", "#484d51", "#858585"},
      {"@SECTION_TEXT@", "#d9dddf", "#171717"},
      {"@SECTION_BORDER@", "#383c40", "#a0a0a0"},
      {"@MUTED_TEXT@", "#949ba0", "#444444"},
      {"@GOOD_TEXT@", "#66c1a8", "#176b37"},
      {"@WARN_TEXT@", "#d9ad5b", "#805409"},
      {"@TOOLTIP@", "#2c3033", "#fffff5"},
      {"@TOOLTIP_TEXT@", "#ffffff", "#171717"},
      {"@TOOLTIP_BORDER@", "#555b60", "#8a8a8a"},
  };
  for (const ColorToken &color : colors) {
    css.replace(QString::fromLatin1(color.token),
                QString::fromLatin1(theme == UiTheme::Light ? color.light
                                                          : color.dark));
  }

  const auto replace = [&css, scalePercent](const QString &token, const int baseValue) {
    css.replace(token, QString::number(AppTheme::scaled(baseValue, scalePercent)));
  };
  replace(QStringLiteral("@MENU_PAD@"), 2);
  replace(QStringLiteral("@MENU_ITEM_V@"), 5);
  replace(QStringLiteral("@MENU_ITEM_H@"), 9);
  replace(QStringLiteral("@MENU_ROW_V@"), 6);
  replace(QStringLiteral("@MENU_ROW_H@"), 26);
  replace(QStringLiteral("@TOOL_GAP@"), 2);
  replace(QStringLiteral("@TOOL_PAD@"), 3);
  replace(QStringLiteral("@BUTTON_PAD@"), 5);
  replace(QStringLiteral("@CONTROL_HEIGHT@"), 26);
  replace(QStringLiteral("@CONTROL_PAD@"), 10);
  replace(QStringLiteral("@INPUT_PAD@"), 7);
  replace(QStringLiteral("@COMBO_ARROW@"), 22);
  replace(QStringLiteral("@ROW_HEIGHT@"), 25);
  replace(QStringLiteral("@HEADER_PAD@"), 6);
  replace(QStringLiteral("@DOCK_FALLBACK_V@"), 1);
  replace(QStringLiteral("@DOCK_FALLBACK_H@"), 6);
  replace(QStringLiteral("@DOCK_FALLBACK_FONT@"), 15);
  replace(QStringLiteral("@TAB_V@"), 5);
  replace(QStringLiteral("@TAB_H@"), 12);
  replace(QStringLiteral("@SCROLL@"), 10);
  replace(QStringLiteral("@SECTION_TOP@"), 6);
  replace(QStringLiteral("@SECTION_BOTTOM@"), 4);
  if (theme == UiTheme::Light) {
    // Paint Qt's real dock resize handles, not an overlay over the viewport.
    // Match the panel-header gray and distinguish regions through modest width,
    // not dark outlines. Include both one-pixel edges in the scaled extent;
    // native mouse handling, floating panels and dark separators stay unchanged.
    const int separatorContent = std::max(2, AppTheme::scaled(8, scalePercent) - 2);
    css += QStringLiteral(R"CSS(
QMainWindow::separator {
  background: #dedede;
  border: 1px solid #c8c8c8;
  width: %1px;
  height: %1px;
}
QMainWindow::separator:hover { background: #d0d0d0; border-color: #b5b5b5; }
)CSS").arg(separatorContent);
  }
  return css;
}

} // namespace gsw
