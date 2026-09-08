#include "MultiSceneSmokeTest.h"
#include "MainWindow.h"
#include "NativeViewport.h"
#include "WorkspaceDocument.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <functional>

namespace gsw {
namespace {
bool waitUntil(const std::function<bool()> &predicate, int timeout = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return predicate();
}

struct ColorFootprint {
  int count = 0;
  QPointF center;
};

ColorFootprint footprint(const QImage &image, bool red) {
  ColorFootprint result;
  // Ignore toolbars, labels and the navigation gizmo at the image edges.
  for (int y = 100; y < image.height() - 100; ++y) {
    for (int x = 120; x < image.width() - 140; ++x) {
      const QColor c = image.pixelColor(x, y);
      const int primary = red ? c.red() : c.blue();
      const int secondary = red ? c.blue() : c.red();
      if (primary > 70 && primary > secondary * 2 && primary > c.green() * 2) {
        ++result.count;
        result.center += QPointF(x, y);
      }
    }
  }
  if (result.count > 0) result.center /= result.count;
  return result;
}
}

bool runMultiSceneSmokeTest(MainWindow &window) {
  auto *viewport = qobject_cast<NativeViewport *>(window.centralWidget());
  auto *document = window.findChild<WorkspaceDocument *>();
  QTemporaryDir temporary;
  if (!viewport || !document || !temporary.isValid()) return false;
  const auto check = [](bool success, const char *message) {
    if (!success) qCritical() << "MULTI_SCENE FAIL:" << message;
    return success;
  };
  // Two differently colored cubes, with different source-coordinate origins.
  QStringList files;
  for (int model = 0; model < 3; ++model) {
    QFile file(QDir(temporary.path()).filePath(QString("model-%1.ply").arg(model)));
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray ply("ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nelement face 6\nproperty list uchar int vertex_indices\nend_header\n");
    for (int vertex = 0; vertex < 8; ++vertex) {
      ply += QString("%1 %2 %3 %4\n")
          .arg((vertex & 1 ? 1 : -1) + (model == 1 ? 3 : -3))
          .arg(vertex & 2 ? 1 : -1).arg(vertex & 4 ? 1 : -1)
          .arg(model == 1 ? "20 20 240" : "240 20 20").toUtf8();
    }
    ply += "4 0 1 3 2\n4 4 6 7 5\n4 0 4 5 1\n4 2 3 7 6\n4 0 2 6 4\n4 1 5 7 3\n";
    file.write(ply);
    files.append(file.fileName());
  }
  QString error;
  const QString working = QDir(temporary.path()).filePath("working");
  if (!QDir().mkpath(working) || !document->createUntitled(working, "Multi-scene smoke", &error)) return false;
  if (!check(window.importSceneFile(files[0]), "first import")) return false;
  const QString first = document->activeSceneId();
  if (!check(waitUntil([&] { return viewport->meshRenderingAvailable(); }), "first mesh load")) return false;
  bool dialogSeen = false;
  const auto chooseImport = [&](int index, const QString &buttonName) {
    dialogSeen = false;
    QTimer clickTimer;
    clickTimer.setInterval(10);
    QObject::connect(&clickTimer, &QTimer::timeout, &window, [&] {
      auto *box = window.findChild<QMessageBox *>("sceneImportChoiceDialog");
      if (!box) return;
      auto *button = box->findChild<QAbstractButton *>(buttonName);
      if (!button) { box->reject(); return; }
      dialogSeen = true;
      button->click();
      clickTimer.stop();
    });
    clickTimer.start();
    QTimer::singleShot(2000, &clickTimer, [&] {
      if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->reject();
    });
    return window.importSceneFile(files[index]);
  };
  if (!check(chooseImport(1, "appendSceneButton") && dialogSeen, "append choice")) return false;
  const QString second = document->activeSceneId();
  if (!check(document->sceneObjects().size() == 2 && viewport->sceneObjectCount() == 2 && first != second,
             "two independent objects")) return false;
  if (!check(waitUntil([&] { return viewport->meshRenderingAvailable(); }), "second mesh load")) return false;
  viewport->setAxisView(NavigationAxis::PositiveZ);
  waitUntil([&] { return false; }, 400);
  // Fit both cubes using the ordinary wheel navigation, not a test camera.
  const QPointF center = viewport->rect().center();
  QWheelEvent zoom(center, viewport->mapToGlobal(center.toPoint()), {}, {0, -600},
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(viewport, &zoom);
  QImage frame;
  if (!check(waitUntil([&] {
        frame = viewport->grabFramebuffer();
        return footprint(frame, true).count > 500 && footprint(frame, false).count > 500;
      }), "both meshes visible in the same framebuffer")) return false;
  const QString capture = qEnvironmentVariable("GSW_MULTI_SCENE_CAPTURE");
  if (!capture.isEmpty()) frame.save(capture);
  // Click real red geometry rather than its bounding rectangle or the tree.
  const QPointF red = footprint(frame, true).center / viewport->devicePixelRatioF();
  QMouseEvent press(QEvent::MouseButtonPress, red, viewport->mapToGlobal(red.toPoint()),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, red, viewport->mapToGlobal(red.toPoint()),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &press);
  QApplication::sendEvent(viewport, &release);
  if (!check(document->activeSceneId() == first && viewport->activeSceneId() == first,
             "geometry click activates the old model")) return false;
  document->setSceneTransform({0, 0, 2}, QQuaternion::fromAxisAndAngle(0, 0, 1, 25), {1, 1, 1}, &error);
  if (!check(document->sceneObjects()[1].translation.isNull(), "independent transforms")) return false;
  const auto before = document->sceneCollectionJson();
  if (!check(!chooseImport(2, "cancelSceneImportButton") && dialogSeen &&
             document->sceneCollectionJson() == before, "cancel leaves both models untouched")) return false;
  if (!check(chooseImport(2, "replaceSceneButton") && dialogSeen &&
             document->sceneObjects().size() == 2 && document->sceneObjects()[1].path == files[1] &&
             document->activeSceneId() == first && document->scenePath() == files[2],
             "replace affects only the active model")) return false;
  if (!check(waitUntil([&] { return viewport->meshRenderingAvailable(); }), "replacement mesh load")) return false;
  const QString saved = QDir(temporary.path()).filePath("multi.gsw.json");
  if (!check(document->saveManifest(saved, &error), qPrintable("save collection: " + error))) return false;
  if (!check(document->load(saved, &error), "reopen collection")) return false;
  if (!check(document->sceneObjects().size() == 2 && viewport->sceneObjectCount() == 2,
             "both restored after reopen")) return false;
  // Repeated rapid replacement exercises cancellation of obsolete async loads.
  document->setScenePath(files[0], &error);
  document->setScenePath(files[2], &error);
  document->setScenePath(files[0], &error);
  if (!check(waitUntil([&] { return viewport->scenePath() == files[0] && viewport->meshRenderingAvailable(); }),
             "rapid replacement preserves the latest source")) return false;
  // Cleanup test project before deleting its temporary fixture directory.
  document->createUntitled(temporary.path(), "Empty", &error);
  if (!check(viewport->sceneObjectCount() == 0, "clearing project releases every layer")) return false;
  // Concurrent background loading of point/Gaussian layers must retain both
  // vertex buffers, including independent automatic shifts for large XYZ.
  QList<SceneObject> mixed;
  for (int kind = 0; kind < 2; ++kind) {
    QFile file(QDir(temporary.path()).filePath(QString("mixed-%1.ply").arg(kind)));
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray ply("ply\nformat ascii 1.0\nelement vertex 1681\nproperty double x\nproperty double y\nproperty double z\n");
    ply += kind == 0 ? "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        : "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
    ply += "end_header\n";
    for (int y = -20; y <= 20; ++y) {
      for (int x = -20; x <= 20; ++x) {
        ply += QString("%1 %2 1000000 %3\n")
            .arg(1'000'000.0 + (kind == 0 ? -3.0 : 3.0) + x * 0.05, 0, 'f', 3)
            .arg(1'000'000.0 + y * 0.05, 0, 'f', 3)
            .arg(kind == 0 ? "240 20 20" : "-1.5 -1.5 1.5 3 -3 -3 -3 1 0 0 0").toUtf8();
      }
    }
    file.write(ply);
    file.close();
    mixed.append({QString("mixed-%1").arg(kind), file.fileName(), {}, {}, {1, 1, 1}, 1681});
  }
  viewport->setSceneObjects(mixed, mixed[0].id);
  if (!check(waitUntil([&] { return viewport->selectableModelAvailable(); }), "mixed point load")) return false;
  viewport->setAxisView(NavigationAxis::PositiveZ);
  waitUntil([&] { return false; }, 400);
  QWheelEvent mixedZoom(center, viewport->mapToGlobal(center.toPoint()), {}, {0, -900},
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(viewport, &mixedZoom);
  viewport->clearSelection();
  const bool mixedVisible = waitUntil([&] {
        frame = viewport->grabFramebuffer();
        return footprint(frame, true).count > 100 && footprint(frame, false).count > 100;
      });
  if (!capture.isEmpty()) frame.save(capture + ".mixed.png");
  if (!mixedVisible) {
    qInfo() << "MIXED DEBUG" << footprint(frame, true).count << footprint(frame, false).count
            << viewport->viewTarget() << viewport->viewDistance() << frame.size();
    viewport->activateSceneObject(mixed[1].id);
    qInfo() << "GAUSSIAN DEBUG" << viewport->scenePath() << viewport->gaussianRenderingAvailable()
            << viewport->selectableModelAvailable() << viewport->sceneCoordinates().localCenter()
            << viewport->viewTarget();
  }
  if (!check(mixedVisible, "concurrent large-coordinate point and Gaussian layers visible")) return false;
  const QPointF beforeSwitch = footprint(frame, false).center;
  if (!check(viewport->activateSceneObject(mixed[1].id) && viewport->gaussianRenderingAvailable(),
             "background Gaussian remains available")) return false;
  viewport->clearSelection();
  frame = viewport->grabFramebuffer();
  if (!check((footprint(frame, false).center - beforeSwitch).manhattanLength() < 3,
             "changing automatic coordinate shift does not move the image")) return false;
  viewport->setSceneObjects({}, {});
  qInfo() << "MULTI_SCENE PASS: append, same-frame rendering, geometry picking, independent transforms, cancel, replace, save/reopen, async replacement, mixed point/Gaussian layers with large XYZ";
  return true;
}
}
