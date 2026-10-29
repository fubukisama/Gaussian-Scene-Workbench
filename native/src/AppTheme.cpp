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
#include <iterator>

namespace gsw {

namespace {
constexpr int kMinimumScale = 90;
constexpr int kMaximumScale = 150;
constexpr int kDayStartHour = 6;
constexpr int kNightStartHour = 18;

struct ColorDefinition {
  UiColorRole role;
  const char *dark;
  const char *light;
};

// One source for QPalette, QSS and custom-painted chrome. Keep neutral field,
// panel and typography baselines; application actions share the blue family.
constexpr ColorDefinition kColors[] = {
    {UiColorRole::Panel, "#1b1d1f", "#ffffff"},
    {UiColorRole::Window, "#17191b", "#f3f3f3"},
    {UiColorRole::Chrome, "#202326", "#e7e7e7"},
    {UiColorRole::Field, "#151719", "#ffffff"},
    {UiColorRole::Menu, "#24272a", "#ffffff"},
    {UiColorRole::ItemView, "#191b1d", "#ffffff"},
    {UiColorRole::AlternateItem, "#1e2123", "#f5f5f5"},
    {UiColorRole::Header, "#24272a", "#dedede"},
    {UiColorRole::DockTitle, "#222528", "#dedede"},
    {UiColorRole::Button, "#2a2e31", "#f3f3f3"},
    {UiColorRole::ButtonHover, "#34383c", "#e8eaed"},
    {UiColorRole::DisabledSurface, "#242629", "#ededed"},
    {UiColorRole::Text, "#e7e9ea", "#171717"},
    {UiColorRole::MutedText, "#a2aaaf", "#444444"},
    {UiColorRole::DisabledText, "#929a9f", "#6b6b6b"},
    {UiColorRole::HeaderText, "#b9bec2", "#252525"},
    {UiColorRole::DockTitleText, "#dfe2e4", "#171717"},
    {UiColorRole::TabText, "#aeb4b8", "#444444"},
    {UiColorRole::ActiveTabText, "#ffffff", "#171717"},
    {UiColorRole::LogSurface, "#111315", "#ffffff"},
    {UiColorRole::LogText, "#ced3d6", "#171717"},
    {UiColorRole::LineSubtle, "#34383c", "#d5d9de"},
    {UiColorRole::FieldLine, "#41464b", "#8a8a8a"},
    {UiColorRole::ButtonLine, "#454a4f", "#8a8a8a"},
    {UiColorRole::HeaderLine, "#35393d", "#a0a0a0"},
    {UiColorRole::DockTitleLine, "#363a3e", "#a0a0a0"},
    {UiColorRole::TabLine, "#373b3f", "#a0a0a0"},
    {UiColorRole::SeparatorLine, "#3b3f43", "#aaaaaa"},
    {UiColorRole::DisabledLine, "#34373a", "#b0b0b0"},
    {UiColorRole::Hover, "#303438", "#e1e3e5"},
    {UiColorRole::HoverLine, "#444a4f", "#9ba1a7"},
    {UiColorRole::ItemHover, "#282c2f", "#edf0f3"},
    {UiColorRole::Action, "#2d6bb2", "#2467a5"},
    {UiColorRole::ActionTint, "#243c56", "#e4effb"},
    {UiColorRole::ActionPressed, "#2b4866", "#d5e5f7"},
    {UiColorRole::ActionInk, "#8cbcf0", "#135f9b"},
    {UiColorRole::OnAction, "#ffffff", "#ffffff"},
    {UiColorRole::Focus, "#78aef1", "#135f9b"},
    {UiColorRole::DisabledSelection, "#40566c", "#666666"},
    {UiColorRole::Progress, "#2d6bb2", "#aecbe8"},
    {UiColorRole::LinkVisited, "#c7a9e9", "#64518a"},
    {UiColorRole::Success, "#66c1a8", "#176b37"},
    {UiColorRole::Warning, "#d9ad5b", "#805409"},
    {UiColorRole::Danger, "#ef8585", "#ac2828"},
    {UiColorRole::ToolTipSurface, "#2c3033", "#fffff5"},
    {UiColorRole::ToolTipText, "#ffffff", "#171717"},
    {UiColorRole::ToolTipLine, "#555b60", "#8a8a8a"},
    {UiColorRole::ScrollHandle, "#484d51", "#858585"},
    {UiColorRole::PaletteLight, "#5a6268", "#ffffff"},
    {UiColorRole::PaletteMidlight, "#41464b", "#e0e0e0"},
    {UiColorRole::PaletteMid, "#34383c", "#a0a0a0"},
    {UiColorRole::PaletteDark, "#111315", "#8a8a8a"},
    {UiColorRole::PaletteShadow, "#090b0d", "#656565"},
    {UiColorRole::SectionText, "#d9dddf", "#171717"},
    {UiColorRole::SectionLine, "#383c40", "#a0a0a0"},
};
static_assert(std::size(kColors) == static_cast<std::size_t>(UiColorRole::Count));

int clampScale(const int value) {
  return std::clamp(value, kMinimumScale, kMaximumScale);
}

QString themeName(const UiTheme theme) {
  return theme == UiTheme::Light ? QStringLiteral("light")
                               : QStringLiteral("dark");
}

QString themeModeName(const UiThemeMode mode) {
  if (mode == UiThemeMode::Automatic) return QStringLiteral("auto");
  return mode == UiThemeMode::Light ? QStringLiteral("light")
                                   : QStringLiteral("dark");
}

UiThemeMode themeModeFromName(const QString &name) {
  if (name == QStringLiteral("light")) return UiThemeMode::Light;
  if (name == QStringLiteral("dark")) return UiThemeMode::Dark;
  return UiThemeMode::Automatic;
}

UiTheme resolveTheme(const UiThemeMode mode, const QTime &localTime) {
  if (mode == UiThemeMode::Automatic) return AppTheme::themeForTime(localTime);
  return mode == UiThemeMode::Light ? UiTheme::Light : UiTheme::Dark;
}
} // namespace

UiThemeMode AppTheme::loadThemeMode() {
  return themeModeFromName(
      QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("auto")).toString());
}

