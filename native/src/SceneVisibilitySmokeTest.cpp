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
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEnterEvent>
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
#include <QOpenGLFramebufferObject>
#include <QPaintEvent>
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
#include <QWindow>

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

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
  const auto backgroundIsolated = [&] {
    if (qEnvironmentVariable("GSW_SMOKE_BACKGROUND") != QStringLiteral("1")) return true;
    bool intersectsScreen = false;
    for (const auto *screen : QGuiApplication::screens())
      intersectsScreen |= window.frameGeometry().intersects(screen->geometry());
    const bool activeWindow = window.isActiveWindow();
    if (intersectsScreen || activeWindow) {
      window.hide();
      qCritical() << "GAUSSIAN_CLICK_DIAG FAIL: background window isolation violated; window hidden"
                  << "intersects-screen" << intersectsScreen << "active" << activeWindow;
      return false;
    }
    return true;
  };
  if (!check(backgroundIsolated(), "background regression remains outside every screen without activation")) return false;
  const bool originallyDisabled = qEnvironmentVariableIsSet("GSW_DISABLE_INDEXED_GAUSSIANS");
  const auto originalDisabled = qgetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
  const auto restoreEnvironment = qScopeGuard([&] {
    if (originallyDisabled) qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", originalDisabled);
    else qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
  });
  window.resize(1440, 900);
  if (!check(backgroundIsolated(), "resized regression window remains isolated from the user's desktop")) return false;
  auto *lock = window.findChild<QAction *>("lockEditToolsAction");
  if (!check(lock, "actual edit-lock action")) return false;
  lock->setChecked(false);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  const QString evidence = QDir(qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", temporary.path())).filePath(
      "gaussian-click-" + AppLanguage::current() + (AppTheme::currentTheme() == UiTheme::Light ? "-light" : "-dark"));
  if (!QDir().mkpath(evidence)) return false;
  const auto click = [&](const QPointF &point) {
    if (!backgroundIsolated()) return false;
    QMouseEvent down(QEvent::MouseButtonPress, point, viewport->mapToGlobal(point.toPoint()),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, viewport->mapToGlobal(point.toPoint()),
                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &down);
    QApplication::sendEvent(viewport, &up);
    return backgroundIsolated();
  };
  const auto clickPreservingGlState = [&](const QPointF &point) {
    auto *previousContext = QOpenGLContext::currentContext();
    auto *previousSurface = previousContext ? previousContext->surface() : nullptr;
    viewport->makeCurrent();
    auto *expectedContext = viewport->context();
    if (!check(expectedContext && QOpenGLContext::currentContext() == expectedContext,
               "state contract starts with the actual viewport context current")) return false;
    auto *expectedSurface = expectedContext->surface();
    auto *functions = expectedContext->extraFunctions();
    GLint originalActiveTexture = GL_TEXTURE0, originalTexture0 = 0;
    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &originalActiveTexture);
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &originalTexture0);
    GLuint sentinel = 0;
    functions->glGenTextures(1, &sentinel);
    functions->glBindTexture(GL_TEXTURE_2D, sentinel);
    // Binding creates a valid texture object without uploading pixels or
    // depending on any inherited unpack/PBO state.
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    const auto cleanup = qScopeGuard([&] {
      // Record violations before any recovery makeCurrent(): cleanup must
      // never mask a picking path that detached the caller's context.
      if (QOpenGLContext::currentContext() != expectedContext) viewport->makeCurrent();
      if (QOpenGLContext::currentContext() == expectedContext) {
        functions->glActiveTexture(GL_TEXTURE0);
        functions->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(originalTexture0));
        functions->glDeleteTextures(1, &sentinel);
        functions->glActiveTexture(static_cast<GLenum>(originalActiveTexture));
      }
      if (previousContext && previousSurface) previousContext->makeCurrent(previousSurface);
      else expectedContext->doneCurrent();
    });
    if (!check(sentinel != 0 && functions->glIsTexture(sentinel), "valid QA sentinel texture is bound before clicking")) return false;
    QElapsedTimer clickTimer;
    clickTimer.start();
    const bool delivered = click(point);
    const qint64 clickMicroseconds = clickTimer.nsecsElapsed() / 1000;
    const bool sameContext = QOpenGLContext::currentContext() == expectedContext;
    const bool sameSurface = sameContext && expectedContext->surface() == expectedSurface;
    GLint activeAfter = -1, textureAfter = -1;
    if (sameContext) {
      functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeAfter);
      functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureAfter);
    }
    qInfo() << "GAUSSIAN_CLICK_STATE_CONTRACT context/surface-preserved" << sameContext << sameSurface
            << "active-before/after" << GL_TEXTURE0 << activeAfter
            << "texture-before/after" << sentinel << textureAfter
            << "click-us" << clickMicroseconds;
    return check(delivered && sameContext && sameSurface && activeAfter == GL_TEXTURE0 &&
                 textureAfter == static_cast<GLint>(sentinel),
                 "static Gaussian click preserves caller context, surface, active unit and existing texture binding");
  };
  const bool stateContractOnly = qEnvironmentVariable("GSW_SCENE_SELECTION_STATE_CONTRACT") == QStringLiteral("1");
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
    if (!check(backgroundIsolated() && viewport->context(), "loaded regression retains an isolated real OpenGL context")) return false;
    viewport->makeCurrent();
    auto *gl = viewport->context()->extraFunctions();
    qInfo() << "GAUSSIAN_CLICK_DIAG OpenGL"
            << "vendor" << reinterpret_cast<const char *>(gl->glGetString(GL_VENDOR))
            << "renderer" << reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER))
            << "version" << reinterpret_cast<const char *>(gl->glGetString(GL_VERSION));
    viewport->doneCurrent();
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
        if (!check(repeat == 0 ? clickPreservingGlState(pick) : click(pick),
                   "actual body click keeps the background window and current rendering state isolated")) return false;
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
    if (stateContractOnly) {
      if (!check(!sourceHash.isEmpty() && sha256(path) == sourceHash, "state-contract clicks never rewrite input data")) return false;
      continue;
    }
    // Replay the visible sequence from the recording as well as the immediate
    // framebuffer checks above. Precollect the empty image at the final zoom
    // before starting: never toggle visibility or rebuild render modes between
    // the initial click, deselection, zoom-out and subsequent click.
    const bool zoomToCursor = viewport->zoomToCursorEnabled();
    const auto restoreZoomPreference = qScopeGuard([&] { viewport->setZoomToCursorEnabled(zoomToCursor); });
    viewport->setZoomToCursorEnabled(false);
    const auto queuedFrames = [&] {
      if (!backgroundIsolated()) return false;
      for (int frame = 0; frame < 2; ++frame) {
        const auto beforeSerial = viewport->renderedFrameSerial();
        viewport->update();
        if (!waitUntil([&] { return viewport->renderedFrameSerial() > beforeSerial; }, 5000)) return false;
        if (!backgroundIsolated()) return false;
      }
      return true;
    };
    const auto wheel = [&](int delta) {
      const QPointF center = viewport->rect().center();
      QWheelEvent event(center, viewport->mapToGlobal(center.toPoint()), {}, QPoint(0, delta),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(viewport, &event);
    };
    for (int compatibility = 0; compatibility < 2; ++compatibility) {
      if (compatibility) qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", "1");
      else qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
      viewport->setRenderMode(NativeViewport::RenderMode::Points);
      viewport->setRenderMode(NativeViewport::RenderMode::Gaussians);
      viewport->clearSelection();
      wheel(-240);
      if (!check(document->setSceneObjectVisibility({object.id}, false), "prepare only the future-zoom background reference")) return false;
      const QImage futureEmpty = viewport->grabFramebuffer();
      if (!check(!futureEmpty.isNull() && document->setSceneObjectVisibility({object.id}, true),
                 "restore synthetic input before replaying the uninterrupted recording sequence")) return false;
      wheel(240);
      if (!check(queuedFrames(), "initial recording state receives two real queued viewport paints")) return false;
      const QImage initial = viewport->grabFramebuffer();
      const auto initialSignature = patches(initial)[3];
      const float initialDistance = viewport->viewDistance();
      if (!check(initialSignature.count > 5000 && viewport->indexedGaussianRendering() == !compatibility,
                 "recording sequence starts with the full visible Gaussian input on its required renderer")) return false;
      if (!check(click(initialSignature.center / viewport->devicePixelRatioF()), "initial replay click retains background isolation")) return false;
      if (!check(queuedFrames() && viewport->modelSelected() && viewport->selectedSceneIds() == QStringList{object.id},
                 "initial body click reaches selection and two normal queued paints")) return false;
      const QString prefix = label + (compatibility ? "-compatibility-replay" : "-resident-replay");
      const QImage initialSelected = viewport->grabFramebuffer();
      if (!check(initialSelected.save(QDir(evidence).filePath(prefix + "-initial-selected.png")),
                 "retain initial selected state after actual queued paints")) return false;
      const auto initialSelectedSignature = patches(initialSelected)[3];
      if (!check(initialSelectedSignature.count >= initialSignature.count * 85 / 100 &&
                 initialSelectedSignature.count <= initialSignature.count * 115 / 100 &&
                 QLineF(initialSelectedSignature.center, initialSignature.center).length() < 2.0,
                 "initial clicked model also remains visible through subsequent queued paints")) return false;
      viewport->clearSelection();
      if (!check(queuedFrames(), "deselection completes its normal viewport paints")) return false;
      wheel(-240);
      if (!check(queuedFrames() && viewport->viewDistance() > initialDistance * 1.4F &&
                 viewport->viewDistance() < initialDistance * 1.43F,
                 "two actual wheel detents move the camera out before the second click")) return false;
      const QImage zoomed = viewport->grabFramebuffer();
      const QRect sceneBody(35, 120, zoomed.width() - 70, zoomed.height() - 230);
      const auto zoomedSignature = patches(zoomed, sceneBody)[3];
      const QRect body = zoomedSignature.bounds.intersected(sceneBody);
      const auto targetAfterZoom = viewport->viewTarget();
      const auto distanceAfterZoom = viewport->viewDistance();
      const auto anglesAfterZoom = viewport->viewOrbitAngles();
      if (!check(zoomedSignature.count > 2500 && zoomed.save(QDir(evidence).filePath(prefix + "-zoomed-before.png")),
                 "retain actually visible zoomed model immediately before the recorded second click")) return false;
      if (!check(click(zoomedSignature.center / viewport->devicePixelRatioF()), "zoomed replay click retains background isolation")) return false;
      if (!check(queuedFrames(), "second body click completes two real queued viewport paints")) return false;
      const QImage selected = viewport->grabFramebuffer();
      if (!check(selected.save(QDir(evidence).filePath(prefix + "-zoomed-selected.png")),
                 "retain zoomed selected pixels after two queued paints")) return false;
      const auto *item = sceneItem(tree, object.id);
      if (!check(viewport->modelSelected() && viewport->selectedSceneIds() == QStringList{object.id} &&
                 item && item->isSelected() && item->checkState(1) == Qt::Checked &&
                 viewport->visibleSceneObjectCount() == 1 && viewport->renderedPointCount() == 512202 &&
                 viewport->indexedGaussianRendering() == !compatibility &&
                 viewport->viewTarget() == targetAfterZoom && viewport->viewDistance() == distanceAfterZoom &&
                 viewport->viewOrbitAngles() == anglesAfterZoom && viewport->modelTranslation() == object.translation &&
                 viewport->modelRotation() == object.rotation && viewport->modelScale() == object.scale &&
                 !viewport->hasUnsavedSceneEdits(), "recording replay changes selection only, retaining the visible source and camera")) return false;
      const int strip = std::max(1, body.height() / 5);
      QRegion sample(QRect(body.left(), body.top(), body.width(), strip));
      sample += QRect(body.left(), body.bottom() - strip + 1, body.width(), strip);
      const auto hint = viewport->selectionHintBounds();
      const qreal ratio = viewport->devicePixelRatioF();
      sample -= QRectF(hint.x() * ratio, hint.y() * ratio, hint.width() * ratio,
                       hint.height() * ratio).toAlignedRect().adjusted(-3, -3, 3, 3);
      Patch expected, actual;
      for (const auto &region : sample) {
        for (int y = region.top(); y <= region.bottom(); ++y) {
          for (int x = region.left(); x <= region.right(); ++x) {
            const auto base = zoomed.pixelColor(x, y), after = selected.pixelColor(x, y), clear = futureEmpty.pixelColor(x, y);
            const auto difference = [&](const QColor &color) {
              return std::max({std::abs(color.red() - clear.red()), std::abs(color.green() - clear.green()),
                               std::abs(color.blue() - clear.blue())});
            };
            // New bounds/gizmo pixels are annotations, not retained source
            // geometry. Restrict both centroid masks to baseline coverage.
            if (difference(base) <= 45) continue;
            ++expected.count; expected.center += QPointF(x, y);
            if (difference(after) > 30) { ++actual.count; actual.center += QPointF(x, y); }
          }
        }
      }
      if (expected.count) expected.center /= expected.count;
      if (actual.count) actual.center /= actual.count;
      qInfo() << "GAUSSIAN_CLICK_REPLAY" << label << "compatibility" << compatibility
              << "foreground-before/selected" << expected.count << actual.count
              << "centroid-delta" << QLineF(expected.center, actual.center).length()
              << "distance-before/after-wheel" << initialDistance << distanceAfterZoom;
      if (!check(expected.count > 600 && actual.count >= expected.count * 85 / 100 &&
                 actual.count <= expected.count * 115 / 100 && QLineF(expected.center, actual.center).length() < 2.0,
                 "zoomed Gaussian body stays visible after the recorded click sequence and queued paints")) return false;
      viewport->clearSelection();
      if (!check(queuedFrames() && viewport->grabFramebuffer().save(QDir(evidence).filePath(prefix + "-zoomed-cleared.png")),
                 "retain normal deselection paints after recording replay")) return false;
      wheel(240);
    }
    if (!check(!sourceHash.isEmpty() && sha256(path) == sourceHash, "model clicks never rewrite input data")) return false;
  }
  QString error;
  if (!document->createUntitled(temporary.path(), "Empty regression", &error)) return false;
  qInfo() << "GAUSSIAN_CLICK_VISIBILITY PASS: 512202-Gaussian 3DGS/2DGS real click, both renderer paths, unchanged pixels and source data";
  return true;
}

