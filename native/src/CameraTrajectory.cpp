#include <QCoreApplication>
#include "CameraTrajectory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <cmath>

namespace gsw {
namespace {
constexpr qint64 kMaximumCameraFileSize = 64LL * 1024LL * 1024LL;
constexpr int kMaximumAncestorSearchDepth = 8;
constexpr float kMinimumAxisLength = 0.000001F;
constexpr float kRotationTolerance = 0.01F;
constexpr float kMinimumFrustumSize = 0.08F;
constexpr qsizetype kMaximumDisplayedFrustumCameras = 1000;
constexpr qsizetype kMaximumDisplayedPathCameras = 5000;

bool readVector3(const QJsonValue &value, QVector3D *result) {
  if (result == nullptr || !value.isArray()) {
    return false;
  }
  const QJsonArray values = value.toArray();
  if (values.size() != 3 || !values.at(0).isDouble() ||
      !values.at(1).isDouble() || !values.at(2).isDouble()) {
    return false;
  }
  const double x = values.at(0).toDouble();
  const double y = values.at(1).toDouble();
  const double z = values.at(2).toDouble();
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }
  *result = QVector3D(static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(z));
  return std::isfinite(result->x()) && std::isfinite(result->y()) &&
         std::isfinite(result->z());
}

bool normalizedAxis(const QVector3D &axis, QVector3D *result) {
  if (result == nullptr || !std::isfinite(axis.lengthSquared()) ||
      axis.lengthSquared() <= kMinimumAxisLength * kMinimumAxisLength) {
    return false;
  }
  *result = axis.normalized();
  return true;
}

bool parseCamera(const QJsonValue &value, CameraPose *camera) {
  if (camera == nullptr || !value.isObject()) {
    return false;
  }
  const QJsonObject object = value.toObject();
  QVector3D position;
  if (!readVector3(object.value(QStringLiteral("position")), &position)) {
    return false;
  }

  const QJsonValue rotationValue = object.value(QStringLiteral("rotation"));
  if (!rotationValue.isArray()) {
    return false;
  }
  const QJsonArray rotation = rotationValue.toArray();
  if (rotation.size() != 3) {
    return false;
  }
  QVector3D rows[3];
  if (!readVector3(rotation.at(0), &rows[0]) ||
      !readVector3(rotation.at(1), &rows[1]) ||
      !readVector3(rotation.at(2), &rows[2])) {
    return false;
  }

  QVector3D right;
  QVector3D imageDown;
  QVector3D forward;
  if (!normalizedAxis(QVector3D(rows[0].x(), rows[1].x(), rows[2].x()),
                      &right) ||
      !normalizedAxis(QVector3D(rows[0].y(), rows[1].y(), rows[2].y()),
                      &imageDown) ||
      !normalizedAxis(QVector3D(rows[0].z(), rows[1].z(), rows[2].z()),
                      &forward)) {
    return false;
  }
  const float determinant =
      QVector3D::dotProduct(QVector3D::crossProduct(right, imageDown),
                            forward);
  if (std::abs(QVector3D::dotProduct(right, imageDown)) >
          kRotationTolerance ||
      std::abs(QVector3D::dotProduct(right, forward)) >
          kRotationTolerance ||
      std::abs(QVector3D::dotProduct(imageDown, forward)) >
          kRotationTolerance ||
      std::abs(determinant - 1.0F) > kRotationTolerance) {
    return false;
  }

  camera->imageName = object.value(QStringLiteral("img_name")).toString();
  camera->position = position;
  camera->right = right;
  camera->imageDown = imageDown;
  camera->forward = forward;
  const int width = object.value(QStringLiteral("width")).toInt();
  const int height = object.value(QStringLiteral("height")).toInt();
  camera->width = width > 0 ? width : 0;
  camera->height = height > 0 ? height : 0;
  return true;
}

QString findCameraFile(const QString &scenePath) {
  if (scenePath.trimmed().isEmpty()) {
    return {};
  }
  const QFileInfo sceneInfo(scenePath);
  QDir directory = sceneInfo.isDir() ? QDir(sceneInfo.absoluteFilePath())
                                      : sceneInfo.absoluteDir();
  for (int depth = 0; depth <= kMaximumAncestorSearchDepth; ++depth) {
    const QString candidate =
        directory.filePath(QStringLiteral("cameras.json"));
    if (QFileInfo::exists(candidate)) {
      return QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    }
    const QString before = directory.absolutePath();
    if (!directory.cdUp() || directory.absolutePath() == before) {
      break;
    }
  }
  return {};
}
} // namespace

