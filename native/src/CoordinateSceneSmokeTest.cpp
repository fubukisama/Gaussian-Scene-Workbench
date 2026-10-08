#include "CoordinateSceneSmokeTest.h"

#include "AppLanguage.h"
#include "ModelExportDialog.h"
#include "NativeViewport.h"
#include "WorkspaceDocument.h"

#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace gsw {
namespace {
void processFor(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

bool waitUntil(const std::function<bool()> &predicate) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < 15000) processFor(20);
  return predicate();
}

bool useTopOrthographicView(NativeViewport &viewport) {
  viewport.setAxisView(NavigationAxis::PositiveZ);
  processFor(300);
  if (!viewport.orthographicProjection()) {
    const QPointF center = navigationGizmoLayout(QMatrix4x4(), viewport.size(),
        QFontMetricsF(viewport.font()).height()).projectionCube.center();
    QMouseEvent press(QEvent::MouseButtonPress, center, center,
        viewport.mapToGlobal(center.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, center, center,
        viewport.mapToGlobal(center.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press);
    QApplication::sendEvent(&viewport, &release);
  }
  return viewport.orthographicProjection();
}

bool writeCube(const QString &path, bool second, double unitsPerMetre = 1.0,
               bool declareCoordinates = true,
               const QString &crs = QStringLiteral("gsw-coordinate-smoke-local"),
               double physicalScale = 1.0, bool nearOrigin = false) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray ply("ply\nformat ascii 1.0\n");
  if (declareCoordinates) {
    ply += unitsPerMetre == 100.0 ? "comment unit cm\n" : "comment unit m\n";
    ply += "comment crs: " + crs.toUtf8() + '\n';
  }
  ply += QByteArray("element vertex 8\nproperty double x\nproperty double y\n"
                 "property double z\nproperty uchar red\nproperty uchar green\n"
                 "property uchar blue\nelement face 6\n"
                 "property list uchar int vertex_indices\nend_header\n");
  // Both objects belong to one known coordinate frame. Their independent
  // loader shifts, different minima, and 6 m separation must not move the grid
  // when the user changes the active object. Large doubles exercise precision
  // shifting without requiring large or expensive fixtures.
  for (int vertex = 0; vertex < 8; ++vertex) {
    ply += QString("%1 %2 %3 %4\n")
        .arg(((nearOrigin ? 0.0 : 500000000.0) + ((second ? 3.0 : -3.0) +
              (vertex & 1 ? 1.0 : -1.0)) * physicalScale) * unitsPerMetre, 0, 'f', 3)
        .arg(((nearOrigin ? 0.0 : 400000000.0) + (vertex & 2 ? 1.0 : -1.0) * physicalScale) * unitsPerMetre, 0, 'f', 3)
        .arg(((second ? 23.0 : 20.0) + (vertex & 4 ? 1.0 : -1.0)) * physicalScale * unitsPerMetre, 0, 'f', 3)
        .arg(second ? "15 240 240" : "240 15 240").toUtf8();
  }
  ply += "4 0 1 3 2\n4 4 6 7 5\n4 0 4 5 1\n4 2 3 7 6\n4 0 2 6 4\n4 1 5 7 3\n";
  return file.write(ply) == ply.size();
}

bool writePatch(const QString &path, bool second, const QString &kind) {
  const bool gaussian = kind != "points";
  const bool surfel = kind == "2dgs";
  const double unitsPerMetre = second ? 100.0 : 1.0;
  QByteArray ply("ply\nformat ascii 1.0\n");
  ply += second ? "comment unit cm\n" : "comment unit m\n";
  ply += "comment crs: gsw-coordinate-smoke-local\nelement vertex 441\n"
         "property double x\nproperty double y\nproperty double z\n";
  if (gaussian) {
    ply += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
           "property float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (!surfel) ply += "property float scale_2\n";
    ply += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
  } else {
    ply += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  }
  ply += "end_header\n";
  for (int y = -10; y <= 10; ++y) {
    for (int x = -10; x <= 10; ++x) {
      ply += QString("%1 %2 %3 ")
          .arg((500000000.0 + (second ? 3.0 : -3.0) + x * 0.1) * unitsPerMetre, 0, 'f', 3)
          .arg((400000000.0 + y * 0.1) * unitsPerMetre, 0, 'f', 3)
          .arg((second ? 23.0 : 20.0) * unitsPerMetre, 0, 'f', 3).toUtf8();
      if (gaussian) {
        ply += second ? "-1.5 1.5 1.5 3 " : "1.5 -1.5 1.5 3 ";
        const QByteArray logScale = QByteArray::number(std::log(0.04 * unitsPerMetre), 'g', 12);
        ply += logScale + ' ' + logScale + ' ';
        if (!surfel) ply += logScale + ' ';
        ply += "1 0 0 0\n";
      } else {
        ply += second ? "15 240 240\n" : "240 15 240\n";
      }
    }
  }
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(ply) == ply.size();
}

QByteArray fileContents(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

struct Footprint {
  QPointF center;
  qsizetype pixels = 0;
  QRect bounds;
};

Footprint footprint(const QImage &image, bool cyan, bool fullWidth = false) {
  Footprint result;
  for (int y = 110; y < image.height() - 100; ++y) {
    for (int x = fullWidth ? 0 : 100; x < image.width() - (fullWidth ? 0 : 150); ++x) {
      const auto c = image.pixelColor(x, y);
      const int other = cyan ? c.green() : c.red();
      const int rejected = cyan ? c.red() : c.green();
      // The fixtures have equal blue/green (cyan) or blue/red (magenta).
      // A blue navigation control in the light theme is not model geometry.
      if (c.blue() > 60 && other > 60 && std::abs(c.blue() - other) < 20 &&
          c.blue() > rejected * 2 && other > rejected * 2) {
        result.center += QPointF(x, y);
        result.bounds = result.pixels == 0 ? QRect(x, y, 1, 1)
                                          : result.bounds.united(QRect(x, y, 1, 1));
        ++result.pixels;
      }
    }
  }
  if (result.pixels > 0) result.center /= result.pixels;
  return result;
}

double changedBodyFraction(const QImage &before, const QImage &after) {
  if (before.size() != after.size()) return 1.0;
  qsizetype changed = 0;
  qsizetype samples = 0;
  // Exclude dynamic FPS, active-object text, and the corner view controls.
  // The remaining pixels are actual geometry, grid and world-reference axes.
  for (int y = 110; y < before.height() - 100; ++y) {
    for (int x = 100; x < before.width() - 150; ++x) {
      const auto a = before.pixelColor(x, y);
      const auto b = after.pixelColor(x, y);
      if (std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                    std::abs(a.blue() - b.blue())}) > 12) ++changed;
      ++samples;
    }
  }
  return samples == 0 ? 1.0 : static_cast<double>(changed) / samples;
}

}

