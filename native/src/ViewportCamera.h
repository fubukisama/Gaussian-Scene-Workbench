#pragma once

#include <QPoint>
#include <QString>
#include <QVector3D>

namespace gsw {

struct OrbitAngles {
  float yawDegrees = 0.0F;
  float pitchDegrees = 0.0F;

  [[nodiscard]] bool operator==(const OrbitAngles &) const = default;
};

struct OrbitFrame {
  QVector3D cameraOffsetDirection;
  QVector3D upDirection;
};

enum class ReferenceGridPlane { XY, XZ, YZ };

struct ViewportZoomLimits {
  float minimumDistance = 0.001F;
  float maximumDistance = 100.0F;
};

struct ReferenceGridScale {
  float minimumStep = 0.001F;
  float lowerMinorStep = 0.001F;
  float upperMinorStep = 0.001F;
  float levelBlend = 0.0F;
  float displayMajorStep = 0.01F;
  float visibleDistance = 1.0F;
};

struct ReferenceGridDrawSpans {
  float fineHalfSpan = 1.0F;
  float majorHalfSpan = 1.0F;
  float axisHalfSpan = 1.0F;
};

[[nodiscard]] OrbitAngles orbitAnglesAfterLeftDrag(OrbitAngles current,
                                                   const QPoint &delta);

// In an editing tool, Ctrl temporarily gives the left mouse button back to
// camera orbit. The caller latches this decision for the full drag so modifier
// changes cannot turn an orbit into a selection gesture halfway through.
[[nodiscard]] bool
isTemporaryOrbitShortcut(Qt::MouseButton button,
                         Qt::KeyboardModifiers modifiers);

[[nodiscard]] OrbitFrame orbitFrame(OrbitAngles angles);

[[nodiscard]] ReferenceGridPlane
referenceGridPlane(OrbitAngles angles, bool orthographic);

[[nodiscard]] ViewportZoomLimits viewportZoomLimits(float sceneRadius);

[[nodiscard]] float clampViewportDistance(float distance, float sceneRadius);

[[nodiscard]] ReferenceGridScale referenceGridScale(float cameraDistance,
                                                    int viewportPixelHeight);

[[nodiscard]] ReferenceGridDrawSpans
referenceGridDrawSpans(const ReferenceGridScale &scale);

// Metashape-style point previews use a one-logical-pixel footprint. Convert
// that footprint to the physical pixels expected by OpenGL.
[[nodiscard]] float pointPreviewDiameterPixels(float devicePixelRatio);

// Display convention: one scene unit is treated as one metre until a project
// supplies an explicit scale calibration.
[[nodiscard]] QString formatMetricDistance(float metres);

} // namespace gsw
