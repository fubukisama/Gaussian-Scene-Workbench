#include <QCoreApplication>
#include "SceneCoordinates.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace gsw {
namespace {

QJsonArray vectorToJson(const SceneCoordinate3D &value) {
  return {value.x, value.y, value.z};
}

bool vectorFromJson(const QJsonValue &value, SceneCoordinate3D &result) {
  const QJsonArray values = value.toArray();
  if (values.size() != 3) {
    return false;
  }
  result = {values.at(0).toDouble(), values.at(1).toDouble(),
            values.at(2).toDouble()};
  return result.isFinite();
}

QString unitKey(const SceneLengthUnit unit) {
  switch (unit) {
  case SceneLengthUnit::Millimetres:
    return QStringLiteral("millimetres");
  case SceneLengthUnit::Centimetres:
    return QStringLiteral("centimetres");
  case SceneLengthUnit::Metres:
    return QStringLiteral("metres");
  case SceneLengthUnit::Unknown:
    return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

SceneLengthUnit unitFromKey(const QString &key) {
  const QString normalized = key.trimmed().toLower();
  if (normalized == QStringLiteral("millimetres") ||
      normalized == QStringLiteral("millimeters") ||
      normalized == QStringLiteral("mm")) {
    return SceneLengthUnit::Millimetres;
  }
  if (normalized == QStringLiteral("centimetres") ||
      normalized == QStringLiteral("centimeters") ||
      normalized == QStringLiteral("cm")) {
    return SceneLengthUnit::Centimetres;
  }
  if (normalized == QStringLiteral("metres") ||
      normalized == QStringLiteral("meters") ||
      normalized == QStringLiteral("m")) {
    return SceneLengthUnit::Metres;
  }
  return SceneLengthUnit::Unknown;
}

double metresPerUnit(const SceneLengthUnit unit) {
  switch (unit) {
  case SceneLengthUnit::Millimetres:
    return 0.001;
  case SceneLengthUnit::Centimetres:
    return 0.01;
  case SceneLengthUnit::Metres:
    return 1.0;
  case SceneLengthUnit::Unknown:
    return 1.0;
  }
  return 1.0;
}

QString conciseNumber(const double value, const int significantDigits = 12) {
  if (!std::isfinite(value)) {
    return QStringLiteral("-");
  }
  return QString::number(value, 'g', significantDigits);
}

QString csvEscape(QString value) {
  value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  return QStringLiteral("\"%1\"").arg(value);
}

} // namespace

bool SceneCoordinate3D::isFinite() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

SceneCoordinate3D SceneCoordinateInfo::globalCenter() const {
  return {(globalMinimum.x + globalMaximum.x) * 0.5,
          (globalMinimum.y + globalMaximum.y) * 0.5,
          (globalMinimum.z + globalMaximum.z) * 0.5};
}

SceneCoordinate3D SceneCoordinateInfo::globalSize() const {
  return {globalMaximum.x - globalMinimum.x,
          globalMaximum.y - globalMinimum.y,
          globalMaximum.z - globalMinimum.z};
}

double SceneCoordinateInfo::globalDiagonal() const {
  const SceneCoordinate3D size = globalSize();
  return std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z);
}

QVector3D SceneCoordinateInfo::localFromGlobal(
    const SceneCoordinate3D &value) const {
  return QVector3D(static_cast<float>((value.x + displayShift.x) * displayScale),
                   static_cast<float>((value.y + displayShift.y) * displayScale),
                   static_cast<float>((value.z + displayShift.z) * displayScale));
}

SceneCoordinate3D SceneCoordinateInfo::globalFromLocal(
    const QVector3D &value) const {
  const double inverseScale = displayScale == 0.0 ? 1.0 : 1.0 / displayScale;
  return {static_cast<double>(value.x()) * inverseScale - displayShift.x,
          static_cast<double>(value.y()) * inverseScale - displayShift.y,
          static_cast<double>(value.z()) * inverseScale - displayShift.z};
}

QVector3D SceneCoordinateInfo::localMinimum() const {
  return localFromGlobal(globalMinimum);
}

QVector3D SceneCoordinateInfo::localMaximum() const {
  return localFromGlobal(globalMaximum);
}

QVector3D SceneCoordinateInfo::localCenter() const {
  return localFromGlobal(globalCenter());
}

SceneCoordinateTracker::SceneCoordinateTracker(
    const SceneCoordinateMetadata &metadata,
    const double automaticShiftThreshold)
    : mAutomaticShiftThreshold(std::max(1.0, automaticShiftThreshold)) {
  mInfo.unit = metadata.unit;
  mInfo.unitDeclared = metadata.unitDeclared;
  mInfo.coordinateReferenceSystem = metadata.coordinateReferenceSystem;
  mInfo.sourceUsesFloat64 = metadata.sourceUsesFloat64;
}

bool SceneCoordinateTracker::observeAndMap(const double x, const double y,
                                           const double z,
                                           QVector3D &localPosition) {
  const SceneCoordinate3D global{x, y, z};
  if (!global.isFinite()) {
    return false;
  }
  if (!mInfo.valid) {
    mInfo.valid = true;
    mInfo.globalMinimum = global;
    mInfo.globalMaximum = global;
    const double largestMagnitude =
        std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (largestMagnitude >= mAutomaticShiftThreshold) {
      mInfo.displayShift = {-x, -y, -z};
      mInfo.automaticDisplayShift = true;
    }
  } else {
    mInfo.globalMinimum.x = std::min(mInfo.globalMinimum.x, x);
    mInfo.globalMinimum.y = std::min(mInfo.globalMinimum.y, y);
    mInfo.globalMinimum.z = std::min(mInfo.globalMinimum.z, z);
    mInfo.globalMaximum.x = std::max(mInfo.globalMaximum.x, x);
    mInfo.globalMaximum.y = std::max(mInfo.globalMaximum.y, y);
    mInfo.globalMaximum.z = std::max(mInfo.globalMaximum.z, z);
  }
  localPosition = mInfo.localFromGlobal(global);
  return std::isfinite(localPosition.x()) && std::isfinite(localPosition.y()) &&
         std::isfinite(localPosition.z());
}

SceneCoordinateMetadata parsePlyCoordinateMetadata(
    const QStringList &headerMetadata, const QString &sourcePath,
    const bool sourceUsesFloat64) {
  SceneCoordinateMetadata result;
  result.sourceUsesFloat64 = sourceUsesFloat64;
  static const QRegularExpression unitExpression(
      QStringLiteral(
          R"((?:^|\b)(?:coordinate[_ ]?unit|units?)\s*[:=]?\s*(mm|millimet(?:er|re)s?|cm|centimet(?:er|re)s?|m|met(?:er|re)s?)\b)"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression epsgExpression(
      QStringLiteral(R"(\bEPSG\s*[:=]?\s*(\d{3,8})\b)"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression crsExpression(
      QStringLiteral(R"((?:^|\b)(?:crs|coordinate[_ ]?system)\s*[:=]\s*(.+)$)"),
      QRegularExpression::CaseInsensitiveOption);

  for (const QString &line : headerMetadata) {
    const QRegularExpressionMatch unitMatch = unitExpression.match(line);
    if (!result.unitDeclared && unitMatch.hasMatch()) {
      result.unit = unitFromKey(unitMatch.captured(1));
      result.unitDeclared = result.unit != SceneLengthUnit::Unknown;
    }
    if (result.coordinateReferenceSystem.isEmpty()) {
      const QRegularExpressionMatch epsgMatch = epsgExpression.match(line);
      if (epsgMatch.hasMatch()) {
        result.coordinateReferenceSystem =
            QStringLiteral("EPSG:%1").arg(epsgMatch.captured(1));
      } else {
        const QRegularExpressionMatch crsMatch = crsExpression.match(line);
        if (crsMatch.hasMatch()) {
          result.coordinateReferenceSystem = crsMatch.captured(1).trimmed();
        }
      }
    }
  }

  const QFileInfo source(sourcePath);
  const QString prjPath = source.dir().filePath(source.completeBaseName() +
                                                QStringLiteral(".prj"));
  QFile prj(prjPath);
  if (prj.open(QIODevice::ReadOnly) && prj.size() <= 1024 * 1024) {
    const QString projection = QString::fromUtf8(prj.readAll()).simplified();
    if (result.coordinateReferenceSystem.isEmpty()) {
      result.coordinateReferenceSystem =
          projection;
    }
    if (!result.unitDeclared) {
      static const QRegularExpression projectionUnitExpression(
          QStringLiteral(
              R"regex((?:LENGTHUNIT|UNIT)\s*\[\s*"(millimet(?:er|re)s?|centimet(?:er|re)s?|met(?:er|re)s?)")regex"),
          QRegularExpression::CaseInsensitiveOption);
      const QRegularExpressionMatch unitMatch =
          projectionUnitExpression.match(projection);
      if (unitMatch.hasMatch()) {
        result.unit = unitFromKey(unitMatch.captured(1));
        result.unitDeclared = result.unit != SceneLengthUnit::Unknown;
      }
    }
  }
  return result;
}

QString sceneLengthUnitSymbol(const SceneLengthUnit unit) {
  switch (unit) {
  case SceneLengthUnit::Millimetres:
    return QStringLiteral("mm");
  case SceneLengthUnit::Centimetres:
    return QStringLiteral("cm");
  case SceneLengthUnit::Metres:
    return QStringLiteral("m");
  case SceneLengthUnit::Unknown:
    return QStringLiteral("u");
  }
  return QStringLiteral("u");
}

QString sceneLengthUnitDescription(const SceneCoordinateInfo &coordinates) {
  if (!coordinates.unitDeclared ||
      coordinates.unit == SceneLengthUnit::Unknown) {
    return QCoreApplication::translate("Workbench", "未声明（原始单位 u）");
  }
  return sceneLengthUnitSymbol(coordinates.unit);
}

QString formatSceneCoordinate(const double value,
                              const SceneCoordinateInfo &coordinates) {
  return QStringLiteral("%1 %2")
      .arg(conciseNumber(value), sceneLengthUnitSymbol(coordinates.unit));
}

QString formatSceneLength(const double value,
                          const SceneCoordinateInfo &coordinates) {
  if (!coordinates.unitDeclared ||
      coordinates.unit == SceneLengthUnit::Unknown) {
    return QStringLiteral("%1 u").arg(conciseNumber(value, 7));
  }
  const double metres = value * metresPerUnit(coordinates.unit);
  if (std::abs(metres) < 0.01) {
    return QStringLiteral("%1 mm").arg(conciseNumber(metres * 1000.0, 6));
  }
  if (std::abs(metres) < 1.0) {
    return QStringLiteral("%1 cm").arg(conciseNumber(metres * 100.0, 6));
  }
  return QStringLiteral("%1 m").arg(conciseNumber(metres, 7));
}

QString formatSceneVector(const SceneCoordinate3D &value,
                          const SceneCoordinateInfo &coordinates) {
  return QStringLiteral("X %1  Y %2  Z %3 %4")
      .arg(conciseNumber(value.x), conciseNumber(value.y),
           conciseNumber(value.z), sceneLengthUnitSymbol(coordinates.unit));
}

QString formatSceneSize(const SceneCoordinateInfo &coordinates) {
  const SceneCoordinate3D size = coordinates.globalSize();
  return QStringLiteral("%1 × %2 × %3 %4")
      .arg(conciseNumber(size.x), conciseNumber(size.y), conciseNumber(size.z),
           sceneLengthUnitSymbol(coordinates.unit));
}

QJsonObject sceneCoordinateInfoToJson(
    const SceneCoordinateInfo &coordinates) {
  QJsonObject object;
  object.insert(QStringLiteral("valid"), coordinates.valid);
  object.insert(QStringLiteral("globalMinimum"),
                vectorToJson(coordinates.globalMinimum));
  object.insert(QStringLiteral("globalMaximum"),
                vectorToJson(coordinates.globalMaximum));
  object.insert(QStringLiteral("displayShift"),
                vectorToJson(coordinates.displayShift));
  object.insert(QStringLiteral("displayScale"), coordinates.displayScale);
  object.insert(QStringLiteral("unit"), unitKey(coordinates.unit));
  object.insert(QStringLiteral("unitDeclared"), coordinates.unitDeclared);
  object.insert(QStringLiteral("coordinateReferenceSystem"),
                coordinates.coordinateReferenceSystem);
  object.insert(QStringLiteral("sourceUsesFloat64"),
                coordinates.sourceUsesFloat64);
  object.insert(QStringLiteral("automaticDisplayShift"),
                coordinates.automaticDisplayShift);
  return object;
}

bool sceneCoordinateInfoFromJson(const QJsonObject &object,
                                 SceneCoordinateInfo &coordinates) {
  SceneCoordinateInfo parsed;
  parsed.valid = object.value(QStringLiteral("valid")).toBool(false);
  if (!parsed.valid ||
      !vectorFromJson(object.value(QStringLiteral("globalMinimum")),
                      parsed.globalMinimum) ||
      !vectorFromJson(object.value(QStringLiteral("globalMaximum")),
                      parsed.globalMaximum) ||
      !vectorFromJson(object.value(QStringLiteral("displayShift")),
                      parsed.displayShift)) {
    return false;
  }
  parsed.displayScale = object.value(QStringLiteral("displayScale")).toDouble();
  parsed.unit = unitFromKey(object.value(QStringLiteral("unit")).toString());
  parsed.unitDeclared =
      object.value(QStringLiteral("unitDeclared")).toBool(false);
  parsed.coordinateReferenceSystem =
      object.value(QStringLiteral("coordinateReferenceSystem")).toString();
  parsed.sourceUsesFloat64 =
      object.value(QStringLiteral("sourceUsesFloat64")).toBool(false);
  parsed.automaticDisplayShift =
      object.value(QStringLiteral("automaticDisplayShift")).toBool(false);
  if (!std::isfinite(parsed.displayScale) || parsed.displayScale <= 0.0 ||
      parsed.globalMinimum.x > parsed.globalMaximum.x ||
      parsed.globalMinimum.y > parsed.globalMaximum.y ||
      parsed.globalMinimum.z > parsed.globalMaximum.z) {
    return false;
  }
  coordinates = parsed;
  return true;
}

bool writeSceneCoordinateReport(
    const QString &destinationPath, const QString &sourcePath,
    const SceneCoordinateInfo &coordinates,
    const double referencePlaneElevation, const QString &referencePlaneMode,
    QString *errorMessage) {
  QString error;
  if (!coordinates.valid) {
    error = QCoreApplication::translate("Workbench", "当前场景没有可导出的坐标范围。");
  }
  QSaveFile file(destinationPath);
  if (error.isEmpty() && !file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    error = QCoreApplication::translate("Workbench", "无法创建坐标报告：%1").arg(file.errorString());
  }

  const SceneCoordinate3D center = coordinates.globalCenter();
  const SceneCoordinate3D size = coordinates.globalSize();
  const bool csv = QFileInfo(destinationPath).suffix().compare(
                       QStringLiteral("csv"), Qt::CaseInsensitive) == 0;
  if (error.isEmpty() && csv) {
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "field,x,y,z,value,unit\n";
    stream << "minimum," << conciseNumber(coordinates.globalMinimum.x) << ','
           << conciseNumber(coordinates.globalMinimum.y) << ','
           << conciseNumber(coordinates.globalMinimum.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "maximum," << conciseNumber(coordinates.globalMaximum.x) << ','
           << conciseNumber(coordinates.globalMaximum.y) << ','
           << conciseNumber(coordinates.globalMaximum.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "center," << conciseNumber(center.x) << ','
           << conciseNumber(center.y) << ',' << conciseNumber(center.z)
           << ",," << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "size," << conciseNumber(size.x) << ','
           << conciseNumber(size.y) << ',' << conciseNumber(size.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "diagonal,,,," << conciseNumber(coordinates.globalDiagonal())
           << ',' << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "display_shift," << conciseNumber(coordinates.displayShift.x)
           << ',' << conciseNumber(coordinates.displayShift.y) << ','
           << conciseNumber(coordinates.displayShift.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "display_scale,,,," << conciseNumber(coordinates.displayScale)
           << ",\n";
    stream << "reference_plane,,,," << conciseNumber(referencePlaneElevation)
           << ',' << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "reference_plane_mode,,,," << csvEscape(referencePlaneMode)
           << ",\n";
    stream << "source,,,," << csvEscape(QDir::toNativeSeparators(sourcePath))
           << ",\n";
    stream << "crs,,,," << csvEscape(coordinates.coordinateReferenceSystem)
           << ",\n";
    stream << "unit_declared,,,,"
           << (coordinates.unitDeclared ? "true" : "false") << ",\n";
    stream.flush();
  } else if (error.isEmpty()) {
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("sourcePath"),
                QDir::toNativeSeparators(sourcePath));
    root.insert(QStringLiteral("coordinates"),
                sceneCoordinateInfoToJson(coordinates));
    root.insert(QStringLiteral("globalCenter"), vectorToJson(center));
    root.insert(QStringLiteral("globalSize"), vectorToJson(size));
    root.insert(QStringLiteral("globalDiagonal"),
                coordinates.globalDiagonal());
    root.insert(QStringLiteral("displayTransformFormula"),
                QStringLiteral("local = (global + displayShift) * displayScale"));
    root.insert(QStringLiteral("referencePlaneMode"), referencePlaneMode);
    root.insert(QStringLiteral("referencePlaneElevation"),
                referencePlaneElevation);
    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
      error = QCoreApplication::translate("Workbench", "无法写入坐标报告：%1").arg(file.errorString());
    }
  }

  if (error.isEmpty() && !file.commit()) {
    error = QCoreApplication::translate("Workbench", "无法完成坐标报告：%1").arg(file.errorString());
  } else if (!error.isEmpty()) {
    file.cancelWriting();
  }
  if (errorMessage != nullptr) {
    *errorMessage = error;
  }
  return error.isEmpty();
}

} // namespace gsw
