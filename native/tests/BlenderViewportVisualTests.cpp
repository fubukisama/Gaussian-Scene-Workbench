#include "BlenderViewportVisualTests.h"

#include "AppTheme.h"
#include "NativeViewport.h"
#include "NavigationGizmo.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {

void processFrames(const int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

bool saveEvidence(const QImage &frame, const QString &name) {
  const QString configured = qEnvironmentVariable("GSW_BLENDER_VIEWPORT_EVIDENCE");
  if (configured.isEmpty()) return true;
  const QString directory = QDir::cleanPath(QFileInfo(configured).absoluteFilePath());
  if (!directory.startsWith(QStringLiteral("D:/"), Qt::CaseInsensitive)) {
    qCritical() << "Viewport evidence must remain on D:" << directory;
    return false;
  }
  QFileInfo ancestor(directory);
  while (!ancestor.exists()) ancestor.setFile(ancestor.absolutePath());
  if (!ancestor.canonicalFilePath().startsWith(QStringLiteral("D:/"),
                                               Qt::CaseInsensitive) ||
      !QDir().mkpath(directory)) {
    qCritical() << "Unable to create safe D: viewport evidence directory";
    return false;
  }
  return frame.save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

void clickViewport(NativeViewport &viewport, const QPointF &position) {
  const QPointF global(viewport.mapToGlobal(position.toPoint()));
  QMouseEvent press(QEvent::MouseButtonPress, position, position, global,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, position, position, global,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&viewport, &press);
  QCoreApplication::sendEvent(&viewport, &release);
}

struct AxisPixels final {
  int red = 0;
  int green = 0;
  int blue = 0;
};

AxisPixels groundAxisPixels(const QImage &frame) {
  AxisPixels result;
  // Observe the long world-origin axes, not the small reference triad or the
  // navigation gizmo. The rectangles are screen regions, not shader internals.
  const QRect sample(qRound(frame.width() * 0.20),
                     qRound(frame.height() * 0.20),
                     qRound(frame.width() * 0.60),
                     qRound(frame.height() * 0.60));
  const QPointF center(frame.width() * 0.5, frame.height() * 0.5);
  const qreal excludedRadius = frame.height() * 0.13;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QPointF fromCenter = QPointF(x, y) - center;
      if (QPointF::dotProduct(fromCenter, fromCenter) <
          excludedRadius * excludedRadius) continue;
      const QColor pixel = frame.pixelColor(x, y);
      if (pixel.red() > pixel.green() + 8 &&
          pixel.red() > pixel.blue() + 8) ++result.red;
      if (pixel.green() > pixel.red() + 8 &&
          pixel.green() > pixel.blue() + 8) ++result.green;
      if (pixel.blue() > pixel.red() + 8 &&
          pixel.blue() > pixel.green() + 8) ++result.blue;
    }
  }
  return result;
}

double neutralGridInk(const QImage &frame) {
  const QRect sample(qRound(frame.width() * 0.18),
                     qRound(frame.height() * 0.22),
                     qRound(frame.width() * 0.64),
                     qRound(frame.height() * 0.56));
  const QPoint center(frame.width() / 2, frame.height() / 2);
  double ink = 0.0;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      // Keep the measurement independent of world-axis colour and the origin
      // marker. The remaining region contains only the neutral ground grid.
      if (std::abs(x - center.x()) <= 12 ||
          std::abs(y - center.y()) <= 12) continue;
      const QColor pixel = frame.pixelColor(x, y);
      const int high = std::max({pixel.red(), pixel.green(), pixel.blue()});
      const int low = std::min({pixel.red(), pixel.green(), pixel.blue()});
      if (high - low > 12) continue;
      ink += std::max(0.0, (pixel.red() + pixel.green() + pixel.blue()) / 3.0 - 13.0);
    }
  }
  return ink;
}

