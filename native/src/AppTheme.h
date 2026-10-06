#pragma once

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

class AppTheme final {
public:
  static UiThemeMode loadThemeMode();
  static UiThemeMode currentThemeMode();
  static UiTheme themeForTime(const QTime &localTime);
  static UiTheme loadTheme(const QTime &localTime = QTime::currentTime());
  static UiTheme currentTheme();
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
