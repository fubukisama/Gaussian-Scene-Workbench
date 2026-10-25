#include "BlenderViewportVisualTests.h"
#include "BlenderViewportVisualTestSupport.h"

#include "AppTheme.h"
#include "NativeViewport.h"
#include "NavigationGizmo.h"

#include <QApplication>
#include <QDebug>
#include <QFontMetricsF>
#include <QOpenGLContext>

#include <algorithm>
#include <array>
#include <cmath>

namespace gsw {
namespace {
using namespace viewportqa;

// Zero means continue. Nonzero preserves the original immediate-return code;
// failureCode retains the first non-fatal assertion failure across stages.
int verifyGridAndZoom(NativeViewport &viewport, QImage &topFrame, int &failureCode) {
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
  clickViewport(viewport, navigation.projectionButton.center());
  if (!viewport.orthographicProjection()) {
    qCritical() << "Projection control did not switch to orthographic";
    return 4;
  }
  viewport.setAxisView(NavigationAxis::PositiveZ);
  processFrames(350);
  topFrame = viewport.grabFramebuffer();
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

  return 0;
}

int verifyThemeAndOrigin(NativeViewport &viewport, QApplication &application,
                         const QImage &topFrame, int &failureCode) {
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

  return 0;
}

int verifySignedPlanes(NativeViewport &viewport, int &failureCode) {
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

  return 0;
}

int verifyShallowHorizon(NativeViewport &viewport, int &failureCode) {
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
  return 0;
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
  viewportqa::processFrames(250);
  if (viewportqa::nativeWindowExposed(viewport)) return 2;

  int failureCode = 0;
  QImage topFrame;
  if (const int fatal = verifyGridAndZoom(viewport, topFrame, failureCode)) return fatal;
  if (const int fatal = verifyThemeAndOrigin(viewport, application, topFrame, failureCode)) return fatal;
  if (const int fatal = verifySignedPlanes(viewport, failureCode)) return fatal;
  if (const int fatal = verifyShallowHorizon(viewport, failureCode)) return fatal;
  if (!viewportqa::verifyModelRendering(viewport, application)) {
    qCritical() << "A real point, indexed mesh, 3DGS or 2DGS renderer regressed";
    if (failureCode == 0) failureCode = 21;
  }
  if (failureCode == 0)
    qInfo() << "Blender viewport visual acceptance passed: axes, grid zoom, themes, unadorned origin, signed planes, horizon and model modes";
  return failureCode;
}

} // namespace gsw