void wheelViewport(NativeViewport &viewport, const int angleDelta) {
  const QPointF center(viewport.width() * 0.5, viewport.height() * 0.5);
  const QPointF global(viewport.mapToGlobal(center.toPoint()));
  QWheelEvent wheel(center, global, QPoint(), QPoint(0, angleDelta),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(&viewport, &wheel);
}

QColor unobstructedBackdrop(const QImage &frame) {
  // A median rejects the few thin neutral grid strokes in this background
  // sample without masking or changing the viewport's actual rendering.
  const QRect sample(qRound(frame.width() * 0.40),
                     qRound(frame.height() * 0.08),
                     qRound(frame.width() * 0.20),
                     qRound(frame.height() * 0.06));
  std::array<std::vector<int>, 3> channels;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      channels[0].push_back(pixel.red());
      channels[1].push_back(pixel.green());
      channels[2].push_back(pixel.blue());
    }
  }
  std::array<int, 3> median{};
  for (std::size_t index = 0; index < channels.size(); ++index) {
    auto &values = channels[index];
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    median[index] = *middle;
  }
  return QColor(median[0], median[1], median[2]);
}

int cursorRedArcPixels(const QImage &frame, const QSize &logicalSize) {
  const qreal ratio = static_cast<qreal>(frame.width()) / logicalSize.width();
  const QPoint center(frame.width() / 2, frame.height() / 2);
  const int radius = qRound(36.0 * ratio);
  int count = 0;
  for (int y = center.y() - radius; y <= center.y() + radius; ++y) {
    for (int x = center.x() - radius; x <= center.x() + radius; ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      // A Blender 3D cursor is a separate user-positionable tool, not the
      // source origin. Its bright red ink differs from the world X axis.
      if (pixel.red() >= 220 && pixel.green() <= 40 && pixel.blue() <= 40)
        ++count;
    }
  }
  return count;
}

void dragViewport(NativeViewport &viewport, const QPointF &start,
                  const QPointF &end) {
  const QPointF globalStart(viewport.mapToGlobal(start.toPoint()));
  const QPointF globalEnd(viewport.mapToGlobal(end.toPoint()));
  QMouseEvent press(QEvent::MouseButtonPress, start, start, globalStart,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent move(QEvent::MouseMove, end, end, globalEnd,
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, end, end, globalEnd,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&viewport, &press);
  QCoreApplication::sendEvent(&viewport, &move);
  QCoreApplication::sendEvent(&viewport, &release);
}

struct BackgroundContrast final {
  double meanNeutralInk = 0.0;
  int visiblyChangedPixels = 0;
};

BackgroundContrast backgroundContrast(const QImage &frame,
                                       const QRectF &normalizedRect) {
  const QRect sample(qRound(frame.width() * normalizedRect.x()),
                     qRound(frame.height() * normalizedRect.y()),
                     qRound(frame.width() * normalizedRect.width()),
                     qRound(frame.height() * normalizedRect.height()));
  BackgroundContrast result;
  double ink = 0.0;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      const int high = std::max({pixel.red(), pixel.green(), pixel.blue()});
      const int low = std::min({pixel.red(), pixel.green(), pixel.blue()});
      if (std::abs(pixel.red() - 12) > 8 ||
          std::abs(pixel.green() - 13) > 8 ||
          std::abs(pixel.blue() - 14) > 8) ++result.visiblyChangedPixels;
      if (high - low <= 12)
        ink += std::max(0.0, (pixel.red() + pixel.green() + pixel.blue()) / 3.0 - 13.0);
    }
  }
  result.meanNeutralInk = ink / std::max(1, sample.width() * sample.height());
  return result;
}

enum class FixtureKind { Points, Mesh, Gaussian3D, Surfel2D };

