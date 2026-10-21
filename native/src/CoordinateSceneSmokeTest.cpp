#include "CoordinateSceneSmokeTest.h"

#include "AppLanguage.h"
#include "ModelExportDialog.h"
#include "NativeViewport.h"
#include "WorkspaceDocument.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QFutureWatcher>
#include <QImage>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
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
               double physicalScale = 1.0, bool nearOrigin = false,
               double elevationOffset = 0.0) {
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
        .arg((((second ? 23.0 : 20.0) + (vertex & 4 ? 1.0 : -1.0)) * physicalScale + elevationOffset) * unitsPerMetre, 0, 'f', 9)
        .arg(second ? "15 240 240" : "240 15 240").toUtf8();
  }
  ply += "4 0 1 3 2\n4 4 6 7 5\n4 0 4 5 1\n4 2 3 7 6\n4 0 2 6 4\n4 1 5 7 3\n";
  return file.write(ply) == ply.size();
}

bool writePatch(const QString &path, bool second, const QString &kind,
                bool nearOrigin = false, double elevationOffset = 0.0,
                int resolution = 10) {
  const bool gaussian = kind != "points";
  const bool surfel = kind == "2dgs";
  const double unitsPerMetre = second ? 100.0 : 1.0;
  QByteArray ply("ply\nformat ascii 1.0\n");
  ply += second ? "comment unit cm\n" : "comment unit m\n";
  ply += "comment crs: gsw-coordinate-smoke-local\nelement vertex " +
         QByteArray::number((2 * resolution + 1) * (2 * resolution + 1)) +
         "\nproperty double x\nproperty double y\nproperty double z\n";
  if (gaussian) {
    ply += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
           "property float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (!surfel) ply += "property float scale_2\n";
    ply += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
  } else {
    ply += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  }
  ply += "end_header\n";
  for (int y = -resolution; y <= resolution; ++y) {
    for (int x = -resolution; x <= resolution; ++x) {
      ply += QString("%1 %2 %3 ")
          .arg(((nearOrigin ? 0.0 : 500000000.0) + (second ? 3.0 : -3.0) + x / static_cast<double>(resolution)) * unitsPerMetre, 0, 'f', 3)
          .arg(((nearOrigin ? 0.0 : 400000000.0) + y / static_cast<double>(resolution)) * unitsPerMetre, 0, 'f', 3)
          .arg(((second ? 23.0 : 20.0) + elevationOffset) * unitsPerMetre, 0, 'f', 9).toUtf8();
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

Footprint footprint(const QImage &image, bool cyan, bool fullWidth = false,
                    bool allowPastels = false) {
  Footprint result;
  for (int y = 110; y < image.height() - 100; ++y) {
    for (int x = fullWidth ? 0 : 100; x < image.width() - (fullWidth ? 0 : 150); ++x) {
      const auto c = image.pixelColor(x, y);
      const int other = cyan ? c.green() : c.red();
      const int rejected = cyan ? c.red() : c.green();
      // The fixtures have equal blue/green (cyan) or blue/red (magenta).
      // A blue navigation control in the light theme is not model geometry.
      const bool colorContrast = allowPastels
          ? c.blue() > rejected + 20 && other > rejected + 20
          : c.blue() > rejected * 2 && other > rejected * 2;
      // One-pixel GL points are multisampled over the already drawn grid.
      // A red/green axis in the background changes the surviving channels by
      // different amounts: equal source R/B (or G/B) need not stay equal in
      // the captured framebuffer. Require both marker channels to contrast
      // against the rejected channel; single colored axes/gray cannot pass.
      const bool sourceChannelBalance = allowPastels || std::abs(c.blue() - other) < 20;
      if (c.blue() > 60 && other > 60 && sourceChannelBalance && colorContrast) {
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
  // Calibrate the pixel oracle independently of the renderer under test.
  // These literal colors include source-marker coverage over a red/green
  // background (unequal surviving channels), plus subpixel edge coverage.
  // The extent/count gates below are unchanged; this prevents the oracle
  // from dropping an entire real point row merely because an axis is behind it.
  QImage coverageOracle(800, 500, QImage::Format_RGB32);
  coverageOracle.fill(Qt::white);
  for (const QPoint corner : {QPoint(200,200), QPoint(220,200), QPoint(200,220), QPoint(220,220)}) {
    coverageOracle.setPixelColor(corner, corner.y() == 200 ? QColor(221,21,194) : QColor(87,43,68));
    coverageOracle.setPixelColor(corner + QPoint(100,0), corner.y() == 200 ? QColor(21,221,194) : QColor(43,87,68));
  }
  coverageOracle.setPixelColor(450, 200, QColor(221,21,21));
  coverageOracle.setPixelColor(451, 200, QColor(21,221,21));
  coverageOracle.setPixelColor(452, 200, QColor(21,21,221));
  coverageOracle.setPixelColor(453, 200, QColor(130,130,130));
  const auto magentaCoverage = footprint(coverageOracle, false, false, true);
  const auto cyanCoverage = footprint(coverageOracle, true, false, true);
  check(magentaCoverage.pixels == 4 && magentaCoverage.bounds == QRect(200,200,21,21) &&
        cyanCoverage.pixels == 4 && cyanCoverage.bounds == QRect(300,200,21,21),
        "point coverage oracle includes grid-blended marker pixels but excludes axes and gray grid");
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
    const bool pointCoverage = kind == QStringLiteral("points");
    const auto firstPatch = footprint(matrixBefore, false, false, pointCoverage);
    const auto secondPatch = footprint(matrixBefore, true, false, pointCoverage);
    const auto fullFirstPatch = footprint(matrixBefore, false, true, pointCoverage);
    const auto fullSecondPatch = footprint(matrixBefore, true, true, pointCoverage);
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
    const auto firstPatchAfter = footprint(matrixAfter, false, false, pointCoverage);
    const auto secondPatchAfter = footprint(matrixAfter, true, false, pointCoverage);
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
  // Decimal, unshifted source elevations must not round-trip through the GPU's
  // float bounds before reaching coordinate reports. Cover every model family,
  // mixed source units, fixed source bases and active-object switches.
  constexpr double preciseElevation = 98.469025;
  for (const auto &kind : {QStringLiteral("points"), QStringLiteral("3dgs"),
                          QStringLiteral("2dgs"), QStringLiteral("mesh")}) {
    viewport.setSceneObjects({}, {});
    QList<SceneObject> preciseObjects;
    for (int i = 0; i < 2; ++i) {
      const QString path = QDir(temporary.path()).filePath(QString("precise-%1-%2.ply").arg(kind).arg(i));
      const bool written = kind == "mesh"
          ? writeCube(path, i == 1, i == 0 ? 1.0 : 100.0, true,
                      QStringLiteral("gsw-coordinate-smoke-local"), 1.0, true, preciseElevation - 19.0)
          : writePatch(path, i == 1, kind, true, preciseElevation - 20.0);
      if (!check(written, "decimal elevation fixture is written")) return false;
      preciseObjects.append({QString("precise-%1-%2").arg(kind).arg(i), path, {}, {}, {1, 1, 1}, kind == "mesh" ? 8 : 441});
    }
    viewport.setSceneObjects(preciseObjects, preciseObjects[0].id);
    for (const auto &object : preciseObjects) {
      if (!check(viewport.activateSceneObject(object.id), "decimal elevation object can be activated") ||
          !check(waitUntil([&] { return viewport.scenePath() == object.path && viewport.selectableModelAvailable(); }),
                 "decimal elevation object finishes loading")) return false;
    }
    check(std::abs(viewport.referencePlaneElevation() - preciseElevation) < 1.0e-10,
          "shared reference elevation retains source double precision in mixed units");
    preciseObjects[1].translation.setZ(-500.0F); // -5 scene metres in centimetre source units.
    viewport.setSceneObjects(preciseObjects, preciseObjects[1].id);
    check(std::abs(viewport.referencePlaneElevation() - preciseElevation) < 1.0e-10,
          "model translation leaves the original source base fixed with decimal precision");
    check(viewport.activateSceneObject(preciseObjects[0].id) &&
          std::abs(viewport.referencePlaneElevation() - preciseElevation) < 1.0e-10,
          "decimal collection elevation remains independent of active-object selection");
  }
  viewport.setSceneObjects({}, {});
  const QString zMovePath = QDir(temporary.path()).filePath("fixed-base-z-move.ply");
  if (!check(writeCube(zMovePath, false, 1.0, true,
                      QStringLiteral("gsw-coordinate-smoke-local"), 1.0, true,
                      preciseElevation - 19.0),
             "fixed-base Z movement fixture is written")) return false;
  const SceneObject zMoveObject{"fixed-base-z-move", zMovePath, {}, {}, {1, 1, 1}, 8};
  viewport.setSceneObjects({zMoveObject}, zMoveObject.id);
  if (!check(waitUntil([&] { return viewport.meshRenderingAvailable(); }),
             "fixed-base Z movement fixture finishes loading")) return false;
  viewport.setEditToolsLocked(false);
  viewport.selectModel();
  const double fixedBase = viewport.referencePlaneElevation();
  key(Qt::Key_G); key(Qt::Key_Z); key(Qt::Key_1, "1"); key(Qt::Key_Return);
  const auto movedZ = viewport.modelExportOptions().transform.translation;
  qInfo() << "COORDINATE_SCENE Z movement:" << "source translation" << movedZ
          << "base before/after" << fixedBase << viewport.referencePlaneElevation();
  check((movedZ - QVector3D(0, 0, 1)).length() < 1.0e-4F,
        "numeric G Z 1 commits one scene metre without changing X or Y");
  check(std::abs(viewport.referencePlaneElevation() - fixedBase) < 1.0e-10,
        "Z movement leaves the reference plane fixed so the model can leave the ground");
  viewport.undoEdit();
  check(viewport.modelExportOptions().transform.translation.isNull(),
        "Z movement remains undoable");
  const auto mouse = [&](QEvent::Type type, const QPointF &point,
                         Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, viewport.mapToGlobal(point.toPoint()),
                      button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &event);
  };
  for (const auto &kind : {QStringLiteral("points"), QStringLiteral("3dgs"),
                          QStringLiteral("2dgs"), QStringLiteral("mesh")}) {
    viewport.setSceneObjects({}, {});
    const QString path = QDir(temporary.path()).filePath("z-drag-" + kind + ".ply");
    const bool written = kind == "mesh"
        ? writeCube(path, false, 1.0, true, "gsw-coordinate-smoke-local", 1.0, true,
                    preciseElevation - 19.0)
        : writePatch(path, false, kind, true, preciseElevation - 20.0, kind == "points" ? 40 : 10);
    if (!check(written, "Z drag model-family fixture is written")) return false;
    const SceneObject object{"z-drag-" + kind, path, {}, {}, {1, 1, 1}, kind == "mesh" ? 8 : kind == "points" ? 6561 : 441};
    viewport.setSceneObjects({object}, object.id);
    if (!check(waitUntil([&] { return viewport.selectableModelAvailable(); }),
               "Z drag model-family fixture finishes loading")) return false;
    viewport.setRenderMode(kind == "mesh" ? NativeViewport::RenderMode::Mesh :
        kind == "points" ? NativeViewport::RenderMode::Points : NativeViewport::RenderMode::Gaussians);
    for (const bool orthographic : {false, true}) {
      viewport.setModelTransform({}, {});
      viewport.resetCamera();
      viewport.setModelGizmoMode(TransformGizmoMode::Move);
      viewport.selectModel();
      check(viewport.focusModel(), "Z drag fixture can be focused");
      if (orthographic) {
        const auto navigation = navigationGizmoLayout({}, viewport.size(), QFontMetricsF(viewport.font()).height());
        mouse(QEvent::MouseButtonPress, navigation.projectionCube.center(), Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, navigation.projectionCube.center(), Qt::LeftButton, Qt::NoButton);
      }
      // Leave room for the one-metre elevation change instead of clipping the
      // flat point/surfel fixture against the top of a tightly framed view.
      const QPointF zZoomCenter = viewport.rect().center();
      QWheelEvent zoomForMovement(zZoomCenter, viewport.mapToGlobal(zZoomCenter.toPoint()), {}, {0, -480},
          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&viewport, &zoomForMovement);
      processFor(80);
      const QImage zBefore = viewport.grabFramebuffer();
      // Subpixel points blend into the white theme as pastel magenta. Require
      // red and blue contrast against green (white/gray and axes cannot pass),
      // while retaining the same pixel-count and displacement acceptance.
      const auto markerBefore = footprint(zBefore, false, false, true);
      const auto targetBefore = viewport.viewTarget();
      const auto orbitBefore = viewport.viewOrbitAngles();
      const double baseBefore = viewport.referencePlaneElevation();
      key(Qt::Key_G); key(Qt::Key_Z); key(Qt::Key_1, "1"); key(Qt::Key_Return);
      const QImage zAfter = viewport.grabFramebuffer();
      const auto markerAfter = footprint(zAfter, false, false, true);
      const auto numericalTranslation = viewport.modelExportOptions().transform.translation;
      qInfo() << "COORDINATE_SCENE fixed-base Z:" << kind << "ortho" << orthographic
              << "translation" << numericalTranslation << "screen delta"
              << markerAfter.center - markerBefore.center << "base" << baseBefore
              << viewport.referencePlaneElevation() << "pixels" << markerBefore.pixels << markerAfter.pixels
              << "target" << targetBefore << viewport.viewTarget();
      check((numericalTranslation - QVector3D(0, 0, 1)).length() < 1.0e-4F &&
            std::abs(viewport.referencePlaneElevation() - baseBefore) < 1.0e-10,
            "each model family moves one metre above a stationary source plane");
      check(markerBefore.pixels > 100 && markerAfter.pixels > 100 &&
            markerAfter.center.y() < markerBefore.center.y() - 10.0 &&
            viewport.viewTarget() == targetBefore &&
            viewport.viewOrbitAngles().yawDegrees == orbitBefore.yawDegrees &&
            viewport.viewOrbitAngles().pitchDegrees == orbitBefore.pitchDegrees &&
            viewport.orthographicProjection() == orthographic,
            "actual model pixels move upward in a fixed camera for perspective and orthographic views");
      // The lower-left empty part of the actual framebuffer contains only the
      // ground grid; changing the model height must not translate those pixels.
      const QRect ground(120, zBefore.height() - 185, 65, 55);
      check(zBefore.copy(ground) == zAfter.copy(ground),
            "ground-grid framebuffer pixels stay fixed while model height changes");
      if (!evidence.isEmpty()) {
        const QString suffix = originalLanguage + "-z-" + kind + (orthographic ? "-ortho" : "-persp");
        check(zBefore.save(QDir(evidence).filePath(suffix + "-before.png")) &&
              zAfter.save(QDir(evidence).filePath(suffix + "-after.png")),
              "actual Z movement framebuffer evidence is saved");
      }
      viewport.undoEdit();
      check(viewport.modelExportOptions().transform.translation.isNull(), "numeric Z movement can be undone");
      viewport.redoEdit();
      check((viewport.modelExportOptions().transform.translation - QVector3D(0, 0, 1)).length() < 1.0e-4F,
            "numeric Z movement can be redone");
      viewport.undoEdit();

      // Find the real Z drag target through the same localized hover feedback
      // used by the UI, rather than duplicating the gizmo's private layout.
      const QPointF center(viewport.width() * 0.5, viewport.height() * 0.5);
      viewport.setEditToolsLocked(false);
      viewport.selectModel();
      QPointF zHandle;
      qreal longest = 0;
      const QString zTooltip = QCoreApplication::translate("Workbench", "沿 %1 轴移动").arg("Z");
      for (int dy = -220; dy < -20; dy += 3) {
        for (int dx = -6; dx <= 6; dx += 3) {
          const QPointF point = center + QPointF(dx, dy);
          mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
          if (viewport.toolTip().startsWith(zTooltip) && QLineF(center, point).length() > longest) {
            zHandle = point;
            longest = QLineF(center, point).length();
          }
        }
      }
      qInfo() << "COORDINATE_SCENE Z arrow target:" << kind << "ortho" << orthographic
              << "distance" << viewport.viewDistance() << "center" << center
              << "handle" << zHandle << "selected" << viewport.modelSelected()
              << "translation" << viewport.modelTranslation();
      if (!check(longest > 20, "visible Z arrow provides an actual axis drag target")) return false;
      const QPointF end = zHandle + QPointF(0, -24);
      mouse(QEvent::MouseButtonPress, zHandle, Qt::LeftButton, Qt::LeftButton);
      check(viewport.modelTransformActive(), "Z arrow begins a model transform");
      for (int step = 1; step <= 4; ++step)
        mouse(QEvent::MouseMove, zHandle + (end - zHandle) * (step / 4.0), Qt::NoButton, Qt::LeftButton);
      mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
      const auto dragged = viewport.modelExportOptions().transform.translation;
      check(dragged.z() > 0.05F && std::abs(dragged.x()) < 1.0e-4F && std::abs(dragged.y()) < 1.0e-4F &&
            !viewport.modelTransformActive() && std::abs(viewport.referencePlaneElevation() - baseBefore) < 1.0e-10,
            "dragging the green Z arrow moves only model Z and commits above the fixed plane");
      viewport.undoEdit();
      check(viewport.modelExportOptions().transform.translation.isNull(), "Z arrow drag remains undoable");
      viewport.redoEdit();
      check((viewport.modelExportOptions().transform.translation - dragged).length() < 1.0e-4F,
            "Z arrow drag remains redoable");
      viewport.undoEdit();
    }
  }
  // Save through the document interface and reload into an independent
  // document/viewport state, keeping the edited height separate from the base.
  WorkspaceDocument savedZ, reopenedZ;
  QString saveError;
  const QString zProjectRoot = QDir(temporary.path()).filePath("z-project");
  check(QDir().mkpath(zProjectRoot) && savedZ.create(zProjectRoot, &saveError) &&
        savedZ.addScenePath(zMovePath, &saveError) &&
        savedZ.setSceneTranslation(QVector3D(0, 0, 1), &saveError) &&
        savedZ.save(QDir(temporary.path()).filePath("z-project.gsw.json"), &saveError) &&
        reopenedZ.load(savedZ.projectFilePath(), &saveError),
        "Z movement can be saved and reopened through an independent project document");
  if (!saveError.isEmpty()) qCritical() << "COORDINATE_SCENE Z project:" << saveError;
  viewport.setSceneObjects({}, {});
  viewport.setSceneObjects(reopenedZ.sceneObjects(), reopenedZ.activeSceneId());
  check(waitUntil([&] { return viewport.meshRenderingAvailable(); }) &&
        (viewport.modelExportOptions().transform.translation - QVector3D(0, 0, 1)).length() < 1.0e-4F &&
        std::abs(viewport.referencePlaneElevation() - preciseElevation) < 1.0e-10,
        "save/reopen retains edited model height and the original fixed reference plane");
  // v3 display-only fixtures: two asymmetric point models, no optimizer state,
  // no reference measurement and no claim of GS/mesh export capability.
  const auto waitV3 = [](const std::function<bool()> &predicate) {
    QElapsedTimer deadline;deadline.start();
    while(!predicate()&&deadline.elapsed()<5000)processFor(10);
    return predicate();
  };
  const auto writeV3 = [](const QString &path,const QByteArray &bytes) {
    QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size();
  };
  const QJsonArray identityV3{QJsonArray{1.,0.,0.},QJsonArray{0.,1.,0.},QJsonArray{0.,0.,1.}};
  const QJsonArray identityPoseV3{QJsonArray{1.,0.,0.,0.},QJsonArray{0.,1.,0.,0.},QJsonArray{0.,0.,1.,0.},QJsonArray{0.,0.,0.,1.}};
  const QJsonArray calibrationV3{QJsonArray{520.,23.,271.25},QJsonArray{0.,470.,219.75},QJsonArray{0.,0.,1.}};
  const QList<SceneCoordinate3D> canonicalV3{{0.,0.,2.},{.8,.1,2.},{-.3,.7,2.3},{.2,-.4,2.8},{.7,.6,3.4}};
  QJsonArray canonicalPointsV3;for(const auto &point:canonicalV3)canonicalPointsV3.append(QJsonArray{point.x,point.y,point.z});
  const QString canonicalIdentityV3=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonObject{
      {"points",canonicalPointsV3},{"K",calibrationV3},{"pose",identityPoseV3},{"unit","m"}}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
  QList<SceneObject> layersV3;QStringList sidecarsV3;QList<QJsonObject> recordsV3;
  for(int model=0;model<2;++model) {
    const QString path=temporary.filePath(QStringLiteral("v3-frame-%1.ply").arg(model));
    const auto similarity=[&](double scale,const QJsonArray &rotation,const QJsonArray &translation) {
      return QJsonObject{{"s",scale},{"R",rotation},{"t",translation},{"applied",true}};
    };
    const auto normalization=model==0?similarity(2.,{QJsonArray{0.,-1.,0.},QJsonArray{1.,0.,0.},QJsonArray{0.,0.,1.}},{10.,-6.,3.}):
        similarity(4.,{QJsonArray{-1.,0.,0.},QJsonArray{0.,-1.,0.},QJsonArray{0.,0.,1.}},{-20.,8.,4.});
    const QJsonObject camera{{"view_id","owned-view"},{"image_path","pose-only-no-image.png"},{"split","train"},
        {"projection_model","PINHOLE"},{"pixel_coordinates","opencv_integer"},{"width",640},{"height",480},
        {"K",calibrationV3},{"c2w_opencv",identityPoseV3}};
    const double normalScale=model==0?2.:4.;
    QJsonObject contract{{"schema","gsw.training_camera_contract/1"},{"input_data_identity",canonicalIdentityV3},
        {"scale",QJsonObject{{"status","source_declared"},{"source_unit","m"},{"canonical_unit","m"},{"meters_per_unit",1.},
            {"evidence",QJsonArray{QJsonObject{{"kind","source_declaration"},{"source","generated-display-fixture"},{"unit","m"}}}}}},
        {"transforms",QJsonObject{{"source_to_canonical",similarity(1.,identityV3,{0.,0.,0.})},{"canonical_to_training",normalization}}},
        {"views",QJsonArray{camera}},{"clipping",QJsonObject{{"frame","training"},{"znear",.05*normalScale},
            {"backend_near",.1*normalScale},{"zfar",20.*normalScale}}}};
    QByteArray bytes="ply\nformat ascii 1.0\nelement vertex 5\nproperty double x\nproperty double y\nproperty double z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
    for(const auto &p:canonicalV3) {
      const SceneCoordinate3D value=model==0?SceneCoordinate3D{10.-2.*p.y,-6.+2.*p.x,3.+2.*p.z}:SceneCoordinate3D{1000.*p.x,1000.*p.y,1000.*p.z};
      bytes+=QStringLiteral("%1 %2 %3 %4\n").arg(value.x,0,'g',17).arg(value.y,0,'g',17).arg(value.z,0,'g',17)
          .arg(model==0?QStringLiteral("255 0 255"):QStringLiteral("0 255 255")).toUtf8();
    }
    QJsonObject record=contract;
    if(model==1) {
      auto applied=similarity(250.,{QJsonArray{-1.,0.,0.},QJsonArray{0.,-1.,0.},QJsonArray{0.,0.,1.}},{-5000.,2000.,-1000.});
      applied.insert(QStringLiteral("input_frame"),QStringLiteral("training"));applied.insert(QStringLiteral("output_frame"),QStringLiteral("canonical"));
      record={{"schema","gsw.coordinate_export/1"},{"export_version",2},{"output_frame","canonical"},{"output_unit","mm"},
          {"current_frame","canonical"},{"current_unit","mm"},{"transform_applied",true},{"applied_transform",applied},
          {"input_data_identity",contract.value("input_data_identity")},{"training_contract",contract}};
    }
    record.insert(QStringLiteral("ply_sha256"),QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()));
    const QString sidecar=temporary.filePath(QStringLiteral("v3-frame-%1.camera-scale.json").arg(model));
    if(!check(writeV3(path,bytes)&&writeV3(sidecar,QJsonDocument(record).toJson()),"write hash-bound asymmetric same-canonical display fixtures"))return false;
    layersV3.append({QStringLiteral("v3-frame-%1").arg(model),path,{},{},{1,1,1},5});
    sidecarsV3.append(sidecar);recordsV3.append(record);
  }
  viewport.setSceneObjects({},{});viewport.setSceneObjects(layersV3,layersV3[0].id);
  for(const auto &layer:layersV3) {
    if(!check(viewport.activateSceneObject(layer.id)&&waitV3([&]{return viewport.renderedPointCount()==5&&viewport.cameraCount()==1&&
        viewport.sceneCoordinates().provenance.value(QStringLiteral("native_hash_verified")).toBool();}),
        "both real point models and bound cameras load within a five-second case deadline"))return false;
    viewport.setRenderMode(NativeViewport::RenderMode::Points);
  }
  viewport.activateSceneObject(layersV3[0].id);viewport.setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);
  viewport.setShowCameras(false);viewport.setEditToolsLocked(true);viewport.selectAllModels();
  check(viewport.focusModel(),"frame both asymmetric coordinate fixtures through the real selection operation");viewport.clearSelection();processFor(50);
  const auto projectionPointsV3=[&]() {
    QList<QPointF> pixels;const auto matrix=viewport.projectionMatrix()*viewport.viewMatrix()*viewport.modelMatrix();
    for(const auto &point:canonicalV3) {
      const bool training=viewport.activeSceneId()==layersV3[0].id;
      const auto raw=training?SceneCoordinate3D{10.-2.*point.y,-6.+2.*point.x,3.+2.*point.z}:
          SceneCoordinate3D{1000.*point.x,1000.*point.y,1000.*point.z};
      const auto clip=matrix*QVector4D(viewport.sceneCoordinates().localFromGlobal(raw),1.);
      pixels.append(QPointF((clip.x()/clip.w()+1.)*.5*viewport.width(),(1.-clip.y()/clip.w())*.5*viewport.height()));
    }
    return pixels;
  };
  const auto maxPixelDifferenceV3=[](const QList<QPointF> &a,const QList<QPointF> &b) {
    double maximum=0.;if(a.size()!=b.size())return 1e9;
    for(int i=0;i<a.size();++i)maximum=std::max(maximum,QLineF(a[i],b[i]).length());return maximum;
  };
  const auto referenceV3=sceneCoordinateInfoToJson(viewport.sceneReferenceCoordinates());
  const auto navigationPixelsV3=projectionPointsV3();
  viewport.activateSceneObject(layersV3[1].id);
  check(maxPixelDifferenceV3(navigationPixelsV3,projectionPointsV3())<.02&&
      sceneCoordinateInfoToJson(viewport.sceneReferenceCoordinates())==referenceV3,
      "selection changes preserve actual rendered projection and the collection reference across R/t and millimetres");
  QList<QPointF> calibratedPixelsV3;
  for(int model=0;model<2;++model) {
    viewport.activateSceneObject(layersV3[model].id);
    if(!check(viewport.setSourceCameraView(0),"pose-only model metadata opens the real calibrated source-camera view"))return false;
    const auto points=projectionPointsV3();
    for(int i=0;i<canonicalV3.size();++i) {
      const auto &p=canonicalV3[i];const double u=(520.*p.x+23.*p.y)/p.z+271.25+.5,v=470.*p.y/p.z+219.75+.5;
      const QPointF expected(viewport.width()*.5+(u-320.)*viewport.height()/480.,v*viewport.height()/480.);
      check(QLineF(points[i],expected).length()<.03,"actual viewport projection matches independent full-K principal-point/skew/pixel-center equations");
    }
    const auto &pose=viewport.mScene->mCameraTrajectory.cameras().first();
    check((viewport.cameraPosition()-viewport.modelMatrix().map(viewport.sceneCoordinates().localFromGlobal(pose.globalPosition))).length()<.01F,
        "calibrated camera position uses the same model-frame transform exactly once without reading an image");
    const auto projection=viewport.projectionMatrix();
    const double near=projection(2,3)/(projection(2,2)-1.);
    const double far=projection(2,3)/(projection(2,2)+1.);
    const auto metres=sceneMetresPerCoordinate(viewport.sceneCoordinates());
    check(metres&&std::abs(near**metres-.1)<1e-5&&std::abs(far**metres-20.)<.002,
        "source-camera near/backend-near/far clipping retain the same physical lengths in training and mm frames");
    const auto rawA=viewport.sceneCoordinates().localFromGlobal(model==0?SceneCoordinate3D{10.,-6.,7.}:SceneCoordinate3D{0.,0.,2000.});
    const auto rawB=viewport.sceneCoordinates().localFromGlobal(model==0?SceneCoordinate3D{8.8,-4.6,9.8}:SceneCoordinate3D{700.,600.,3400.});
    check(metres&&std::abs((rawB-rawA).length()**metres-std::sqrt(2.81))<1e-5,
        "asymmetric models retain equal physical dimensions rather than shrinking metric output visually");
    if(model==0) {
      calibratedPixelsV3=points;viewport.activateSceneObject(layersV3[1].id);
      check(viewport.mCameraViewActive&&maxPixelDifferenceV3(calibratedPixelsV3,projectionPointsV3())<.03,
          "switching a verified same-source layer inside full-K camera view preserves principal point, skew, pose and projection");
    } else check(maxPixelDifferenceV3(calibratedPixelsV3,points)<.03,"independently opening both bound camera records yields the same image projection");
  }
  const auto calibratedFrameV3=viewport.grabFramebuffer();
  check(!calibratedFrameV3.isNull(),"the calibrated coordinate case renders a real nonempty framebuffer");
  const auto rasterPointsV3=projectionPointsV3();
  const double rasterScaleV3=calibratedFrameV3.width()/double(std::max(1,viewport.width()));
  for(const auto &point:rasterPointsV3) {
    bool marker=false;const QPoint center(qRound(rasterScaleV3*point.x()),qRound(rasterScaleV3*point.y()));
    for(int y=std::max(0,center.y()-6);y<std::min(calibratedFrameV3.height(),center.y()+7);++y)
      for(int x=std::max(0,center.x()-6);x<std::min(calibratedFrameV3.width(),center.x()+7);++x) {
        const auto color=calibratedFrameV3.pixelColor(x,y);
        marker|=color.blue()>100&&((color.red()-color.green()>45&&color.blue()-color.green()>45)||
            (color.green()-color.red()>45&&color.blue()-color.red()>45));
      }
    check(marker,"actual point-renderer framebuffer contains a colored model sample at the calibrated projected location");
  }
  viewport.setModelTransform({400,-200,100},QQuaternion::fromAxisAndAngle(0,0,1,15),{1.05F,1.05F,1.05F});
  viewport.setEditToolsLocked(false);viewport.setSceneSelection({layersV3[1].id},layersV3[1].id);viewport.selectModel();
  viewport.clearSelection();viewport.invertSelection();viewport.deleteSelection();
  viewport.setSceneSelection({layersV3[1].id},layersV3[1].id);viewport.selectModel();
  check(viewport.hasUnsavedSceneEdits()&&viewport.mScene->mEditModel.deletedCount()==canonicalV3.size()&&
      viewport.selectedSceneIds()==QStringList{layersV3[1].id},"prepare real nonempty crop history and model selection before metadata reload");
  const auto beforeTranslationV3=viewport.modelTranslation(),beforeScaleV3=viewport.modelScale();
  const auto beforeRotationV3=viewport.modelRotation();const auto beforeSelectionV3=viewport.selectedSceneIds();
  const auto beforeDeletedV3=viewport.mScene->mEditModel.deletedBits();
  const auto beforeCameraV3=viewport.mScene->mCameraTrajectory.cameras().first().cameraToWorld;
  int geometryReloadsV3=0;QObject refreshObserver;
  QObject::connect(&viewport,&NativeViewport::sceneLoadStarted,&refreshObserver,[&](const QString &){++geometryReloadsV3;});
  auto revised=recordsV3[1];auto revisedRoot=revised.value(QStringLiteral("training_contract")).toObject();
  revisedRoot.insert(QStringLiteral("warnings"),QJsonArray{QStringLiteral("owned-metadata-refresh")});revised.insert(QStringLiteral("training_contract"),revisedRoot);
  if(!check(writeV3(sidecarsV3[1],QJsonDocument(revised).toJson()),"update only the owned sidecar metadata without modifying model or camera inputs"))return false;
  viewport.refreshActiveSceneMetadata();
  check(waitV3([&]{return viewport.sceneCoordinates().provenance.value(QStringLiteral("training_contract")).toObject().value(QStringLiteral("warnings")).toArray().contains(QStringLiteral("owned-metadata-refresh"));})&&
      geometryReloadsV3==0&&viewport.modelTranslation()==beforeTranslationV3&&viewport.modelRotation()==beforeRotationV3&&
      viewport.modelScale()==beforeScaleV3&&viewport.selectedSceneIds()==beforeSelectionV3&&viewport.hasUnsavedSceneEdits()&&
      viewport.mScene->mEditModel.deletedBits()==beforeDeletedV3&&viewport.mScene->mCameraTrajectory.cameras().first().cameraToWorld==beforeCameraV3,
      "metadata reload preserves TRS, actual crop/deleted selection, model selection and recorded cameras without geometry reload or camera solving");
  // Deterministically deliver an actual old metadata future after a real
  // same-path replacement has loaded. Only the Qt notification is held; the
  // model/sidecar readers and public replacement operations are not mocked.
  const auto oldWatchersV3=viewport.findChildren<QFutureWatcherBase*>();
  viewport.refreshActiveSceneMetadata();
  QPointer<QFutureWatcherBase> heldWatcherV3;
  for(auto *watcher:viewport.findChildren<QFutureWatcherBase*>())if(!oldWatchersV3.contains(watcher))heldWatcherV3=watcher;
  if(!check(!heldWatcherV3.isNull(),"observe the newly created metadata notification for a controlled late-result race"))return false;
  QSignalBlocker holdFinishedV3(heldWatcherV3);
  QElapsedTimer metadataDeadlineV3;metadataDeadlineV3.start();
  while(!heldWatcherV3->isFinished()&&metadataDeadlineV3.elapsed()<5000)QThread::msleep(1);
  if(!check(heldWatcherV3->isFinished(),"the original metadata and camera readers actually finish inside the bounded race fixture"))return false;
  processFor(10); // consume its finished event while notification delivery is held.
  auto replacementBytesV3=fileContents(layersV3[1].path);
  replacementBytesV3.replace("element vertex 5\n","element vertex 6\n");
  replacementBytesV3+="100 200 2600 0 255 255\n";
  auto replacementRecordV3=revised;auto replacementRootV3=revisedRoot;
  auto replacementViewsV3=replacementRootV3.value(QStringLiteral("views")).toArray();
  auto replacementViewV3=replacementViewsV3[0].toObject();auto replacementPoseV3=identityPoseV3;
  replacementPoseV3[0]=QJsonArray{1.,0.,0.,.4};replacementViewV3.insert(QStringLiteral("c2w_opencv"),replacementPoseV3);
  replacementViewsV3[0]=replacementViewV3;replacementRootV3.insert(QStringLiteral("views"),replacementViewsV3);
  const QString replacementIdV3=QString::fromLatin1(QCryptographicHash::hash(replacementBytesV3+
      QJsonDocument(replacementViewsV3).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
  replacementRootV3.insert(QStringLiteral("input_data_identity"),replacementIdV3);
  replacementRecordV3.insert(QStringLiteral("input_data_identity"),replacementIdV3);
  replacementRecordV3.insert(QStringLiteral("training_contract"),replacementRootV3);
  replacementRecordV3.insert(QStringLiteral("ply_sha256"),QString::fromLatin1(QCryptographicHash::hash(replacementBytesV3,QCryptographicHash::Sha256).toHex()));
  if(!check(writeV3(layersV3[1].path,replacementBytesV3)&&writeV3(sidecarsV3[1],QJsonDocument(replacementRecordV3).toJson()),
      "publish a genuinely changed same-path model and matching new camera/coordinate record"))return false;
  viewport.setScene(QString(),0);viewport.setScene(layersV3[1].path,6);
  if(!check(waitV3([&]{return viewport.renderedPointCount()==6&&viewport.sceneCoordinates().dataIdentity==replacementIdV3&&
      viewport.cameraCount()==1&&std::abs(viewport.mScene->mCameraTrajectory.cameras().first().globalPosition.x-400.)<1e-8;}),
      "the replacement model and its new identity/camera pose actually load before old notification delivery"))return false;
  holdFinishedV3.unblock();
  holdFinishedV3.dismiss(); // the production callback schedules its watcher for deletion.
  check(QMetaObject::invokeMethod(heldWatcherV3,"finished",Qt::DirectConnection),"release the real completed old metadata notification after replacement");
  processFor(20);
  check(viewport.renderedPointCount()==6&&viewport.sceneCoordinates().dataIdentity==replacementIdV3&&viewport.cameraCount()==1&&
      std::abs(viewport.mScene->mCameraTrajectory.cameras().first().globalPosition.x-400.)<1e-8,
      "a late metadata result cannot overwrite the replacement model's actual coordinate identity or camera pose");
  viewport.setSceneObjects({}, {});
  if (passed) qInfo() << "COORDINATE_SCENE PASS: same-frame and mixed-unit geometry retain physical size, shared grid and coordinate relationship";
  return passed;
}
}