bool mixedGaussianFixture(const QString &path, int kind) {
  if (kind < 2) return fixture(path, kind);
  const int side = kind == 2 ? 128 : 64;
  const int rest = kind == 2 ? 45 : 9;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray header = "ply\nformat binary_little_endian 1.0\nelement vertex " + QByteArray::number(side * side) + '\n';
  for (const auto *name : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"})
    header += QByteArray("property float ") + name + '\n';
  for (int index = 0; index < rest; ++index) header += "property float f_rest_" + QByteArray::number(index) + '\n';
  for (const auto *name : {"opacity", "scale_0", "scale_1"}) header += QByteArray("property float ") + name + '\n';
  if (kind == 2) header += "property float scale_2\n";
  for (const auto *name : {"rot_0", "rot_1", "rot_2", "rot_3"}) header += QByteArray("property float ") + name + '\n';
  header += "end_header\n";
  if (file.write(header) != header.size()) return false;
  QDataStream data(&file);
  data.setByteOrder(QDataStream::LittleEndian);
  data.setFloatingPointPrecision(QDataStream::SinglePrecision);
  const float center = -4.5F + kind * 3.0F;
  const float scale = std::log(1.5F / side);
  for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
    data << center - 0.75F + 1.5F * x / (side - 1) << -0.75F + 1.5F * y / (side - 1) << 0.0F;
    data << (kind == 2 ? -1.5F : 1.5F) << -1.5F << 1.5F;
    for (int coefficient = 0; coefficient < rest; ++coefficient) data << (coefficient % 7 == 0 ? 0.005F : 0.0F);
    data << 3.0F << scale << scale;
    if (kind == 2) data << scale;
    data << 1.0F << 0.0F << 0.0F << 0.0F;
  }
  return data.status() == QDataStream::Ok && file.flush();
}