QByteArray modelFixture(const FixtureKind kind) {
  const bool gaussian = kind == FixtureKind::Gaussian3D ||
                        kind == FixtureKind::Surfel2D;
  const bool mesh = kind == FixtureKind::Mesh;
  const int side = gaussian ? 3 : 15;
  const int count = mesh ? 4 : side * side;
  QByteArray contents = "ply\nformat ascii 1.0\nelement vertex " +
                        QByteArray::number(count) +
                        "\nproperty float x\nproperty float y\nproperty float z\n";
  if (gaussian) {
    contents += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
                "property float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (kind == FixtureKind::Gaussian3D) contents += "property float scale_2\n";
    contents += "property float rot_0\nproperty float rot_1\n"
                "property float rot_2\nproperty float rot_3\n";
  } else {
    contents += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  }
  if (mesh) contents += "element face 2\nproperty list uchar int vertex_indices\n";
  contents += "end_header\n";
  if (mesh) {
    contents += "19 19 20 225 35 215\n21 19 20 225 35 215\n"
                "21 21 20 225 35 215\n19 21 20 225 35 215\n"
                "3 0 1 2\n3 0 2 3\n";
  } else {
    for (int y = 0; y < side; ++y) {
      for (int x = 0; x < side; ++x) {
        contents += QByteArray::number(19.0 + 2.0 * x / (side - 1), 'g', 9) + ' ' +
                    QByteArray::number(19.0 + 2.0 * y / (side - 1), 'g', 9) + " 20 ";
        if (gaussian) {
          contents += "1.2 -1.5 1.2 8 -1.7 -1.7 ";
          if (kind == FixtureKind::Gaussian3D) contents += "-1.7 ";
          contents += "1 0 0 0\n";
        } else {
          contents += "225 35 215\n";
        }
      }
    }
  }
  return contents;
}

int renderedMagentaPixels(const QImage &frame) {
  const QRect sample(qRound(frame.width() * 0.20),
                     qRound(frame.height() * 0.20),
                     qRound(frame.width() * 0.60),
                     qRound(frame.height() * 0.60));
  int count = 0;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      // None of the grid, world axes, cursor, navigation or neutral UI uses
      // this magenta signature. Positive evidence must come from the model.
      if (pixel.red() >= 80 && pixel.blue() >= 80 &&
          pixel.red() - pixel.green() >= 40 &&
          pixel.blue() - pixel.green() >= 40) ++count;
    }
  }
  return count;
}

