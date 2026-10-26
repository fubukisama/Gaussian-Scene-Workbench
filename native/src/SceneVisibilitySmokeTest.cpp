#include "SceneVisibilitySmokeTest.h"
#include "AppLanguage.h"
#include "AppTheme.h"
#include "MainWindow.h"
#include "ModelExportDialog.h"
#include "NativeViewport.h"
#include "WorkspaceDocument.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDebug>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMessageBox>
#include <QAbstractButton>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPixmap>
#include <QSet>
#include <QScreen>
#include <QScopeGuard>
#include <QRegion>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWheelEvent>

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>

namespace gsw {
namespace {
bool waitUntil(const std::function<bool()> &predicate, int timeout = 7000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return predicate();
}

struct Patch {
  int count = 0;
  QPointF center;
  QRect bounds;
};

std::array<Patch, 4> patches(const QImage &image, const QRect &region = {}) {
  std::array<Patch, 4> result;
  // Only the scene body: exclude badges, axes, toolbar and lower caption.
  QRect body(20, 110, image.width() - 40, image.height() - 230);
  if (!region.isNull()) body = body.intersected(region);
  for (int y = body.top(); y <= body.bottom(); ++y) {
    for (int x = body.left(); x <= body.right(); ++x) {
      const QColor color = image.pixelColor(x, y);
      const int r = color.red(), g = color.green(), b = color.blue();
      const std::array<bool, 4> matches{
          r > 75 && r > g * 3 && r > b * 3,
          g > 75 && g > r * 3 && g > b * 3,
          b > 75 && b > r * 3 && b > g * 3,
          r > 75 && b > 75 && r > g * 3 && b > g * 3};
      for (int kind = 0; kind < 4; ++kind) {
        if (matches[kind]) {
          ++result[kind].count;
          result[kind].center += QPointF(x, y);
          result[kind].bounds |= QRect(x, y, 1, 1);
        }
      }
    }
  }
  for (auto &patch : result) if (patch.count) patch.center /= patch.count;
  return result;
}

Patch coverage(const QImage &image, const QImage &background, const QRect &region) {
  Patch result;
  const QRect sample = region.intersected(image.rect()).intersected(background.rect());
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const auto actual = image.pixelColor(x, y);
      const auto empty = background.pixelColor(x, y);
      if (std::max({std::abs(actual.red() - empty.red()), std::abs(actual.green() - empty.green()),
                    std::abs(actual.blue() - empty.blue())}) > 45) {
        ++result.count;
        result.center += QPointF(x, y);
        result.bounds |= QRect(x, y, 1, 1);
      }
    }
  }
  if (result.count) result.center /= result.count;
  return result;
}

QByteArray sha256(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result();
}

QTreeWidgetItem *sceneItem(QTreeWidget *tree, const QString &id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, Qt::UserRole + 2).toString() == id) return *it;
  return nullptr;
}

bool fixture(const QString &path, int kind) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const bool gaussian = kind >= 2;
  const bool mesh = kind == 1;
  QByteArray ply = "ply\nformat ascii 1.0\nelement vertex ";
  ply += mesh ? "4\n" : "2401\n";
  ply += "property float x\nproperty float y\nproperty float z\n";
  if (gaussian) {
    ply += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (kind == 2) ply += "property float scale_2\n";
    ply += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
  } else {
    ply += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  }
  if (mesh) ply += "element face 2\nproperty list uchar int vertex_indices\n";
  ply += "end_header\n";
  const float center = -4.5F + kind * 3.0F;
  const auto vertex = [&](float x, float y) {
    QByteArray data = QString("%1 %2 0 ").arg(center + x, 0, 'f', 4).arg(y, 0, 'f', 4).toUtf8();
    if (kind == 0) data += "240 12 12\n";
    else if (kind == 1) data += "12 240 12\n";
    else if (kind == 2) data += "-1.5 -1.5 1.5 3 -3.3 -3.3 -3.3 1 0 0 0\n";
    else data += "1.5 -1.5 1.5 3 -3.3 -3.3 1 0 0 0\n";
    return data;
  };
  if (mesh) {
    for (const auto &point : std::array<QPointF, 4>{QPointF(-0.75, -0.75), QPointF(0.75, -0.75), QPointF(-0.75, 0.75), QPointF(0.75, 0.75)})
      ply += vertex(static_cast<float>(point.x()), static_cast<float>(point.y()));
    ply += "3 0 1 2\n3 2 1 3\n";
  } else {
    for (int y = -24; y <= 24; ++y)
      for (int x = -24; x <= 24; ++x) ply += vertex(x / 32.0F, y / 32.0F);
  }
  return file.write(ply) == ply.size();
}

// This synthetic input has the recorded model's size and vertex count, but
// never includes a user's PLY or photos in public CI artifacts.
bool largeSelectionFixture(const QString &path, bool surfel) {
  constexpr int count = 512202;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray header("ply\nformat binary_little_endian 1.0\nelement vertex 512202\n");
  for (const auto *name : {"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"})
    header += QByteArray("property float ") + name + '\n';
  for (int coefficient = 0; coefficient < 45; ++coefficient)
    header += "property float f_rest_" + QByteArray::number(coefficient) + '\n';
  for (const auto *name : {"opacity", "scale_0", "scale_1"})
    header += QByteArray("property float ") + name + '\n';
  if (!surfel) header += "property float scale_2\n";
  for (const auto *name : {"rot_0", "rot_1", "rot_2", "rot_3"})
    header += QByteArray("property float ") + name + '\n';
  header += "end_header\n";
  if (file.write(header) != header.size()) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  for (int i = 0; i < count; ++i) {
    const float x = -22.0F + 42.0F * (i % 714) / 713.0F;
    const float y = -16.0F + 30.0F * (i / 714) / 717.0F;
    const float z = -1.76F + 9.0F * std::sin(x * 0.13F) * std::cos(y * 0.11F);
    stream << x << y << z << 0.0F << 0.0F << 1.0F << 1.5F << -1.5F << 1.5F;
    // Degree three exercises the separate SH texture used by real trained
    // PLYs; small finite coefficients preserve the unique magenta signature.
    for (int coefficient = 0; coefficient < 45; ++coefficient)
      stream << (coefficient % 7 == 0 ? 0.015F : 0.0F);
    stream << -0.3F << -3.1F << -2.6F;
    if (!surfel) stream << -4.2F;
    stream << 1.0F << 0.0F << 0.0F << 0.0F;
  }
  return stream.status() == QDataStream::Ok && file.flush();
}

