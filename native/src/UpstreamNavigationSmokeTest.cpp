#include "UpstreamNavigationSmokeTest.h"

#include "AppLanguage.h"
#include "ModelInteraction.h"
#include "NativeViewport.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontMetricsF>
#include <QLineF>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace gsw {
namespace {
void processFor(const int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

QByteArray fileHash(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

bool writeFixture(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray bytes = "ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\n"
      "property float y\nproperty float z\nproperty uchar red\nproperty uchar green\n"
      "property uchar blue\nelement face 6\nproperty list uchar int vertex_indices\nend_header\n";
  for (int vertex = 0; vertex < 8; ++vertex) {
    bytes += QStringLiteral("%1 %2 %3 240 10 220\n")
        .arg(vertex & 1 ? 0.65 : -0.65).arg(vertex & 2 ? 0.65 : -0.65)
        .arg(vertex & 4 ? 0.65 : -0.65).toUtf8();
  }
  bytes += "4 0 1 3 2\n4 4 6 7 5\n4 0 4 5 1\n4 2 3 7 6\n4 0 2 6 4\n4 1 5 7 3\n";
  return file.write(bytes) == bytes.size();
}

// Independent camera-to-pixel oracle. It never calls the ported navigation
// helpers: the known target-plane point is obtained through the public ray
// intersection interface, then projected from public camera observations.
QMatrix4x4 cameraMatrix(const NativeViewport &viewport) {
  const auto frame = orbitFrame(viewport.viewOrbitAngles());
  const auto target = viewport.viewTarget();
  const float distance = viewport.viewDistance();
  QMatrix4x4 view;
  view.lookAt(target + frame.cameraOffsetDirection * distance, target, frame.upDirection);
  QMatrix4x4 projection;
  const float aspect = static_cast<float>(viewport.width()) / viewport.height();
  // Clip limits only affect Z. Relative oracle limits avoid precision loss at
  // the extreme distance test without consulting NativeViewport's internals.
  const float nearClip = std::max(distance * 0.01F, 1.0e-9F);
  const float farClip = std::max(distance * 1000.0F, nearClip * 100.0F);
  if (viewport.orthographicProjection()) {
    // The fixture and shared reference use identical scene units, so the
    // documented minimum orthographic half-height is .05 local units.
    constexpr float pi = 3.14159265358979323846F;
    const float extent = std::max(0.05F, distance *
        std::tan(viewport.perspectiveFovDegrees() * pi / 360.0F));
    projection.ortho(-extent * aspect, extent * aspect, -extent, extent, nearClip, farClip);
  } else {
    projection.perspective(viewport.perspectiveFovDegrees(), aspect, nearClip, farClip);
  }
  return projection * view;
}

std::optional<QVector3D> targetPlanePoint(const NativeViewport &viewport,
                                        const QPointF &pixel) {
  const auto ray = screenRay(pixel, viewport.size(), cameraMatrix(viewport));
  if (!ray) return std::nullopt;
  return rayPlaneIntersection(*ray, viewport.viewTarget(),
      orbitFrame(viewport.viewOrbitAngles()).cameraOffsetDirection);
}

QPointF projectPoint(const NativeViewport &viewport, const QVector3D &point) {
  const QVector4D clip = cameraMatrix(viewport) * QVector4D(point, 1);
  return {(clip.x() / clip.w() + 1) * viewport.width() * 0.5,
          (1 - clip.y() / clip.w()) * viewport.height() * 0.5};
}

qsizetype markerPixels(const QImage &image) {
  qsizetype count = 0;
  for (int y = 90; y < image.height() - 70; ++y) {
    for (int x = 70; x < image.width() - 100; ++x) {
      const auto color = image.pixelColor(x, y);
      if (color.red() > 60 && color.blue() > 60 &&
          color.red() > color.green() * 2 && color.blue() > color.green() * 2) ++count;
    }
  }
  return count;
}
} // namespace

bool runUpstreamNavigationSmokeTest(NativeViewport &viewport) {
  QElapsedTimer total;
  total.start();
  bool passed = true;
  const auto check = [&](const bool condition, const char *message) {
    if (!condition) {
      qCritical() << "Upstream navigation FAIL:" << message;
      passed = false;
    }
    return condition;
  };
  auto *action = viewport.window()->findChild<QAction *>(QStringLiteral("zoomToCursorAction"));
  if (!check(action && action->isCheckable(), "real View menu zoom-to-cursor action exists")) return false;
  QSettings settings;
  const QString settingKey = QStringLiteral("view/zoomToCursor");
  const bool settingExisted = settings.contains(settingKey);
  const QVariant savedSetting = settings.value(settingKey);
  const bool oldChecked = action->isChecked();
  const QString oldLanguage = AppLanguage::current();
  const QSize oldMinimum = viewport.minimumSize();
  const QSize oldMaximum = viewport.maximumSize();
  const auto restore = qScopeGuard([&] {
    action->setChecked(oldChecked);
    if (settingExisted) settings.setValue(settingKey, savedSetting);
    else settings.remove(settingKey);
    settings.sync();
    AppLanguage::apply(oldLanguage, false);
    viewport.setMinimumSize(oldMinimum);
    viewport.setMaximumSize(oldMaximum);
  });

  QString fixtureRoot = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
  if (fixtureRoot.isEmpty()) {
    fixtureRoot = QDir(QStringLiteral("D:/")).exists()
        ? QStringLiteral("D:/Codex/qa-upstream-navigation") : QDir::tempPath();
  }
  if (!check(QDir().mkpath(fixtureRoot), "test-owned fixture root is available")) return false;
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("upstream-navigation-XXXXXX")));
  if (!check(temporary.isValid(), "isolated temporary fixture directory exists")) return false;
  const QString source = QDir(temporary.path()).filePath(QStringLiteral("navigation-marker.ply"));
  if (!check(writeFixture(source), "test-owned mesh fixture is written")) return false;
  const QByteArray originalHash = fileHash(source);
  viewport.setScene(source, 0);
  while (!viewport.meshRenderingAvailable() && total.elapsed() < 8000) processFor(10);
  if (!check(viewport.meshRenderingAvailable(), "actual viewport loaded the mesh fixture")) return false;
  viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport.setInteractionMode(NativeViewport::InteractionMode::Inspect);
  viewport.selectModel();
  viewport.setEditToolsLocked(true);
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  const auto translation = viewport.modelTranslation();
  const auto rotation = viewport.modelRotation();
  const auto scale = viewport.modelScale();
  const auto selected = viewport.selectedSceneIds();
  const auto sourceCoordinates = viewport.sceneCoordinates();