// Infrastructure acceptance only: the final presented desktop is captured by
// F12 before any diagnostic GL readback. This does not assert that the user's
// original visual corruption was reproduced by these synthetic fixtures.
bool runRenderDiagnosticsSmoke(MainWindow &window) {
  const auto check = [](bool ok, const char *message) {
    if (!ok) qCritical() << "RENDER_DIAGNOSTICS_SMOKE FAIL:" << message;
    return ok;
  };
  if (!check(qEnvironmentVariable("GSW_SMOKE_BACKGROUND") != QStringLiteral("1"),
             "presentation diagnostics cannot run in an unexposed background window")) return false;
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *screen = QGuiApplication::primaryScreen();
  const QString output = qEnvironmentVariable("GSW_RENDER_DIAGNOSTICS_DIR");
  if (!check(viewport && document && screen && !output.isEmpty() &&
             QDir::isAbsolutePath(output) && QDir().mkpath(output),
             "real window services and explicit absolute diagnostic output directory")) return false;
  QTemporaryDir temporary(QDir(QCoreApplication::applicationDirPath()).filePath("render-diagnostics-XXXXXX"));
  if (!check(temporary.isValid(), "isolated synthetic input directory")) return false;
  const auto available = screen->availableGeometry();
  if (!check(available.width() >= 1000 && available.height() >= 750,
             "runner desktop is sufficiently large for an exposed diagnostic window")) return false;
  window.showNormal();
  window.resize(std::min(1440, available.width() - 100), std::min(900, available.height() - 100));
  window.move(available.topLeft() + QPoint(30, 30));
  if (qEnvironmentVariable("GITHUB_ACTIONS") == QStringLiteral("true")) {
    // This smoke entry is run only on an owned remote CI desktop. Never raise
    // a local user's application as a side effect of the diagnostic module.
    window.raise();
    window.activateWindow();
  }
  const auto exposed = [&] {
    if (!window.isVisible() || window.isMinimized() || !window.windowHandle() ||
        !window.windowHandle()->isExposed() || !window.isActiveWindow()) return false;
    const QRect viewportRect(viewport->mapToGlobal(QPoint()), viewport->size());
    int containingScreens = 0;
    for (const auto *candidate : QGuiApplication::screens())
      containingScreens += candidate->geometry().contains(viewportRect);
    return containingScreens == 1 && screen->availableGeometry().contains(window.frameGeometry());
  };
  if (!check(waitUntil(exposed, 5000), "owned test window is active, exposed and entirely on one screen")) return false;
  quint64 lastPresented = 0;
  const auto connection = QObject::connect(viewport, &QOpenGLWidget::frameSwapped, viewport, [&] {
    lastPresented = viewport->renderedFrameSerial();
  });
  const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
  QString error;
  const QString workspace = temporary.filePath("workspace");
  if (!check(QDir().mkpath(workspace) && document->createUntitled(workspace, "Render diagnostics smoke", &error),
             "create isolated diagnostic workspace")) return false;
  QStringList paths;
  QList<QByteArray> hashes;
  for (const int kind : {0, 2, 3}) {
    const QString path = temporary.filePath(QString("diagnostic-%1.ply").arg(kind));
    const auto before = viewport->renderedFrameSerial();
    if (!check(mixedGaussianFixture(path, kind) && document->addScenePath(path, &error),
               "import ordinary points, SH3 3DGS and true two-scale SH1 2DGS")) return false;
    paths.append(path); hashes.append(sha256(path));
    if (!check(waitUntil([&] {
          return exposed() && lastPresented > before && viewport->selectableModelAvailable() &&
              (kind == 0 || (viewport->gaussianRenderingAvailable() &&
               viewport->sourceHasSurfelAttributes() == (kind == 3)));
        }, 15000), "import reaches a normally rendered and presented frame without forced capture")) return false;
  }
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  viewport->setEditToolsLocked(true);
  viewport->setInteractionMode(NativeViewport::InteractionMode::Inspect);
  const auto beforeFit = viewport->renderedFrameSerial();
  viewport->selectAllModels();
  if (!check(viewport->focusModel(), "frame three synthetic model sources")) return false;
  viewport->clearSelection();
  viewport->setFocus(Qt::OtherFocusReason);
  if (!check(waitUntil([&] { return exposed() && lastPresented > beforeFit; }, 5000),
             "fit operation reaches normal composition")) return false;
  QElapsedTimer stable;
  stable.start();
  auto stableSerial = viewport->renderedFrameSerial();
  if (!check(waitUntil([&] {
        const auto serial = viewport->renderedFrameSerial();
        if (serial != stableSerial) { stableSerial = serial; stable.restart(); }
        return exposed() && serial > 0 && lastPresented == serial && stable.elapsed() >= 400;
      }, 10000), "normally presented scene is stable for 400ms before critical capture")) return false;
  const auto manifests = [&] {
    QSet<QString> result;
    QDirIterator iterator(output, {"manifest.json"}, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) result.insert(iterator.next());
    return result;
  };
  const auto oldManifests = manifests();
  const auto requestSerial = viewport->renderedFrameSerial();
  const auto objects = document->sceneObjects();
  const auto cameraTarget = viewport->viewTarget();
  const auto cameraDistance = viewport->viewDistance();
  const auto cameraAngles = viewport->viewOrbitAngles();
  const auto cameraOrthographic = viewport->orthographicProjection();
  // Critical phase: no grabFramebuffer(), QPaintEvent, QWidget::render/grab,
  // makeCurrent() or GL-state sentinels. F12 is delivered through the owned
  // top-level QWindow, followed only by normal events and artifact-file reads.
  QKeyEvent down(QEvent::KeyPress, Qt::Key_F12, Qt::NoModifier);
  QKeyEvent up(QEvent::KeyRelease, Qt::Key_F12, Qt::NoModifier);
  QApplication::sendEvent(window.windowHandle(), &down);
  QApplication::sendEvent(window.windowHandle(), &up);
  QString manifestPath;
  QJsonObject manifest;
  if (!check(waitUntil([&] {
        const auto current = manifests();
        for (const auto &path : current) {
          if (oldManifests.contains(path)) continue;
          QFile file(path);
          if (!file.open(QIODevice::ReadOnly)) continue;
          const auto parsed = QJsonDocument::fromJson(file.readAll());
          if (!parsed.isObject()) continue;
          const auto candidate = parsed.object();
          const auto status = candidate.value("status").toString();
          if (status == "complete" || status == "rejected") {
            manifestPath = path; manifest = candidate; return true;
          }
        }
        return false;
      }, 20000), "F12 creates a completed or explicitly rejected diagnostic manifest")) return false;
  qInfo() << "RENDER_DIAGNOSTICS_SMOKE manifest" << manifestPath << "status" << manifest.value("status");
  if (!check(manifest.value("status").toString() == "complete", "capture privacy/visibility guards accept the owned test window")) return false;
  const auto serial = [&](const char *name) {
    return manifest.value(QLatin1String(name)).toVariant().toULongLong();
  };
  if (!check(manifest.value("requestSerialMatch").toBool() && manifest.value("stageSerialMatch").toBool() &&
             serial("requestRenderedSerial") >= requestSerial && serial("requestRenderedSerial") > 0 &&
             serial("requestRenderedSerial") == serial("requestPresentedSerial") &&
             serial("stageFrameSerial") > serial("requestRenderedSerial") &&
             serial("stageFrameSerial") == serial("stagePresentedSerial"),
             "capture binds its before and staged output to actually presented serials")) return false;
  const QString captureRoot = QFileInfo(manifestPath).absolutePath();
  const auto image = [&](const QString &path, const bool nonuniform) {
    const QString absolute = QFileInfo(path).isAbsolute() ? QFileInfo(path).absoluteFilePath()
                                                         : QDir(captureRoot).absoluteFilePath(path);
    const auto clean = QDir::cleanPath(absolute);
    if (!clean.startsWith(QDir::cleanPath(captureRoot) + '/', Qt::CaseInsensitive) || QFileInfo(clean).isSymLink()) return false;
    const QImage pixels(clean);
    if (pixels.isNull() || pixels.width() < 100 || pixels.height() < 100) return false;
    if (!nonuniform) return true;
    int minimum = 255, maximum = 0;
    for (int y = 0; y < pixels.height(); y += std::max(1, pixels.height() / 64))
      for (int x = 0; x < pixels.width(); x += std::max(1, pixels.width() / 64)) {
        const auto color = pixels.pixelColor(x, y);
        const int peak = std::max({color.red(), color.green(), color.blue()});
        minimum = std::min(minimum, peak); maximum = std::max(maximum, peak);
      }
    return maximum - minimum > 32;
  };
  for (const auto *name : {"presented-before.png", "existing-before.png", "presented-after.png"})
    if (!check(image(QDir(captureRoot).filePath(name), true), "presented and existing images are decodable, substantial and nonblank")) return false;
  const QImage presentedBefore(QDir(captureRoot).filePath("presented-before.png"));
  const QImage presentedAfter(QDir(captureRoot).filePath("presented-after.png"));
  const QImage existingBefore(QDir(captureRoot).filePath("existing-before.png"));
  if (!check(presentedBefore.size() == presentedAfter.size() &&
             std::abs(presentedBefore.width() - existingBefore.width()) <= 2 &&
             std::abs(presentedBefore.height() - existingBefore.height()) <= 2,
             "presented and existing captures retain consistent physical pixel dimensions")) return false;
  const auto stages = manifest.value("stages").toArray();
  int validColors = 0, validDepths = 0;
  bool postPainter = false;
  if (!check(stages.size() >= 4 && stages.size() <= 32, "staged capture stays within its bounded pipeline")) return false;
  for (const auto &value : stages) {
    const auto stage = value.toObject();
    const auto name = stage.value("name").toString();
    if (stage.value("colorValid").toBool()) {
      if (!check(image(stage.value("colorPath").toString(), false), "valid stage color flag names a decodable in-root image")) return false;
      ++validColors;
      postPainter |= name == QStringLiteral("after-painter");
    }
    if (stage.value("depthValid").toBool()) {
      if (!check(image(stage.value("depthPath").toString(), false), "valid depth flag names a decodable in-root image")) return false;
      ++validDepths;
    }
    if (stage.value("stencilValid").toBool() &&
        !check(image(stage.value("stencilPath").toString(), false), "valid stencil flag names a decodable in-root image")) return false;
  }
  if (!check(validColors >= 4 && validDepths >= 1 && postPainter,
             "capture includes scene colors, depth and the post-QPainter boundary")) return false;
  if (!check(viewport->viewTarget() == cameraTarget && viewport->viewDistance() == cameraDistance &&
             viewport->viewOrbitAngles() == cameraAngles && viewport->orthographicProjection() == cameraOrthographic,
             "diagnostics leave camera target, distance, orbit and projection unchanged")) return false;
  const auto after = document->sceneObjects();
  if (!check(after.size() == objects.size(), "diagnostics preserve the imported collection")) return false;
  for (qsizetype index = 0; index < after.size(); ++index)
    if (!check(after[index].id == objects[index].id && after[index].path == objects[index].path &&
               after[index].translation == objects[index].translation && after[index].rotation == objects[index].rotation &&
               after[index].scale == objects[index].scale && after[index].visible == objects[index].visible &&
               sha256(paths[index]) == hashes[index], "diagnostics leave fixture sources and model state unchanged")) return false;
  if (!document->createUntitled(temporary.path(), "Empty diagnostics smoke", &error)) return false;
  qInfo() << "RENDER_DIAGNOSTICS_SMOKE PASS: presented-first infrastructure, serial binding and bounded stage artifacts; NOT original visual-anomaly reproduction";
  return true;
}