bool runLargeSelectionRegression(MainWindow &window) {
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  const QString root = QDir(QCoreApplication::applicationDirPath()).filePath("test-settings");
  if (!QDir().mkpath(root)) return false;
  QTemporaryDir temporary(QDir(root).filePath("selection-large-XXXXXX"));
  const auto check = [](bool ok, const char *message) {
    if (!ok) qCritical() << "GAUSSIAN_CLICK_VISIBILITY FAIL:" << message;
    return ok;
  };
  if (!check(viewport && document && tree && temporary.isValid(), "real window services")) return false;
  const bool originallyDisabled = qEnvironmentVariableIsSet("GSW_DISABLE_INDEXED_GAUSSIANS");
  const auto originalDisabled = qgetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
  const auto restoreEnvironment = qScopeGuard([&] {
    if (originallyDisabled) qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", originalDisabled);
    else qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
  });
  window.resize(1440, 900);
  auto *lock = window.findChild<QAction *>("lockEditToolsAction");
  if (!check(lock, "actual edit-lock action")) return false;
  lock->setChecked(false);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  const QString evidence = QDir(qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", temporary.path())).filePath(
      "gaussian-click-" + AppLanguage::current() + (AppTheme::currentTheme() == UiTheme::Light ? "-light" : "-dark"));
  if (!QDir().mkpath(evidence)) return false;
  const auto click = [&](const QPointF &point) {
    QMouseEvent down(QEvent::MouseButtonPress, point, viewport->mapToGlobal(point.toPoint()),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, viewport->mapToGlobal(point.toPoint()),
                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &down);
    QApplication::sendEvent(viewport, &up);
  };
  for (const bool surfel : {false, true}) {
    qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
    const QString label = surfel ? "2dgs" : "3dgs";
    const QString path = temporary.filePath(label + ".ply");
    const QString working = temporary.filePath("workspace-" + label);
    QString error;
    if (!check(largeSelectionFixture(path, surfel) &&
               QDir().mkpath(working) && document->createUntitled(working, "Click visibility", &error) &&
               document->addScenePath(path, &error), "import exact-size synthetic model")) return false;
    if (!check(waitUntil([&] {
          (void)viewport->grabFramebuffer();
          return viewport->gaussianRenderingAvailable() && viewport->renderedPointCount() == 512202;
        }, 30000), "all 512202 Gaussians loaded and rendered without decimation")) return false;
    viewport->setRenderMode(NativeViewport::RenderMode::Gaussians);
    viewport->setInteractionMode(NativeViewport::InteractionMode::Inspect);
    viewport->resetCamera();
    if (!check(viewport->focusModel() && !viewport->orthographicProjection() &&
               viewport->sourceHasSurfelAttributes() == surfel && viewport->sourceShDegree() == 3,
               "angled perspective and actual SH3/model attributes")) return false;
    viewport->clearSelection();
    const auto object = document->sceneObjects().first();
    const auto sourceHash = sha256(path);
    const auto target = viewport->viewTarget();
    const auto distance = viewport->viewDistance();
    const auto angles = viewport->viewOrbitAngles();
    const auto unchanged = [&] {
      const auto after = document->sceneObjects().first();
      return viewport->sceneObjectCount() == 1 && viewport->visibleSceneObjectCount() == 1 &&
          viewport->renderedPointCount() == 512202 && viewport->renderMode() == NativeViewport::RenderMode::Gaussians &&
          viewport->modelTranslation() == object.translation && viewport->modelRotation() == object.rotation &&
          viewport->modelScale() == object.scale && viewport->viewTarget() == target && viewport->viewDistance() == distance &&
          viewport->viewOrbitAngles() == angles && after.visible && after.translation == object.translation &&
          after.rotation == object.rotation && after.scale == object.scale && !viewport->hasUnsavedSceneEdits();
    };
    for (int compatibility = 0; compatibility < 2; ++compatibility) {
      if (compatibility) qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", "1");
      else qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
      viewport->setRenderMode(NativeViewport::RenderMode::Points);
      viewport->setRenderMode(NativeViewport::RenderMode::Gaussians);
      viewport->clearSelection();
      if (!check(document->setSceneObjectVisibility({object.id}, false), "hide only synthetic geometry for reference")) return false;
      const QImage empty = viewport->grabFramebuffer();
      if (!check(!empty.isNull() && document->setSceneObjectVisibility({object.id}, true), "restore synthetic geometry")) return false;
      const QImage before = viewport->grabFramebuffer();
      const QRect sceneBody(35, 120, before.width() - 70, before.height() - 230);
      const auto signature = patches(before, sceneBody)[3];
      const QRect body = signature.bounds.intersected(sceneBody);
      const Patch baseline = coverage(before, empty, body);
      if (!check(signature.count > 5000 && baseline.count > 5000 && viewport->indexedGaussianRendering() == !compatibility,
                 "large model visible on required resident or compatibility renderer")) return false;
      const QString prefix = label + (compatibility ? "-compatibility" : "-resident");
      if (!check(before.save(QDir(evidence).filePath(prefix + "-before.png")), "retain real baseline pixels")) return false;
      const QPointF pick = baseline.center / viewport->devicePixelRatioF();
      for (int repeat = 0; repeat < 2; ++repeat) {
        click(pick);
        QImage selected = viewport->grabFramebuffer();
        if (!check(selected.save(QDir(evidence).filePath(prefix + QString("-selected-%1.png").arg(repeat))), "retain selected-model pixels")) return false;
        auto *item = sceneItem(tree, object.id);
        if (!check(viewport->selectedSceneIds() == QStringList{object.id} && viewport->modelSelected() &&
                   item && item->isSelected() && item->checkState(1) == Qt::Checked && unchanged(),
                   "actual model click selects without hiding, moving, resizing or editing source")) return false;
        // Compare the outside strips of the model, away from the central
        // transform gizmo and the exact selection-hint rectangle. A bounding
        // box by itself cannot supply the thousands of foreground pixels.
        const QRect bounds = baseline.bounds.intersected(body);
        const int strip = std::max(1, bounds.height() / 5);
        QRegion sample(QRect(bounds.left(), bounds.top(), bounds.width(), strip));
        sample += QRect(bounds.left(), bounds.bottom() - strip + 1, bounds.width(), strip);
        const auto hint = viewport->selectionHintBounds();
        const qreal ratio = viewport->devicePixelRatioF();
        sample -= QRectF(hint.x() * ratio, hint.y() * ratio, hint.width() * ratio,
                         hint.height() * ratio).toAlignedRect().adjusted(-3, -3, 3, 3);
        Patch expected, actual;
        for (const auto &region : sample) {
          const auto a = coverage(before, empty, region);
          const auto b = coverage(selected, empty, region);
          expected.center += a.center * a.count; expected.count += a.count;
          actual.center += b.center * b.count; actual.count += b.count;
        }
        if (expected.count) expected.center /= expected.count;
        if (actual.count) actual.center /= actual.count;
        qInfo() << "GAUSSIAN_CLICK_VISIBILITY" << label << "compatibility" << compatibility << "repeat" << repeat
                << "foreground-before/selected" << expected.count << actual.count
                << "centroid-delta" << QLineF(expected.center, actual.center).length();
        if (!check(expected.count > 1000 && actual.count >= expected.count * 85 / 100 &&
                   actual.count <= expected.count * 115 / 100 && QLineF(expected.center, actual.center).length() < 2.0,
                   "click-selected Gaussian body stays visible outside the selection overlays")) return false;
        viewport->clearSelection();
        const QImage cleared = viewport->grabFramebuffer();
        const auto clearedPatch = coverage(cleared, empty, body);
        if (!check(clearedPatch.count >= baseline.count * 95 / 100 && clearedPatch.count <= baseline.count * 105 / 100 &&
                   QLineF(clearedPatch.center, baseline.center).length() < 1.0 && unchanged(),
                   "deselection restores unchanged full geometry without reimport")) return false;
        if (!check(cleared.save(QDir(evidence).filePath(prefix + QString("-cleared-%1.png").arg(repeat))), "retain deselection pixels")) return false;
      }
    }
    if (!check(!sourceHash.isEmpty() && sha256(path) == sourceHash, "model clicks never rewrite input data")) return false;
  }
  QString error;
  if (!document->createUntitled(temporary.path(), "Empty regression", &error)) return false;
  qInfo() << "GAUSSIAN_CLICK_VISIBILITY PASS: 512202-Gaussian 3DGS/2DGS real click, both renderer paths, unchanged pixels and source data";
  return true;
}