bool verifyModelRendering(NativeViewport &viewport, QApplication &application) {
  const QString root = QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("test-settings"));
  if (!QDir().mkpath(root)) return false;
  QTemporaryDir fixtures(QDir(root).filePath(QStringLiteral("viewport-models-XXXXXX")));
  if (!fixtures.isValid()) return false;
  struct ModelCase final { FixtureKind kind; const char *name; };
  constexpr std::array<ModelCase, 4> cases{{
      {FixtureKind::Points, "points"}, {FixtureKind::Mesh, "mesh"},
      {FixtureKind::Gaussian3D, "3dgs"}, {FixtureKind::Surfel2D, "2dgs"},
  }};
  for (const auto &model : cases) {
    const QString path = QDir(fixtures.path()).filePath(
        QString::fromLatin1(model.name) + QStringLiteral(".ply"));
    const QByteArray original = modelFixture(model.kind);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(original) != original.size())
      return false;
    file.close();
    bool loaded = false;
    bool loadFailed = false;
    const auto loadedConnection = QObject::connect(
        &viewport, &NativeViewport::sceneLoaded, &viewport,
        [&](qint64 vertices, qsizetype displayed, qint64, qsizetype) {
          loaded = vertices > 0 && displayed > 0;
        });
    const auto failedConnection = QObject::connect(
        &viewport, &NativeViewport::sceneLoadFailed, &viewport,
        [&](const QString &, const QString &message) {
          qCritical() << "Viewport model fixture failed to load:" << model.name << message;
          loadFailed = true;
        });
    const auto disconnect = qScopeGuard([&]() {
      QObject::disconnect(loadedConnection);
      QObject::disconnect(failedConnection);
    });
    viewport.setScene(path, 0);
    QElapsedTimer deadline;
    deadline.start();
    while (!loaded && !loadFailed && deadline.elapsed() < 5000) processFrames(20);
    if (!loaded || loadFailed) return false;
    const bool gaussian = model.kind == FixtureKind::Gaussian3D ||
                          model.kind == FixtureKind::Surfel2D;
    const bool surfel = model.kind == FixtureKind::Surfel2D;
    if (viewport.sourceHasGaussianAttributes() != gaussian ||
        viewport.sourceHasSurfelAttributes() != surfel ||
        (gaussian && !viewport.gaussianRenderingAvailable()) ||
        (model.kind == FixtureKind::Mesh && !viewport.meshRenderingAvailable())) {
      qCritical() << "Required real model renderer unavailable:" << model.name
                  << "Gaussian/surfel attributes" << viewport.sourceHasGaussianAttributes()
                  << viewport.sourceHasSurfelAttributes()
                  << "Gaussian/mesh renderer" << viewport.gaussianRenderingAvailable()
                  << viewport.meshRenderingAvailable();
      return false;
    }
    const auto mode = gaussian ? NativeViewport::RenderMode::Gaussians
        : model.kind == FixtureKind::Mesh ? NativeViewport::RenderMode::Mesh
                                         : NativeViewport::RenderMode::Points;
    viewport.setRenderMode(mode);
    viewport.setEditToolsLocked(true);
    viewport.setShowCameras(false);
    viewport.setShowObservationTrackball(false);
    const QVector3D translation(0.25F, -0.125F, 0.375F);
    const QQuaternion rotation = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 17.0F);
    const QVector3D scale(1.1F, 0.9F, 1.2F);
    viewport.setModelTransform(translation, rotation, scale);
    const QVector3D storedTranslation = viewport.modelTranslation();
    const QQuaternion storedRotation = viewport.modelRotation();
    const QVector3D storedScale = viewport.modelScale();
    if (storedTranslation != translation || storedScale != scale ||
        storedRotation.isIdentity()) return false;
    viewport.resetCamera();
    viewport.setAxisView(NavigationAxis::PositiveZ);
    processFrames(300);
    if (!viewport.focusModel()) return false;
    // Four actual image captures cover both themes and both projections.
    for (int configuration = 0; configuration < 4; ++configuration) {
      const bool light = configuration == 1 || configuration == 2;
      AppTheme::applyTheme(application, light ? UiTheme::Light : UiTheme::Dark, false);
      if (configuration == 2) {
        const auto navigation = navigationGizmoLayout(
            QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
        clickViewport(viewport, navigation.projectionCube.center());
      }
      processFrames(30);
      const QImage frame = viewport.grabFramebuffer();
      if (frame.isNull()) return false;
      const int modelPixels = renderedMagentaPixels(frame);
      const bool matchingState = viewport.renderMode() == mode &&
          viewport.modelTranslation() == storedTranslation &&
          viewport.modelRotation() == storedRotation && viewport.modelScale() == storedScale &&
          viewport.orthographicProjection() == (configuration >= 2) &&
          (!gaussian || viewport.gaussianRenderingAvailable());
      qInfo() << "Blender viewport real model rendering:" << model.name
              << "configuration" << configuration << "magenta-pixels" << modelPixels
              << "transform/render-path-preserved" << matchingState;
      if (!saveEvidence(frame, QStringLiteral("blender-model-") +
                               QString::fromLatin1(model.name) + QLatin1Char('-') +
                               QString::number(configuration))) return false;
      if (modelPixels < 20 || !matchingState) return false;
    }
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != original) return false;
    file.close();
  }
  viewport.setScene({}, 0);
  processFrames(30);
  return true;
}

} // namespace

int runBlenderViewportVisualTests(QApplication &application) {
  AppTheme::applyTheme(application, UiTheme::Dark, false);
  NativeViewport viewport;
  viewport.setAttribute(Qt::WA_DontShowOnScreen, true);
  viewport.setAttribute(Qt::WA_ShowWithoutActivating, true);
  viewport.setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
  viewport.resize(960, 720);
  viewport.setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);
  viewport.setShowObservationTrackball(false);
  viewport.show();
  processFrames(250);

