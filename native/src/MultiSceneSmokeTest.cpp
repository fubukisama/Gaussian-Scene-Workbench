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
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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
  const auto key = [&](int code, const QString &text = {}, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(viewport, &event);
  };
  const QPointF blue = footprint(frame, false).center / viewport->devicePixelRatioF();
  const auto click = [&](const QPointF &point, Qt::KeyboardModifiers modifiers) {
    QMouseEvent down(QEvent::MouseButtonPress, point, viewport->mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, modifiers);
    QMouseEvent up(QEvent::MouseButtonRelease, point, viewport->mapToGlobal(point.toPoint()),
                    Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(viewport, &down);
    QApplication::sendEvent(viewport, &up);
  };
  click(blue, Qt::ControlModifier);
  if (!check(viewport->selectedModelCount() == 2, "Ctrl click adds another model")) return false;
  click(blue, Qt::ControlModifier);
  if (!check(viewport->selectedModelCount() == 1 && viewport->selectedSceneIds().contains(first),
             "Ctrl click removes just the clicked model")) return false;
  key(Qt::Key_A, "a", Qt::ControlModifier);
  if (!check(viewport->selectedModelCount() == 2, "Ctrl+A selects all models")) return false;
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  if (!check(tree != nullptr, "project tree available")) return false;
  const auto treeClick = [&](const QString &id, Qt::KeyboardModifiers modifiers) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
      if ((*it)->data(0, Qt::UserRole + 2).toString() != id) continue;
      tree->scrollToItem(*it);
      const QPointF point = tree->visualItemRect(*it).center();
      QMouseEvent down(QEvent::MouseButtonPress, point, tree->viewport()->mapToGlobal(point.toPoint()),
                        Qt::LeftButton, Qt::LeftButton, modifiers);
      QMouseEvent up(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::NoButton, modifiers);
      QApplication::sendEvent(tree->viewport(), &down);
      QApplication::sendEvent(tree->viewport(), &up);
      return;
    }
  };
  treeClick(first, Qt::NoModifier);
  treeClick(second, Qt::ShiftModifier);
  if (!check(viewport->selectedModelCount() == 2 && tree->selectedItems().size() == 2,
             "Shift tree range selection survives active-object changes")) return false;
  treeClick(second, Qt::ControlModifier);
  if (!check(viewport->selectedModelCount() == 1 && viewport->selectedSceneIds().contains(first),
             "Ctrl tree click removes only one model")) return false;
  treeClick(second, Qt::ControlModifier);
  if (!check(viewport->selectedModelCount() == 2, "Ctrl tree click adds one model")) return false;
  if (!capture.isEmpty()) viewport->grabFramebuffer().save(capture + ".selected.png");
  const auto close = [](QVector3D actual, QVector3D expected) { return (actual - expected).length() < 0.001F; };
  key(Qt::Key_G); key(Qt::Key_X); key(Qt::Key_2, "2"); key(Qt::Key_Return);
  if (!check(close(document->sceneObjects()[0].translation, {2,0,0}) &&
             close(document->sceneObjects()[1].translation, {2,0,0}), "G X moves both models")) return false;
  viewport->undoEdit();
  if (!check(close(document->sceneObjects()[0].translation, {}) &&
             close(document->sceneObjects()[1].translation, {}), "one undo restores entire group")) return false;
  viewport->redoEdit();
  if (!check(close(document->sceneObjects()[0].translation, {2,0,0}) &&
             close(document->sceneObjects()[1].translation, {2,0,0}), "one redo restores entire group")) return false;
  viewport->undoEdit();
  key(Qt::Key_R); key(Qt::Key_Z); key(Qt::Key_9, "9"); key(Qt::Key_0, "0"); key(Qt::Key_Return);
  if (!check(close(document->sceneObjects()[0].translation, {3,-3,0}) &&
             close(document->sceneObjects()[1].translation, {-3,3,0}) &&
             rotationsEquivalent(document->sceneObjects()[0].rotation, document->sceneObjects()[1].rotation),
             "R Z rotates around a shared pivot, preserving relative positions")) return false;
  viewport->undoEdit();
  key(Qt::Key_S); key(Qt::Key_2, "2"); key(Qt::Key_Return);
  if (!check(close(document->sceneObjects()[0].translation, {-3,0,0}) &&
             close(document->sceneObjects()[1].translation, {3,0,0}) &&
             close(document->sceneObjects()[0].scale, {2,2,2}) && close(document->sceneObjects()[1].scale, {2,2,2}),
             "S scales the group around its shared pivot")) return false;
  viewport->undoEdit();
  key(Qt::Key_G); key(Qt::Key_X); key(Qt::Key_8, "8"); key(Qt::Key_Escape);
  if (!check(close(viewport->modelTranslation(), {}) && close(document->sceneObjects()[0].translation, {}) &&
             close(document->sceneObjects()[1].translation, {}), "Esc cancels entire group without persisting")) return false;
  viewport->activateSceneObject(first);
  if (!check(close(viewport->modelTranslation(), {}), "cancel restored first runtime transform")) return false;
  viewport->activateSceneObject(second);
  if (!check(close(viewport->modelTranslation(), {}), "cancel restored second runtime transform")) return false;
  const auto unitTransforms = document->sceneObjects();
  auto smallTransforms = unitTransforms;
  smallTransforms[0].scale = {0.03F, 0.03F, 0.03F};
  smallTransforms[1].scale = {0.06F, 0.06F, 0.06F};
  if (!document->setSceneObjectTransforms(smallTransforms, &error) ||
      !viewport->setSceneSelection({first, second}, first)) return false;
  key(Qt::Key_S); key(Qt::Key_0, "0"); key(Qt::Key_Period, ".");
  key(Qt::Key_0, "0"); key(Qt::Key_0, "0"); key(Qt::Key_0, "0"); key(Qt::Key_1, "1"); key(Qt::Key_Return);
  if (!check(document->sceneObjects()[0].scale.x() >= 1.0e-4F &&
             document->sceneObjects()[0].scale.x() < 1.001e-4F &&
             std::abs(document->sceneObjects()[1].scale.x() / document->sceneObjects()[0].scale.x() - 2.0F) < 1.0e-5F,
             "minimum scale clamps the common factor without resetting members")) return false;
  auto largeTransforms = unitTransforms;
  largeTransforms[0].scale = {3000, 3000, 3000};
  largeTransforms[1].scale = {1500, 1500, 1500};
  if (!document->setSceneObjectTransforms(largeTransforms, &error)) return false;
  key(Qt::Key_S); key(Qt::Key_1, "1"); key(Qt::Key_0, "0"); key(Qt::Key_Return);
  if (!check(document->sceneObjects()[0].scale.x() <= 10000 && document->sceneObjects()[0].scale.x() > 9999 &&
             std::abs(document->sceneObjects()[0].scale.x() / document->sceneObjects()[1].scale.x() - 2.0F) < 1.0e-5F,
             "maximum scale preserves proportions for every selected member")) return false;
  if (!document->setSceneObjectTransforms(unitTransforms, &error)) return false;
  // Reset to the first object for the independent replacement checks below.
  viewport->activateSceneObject(first);
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
  if (!check(chooseImport(2, "appendSceneButton") && waitUntil([&] { return viewport->meshRenderingAvailable(); }),
             "third object imported for partial-selection regression")) return false;
  const QString third = document->activeSceneId();
  if (!check(viewport->setSceneSelection({first, second}, first), "select two out of three")) return false;
  key(Qt::Key_G); key(Qt::Key_X); key(Qt::Key_2, "2"); key(Qt::Key_Return);
  if (!check(close(document->sceneObjects()[0].translation, {2,0,0}) &&
             close(document->sceneObjects()[1].translation, {2,0,0}) &&
             close(document->sceneObjects()[2].translation, {}), "unselected third object stays unchanged")) return false;
  viewport->activateSceneObject(third);
  viewport->undoEdit();
  if (!check(close(document->sceneObjects()[0].translation, {}) &&
             close(document->sceneObjects()[1].translation, {}) && close(viewport->modelTranslation(), {}),
             "group undo works after changing the selection")) return false;
  viewport->redoEdit();
  if (!check(close(document->sceneObjects()[0].translation, {2,0,0}) &&
             close(document->sceneObjects()[1].translation, {2,0,0}) && close(viewport->modelTranslation(), {}),
             "group redo works after changing the selection")) return false;
  if (!check(document->saveManifest(saved, &error) && document->load(saved, &error) &&
             close(document->sceneObjects()[0].translation, {2,0,0}) &&
             close(document->sceneObjects()[1].translation, {2,0,0}) &&
             close(document->sceneObjects()[2].translation, {}), "group transforms survive save/reopen")) return false;
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
  qInfo() << "MULTI_SCENE PASS: append, same-frame rendering, Ctrl/Shift multiselection, shared-pivot group transforms, atomic cancel/undo/redo, unselected isolation, save/reopen, async replacement, mixed point/Gaussian layers with large XYZ";
  return true;
}
}
