#pragma once

#include <QSize>
#include <QString>

class QApplication;
class QScreen;

namespace gsw {

enum class UiScaleMode {
  Automatic,
  Manual,
};

class AppTheme final {
public:
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
  static void saveScaleMode(UiScaleMode mode);
  static void apply(QApplication &application, int scalePercent, bool persist);
  static int scaled(int value, int scalePercent);

private:
  static QString styleSheet(int scalePercent);
};

} // namespace gsw
