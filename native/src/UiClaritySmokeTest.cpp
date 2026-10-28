#include "UiClaritySmokeTest.h"
#include "NativeViewport.h"
#include "AppLanguage.h"
#include "AppTheme.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontInfo>
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
  const QFontInfo actual(viewport.font());
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
      {"actualFont", QJsonObject{{"family", actual.family()}, {"pixelSize", actual.pixelSize()},
          {"pointSize", actual.pointSizeF()}, {"exactMatch", actual.exactMatch()}}},
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
  QJsonArray fonts;
  // Immutable glyph-test data, not a translatable user-interface message.
  const char32_t glyphProbe[]{0x4c, 0x61, 0x74, 0x69, 0x6e, 0x20, 0x4e2d, 0x6587,
      0x20, 0x65e5, 0x672c, 0x8a9e, 0x20, 0x30, 0x31, 0x32, 0x33};
  const QString mixedProbe = QString::fromUcs4(glyphProbe, qsizetype(std::size(glyphProbe)));
  const auto widgets = viewport.window()->findChildren<QWidget *>();
  for (QWidget *widget : widgets) {
    if (!widget->isVisible()) continue;
    const QFontInfo info(widget->font());
    QTextLayout mixed(mixedProbe, widget->font());
    mixed.beginLayout();
    QTextLine line = mixed.createLine();
    if (line.isValid()) line.setLineWidth(2000);
    mixed.endLayout();
    QJsonArray runs;
    for (const QGlyphRun &run : mixed.glyphRuns()) {
      const auto glyphs = run.glyphIndexes();
      runs.append(QJsonObject{{"family", run.rawFont().familyName()}, {"style", run.rawFont().styleName()},
          {"glyphCount", glyphs.size()}, {"missingGlyphCount", glyphs.count(0)}});
    }
    fonts.append(QJsonObject{{"class", QString::fromLatin1(widget->metaObject()->className())},
        {"name", widget->objectName()}, {"requestedFamily", widget->font().family()},
        {"resolvedFamily", info.family()}, {"pointSize", info.pointSizeF()},
        {"pixelSize", info.pixelSize()}, {"weight", int(info.weight())},
        {"mixedScriptGlyphRuns", runs}});
  }
  environment.insert("visibleWidgetFontResolution", fonts);
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
