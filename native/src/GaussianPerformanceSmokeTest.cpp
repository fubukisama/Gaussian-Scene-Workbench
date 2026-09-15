#include "GaussianPerformanceSmokeTest.h"
#include "NativeViewport.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <random>

namespace gsw {
bool runGaussianPerformanceSmokeTest(NativeViewport &viewport, const QString &path) {
  QTemporaryDir temporary(QDir(QCoreApplication::applicationDirPath()).filePath("gaussian-benchmark-XXXXXX"));
  if (!temporary.isValid()) return false;
  QString scenePath = path;
  if (scenePath.isEmpty()) {
    scenePath = QDir(temporary.path()).filePath("gaussians.ply");
    QFile file(scenePath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray header("ply\nformat binary_little_endian 1.0\nelement vertex 512202\n");
    for (const auto *property : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                                "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
      header += QByteArray("property float ") + property + '\n';
    header += "end_header\n";
    if (file.write(header) != header.size()) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    std::mt19937 random(42);
    std::uniform_real_distribution<float> coordinate(-1.0F, 1.0F);
    for (int i = 0; i < 512202; ++i) {
      stream << coordinate(random) << coordinate(random) << coordinate(random)
             << 0.7F << -0.3F << 0.1F << 0.0F
             << -5.0F << -6.0F << -7.0F << 0.7F << 0.1F << 0.2F << 0.4F;
    }
    if (stream.status() != QDataStream::Ok || !file.flush()) return false;
  }
  const auto check = [](bool ok, const char *message) {
    if (!ok) qCritical() << "Gaussian benchmark FAIL:" << message;
    return ok;
  };
  bool loaded = false;
  qsizetype expectedCount = 0;
  const auto connection = QObject::connect(&viewport, &NativeViewport::sceneLoaded, &viewport,
      [&](qint64 count, qsizetype preview, qint64, qsizetype) {
        loaded = count > 0;
        expectedCount = preview;
        qInfo() << "Gaussian benchmark: source" << count << "preview" << preview;
      });
  viewport.setScene(scenePath, 0);
  QElapsedTimer loading; loading.start();
  while (!loaded && loading.elapsed() < 30000) {
    QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
  }
  QObject::disconnect(connection);
  if (!loaded || !viewport.gaussianRenderingAvailable()) return false;
  viewport.setRenderMode(NativeViewport::RenderMode::Gaussians);
  viewport.setEditToolsLocked(true);
  const auto send = [&](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, viewport.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &event);
  };
  auto frame = viewport.grabFramebuffer();
  qInfo() << "Gaussian benchmark: physical framebuffer" << frame.size();
  const bool indexed = viewport.indexedGaussianRendering();
  qInfo() << "Gaussian benchmark: GPU-resident attributes" << indexed;
  if (!check(viewport.renderedPointCount() == expectedCount, "all loaded Gaussians drawn")) return false;
  if (qEnvironmentVariableIsSet("GSW_EXPECT_INDEXED_GAUSSIANS") && !indexed) return false;
  const QPoint start(viewport.width() / 2 + 50, viewport.height() / 2);
  double idleMedian = 0.0;
  bool withinBudget = true;
  const int orbitBudget = qEnvironmentVariableIntValue("GSW_GAUSSIAN_ORBIT_BUDGET_MS");
  const QString imageDirectory = qEnvironmentVariable("GSW_GAUSSIAN_BENCHMARK_DIR");
  for (const auto phase : {"idle", "orbit", "wheel", "hover"}) {
    const auto attributesBefore = viewport.gaussianAttributeUploads();
    const auto orderBefore = viewport.gaussianOrderUploads();
    QVector<double> frameTimes, inputTimes;
    if (QString::fromLatin1(phase) == QStringLiteral("orbit"))
      send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    for (int i = 0; i < 40; ++i) {
      QElapsedTimer timer; timer.start();
      if (QString::fromLatin1(phase) == QStringLiteral("orbit"))
        send(QEvent::MouseMove, start + QPoint(i * 4, i % 5), Qt::NoButton, Qt::LeftButton);
      if (QString::fromLatin1(phase) == QStringLiteral("hover"))
        send(QEvent::MouseMove, start + QPoint(i * 2, 0), Qt::NoButton, Qt::NoButton);
      if (QString::fromLatin1(phase) == QStringLiteral("wheel")) {
        QWheelEvent event(start, viewport.mapToGlobal(start), QPoint(), QPoint(0, i % 2 ? 120 : -120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&viewport, &event);
      }
      inputTimes.append(timer.nsecsElapsed() / 1.0e6);
      frame = viewport.grabFramebuffer(); // complete render + readback, identical for every phase
      frameTimes.append(timer.nsecsElapsed() / 1.0e6);
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    if (QString::fromLatin1(phase) == QStringLiteral("orbit"))
      send(QEvent::MouseButtonRelease, start + QPoint(156, 4), Qt::LeftButton, Qt::NoButton);
    std::sort(frameTimes.begin(), frameTimes.end());
    std::sort(inputTimes.begin(), inputTimes.end());
    qInfo().nospace() << "Gaussian benchmark " << phase << ": frame p50=" << frameTimes[20]
        << " ms p95=" << frameTimes[37] << " ms, input p95=" << inputTimes[37] << " ms";
    if (QString::fromLatin1(phase) == QStringLiteral("idle")) idleMedian = frameTimes[20];
    if (QString::fromLatin1(phase) == QStringLiteral("orbit") && orbitBudget > 0) {
      withinBudget = frameTimes[20] <= idleMedian + orbitBudget;
      if (!withinBudget) qCritical() << "Gaussian benchmark: orbit exceeded idle +" << orbitBudget << "ms";
    }
    if (!imageDirectory.isEmpty()) {
      if (!QDir().mkpath(imageDirectory) ||
          !frame.save(QDir(imageDirectory).filePath(QString::fromLatin1(phase) + ".png"))) return false;
    }
    if (indexed) {
      if (!check(viewport.gaussianAttributeUploads() == attributesBefore, "navigation must not reupload attributes") ||
          !check(viewport.renderedPointCount() == expectedCount, "navigation must not decimate Gaussians")) return false;
      if (QString::fromLatin1(phase) == QStringLiteral("orbit")) {
        if (!check(viewport.gaussianOrderUploads() > orderBefore, "rotation must refresh depth order")) return false;
      } else if (!check(viewport.gaussianOrderUploads() == orderBefore, "unchanged direction must reuse depth order")) return false;
    }
  }
  if (indexed) {
    const auto translation = viewport.modelTranslation();
    const auto rotation = viewport.modelRotation();
    const auto scale = viewport.modelScale();
    const auto attributesBefore = viewport.gaussianAttributeUploads();
    const auto orderBefore = viewport.gaussianOrderUploads();
    viewport.setModelTransform(translation, QQuaternion::fromAxisAndAngle(0, 0, 1, 31) * rotation,
                               QVector3D(scale.x() * 1.2F, scale.y() * 0.8F, scale.z()));
    frame = viewport.grabFramebuffer();
    if (!check(viewport.gaussianAttributeUploads() == attributesBefore && viewport.gaussianOrderUploads() > orderBefore,
               "model rotation/nonuniform scale changes order without reuploading attributes")) return false;
    viewport.setModelTransform(translation, rotation, scale);
    frame = viewport.grabFramebuffer();
  }
  // Differential image check at the same camera: resident/indexed versus the
  // compatibility VAO. Ignore only UI margins containing changing FPS text.
  if (indexed) {
    const QRect region(80, 100, frame.width() - 160, frame.height() - 200);
    const auto resident = viewport.grabFramebuffer().copy(region).convertToFormat(QImage::Format_RGB32);
    qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", "1");
    viewport.setRenderMode(NativeViewport::RenderMode::Points);
    viewport.setRenderMode(NativeViewport::RenderMode::Gaussians);
    const auto compatibility = viewport.grabFramebuffer().copy(region).convertToFormat(QImage::Format_RGB32);
    qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
    int maximumDifference = 0, changedPixels = 0;
    for (int y = 0; y < resident.height(); ++y) {
      const auto *a = reinterpret_cast<const QRgb *>(resident.constScanLine(y));
      const auto *b = reinterpret_cast<const QRgb *>(compatibility.constScanLine(y));
      for (int x = 0; x < resident.width(); ++x) {
        if (a[x] != b[x]) ++changedPixels;
        maximumDifference = std::max({maximumDifference, std::abs(qRed(a[x]) - qRed(b[x])),
            std::abs(qGreen(a[x]) - qGreen(b[x])), std::abs(qBlue(a[x]) - qBlue(b[x]))});
      }
    }
    qInfo() << "Gaussian benchmark: image max channel difference" << maximumDifference << "changed pixels" << changedPixels;
    if (!imageDirectory.isEmpty()) {
      resident.save(QDir(imageDirectory).filePath("resident.png"));
      compatibility.save(QDir(imageDirectory).filePath("compatibility.png"));
    }
    // Different driver vertex-fetch paths can round the final UNORM color by
    // one level. Geometry/depth/order are unchanged; anything larger fails.
    if (!check(maximumDifference <= 1, "resident and compatibility frames differ beyond 8-bit rounding")) return false;
    viewport.setRenderMode(NativeViewport::RenderMode::Points);
    frame = viewport.grabFramebuffer();
    if (!check(viewport.renderedPointCount() == expectedCount, "point mode retains all centers")) return false;
    viewport.setRenderMode(NativeViewport::RenderMode::Gaussians);
    frame = viewport.grabFramebuffer();
    if (!check(viewport.indexedGaussianRendering(), "Gaussian mode restores resident rendering")) return false;
  }
  viewport.setEditToolsLocked(false);
  const auto attributesBeforeEdit = viewport.gaussianAttributeUploads();
  viewport.invertSelection();
  frame = viewport.grabFramebuffer();
  if (indexed && !check(viewport.gaussianAttributeUploads() > attributesBeforeEdit, "selection updates attributes")) return false;
  viewport.deleteSelection();
  frame = viewport.grabFramebuffer();
  if (!check(viewport.renderedPointCount() == 0, "deletion clears rendered Gaussians")) return false;
  viewport.undoEdit();
  frame = viewport.grabFramebuffer();
  if (!check(viewport.renderedPointCount() == expectedCount, "undo restores all Gaussians")) return false;
  viewport.clearSelection();
  frame = viewport.grabFramebuffer();
  viewport.setScene({}, 0);
  frame = viewport.grabFramebuffer();
  if (!check(!viewport.indexedGaussianRendering() && viewport.renderedPointCount() == 0,
             "closing a scene invalidates resident data")) return false;
  qInfo() << "Gaussian benchmark: image equivalence, residency, editing and cleanup checks passed";
  return !frame.isNull() && withinBudget;
}
}
