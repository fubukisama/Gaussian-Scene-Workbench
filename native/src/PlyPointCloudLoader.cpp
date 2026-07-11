#include "PlyPointCloudLoader.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace gsw {

namespace {
constexpr double kSphericalHarmonicDc = 0.28209479177387814;
constexpr qint64 kMaximumHeaderBytes = 1024 * 1024;

enum class PlyFormat {
  Unknown,
  Ascii,
  BinaryLittleEndian,
};

enum class ScalarType {
  Invalid,
  Int8,
  UInt8,
  Int16,
  UInt16,
  Int32,
  UInt32,
  Float32,
  Float64,
};

struct PropertyDefinition {
  QString name;
  ScalarType valueType = ScalarType::Invalid;
  ScalarType listCountType = ScalarType::Invalid;
  bool isList = false;
};

struct ElementDefinition {
  QString name;
  qint64 count = 0;
  QVector<PropertyDefinition> properties;
};

struct PlyHeader {
  PlyFormat format = PlyFormat::Unknown;
  QVector<ElementDefinition> elements;
};

ScalarType scalarTypeFromName(const QString &name) {
  const QString lower = name.toLower();
  if (lower == QStringLiteral("char") || lower == QStringLiteral("int8")) {
    return ScalarType::Int8;
  }
  if (lower == QStringLiteral("uchar") || lower == QStringLiteral("uint8")) {
    return ScalarType::UInt8;
  }
  if (lower == QStringLiteral("short") || lower == QStringLiteral("int16")) {
    return ScalarType::Int16;
  }
  if (lower == QStringLiteral("ushort") || lower == QStringLiteral("uint16")) {
    return ScalarType::UInt16;
  }
  if (lower == QStringLiteral("int") || lower == QStringLiteral("int32")) {
    return ScalarType::Int32;
  }
  if (lower == QStringLiteral("uint") || lower == QStringLiteral("uint32")) {
    return ScalarType::UInt32;
  }
  if (lower == QStringLiteral("float") || lower == QStringLiteral("float32")) {
    return ScalarType::Float32;
  }
  if (lower == QStringLiteral("double") || lower == QStringLiteral("float64")) {
    return ScalarType::Float64;
  }
  return ScalarType::Invalid;
}

bool isIntegralType(const ScalarType type) {
  return type == ScalarType::Int8 || type == ScalarType::UInt8 ||
         type == ScalarType::Int16 || type == ScalarType::UInt16 ||
         type == ScalarType::Int32 || type == ScalarType::UInt32;
}

double colorMaximum(const ScalarType type) {
  switch (type) {
  case ScalarType::Int8:
    return 127.0;
  case ScalarType::UInt8:
    return 255.0;
  case ScalarType::Int16:
    return 32767.0;
  case ScalarType::UInt16:
    return 65535.0;
  case ScalarType::Int32:
    return 2147483647.0;
  case ScalarType::UInt32:
    return 4294967295.0;
  default:
    return 1.0;
  }
}

bool parseHeader(QFile &file, PlyHeader &header, QString &error) {
  qint64 consumed = 0;
  bool firstLine = true;
  ElementDefinition *currentElement = nullptr;

  while (!file.atEnd() && consumed < kMaximumHeaderBytes) {
    const QByteArray rawLine = file.readLine();
    consumed += rawLine.size();
    const QString line = QString::fromLatin1(rawLine).trimmed();
    if (firstLine) {
      firstLine = false;
      if (line != QStringLiteral("ply")) {
        error = QStringLiteral("The selected file is not a PLY file.");
        return false;
      }
      continue;
    }

    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.first() == QStringLiteral("comment") ||
        parts.first() == QStringLiteral("obj_info")) {
      continue;
    }
    if (parts.first() == QStringLiteral("format") && parts.size() >= 2) {
      if (parts.at(1) == QStringLiteral("ascii")) {
        header.format = PlyFormat::Ascii;
      } else if (parts.at(1) == QStringLiteral("binary_little_endian")) {
        header.format = PlyFormat::BinaryLittleEndian;
      } else {
        error = QStringLiteral("Unsupported PLY format: %1").arg(parts.at(1));
        return false;
      }
      continue;
    }
    if (parts.first() == QStringLiteral("element") && parts.size() >= 3) {
      bool ok = false;
      const qint64 count = parts.at(2).toLongLong(&ok);
      if (!ok || count < 0) {
        error = QStringLiteral("Invalid PLY element count: %1").arg(parts.at(2));
        return false;
      }
      header.elements.append({parts.at(1), count, {}});
      currentElement = &header.elements.last();
      continue;
    }
    if (parts.first() == QStringLiteral("property") && currentElement != nullptr) {
      PropertyDefinition property;
      if (parts.size() >= 5 && parts.at(1) == QStringLiteral("list")) {
        property.isList = true;
        property.listCountType = scalarTypeFromName(parts.at(2));
        property.valueType = scalarTypeFromName(parts.at(3));
        property.name = parts.at(4);
      } else if (parts.size() >= 3) {
        property.valueType = scalarTypeFromName(parts.at(1));
        property.name = parts.at(2);
      }
      if (property.valueType == ScalarType::Invalid ||
          (property.isList && property.listCountType == ScalarType::Invalid)) {
        error = QStringLiteral("Unsupported PLY property declaration: %1").arg(line);
        return false;
      }
      currentElement->properties.append(property);
      continue;
    }
    if (parts.first() == QStringLiteral("end_header")) {
      if (header.format == PlyFormat::Unknown) {
        error = QStringLiteral("The PLY header does not declare a supported format.");
        return false;
      }
      return true;
    }
  }

