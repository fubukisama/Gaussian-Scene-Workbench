#pragma once

#include "ModelInteraction.h"
#include "SceneCoordinates.h"
#include <QBitArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace gsw {
enum class ModelExportFormat { Ply, Xyz, Csv, Obj, Stl, Glb, Spz };

struct ModelExportOptions {
  QString sourcePath;
  QString destinationPath;
  ModelExportFormat format = ModelExportFormat::Ply;
  QBitArray deletedVertices;
  bool applyTransform = false;
  ModelTransform transform;
  SceneCoordinate3D pivot;
  SceneCoordinateInfo coordinates;
  // When baking, convert source-unit TRS into the shared scene unit. This is
  // not a GPU display scale/shift and is ignored by source-coordinate export.
  double sceneUnitScale = 1.0;
  SceneLengthUnit sceneUnit = SceneLengthUnit::Unknown;
  bool sceneUnitDeclared = false;
  int spzVersion = 4;
  int spzMaximumShDegree = -1; // -1 retains the source degree.
  int spzQuality = 1; // 0 compact (4/3), 1 balanced (5/4), 2 high (8/8).
  // Explicit recorded-frame recovery; an optional proper Sim3 scene edit is
  // baked before passive frame recovery. sceneUnitScale does not enter it.
  QString cameraCoordinateRecovery; // empty, source, canonical, metric, training, preserve
  QString recordedRepresentation = QStringLiteral("auto"); // auto, gaussian, point_cloud, mesh
  QString cameraOutputUnit; // empty uses recorded/default unit; m/cm/mm explicit.
  QString recoveryPythonPath;
  QString recoveryRepositoryRoot;
  // Called on the export thread. A true return requests cancellation.
  std::function<bool(int)> progress;
};

struct ModelExportResult {
  bool success = false;
  bool cancelled = false;
  QString error;
  QJsonObject coordinateRecovery;
};

struct CameraScaleRecoveryCapability {
  bool preserve = false;
  bool source = false;
  bool metric = false;
  QString currentFrame;
  QString currentUnit;
  QString sourceUnit;
  QString scaleStatus;
  QString validationError;
};
[[nodiscard]] QString cameraScaleMetadataPath(const QString &source);
[[nodiscard]] CameraScaleRecoveryCapability cameraScaleRecoveryCapability(const QString &source);
[[nodiscard]] QString verifiedTrainingDeliveryPath(const QString &outputDirectory, int expectedIteration = -1);

[[nodiscard]] SceneCoordinate3D exportPosition(const SceneCoordinate3D &position,
                                              const ModelExportOptions &options);
[[nodiscard]] QVector3D exportNormal(const QVector3D &normal,
                                     const ModelExportOptions &options);
[[nodiscard]] ModelExportResult exportModelFile(const ModelExportOptions &options);
[[nodiscard]] QString modelExportSuffix(ModelExportFormat format);
} // namespace gsw
