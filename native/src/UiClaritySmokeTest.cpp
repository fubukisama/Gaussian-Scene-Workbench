#include "UiClaritySmokeTest.h"
#include "NativeViewport.h"
#include "AppLanguage.h"
#include "AppTheme.h"
#include "UiSurfaceEvidence.h"
#include "FontResolutionEvidence.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTextLayout>
#include <QTimer>
#include <QWindow>
#include <QDebug>
#include <iterator>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {
namespace {
void processFor(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}
}

bool runUiClaritySmokeTest(NativeViewport &viewport) {
  const QString evidence = qEnvironmentVariable("GSW_UI_CLARITY_EVIDENCE");
  const QString captureRoot = qEnvironmentVariable("GSW_RENDER_DIAGNOSTICS_DIR");
  if (!QDir::isAbsolutePath(evidence) || !QDir::isAbsolutePath(captureRoot) ||
      !QDir().mkpath(evidence) || !QDir().mkpath(captureRoot)) {
    qCritical() << "UI clarity: absolute owned evidence directories required";
    return false;
  }
  QDirIterator oldCaptures(captureRoot, {QStringLiteral("manifest.json")}, QDir::Files, QDirIterator::Subdirectories);
  if (oldCaptures.hasNext()) {
    qCritical() << "UI clarity: evidence directory is not fresh";
    return false;
  }
  QElapsedTimer deadline;
  deadline.start();
  while ((!viewport.isValid() || !viewport.window()->windowHandle() ||
          !viewport.window()->windowHandle()->isExposed()) && deadline.elapsed() < 10000)
    processFor(20);
  if (!viewport.isValid() || !viewport.context() || !viewport.context()->isValid() ||
      !viewport.window()->windowHandle() || !viewport.window()->windowHandle()->isExposed()) {
    qCritical() << "UI clarity: exposed real QOpenGLWidget required; no offscreen substitute";
    return false;
  }
  // This explicit smoke flag runs on the isolated remote desktop only. F12
  // correctly refuses desktop evidence from a different foreground window.
  viewport.window()->raise();
  viewport.window()->activateWindow();
  viewport.setFocus(Qt::OtherFocusReason);
  QElapsedTimer activation;
  activation.start();
  while (!viewport.window()->isActiveWindow() && activation.elapsed() < 5000) processFor(20);
  if (!viewport.window()->isActiveWindow()) {
    qCritical() << "UI clarity: owned remote window did not become active";
    return false;
  }
  const auto grid = viewport.viewportGridSettings();
  const auto target = viewport.viewTarget();
  const auto angles = viewport.viewOrbitAngles();
  const auto distance = viewport.viewDistance();
  const auto translation = viewport.modelTranslation();
  const auto rotation = viewport.modelRotation();
  const auto scale = viewport.modelScale();
  const bool oldLock = viewport.editToolsLocked();
  const auto restore = qScopeGuard([&] {
    viewport.setViewportGridSettings(grid);
    viewport.setEditToolsLocked(oldLock);
  });
  int observedSwaps = 0;
  const auto observedPresentation = QObject::connect(&viewport, &QOpenGLWidget::frameSwapped,
      &viewport, [&] { ++observedSwaps; });
  const auto disconnectPresentation = qScopeGuard([&] { QObject::disconnect(observedPresentation); });
  auto quietGrid = grid;
  quietGrid.gridVisible = false;
  quietGrid.axesVisible = false;
  if (!viewport.setViewportGridSettings(quietGrid)) return false;
  viewport.setEditToolsLocked(true);
  viewport.setMouseTracking(true);
  QMouseEvent move(QEvent::MouseMove, QPointF(12, viewport.height() - 18),
                   viewport.mapToGlobal(QPoint(12, viewport.height() - 18)),
                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&viewport, &move);
  // grabFramebuffer() can render a fresh but not yet presented frame. Wait
  // for the real swap signal instead, so F12's before/after serials agree.
  QElapsedTimer settle;
  settle.start();
  quint64 previousSerial = viewport.renderedFrameSerial();
  int stableObservations = 0;
  while ((observedSwaps == 0 || stableObservations < 4) && settle.elapsed() < 10000) {
    processFor(20);
    const quint64 current = viewport.renderedFrameSerial();
    stableObservations = current == previousSerial ? stableObservations + 1 : 0;
    previousSerial = current;
  }
  if (observedSwaps == 0 || stableObservations < 4) {
    qCritical() << "UI clarity: no stable actually presented frame before capture";
    return false;
  }
  const int swapsBeforeCapture = observedSwaps;
  QKeyEvent capture(QEvent::KeyPress, Qt::Key_F12, Qt::NoModifier);
  QApplication::sendEvent(&viewport, &capture);
  QString manifest;
  QJsonObject captured;
  QElapsedTimer captureWait;
  captureWait.start();
  while (captureWait.elapsed() < 10000 && manifest.isEmpty()) {
    processFor(25);
    QDirIterator files(captureRoot, {QStringLiteral("manifest.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
      const QString path = files.next();
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) continue;
      const auto object = QJsonDocument::fromJson(file.readAll()).object();
      if (object.value("status").toString() == QStringLiteral("complete")) {
        manifest = path;
        captured = object;
        break;
      }
    }
  }
  if (manifest.isEmpty() || !captured.value("requestSerialMatch").toBool() ||
      !captured.value("stageSerialMatch").toBool()) {
    qCritical() << "UI clarity: full presented diagnostic frame did not complete";
    return false;
  }
  QJsonObject environment{
      {"schema", "gsw.ui_clarity_capture/1"}, {"manifest", manifest},
      {"locale", AppLanguage::current()}, {"theme", AppTheme::currentTheme() == UiTheme::Light ? "light" : "dark"},
      {"logicalSize", QJsonArray{viewport.width(), viewport.height()}},
      {"measuredWidgetDpr", viewport.devicePixelRatioF()},
      {"requestedQtScaleFactor", qEnvironmentVariable("QT_SCALE_FACTOR")},
      {"requestedSamples", QSurfaceFormat::defaultFormat().samples()},
      {"actualContextSamples", viewport.context()->format().samples()},
      {"observedSwapsBeforeCapture", swapsBeforeCapture},
      {"actualUiScalePercent", qApp->property("gswUiScalePercent").toInt()},
      {"actualFont", fontResolutionEvidence(viewport.font(), &viewport)},
      {"productionCameraAndTransformUnchanged", target == viewport.viewTarget() &&
          angles == viewport.viewOrbitAngles() && distance == viewport.viewDistance() &&
          translation == viewport.modelTranslation() && rotation == viewport.modelRotation() && scale == viewport.modelScale()},
      {"controlledDprNotRealCrossMonitorEvidence", true}};
  if (QScreen *screen = viewport.screen()) {
    environment.insert("screenLogicalDpi", screen->logicalDotsPerInch());
    environment.insert("screenPhysicalDpi", screen->physicalDotsPerInch());
    environment.insert("screenDpr", screen->devicePixelRatio());
    const QPixmap fullWindow = screen->grabWindow(viewport.window()->winId());
    if (fullWindow.isNull() || !fullWindow.save(QDir(evidence).filePath(QStringLiteral("window-native.png")))) {
      qCritical() << "UI clarity: actual owned window screenshot is unavailable";
      return false;
    }
    environment.insert("nativeWindowCapture", QJsonObject{
        {"method", "QScreen::grabWindow(owned native window id)"},
        {"logicalWindowSize", QJsonArray{viewport.window()->width(), viewport.window()->height()}},
        {"physicalImageSize", QJsonArray{fullWindow.width(), fullWindow.height()}},
        {"pixmapDpr", fullWindow.devicePixelRatio()}});
  }
  environment.insert("visibleWidgetFontResolution", uiSurfaceFontEvidence(viewport.window()));
#ifdef Q_OS_WIN
  environment.insert("actualWindowsWindowDpi", int(GetDpiForWindow(reinterpret_cast<HWND>(viewport.window()->winId()))));
#endif
  QSaveFile output(QDir(evidence).filePath(QStringLiteral("capture.json")));
  const QByteArray bytes = QJsonDocument(environment).toJson(QJsonDocument::Indented);
  if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() ||
      !output.commit()) return false;
  qInfo().noquote() << "UI_CLARITY_CAPTURE:" << manifest << "DPR" << viewport.devicePixelRatioF();
  return environment.value("productionCameraAndTransformUnchanged").toBool();
}
} // namespace gsw