  error = QStringLiteral("The PLY header is incomplete or too large.");
  return false;
}

template <typename UnsignedType>
std::optional<UnsignedType> readLittleEndian(QFile &file) {
  char bytes[sizeof(UnsignedType)]{};
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) {
    return std::nullopt;
  }
  return qFromLittleEndian<UnsignedType>(reinterpret_cast<const uchar *>(bytes));
}

bool readBinaryScalar(QFile &file, const ScalarType type, double &value) {
  switch (type) {
  case ScalarType::Int8: {
    char raw = 0;
    if (file.read(&raw, 1) != 1) {
      return false;
    }
    value = static_cast<qint8>(raw);
    return true;
  }
  case ScalarType::UInt8: {
    char raw = 0;
    if (file.read(&raw, 1) != 1) {
      return false;
    }
    value = static_cast<quint8>(raw);
    return true;
  }
  case ScalarType::Int16: {
    const auto raw = readLittleEndian<quint16>(file);
    if (!raw.has_value()) {
      return false;
    }
    value = static_cast<qint16>(*raw);
    return true;
  }
  case ScalarType::UInt16: {
    const auto raw = readLittleEndian<quint16>(file);
    if (!raw.has_value()) {
      return false;
    }
    value = *raw;
    return true;
  }
  case ScalarType::Int32: {
    const auto raw = readLittleEndian<quint32>(file);
    if (!raw.has_value()) {
      return false;
    }
    value = static_cast<qint32>(*raw);
    return true;
  }
  case ScalarType::UInt32: {
    const auto raw = readLittleEndian<quint32>(file);
    if (!raw.has_value()) {
      return false;
    }
    value = *raw;
    return true;
  }
  case ScalarType::Float32: {
    const auto raw = readLittleEndian<quint32>(file);
    if (!raw.has_value()) {
      return false;
    }
    value = std::bit_cast<float>(*raw);
    return true;
  }
  case ScalarType::Float64: {
    const auto raw = readLittleEndian<quint64>(file);
    if (!raw.has_value()) {
      return false;
    }
    value = std::bit_cast<double>(*raw);
    return true;
  }
  case ScalarType::Invalid:
    return false;
  }
  return false;
}