bool runInputSelectionDiagnostic(MainWindow &window, const QJsonArray &inputs) {
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  QTemporaryDir temporary;
  if (!viewport || !document || !tree || !temporary.isValid() || inputs.size() < 2) return false;
  if (qEnvironmentVariable("GSW_SMOKE_BACKGROUND") == QStringLiteral("1")) {
    for (const auto *screen : QGuiApplication::screens()) {
      if (window.frameGeometry().intersects(screen->geometry()) || window.isActiveWindow()) {
        window.hide();
        qCritical() << "SCENE_INPUT_DIAGNOSTIC FAIL: background window is not isolated from the desktop";
        return false;
      }
    }
  }
  const QString evidence = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
  if (evidence.isEmpty() || !QDir().mkpath(evidence)) return false;
  QString error;
  const QString workspace = temporary.filePath("workspace");
  if (!QDir().mkpath(workspace) || !document->createUntitled(workspace, "Selection diagnostic", &error)) return false;
  const QString stagedProject = qEnvironmentVariable("GSW_SCENE_SELECTION_PROJECT");
  if (!stagedProject.isEmpty() && !document->load(stagedProject, &error)) return false;
  QStringList ids, paths;
  QList<QByteArray> hashes;
  for (const auto &input : inputs) {
    const QString path = input.toString();
    const QFileInfo source(path);
    if (!source.isAbsolute() || !source.isFile() || source.isSymLink() ||
        source.suffix().compare(QStringLiteral("ply"), Qt::CaseInsensitive) != 0) return false;
    hashes.append(sha256(path));
    if (hashes.last().isEmpty()) return false;
    if (stagedProject.isEmpty()) {
      if (!document->addScenePath(path, &error)) return false;
      ids.append(document->activeSceneId());
    } else {
      const auto objects = document->sceneObjects();
      const auto found = std::find_if(objects.cbegin(), objects.cend(), [&](const auto &object) {
        return QFileInfo(object.path).canonicalFilePath() == source.canonicalFilePath();
      });
      if (found == objects.cend()) return false;
      ids.append(found->id);
    }
    paths.append(path);
    if (!waitUntil([&] {
          (void)viewport->grabFramebuffer();
          return viewport->sceneCoordinates().valid && viewport->selectableModelAvailable();
        }, 30000)) return false;
  }
  auto *lockTools = window.findChild<QAction *>("lockEditToolsAction");
  if (!lockTools) return false;
  lockTools->setChecked(false);
  if (!viewport->focusModel()) return false;
  viewport->clearSelection();
  const auto initialObjects = document->sceneObjects();
  const auto snapshot = [&](const QString &name) {
    const QImage image = viewport->grabFramebuffer();
    viewport->makeCurrent();
    auto *gl = viewport->context()->extraFunctions();
    GLboolean depthWrite = GL_TRUE;
    GLint depthFunction = 0;
    gl->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    gl->glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
    qInfo() << "SCENE_INPUT_DIAGNOSTIC" << name << "active" << viewport->activeSceneId()
            << "mode/gaussian/indexed/points" << viewport->renderMode() << viewport->sourceHasGaussianAttributes()
            << viewport->indexedGaussianRendering() << viewport->renderedPointCount()
            << "model-translation" << viewport->modelTranslation()
            << "view" << viewport->viewTarget() << viewport->viewDistance()
            << "orbit" << viewport->viewOrbitAngles().yawDegrees << viewport->viewOrbitAngles().pitchDegrees
            << "depth-write/function" << depthWrite << depthFunction
            << "stencil/scissor/alpha-coverage" << gl->glIsEnabled(GL_STENCIL_TEST)
            << gl->glIsEnabled(GL_SCISSOR_TEST) << gl->glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE)
            << "renderer" << reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER));
    viewport->doneCurrent();
    return !image.isNull() && image.save(QDir(evidence).filePath(name + QStringLiteral(".png")));
  };
  if (!snapshot(QStringLiteral("before-selection"))) return false;
  for (int pass = 0; pass < 2; ++pass) {
    for (int index = 0; index < ids.size(); ++index) {
      auto *item = sceneItem(tree, ids[index]);
      if (!item) return false;
      tree->scrollToItem(item);
      const QPoint point = tree->visualRect(tree->indexFromItem(item, 0)).center();
      QMouseEvent down(QEvent::MouseButtonPress, point, tree->viewport()->mapToGlobal(point),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent up(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point),
                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(tree->viewport(), &down);
      QApplication::sendEvent(tree->viewport(), &up);
      if (viewport->activeSceneId() != ids[index] ||
          !snapshot(QStringLiteral("selected-%1-%2").arg(pass).arg(index))) return false;
      viewport->clearSelection();
      if (!snapshot(QStringLiteral("cleared-%1-%2").arg(pass).arg(index))) return false;
    }
  }
  if (!snapshot(QStringLiteral("before-viewport-pick"))) return false;
  const QPointF pick = viewport->rect().center();
  QMouseEvent pickDown(QEvent::MouseButtonPress, pick, viewport->mapToGlobal(pick.toPoint()),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent pickUp(QEvent::MouseButtonRelease, pick, viewport->mapToGlobal(pick.toPoint()),
                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &pickDown);
  QApplication::sendEvent(viewport, &pickUp);
  if (!snapshot(QStringLiteral("after-viewport-pick"))) return false;
  viewport->clearSelection();
  if (!snapshot(QStringLiteral("cleared-viewport-pick"))) return false;
  // The saved project does not contain the live observation angle. Exercise
  // the opposite side and a closer view through public navigation events.
  lockTools->setChecked(true);
  const QPointF end = pick + QPointF(viewport->width() * 0.45, 0);
  QMouseEvent orbitDown(QEvent::MouseButtonPress, pick, viewport->mapToGlobal(pick.toPoint()),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent orbitMove(QEvent::MouseMove, end, viewport->mapToGlobal(end.toPoint()),
                        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent orbitUp(QEvent::MouseButtonRelease, end, viewport->mapToGlobal(end.toPoint()),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &orbitDown);
  QApplication::sendEvent(viewport, &orbitMove);
  QApplication::sendEvent(viewport, &orbitUp);
  lockTools->setChecked(false);
  viewport->clearSelection();
  if (!snapshot(QStringLiteral("opposite-before-selection"))) return false;
  viewport->selectModel();
  if (!snapshot(QStringLiteral("opposite-selected"))) return false;
  viewport->clearSelection();
  QWheelEvent zoom(pick, viewport->mapToGlobal(pick.toPoint()), {}, QPoint(0, 360),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(viewport, &zoom);
  if (!snapshot(QStringLiteral("close-before-selection"))) return false;
  viewport->selectModel();
  if (!snapshot(QStringLiteral("close-selected"))) return false;
  viewport->clearSelection();
  viewport->setAxisView(NavigationAxis::PositiveY);
  if (!waitUntil([&] { return std::abs(viewport->viewOrbitAngles().yawDegrees) < 0.01F &&
      std::abs(viewport->viewOrbitAngles().pitchDegrees) < 0.01F; })) return false;
  lockTools->setChecked(true);
  const auto dragView = [&](const QPointF &delta) {
    const QPointF start = viewport->rect().center();
    const QPointF finish = start + delta;
    QMouseEvent down(QEvent::MouseButtonPress, start, viewport->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, finish, viewport->mapToGlobal(finish.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, finish, viewport->mapToGlobal(finish.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &down); QApplication::sendEvent(viewport, &move); QApplication::sendEvent(viewport, &up);
  };
  dragView(QPointF(-297, 0));
  dragView(QPointF(0, 58));
  lockTools->setChecked(false);
  if (!viewport->focusModel()) return false;
  viewport->clearSelection();
  if (!snapshot(QStringLiteral("recording-view-before-selection"))) return false;
  viewport->selectModel();
  if (!snapshot(QStringLiteral("recording-view-selected"))) return false;
  viewport->clearSelection();
  for (int index = 0; index < paths.size(); ++index) {
    const auto after = document->sceneObjects().at(index);
    const auto before = initialObjects.at(index);
    if (hashes[index] != sha256(paths[index]) || before.translation != after.translation ||
        before.rotation != after.rotation || before.scale != after.scale || before.visible != after.visible) return false;
  }
  // This is a diagnostic capture, not a claim that its visual anomaly passed.
  document->createUntitled(temporary.path(), "Empty diagnostic", &error);
  qInfo() << "SCENE_INPUT_DIAGNOSTIC CAPTURED: owned settings/process, unchanged original hashes and transforms";
  return true;
}
}

bool runSceneVisibilitySmokeTest(MainWindow &window) {
  if (qEnvironmentVariable("GSW_SCENE_SELECTION_LARGE_SYNTHETIC") == QStringLiteral("1"))
    return runLargeSelectionRegression(window);
  const QByteArray inputJson = qgetenv("GSW_SCENE_SELECTION_INPUTS_JSON");
  if (!inputJson.isEmpty()) {
    QJsonParseError error;
    const auto inputs = QJsonDocument::fromJson(inputJson, &error);
    return error.error == QJsonParseError::NoError && inputs.isArray() &&
        runInputSelectionDiagnostic(window, inputs.array());
  }
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  QTemporaryDir temporary;
  const auto check = [](bool success, const char *message) {
    if (!success) qCritical() << "SCENE_VISIBILITY FAIL:" << message;
    return success;
  };
  if (!check(viewport && document && tree && temporary.isValid(), "window services")) return false;
  window.resize(1440, 900);
  QString error;
  const QString working = QDir(temporary.path()).filePath("working");
  if (!QDir().mkpath(working) || !document->createUntitled(working, "Visibility smoke", &error)) return false;
  auto *lockTools = window.findChild<QAction *>("lockEditToolsAction");
  if (!check(lockTools, "edit lock action")) return false;
  lockTools->setChecked(false);
  QStringList ids, paths;
  QList<QByteArray> hashes;
  for (int kind = 0; kind < 4; ++kind) {
    const QString path = QDir(temporary.path()).filePath(QString("visibility-%1.ply").arg(kind));
    if (!check(fixture(path, kind) && document->addScenePath(path, &error), "import fixture")) return false;
    ids.append(document->activeSceneId());
    paths.append(path);
    hashes.append(sha256(path));
    if (!check(waitUntil([&] {
          return kind == 1 ? viewport->meshRenderingAvailable()
              : kind >= 2 ? viewport->gaussianRenderingAvailable() : viewport->selectableModelAvailable();
        }), "point, mesh, 3DGS and 2DGS resources loaded")) return false;
    if (kind >= 2 && !check(viewport->sourceHasSurfelAttributes() == (kind == 3), "2DGS is a real two-scale surfel fixture")) return false;
  }
  if (!check(viewport->sceneObjectCount() == 4 && viewport->visibleSceneObjectCount() == 4, "all imported objects visible by default")) return false;
  viewport->setAxisView(NavigationAxis::PositiveZ);
  waitUntil([&] { return false; }, 400);
  viewport->selectAllModels();
  if (!check(viewport->focusModel(), "fit imported models")) return false;
  viewport->clearSelection();
  if (!check(document->setSceneObjectVisibility(ids, false), "capture geometry-free selection reference")) return false;
  const QImage emptyFrame = viewport->grabFramebuffer();
  if (!check(!emptyFrame.isNull() && document->setSceneObjectVisibility(ids, true),
             "restore all four sources after the background capture")) return false;
  QImage frame;
  std::array<Patch, 4> baseline;
  if (!check(waitUntil([&] {
        frame = viewport->grabFramebuffer();
        baseline = patches(frame);
        return std::all_of(baseline.begin(), baseline.end(), [](const Patch &patch) { return patch.count > 120; });
      }), "point, mesh, 3DGS and 2DGS simultaneously rendered")) {
    qInfo() << "SCENE_VISIBILITY pixels" << baseline[0].count << baseline[1].count << baseline[2].count << baseline[3].count << frame.size();
    return false;
  }
  const QString evidence = QDir(qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", QDir::tempPath())).filePath(
      "scene-visibility-" + AppLanguage::current() + (AppTheme::currentTheme() == UiTheme::Light ? "-light" : "-dark"));
  QDir().mkpath(evidence);
  frame.save(QDir(evidence).filePath("all-visible.png"));
  const QImage baselineFrame = frame;
  const auto initialObjects = document->sceneObjects();
  const auto target = viewport->viewTarget();
  const float distance = viewport->viewDistance();
  const auto angles = viewport->viewOrbitAngles();
  const QString active = document->activeSceneId();
  const auto selectedTreeIds = [&] {
    QSet<QString> selected;
    for (const auto *item : tree->selectedItems()) {
      const QString id = item->data(0, Qt::UserRole + 2).toString();
      if (!id.isEmpty()) selected.insert(id);
    }
    return selected;
  };
  const auto nameClick = [&](const QString &id) {
    auto *item = sceneItem(tree, id);
    if (!item) return false;
    tree->scrollToItem(item);
    const QPoint point = tree->visualRect(tree->indexFromItem(item, 0)).center();
    if (tree->indexAt(point).column() != 0 || tree->itemAt(point) != item) return false;
    QMouseEvent down(QEvent::MouseButtonPress, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &down);
    QApplication::sendEvent(tree->viewport(), &up);
    return true;
  };
  // The real tree-name seam must not move, shrink or replace any visible
  // peer when switching between point, mesh, 3DGS and 2DGS sources. Compare
  // each source's original upper/lower screen strips. The center hint and
  // transform gizmo are deliberately not geometry and can cover a peer.
  // Keep the same pixel-count and centroid limits outside those overlays.
  for (int pass = 0; pass < 2; ++pass) {
    for (int kind = 0; kind < 4; ++kind) {
      if (pass == 0) {
        if (!check(nameClick(ids[kind]), "click visible source name")) return false;
      } else {
        const QPointF point = baseline[kind].center / viewport->devicePixelRatioF();
        QMouseEvent down(QEvent::MouseButtonPress, point, viewport->mapToGlobal(point.toPoint()),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, point, viewport->mapToGlobal(point.toPoint()),
                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &down);
        QApplication::sendEvent(viewport, &up);
      }
      frame = viewport->grabFramebuffer();
      frame.save(QDir(evidence).filePath(QString("selected-%1-%2.png").arg(pass).arg(kind)));
      if (!check(viewport->activeSceneId() == ids[kind] && document->activeSceneId() == ids[kind] &&
                 viewport->selectedSceneIds() == QStringList{ids[kind]} &&
                 viewport->visibleSceneObjectCount() == 4,
                 "name click changes only the active and selected source")) return false;
      const auto expectedMode = kind == 0 ? NativeViewport::RenderMode::Points
          : kind == 1 ? NativeViewport::RenderMode::Mesh : NativeViewport::RenderMode::Gaussians;
      if (!check(viewport->renderMode() == expectedMode &&
                 viewport->sourceHasGaussianAttributes() == (kind >= 2) &&
                 viewport->sourceHasSurfelAttributes() == (kind == 3),
                 "tree and viewport selection retain the source's actual render mode and attributes")) return false;
      // Read-only diagnostics, not assertions about private renderer details:
      // retain the actual post-QPainter state if a driver's frame diverges.
      viewport->makeCurrent();
      auto *gl = viewport->context()->extraFunctions();
      GLboolean depthWrite = GL_TRUE;
      GLint depthFunction = 0;
      gl->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
      gl->glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
      qInfo() << "SCENE_SELECTION render" << pass << kind << viewport->renderMode()
              << "indexed/points" << viewport->indexedGaussianRendering() << viewport->renderedPointCount()
              << "depth-write/function" << depthWrite << depthFunction
              << "stencil/scissor/alpha-coverage" << gl->glIsEnabled(GL_STENCIL_TEST)
              << gl->glIsEnabled(GL_SCISSOR_TEST) << gl->glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
      viewport->doneCurrent();
      const auto hint = viewport->selectionHintBounds();
      const qreal ratio = viewport->devicePixelRatioF();
      const QRect hintPixels = hint.isEmpty() ? QRect() :
          QRectF(hint.x() * ratio, hint.y() * ratio, hint.width() * ratio,
                 hint.height() * ratio).toAlignedRect().adjusted(-2, -2, 2, 2);
      for (int peer = 0; peer < 4; ++peer) {
        const auto bounds = baseline[peer].bounds;
        const int stripHeight = std::max(1, bounds.height() / 5);
        QRegion sample(QRect(bounds.left(), bounds.top(), bounds.width(), stripHeight));
        sample += QRect(bounds.left(), bounds.bottom() - stripHeight + 1, bounds.width(), stripHeight);
        if (!hintPixels.isEmpty()) sample -= hintPixels;
        Patch expected, actual;
        for (const auto &region : sample) {
          // Object selection intentionally tints its RGB, especially the
          // antialiased edge of a small point. For that source compare actual
          // foreground coverage against the same empty backdrop, not a hue
          // threshold. Unselected peers retain the original color predicate.
          const auto before = peer == kind ? coverage(baselineFrame, emptyFrame, region)
                                              : patches(baselineFrame, region)[peer];
          const auto after = peer == kind ? coverage(frame, emptyFrame, region)
                                            : patches(frame, region)[peer];
          expected.center += before.center * before.count; expected.count += before.count;
          actual.center += after.center * after.count; actual.count += after.count;
        }
        if (expected.count) expected.center /= expected.count;
        if (actual.count) actual.center /= actual.count;
        qInfo() << "SCENE_SELECTION peer" << pass << kind << peer << "hint" << hintPixels
                << "pixels before/after" << expected.count << actual.count
                << "centers before/after" << expected.center << actual.center;
        if (!check(expected.count > 60 && actual.count >= expected.count * 9 / 10 &&
                     actual.count <= expected.count * 11 / 10 &&
                     QLineF(actual.center, expected.center).length() < 1.0,
                   "selection preserves the selected model and peer geometry outside actual hint bounds")) return false;
      }
      viewport->clearSelection();
      const auto cleared = patches(viewport->grabFramebuffer());
      for (int peer = 0; peer < 4; ++peer)
        if (!check(cleared[peer].count >= baseline[peer].count * 9 / 10 &&
                   cleared[peer].count <= baseline[peer].count * 11 / 10 &&
                   QLineF(cleared[peer].center, baseline[peer].center).length() < 1.0,
                   "selection round-trip retains each model's size and position")) return false;
      const auto after = document->sceneObjects();
      for (int peer = 0; peer < 4; ++peer)
        if (!check(after[peer].path == initialObjects[peer].path &&
                   after[peer].translation == initialObjects[peer].translation &&
                   after[peer].rotation == initialObjects[peer].rotation &&
                   after[peer].scale == initialObjects[peer].scale && after[peer].visible,
                   "source selection never rewrites peer transforms or visibility")) return false;
    }
  }
  if (!check(nameClick(active), "restore initial active source after selection regression")) return false;
  viewport->clearSelection();
  for (int kind = 0; kind < 4; ++kind) {
    auto *item = sceneItem(tree, ids[kind]);
    if (!check(item && item->checkState(1) == Qt::Checked, "visible eye cell state")) return false;
    tree->scrollToItem(item);
    const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2,
                       tree->visualItemRect(item).center().y());
    const auto selectedBeforeEye = selectedTreeIds();
    const auto eyeClick = [&] {
      QMouseEvent down(QEvent::MouseButtonPress, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent up(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(tree->viewport(), &down);
      QApplication::sendEvent(tree->viewport(), &up);
    };
    eyeClick();
    if (!check(waitUntil([&] {
          frame = viewport->grabFramebuffer();
          return !viewport->sceneObjectVisible(ids[kind]) && patches(frame)[kind].count < baseline[kind].count / 8;
        }), "eye click hides the actual rendered model")) return false;
    if (!check(document->activeSceneId() == active && viewport->viewTarget() == target &&
               viewport->viewDistance() == distance && viewport->viewOrbitAngles() == angles &&
               viewport->selectedSceneIds().isEmpty() && selectedTreeIds() == selectedBeforeEye,
               "eye click leaves active model, camera and exact tree selection unchanged")) return false;
    lockTools->setChecked(true);
    const auto beforeMiss = viewport->viewTarget();
    if (!check(!viewport->centerObservationAt(baseline[kind].center / viewport->devicePixelRatioF()) &&
               viewport->viewTarget() == beforeMiss, "hidden geometry cannot be picked for observation")) return false;
    lockTools->setChecked(false);
    frame.save(QDir(evidence).filePath(QString("hidden-%1.png").arg(kind)));
    item = sceneItem(tree, ids[kind]);
    if (!check(item && item->checkState(1) == Qt::Unchecked, "hidden eye cell state")) return false;
    // Refreshing the tree may replace item pointers; use the actual new cell.
    const QPoint restoredPoint(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2,
                              tree->visualItemRect(item).center().y());
    QMouseEvent restoreDown(QEvent::MouseButtonPress, restoredPoint, tree->viewport()->mapToGlobal(restoredPoint), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent restoreUp(QEvent::MouseButtonRelease, restoredPoint, tree->viewport()->mapToGlobal(restoredPoint), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &restoreDown);
    QApplication::sendEvent(tree->viewport(), &restoreUp);
    if (!check(waitUntil([&] { return viewport->sceneObjectVisible(ids[kind]) && patches(viewport->grabFramebuffer())[kind].count > baseline[kind].count / 2; }), "eye click restores model without re-import")) return false;
    if (!check(selectedTreeIds() == selectedBeforeEye, "restoring an eye retains exact selected tree rows")) return false;
  }
  bool sourceUiChecks = true;
  auto *exportAction = window.findChild<QAction *>("exportModelAction");
  if (!check(exportAction != nullptr, "export action available for source identity checks")) return false;
  for (int kind = 0; kind < 4; ++kind) {
    const int other = (kind + 1) % 4;
    if (!check(viewport->activateSceneObject(ids[other]) && document->setSceneObjectVisibility({ids[kind]}, false),
               "prepare a hidden model beside another active source")) return false;
    tree->clearSelection();
    if (!check(nameClick(ids[kind]), "click hidden model name cell")) return false;
    sourceUiChecks &= check(document->activeSceneId() == ids[kind] && viewport->activeSceneId() == ids[kind] &&
        viewport->modelExportOptions().sourcePath == paths[kind] && !viewport->sceneObjectVisible(ids[kind]) &&
        viewport->selectedSceneIds().isEmpty() && !viewport->modelSelected(),
        "hidden row becomes the inspected/export source without being shown or transform-selected");
    bool inspectorSource = false;
    for (const auto *label : window.findChildren<QLabel *>())
      inspectorSource |= label->text().contains(QFileInfo(paths[kind]).fileName());
    sourceUiChecks &= check(inspectorSource, "inspector identifies the selected hidden source");
    sourceUiChecks &= check(exportAction->isEnabled(), "hidden loaded source remains exportable");
    bool dialogSeen = false, dialogSource = false;
    QTimer dialogTimer;
    dialogTimer.setInterval(10);
    QObject::connect(&dialogTimer, &QTimer::timeout, &window, [&] {
      auto *dialog = window.findChild<QDialog *>(QStringLiteral("modelExportDialog"));
      if (!dialog) return;
      dialogSeen = true;
      dialogSource = static_cast<ModelExportDialog *>(dialog)->options().sourcePath == paths[kind];
      dialog->reject();
    });
    dialogTimer.start();
    exportAction->trigger();
    dialogTimer.stop();
    sourceUiChecks &= check(dialogSeen && dialogSource, "real export dialog uses the selected hidden model, not the previous visible source");
    if (!check(document->setSceneObjectVisibility({ids[kind]}, true) && viewport->activateSceneObject(active),
               "restore display after hidden source identity check")) return false;
    viewport->clearSelection();
  }
  // A hidden target must not bypass the normal unsaved-crop confirmation.
  if (!check(viewport->activateSceneObject(ids[0]), "activate point source before pending crop check")) return false;
  viewport->invertSelection();
  viewport->deleteSelection();
  if (!check(viewport->hasUnsavedSceneEdits(), "test-owned point deletion creates pending crop edits")) return false;
  if (!check(document->setSceneObjectVisibility({ids[1]}, false), "hide the pending-crop navigation target")) return false;
  bool cancelPromptSeen = false;
  QTimer cancelTimer;
  cancelTimer.setInterval(10);
  QObject::connect(&cancelTimer, &QTimer::timeout, &window, [&] {
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!box || !box->button(QMessageBox::Cancel)) return;
    cancelPromptSeen = true;
    box->button(QMessageBox::Cancel)->click();
  });
  cancelTimer.start();
  if (!check(nameClick(ids[1]), "request hidden source while crop edits are pending")) return false;
  cancelTimer.stop();
  sourceUiChecks &= check(cancelPromptSeen && document->activeSceneId() == ids[0] &&
      viewport->activeSceneId() == ids[0] && viewport->hasUnsavedSceneEdits() &&
      !selectedTreeIds().contains(ids[1]), "canceling the crop prompt keeps source identity, edits and tree selection consistent");
  viewport->undoEdit();
  if (!check(!viewport->hasUnsavedSceneEdits() && document->setSceneObjectVisibility({ids[1]}, true) &&
             viewport->activateSceneObject(active), "restore test-owned crop and visibility state")) return false;
  viewport->clearSelection();
  auto *hide = window.findChild<QAction *>("hideSelectedModelsAction");
  auto *show = window.findChild<QAction *>("showSelectedModelsAction");
  auto *isolate = window.findChild<QAction *>("isolateSelectedModelsAction");
  auto *showAll = window.findChild<QAction *>("showAllModelsAction");
  if (!check(hide && show && isolate && showAll, "batch visibility actions")) return false;
  const auto selectRows = [&](const QStringList &selected) {
    tree->clearSelection();
    for (const QString &id : selected) if (auto *item = sceneItem(tree, id)) item->setSelected(true);
  };
  selectRows({ids[0], ids[2]});
  hide->trigger();
  const QSet<QString> firstPair{ids[0], ids[2]};
  if (!check(viewport->visibleSceneObjectCount() == 2 && !viewport->sceneObjectVisible(ids[0]) && !viewport->sceneObjectVisible(ids[2]) &&
             selectedTreeIds() == firstPair, "batch hide preserves exact hidden selected rows")) return false;
  show->trigger();
  if (!check(viewport->visibleSceneObjectCount() == 4 && selectedTreeIds() == firstPair,
             "batch show includes and preserves exact selected hidden rows")) return false;
  if (!check(document->setSceneObjectVisibility({ids[0]}, false) && selectedTreeIds() == firstPair,
             "mixed visible/hidden keyboard selection")) return false;
  const auto pressSpace = [&] {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ");
    QApplication::sendEvent(tree, &event);
  };
  pressSpace();
  if (!check(viewport->visibleSceneObjectCount() == 2 && !viewport->sceneObjectVisible(ids[0]) &&
             !viewport->sceneObjectVisible(ids[2]) && selectedTreeIds() == firstPair,
             "Space hides a mixed selection and preserves the exact hidden rows")) return false;
  pressSpace();
  if (!check(viewport->visibleSceneObjectCount() == 4 && selectedTreeIds() == firstPair,
             "Space reveals an all-hidden selection without selecting other rows")) return false;
  selectRows({ids[1], ids[3]});
  isolate->trigger();
  if (!check(viewport->visibleSceneObjectCount() == 2 && viewport->sceneObjectVisible(ids[1]) && viewport->sceneObjectVisible(ids[3]) &&
             selectedTreeIds() == QSet<QString>{ids[1], ids[3]}, "isolate preserves exact selected model rows")) return false;
  const auto visibilityState = document->sceneCollectionJson();
  for (const QString &language : AppLanguage::supported()) {
    if (!check(AppLanguage::apply(language, false), "live language switch")) return false;
    if (!check(document->sceneCollectionJson() == visibilityState && viewport->visibleSceneObjectCount() == 2 &&
               show->text() == QCoreApplication::translate("Workbench", "显示所选模型") &&
               hide->text() == QCoreApplication::translate("Workbench", "隐藏所选模型") &&
               isolate->text() == QCoreApplication::translate("Workbench", "仅显示所选模型") &&
               showAll->text() == QCoreApplication::translate("Workbench", "显示全部模型"), "language switch preserves visibility and updates exact action labels")) return false;
    const QLabel *renderer = nullptr;
    for (const auto *label : window.findChildren<QLabel *>())
      if (label->property("gswStatusRole").toString() == QStringLiteral("renderer")) renderer = label;
    const QString expectedMode = viewport->renderMode() == NativeViewport::RenderMode::Mesh
        ? QCoreApplication::translate("Workbench", "网格预览")
        : viewport->renderMode() == NativeViewport::RenderMode::Gaussians
            ? QCoreApplication::translate("Workbench", "高斯预览")
            : QCoreApplication::translate("Workbench", "点预览");
    sourceUiChecks &= check(renderer && renderer->text().startsWith(expectedMode + QStringLiteral(" | ")) &&
        !renderer->text().contains(QCoreApplication::translate("Workbench", "未载入场景")) &&
        renderer->text() != QCoreApplication::translate("Workbench", "正在读取 PLY 场景"),
        "live language switch immediately retains the actual loaded renderer status without waiting for a new frame");
    window.grab().save(QDir(evidence).filePath("window-" + language + ".png"));
  }
  const UiTheme originalTheme = AppTheme::currentTheme();
  for (const UiTheme theme : {UiTheme::Light, UiTheme::Dark}) {
    AppTheme::applyTheme(*qApp, theme, false);
    if (!check(document->sceneCollectionJson() == visibilityState && viewport->visibleSceneObjectCount() == 2,
               "live theme switch preserves hidden states and sources")) return false;
    for (int kind = 0; kind < 4; ++kind) {
      auto *item = sceneItem(tree, ids[kind]);
      if (!check(item && item->checkState(1) == (kind == 1 || kind == 3 ? Qt::Checked : Qt::Unchecked),
                 "eye cells retain their state in both themes")) return false;
    }
  }
  AppTheme::applyTheme(*qApp, originalTheme, false);
  if (!check(document->setSceneObjectVisibility(ids, false) && viewport->visibleSceneObjectCount() == 0,
             "all models can be hidden without removing any object")) return false;
  viewport->selectAllModels();
  viewport->selectModelForMove();
  if (!check(viewport->selectedModelCount() == 0 && !viewport->focusModel() &&
             !viewport->modelTransformActive() && !viewport->hasEditableScene(),
             "an all-hidden collection cannot be selected, framed or edited")) return false;
  if (!check(waitUntil([&] {
        frame = viewport->grabFramebuffer();
        const auto hidden = patches(frame);
        for (int kind = 0; kind < 4; ++kind)
          if (hidden[kind].count >= baseline[kind].count / 8) return false;
        return true;
      }), "all-hidden framebuffer contains no imported geometry")) return false;
  frame.save(QDir(evidence).filePath("all-hidden.png"));
  if (!check(document->isolateSceneObjects({ids[1], ids[3]}), "restore isolated visibility before saving")) return false;
  const QString saved = QDir(temporary.path()).filePath("visibility.gsw.json");
  if (!check(document->saveManifest(saved, &error) && document->load(saved, &error), "save and reopen visibility")) return false;
  if (!check(document->sceneObjects().size() == 4 && viewport->visibleSceneObjectCount() == 2 &&
             !viewport->sceneObjectVisible(ids[0]) && viewport->sceneObjectVisible(ids[1]) &&
             !viewport->sceneObjectVisible(ids[2]) && viewport->sceneObjectVisible(ids[3]), "hidden state persists across reopen")) return false;
  showAll->trigger();
  if (!check(viewport->visibleSceneObjectCount() == 4, "show all restores persisted hidden models")) return false;
  const auto restoredObjects = document->sceneObjects();
  for (int kind = 0; kind < 4; ++kind) {
    const auto &before = initialObjects[kind];
    const auto &after = restoredObjects[kind];
    if (!check(before.id == after.id && before.path == after.path && before.translation == after.translation &&
               before.rotation == after.rotation && before.scale == after.scale && before.vertexCount == after.vertexCount &&
               hashes[kind] == sha256(paths[kind]), "visibility never rewrites sources or model transforms")) return false;
  }
  document->createUntitled(temporary.path(), "Empty", &error);
  if (!sourceUiChecks) return false;
  qInfo() << "SCENE_VISIBILITY PASS: real point/mesh/3DGS/2DGS render suppression, eye picking isolation, batch hide/show/isolate, live trilingual labels, save/reopen, unchanged sources and transforms";
  return true;
}
}
