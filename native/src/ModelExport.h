#pragma once

#include "ModelInteraction.h"
#include "SceneCoordinates.h"
#include <QBitArray>
#include <QStringList>
#include <functional>

namespace gsw {
enum class ModelExportFormat { Ply, Xyz, Csv, Obj, Stl, Glb };

struct ModelExportOptions {
  QString sourcePath;
  QString destinationPath;
  ModelExportFormat format = ModelExportFormat::Ply;
  QBitArray deletedVertices;
  bool applyTransform = false;
  ModelTransform transform;
  SceneCoordinate3D pivot;
  SceneCoordinateInfo coordinates;
  // Called on the export thread. A true return requests cancellation.
  std::function<bool(int)> progress;
};

struct ModelExportResult {
  bool success = false;
  bool cancelled = false;
  QString error;
};

[[nodiscard]] SceneCoordinate3D exportPosition(const SceneCoordinate3D &position,
                                              const ModelExportOptions &options);
[[nodiscard]] QVector3D exportNormal(const QVector3D &normal,
                                     const ModelExportOptions &options);
[[nodiscard]] ModelExportResult exportModelFile(const ModelExportOptions &options);
[[nodiscard]] QString modelExportSuffix(ModelExportFormat format);
} // namespace gsw
