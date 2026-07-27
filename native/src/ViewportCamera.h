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

[[nodiscard]] OrbitAngles orbitAnglesAfterLeftDrag(OrbitAngles current,
                                                   const QPoint &delta);

[[nodiscard]] OrbitFrame orbitFrame(OrbitAngles angles);

[[nodiscard]] ReferenceGridPlane
referenceGridPlane(OrbitAngles angles, bool orthographic);

[[nodiscard]] ViewportZoomLimits viewportZoomLimits(float sceneRadius);

[[nodiscard]] float clampViewportDistance(float distance, float sceneRadius);

[[nodiscard]] ReferenceGridScale referenceGridScale(float cameraDistance,
                                                    int viewportPixelHeight);

// Display convention: one scene unit is treated as one metre until a project
// supplies an explicit scale calibration.
[[nodiscard]] QString formatMetricDistance(float metres);

} // namespace gsw
