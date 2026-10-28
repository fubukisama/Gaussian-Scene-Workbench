#pragma once

#include <QColor>
#include <QMargins>
#include <QSize>
#include <QString>
#include <QPalette>
#include <QTime>

class QApplication;
class QScreen;

namespace gsw {

enum class UiScaleMode {
  Automatic,
  Manual,
};

enum class UiTheme {
  Dark,
  Light,
};

// The saved policy is separate from the palette currently being displayed.
enum class UiThemeMode {
  Automatic,
  Dark,
  Light,
};

// Shared chrome roles. Viewport backgrounds and XYZ/domain colors remain
// owned by their existing renderers, not this UI palette.
enum class UiColorRole {
  Panel,
  Window,
  Chrome,
  Field,
  Menu,
  ItemView,
  AlternateItem,
  Header,
  DockTitle,
  Button,
  ButtonHover,
  DisabledSurface,
  Text,
  MutedText,
  DisabledText,
  HeaderText,
  DockTitleText,
  TabText,
  ActiveTabText,
  LogSurface,
  LogText,
  LineSubtle,
  FieldLine,
  ButtonLine,
  HeaderLine,
  DockTitleLine,
  TabLine,
  SeparatorLine,
  DisabledLine,
  Hover,
  HoverLine,
  ItemHover,
  Action,
  ActionTint,
  ActionPressed,
  ActionInk,
  OnAction,
  Focus,
  DisabledSelection,
  Progress,
  LinkVisited,
  Success,
  Warning,
  Danger,
  ToolTipSurface,
  ToolTipText,
  ToolTipLine,
  ScrollHandle,
  PaletteLight,
  PaletteMidlight,
  PaletteMid,
  PaletteDark,
  PaletteShadow,
  SectionText,
  SectionLine,
  Count,
};

class AppTheme final {
public:
  static UiThemeMode loadThemeMode();
  static UiThemeMode currentThemeMode();
  static UiTheme themeForTime(const QTime &localTime);
  static UiTheme loadTheme(const QTime &localTime = QTime::currentTime());
  static UiTheme currentTheme();
  static QColor color(UiTheme theme, UiColorRole role);
  static QPalette palette(UiTheme theme);
  static void applyTheme(QApplication &application, UiTheme theme, bool persist);
  static void applyThemeMode(QApplication &application, UiThemeMode mode,
                             bool persist,
                             const QTime &localTime = QTime::currentTime());
  static bool refreshAutomaticTheme(QApplication &application,
                                    const QTime &localTime = QTime::currentTime());
  static int automaticThemeCheckInterval(const QTime &localTime);
  static UiScaleMode loadScaleMode();
  static int loadScalePercent(const QScreen *screen);
  static int recommendedScalePercent(const QScreen *screen);
  static int recommendedScalePercent(const QSize &availableSize,
                                     const QSize &windowSize);
  static int recommendedScalePercent(const QSize &availableSize,
                                     const QSize &windowSize,
                                     double devicePixelRatio);
  static QSize fitWindowResolution(const QSize &requestedSize,
                                   const QSize &availableSize,
                                   const QSize &minimumSize);
  // Preserve the design minimum where it fits. Otherwise bound the client
  // minimum by screen availability minus positive frame edges and a 16 logical
  // pixel safety gap. Empty/invalid screen observations keep the positive design
  // minimum; neither this policy nor the returned size changes UI font density.
  static QSize effectiveWindowMinimum(const QSize &designMinimum,
                                      const QSize &availableSize,
                                      const QMargins &frameMargins);
  static int rescaledDockExtent(int currentExtent, int fromScalePercent,
                                int toScalePercent, int minimumExtent);
  static int dockTitleFontPixelSize(int scalePercent);
  static int dockTitleHeight(int scalePercent);
  static int dockTitleButtonSize(int scalePercent);
  static void saveScaleMode(UiScaleMode mode);
  static void apply(QApplication &application, int scalePercent, bool persist);
  static int scaled(int value, int scalePercent);

private:
  static QString styleSheet(int scalePercent, UiTheme theme);
};

} // namespace gsw
