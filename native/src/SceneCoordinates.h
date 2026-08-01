#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector3D>

namespace gsw {

enum class SceneLengthUnit {
  Millimetres,
  Centimetres,
  Metres,
  Unknown,
};

struct SceneCoordinate3D final {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  [[nodiscard]] bool isFinite() const;
};

struct SceneCoordinateMetadata final {
  SceneLengthUnit unit = SceneLengthUnit::Unknown;
  bool unitDeclared = false;
  QString coordinateReferenceSystem;
  bool sourceUsesFloat64 = false;
};

struct SceneCoordinateInfo final {
  bool valid = false;
  SceneCoordinate3D globalMinimum;
  SceneCoordinate3D globalMaximum;
  SceneCoordinate3D displayShift;
  double displayScale = 1.0;
  SceneLengthUnit unit = SceneLengthUnit::Unknown;
  bool unitDeclared = false;
  QString coordinateReferenceSystem;
  bool sourceUsesFloat64 = false;
  bool automaticDisplayShift = false;

  [[nodiscard]] SceneCoordinate3D globalCenter() const;
  [[nodiscard]] SceneCoordinate3D globalSize() const;
  [[nodiscard]] double globalDiagonal() const;
  [[nodiscard]] QVector3D localFromGlobal(const SceneCoordinate3D &value) const;
  [[nodiscard]] SceneCoordinate3D globalFromLocal(const QVector3D &value) const;
  [[nodiscard]] QVector3D localMinimum() const;
  [[nodiscard]] QVector3D localMaximum() const;
  [[nodiscard]] QVector3D localCenter() const;
};

class SceneCoordinateTracker final {
public:
  static constexpr double AutomaticShiftThreshold = 100'000.0;

  explicit SceneCoordinateTracker(
      const SceneCoordinateMetadata &metadata = {},
      double automaticShiftThreshold = AutomaticShiftThreshold);

  [[nodiscard]] bool observeAndMap(double x, double y, double z,
                                   QVector3D &localPosition);
  [[nodiscard]] const SceneCoordinateInfo &info() const { return mInfo; }

private:
  SceneCoordinateInfo mInfo;
  double mAutomaticShiftThreshold = AutomaticShiftThreshold;
};

[[nodiscard]] SceneCoordinateMetadata parsePlyCoordinateMetadata(
    const QStringList &headerMetadata, const QString &sourcePath,
    bool sourceUsesFloat64);

[[nodiscard]] QString sceneLengthUnitSymbol(SceneLengthUnit unit);
[[nodiscard]] QString sceneLengthUnitDescription(
    const SceneCoordinateInfo &coordinates);
[[nodiscard]] QString formatSceneCoordinate(double value,
                                            const SceneCoordinateInfo &coordinates);
[[nodiscard]] QString formatSceneLength(double value,
                                        const SceneCoordinateInfo &coordinates);
[[nodiscard]] QString formatSceneVector(const SceneCoordinate3D &value,
                                        const SceneCoordinateInfo &coordinates);
[[nodiscard]] QString formatSceneSize(const SceneCoordinateInfo &coordinates);

[[nodiscard]] QJsonObject sceneCoordinateInfoToJson(
    const SceneCoordinateInfo &coordinates);
[[nodiscard]] bool sceneCoordinateInfoFromJson(
    const QJsonObject &object, SceneCoordinateInfo &coordinates);

[[nodiscard]] bool writeSceneCoordinateReport(
    const QString &destinationPath, const QString &sourcePath,
    const SceneCoordinateInfo &coordinates, double referencePlaneElevation,
    const QString &referencePlaneMode, QString *errorMessage = nullptr);

} // namespace gsw