UiThemeMode AppTheme::currentThemeMode() {
  if (QCoreApplication::instance() != nullptr) {
    const QVariant mode = QCoreApplication::instance()->property("gswUiThemeMode");
    if (mode.isValid()) return themeModeFromName(mode.toString());
  }
  return loadThemeMode();
}

UiTheme AppTheme::themeForTime(const QTime &localTime) {
  return localTime.isValid() && localTime >= QTime(kDayStartHour, 0) &&
         localTime < QTime(kNightStartHour, 0)
      ? UiTheme::Light : UiTheme::Dark;
}

UiTheme AppTheme::loadTheme(const QTime &localTime) {
  return resolveTheme(loadThemeMode(), localTime);
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

QColor AppTheme::color(const UiTheme theme, const UiColorRole role) {
  const auto index = static_cast<std::size_t>(role);
  if (index >= std::size(kColors)) return {};
  const ColorDefinition &definition = kColors[index];
  Q_ASSERT(definition.role == role);
  return QColor(theme == UiTheme::Light ? definition.light : definition.dark);
}

QPalette AppTheme::palette(const UiTheme theme) {
  const auto role = [theme](const UiColorRole value) { return color(theme, value); };
  QPalette result;
  const QColor window = role(UiColorRole::Panel);
  const QColor base = role(UiColorRole::Field);
  const QColor text = role(UiColorRole::Text);
  const QColor muted = role(UiColorRole::MutedText);
  const QColor disabled = role(UiColorRole::DisabledText);
  const QColor disabledBase = role(UiColorRole::DisabledSurface);
  const QColor button = role(UiColorRole::Button);
  const QColor accent = role(UiColorRole::Action);
  for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive,
                                         QPalette::Disabled}) {
    const bool isDisabled = group == QPalette::Disabled;
    const QColor foreground = isDisabled ? disabled : text;
    result.setColor(group, QPalette::Window, window);
    result.setColor(group, QPalette::WindowText, foreground);
    result.setColor(group, QPalette::Base, isDisabled ? disabledBase : base);
    result.setColor(group, QPalette::AlternateBase, role(UiColorRole::AlternateItem));
    result.setColor(group, QPalette::Text, foreground);
    result.setColor(group, QPalette::Button, isDisabled ? disabledBase : button);
    result.setColor(group, QPalette::ButtonText, foreground);
    result.setColor(group, QPalette::BrightText, role(UiColorRole::OnAction));
    result.setColor(group, QPalette::Highlight,
                    isDisabled ? role(UiColorRole::DisabledSelection) : accent);
    result.setColor(group, QPalette::HighlightedText, role(UiColorRole::OnAction));
    result.setColor(group, QPalette::ToolTipBase, role(UiColorRole::ToolTipSurface));
    result.setColor(group, QPalette::ToolTipText,
                    isDisabled ? disabled : role(UiColorRole::ToolTipText));
    result.setColor(group, QPalette::PlaceholderText, isDisabled ? disabled : muted);
    result.setColor(group, QPalette::Link, role(UiColorRole::ActionInk));
    result.setColor(group, QPalette::LinkVisited, role(UiColorRole::LinkVisited));
    result.setColor(group, QPalette::Light, role(UiColorRole::PaletteLight));
    result.setColor(group, QPalette::Midlight, role(UiColorRole::PaletteMidlight));
    result.setColor(group, QPalette::Mid, role(UiColorRole::PaletteMid));
    result.setColor(group, QPalette::Dark, role(UiColorRole::PaletteDark));
    result.setColor(group, QPalette::Shadow, role(UiColorRole::PaletteShadow));
    result.setColor(group, QPalette::Accent, accent);
  }
  return result;
}