bool runMixedGaussianIsolation(MainWindow &window) {
  const auto check = [](bool ok, const char *message) {
    if (!ok) qCritical() << "MIXED_GAUSSIAN_ISOLATION FAIL:" << message;
    return ok;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  auto *lock = window.findChild<QAction *>("lockEditToolsAction");
  QTemporaryDir temporary(QDir(QCoreApplication::applicationDirPath()).filePath("mixed-gaussian-XXXXXX"));
  if (!check(viewport && document && tree && lock && temporary.isValid(), "real window services and isolated fixture directory")) return false;
  const bool hadOverride = qEnvironmentVariableIsSet("GSW_DISABLE_INDEXED_GAUSSIANS");
  const auto previousOverride = qgetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
  const auto restore = qScopeGuard([&] {
    if (hadOverride) qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", previousOverride);
    else qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS");
  });
  const QString evidence = QDir(qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", temporary.path())).filePath("mixed-gaussian-isolation");
  if (!QDir().mkpath(evidence)) return false;
  const bool backgroundRun = qEnvironmentVariable("GSW_SMOKE_BACKGROUND") == QStringLiteral("1");
  const auto backgroundSafe = [&] {
    if (!backgroundRun) return true;
    bool intersects = false;
    for (const auto *screen : QGuiApplication::screens()) intersects |= window.frameGeometry().intersects(screen->geometry());
    if (!intersects && !window.isActiveWindow()) return true;
    window.hide();
    qCritical() << "MIXED_GAUSSIAN_ISOLATION FAIL: background window became visible or active; hidden";
    return false;
  };
  if (!check(backgroundSafe(), "mixed background window remains outside all screens and inactive")) return false;
  window.resize(1440, 900);
  if (!check(backgroundSafe(), "resized mixed background window remains isolated")) return false;
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  lock->setChecked(false);
  QStringList paths;
  QList<QByteArray> hashes;
  for (int kind = 0; kind < 4; ++kind) {
    const QString path = temporary.filePath(QString("mixed-%1.ply").arg(kind));
    if (!check(mixedGaussianFixture(path, kind), "write distinct point, mesh, SH3 3DGS and SH1 2DGS fixtures")) return false;
    paths.append(path); hashes.append(sha256(path));
  }
  const auto mouse = [&](QWidget *widget, const QPointF &point) {
    QMouseEvent down(QEvent::MouseButtonPress, point, widget->mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, widget->mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &down); QApplication::sendEvent(widget, &up);
  };
  const auto stateClick = [&](QWidget *widget, const QPointF &point, int config, int pose, int route, int kind) {
    if (!backgroundSafe()) return false;
    auto *previous = QOpenGLContext::currentContext();
    auto *previousSurface = previous ? previous->surface() : nullptr;
    auto *context = viewport->context();
    if (context && previous != context) viewport->makeCurrent();
    if (!context || QOpenGLContext::currentContext() != context) return false;
    auto *surface = context->surface();
    auto *gl = context->extraFunctions();
    GLint oldUnit = GL_TEXTURE0, oldTexture = 0, oldDrawFbo = 0, oldReadFbo = 0;
    GLint oldReadBuffer = 0, oldPackRowLength = 0, oldUnpackBuffer = 0, oldRenderbuffer = 0;
    GLfloat oldClearDepth = 1.0F;
    gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &oldUnit);
    gl->glActiveTexture(GL_TEXTURE0); gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    gl->glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
    gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &oldPackRowLength);
    gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
    gl->glGetIntegerv(GL_RENDERBUFFER_BINDING, &oldRenderbuffer);
    gl->glGetFloatv(GL_DEPTH_CLEAR_VALUE, &oldClearDepth);
    GLuint sentinel = 0, unpack = 0;
    std::unique_ptr<QOpenGLFramebufferObject> callerFramebuffer;
    const auto cleanup = qScopeGuard([&] {
      if (QOpenGLContext::currentContext() != context) viewport->makeCurrent();
      if (QOpenGLContext::currentContext() == context) {
        // Qt's FBO destructor can change bindings. Delete every owned object
        // first, then restore the caller state, never the opposite order.
        callerFramebuffer.reset();
        if (unpack) gl->glDeleteBuffers(1, &unpack);
        if (sentinel) gl->glDeleteTextures(1, &sentinel);
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(oldUnpackBuffer));
        gl->glPixelStorei(GL_PACK_ROW_LENGTH, oldPackRowLength);
        gl->glClearDepthf(oldClearDepth);
        gl->glActiveTexture(GL_TEXTURE0); gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(oldRenderbuffer));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFbo));
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFbo));
        gl->glReadBuffer(static_cast<GLenum>(oldReadBuffer));
        gl->glActiveTexture(static_cast<GLenum>(oldUnit));
      }
      if (previous && previousSurface) previous->makeCurrent(previousSurface); else context->doneCurrent();
    });
    // Allocate the caller's small FBO with no unpack PBO bound. Only after it
    // is complete do we install intentionally nondefault, valid caller state.
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    QOpenGLFramebufferObjectFormat callerFormat;
    callerFormat.setSamples(0);
    callerFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    callerFormat.setInternalTextureFormat(GL_RGBA8);
    callerFramebuffer = std::make_unique<QOpenGLFramebufferObject>(QSize(4, 4), callerFormat);
    if (!check(callerFramebuffer->isValid() && callerFramebuffer->bind() &&
               gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
               "complete custom caller framebuffer before mixed click")) return false;
    const GLuint callerFbo = callerFramebuffer->handle();
    gl->glGenBuffers(1, &unpack); gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
    gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, 16, nullptr, GL_STATIC_DRAW);
    gl->glGenTextures(1, &sentinel); gl->glBindTexture(GL_TEXTURE_2D, sentinel);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 1);
    gl->glClearDepthf(0.0F);
    if (!check(sentinel && gl->glIsTexture(sentinel) && unpack && gl->glIsBuffer(unpack),
               "valid texture and unpack-buffer sentinels before mixed click")) return false;
    mouse(widget, point);
    const bool sameContext = QOpenGLContext::currentContext() == context;
    const bool sameSurface = sameContext && context->surface() == surface;
    GLint unit = -1, texture = -1, drawFbo = -1, readFbo = -1, packRowLength = -1, unpackBuffer = -1;
    GLfloat clearDepth = -1.0F;
    if (sameContext) {
      gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &unit); gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
      gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo); gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
      gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength); gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
      gl->glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
    }
    qInfo() << "MIXED_GAUSSIAN_STATE_CONTRACT" << config << pose << route << kind
            << "context/surface" << sameContext << sameSurface << "active" << unit << "texture" << sentinel << texture
            << "caller/draw/read-fbo" << callerFbo << drawFbo << readFbo << "pack-row" << packRowLength
            << "clear-depth" << clearDepth << "unpack-before/after" << unpack << unpackBuffer;
    return check(backgroundSafe() && sameContext && sameSurface && unit == GL_TEXTURE0 && texture == static_cast<GLint>(sentinel) &&
                 drawFbo == static_cast<GLint>(callerFbo) && readFbo == static_cast<GLint>(callerFbo) &&
                 packRowLength == 1 && clearDepth == 0.0F && unpackBuffer == static_cast<GLint>(unpack),
                 "mixed scene selection preserves caller framebuffer, pack/unpack, clear depth, context and texture before repaint");
  };
  for (int config = 0; config < 3; ++config) {
    QString error;
    const QString working = temporary.filePath(QString("workspace-%1").arg(config));
    if (!check(QDir().mkpath(working) && document->createUntitled(working, "Mixed Gaussian isolation", &error), "create isolated empty project")) return false;
    QStringList ids;
    for (int kind = 0; kind < 4; ++kind) {
      QTimer answer;
      bool appended = false, unexpected = false;
      QObject::connect(&answer, &QTimer::timeout, &window, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
          auto *box = qobject_cast<QMessageBox *>(widget);
          if (!box || !box->isVisible()) continue;
          auto *button = box->findChild<QAbstractButton *>("appendSceneButton");
          if (box->objectName() == QStringLiteral("sceneImportChoiceDialog") && button) {
            appended = true; button->click();
          } else { unexpected = true; box->reject(); }
        }
      });
      if (kind && !backgroundRun) { answer.setInterval(10); answer.start(); }
      // The two-argument MainWindow append overload is private. Background QA
      // invokes its public document mutation instead of exposing a dialog;
      // ordinary remote acceptance still exercises the actual confirmation.
      const bool imported = backgroundRun && kind ? document->addScenePath(paths[kind], &error) : window.importSceneFile(paths[kind]);
      answer.stop();
      if (!check(backgroundSafe() && imported && !unexpected && (!kind || backgroundRun || appended),
                 "import completes through actual confirmation or isolated background append core")) return false;
      ids.append(document->activeSceneId());
      if (!check(waitUntil([&] {
            (void)viewport->grabFramebuffer();
            return kind == 1 ? viewport->meshRenderingAvailable() : kind >= 2 ? viewport->gaussianRenderingAvailable() : viewport->selectableModelAvailable();
          }), "each imported source reaches its own renderer")) return false;
      if (kind >= 2 && !check(viewport->sourceShDegree() == (kind == 2 ? 3 : 1) &&
                  viewport->sourceHasSurfelAttributes() == (kind == 3) && viewport->renderedPointCount() == (kind == 2 ? 16384 : 4096),
                  "unequal Gaussian counts, SH degrees and real surfel attributes remain source-owned")) return false;
    }
    viewport->setAxisView(NavigationAxis::PositiveZ);
    waitUntil([&] { return false; }, 400);
    viewport->selectAllModels();
    if (!check(viewport->focusModel(), "fit all imported geometry once")) return false;
    viewport->clearSelection();
    const auto initial = document->sceneObjects();
    for (int pose = 0; pose < 2; ++pose) {
      if (pose) {
        lock->setChecked(true);
        const QPointF start(viewport->width() * 0.15, viewport->height() * 0.75), end = start + QPointF(37, 19);
        QMouseEvent down(QEvent::MouseButtonPress, start, viewport->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, end, viewport->mapToGlobal(end.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, end, viewport->mapToGlobal(end.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &down); QApplication::sendEvent(viewport, &move); QApplication::sendEvent(viewport, &up);
        lock->setChecked(false);
      }
      viewport->clearSelection();
      if (!check(document->setSceneObjectVisibility(ids, false), "geometry-free reference hides every layer")) return false;
      const QImage empty = viewport->grabFramebuffer();
      if (!check(!empty.isNull() && document->setSceneObjectVisibility(ids, true), "restore all sources before the selection replay")) return false;
      // Construct all-resident, all-compatibility and a genuinely mixed pair
      // after any visibility-driven resource reload, using public mode APIs.
      for (int kind : {2, 3}) {
        const bool indexed = config == 0 || (config == 2 && kind == 2);
        if (indexed) qunsetenv("GSW_DISABLE_INDEXED_GAUSSIANS"); else qputenv("GSW_DISABLE_INDEXED_GAUSSIANS", "1");
        if (!check(document->activateSceneObject(ids[kind]), "activate renderer-path fixture")) return false;
        viewport->setRenderMode(NativeViewport::RenderMode::Points);
        viewport->setRenderMode(NativeViewport::RenderMode::Gaussians);
        if (!check(waitUntil([&] { (void)viewport->grabFramebuffer(); return viewport->indexedGaussianRendering() == indexed && viewport->gaussianRenderingAvailable(); }),
                   "required resident/compatibility path is actually active")) return false;
      }
      viewport->clearSelection();
      QImage baseline;
      std::array<Patch, 4> body;
      if (!check(waitUntil([&] { baseline = viewport->grabFramebuffer(); body = patches(baseline);
              return std::all_of(body.begin(), body.end(), [](const Patch &p) { return p.count > 120; }); }),
                 "all four distinct source bodies render before selection")) return false;
      const QString stem = QString("config-%1-pose-%2").arg(config).arg(pose);
      if (!baseline.save(QDir(evidence).filePath(stem + "-all.png"))) return false;
      for (int route = 0; route < 2; ++route) for (int kind : {2, 0, 1, 3}) {
        viewport->clearSelection();
        QPointF point;
        QWidget *receiver = viewport;
        if (route == 0) {
          auto *item = sceneItem(tree, ids[kind]); if (!check(item, "actual tree row exists")) return false;
          tree->scrollToItem(item); receiver = tree->viewport();
          point = tree->visualRect(tree->indexFromItem(item, 0)).center();
        } else point = body[kind].center / viewport->devicePixelRatioF();
        if (!stateClick(receiver, point, config, pose, route, kind)) return false;
        const QImage selected = viewport->grabFramebuffer();
        const QString selectedStem = stem + QString("-route-%1-source-%2").arg(route).arg(kind);
        if (!selected.save(QDir(evidence).filePath(selectedStem + "-selected.png"))) return false;
        if (!check(viewport->selectedSceneIds() == QStringList{ids[kind]} && viewport->visibleSceneObjectCount() == 4,
                   "selection changes only the intended active source, not visibility")) return false;
        const auto hint = viewport->selectionHintBounds(); const qreal ratio = viewport->devicePixelRatioF();
        const QRect hintPixels = QRectF(hint.x() * ratio, hint.y() * ratio, hint.width() * ratio,
                                         hint.height() * ratio).toAlignedRect().adjusted(-3, -3, 3, 3);
        for (int peer = 0; peer < 4; ++peer) {
          const auto bounds = body[peer].bounds; const int strip = std::max(1, bounds.height() / 5);
          QRegion sample(QRect(bounds.left(), bounds.top(), bounds.width(), strip));
          sample += QRect(bounds.left(), bounds.bottom() - strip + 1, bounds.width(), strip); sample -= hintPixels;
          int expected = 0, retained = 0; Patch beforePatch, afterPatch;
          for (const auto &region : sample) {
            if (peer == kind) {
              for (int y = region.top(); y <= region.bottom(); ++y) for (int x = region.left(); x <= region.right(); ++x) {
                const auto base = baseline.pixelColor(x, y), after = selected.pixelColor(x, y), background = empty.pixelColor(x, y);
                const auto distance = [&](const QColor &color) { return std::max({std::abs(color.red() - background.red()),
                      std::abs(color.green() - background.green()), std::abs(color.blue() - background.blue())}); };
                if (distance(base) > 45) { ++expected; retained += distance(after) > 30; }
              }
            } else {
              const auto before = patches(baseline, region)[peer], after = patches(selected, region)[peer];
              beforePatch.center += before.center * before.count; beforePatch.count += before.count;
              afterPatch.center += after.center * after.count; afterPatch.count += after.count;
            }
          }
          if (peer == kind) {
            qInfo() << "MIXED_GAUSSIAN_PIXELS" << config << pose << route << kind << peer << "selected-before/retained" << expected << retained;
            if (!check(expected > 40 && retained >= expected * 85 / 100, "selected source retains its real body, not merely bounds or gizmo")) return false;
          } else {
            if (beforePatch.count) beforePatch.center /= beforePatch.count;
            if (afterPatch.count) afterPatch.center /= afterPatch.count;
            qInfo() << "MIXED_GAUSSIAN_PIXELS" << config << pose << route << kind << peer << "peer-before/after" << beforePatch.count << afterPatch.count;
            if (!check(beforePatch.count > 40 && afterPatch.count >= beforePatch.count * 9 / 10 &&
                       afterPatch.count <= beforePatch.count * 11 / 10 && QLineF(beforePatch.center, afterPatch.center).length() < 1.0,
                       "unselected peer body retains its source color, position and size")) return false;
          }
        }
        viewport->clearSelection();
        const QImage cleared = viewport->grabFramebuffer();
        if (!cleared.save(QDir(evidence).filePath(selectedStem + "-cleared.png"))) return false;
        const auto restored = patches(cleared);
        for (int peer = 0; peer < 4; ++peer) if (!check(restored[peer].count >= body[peer].count * 9 / 10 &&
              restored[peer].count <= body[peer].count * 11 / 10 && QLineF(restored[peer].center, body[peer].center).length() < 1.0,
              "deselection restores all source bodies without reimport")) return false;
        const auto after = document->sceneObjects();
        for (int peer = 0; peer < 4; ++peer) if (!check(after[peer].path == initial[peer].path && after[peer].visible &&
              after[peer].translation == initial[peer].translation && after[peer].rotation == initial[peer].rotation &&
              after[peer].scale == initial[peer].scale, "click does not rewrite source transforms, paths or visibility")) return false;
      }
    }
    for (int kind = 0; kind < 4; ++kind) if (!check(sha256(paths[kind]) == hashes[kind], "all source files remain byte-for-byte unchanged")) return false;
  }
  QString error;
  if (!document->createUntitled(temporary.path(), "Empty isolation", &error)) return false;
  qInfo() << "MIXED_GAUSSIAN_ISOLATION PASS: distinct counts/SH, all resident/all compatibility/mixed paths, real source clicks, pixels and immutable sources";
  return true;
}

