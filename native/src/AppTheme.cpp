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
  const QColor window(light ? "#f3f5f3" : "#1b1d1f");
  const QColor base(light ? "#ffffff" : "#151719");
  const QColor text(light ? "#24302c" : "#e7e9ea");
  const QColor muted(light ? "#596b60" : "#a2aaaf");
  const QColor disabled(light ? "#67756e" : "#929a9f");
  const QColor disabledBase(light ? "#edf1ee" : "#242629");
  const QColor button(light ? "#f7f9f7" : "#2a2e31");
  const QColor accent(light ? "#176d60" : "#327e74");
  for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive,
                                         QPalette::Disabled}) {
    const bool isDisabled = group == QPalette::Disabled;
    const QColor foreground = isDisabled ? disabled : text;
    result.setColor(group, QPalette::Window, window);
    result.setColor(group, QPalette::WindowText, foreground);
    result.setColor(group, QPalette::Base, isDisabled ? disabledBase : base);
    result.setColor(group, QPalette::AlternateBase,
                    QColor(light ? "#f0f4f1" : "#1e2123"));
    result.setColor(group, QPalette::Text, foreground);
    result.setColor(group, QPalette::Button, isDisabled ? disabledBase : button);
    result.setColor(group, QPalette::ButtonText, foreground);
    result.setColor(group, QPalette::BrightText, QColor(Qt::white));
    result.setColor(group, QPalette::Highlight,
                    isDisabled ? QColor(light ? "#63796d" : "#445b54") : accent);
    result.setColor(group, QPalette::HighlightedText, QColor(Qt::white));
    result.setColor(group, QPalette::ToolTipBase,
                    QColor(light ? "#fffef8" : "#2c3033"));
    result.setColor(group, QPalette::ToolTipText, foreground);
    result.setColor(group, QPalette::PlaceholderText, isDisabled ? disabled : muted);
    result.setColor(group, QPalette::Link,
                    QColor(light ? "#126d54" : "#70cdb4"));
    result.setColor(group, QPalette::LinkVisited,
                    QColor(light ? "#64518a" : "#c7a9e9"));
    result.setColor(group, QPalette::Light,
                    QColor(light ? "#ffffff" : "#5a6268"));
    result.setColor(group, QPalette::Midlight,
                    QColor(light ? "#e1ebe4" : "#41464b"));
    result.setColor(group, QPalette::Mid,
                    QColor(light ? "#b8c7bd" : "#34383c"));
    result.setColor(group, QPalette::Dark,
                    QColor(light ? "#84998c" : "#111315"));
    result.setColor(group, QPalette::Shadow,
                    QColor(light ? "#647a6d" : "#090b0d"));
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
  background: @MENU@;
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

  // Both modes share one geometry/font template, so switching appearance cannot
  // introduce a different UI density or undo the selected language's font.
  struct ColorToken {
    const char *token;
    const char *dark;
    const char *light;
  };
  const ColorToken colors[] = {
      {"@SURFACE@", "#1b1d1f", "#f3f5f3"},
      {"@TEXT@", "#e7e9ea", "#24302c"},
      {"@SELECTION@", "#327e74", "#176d60"},
      {"@PROGRESS@", "#327e74", "#a5d4c1"},
      {"@WINDOW@", "#17191b", "#edf1ee"},
      {"@BAR@", "#202326", "#e6ece8"},
      {"@BORDER@", "#34383c", "#c5cfc9"},
      {"@HOVER@", "#303438", "#dbe6e0"},
      {"@MENU@", "#24272a", "#f7f9f7"},
      {"@FIELD_BORDER@", "#41464b", "#adbcb3"},
      {"@MENU_SELECTION@", "#356e67", "#d0e7de"},
      {"@SEPARATOR@", "#3b3f43", "#bdcac1"},
      {"@HOVER_BORDER@", "#444a4f", "#91a69a"},
      {"@PRESSED@", "#315d58", "#c7e3d7"},
      {"@ACCENT@", "#4a9a8f", "#287d64"},
      {"@DISABLED_TEXT@", "#929a9f", "#67756e"},
      {"@BUTTON@", "#2a2e31", "#f7f9f7"},
      {"@BUTTON_BORDER@", "#454a4f", "#acbbb2"},
      {"@BUTTON_HOVER@", "#34383c", "#e1ebe4"},
      {"@BUTTON_HOVER_BORDER@", "#5a6268", "#84998c"},
      {"@DISABLED_SURFACE@", "#242629", "#edf1ee"},
      {"@DISABLED_BORDER@", "#34373a", "#cdd7d0"},
      {"@FIELD@", "#151719", "#ffffff"},
      {"@FOCUS@", "#55b2a5", "#137854"},
      {"@ITEM_VIEW@", "#191b1d", "#fafcf9"},
      {"@ALTERNATE@", "#1e2123", "#f0f4f1"},
      {"@ITEM_HOVER@", "#282c2f", "#e4eee7"},
      {"@HEADER_TEXT@", "#b9bec2", "#43594b"},
      {"@HEADER_BORDER@", "#35393d", "#c8d3cb"},
      {"@TITLE_TEXT@", "#dfe2e4", "#304b3b"},
      {"@TITLE@", "#222528", "#e0e9e2"},
      {"@TITLE_BORDER@", "#363a3e", "#bacbbf"},
      {"@TAB_BORDER@", "#373b3f", "#c5d2c8"},
      {"@TAB_TEXT@", "#aeb4b8", "#52685a"},
      {"@ACTIVE_TAB_TEXT@", "#ffffff", "#24302c"},
      {"@TAB_HOVER@", "#272a2d", "#dce7df"},
      {"@LOG@", "#111315", "#ffffff"},
      {"@LOG_TEXT@", "#ced3d6", "#30433a"},
      {"@SCROLL_HANDLE@", "#484d51", "#91a699"},
      {"@SECTION_TEXT@", "#d9dddf", "#304b3b"},
      {"@SECTION_BORDER@", "#383c40", "#c2d1c6"},
      {"@MUTED_TEXT@", "#949ba0", "#596b60"},
      {"@GOOD_TEXT@", "#66c1a8", "#187456"},
      {"@WARN_TEXT@", "#d9ad5b", "#8a580d"},
      {"@TOOLTIP@", "#2c3033", "#fffef8"},
      {"@TOOLTIP_TEXT@", "#ffffff", "#24302c"},
      {"@TOOLTIP_BORDER@", "#555b60", "#adbdb1"},
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
  return css;
}

} // namespace gsw