void AppTheme::applyTheme(QApplication &application, const UiTheme theme,
                          const bool persist) {
  applyThemeMode(application, theme == UiTheme::Light ? UiThemeMode::Light
                                                    : UiThemeMode::Dark, persist);
}

void AppTheme::applyThemeMode(QApplication &application, const UiThemeMode mode,
                              const bool persist, const QTime &localTime) {
  application.setProperty("gswUiThemeMode", themeModeName(mode));
  application.setProperty("gswUiTheme", themeName(resolveTheme(mode, localTime)));
  if (persist) {
    QSettings().setValue(QStringLiteral("ui/theme"), themeModeName(mode));
  }
  const QVariant scale = application.property("gswUiScalePercent");
  apply(application, scale.isValid() ? scale.toInt() : 100, false);
}

bool AppTheme::refreshAutomaticTheme(QApplication &application,
                                     const QTime &localTime) {
  if (currentThemeMode() != UiThemeMode::Automatic || !localTime.isValid()) return false;
  const UiTheme theme = themeForTime(localTime);
  if (currentTheme() == theme) return false;
  // Do not save the resolved palette over the user's automatic policy.
  application.setProperty("gswUiTheme", themeName(theme));
  const QVariant scale = application.property("gswUiScalePercent");
  apply(application, scale.isValid() ? scale.toInt() : 100, false);
  return true;
}