bool runCoordinateSceneSmokeTest(NativeViewport &viewport) {
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qCritical() << "COORDINATE_SCENE FAIL:" << message;
      passed = false;
    }
    return condition;
  };
  QList<SceneObject> objects;
  for (int i = 0; i < 2; ++i) {
    const QString path = QDir(temporary.path()).filePath(QString("same-frame-%1.ply").arg(i));
    if (!check(writeCube(path, i == 1), "fixture is written")) return false;
    objects.append({QString("same-frame-%1").arg(i), path, {}, {}, {1, 1, 1}, 8});
  }
  viewport.setSceneObjects(objects, objects[0].id);
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  viewport.setInteractionMode(NativeViewport::InteractionMode::Inspect);
  viewport.setEditToolsLocked(true);
  viewport.setReferencePlaneMode(NativeViewport::ReferencePlaneMode::ModelBase);
  for (const auto &object : objects) {
    if (!check(viewport.activateSceneObject(object.id), "object can be activated") ||
        !check(waitUntil([&] { return viewport.meshRenderingAvailable(); }), "mesh finishes loading")) return false;
    check(viewport.sceneCoordinates().automaticDisplayShift, "large source coordinates retain precision shifting");
    viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  }
  if (!check(viewport.activateSceneObject(objects[0].id), "first object is active")) return false;
  viewport.selectAllModels();
  if (!check(viewport.focusModel(), "both known-frame objects can be framed together")) return false;
  viewport.clearSelection();
  processFor(150);
  const auto requireVisible = [&] {
    return check(viewport.isVisible() && !viewport.window()->isMinimized(),
                 "real framebuffer test window stays visible");
  };
  if (!requireVisible()) return false;
  const QImage before = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const double elevation = viewport.referencePlaneElevation();
  const QString description = viewport.referencePlaneDescription();
  const auto magentaBefore = footprint(before, false);
  const auto cyanBefore = footprint(before, true);
  check(magentaBefore.pixels > 100 && cyanBefore.pixels > 100, "both meshes are visible before active switch");

  if (!check(viewport.activateSceneObject(objects[1].id), "second object is active")) return false;
  viewport.clearSelection();
  processFor(150);
  if (!requireVisible()) return false;
  const QImage after = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const auto magentaAfter = footprint(after, false);
  const auto cyanAfter = footprint(after, true);
  const double changedFraction = changedBodyFraction(before, after);
  qInfo() << "COORDINATE_SCENE active switch: reference elevation" << elevation << "->"
          << viewport.referencePlaneElevation() << "body changed fraction" << changedFraction
          << "magenta/cyan pixels" << magentaBefore.pixels << cyanBefore.pixels
          << magentaAfter.pixels << cyanAfter.pixels;
  check(magentaAfter.pixels > 100 && cyanAfter.pixels > 100 &&
        QLineF(magentaBefore.center, magentaAfter.center).length() < 1.0 &&
        QLineF(cyanBefore.center, cyanAfter.center).length() < 1.0,
        "active switch preserves both meshes' screen positions and relative separation");
  check(std::abs(viewport.referencePlaneElevation() - elevation) < 1.0e-9 &&
        viewport.referencePlaneDescription() == description,
        "active switch does not move or relabel the shared reference plane");
  check(changedFraction < 0.002,
        "active switch keeps the framebuffer grid and world-reference axes stationary");
  check(viewport.modelTranslation().isNull() && viewport.modelRotation().isIdentity() &&
        viewport.modelScale() == QVector3D(1, 1, 1) && !viewport.hasUnsavedSceneEdits(),
        "inspection leaves the second model unchanged");
  check(viewport.activateSceneObject(objects[0].id) && viewport.modelTranslation().isNull() &&
        viewport.modelRotation().isIdentity() && viewport.modelScale() == QVector3D(1, 1, 1) &&
        !viewport.hasUnsavedSceneEdits(), "inspection leaves the first model unchanged");
  const QString evidence = qEnvironmentVariable("GSW_COORDINATE_SCENE_SCREENSHOT_DIR");
  if (!evidence.isEmpty()) {
    check(QDir().mkpath(evidence), "evidence directory is available");
    check(before.save(QDir(evidence).filePath(AppLanguage::current() + "-before-switch.png")) &&
          after.save(QDir(evidence).filePath(AppLanguage::current() + "-after-switch.png")),
          "independent framebuffer evidence is saved");
  }
  viewport.setSceneObjects({}, {});
  objects.clear();
  for (int i = 0; i < 2; ++i) {
    const QString path = QDir(temporary.path()).filePath(QString("mixed-unit-%1.ply").arg(i));
    if (!check(writeCube(path, i == 1, i == 1 ? 100.0 : 1.0), "mixed-unit fixture is written")) return false;
    objects.append({QString("mixed-unit-%1").arg(i), path, {}, {}, {1, 1, 1}, 8});
  }
  viewport.setSceneObjects(objects, objects[0].id);
  for (const auto &object : objects) {
    if (!check(viewport.activateSceneObject(object.id), "mixed-unit object can be activated") ||
        !check(waitUntil([&] { return viewport.meshRenderingAvailable(); }), "mixed-unit mesh finishes loading")) return false;
    viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  }
  check(viewport.sceneCoordinates().unit == SceneLengthUnit::Centimetres,
        "second source explicitly declares centimetres");
  if (!check(viewport.activateSceneObject(objects[0].id), "metre source is the active object")) return false;
  viewport.selectAllModels();
  if (!check(viewport.focusModel(), "mixed-unit objects can be framed together")) return false;
  if (!check(useTopOrthographicView(viewport), "physical-size oracle uses an actual orthographic view")) return false;
  viewport.clearSelection();
  if (!requireVisible()) return false;
  const QImage mixedBefore = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const auto metre = footprint(mixedBefore, false);
  const auto centimetre = footprint(mixedBefore, true);
  const double metreWidth = metre.bounds.width();
  const double centimetreWidth = centimetre.bounds.width();
  const double widthRatio = metreWidth > 0 ? centimetreWidth / metreWidth : -1.0;
  const double separationInCubeWidths = metreWidth > 0
      ? QLineF(metre.center, centimetre.center).length() / metreWidth : -1.0;
  qInfo() << "COORDINATE_SCENE mixed-unit frame: pixels" << metre.pixels << centimetre.pixels
          << "width ratio" << widthRatio << "separation/cube edge" << separationInCubeWidths;
  // Worked fixture expectation, independent of rendering or unit-conversion
  // implementation: 200 cm == 2 m and 6 m centre separation == 3 cube edges.
  check(metre.pixels > 100 && centimetre.pixels > 100 &&
        std::abs(widthRatio - 1.0) < 0.03 && std::abs(separationInCubeWidths - 3.0) < 0.08,
        "metre and centimetre sources retain equal physical size and 6 m separation");
  const QString mixedDescription = viewport.referencePlaneDescription();
  const double mixedElevation = viewport.referencePlaneElevation();
  if (!check(viewport.activateSceneObject(objects[1].id), "centimetre source becomes active")) return false;
  viewport.clearSelection();
  processFor(150);
  if (!requireVisible()) return false;
  const QImage mixedAfter = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const auto metreAfter = footprint(mixedAfter, false);
  const auto centimetreAfter = footprint(mixedAfter, true);
  check(metreAfter.pixels > 100 && centimetreAfter.pixels > 100 &&
        QLineF(metre.center, metreAfter.center).length() < 1.0 &&
        QLineF(centimetre.center, centimetreAfter.center).length() < 1.0 &&
        changedBodyFraction(mixedBefore, mixedAfter) < 0.002 &&
        std::abs(viewport.referencePlaneElevation() - mixedElevation) < 1.0e-9 &&
        viewport.referencePlaneDescription() == mixedDescription,
        "mixed-unit active switch does not change physical geometry, grid, or displayed coordinate units");
  if (!evidence.isEmpty()) {
    check(mixedBefore.save(QDir(evidence).filePath(AppLanguage::current() + "-mixed-units-before.png")) &&
          mixedAfter.save(QDir(evidence).filePath(AppLanguage::current() + "-mixed-units-after.png")),
          "mixed-unit framebuffer evidence is saved");
  }
  auto *relation = viewport.window()->findChild<QLabel *>("sceneCoordinateRelationValue");
  check(relation && !relation->text().trimmed().isEmpty() && !relation->toolTip().trimmed().isEmpty(),
        "inspector exposes the scene coordinate relationship with an explanation");
  viewport.setEditToolsLocked(false);
  const auto key = [&](int code, const QString &text = {}) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier, text);
    QApplication::sendEvent(&viewport, &event);
  };
  for (int i = 0; i < objects.size(); ++i) {
    if (!check(viewport.activateSceneObject(objects[i].id), "numeric-move target can be activated")) return false;
    key(Qt::Key_G); key(Qt::Key_X); key(Qt::Key_1, "1"); key(Qt::Key_Return);
    const QVector3D sourceTranslation = viewport.modelExportOptions().transform.translation;
    const float expectedSourceDistance = i == 0 ? 1.0F : 100.0F;
    qInfo() << "COORDINATE_SCENE numeric G X 1:" << (i == 0 ? "metres" : "centimetres")
            << "source translation" << sourceTranslation;
    check((sourceTranslation - QVector3D(expectedSourceDistance, 0, 0)).length() < 1.0e-4F,
          "numeric movement uses one shared scene metre for either metre or centimetre source");
    viewport.undoEdit();
    check(viewport.modelExportOptions().transform.translation.isNull(),
          "numeric coordinate movement remains undoable in source units");
  }
  viewport.setEditToolsLocked(true);
  {
    ModelExportDialog dialog(viewport.modelExportOptions(), true, false, {}, viewport.window());
    auto *format = dialog.findChild<QComboBox *>("modelExportFormat");
    auto *coordinates = dialog.findChild<QComboBox *>("modelExportCoordinates");
    auto *descriptionLabel = dialog.findChild<QLabel *>("modelExportDescription");
    if (!check(format && coordinates && descriptionLabel, "real export dialog exposes format, coordinate choice and description")) return false;
    const QString initialLanguage = AppLanguage::current();
    dialog.show();
    for (const QString &language : AppLanguage::supported()) {
      check(AppLanguage::apply(language, false), "export dialog language changes immediately");
      for (const ModelExportFormat outputFormat : {ModelExportFormat::Xyz, ModelExportFormat::Stl}) {
        format->setCurrentIndex(format->findData(static_cast<int>(outputFormat)));
        for (const int coordinateChoice : {0, 1}) {
          coordinates->setCurrentIndex(coordinateChoice);
          const auto options = dialog.options();
          check(descriptionLabel->text().contains(QCoreApplication::translate("Workbench",
              "原始坐标保留源单位；场景坐标应用模型变换，并在源单位与场景参考单位均已声明时换算到场景参考单位。单位未知时保留数值，不猜测物理比例。GLB 随后按格式约定转换为米制。")),
              "export description distinguishes original units from shared scene units for XYZ and STL");
          check(options.applyTransform == (coordinateChoice == 1) && options.format == outputFormat &&
                options.coordinates.unit == SceneLengthUnit::Centimetres && options.sceneUnit == SceneLengthUnit::Metres &&
                std::abs(options.sceneUnitScale - 0.01) < 1.0e-12,
                "export format, coordinate choice and unit conversion survive live language changes");
        }
      }
    }
    dialog.close();
    check(AppLanguage::apply(initialLanguage, false), "export dialog restores the original language");
  }
  viewport.setSceneObjects({}, {});
  const QString unknownPath = QDir(temporary.path()).filePath("unknown-coordinate-system.ply");
  if (!check(writeCube(unknownPath, true, 1.0, false), "undeclared-coordinate fixture is written")) return false;
  const SceneObject known = objects[0];
  const SceneObject unknown{"unknown-coordinate-system", unknownPath, {}, {}, {1, 1, 1}, 8};
  viewport.setSceneObjects({known}, known.id);
  if (!check(waitUntil([&] { return viewport.meshRenderingAvailable(); }), "reference mesh finishes loading")) return false;
  viewport.resetCamera();
  if (!check(useTopOrthographicView(viewport), "background-loading oracle uses an orthographic view")) return false;
  const QPointF zoomCenter = viewport.rect().center();
  QWheelEvent zoomOut(zoomCenter, viewport.mapToGlobal(zoomCenter.toPoint()), {}, {0, -480},
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&viewport, &zoomOut);
  // Append to an already loaded active scene. Do not activate the background
  // object, select anything, or explicitly refresh the inspector: its visible
  // coordinate warning must update when the real background load completes.
  viewport.setSceneObjects({known, unknown}, known.id);
  QImage unknownFrame;
  if (!check(waitUntil([&] {
        if (!viewport.isVisible() || viewport.window()->isMinimized()) return false;
        unknownFrame = viewport.grabFramebuffer();
        return footprint(unknownFrame, false).pixels > 100 && footprint(unknownFrame, true).pixels > 100;
      }), "known and unknown-coordinate models are both visible after background loading")) return false;
  const auto unknownSummary = [] {
    return QCoreApplication::translate("Workbench", "坐标关系未确认");
  };
  const auto unknownDetail = [] {
    return QCoreApplication::translate("Workbench", "未声明单位或 CRS，不能确认模型已配准；保留未知单位的原始数值，不猜测比例或自动对齐。");
  };
  check(relation && relation->text() == unknownSummary() && relation->toolTip().contains(unknownDetail()),
        "background load publishes the unconfirmed-coordinate warning without changing selection");
  check(viewport.activeSceneId() == known.id,
        "background coordinate inspection does not change the active object");
  const QString originalLanguage = AppLanguage::current();
  for (const QString &language : AppLanguage::supported()) {
    check(AppLanguage::apply(language, false), "language can change while the scene stays loaded");
    processFor(20);
    check(relation && relation->text() == unknownSummary() && relation->toolTip().contains(unknownDetail()) &&
          viewport.activeSceneId() == known.id && viewport.sceneObjectCount() == 2,
          "coordinate warning and explanation translate immediately without rebuilding the scene");
  }
  check(AppLanguage::apply(originalLanguage, false), "original test language is restored");
  if (!check(viewport.activateSceneObject(unknown.id), "unknown-coordinate object can be inspected")) return false;
  check(!viewport.sceneCoordinates().unitDeclared && viewport.sceneCoordinates().coordinateReferenceSystem.isEmpty() &&
        viewport.modelTranslation().isNull() && viewport.modelRotation().isIdentity() &&
        viewport.modelScale() == QVector3D(1, 1, 1) && viewport.modelExportOptions().sceneUnitScale == 1.0,
        "unknown units retain source values and identity transform without invented metric conversion or alignment");
  if (!evidence.isEmpty()) check(unknownFrame.save(QDir(evidence).filePath(originalLanguage + "-unknown-background.png")),
                                "background-load framebuffer evidence is saved");
  const QString conflictPath = QDir(temporary.path()).filePath("different-declared-crs.ply");
  if (!check(writeCube(conflictPath, true, 1.0, true, "gsw-different-local-frame"),
             "different-CRS fixture is written")) return false;
  const SceneObject conflict{"different-declared-crs", conflictPath, {}, {}, {1, 1, 1}, 8};
  viewport.setSceneObjects({known, conflict}, known.id);
  if (!check(viewport.activateSceneObject(conflict.id) && waitUntil([&] { return viewport.meshRenderingAvailable(); }),
             "different-CRS object finishes loading")) return false;
  check(relation && relation->text() == QCoreApplication::translate("Workbench", "CRS 不同 · 需要配准或重投影") &&
        relation->toolTip().contains(QCoreApplication::translate("Workbench",
            "CRS 不一致；这里只统一可识别的长度单位，不执行坐标投影转换。请先在配准或测绘工具中建立明确变换。")),
        "inspector explicitly warns that different declared coordinate systems need registration or reprojection");
  viewport.setSceneObjects({}, {});

  // Save and reopen an independent project, including source-unit translations.
  // The view is reframed through the same public controls; no camera/private
  // state is injected, and the original fixture files must remain untouched.
  WorkspaceDocument document;
  const QString working = QDir(temporary.path()).filePath("coordinate-project-working");
  QString documentError;
  if (!check(QDir().mkpath(working) && document.createUntitled(working, "Coordinate smoke", &documentError),
             "independent coordinate project is created")) return false;
  QList<QByteArray> sourceContents;
  for (const auto &object : objects) {
    sourceContents.append(fileContents(object.path));
    if (!check(!sourceContents.last().isEmpty() && document.addScenePath(object.path, &documentError),
               "source object is added to the independent project")) return false;
  }
  auto persistedObjects = document.sceneObjects();
  persistedObjects[0].translation = {1, 0, 0};
  persistedObjects[1].translation = {100, 0, 0};
  if (!check(document.setSceneObjectTransforms(persistedObjects, &documentError) &&
             document.activateSceneObject(persistedObjects[0].id),
             "equivalent one-metre source-unit transforms are stored")) return false;
  const auto prepareMeshes = [&](const QList<SceneObject> &collection, const QString &active) {
    viewport.setSceneObjects(collection, active);
    for (const auto &object : collection) {
      if (!viewport.activateSceneObject(object.id) ||
          !waitUntil([&] { return viewport.meshRenderingAvailable(); })) return false;
      viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
    }
    if (!viewport.activateSceneObject(active)) return false;
    viewport.resetCamera();
    if (!useTopOrthographicView(viewport)) return false;
    viewport.selectAllModels();
    if (!viewport.focusModel()) return false;
    viewport.clearSelection();
    processFor(100);
    return true;
  };
  if (!check(prepareMeshes(document.sceneObjects(), document.activeSceneId()), "saved project models load together") ||
      !requireVisible()) return false;
  const QImage projectBefore = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const double projectElevation = viewport.referencePlaneElevation();
  const QString projectDescription = viewport.referencePlaneDescription();
  const QString projectFile = QDir(temporary.path()).filePath("coordinate-smoke.gsw.json");
  if (!check(document.saveManifest(projectFile, &documentError), "coordinate project saves successfully")) {
    qCritical() << documentError;
    return false;
  }
  WorkspaceDocument reopened;
  if (!check(reopened.load(projectFile, &documentError), "coordinate project reopens successfully")) {
    qCritical() << documentError;
    return false;
  }
  const auto reopenedObjects = reopened.sceneObjects();
  if (!check(reopenedObjects.size() == 2 && reopened.activeSceneId() == document.activeSceneId(),
             "save/reopen retains object order and active object identity")) return false;
  for (int i = 0; i < persistedObjects.size(); ++i) {
    check(reopenedObjects[i].id == persistedObjects[i].id &&
          QFileInfo(reopenedObjects[i].path).canonicalFilePath() == QFileInfo(persistedObjects[i].path).canonicalFilePath() &&
          reopenedObjects[i].translation == persistedObjects[i].translation &&
          reopenedObjects[i].rotation == persistedObjects[i].rotation && reopenedObjects[i].scale == persistedObjects[i].scale,
          "save/reopen preserves each source path and source-unit transform");
    check(fileContents(objects[i].path) == sourceContents[i], "save/reopen does not rewrite original source files");
  }
  viewport.setSceneObjects({}, {});
  if (!check(prepareMeshes(reopenedObjects, reopened.activeSceneId()), "reopened project models reload together") ||
      !requireVisible()) return false;
  const QImage projectAfter = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  check(footprint(projectBefore, false).pixels > 100 && footprint(projectBefore, true).pixels > 100 &&
        footprint(projectAfter, false).pixels > 100 && footprint(projectAfter, true).pixels > 100 &&
        changedBodyFraction(projectBefore, projectAfter) < 0.002 &&
        viewport.referencePlaneElevation() == projectElevation && viewport.referencePlaneDescription() == projectDescription,
        "save/reopen preserves physical placement, scale, reference origin and coordinate units in the real viewport");
  if (!evidence.isEmpty()) check(projectBefore.save(QDir(evidence).filePath(originalLanguage + "-project-before.png")) &&
                                projectAfter.save(QDir(evidence).filePath(originalLanguage + "-project-reopened.png")),
                                "project roundtrip framebuffer evidence is saved");
  viewport.setSceneObjects({}, {});

  for (const QString &kind : {QString("points"), QString("3dgs"), QString("2dgs")}) {
    QList<SceneObject> layers;
    for (int i = 0; i < 2; ++i) {
      const QString path = QDir(temporary.path()).filePath(QString("matrix-%1-%2.ply").arg(kind).arg(i));
      if (!check(writePatch(path, i == 1, kind), "coordinate capability fixture is written")) return false;
      layers.append({QString("matrix-%1-%2").arg(kind).arg(i), path, {}, {}, {1, 1, 1}, 441});
    }
    viewport.setSceneObjects(layers, layers[0].id);
    for (const auto &layer : layers) {
      if (!check(viewport.activateSceneObject(layer.id), "capability-matrix object can be activated") ||
          !check(waitUntil([&] { return viewport.selectableModelAvailable() && viewport.renderedPointCount() > 0; }),
                 "capability-matrix point data finishes loading")) return false;
      check(viewport.sourceHasGaussianAttributes() == (kind != "points") &&
            viewport.sourceHasSurfelAttributes() == (kind == "2dgs"),
            "fixture is recognized as its actual point, 3DGS or 2DGS representation");
      viewport.setRenderMode(kind == "points" ? NativeViewport::RenderMode::Points : NativeViewport::RenderMode::Gaussians);
      check(viewport.renderMode() == (kind == "points" ? NativeViewport::RenderMode::Points : NativeViewport::RenderMode::Gaussians),
            "coordinate matrix renders the actual representation rather than a fallback");
    }
    if (!check(viewport.activateSceneObject(layers[0].id), "matrix metre reference is active")) return false;
    viewport.resetCamera();
    if (!check(useTopOrthographicView(viewport), "matrix size oracle uses an orthographic view")) return false;
    viewport.selectAllModels();
    if (!check(viewport.focusModel(), "matrix objects can be framed together")) return false;
    viewport.clearSelection();
    const QPointF matrixZoomCenter = viewport.rect().center();
    QWheelEvent matrixZoomOut(matrixZoomCenter, viewport.mapToGlobal(matrixZoomCenter.toPoint()), {}, {0, -240},
                             Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&viewport, &matrixZoomOut);
    processFor(150);
    if (!requireVisible()) return false;
    const QImage matrixBefore = viewport.grabFramebuffer();
    if (!requireVisible()) return false;
    const auto firstPatch = footprint(matrixBefore, false);
    const auto secondPatch = footprint(matrixBefore, true);
    const auto fullFirstPatch = footprint(matrixBefore, false, true);
    const auto fullSecondPatch = footprint(matrixBefore, true, true);
    const QRect matrixRoi(100, 110, matrixBefore.width() - 250, matrixBefore.height() - 210);
    check(matrixRoi.adjusted(2, 2, -2, -2).contains(fullFirstPatch.bounds) &&
          matrixRoi.adjusted(2, 2, -2, -2).contains(fullSecondPatch.bounds),
          "entire colored matrix fixtures fit inside the measurement region without boundary clipping");
    const double matrixWidthRatio = firstPatch.bounds.width() > 0
        ? static_cast<double>(secondPatch.bounds.width()) / firstPatch.bounds.width() : -1.0;
    qInfo() << "COORDINATE_SCENE capability matrix:" << kind << "pixels" << firstPatch.pixels << secondPatch.pixels
            << "physical width ratio" << matrixWidthRatio << "orthographic" << viewport.orthographicProjection()
            << "full color bounds" << fullFirstPatch.bounds << fullSecondPatch.bounds;
    check(firstPatch.pixels > 100 && secondPatch.pixels > 100 && std::abs(matrixWidthRatio - 1.0) < 0.04,
          "point, 3DGS and 2DGS renderers honor equal physical size across metre and centimetre sources");
    const QString matrixDescription = viewport.referencePlaneDescription();
    if (!check(viewport.activateSceneObject(layers[1].id), "matrix centimetre source becomes active")) return false;
    viewport.clearSelection();
    processFor(150);
    if (!requireVisible()) return false;
    const QImage matrixAfter = viewport.grabFramebuffer();
    if (!requireVisible()) return false;
    const auto firstPatchAfter = footprint(matrixAfter, false);
    const auto secondPatchAfter = footprint(matrixAfter, true);
    qInfo() << "COORDINATE_SCENE matrix active switch:" << kind << "body changed fraction"
            << changedBodyFraction(matrixBefore, matrixAfter) << "marker shifts"
            << QLineF(firstPatch.center, firstPatchAfter.center).length()
            << QLineF(secondPatch.center, secondPatchAfter.center).length()
            << "before/after pixels" << firstPatch.pixels << firstPatchAfter.pixels
            << secondPatch.pixels << secondPatchAfter.pixels;
    check(firstPatchAfter.pixels > 100 && secondPatchAfter.pixels > 100 &&
          QLineF(firstPatch.center, firstPatchAfter.center).length() < 1.0 &&
          QLineF(secondPatch.center, secondPatchAfter.center).length() < 1.0 &&
          changedBodyFraction(matrixBefore, matrixAfter) < 0.002 && viewport.referencePlaneDescription() == matrixDescription,
          "point, 3DGS and 2DGS active switches preserve both rendered models and the shared reference grid");
    if (!evidence.isEmpty()) check(matrixBefore.save(QDir(evidence).filePath(originalLanguage + "-" + kind + "-before.png")) &&
                                  matrixAfter.save(QDir(evidence).filePath(originalLanguage + "-" + kind + "-after.png")),
                                  "capability-matrix framebuffer evidence is saved");
    viewport.setSceneObjects({}, {});
  }
  QList<SceneObject> tinyObjects;
  for (int i = 0; i < 2; ++i) {
    const QString path = QDir(temporary.path()).filePath(QString("tiny-mixed-unit-%1.ply").arg(i));
    if (!check(writeCube(path, i == 1, i == 1 ? 100.0 : 1.0, true,
                         "gsw-coordinate-smoke-local", 0.002, true),
               "millimetre-sized mixed-unit fixture is written")) return false;
    tinyObjects.append({QString("tiny-mixed-unit-%1").arg(i), path, {}, {}, {1, 1, 1}, 8});
  }
  viewport.setSceneObjects(tinyObjects, tinyObjects[0].id);
  for (const auto &object : tinyObjects) {
    if (!check(viewport.activateSceneObject(object.id) && waitUntil([&] { return viewport.meshRenderingAvailable(); }),
               "millimetre-sized mesh finishes loading")) return false;
    viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  }
  if (!check(viewport.activateSceneObject(tinyObjects[0].id), "tiny metre reference is active")) return false;
  // Reset and projection controls reproduce a close view of two 4 mm cubes.
  // They remain below the projection limit but have enough visible face pixels
  // to distinguish geometry from anti-aliasing and the reference-axis strokes.
  // No private projection/depth matrix or synthetic framebuffer is consulted.
  viewport.resetCamera();
  if (!check(useTopOrthographicView(viewport), "tiny-model oracle uses the actual orthographic control")) return false;
  viewport.clearSelection();
  processFor(100);
  if (!requireVisible()) return false;
  const QImage tinyBefore = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const auto tinyMetreBefore = footprint(tinyBefore, false);
  const auto tinyCentimetreBefore = footprint(tinyBefore, true);
  const float tinyDistanceBefore = viewport.viewDistance();
  if (!check(viewport.activateSceneObject(tinyObjects[1].id), "tiny centimetre object becomes active")) return false;
  viewport.clearSelection();
  processFor(100);
  if (!requireVisible()) return false;
  const QImage tinyAfter = viewport.grabFramebuffer();
  if (!requireVisible()) return false;
  const auto tinyMetreAfter = footprint(tinyAfter, false);
  const auto tinyCentimetreAfter = footprint(tinyAfter, true);
  qInfo() << "COORDINATE_SCENE tiny-model active switch: local camera distance" << tinyDistanceBefore << "->"
          << viewport.viewDistance() << "magenta widths" << tinyMetreBefore.bounds.width() << tinyMetreAfter.bounds.width()
          << "cyan widths" << tinyCentimetreBefore.bounds.width() << tinyCentimetreAfter.bounds.width()
          << "pixels" << tinyMetreBefore.pixels << tinyMetreAfter.pixels
          << tinyCentimetreBefore.pixels << tinyCentimetreAfter.pixels
          << "center shifts" << QLineF(tinyMetreBefore.center, tinyMetreAfter.center).length()
          << QLineF(tinyCentimetreBefore.center, tinyCentimetreAfter.center).length();
  check(tinyMetreBefore.pixels > 100 && tinyMetreAfter.pixels > 100 &&
        tinyCentimetreBefore.pixels > 100 && tinyCentimetreAfter.pixels > 100 &&
        std::abs(tinyMetreAfter.bounds.width() - tinyMetreBefore.bounds.width()) <= 1 &&
        std::abs(tinyCentimetreAfter.bounds.width() - tinyCentimetreBefore.bounds.width()) <= 1 &&
        QLineF(tinyMetreBefore.center, tinyMetreAfter.center).length() < 1.0 &&
        QLineF(tinyCentimetreBefore.center, tinyCentimetreAfter.center).length() < 1.0,
        "active unit switch preserves actual tiny-model pixel size and position at the close-view projection limit");
  if (!evidence.isEmpty()) check(tinyBefore.save(QDir(evidence).filePath(originalLanguage + "-tiny-before.png")) &&
                                tinyAfter.save(QDir(evidence).filePath(originalLanguage + "-tiny-after.png")),
                                "tiny-model projection framebuffer evidence is saved");
  viewport.setSceneObjects({}, {});
  if (passed) qInfo() << "COORDINATE_SCENE PASS: same-frame and mixed-unit geometry retain physical size, shared grid and coordinate relationship";
  return passed;
}
}
