#pragma once

#include <QList>
#include <QString>
#include <QVector3D>

namespace gsw {

struct CameraPose final {
  QString imageName;
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
  [[nodiscard]] CameraTrajectoryGeometry geometry(float sceneRadius) const;

private:
  QList<CameraPose> mCameras;
  QString mSourcePath;
  QString mError;
  qsizetype mInvalidCameraCount = 0;
};

} // namespace gsw
