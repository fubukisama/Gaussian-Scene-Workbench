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
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWindow>
#include <QDebug>

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
  processFor(250);
  if (viewport.grabFramebuffer().isNull()) return false;
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
  if (manifest.isEmpty() || !captured.value("stageSerialMatch").toBool()) {
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
  }
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