int findProperty(const ElementDefinition &element, const QStringList &names) {
  for (qsizetype index = 0; index < element.properties.size(); ++index) {
    if (names.contains(element.properties.at(index).name, Qt::CaseInsensitive)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

float normalizedColor(const double value, const ScalarType type) {
  double normalized = value;
  if (isIntegralType(type)) {
    normalized /= colorMaximum(type);
  } else if (normalized > 1.0) {
    normalized /= 255.0;
  }
  return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

bool shouldSampleVertex(const qint64 vertexIndex, const qint64 sourceCount,
                        const qsizetype sampleCount, qsizetype &nextSampleIndex) {
  if (nextSampleIndex >= sampleCount || sourceCount <= 0) {
    return false;
  }
  const qint64 targetIndex = static_cast<qint64>(
      (static_cast<long double>(nextSampleIndex) * sourceCount) / sampleCount);
  if (vertexIndex != targetIndex) {
    return false;
  }
  ++nextSampleIndex;
  return true;
}

bool appendVertex(const ElementDefinition &element, const QVector<double> &values,
                  const int xIndex, const int yIndex, const int zIndex,
                  const int redIndex, const int greenIndex, const int blueIndex,
                  const bool sphericalHarmonicColor, PointCloudData &result) {
  const double x = values.at(xIndex);
  const double y = values.at(yIndex);
  const double z = values.at(zIndex);
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }

  PointCloudVertex vertex;
  vertex.x = static_cast<float>(x);
  vertex.y = static_cast<float>(y);
  vertex.z = static_cast<float>(z);
  if (redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0) {
    if (sphericalHarmonicColor) {
      vertex.red = static_cast<float>(std::clamp(0.5 + kSphericalHarmonicDc * values.at(redIndex), 0.0, 1.0));
      vertex.green = static_cast<float>(std::clamp(0.5 + kSphericalHarmonicDc * values.at(greenIndex), 0.0, 1.0));
      vertex.blue = static_cast<float>(std::clamp(0.5 + kSphericalHarmonicDc * values.at(blueIndex), 0.0, 1.0));
    } else {
      vertex.red = normalizedColor(values.at(redIndex), element.properties.at(redIndex).valueType);
      vertex.green = normalizedColor(values.at(greenIndex), element.properties.at(greenIndex).valueType);
      vertex.blue = normalizedColor(values.at(blueIndex), element.properties.at(blueIndex).valueType);
    }
  }

  const QVector3D position(vertex.x, vertex.y, vertex.z);
  if (result.vertices.isEmpty()) {
    result.boundsMinimum = position;
    result.boundsMaximum = position;
  } else {
    result.boundsMinimum.setX(std::min(result.boundsMinimum.x(), vertex.x));
    result.boundsMinimum.setY(std::min(result.boundsMinimum.y(), vertex.y));
    result.boundsMinimum.setZ(std::min(result.boundsMinimum.z(), vertex.z));
    result.boundsMaximum.setX(std::max(result.boundsMaximum.x(), vertex.x));
    result.boundsMaximum.setY(std::max(result.boundsMaximum.y(), vertex.y));
    result.boundsMaximum.setZ(std::max(result.boundsMaximum.z(), vertex.z));
  }
  result.vertices.append(vertex);
  return true;
}

bool readAsciiElementRecord(QFile &file, const ElementDefinition &element,
                            QVector<double> &values, QString &error) {
  QByteArray rawLine;
  do {
    if (file.atEnd()) {
      error = QStringLiteral("Unexpected end of ASCII PLY data.");
      return false;
    }
    rawLine = file.readLine().trimmed();
  } while (rawLine.isEmpty());

  rawLine.replace('\t', ' ');
  const QList<QByteArray> tokens = rawLine.split(' ');
  qsizetype tokenIndex = 0;
  values.fill(0.0, element.properties.size());
  for (qsizetype propertyIndex = 0; propertyIndex < element.properties.size(); ++propertyIndex) {
    const PropertyDefinition &property = element.properties.at(propertyIndex);
    while (tokenIndex < tokens.size() && tokens.at(tokenIndex).isEmpty()) {
      ++tokenIndex;
    }
    if (tokenIndex >= tokens.size()) {
      error = QStringLiteral("ASCII PLY record has fewer values than its header declares.");
      return false;
    }
    bool ok = false;
    const double first = tokens.at(tokenIndex++).toDouble(&ok);
    if (!ok) {
      error = QStringLiteral("ASCII PLY record contains an invalid number.");
      return false;
    }
    if (!property.isList) {
      values[propertyIndex] = first;
      continue;
    }
    const qint64 listCount = static_cast<qint64>(first);
    if (listCount < 0 || tokenIndex + listCount > tokens.size()) {
      error = QStringLiteral("ASCII PLY list property is truncated.");
      return false;
    }
    tokenIndex += listCount;
  }
  return true;
}

bool readBinaryElementRecord(QFile &file, const ElementDefinition &element,
                             QVector<double> &values, QString &error) {
  values.fill(0.0, element.properties.size());
  for (qsizetype propertyIndex = 0; propertyIndex < element.properties.size(); ++propertyIndex) {
    const PropertyDefinition &property = element.properties.at(propertyIndex);
    if (!property.isList) {
      if (!readBinaryScalar(file, property.valueType, values[propertyIndex])) {
        error = QStringLiteral("Unexpected end of binary PLY data.");
        return false;
      }
      continue;
    }
    double countValue = 0.0;
    if (!readBinaryScalar(file, property.listCountType, countValue)) {
      error = QStringLiteral("Unexpected end of binary PLY list data.");
      return false;
    }
    const qint64 listCount = static_cast<qint64>(countValue);
    if (listCount < 0 || listCount > 100'000'000) {
      error = QStringLiteral("Invalid binary PLY list length.");
      return false;
    }
    double ignored = 0.0;
    for (qint64 index = 0; index < listCount; ++index) {
      if (!readBinaryScalar(file, property.valueType, ignored)) {
        error = QStringLiteral("Unexpected end of binary PLY list data.");
        return false;
      }
    }
  }
  return true;
}
} // namespace

bool PointCloudData::isValid() const {
  return error.isEmpty() && sourceVertexCount >= 0 && !vertices.isEmpty();
}

QVector3D PointCloudData::center() const {
  return (boundsMinimum + boundsMaximum) * 0.5F;
}

float PointCloudData::radius() const {
  return std::max((boundsMaximum - boundsMinimum).length() * 0.5F, 0.001F);
}

PointCloudData PlyPointCloudLoader::load(const QString &filePath,
                                         const qsizetype maximumPreviewPoints) {
  PointCloudData result;
  if (maximumPreviewPoints <= 0) {
    result.error = QStringLiteral("The point preview limit must be greater than zero.");
    return result;
  }

  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    result.error = QStringLiteral("Unable to open PLY file %1: %2")
                       .arg(QFileInfo(filePath).fileName(), file.errorString());
    return result;
  }

  PlyHeader header;
  if (!parseHeader(file, header, result.error)) {
    return result;
  }

  auto vertexElementIterator = std::find_if(
      header.elements.cbegin(), header.elements.cend(),
      [](const ElementDefinition &element) { return element.name == QStringLiteral("vertex"); });
  if (vertexElementIterator == header.elements.cend() || vertexElementIterator->count <= 0) {
    result.error = QStringLiteral("The PLY file does not contain any vertices.");
    return result;
  }
  const ElementDefinition &vertexElement = *vertexElementIterator;
  result.sourceVertexCount = vertexElement.count;

  const int xIndex = findProperty(vertexElement, {QStringLiteral("x")});
  const int yIndex = findProperty(vertexElement, {QStringLiteral("y")});
  const int zIndex = findProperty(vertexElement, {QStringLiteral("z")});
  if (xIndex < 0 || yIndex < 0 || zIndex < 0 ||
      vertexElement.properties.at(xIndex).isList ||
      vertexElement.properties.at(yIndex).isList ||
      vertexElement.properties.at(zIndex).isList) {
    result.error = QStringLiteral("The PLY vertex element must contain scalar x, y, and z properties.");
    return result;
  }

  int redIndex = findProperty(vertexElement, {QStringLiteral("red"), QStringLiteral("r"),
                                               QStringLiteral("diffuse_red")});
  int greenIndex = findProperty(vertexElement, {QStringLiteral("green"), QStringLiteral("g"),
                                                 QStringLiteral("diffuse_green")});
  int blueIndex = findProperty(vertexElement, {QStringLiteral("blue"), QStringLiteral("b"),
                                                QStringLiteral("diffuse_blue")});
  bool sphericalHarmonicColor = false;
  if (redIndex < 0 || greenIndex < 0 || blueIndex < 0) {
    redIndex = findProperty(vertexElement, {QStringLiteral("f_dc_0")});
    greenIndex = findProperty(vertexElement, {QStringLiteral("f_dc_1")});
    blueIndex = findProperty(vertexElement, {QStringLiteral("f_dc_2")});
    sphericalHarmonicColor = redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0;
  }

  const qsizetype sampleCount = static_cast<qsizetype>(
      std::min<qint64>(vertexElement.count, maximumPreviewPoints));
  result.vertices.reserve(sampleCount);
  qsizetype nextSampleIndex = 0;

  for (const ElementDefinition &element : header.elements) {
    QVector<double> values(element.properties.size());
    for (qint64 recordIndex = 0; recordIndex < element.count; ++recordIndex) {
      const bool read = header.format == PlyFormat::Ascii
                            ? readAsciiElementRecord(file, element, values, result.error)
                            : readBinaryElementRecord(file, element, values, result.error);
      if (!read) {
        result.vertices.clear();
        return result;
      }
      if (element.name != QStringLiteral("vertex") ||
          !shouldSampleVertex(recordIndex, vertexElement.count, sampleCount, nextSampleIndex)) {
        continue;
      }
      appendVertex(element, values, xIndex, yIndex, zIndex, redIndex, greenIndex,
                   blueIndex, sphericalHarmonicColor, result);
    }
  }

  if (result.vertices.isEmpty()) {
    result.error = QStringLiteral("The PLY file contains no finite preview vertices.");
  }
  return result;
}

} // namespace gsw
