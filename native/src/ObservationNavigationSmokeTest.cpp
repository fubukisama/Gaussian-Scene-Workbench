#include "ObservationNavigationSmokeTest.h"
#include "AppLanguage.h"
#include "NativeViewport.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>

namespace gsw {
bool runObservationNavigationSmokeTest(NativeViewport &viewport, const QString &path) {
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  QString source = path;
  if (source.isEmpty()) {
    source = QDir(temporary.path()).filePath("navigation.ply");
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write("ply\nformat ascii 1.0\nelement vertex 5\nproperty float x\nproperty float y\nproperty float z\nend_header\n-1 -1 0\n1 -1 0\n0 1 0\n0 0 1\n0 0 0\n");
  }
  viewport.setScene(source, 0);
  QElapsedTimer load; load.start();
  while (!viewport.selectableModelAvailable() && load.elapsed() < 15000) {
    QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
  }
  if (!viewport.selectableModelAvailable()) return false;
  viewport.setEditToolsLocked(true);
  viewport.setInteractionMode(NativeViewport::InteractionMode::Inspect);
  viewport.grabFramebuffer();
  const auto modelPosition = viewport.modelTranslation();
  const auto modelRotation = viewport.modelRotation();
  const auto modelScale = viewport.modelScale();
  const auto selected = viewport.selectedSceneIds();
  bool passed = true;
  const auto check = [&](bool ok, const char *label) {
    if (!ok) { passed = false; qCritical() << "Observation navigation FAIL:" << label; }
  };
  const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                        Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, point, viewport.mapToGlobal(point.toPoint()), button, buttons, modifiers);
    QApplication::sendEvent(&viewport, &event);
  };
  const auto drag = [&](QPointF start, QPointF end, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, modifiers);
    send(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton, modifiers);
    send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton, modifiers);
  };
  const QPointF center(viewport.width() * 0.5, viewport.height() * 0.5);
  const double radius = std::min(viewport.width(), viewport.height()) * 0.12;
  const auto sameFrame = [](OrbitAngles a, OrbitAngles b) {
    const auto first = orbitFrame(a), second = orbitFrame(b);
    return (first.cameraOffsetDirection - second.cameraOffsetDirection).length() < 1.0e-4F &&
           (first.upDirection - second.upDirection).length() < 1.0e-4F;
  };
  const QPointF start = center + QPointF(radius * 2.3, 0);
  for (const bool orthographic : {false, true}) {
    if (viewport.orthographicProjection() != orthographic) {
      const auto layout = navigationGizmoLayout(QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
      drag(layout.projectionCube.center(), layout.projectionCube.center());
    }
    check(viewport.orthographicProjection() == orthographic, "projection control remains usable while locked");
    for (const bool visible : {true, false}) {
      viewport.setShowObservationTrackball(visible);
      for (const QPointF direction : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) {
        const auto before = orbitFrame(viewport.viewOrbitAngles());
        const auto target = viewport.viewTarget();
        const auto distance = viewport.viewDistance();
        const QPointF outside = center + direction * (radius * 2.3);
        drag(outside, outside + direction * 30.0);
        const float change = (orbitFrame(viewport.viewOrbitAngles()).cameraOffsetDirection - before.cameraOffsetDirection).length();
        check(change > 0.01F, "radial drag outside sphere must orbit, not freeze or merely roll");
        check(viewport.viewTarget() == target && viewport.viewDistance() == distance, "orbit preserves pivot and zoom");
      }
      // Cross the guide in both directions during one drag; crossing an arc
      // must neither capture its axis nor clip motion to a sphere equator.
      send(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
      QPointF previous = center;
      for (const double scale : {0.7, 1.3, 2.3, 1.2, 0.1}) {
        const QPointF point = center + QPointF(radius * scale, 0);
        const auto expected = orbitAnglesAfterScreenDrag(viewport.viewOrbitAngles(), point - previous);
        send(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
        check(sameFrame(viewport.viewOrbitAngles(), expected), "crossing sphere boundary keeps free orbit continuous");
        previous = point;
      }
      send(QEvent::MouseButtonRelease, previous, Qt::LeftButton, Qt::NoButton);

      for (const Qt::MouseButton button : {Qt::LeftButton, Qt::MiddleButton, Qt::RightButton}) {
        const auto target = viewport.viewTarget();
        const auto angles = viewport.viewOrbitAngles();
        const auto distance = viewport.viewDistance();
        const auto modifiers = button == Qt::LeftButton ? Qt::ControlModifier : Qt::NoModifier;
        send(QEvent::MouseButtonPress, start, button, button, modifiers);
        send(QEvent::MouseMove, start + QPointF(30, 20), Qt::NoButton, button, modifiers);
        send(QEvent::MouseButtonRelease, start + QPointF(30, 20), button, Qt::NoButton, modifiers);
        check(viewport.viewTarget() != target, "Ctrl/middle/right pan works outside sphere");
        check(viewport.viewDistance() == distance && sameFrame(viewport.viewOrbitAngles(), angles), "pan changes only target");
      }

      // Start with orbit, switch to pan/zoom, then resume orbit without a new
      // button press. Neither the old rotation nor its pivot may snap back.
      for (const auto modifier : {Qt::ControlModifier, Qt::ShiftModifier}) {
        send(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, center + QPointF(20, 0), Qt::NoButton, Qt::LeftButton);
        const auto angles = viewport.viewOrbitAngles();
        const auto target = viewport.viewTarget();
        const auto distance = viewport.viewDistance();
        const QPointF point = center + QPointF(40, 20);
        send(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton, modifier);
        check(sameFrame(viewport.viewOrbitAngles(), angles), "modifier suspends rotation mid-drag");
        if (modifier == Qt::ControlModifier) {
          check(viewport.viewTarget() != target && viewport.viewDistance() == distance, "Ctrl switches ongoing orbit to pan");
        } else {
          check(viewport.viewDistance() > distance && viewport.viewTarget() == target, "Shift switches ongoing orbit to zoom");
        }
        const auto resumedTarget = viewport.viewTarget();
        const auto resumedDistance = viewport.viewDistance();
        send(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
        check(sameFrame(viewport.viewOrbitAngles(), angles), "modifier release alone never snaps camera");
        const auto expected = orbitAnglesAfterScreenDrag(angles, QPointF(12, 5));
        send(QEvent::MouseMove, point + QPointF(12, 5), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, point + QPointF(12, 5), Qt::LeftButton, Qt::NoButton);
        check(sameFrame(viewport.viewOrbitAngles(), expected), "resumed orbit uses fresh anchor");
        check(viewport.viewTarget() == resumedTarget && viewport.viewDistance() == resumedDistance, "resumed orbit retains pan and zoom");
      }
      auto distance = viewport.viewDistance();
      drag(start, start + QPointF(0, 20), Qt::ShiftModifier);
      check(viewport.viewDistance() > distance, "Shift zoom works outside sphere from press");
      distance = viewport.viewDistance();
      QWheelEvent wheel(start, viewport.mapToGlobal(start.toPoint()), QPoint(), QPoint(0, 120),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&viewport, &wheel);
      check(viewport.viewDistance() < distance, "wheel zoom works outside sphere");
      check(viewport.orthographicProjection() == orthographic, "navigation preserves projection");
    }
  }
  // The visible front arcs still explicitly capture their world axes. Choose
  // the frontmost point of each ring, away from ring intersections.
  viewport.setShowObservationTrackball(true);
  for (int axis = 0; axis < 3; ++axis) {
    viewport.resetCamera();
    const auto before = orbitFrame(viewport.viewOrbitAngles());
    QMatrix4x4 view;
    view.lookAt(before.cameraOffsetDirection, QVector3D(), before.upDirection);
    QVector3D world = before.cameraOffsetDirection;
    world[axis] = 0;
    world.normalize();
    const auto screen = [&](const QVector3D &value) {
      const auto point = view.mapVector(value);
      return center + QPointF(point.x() * radius, -point.y() * radius);
    };
    QVector3D direction; direction[axis] = 1;
    const auto point = screen(world);
    const auto end = screen(QQuaternion::fromAxisAndAngle(direction, 12).rotatedVector(world));
    drag(point, end);
    const auto after = orbitFrame(viewport.viewOrbitAngles());
    check((after.cameraOffsetDirection - before.cameraOffsetDirection).length() > 0.01F, "colored arc still rotates view");
    check(qAbs(after.cameraOffsetDirection[axis] - before.cameraOffsetDirection[axis]) < 1.0e-4F &&
          qAbs(after.upDirection[axis] - before.upDirection[axis]) < 1.0e-4F, "colored arc retains world-axis constraint");
  }
  send(QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
  const auto locale = AppLanguage::current();
  QStringList tooltips;
  for (const auto &language : AppLanguage::supported()) {
    AppLanguage::apply(language, false);
    tooltips.append(viewport.toolTip());
    check(viewport.toolTip().contains("Ctrl+") && viewport.toolTip().contains("Shift+"), "live navigation tooltip preserves shortcuts");
  }
  check(tooltips.size() == 3 && tooltips[0] != tooltips[1] && tooltips[1] != tooltips[2] && tooltips[0] != tooltips[2], "navigation tooltip changes language without mouse movement");
  AppLanguage::apply(locale, false);
  check(viewport.modelTranslation() == modelPosition && viewport.modelRotation() == modelRotation &&
        viewport.modelScale() == modelScale && viewport.selectedSceneIds() == selected, "observation retains model transforms and selection");
  check(!viewport.hasUnsavedSceneEdits() && !viewport.modelTransformActive(), "observation never edits model");
  qInfo() << "Observation navigation smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}
}