  struct ExpectedLocale { const char *locale; const char *title; const char *tooltip; };
  const ExpectedLocale locales[] = {
      {"zh_CN", "缩放到光标", "滚轮围绕光标在视点目标平面上的位置缩放；关闭后围绕视图中心缩放。立即生效并记住选择，不修改模型坐标。"},
      {"en_US", "Zoom to Cursor", "The wheel zooms around the cursor on the view-target plane; when off, zoom around the view center. Applies immediately and remembers your choice without changing model coordinates."},
      {"ja_JP", "カーソル位置へズーム", "ホイールは注視点を通る平面上のカーソル位置を基準にズームします。無効にするとビューの中心を基準にします。即時に反映して選択を保存し、モデル座標は変更しません。"}};
  for (const auto &locale : locales) {
    check(AppLanguage::apply(QString::fromLatin1(locale.locale), false), "language changes without restart");
    check(action->text() == QString::fromUtf8(locale.title), "action title matches independent locale expectation");
    check(action->toolTip() == QString::fromUtf8(locale.tooltip), "action tooltip matches independent locale expectation");
  }
  AppLanguage::apply(oldLanguage, false);
  const auto setCursorZoom = [&](const bool enabled) {
    if (action->isChecked() != enabled) action->trigger();
    settings.sync();
    QSettings reloaded;
    check(action->isChecked() == enabled && viewport.zoomToCursorEnabled() == enabled,
          "menu action changes viewport behavior immediately");
    check(reloaded.value(settingKey).toBool() == enabled && reloaded.status() == QSettings::NoError,
          "a fresh settings reader observes the persisted choice");
  };
  const auto sendMouse = [&](const QEvent::Type type, const QPointF &point,
      const Qt::MouseButton button, const Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, viewport.mapToGlobal(point.toPoint()), button,
                      buttons, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &event);
  };
  const auto drag = [&](const QPointF &point, const QPointF &delta,
                        const Qt::MouseButton button) {
    sendMouse(QEvent::MouseButtonPress, point, button, button);
    sendMouse(QEvent::MouseMove, point + delta, Qt::NoButton, button);
    sendMouse(QEvent::MouseButtonRelease, point + delta, button, Qt::NoButton);
  };
  const auto wheel = [&](const QPointF &cursor, const int angleDelta) {
    QWheelEvent event(cursor, viewport.mapToGlobal(cursor.toPoint()), QPoint(),
        QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&viewport, &event);
  };
  const auto capture = [&](const QString &label) {
    if (!check(viewport.isVisible() && !viewport.window()->isMinimized(),
        "real-framebuffer acceptance requires a visible runner window")) return false;
    const QImage frame = viewport.grabFramebuffer();
    check(!frame.isNull() && markerPixels(frame) > 100,
          "the magenta test model remains visible in the actual framebuffer");
    const QString output = QDir(fixtureRoot).filePath(QStringLiteral("%1-upstream-navigation-%2.png")
        .arg(AppLanguage::current(), label));
    return check(frame.save(output), "real framebuffer evidence is saved");
  };

  for (const QSize size : {QSize(720, 420), QSize(480, 560)}) {
    viewport.setFixedSize(size);
    processFor(20);
    for (const bool upsideDown : {false, true}) {
      for (const bool orthographic : {false, true}) {
        if (!check(total.elapsed() < 28000, "real-input acceptance stays bounded")) return false;
        viewport.resetCamera();
        const QPointF emptyArea(viewport.width() * 0.15, viewport.height() * 0.4);
        if (upsideDown) {
          for (int step = 0; step < 3; ++step) drag(emptyArea, QPointF(0, 200), Qt::LeftButton);
          check(std::abs(viewport.viewOrbitAngles().rollDegrees) > 170,
                "real observation drags reach an upside-down camera");
        }
        if (viewport.orthographicProjection() != orthographic) {
          const auto layout = navigationGizmoLayout(QMatrix4x4(), viewport.size(),
                                                    QFontMetricsF(viewport.font()).height());
          drag(layout.projectionCube.center(), {}, Qt::LeftButton);
        }
        check(viewport.orthographicProjection() == orthographic, "real projection button selects the test mode");
        const QString label = QStringLiteral("%1x%2-%3-%4").arg(size.width()).arg(size.height())
            .arg(orthographic ? QStringLiteral("ortho") : QStringLiteral("persp"))
            .arg(upsideDown ? QStringLiteral("inverted") : QStringLiteral("upright"));
        const QPointF cursor(viewport.width() * 0.62, viewport.height() * 0.43);
        setCursorZoom(false);
        const auto centeredTarget = viewport.viewTarget();
        const float centeredDistance = viewport.viewDistance();
        wheel(cursor, 120);
        check(viewport.viewTarget() == centeredTarget && viewport.viewDistance() < centeredDistance,
              "disabled cursor zoom keeps the existing view center while zooming");
        setCursorZoom(true);
        const auto anchor = targetPlanePoint(viewport, cursor);
        if (!check(anchor.has_value(), "independent ray oracle finds the target-plane anchor")) return false;
        const auto angles = viewport.viewOrbitAngles();
        wheel(cursor, 120);
        check(QLineF(projectPoint(viewport, *anchor), cursor).length() < 0.2,
              "enabled wheel zoom preserves the original target-plane cursor anchor");
        check(viewport.viewOrbitAngles() == angles, "wheel zoom does not rotate the camera");
        const QPointF dragDelta(24, -17);
        const QPointF beforePan = projectPoint(viewport, *anchor);
        const float panDistance = viewport.viewDistance();
        drag(emptyArea, dragDelta, Qt::MiddleButton);
        check(QLineF(projectPoint(viewport, *anchor), beforePan + dragDelta).length() < 0.2,
              "actual middle-button pan follows the pointer in projection pixels");
        check(viewport.viewOrbitAngles() == angles && viewport.viewDistance() == panDistance,
              "pan preserves camera orientation and distance");
        if (!capture(label)) return false;

        const auto limitAnchor = targetPlanePoint(viewport, cursor);
        if (!check(limitAnchor.has_value(), "limit test has an independent anchor")) return false;
        // A very large zoom-out request reaches the actual scene distance
        // limit. The anchor uses that actual ratio, not the requested factor.
        wheel(cursor, -12000);
        check(QLineF(projectPoint(viewport, *limitAnchor), cursor).length() < 0.2,
              "cursor anchor survives a clamped real wheel request");
        const auto limitedTarget = viewport.viewTarget();
        const float limitedDistance = viewport.viewDistance();
        wheel(cursor, -12000);
        check(viewport.viewDistance() == limitedDistance &&
              (viewport.viewTarget() - limitedTarget).length() < 0.0001F,
              "another wheel at the same distance limit introduces no target drift");
      }
    }
  }
  viewport.resetCamera();
  viewport.setAxisView(NavigationAxis::PositiveX);
  processFor(25);
  wheel(QPointF(viewport.width() * 0.6, viewport.height() * 0.4), 120);
  const auto stoppedAngles = viewport.viewOrbitAngles();
  const auto stoppedTarget = viewport.viewTarget();
  const float stoppedDistance = viewport.viewDistance();
  processFor(cameraSnapDurationMilliseconds + 40);
  check(viewport.viewOrbitAngles() == stoppedAngles && viewport.viewTarget() == stoppedTarget &&
        viewport.viewDistance() == stoppedDistance, "real wheel input stops an in-flight axis-view animation");
  check(fileHash(source) == originalHash && !originalHash.isEmpty(), "original fixture bytes remain unchanged");
  check(viewport.scenePath() == source && viewport.modelTranslation() == translation &&
        viewport.modelRotation() == rotation && viewport.modelScale() == scale &&
        viewport.selectedSceneIds() == selected && !viewport.modelTransformActive() &&
        !viewport.hasUnsavedSceneEdits(), "navigation preserves source, model TRS and selection");
  const auto unchangedCoordinates = viewport.sceneCoordinates();
  check(unchangedCoordinates.displayScale == sourceCoordinates.displayScale &&
        unchangedCoordinates.displayShift.x == sourceCoordinates.displayShift.x &&
        unchangedCoordinates.displayShift.y == sourceCoordinates.displayShift.y &&
        unchangedCoordinates.displayShift.z == sourceCoordinates.displayShift.z,
        "source coordinate mapping is unchanged");
  check(total.elapsed() < 30000, "full actual-input acceptance finishes within thirty seconds");
  qInfo() << "Upstream navigation smoke:" << (passed ? "PASS" : "FAIL") << "elapsed ms" << total.elapsed();
  return passed;
}
} // namespace gsw
