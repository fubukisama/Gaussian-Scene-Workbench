#include "EditNavigationSmokeTest.h"

#include "AppLanguage.h"
#include "NativeViewport.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontMetricsF>
#include <QImage>
#include <QLineF>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>

namespace gsw {
namespace {
struct NavigationTestAborted {};

void processFor(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

struct Marker {
  QPointF center;
  qsizetype pixels = 0;
};

Marker markerIn(const QImage &image) {
  Marker result;
  // The generated magenta mesh is the oracle. Ignore overlays and controls.
  for (int y = 100; y < image.height() - 90; ++y) {
    for (int x = 100; x < image.width() - 130; ++x) {
      const auto color = image.pixelColor(x, y);
      if (color.red() > 55 && color.blue() > 55 &&
          color.red() > color.green() * 2 && color.blue() > color.green() * 2) {
        result.center += QPointF(x, y);
        ++result.pixels;
      }
    }
  }
  if (result.pixels) result.center /= result.pixels;
  return result;
}

bool writeFixture(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray body;
  for (int cube = 0; cube < 2; ++cube) {
    for (int vertex = 0; vertex < 8; ++vertex) {
      body += QString("%1 %2 %3 %4\n")
          .arg((cube == 0 ? -0.8 : 0.8) + (vertex & 1 ? 0.3 : -0.3))
          .arg((cube == 0 ? 0.4 : -0.4) + (vertex & 2 ? 0.3 : -0.3))
          .arg((cube == 0 ? 0.2 : -0.2) + (vertex & 4 ? 0.3 : -0.3))
          .arg(cube == 0 ? "240 15 240" : "90 90 90").toUtf8();
    }
  }
  for (int cube = 0; cube < 2; ++cube) {
    const int offset = cube * 8;
    for (const auto &face : {QList<int>{0, 1, 3, 2}, {4, 6, 7, 5},
                             {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}}) {
      body += QString("4 %1 %2 %3 %4\n").arg(face[0] + offset).arg(face[1] + offset)
          .arg(face[2] + offset).arg(face[3] + offset).toUtf8();
    }
  }
  const QByteArray header = "ply\nformat ascii 1.0\nelement vertex 16\nproperty float x\n"
      "property float y\nproperty float z\nproperty uchar red\nproperty uchar green\n"
      "property uchar blue\nelement face 12\nproperty list uchar int vertex_indices\nend_header\n";
  return file.write(header + body) == header.size() + body.size();
}
}

static bool runEditNavigationSmokeTestImpl(NativeViewport &viewport) {
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const QString path = QDir(temporary.path()).filePath("edit-navigation.ply");
  if (!writeFixture(path)) return false;
  viewport.setScene(path, 0);
  QElapsedTimer load;
  load.start();
  while (!viewport.meshRenderingAvailable() && load.elapsed() < 15000) processFor(10);
  if (!viewport.meshRenderingAvailable()) return false;
  viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport.setInteractionMode(NativeViewport::InteractionMode::Inspect);
  viewport.clearSelection();
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  const auto translation = viewport.modelTranslation();
  const auto rotation = viewport.modelRotation();
  const auto scale = viewport.modelScale();
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qCritical() << "Edit navigation FAIL:" << message;
      passed = false;
    }
  };
  const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                        Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, point, viewport.mapToGlobal(point.toPoint()), button, buttons, modifiers);
    QApplication::sendEvent(&viewport, &event);
  };
  const QPointF emptyArea(viewport.width() * 0.15, viewport.height() * 0.4);
  const auto dragAt = [&](QPointF start, QPointF delta, Qt::MouseButton button = Qt::LeftButton,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    send(QEvent::MouseButtonPress, start, button, button, modifiers);
    send(QEvent::MouseMove, start + delta, Qt::NoButton, button, modifiers);
    send(QEvent::MouseButtonRelease, start + delta, button, Qt::NoButton, modifiers);
  };
  enum class Pose { Default, Opposite, UpsideDown, Pole };
  const auto poseName = [](Pose pose) {
    switch (pose) {
    case Pose::Default: return "default";
    case Pose::Opposite: return "opposite";
    case Pose::UpsideDown: return "upside-down";
    case Pose::Pole: return "pole";
    }
    return "unknown";
  };
  const auto navigationLayout = [&] {
    return navigationGizmoLayout(QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
  };
  const auto prepare = [&](Pose pose, bool orthographic) {
    viewport.setEditToolsLocked(true);
    viewport.resetCamera();
    if (pose == Pose::UpsideDown) {
      // Reach the rolled camera by actual observation drags, not private state.
      for (int step = 0; step < 3; ++step) dragAt(emptyArea, QPointF(0, 200));
      check(std::abs(viewport.viewOrbitAngles().rollDegrees) > 170,
            "test pose reaches an upside-down camera");
    } else if (pose == Pose::Opposite || pose == Pose::Pole) {
      viewport.setAxisView(pose == Pose::Opposite ? NavigationAxis::NegativeY : NavigationAxis::PositiveZ);
      processFor(260);
    }
    if (viewport.orthographicProjection() != orthographic) {
      dragAt(navigationLayout().projectionCube.center(), {});
    }
    check(viewport.orthographicProjection() == orthographic, "projection control selects the test projection");
  };
  const QString screenshotRoot = qEnvironmentVariable("GSW_EDIT_NAVIGATION_SCREENSHOT_DIR");
  if (!screenshotRoot.isEmpty()) check(QDir().mkpath(screenshotRoot), "screenshot directory is available");
  int frameNumber = 0;
  const auto capture = [&](const QString &label) {
    const auto requireVisibleWindow = [&] {
      if (!viewport.isVisible() || viewport.window()->isMinimized()) {
        qCritical() << "Edit navigation smoke interrupted:" << label
                    << "viewport visible" << viewport.isVisible()
                    << "window minimized" << viewport.window()->isMinimized()
                    << "window state" << viewport.window()->windowState()
                    << "Real framebuffer validation requires the test window to remain visible;"
                       " no navigation conclusion is drawn from an unpainted framebuffer.";
        throw NavigationTestAborted{};
      }
    };
    requireVisibleWindow();
    const auto image = viewport.grabFramebuffer();
    requireVisibleWindow();
    if (!screenshotRoot.isEmpty()) {
      check(image.save(QDir(screenshotRoot).filePath(QString("%1-%2-%3.png")
          .arg(AppLanguage::current()).arg(++frameNumber, 3, 10, QChar('0')).arg(label))), "framebuffer evidence is saved");
    }
    return markerIn(image);
  };
  const auto sameDirection = [&](const Marker &before, const Marker &after,
                                 const QPointF &reference, const QString &label) {
    const QPointF movement = after.center - before.center;
    qInfo() << "Edit navigation marker:" << label << "reference" << reference << "actual" << movement
            << "pixels" << before.pixels << after.pixels;
    check(before.pixels > 100 && after.pixels > 100, "test marker remains visible in the framebuffer");
    check(QLineF(QPointF(), reference).length() > 0.5, "reference orbit visibly moves the off-center marker");
    check(QPointF::dotProduct(reference, movement) > 0 && QLineF(reference, movement).length() < 2,
          "all view-navigation entry points preserve the observation drag's screen direction");
  };

  // This compares two real public input paths, not the implementation's camera
  // mathematics. The same marker must go to the same screen position when
  // editing is unlocked, even beyond the poles and in the opposite view.
  for (const bool orthographic : {false, true}) {
    for (const Pose pose : {Pose::Default, Pose::Opposite, Pose::UpsideDown, Pose::Pole}) {
      for (const QPointF delta : {QPointF(45, 0), QPointF(-45, 0), QPointF(0, 45), QPointF(0, -45)}) {
        const QString label = QString("%1-%2-%3-%4").arg(poseName(pose))
            .arg(orthographic ? "ortho" : "perspective").arg(delta.x()).arg(delta.y());
        prepare(pose, orthographic);
        const auto lockedBefore = capture(label + "-locked-before");
        dragAt(emptyArea, delta);
        const auto lockedAfter = capture(label + "-locked-after");
        check(lockedBefore.pixels > 100 && lockedAfter.pixels > 100,
              "observation reference marker is visible");
        prepare(pose, orthographic);
        viewport.setEditToolsLocked(false);
        const auto editBefore = capture(label + "-edit-before");
        const auto target = viewport.viewTarget();
        const float distance = viewport.viewDistance();
        dragAt(emptyArea, delta);
        const auto editAfter = capture(label + "-edit-after");
        sameDirection(editBefore, editAfter, lockedAfter.center - lockedBefore.center, label);
        check(viewport.viewTarget() == target && viewport.viewDistance() == distance &&
              viewport.orthographicProjection() == orthographic,
              "orbit preserves camera pivot, distance, and projection");
      }
    }
  }

  // Pan uses a different convention from orbit: the whole scene follows the
  // pointer. Assert literal screen directions, including an upside-down view.
  for (const bool orthographic : {false, true}) {
    for (const Qt::MouseButton button : {Qt::MiddleButton, Qt::RightButton, Qt::LeftButton}) {
      for (const QPointF delta : {QPointF(30, 0), QPointF(-30, 0), QPointF(0, 30), QPointF(0, -30)}) {
        prepare(Pose::UpsideDown, orthographic);
        viewport.setEditToolsLocked(false);
        const auto before = capture("pan-before");
        const auto angles = viewport.viewOrbitAngles();
        const float distance = viewport.viewDistance();
        dragAt(emptyArea, delta, button, button == Qt::LeftButton ? Qt::ShiftModifier : Qt::NoModifier);
        const auto after = capture("pan-after");
        const QPointF movement = after.center - before.center;
        check(before.pixels > 100 && after.pixels > 100, "pan marker remains visible");
        check(QPointF::dotProduct(movement, delta) > 30 &&
              std::abs(movement.x() * delta.y() - movement.y() * delta.x()) < 30,
              "middle/right/Shift-left pan follows the cursor, without swapped screen axes");
        check(viewport.viewOrbitAngles() == angles && viewport.viewDistance() == distance,
              "pan preserves viewing orientation and distance");
      }
    }
  }

  // Trimming tools' temporary Ctrl orbit and the navigation gizmo share the
  // same user-visible camera operation. Neither may resurrect the old sign.
  for (const bool orthographic : {false, true}) {
    for (const QPointF delta : {QPointF(35, 0), QPointF(0, 35)}) {
      prepare(Pose::UpsideDown, orthographic);
      const auto before = capture("shortcut-reference-before");
      dragAt(emptyArea, delta);
      const auto after = capture("shortcut-reference-after");
      const QPointF reference = after.center - before.center;
      for (const auto mode : {NativeViewport::InteractionMode::Rectangle,
                              NativeViewport::InteractionMode::Lasso,
                              NativeViewport::InteractionMode::Brush}) {
        prepare(Pose::UpsideDown, orthographic);
        viewport.setEditToolsLocked(false);
        viewport.setInteractionMode(mode);
        const auto trimBefore = capture("trim-before");
        dragAt(emptyArea, delta, Qt::LeftButton, Qt::ControlModifier);
        const auto trimAfter = capture("trim-after");
        sameDirection(trimBefore, trimAfter, reference, "Ctrl temporary orbit in trim tool");
      }
      prepare(Pose::UpsideDown, orthographic);
      viewport.setEditToolsLocked(false);
      const auto gizmoBefore = capture("gizmo-before");
      const auto layout = navigationLayout();
      // A drag anywhere on the surrounding navigation sphere rotates; the
      // center projection toggle is deliberately avoided.
      dragAt(layout.center + QPointF(layout.radius * 0.65, 0), delta);
      const auto gizmoAfter = capture("gizmo-after");
      sameDirection(gizmoBefore, gizmoAfter, reference, "navigation gizmo orbit");
    }
  }
  check(viewport.modelTranslation() == translation && viewport.modelRotation() == rotation &&
        viewport.modelScale() == scale && !viewport.modelTransformActive() && !viewport.hasUnsavedSceneEdits(),
        "camera navigation never edits the model");
  qInfo() << "Edit navigation smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runEditNavigationSmokeTest(NativeViewport &viewport) {
  try {
    return runEditNavigationSmokeTestImpl(viewport);
  } catch (const NavigationTestAborted &) {
    return false;
  }
}
}
