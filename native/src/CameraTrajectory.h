#pragma once

#include "SceneCoordinates.h"

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QVector3D>

namespace gsw {

struct CameraPose final {
  QString imageName;
  QString viewId;
  QString split;
  QString coordinateFrame;
  QString coordinateUnit;
  QString pixelCoordinates;
  QJsonArray intrinsics;
  QJsonArray cameraToWorld;
  // Current model-file coordinates. Never round these to float before the
  // model's display shift has been applied. position is legacy compatibility.
  SceneCoordinate3D globalPosition;
  QVector3D position;
  QVector3D right;
  QVector3D imageDown;
  QVector3D forward;
  int width = 0;
  int height = 0;
  float verticalFovDegrees = 46.0F;
  float focalAspectCorrection = 1.0F;
};

struct CameraLineSegment final {
  QVector3D start;
  QVector3D end;
};

struct CameraTrajectoryGeometry final {
  QList<CameraLineSegment> frustums;
  QList<CameraLineSegment> path;
  qsizetype displayedFrustumCameraCount = 0;
  qsizetype displayedPathCameraCount = 0;
  bool decimated = false;
};

class CameraTrajectory final {
public:
  [[nodiscard]] static CameraTrajectory loadForScene(const QString &scenePath);

  [[nodiscard]] const QList<CameraPose> &cameras() const;
  [[nodiscard]] QString sourcePath() const;
  [[nodiscard]] QString error() const;
  [[nodiscard]] qsizetype invalidCameraCount() const;
  [[nodiscard]] bool metadataVerified() const;
  [[nodiscard]] QString coordinateFrame() const;
  [[nodiscard]] QString coordinateUnit() const;
  // Positive clipping lengths in the current model-file frame and unit.
  [[nodiscard]] QJsonObject clipping() const;
  [[nodiscard]] CameraTrajectoryGeometry geometry(float sceneRadius) const;
  [[nodiscard]] CameraTrajectoryGeometry geometry(float sceneRadius,
      const SceneCoordinateInfo &coordinates) const;

private:
  QList<CameraPose> mCameras;
  QString mSourcePath;
  QString mError;
  QString mMetadataErrorCode;
  qsizetype mInvalidCameraCount = 0;
  bool mMetadataVerified = false;
  QString mCoordinateFrame;
  QString mCoordinateUnit;
  QJsonObject mClipping;
};

} // namespace gsw
