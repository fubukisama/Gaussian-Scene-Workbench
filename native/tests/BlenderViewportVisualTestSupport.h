#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>

class QApplication;

namespace gsw {
class NativeViewport;

namespace viewportqa {

// Internal visual-test seams; no production renderer or window state is exposed.
struct AxisPixels final {
  int red = 0;
  int green = 0;
  int blue = 0;
};
struct BackgroundContrast final {
  double meanNeutralInk = 0.0;
  int visiblyChangedPixels = 0;
};

void processFrames(int milliseconds);
bool saveEvidence(const QImage &frame, const QString &name);
bool nativeWindowExposed(NativeViewport &viewport);
void clickViewport(NativeViewport &viewport, const QPointF &position);
void wheelViewport(NativeViewport &viewport, int angleDelta);
void dragViewport(NativeViewport &viewport, const QPointF &start, const QPointF &end);
AxisPixels groundAxisPixels(const QImage &frame);
double neutralGridInk(const QImage &frame);
QColor unobstructedBackdrop(const QImage &frame);
int cursorRedArcPixels(const QImage &frame, const QSize &logicalSize);
BackgroundContrast backgroundContrast(const QImage &frame, const QRectF &normalizedRect);
bool verifyModelRendering(NativeViewport &viewport, QApplication &application);

} // namespace viewportqa
} // namespace gsw