CameraTrajectory CameraTrajectory::loadForScene(const QString &scenePath) {
  CameraTrajectory trajectory;
  trajectory.mSourcePath = findCameraFile(scenePath);
  if (trajectory.mSourcePath.isEmpty()) {
    return trajectory;
  }

  const QFileInfo sourceInfo(trajectory.mSourcePath);
  if (sourceInfo.size() > kMaximumCameraFileSize) {
    trajectory.mError = QCoreApplication::translate("Workbench", "cameras.json 过大，已跳过加载（上限 64 MiB）。");
    return trajectory;
  }

  QFile source(trajectory.mSourcePath);
  if (!source.open(QIODevice::ReadOnly)) {
    trajectory.mError =
        QCoreApplication::translate("Workbench", "无法读取 cameras.json：%1").arg(source.errorString());
    return trajectory;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(source.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    trajectory.mError = QCoreApplication::translate("Workbench", "cameras.json 格式无效：%1（偏移 %2）")
                            .arg(parseError.errorString())
                            .arg(parseError.offset);
    return trajectory;
  }
  if (!document.isArray()) {
    trajectory.mError = QCoreApplication::translate("Workbench", "cameras.json 顶层必须是相机数组。");
    return trajectory;
  }

  const QJsonArray cameras = document.array();
  trajectory.mCameras.reserve(cameras.size());
  for (const QJsonValue &value : cameras) {
    CameraPose camera;
    if (parseCamera(value, &camera)) {
      trajectory.mCameras.append(camera);
    } else {
      ++trajectory.mInvalidCameraCount;
    }
  }
  if (!cameras.isEmpty() && trajectory.mCameras.isEmpty()) {
    trajectory.mError =
        QCoreApplication::translate("Workbench", "cameras.json 未包含可用相机位姿，请检查 position 与 rotation 字段。");
  }
  return trajectory;
}

const QList<CameraPose> &CameraTrajectory::cameras() const { return mCameras; }

QString CameraTrajectory::sourcePath() const { return mSourcePath; }

QString CameraTrajectory::error() const { return mError; }

qsizetype CameraTrajectory::invalidCameraCount() const {
  return mInvalidCameraCount;
}

CameraTrajectoryGeometry CameraTrajectory::geometry(const float sceneRadius) const {
  CameraTrajectoryGeometry result;
  if (mCameras.isEmpty()) {
    return result;
  }

  QVector3D minimum = mCameras.constFirst().position;
  QVector3D maximum = minimum;
  for (const CameraPose &camera : mCameras) {
    minimum.setX(std::min(minimum.x(), camera.position.x()));
    minimum.setY(std::min(minimum.y(), camera.position.y()));
    minimum.setZ(std::min(minimum.z(), camera.position.z()));
    maximum.setX(std::max(maximum.x(), camera.position.x()));
    maximum.setY(std::max(maximum.y(), camera.position.y()));
    maximum.setZ(std::max(maximum.z(), camera.position.z()));
  }
  const float span = (maximum - minimum).length();
  const float radius = std::isfinite(sceneRadius) ? std::abs(sceneRadius) : 0.0F;
  const float size =
      std::max({span * 0.015F, radius * 0.025F, kMinimumFrustumSize});
  const qsizetype cameraCount = mCameras.size();
  const qsizetype frustumCameraCount =
      std::min(cameraCount, kMaximumDisplayedFrustumCameras);
  const qsizetype pathCameraCount =
      std::min(cameraCount, kMaximumDisplayedPathCameras);
  result.displayedFrustumCameraCount = frustumCameraCount;
  result.displayedPathCameraCount = pathCameraCount;
  result.decimated = frustumCameraCount < cameraCount ||
                     pathCameraCount < cameraCount;

  const auto sourceIndex = [cameraCount](const qsizetype displayedIndex,
                                         const qsizetype displayedCount) {
    if (displayedCount <= 1 || cameraCount <= 1) {
      return qsizetype(0);
    }
    return displayedIndex * (cameraCount - 1) / (displayedCount - 1);
  };

  result.frustums.reserve(frustumCameraCount * 8);
  for (qsizetype displayedIndex = 0;
       displayedIndex < frustumCameraCount; ++displayedIndex) {
    const CameraPose &camera =
        mCameras.at(sourceIndex(displayedIndex, frustumCameraCount));
    const float aspectHeight =
        camera.width > 0 && camera.height > 0
            ? size * static_cast<float>(camera.height) /
                  static_cast<float>(camera.width)
            : size * 0.65F;
    const QVector3D center = camera.position + camera.forward * size * 1.6F;
    const QVector3D corners[] = {
        center - camera.right * size - camera.imageDown * aspectHeight,
        center + camera.right * size - camera.imageDown * aspectHeight,
        center + camera.right * size + camera.imageDown * aspectHeight,
        center - camera.right * size + camera.imageDown * aspectHeight};
    for (const QVector3D &corner : corners) {
      result.frustums.append({camera.position, corner});
    }
    for (int corner = 0; corner < 4; ++corner) {
      result.frustums.append({corners[corner], corners[(corner + 1) % 4]});
    }
  }

  result.path.reserve(std::max(qsizetype(0), pathCameraCount - 1));
  for (qsizetype displayedIndex = 1; displayedIndex < pathCameraCount;
       ++displayedIndex) {
    const QVector3D start =
        mCameras.at(sourceIndex(displayedIndex - 1, pathCameraCount)).position;
    const QVector3D end =
        mCameras.at(sourceIndex(displayedIndex, pathCameraCount)).position;
    result.path.append({start, end});
  }
  return result;
}

} // namespace gsw