int AppTheme::automaticThemeCheckInterval(const QTime &localTime) {
  if (!localTime.isValid()) return 30000;
  const QTime dayStart(kDayStartHour, 0);
  const QTime nightStart(kNightStartHour, 0);
  const int untilBoundary = localTime < dayStart
      ? localTime.msecsTo(dayStart)
      : localTime < nightStart
          ? localTime.msecsTo(nightStart)
          : localTime.msecsTo(dayStart) + 24 * 60 * 60 * 1000;
  // Reach the next boundary, but also re-read the wall clock after sleep,
  // clock/time-zone edits or a backwards jump. Never use a 24-hour fixed timer.
  return std::clamp(untilBoundary, 1, 30000);
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

QSize AppTheme::effectiveWindowMinimum(const QSize &designMinimum,
                                      const QSize &availableSize,
                                      const QMargins &frameMargins) {
  const QSize positiveDesign(std::max(1, designMinimum.width()),
                             std::max(1, designMinimum.height()));
  if (availableSize.width() <= 0 || availableSize.height() <= 0)
    return positiveDesign;

  const auto clientMinimum = [](const int design, const int available,
                                 const int firstFrame, const int secondFrame) {
    constexpr qint64 safetyGap = 16;
    const qint64 positiveFrames = std::max(qint64(0), qint64(firstFrame)) +
                                  std::max(qint64(0), qint64(secondFrame));
    const qint64 budget = qint64(available) - positiveFrames - safetyGap;
    return int(std::clamp(budget, qint64(1), qint64(design)));
  };
  return QSize(clientMinimum(positiveDesign.width(), availableSize.width(),
                              frameMargins.left(), frameMargins.right()),
               clientMinimum(positiveDesign.height(), availableSize.height(),
                              frameMargins.top(), frameMargins.bottom()));
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
  application.setProperty("gswUiThemeMode", themeModeName(currentThemeMode()));
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
  selection-color: @ON_ACTION@;
}
QWidget:disabled { color: @DISABLED_TEXT@; }
QMainWindow, QDialog { background: @WINDOW@; }
/* Text and layout-only widgets reveal their containing surface in both themes. */
QLabel, QCheckBox, QRadioButton, QDialogButtonBox, QSizeGrip,
QWidget[gswLayoutContainer="true"] { background: transparent; }
/* An outline does not change a control's content or hit rectangle. */
QCheckBox:focus, QRadioButton:focus {
  outline: 1px solid @FOCUS@;
  outline-offset: -1px;
}
QWidget#dialogBody, QScrollArea#dialogBodyScroll,
QScrollArea#dialogBodyScroll > QWidget { background: @WINDOW@; }
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
QToolButton:checked { background: @CHECKED@; border-color: @ACCENT@; }
QToolButton:pressed { background: @PRESSED@; border-color: @ACCENT@; }
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
QPushButton:checked { background: @CHECKED@; border-color: @ACCENT@; }
QPushButton:pressed { background: @PRESSED@; border-color: @ACCENT@; }
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
/* Own ordinary item chrome too, not only hover/selection. Otherwise the
   Windows 11 style draws implicit column rules in normal tree/list cells. */
QTreeView::item, QTableView::item {
  min-height: @ROW_HEIGHT@px;
  background: transparent;
  border: 0;
}
QListView::item { background: transparent; border: 0; }
QTreeView::item:hover, QTableView::item:hover { background: @ITEM_HOVER@; }
QTreeView::item:selected, QTableView::item:selected { background: @CHECKED@; color: @TEXT@; }
QListView::item:hover { background: @ITEM_HOVER@; }
QListView::item:selected { background: @CHECKED@; color: @TEXT@; }
/* Keep border:0 for ordinary cells and blank columns. Focus is an independent
   inset outline rather than an extra border that moves text or narrows rows. */
QAbstractItemView:focus,
QTreeView::item:focus, QTableView::item:focus, QListView::item:focus {
  outline: 1px solid @FOCUS@;
  outline-offset: -1px;
}
QHeaderView, QHeaderView QWidget#qt_scrollarea_viewport { background: @HEADER@; }
QHeaderView::section, QTableCornerButton::section {
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
QToolButton#dockTitleButton:focus {
  outline: 1px solid @FOCUS@;
  outline-offset: -1px;
}
QTabWidget::pane { border: 0; border-top: 1px solid @TAB_BORDER@; }
QTabBar::tab {
  background: @BAR@;
  color: @TAB_TEXT@;
  padding: @TAB_V@px @TAB_H@px;
  border: 0;
  border-top: 2px solid transparent;
}
QTabBar::tab:selected { color: @ACTIVE_TAB_TEXT@; background: @BUTTON@; border-top: 2px solid @FOCUS@; }
QTabBar::tab:hover:!selected { background: @TAB_HOVER@; }
QTabBar::tab:focus { outline: 1px solid @FOCUS@; outline-offset: -1px; }
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
QFrame#inspectorPanel QLabel#sectionTitle { border-bottom: 0; }
QLabel#mutedLabel, QLabel#uiScaleStatus { color: @MUTED_TEXT@; }
QLabel#statusGood { color: @GOOD_TEXT@; }
  QLabel#statusWarn, QLabel#densityGuardWarning, QLabel#trainingGrowthWarning { color: @WARN_TEXT@; }
QFrame#inspectorPanel { background: @SURFACE@; }
QFrame#workspaceDropCard { background: @SURFACE@; border: 2px dashed @FOCUS@; border-radius: 8px; }
QFrame#workspaceDropCard QLabel { background: transparent; border: 0; padding: 0; }
QLabel#workspaceDropTitle { font-weight: 600; color: @TEXT@; }
QLabel#workspaceDropDetail { color: @MUTED_TEXT@; }
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

  // Both modes share the widget/font and continuous-surface rules, retaining
  // text density and the selected language's font. The light-only rules below
  // give resize handles the same neutral gray as the light panel headers.
  struct ColorToken {
    const char *token;
    UiColorRole role;
  };
  const ColorToken colors[] = {
      {"@SURFACE@", UiColorRole::Panel},
      {"@TEXT@", UiColorRole::Text},
      {"@SELECTION@", UiColorRole::Action},
      {"@ON_ACTION@", UiColorRole::OnAction},
      {"@PROGRESS@", UiColorRole::Progress},
      {"@WINDOW@", UiColorRole::Window},
      {"@BAR@", UiColorRole::Chrome},
      {"@BORDER@", UiColorRole::LineSubtle},
      {"@HOVER@", UiColorRole::Hover},
      {"@MENU@", UiColorRole::Menu},
      {"@HEADER@", UiColorRole::Header},
      {"@FIELD_BORDER@", UiColorRole::FieldLine},
      {"@MENU_SELECTION@", UiColorRole::ActionTint},
      {"@SEPARATOR@", UiColorRole::SeparatorLine},
      {"@HOVER_BORDER@", UiColorRole::HoverLine},
      {"@PRESSED@", UiColorRole::ActionPressed},
      {"@CHECKED@", UiColorRole::ActionTint},
      {"@ACCENT@", UiColorRole::Action},
      {"@DISABLED_TEXT@", UiColorRole::DisabledText},
      {"@BUTTON@", UiColorRole::Button},
      {"@BUTTON_BORDER@", UiColorRole::ButtonLine},
      {"@BUTTON_HOVER@", UiColorRole::ButtonHover},
      {"@BUTTON_HOVER_BORDER@", UiColorRole::HoverLine},
      {"@DISABLED_SURFACE@", UiColorRole::DisabledSurface},
      {"@DISABLED_BORDER@", UiColorRole::DisabledLine},
      {"@FIELD@", UiColorRole::Field},
      {"@FOCUS@", UiColorRole::Focus},
      {"@ITEM_VIEW@", UiColorRole::ItemView},
      {"@ALTERNATE@", UiColorRole::AlternateItem},
      {"@ITEM_HOVER@", UiColorRole::ItemHover},
      {"@HEADER_TEXT@", UiColorRole::HeaderText},
      {"@HEADER_BORDER@", UiColorRole::HeaderLine},
      {"@TITLE_TEXT@", UiColorRole::DockTitleText},
      {"@TITLE@", UiColorRole::DockTitle},
      {"@TITLE_BORDER@", UiColorRole::DockTitleLine},
      {"@TAB_BORDER@", UiColorRole::TabLine},
      {"@TAB_TEXT@", UiColorRole::TabText},
      {"@ACTIVE_TAB_TEXT@", UiColorRole::ActiveTabText},
      {"@TAB_HOVER@", UiColorRole::Hover},
      {"@LOG@", UiColorRole::LogSurface},
      {"@LOG_TEXT@", UiColorRole::LogText},
      {"@SCROLL_HANDLE@", UiColorRole::ScrollHandle},
      {"@SECTION_TEXT@", UiColorRole::SectionText},
      {"@SECTION_BORDER@", UiColorRole::SectionLine},
      {"@MUTED_TEXT@", UiColorRole::MutedText},
      {"@GOOD_TEXT@", UiColorRole::Success},
      {"@WARN_TEXT@", UiColorRole::Warning},
      {"@TOOLTIP@", UiColorRole::ToolTipSurface},
      {"@TOOLTIP_TEXT@", UiColorRole::ToolTipText},
      {"@TOOLTIP_BORDER@", UiColorRole::ToolTipLine},
  };
  for (const ColorToken &token : colors) {
    css.replace(QString::fromLatin1(token.token), color(theme, token.role).name());
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
    // Paint Qt's real dock and splitter resize handles, not an overlay.
    // Match the panel-header gray and distinguish regions through modest width,
    // not dark outlines. Include both one-pixel edges in the scaled extent;
    // native mouse handling, floating panels and dark separators stay unchanged.
    const int separatorContent = std::max(2, AppTheme::scaled(8, scalePercent) - 2);
    // QSplitter applies its handle rule in both pixelMetric and sizeFromContents,
    // adding these borders twice. Keep the final extent at the scaled 8px target.
    const int splitterContent = std::max(1, AppTheme::scaled(8, scalePercent) - 4);
    css += QStringLiteral(R"CSS(
QMainWindow::separator {
  background: #dedede;
  border: 1px solid #c8c8c8;
  width: %1px;
  height: %1px;
}
QSplitter::handle {
  background: #dedede;
  border: 1px solid #c8c8c8;
  width: %2px;
  height: %2px;
}
QMainWindow::separator:hover, QSplitter::handle:hover {
  background: #d0d0d0;
  border-color: #b5b5b5;
}
)CSS").arg(separatorContent).arg(splitterContent);
  }
  return css;
}

} // namespace gsw