#ifdef Q_OS_WIN
  if (IsWindowVisible(reinterpret_cast<HWND>(viewport.winId()))) {
    viewport.hide();
    qCritical() << "Viewport acceptance unexpectedly exposed a native window";
    return 2;
  }
#endif

  const QImage initialFrame = viewport.grabFramebuffer();
  if (!viewport.isValid() || viewport.context() == nullptr ||
      !viewport.context()->isValid() || initialFrame.isNull() ||
      !viewport.infiniteGridRenderingAvailable()) {
    qCritical() << "Viewport acceptance unavailable: real OpenGL framebuffer"
                   " and reference-grid renderer are required";
    return 3;
  }
  if (!saveEvidence(initialFrame, QStringLiteral("blender-perspective-initial-dark")))
    return 5;

  // Use the same public navigation control a user clicks. No private slots,
  // camera members, GL state or shader-source inspection participate.
  const NavigationGizmoLayout navigation = navigationGizmoLayout(
      QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
  clickViewport(viewport, navigation.projectionCube.center());
  if (!viewport.orthographicProjection()) {
    qCritical() << "Projection control did not switch to orthographic";
    return 4;
  }
  viewport.setAxisView(NavigationAxis::PositiveZ);
  processFrames(350);
  const QImage topFrame = viewport.grabFramebuffer();
  if (topFrame.isNull() ||
      !saveEvidence(topFrame, QStringLiteral("blender-ground-top-dark"))) {
    qCritical() << "Unable to capture real top-view framebuffer evidence";
    return 5;
  }
  const AxisPixels axes = groundAxisPixels(topFrame);
  qInfo() << "Blender viewport top-view ground axis pixels:"
          << "red" << axes.red << "green" << axes.green
          << "blue" << axes.blue;
  // Blender's documented world-axis convention is X red / Y green / Z blue.
  // Looking down +Z must leave red X and green Y on the XY ground, with no
  // long blue ground line. A tolerance allows antialiasing and driver variance.
  int failureCode = 0;
  if (axes.red < 80 || axes.green < 80 || axes.blue > 20) {
    qCritical() << "Top-view XY ground does not use Blender red-X / green-Y axes";
    failureCode = 6;
  }

  // A high-resolution wheel reports sub-notch input. Smoothly zooming out must
  // not abruptly remove half the grid at a detail-level boundary. Measure
  // rendered neutral-line contrast, not the implementation's selected level.
  viewport.setZoomToCursorEnabled(false);
  double previousInk = neutralGridInk(topFrame);
  double largestContrastJump = 0.0;
  int jumpFrame = -1;
  QImage previousFrame = topFrame;
  QImage beforeLargestJump;
  QImage afterLargestJump;
  const float initialDistance = viewport.viewDistance();
  for (int frameIndex = 0; frameIndex < 64; ++frameIndex) {
    wheelViewport(viewport, -20);
    processFrames(20);
    const QImage frame = viewport.grabFramebuffer();
    if (frame.isNull()) {
      qCritical() << "Zoom sweep lost its OpenGL framebuffer";
      return 7;
    }
    const double ink = neutralGridInk(frame);
    const double contrastJump = std::abs(ink - previousInk) /
                                std::max({ink, previousInk, 1.0});
    if (contrastJump > largestContrastJump) {
      largestContrastJump = contrastJump;
      jumpFrame = frameIndex;
      beforeLargestJump = previousFrame;
      afterLargestJump = frame;
    }
    qInfo() << "Blender viewport zoom sweep:" << frameIndex
            << "distance" << viewport.viewDistance()
            << "neutral-grid-ink" << ink << "relative-jump" << contrastJump;
    previousInk = ink;
    previousFrame = frame;
  }
  if (!saveEvidence(beforeLargestJump,
                    QStringLiteral("blender-zoom-before-largest-jump")) ||
      !saveEvidence(afterLargestJump,
                    QStringLiteral("blender-zoom-after-largest-jump"))) {
    qCritical() << "Unable to retain zoom-transition framebuffer evidence";
    return 8;
  }
  qInfo() << "Blender viewport maximum grid contrast jump:"
          << largestContrastJump << "at frame" << jumpFrame;
  if (viewport.viewDistance() <= initialDistance * 6.0F ||
      previousInk < 200.0 || largestContrastJump > 0.25) {
    qCritical() << "Sub-notch zoom caused a discontinuous ground-grid level change";
    if (failureCode == 0) failureCode = 9;
  }

  // Compatibility gate: the Blender overlay must not import Blender's grey
  // backdrop. The public GSW AppTheme seam still changes a live viewport, and
  // it must not rebuild or reset any scene/camera/render-mode state.
  const QVector3D targetBeforeTheme = viewport.viewTarget();
  const float distanceBeforeTheme = viewport.viewDistance();
  const OrbitAngles anglesBeforeTheme = viewport.viewOrbitAngles();
  const bool orthographicBeforeTheme = viewport.orthographicProjection();
  const auto modeBeforeTheme = viewport.renderMode();
  const auto sceneCountBeforeTheme = viewport.sceneObjectCount();
  for (const auto theme : {UiTheme::Light, UiTheme::Dark}) {
    AppTheme::applyTheme(application, theme, false);
    processFrames(30);
    const QImage frame = viewport.grabFramebuffer();
    if (frame.isNull()) {
      qCritical() << "Live theme change lost the viewport framebuffer";
      return 10;
    }
    const QColor backdrop = unobstructedBackdrop(frame);
    const QColor expected = theme == UiTheme::Light ? QColor(255, 255, 255)
                                                   : QColor(12, 13, 14);
    const bool matchingBackdrop =
        std::abs(backdrop.red() - expected.red()) <= 1 &&
        std::abs(backdrop.green() - expected.green()) <= 1 &&
        std::abs(backdrop.blue() - expected.blue()) <= 1;
    const bool matchingSceneState =
        viewport.viewTarget() == targetBeforeTheme &&
        viewport.viewDistance() == distanceBeforeTheme &&
        viewport.viewOrbitAngles() == anglesBeforeTheme &&
        viewport.orthographicProjection() == orthographicBeforeTheme &&
        viewport.renderMode() == modeBeforeTheme &&
        viewport.sceneObjectCount() == sceneCountBeforeTheme;
    qInfo() << "Blender viewport live theme:" << (theme == UiTheme::Light ? "light" : "dark")
            << "background" << backdrop << "state-preserved" << matchingSceneState;
    if (!saveEvidence(frame, theme == UiTheme::Light
                                  ? QStringLiteral("blender-background-light")
                                  : QStringLiteral("blender-background-dark-restored"))) {
      qCritical() << "Unable to retain theme framebuffer evidence";
      return 11;
    }
    if (!matchingBackdrop || !matchingSceneState) {
      qCritical() << "Blender overlay changed GSW's configured theme or camera/scene state";
      if (failureCode == 0) failureCode = 12;
    }
  }

  // Keep the fixed source origin distinct from Blender's user-positionable
  // 3D cursor and from the selected model's transform pivot. No red/white
  // cursor ring may be silently attached to world zero, near or far.
  const int nearCursorPixels = cursorRedArcPixels(topFrame, viewport.size());
  const QImage farOriginFrame = viewport.grabFramebuffer();
  if (farOriginFrame.isNull()) return 13;
  const int farCursorPixels = cursorRedArcPixels(farOriginFrame, viewport.size());
  qInfo() << "Blender viewport unexpected 3D-cursor pixels near/far:"
          << nearCursorPixels << farCursorPixels;
  if (!saveEvidence(farOriginFrame, QStringLiteral("blender-world-origin-unadorned")))
    return 13;
  if (nearCursorPixels != 0 || farCursorPixels != 0) {
    qCritical() << "A Blender 3D cursor was incorrectly attached to the fixed source origin";
    if (failureCode == 0) failureCode = 14;
  }

  struct PlaneCase final {
    NavigationAxis axis;
    const char *name;
    int absentColor; // 0 = red (X), 1 = green (Y), 2 = blue (Z).
  };
  constexpr std::array<PlaneCase, 6> planeCases{{
      {NavigationAxis::PositiveX, "positive-x", 0},
      {NavigationAxis::NegativeX, "negative-x", 0},
      {NavigationAxis::PositiveY, "positive-y", 1},
      {NavigationAxis::NegativeY, "negative-y", 1},
      {NavigationAxis::PositiveZ, "positive-z", 2},
      {NavigationAxis::NegativeZ, "negative-z", 2},
  }};
  for (const auto &plane : planeCases) {
    viewport.setAxisView(plane.axis);
    processFrames(300);
    const QImage frame = viewport.grabFramebuffer();
    if (frame.isNull()) return 15;
    const AxisPixels visible = groundAxisPixels(frame);
    const std::array<int, 3> counts{visible.red, visible.green, visible.blue};
    bool matchingPlane = viewport.orthographicProjection();
    for (int color = 0; color < 3; ++color)
      matchingPlane = matchingPlane && (color == plane.absentColor
          ? counts[static_cast<std::size_t>(color)] <= 20
          : counts[static_cast<std::size_t>(color)] >= 80);
    qInfo() << "Blender viewport orthographic plane:" << plane.name
            << "red/green/blue" << visible.red << visible.green << visible.blue
            << "matching" << matchingPlane;
    if (!saveEvidence(frame, QStringLiteral("blender-plane-") +
                             QString::fromLatin1(plane.name))) return 16;
    if (!matchingPlane) {
      qCritical() << "Signed orthographic axis view lost its matching world-grid plane";
      if (failureCode == 0) failureCode = 17;
    }
  }

  // Establish a shallow perspective using real navigation input. Looking only
  // ten degrees down, the distant ground must soften before the visible
  // horizon, not terminate at a rectangular patch or continue into the sky.
  viewport.resetCamera();
  viewport.setEditToolsLocked(false);
  viewport.setInteractionMode(NativeViewport::InteractionMode::Inspect);
  const QPointF dragStart(viewport.width() * 0.5, viewport.height() * 0.5);
  dragViewport(viewport, dragStart, dragStart + QPointF(0.0, -50.0));
  processFrames(60);
  const QImage horizonFrame = viewport.grabFramebuffer();
  if (horizonFrame.isNull()) return 18;
  const auto sky = backgroundContrast(horizonFrame, QRectF(0.40, 0.12, 0.30, 0.08));
  const auto horizon = backgroundContrast(horizonFrame, QRectF(0.20, 0.31, 0.60, 0.05));
  const auto foreground = backgroundContrast(horizonFrame, QRectF(0.20, 0.64, 0.60, 0.14));
  const float pitch = viewport.viewOrbitAngles().pitchDegrees;
  const bool fadingHorizon = !viewport.orthographicProjection() &&
      pitch >= 8.0F && pitch <= 12.0F &&
      sky.visiblyChangedPixels <= 5 && foreground.meanNeutralInk >= 0.05 &&
      horizon.meanNeutralInk < foreground.meanNeutralInk * 0.5;
  qInfo() << "Blender viewport shallow horizon: pitch" << pitch
          << "sky-visible-pixels" << sky.visiblyChangedPixels
          << "neutral-ink horizon/foreground" << horizon.meanNeutralInk
          << foreground.meanNeutralInk << "fading" << fadingHorizon;
  if (!saveEvidence(horizonFrame, QStringLiteral("blender-perspective-shallow-horizon")))
    return 19;
  if (!fadingHorizon) {
    qCritical() << "Perspective ground does not fade naturally before the visible horizon";
    if (failureCode == 0) failureCode = 20;
  }
  if (!verifyModelRendering(viewport, application)) {
    qCritical() << "A real point, indexed mesh, 3DGS or 2DGS renderer regressed";
    if (failureCode == 0) failureCode = 21;
  }
  if (failureCode == 0)
    qInfo() << "Blender viewport visual acceptance passed: axes, grid zoom, themes, unadorned origin, signed planes, horizon and model modes";
  return failureCode;
}

} // namespace gsw
