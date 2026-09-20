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
#include <QFontMetricsF>
#include <algorithm>
#include <cmath>
#include <random>

namespace gsw {
static bool verifyShAppearance(NativeViewport &viewport, const QString &directory) {
  QFile file(QDir(directory).filePath("directional-sh.ply"));
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray header("ply\nformat ascii 1.0\nelement vertex 3\n");
  for (const char *p : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
      "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
    header += QByteArray("property float ") + p + '\n';
  for (int i = 0; i < 72; ++i) header += "property float f_rest_" + QByteArray::number(i) + '\n';
  file.write(header + "end_header\n20 20 20 -0.5 0.2 0.4 20 0 0 0 1 0 0 0 ");
  for (int i = 0; i < 72; ++i)
    file.write(i == 1 || i == 5 || i == 11 || i == 19 ? "0.18 " : "0 ");
  file.write("\n");
  // Invisible bounds keep camera fitting away from its singleton near-plane
  // limit; only the center Gaussian contributes to the sampled pixel.
  for (int offset : {-1, 1}) {
    const QByteArray p = QByteArray::number(20 + offset);
    file.write(p + ' ' + p + ' ' + p + " 0 0 0 -20 0 0 0 1 0 0 0 ");
    for (int i = 0; i < 72; ++i) file.write("0 ");
    file.write("\n");
  }
  file.close();
  bool loaded = false;
  const auto connection = QObject::connect(&viewport, &NativeViewport::sceneLoaded, &viewport,
      [&](qint64, qsizetype, qint64, qsizetype) { loaded = true; });
  viewport.setScene(file.fileName(), 3);
  QElapsedTimer timer; timer.start();
  while (!loaded && timer.elapsed() < 10000) {
    QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
  }
  QObject::disconnect(connection);
  if (!loaded) return false;
  viewport.setRenderMode(NativeViewport::RenderMode::Gaussians);
  viewport.setShowObservationTrackball(false);
  viewport.setEditToolsLocked(true);
  const auto toggleProjection = [&]() {
    const auto layout = navigationGizmoLayout(QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
    const auto point = layout.projectionCube.center();
    QMouseEvent press(QEvent::MouseButtonPress, point, viewport.mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, viewport.mapToGlobal(point.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press); QApplication::sendEvent(&viewport, &release);
  };
  for (int pass = 0; pass < 3; ++pass) {
    const QQuaternion rotation = pass == 0 ? QQuaternion() : QQuaternion::fromEulerAngles(37, 25, 16);
    const QVector3D scale = pass == 2 ? QVector3D(2, 0.7F, 1.2F) : QVector3D(1, 1, 1);
    viewport.setModelTransform({4, 5, 6}, rotation, scale);
    if (!viewport.focusModel()) return false;
    viewport.clearSelection();
    if (viewport.orthographicProjection() != (pass == 2)) toggleProjection();
    if (viewport.orthographicProjection() != (pass == 2)) return false;
    for (const auto axis : {NavigationAxis::PositiveZ, NavigationAxis::NegativeZ, NavigationAxis::PositiveX}) {
      viewport.setAxisView(axis);
      QEventLoop loop; QTimer::singleShot(350, &loop, &QEventLoop::quit); loop.exec();
      QMatrix4x4 model; model.rotate(rotation); model.scale(scale);
      const auto direction = model.inverted().mapVector(-orbitFrame(viewport.viewOrbitAngles()).cameraOffsetDirection).normalized();
      const double z = direction.z();
      const double bands[]{0.4886025119 * z, 0.3153915653 * (3*z*z-1),
          0.3731763326 * z * (5*z*z-3), 0.1057855469 * (35*z*z*z*z-30*z*z+3)};
      for (int degree = 0; degree <= 4; ++degree) {
        viewport.setMaximumShDegree(degree);
        const QImage frame = viewport.grabFramebuffer();
        if (frame.isNull() || viewport.effectiveShDegree() != degree) return false;
        const QColor actual = frame.pixelColor(frame.width()/2, frame.height()/2);
        double red = 0.5 - 0.5 * 0.2820947918;
        for (int l = 0; l < degree; ++l) red += 0.18 * bands[l];
        const double expected[]{red, 0.5 + 0.2 * 0.2820947918, 0.5 + 0.4 * 0.2820947918};
        const int pixels[]{actual.red(), actual.green(), actual.blue()};
        for (int c = 0; c < 3; ++c) {
          if (std::abs(pixels[c] - expected[c] * 255.0) > 3.0) {
            qCritical() << "Gaussian appearance FAIL: transform" << pass << "axis" << int(axis)
                << "degree" << degree << "channel" << c << "actual" << pixels[c] << "expected" << expected[c]*255;
            qCritical() << "appearance pixel" << actual << "selected" << viewport.modelSelected()
                << "target" << viewport.viewTarget() << "distance" << viewport.viewDistance();
            frame.save(QDir(qEnvironmentVariable("TEMP")).filePath("sh-appearance-failure.png"));
            return false;
          }
        }
      }
    }
  }
  if (viewport.orthographicProjection()) toggleProjection();
  viewport.setMaximumShDegree(-1);
  viewport.setScene({}, 0);
  qInfo() << "Gaussian appearance: SH 0-4, camera direction, rotation, nonuniform scale and orthographic checks passed";
  return true;
}

bool runGaussianPerformanceSmokeTest(NativeViewport &viewport, const QString &path) {
  QTemporaryDir temporary(QDir(QCoreApplication::applicationDirPath()).filePath("gaussian-benchmark-XXXXXX"));
  if (!temporary.isValid()) return false;
  if (!verifyShAppearance(viewport, temporary.path())) return false;
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
  viewport.setMaximumShDegree(-1);
  viewport.setEditToolsLocked(true);
  const auto send = [&](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, viewport.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &event);
  };
  auto frame = viewport.grabFramebuffer();
  qInfo() << "Gaussian benchmark: source SH" << viewport.sourceShDegree()
          << "effective SH" << viewport.effectiveShDegree();
  if (viewport.sourceShDegree() >= 0 &&
      !check(viewport.effectiveShDegree() == viewport.sourceShDegree(), "source SH must reach the GPU")) return false;
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
    const auto shBefore = viewport.gaussianShUploads();
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
      if (!check(viewport.gaussianShUploads() == shBefore, "navigation must not reupload SH")) return false;
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
  const auto shBeforeSwitch = viewport.gaussianShUploads();
  for (int degree = 0; degree <= 4; ++degree) {
    viewport.setMaximumShDegree(degree);
    frame = viewport.grabFramebuffer();
    if (viewport.sourceShDegree() >= 0 && !check(viewport.effectiveShDegree() ==
          std::min(degree, viewport.sourceShDegree()), "display degree capped by source")) return false;
  }
  viewport.setMaximumShDegree(-1);
  frame = viewport.grabFramebuffer();
  if (!check(viewport.gaussianShUploads() == shBeforeSwitch, "degree switch must reuse SH storage")) return false;
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