bool runRecordedInputRegression(MainWindow &window, const QString &path) {
  const auto check = [](bool ok, const char *message) {
    if (!ok) qCritical() << "GAUSSIAN_CLICK_DIAG FAIL:" << message;
    return ok;
  };
  const QFileInfo source(path);
  if (!check(source.isAbsolute() && source.isFile() && !source.isSymLink() &&
             source.suffix().compare("ply", Qt::CaseInsensitive) == 0,
             "private input is an existing absolute regular PLY")) return false;
  const auto originalHash = sha256(path);
  if (!check(!originalHash.isEmpty(), "hash complete private input before diagnosis")) return false;
  const bool passed = [&]() -> bool {
    auto *viewport = window.findChild<NativeViewport *>();
    auto *document = window.findChild<WorkspaceDocument *>();
    auto *tree = window.findChild<QTreeWidget *>("projectTree");
    const QString root = QDir(QCoreApplication::applicationDirPath()).filePath("test-settings");
    if (!QDir().mkpath(root)) return false;
    QTemporaryDir temporary(QDir(root).filePath("private-click-XXXXXX"));
    const auto isolated = [&] {
      if (qEnvironmentVariable("GSW_SMOKE_BACKGROUND") != QStringLiteral("1")) return true;
      bool intersects = false;
      for (const auto *screen : QGuiApplication::screens())
        intersects |= window.frameGeometry().intersects(screen->geometry());
      const bool active = window.isActiveWindow();
      if (!intersects && !active) return true;
      window.hide();
      qCritical() << "GAUSSIAN_CLICK_DIAG FAIL: private-input window isolation violated; hidden"
                  << "intersects-screen" << intersects << "active" << active;
      return false;
    };
    if (!check(viewport && document && tree && temporary.isValid() && isolated(), "isolated private-input services")) return false;
    window.resize(1440, 900);
    if (!check(isolated(), "resized private-input window remains outside every screen")) return false;
    auto *lock = window.findChild<QAction *>("lockEditToolsAction");
    if (!check(lock, "private-input edit lock action")) return false;
    lock->setChecked(false);
    viewport->setShowCameras(false);
    viewport->setShowObservationTrackball(false);
    viewport->setZoomToCursorEnabled(false);
    QString error;
    const QString workspace = temporary.filePath("workspace");
    if (!check(QDir().mkpath(workspace) && document->createUntitled(workspace, "Private click diagnosis", &error),
               "create isolated private-input workspace")) return false;
    const bool importCore = qEnvironmentVariable("GSW_GAUSSIAN_CLICK_IMPORT_CORE") == QStringLiteral("1");
    qInfo() << "GAUSSIAN_CLICK_DIAG import-core" << importCore;
    if (!check(importCore ? window.importSceneFile(path) : document->addScenePath(path, &error),
               "import private input without modifying its source")) return false;
    if (!check(waitUntil([&] {
          (void)viewport->grabFramebuffer();
          return viewport->gaussianRenderingAvailable() && viewport->renderedPointCount() > 0;
        }, 30000) && isolated(), "private Gaussian input loaded in isolated window")) return false;
    viewport->makeCurrent();
    auto *context = viewport->context();
    if (!context || context->isOpenGLES()) {
      viewport->doneCurrent();
      return check(false, "private diagnosis requires actual desktop OpenGL");
    }
    auto *gl = context->extraFunctions();
    qInfo() << "GAUSSIAN_CLICK_DIAG private desktop OpenGL"
            << "vendor" << reinterpret_cast<const char *>(gl->glGetString(GL_VENDOR))
            << "renderer" << reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER))
            << "version" << reinterpret_cast<const char *>(gl->glGetString(GL_VERSION))
            << "vertices" << viewport->renderedPointCount() << "SH" << viewport->sourceShDegree()
            << "indexed" << viewport->indexedGaussianRendering();
    viewport->doneCurrent();
    viewport->setRenderMode(NativeViewport::RenderMode::Gaussians);
    viewport->setInteractionMode(NativeViewport::InteractionMode::Inspect);
    viewport->resetCamera();
    if (!check(viewport->focusModel(), "frame private input once before replay")) return false;
    viewport->clearSelection();
    const auto object = document->sceneObjects().first();
    const auto sourceCount = viewport->renderedPointCount();
    const QString evidence = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
    if (!check(!evidence.isEmpty() && QDir().mkpath(evidence), "explicit private evidence location")) return false;
    const bool paintEventReadback = qEnvironmentVariable("GSW_GAUSSIAN_CLICK_PAINT_EVENT") == QStringLiteral("1");
    qInfo() << "GAUSSIAN_CLICK_DIAG readback" << (paintEventReadback ? "QPaintEvent-existing-FBO" : "grabFramebuffer");
    const auto existingFramebuffer = [&]() -> QImage {
      auto *previousContext = QOpenGLContext::currentContext();
      auto *previousSurface = previousContext ? previousContext->surface() : nullptr;
      auto *readContext = viewport->context();
      if (!readContext) return {};
      if (previousContext != readContext) viewport->makeCurrent();
      if (QOpenGLContext::currentContext() != readContext) return {};
      auto *functions = readContext->extraFunctions();
      GLint drawFramebuffer = 0, readFramebuffer = 0, readBuffer = 0, packBuffer = 0, unpackBuffer = 0;
      GLint viewportState[4] = {}, activeTexture = GL_TEXTURE0, texture2d = 0, renderbuffer = 0;
      GLint packAlignment = 4, packRowLength = 0, packSkipRows = 0, packSkipPixels = 0;
      functions->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
      functions->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
      functions->glGetIntegerv(GL_READ_BUFFER, &readBuffer);
      functions->glGetIntegerv(GL_VIEWPORT, viewportState);
      functions->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
      functions->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
      functions->glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
      functions->glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength);
      functions->glGetIntegerv(GL_PACK_SKIP_ROWS, &packSkipRows);
      functions->glGetIntegerv(GL_PACK_SKIP_PIXELS, &packSkipPixels);
      functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
      functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2d);
      functions->glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
      const GLboolean scissor = functions->glIsEnabled(GL_SCISSOR_TEST);
      const auto restore = qScopeGuard([&] {
        functions->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
        functions->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
        functions->glReadBuffer(static_cast<GLenum>(readBuffer));
        functions->glViewport(viewportState[0], viewportState[1], viewportState[2], viewportState[3]);
        functions->glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        functions->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
        functions->glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
        functions->glPixelStorei(GL_PACK_ROW_LENGTH, packRowLength);
        functions->glPixelStorei(GL_PACK_SKIP_ROWS, packSkipRows);
        functions->glPixelStorei(GL_PACK_SKIP_PIXELS, packSkipPixels);
        functions->glActiveTexture(static_cast<GLenum>(activeTexture));
        functions->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2d));
        functions->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
        if (scissor) functions->glEnable(GL_SCISSOR_TEST); else functions->glDisable(GL_SCISSOR_TEST);
        if (previousContext && previousSurface) previousContext->makeCurrent(previousSurface);
        else readContext->doneCurrent();
      });
      functions->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      functions->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      functions->glPixelStorei(GL_PACK_ALIGNMENT, 4);
      functions->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
      functions->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
      functions->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
      functions->glDisable(GL_SCISSOR_TEST);
      const QSize pixels(qRound(viewport->width() * viewport->devicePixelRatioF()),
                         qRound(viewport->height() * viewport->devicePixelRatioF()));
      QImage result;
      {
        QOpenGLFramebufferObjectFormat format;
        format.setSamples(0);
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        format.setInternalTextureFormat(GL_RGBA8);
        QOpenGLFramebufferObject resolved(pixels, format);
        if (!resolved.isValid()) return {};
        functions->glBindFramebuffer(GL_READ_FRAMEBUFFER, viewport->defaultFramebufferObject());
        functions->glReadBuffer(GL_COLOR_ATTACHMENT0);
        functions->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolved.handle());
        if (!check(functions->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                   functions->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                   "existing-frame readback requires complete source and resolve framebuffers")) return {};
        functions->glBlitFramebuffer(0, 0, pixels.width(), pixels.height(), 0, 0, pixels.width(), pixels.height(),
                                     GL_COLOR_BUFFER_BIT, GL_NEAREST);
        // toImage() may skip bind() when the draw target is already this FBO.
        // Its readPixels must use the resolved target, not the MSAA source.
        functions->glBindFramebuffer(GL_FRAMEBUFFER, resolved.handle());
        result = resolved.toImage();
      }
      result.setDevicePixelRatio(viewport->devicePixelRatioF());
      return result;
    };
    const auto frame = [&](const int frameCount = 3) {
      QImage result;
      // The ordinary UI may keep the first input-triggered frame while idle.
      // Do not let extra forced paints repair a bad first selection frame.
      for (int pass = 0; pass < frameCount; ++pass) {
        if (!isolated()) return QImage();
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
        if (paintEventReadback) {
          const auto serial = viewport->renderedFrameSerial();
          QPaintEvent event(viewport->rect());
          QApplication::sendEvent(viewport, &event);
          if (!check(viewport->renderedFrameSerial() > serial,
                     "normal QPaintEvent must actually render before existing-FBO readback")) return QImage();
          result = existingFramebuffer();
        } else {
          result = viewport->grabFramebuffer();
        }
      }
      return isolated() ? result : QImage();
    };
    const auto wheel = [&](int delta) {
      const QPointF center = viewport->rect().center();
      QWheelEvent event(center, viewport->mapToGlobal(center.toPoint()), {}, QPoint(0, delta),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(viewport, &event);
    };
    std::array<QImage, 2> empty;
    for (int view = 0; view < 2; ++view) {
      if (view) wheel(-480);
      if (!document->setSceneObjectVisibility({object.id}, false)) return false;
      empty[view] = frame();
      if (!check(!empty[view].isNull() && document->setSceneObjectVisibility({object.id}, true),
                 "precollect geometry-free reference before the uninterrupted input sequence")) return false;
    }
    wheel(480);
    const auto mouse = [&](QWidget *widget, const QPointF &point) {
      if (!isolated() || !window.windowHandle()) return false;
      const QPointF windowPoint = widget->mapTo(&window, point.toPoint());
      const QPointF globalPoint = widget->mapToGlobal(point.toPoint());
      QEnterEvent enter(windowPoint, windowPoint, globalPoint);
      QMouseEvent move(QEvent::MouseMove, windowPoint, windowPoint, globalPoint,
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QMouseEvent down(QEvent::MouseButtonPress, windowPoint, windowPoint, globalPoint,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent up(QEvent::MouseButtonRelease, windowPoint, windowPoint, globalPoint,
                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      // Route through the native window's QWidgetWindow dispatcher, not
      // directly to a child. Qt resolves the child and its hover/focus state
      // while every logical and global position remains outside the desktop.
      QApplication::sendEvent(window.windowHandle(), &enter);
      QApplication::sendEvent(window.windowHandle(), &move);
      QApplication::sendEvent(window.windowHandle(), &down);
      QApplication::sendEvent(window.windowHandle(), &up);
      return isolated();
    };
    const auto retained = [&](const QImage &before, const QImage &after, const QImage &background,
                              const QString &stage) {
      if (before.isNull() || after.isNull() || background.isNull()) return false;
      const QRect body(35, 120, before.width() - 70, before.height() - 230);
      const auto model = coverage(before, background, body);
      const int strip = std::max(1, model.bounds.height() / 5);
      QRegion sample(QRect(model.bounds.left(), model.bounds.top(), model.bounds.width(), strip));
      sample += QRect(model.bounds.left(), model.bounds.bottom() - strip + 1, model.bounds.width(), strip);
      const auto hint = viewport->selectionHintBounds();
      const auto ratio = viewport->devicePixelRatioF();
      sample -= QRectF(hint.x() * ratio, hint.y() * ratio, hint.width() * ratio,
                       hint.height() * ratio).toAlignedRect().adjusted(-3, -3, 3, 3);
      int expected = 0, actual = 0;
      for (const auto &region : sample) {
        for (int y = region.top(); y <= region.bottom(); ++y) {
          for (int x = region.left(); x <= region.right(); ++x) {
            const auto base = before.pixelColor(x, y), selected = after.pixelColor(x, y), clear = background.pixelColor(x, y);
            const auto difference = [&](const QColor &color) {
              return std::max({std::abs(color.red() - clear.red()), std::abs(color.green() - clear.green()),
                               std::abs(color.blue() - clear.blue())});
            };
            if (difference(base) > 45) { ++expected; actual += difference(selected) > 30; }
          }
        }
      }
      qInfo() << "GAUSSIAN_CLICK_DIAG coverage" << stage << "before/retained" << expected << actual;
      return check(expected > 300 && actual >= expected * 85 / 100,
                   "selected private model retains its rendered body outside actual selection overlays");
    };
    for (int view = 0; view < 2; ++view) {
      if (view) wheel(-480);
      viewport->clearSelection();
      const QImage before = frame();
      const QString prefix = QString("private-view-%1").arg(view);
      if (!check(!before.isNull() && before.save(QDir(evidence).filePath(prefix + "-before.png")), "save private-input baseline")) return false;
      const QRect body(35, 120, before.width() - 70, before.height() - 230);
      const auto model = coverage(before, empty[view], body);
      if (!check(model.count > 1000, "private input has substantial actual foreground before clicking")) return false;
      const auto cameraTarget = viewport->viewTarget();
      const auto cameraDistance = viewport->viewDistance();
      const auto cameraAngles = viewport->viewOrbitAngles();
      auto *item = sceneItem(tree, object.id);
      if (!check(item, "private input appears in the actual scene tree")) return false;
      tree->scrollToItem(item);
      if (!check(mouse(tree->viewport(), tree->visualRect(tree->indexFromItem(item, 0)).center()), "real tree-name control click")) return false;
      const QImage treeSelected = frame(paintEventReadback ? 1 : 3);
      if (!check(viewport->modelSelected() && treeSelected.save(QDir(evidence).filePath(prefix + "-tree-selected.png")) &&
                 retained(before, treeSelected, empty[view], prefix + "-tree"), "tree control preserves the actual private model")) return false;
      viewport->clearSelection();
      (void)frame();
      QList<QPointF> candidates{model.center};
      for (int y = model.bounds.top() + 2; y <= model.bounds.bottom() - 2; y += std::max(3, model.bounds.height() / 18)) {
        for (int x = model.bounds.left() + 2; x <= model.bounds.right() - 2; x += std::max(3, model.bounds.width() / 18)) {
          const auto actual = before.pixelColor(x, y), clear = empty[view].pixelColor(x, y);
          if (std::max({std::abs(actual.red() - clear.red()), std::abs(actual.green() - clear.green()),
                        std::abs(actual.blue() - clear.blue())}) > 60) candidates.append(QPointF(x, y));
        }
      }
      std::stable_sort(candidates.begin() + 1, candidates.end(), [&](const auto &a, const auto &b) {
        return QLineF(a, model.center).length() < QLineF(b, model.center).length();
      });
      for (int attempt = 0; attempt < std::min(9, static_cast<int>(candidates.size())) && !viewport->modelSelected(); ++attempt) {
        if (!mouse(viewport, candidates[attempt] / viewport->devicePixelRatioF())) return false;
      }
      const QImage selected = frame(paintEventReadback ? 1 : 3);
      item = sceneItem(tree, object.id);
      if (!check(selected.save(QDir(evidence).filePath(prefix + "-viewport-selected.png")) &&
                 viewport->modelSelected() && viewport->selectedSceneIds() == QStringList{object.id} &&
                 item && item->isSelected() && item->checkState(1) == Qt::Checked,
                 "real foreground viewport click selects the private input while its eye remains open")) return false;
      if (!retained(before, selected, empty[view], prefix + "-viewport")) return false;
      if (!check(viewport->viewTarget() == cameraTarget && viewport->viewDistance() == cameraDistance &&
                 viewport->viewOrbitAngles() == cameraAngles && viewport->renderedPointCount() == sourceCount &&
                 viewport->modelTranslation() == object.translation && viewport->modelRotation() == object.rotation &&
                 viewport->modelScale() == object.scale && !viewport->hasUnsavedSceneEdits(),
                 "viewport selection preserves private model, count and camera")) return false;
      viewport->clearSelection();
      const QImage cleared = frame();
      if (!check(cleared.save(QDir(evidence).filePath(prefix + "-cleared.png")) &&
                 retained(before, cleared, empty[view], prefix + "-cleared"), "deselection restores the complete private model")) return false;
    }
    if (!document->createUntitled(temporary.path(), "Empty private diagnosis", &error)) return false;
    qInfo() << "GAUSSIAN_CLICK_DIAG PASS: private input tree control, real viewport click, zoom and source invariants";
    return true;
  }();
  const auto finalHash = sha256(path);
  qInfo() << "GAUSSIAN_CLICK_DIAG source SHA256 before/after" << originalHash.toHex() << finalHash.toHex();
  return check(originalHash == finalHash, "complete private input remains byte-for-byte unchanged") && passed;
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
  const QString initialActiveId = document->activeSceneId();
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
      if (!viewport->setSceneSelection({found->id}, found->id)) return false;
    }
    paths.append(path);
    if (!waitUntil([&] {
          (void)viewport->grabFramebuffer();
          return viewport->sceneCoordinates().valid && viewport->selectableModelAvailable();
        }, 30000)) return false;
  }
  if (!initialActiveId.isEmpty() && !viewport->setSceneSelection({initialActiveId}, initialActiveId)) return false;
  auto *lockTools = window.findChild<QAction *>("lockEditToolsAction");
  if (!lockTools) return false;
  lockTools->setChecked(false);
  viewport->selectAllModels();
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

bool runLastVisibleEyeRegression(MainWindow &window) {
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  auto *lockTools = window.findChild<QAction *>("lockEditToolsAction");
  QTemporaryDir temporary;
  const auto check = [](bool success, const char *message) {
    if (!success) qCritical() << "HIDE_VIEW_REGRESSION FAIL:" << message;
    return success;
  };
  if (!check(viewport && document && tree && lockTools && temporary.isValid(), "public window services")) return false;
  window.resize(1440, 900);
  QString error;
  const QString working = QDir(temporary.path()).filePath("working");
  const QString path = QDir(temporary.path()).filePath("last-visible-3dgs.ply");
  if (!check(QDir().mkpath(working) && document->createUntitled(working, "Hide view regression", &error) &&
      fixture(path, 2) && document->addScenePath(path, &error), "create independent Gaussian fixture")) return false;
  lockTools->setChecked(false);
  if (!check(waitUntil([&] { return viewport->gaussianRenderingAvailable(); }) && viewport->focusModel(),
             "frame the only visible Gaussian in perspective")) return false;
  viewport->clearSelection();
  const QString id = document->activeSceneId();
  const QByteArray sourceHash = sha256(path);
  const QString evidence = QDir(qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", QDir::tempPath())).filePath(
      "hide-view-" + AppLanguage::current() + (AppTheme::currentTheme() == UiTheme::Light ? "-light" : "-dark"));
  QDir().mkpath(evidence);
  const QImage before = viewport->grabFramebuffer();
  before.save(QDir(evidence).filePath("before-hide.png"));
  const QRect viewportRect(viewport->mapToGlobal(QPoint()), viewport->size());
  const auto target = viewport->viewTarget();
  const auto distance = viewport->viewDistance();
  const auto angles = viewport->viewOrbitAngles();
  const auto projection = viewport->orthographicProjection();
  const auto fov = viewport->perspectiveFovDegrees();
  const auto eyeClick = [&] {
    auto *item = sceneItem(tree, id);
    if (!item) return false;
    tree->scrollToItem(item);
    const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2,
                       tree->visualItemRect(item).center().y());
    if (tree->indexAt(point).column() != 1 || tree->itemAt(point) != item) return false;
    QMouseEvent down(QEvent::MouseButtonPress, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &down);
    QApplication::sendEvent(tree->viewport(), &up);
    return true;
  };
  const quint64 frameSerial = viewport->renderedFrameSerial();
  if (!check(eyeClick() && waitUntil([&] {
        return !viewport->sceneObjectVisible(id) && viewport->renderedFrameSerial() > frameSerial;
      }), "real eye click hides the last visible object and presents its empty frame")) return false;
  const QImage hidden = viewport->grabFramebuffer();
  hidden.save(QDir(evidence).filePath("after-hide.png"));
  const QRect hiddenRect(viewport->mapToGlobal(QPoint()), viewport->size());
  qInfo() << "HIDE_VIEW_REGRESSION viewport before/hidden" << viewportRect << hiddenRect
          << "camera" << target << viewport->viewTarget() << distance << viewport->viewDistance();
  if (!check(hiddenRect == viewportRect && hidden.size() == before.size(),
             "hiding the last object preserves the actual viewport rectangle and projection canvas")) return false;
  if (!check(viewport->viewTarget() == target && viewport->viewDistance() == distance &&
      viewport->viewOrbitAngles() == angles && viewport->orthographicProjection() == projection &&
      viewport->perspectiveFovDegrees() == fov, "eye toggle preserves the observed camera and projection")) return false;
  if (!check(eyeClick() && waitUntil([&] { return viewport->sceneObjectVisible(id) && viewport->gaussianRenderingAvailable(); }),
             "same eye reveals the original model without reimport")) return false;
  viewport->grabFramebuffer().save(QDir(evidence).filePath("after-restore.png"));
  if (!check(QRect(viewport->mapToGlobal(QPoint()), viewport->size()) == viewportRect &&
      viewport->viewTarget() == target && viewport->viewDistance() == distance && viewport->viewOrbitAngles() == angles &&
      document->activeSceneId() == id && sha256(path) == sourceHash,
      "restoring visibility retains the view, source identity and original bytes")) return false;
  return check(document->createUntitled(temporary.path(), "After hide regression", &error), "release the owned fixture");
}
}

bool runSceneVisibilitySmokeTest(MainWindow &window) {
  if (qEnvironmentVariable("GSW_RENDER_DIAGNOSTICS_SMOKE") == QStringLiteral("1")) return runRenderDiagnosticsSmoke(window);
  if (qEnvironmentVariable("GSW_MULTI_GAUSSIAN_ISOLATION") == QStringLiteral("1")) return runMixedGaussianIsolation(window);
  const QString privateInput = qEnvironmentVariable("GSW_GAUSSIAN_CLICK_INPUT");
  if (!privateInput.isEmpty()) return runRecordedInputRegression(window, privateInput);
  if (qEnvironmentVariable("GSW_SCENE_SELECTION_LARGE_SYNTHETIC") == QStringLiteral("1"))
    return runLargeSelectionRegression(window);
  const QByteArray inputJson = qgetenv("GSW_SCENE_SELECTION_INPUTS_JSON");
  if (!inputJson.isEmpty()) {
    QJsonParseError error;
    const auto inputs = QJsonDocument::fromJson(inputJson, &error);
    return error.error == QJsonParseError::NoError && inputs.isArray() &&
        runInputSelectionDiagnostic(window, inputs.array());
  }
  if (!runLastVisibleEyeRegression(window)) return false;
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
