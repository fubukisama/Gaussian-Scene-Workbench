#include "PlyPointCloudLoader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <system_error>

namespace gsw {

namespace {
constexpr double kSphericalHarmonicDc = 0.28209479177387814;
constexpr qint64 kMaximumHeaderBytes = 1024 * 1024;
constexpr qint64 kMaximumRecordBytes = 256LL * 1024LL * 1024LL;
constexpr qint64 kMaximumMeshPreviewFaces = 2'000'000;
constexpr qint64 kMaximumMeshPreviewTriangles = 5'000'000;
constexpr qsizetype kFullResolutionPointChunkSize = 1'000'000;
constexpr qsizetype kAsciiSpoolBufferPoints = 262'144;
constexpr qint64 kStaleAsciiSpoolMilliseconds =
    24LL * 60LL * 60LL * 1000LL;

enum class PlyFormat {
  Unknown,
  Ascii,
  BinaryLittleEndian,
  BinaryBigEndian,
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

bool isAsciiWhitespace(const char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
         value == '\v' || value == '\f';
}

class BufferedAsciiScalarReader final {
public:
  BufferedAsciiScalarReader(QFile &file, const qint64 offset) : mFile(file) {
    constexpr qsizetype kBufferBytes = 8 * 1024 * 1024;
    mBuffer.resize(kBufferBytes);
    if (!mFile.seek(offset)) {
      mFailed = true;
    } else {
      refill();
    }
  }

  [[nodiscard]] bool next(double &value) {
    while (true) {
      while (mPosition < mSize &&
             isAsciiWhitespace(mBuffer.at(mPosition))) {
        ++mPosition;
      }
      if (mPosition < mSize) {
        break;
      }
      if (mAtEnd || !refill()) {
        return false;
      }
    }

    while (true) {
      qsizetype tokenEnd = mPosition;
      while (tokenEnd < mSize &&
             !isAsciiWhitespace(mBuffer.at(tokenEnd))) {
        ++tokenEnd;
      }
      if (tokenEnd < mSize || mAtEnd) {
        const char *tokenBegin = mBuffer.constData() + mPosition;
        const char *tokenLimit = mBuffer.constData() + tokenEnd;
        const bool explicitPositive =
            tokenBegin < tokenLimit && *tokenBegin == '+';
        const char *numberBegin = tokenBegin + (explicitPositive ? 1 : 0);
        if (numberBegin >= tokenLimit) {
          return false;
        }
        const auto parsed = std::from_chars(numberBegin, tokenLimit, value,
                                            std::chars_format::general);
        if (parsed.ec != std::errc() || parsed.ptr != tokenLimit) {
          return false;
        }
        mPosition = tokenEnd;
        return true;
      }
      if (!refill()) {
        return false;
      }
    }
  }

private:
  bool refill() {
    if (mFailed || mAtEnd) {
      return false;
    }
    const qsizetype remaining = mSize - mPosition;
    if (remaining >= mBuffer.size()) {
      mFailed = true;
      return false;
    }
    if (remaining > 0 && mPosition > 0) {
      std::memmove(mBuffer.data(), mBuffer.constData() + mPosition,
                   static_cast<std::size_t>(remaining));
    }
    mPosition = 0;
    mSize = remaining;
    const qint64 read = mFile.read(mBuffer.data() + mSize,
                                   mBuffer.size() - mSize);
    if (read < 0) {
      mFailed = true;
      return false;
    }
    if (read == 0) {
      mAtEnd = true;
      return remaining > 0;
    }
    mSize += static_cast<qsizetype>(read);
    return true;
  }

  QFile &mFile;
  QByteArray mBuffer;
  qsizetype mPosition = 0;
  qsizetype mSize = 0;
  bool mAtEnd = false;
  bool mFailed = false;
};

class BufferedBinaryScalarReader final {
public:
  BufferedBinaryScalarReader(QFile &file, const qint64 offset,
                             const PlyFormat format)
      : mFile(file), mBigEndian(format == PlyFormat::BinaryBigEndian) {
    constexpr qsizetype kBufferBytes = 8 * 1024 * 1024;
    mBuffer.resize(kBufferBytes);
    if (!mFile.seek(offset)) {
      mFailed = true;
    } else {
      refill();
    }
  }

  [[nodiscard]] bool next(const ScalarType type, double &value) {
    const qsizetype byteCount = sizeOf(type);
    if (byteCount <= 0 || !ensure(byteCount)) {
      return false;
    }
    const auto *data = reinterpret_cast<const uchar *>(mBuffer.constData() +
                                                       mPosition);
    switch (type) {
    case ScalarType::Int8:
      value = static_cast<qint8>(*data);
      break;
    case ScalarType::UInt8:
      value = *data;
      break;
    case ScalarType::Int16: {
      const quint16 raw = mBigEndian ? qFromBigEndian<quint16>(data)
                                     : qFromLittleEndian<quint16>(data);
      value = static_cast<qint16>(raw);
      break;
    }
    case ScalarType::UInt16:
      value = mBigEndian ? qFromBigEndian<quint16>(data)
                         : qFromLittleEndian<quint16>(data);
      break;
    case ScalarType::Int32: {
      const quint32 raw = mBigEndian ? qFromBigEndian<quint32>(data)
                                     : qFromLittleEndian<quint32>(data);
      value = static_cast<qint32>(raw);
      break;
    }
    case ScalarType::UInt32:
      value = mBigEndian ? qFromBigEndian<quint32>(data)
                         : qFromLittleEndian<quint32>(data);
      break;
    case ScalarType::Float32: {
      const quint32 raw = mBigEndian ? qFromBigEndian<quint32>(data)
                                     : qFromLittleEndian<quint32>(data);
      value = std::bit_cast<float>(raw);
      break;
    }
    case ScalarType::Float64: {
      const quint64 raw = mBigEndian ? qFromBigEndian<quint64>(data)
                                     : qFromLittleEndian<quint64>(data);
      value = std::bit_cast<double>(raw);
      break;
    }
    case ScalarType::Invalid:
      return false;
    }
    mPosition += byteCount;
    return true;
  }

private:
  static qsizetype sizeOf(const ScalarType type) {
    switch (type) {
    case ScalarType::Int8:
    case ScalarType::UInt8:
      return 1;
    case ScalarType::Int16:
    case ScalarType::UInt16:
      return 2;
    case ScalarType::Int32:
    case ScalarType::UInt32:
    case ScalarType::Float32:
      return 4;
    case ScalarType::Float64:
      return 8;
    case ScalarType::Invalid:
      return 0;
    }
    return 0;
  }

  bool ensure(const qsizetype byteCount) {
    while (mSize - mPosition < byteCount) {
      if (!refill()) {
        return false;
      }
    }
    return true;
  }

  bool refill() {
    if (mFailed || mAtEnd) {
      return false;
    }
    const qsizetype remaining = mSize - mPosition;
    if (remaining > 0 && mPosition > 0) {
      std::memmove(mBuffer.data(), mBuffer.constData() + mPosition,
                   static_cast<std::size_t>(remaining));
    }
    mPosition = 0;
    mSize = remaining;
    const qint64 read =
        mFile.read(mBuffer.data() + mSize, mBuffer.size() - mSize);
    if (read < 0) {
      mFailed = true;
      return false;
    }
    if (read == 0) {
      mAtEnd = true;
      return false;
    }
    mSize += static_cast<qsizetype>(read);
    return true;
  }

  QFile &mFile;
  QByteArray mBuffer;
  qsizetype mPosition = 0;
  qsizetype mSize = 0;
  bool mBigEndian = false;
  bool mAtEnd = false;
  bool mFailed = false;
};

struct PlyHeader {
  PlyFormat format = PlyFormat::Unknown;
  QVector<ElementDefinition> elements;
  QVector<QByteArray> rawLines;
  qsizetype vertexElementLine = -1;
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
    header.rawLines.append(rawLine);
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
      } else if (parts.at(1) == QStringLiteral("binary_big_endian")) {
        header.format = PlyFormat::BinaryBigEndian;
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
      if (parts.at(1) == QStringLiteral("vertex")) {
        header.vertexElementLine = header.rawLines.size() - 1;
      }
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
          (property.isList &&
           (property.listCountType == ScalarType::Invalid ||
            !isIntegralType(property.listCountType)))) {
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

qsizetype scalarByteSize(const ScalarType type) {
  switch (type) {
  case ScalarType::Int8:
  case ScalarType::UInt8:
    return 1;
  case ScalarType::Int16:
  case ScalarType::UInt16:
    return 2;
  case ScalarType::Int32:
  case ScalarType::UInt32:
  case ScalarType::Float32:
    return 4;
  case ScalarType::Float64:
    return 8;
  case ScalarType::Invalid:
    return 0;
  }
  return 0;
}

std::optional<qint64> binaryListCount(const QByteArray &bytes,
                                      const ScalarType type,
                                      const PlyFormat format) {
  const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
  const bool bigEndian = format == PlyFormat::BinaryBigEndian;
  switch (type) {
  case ScalarType::Int8:
    return static_cast<qint8>(bytes.at(0));
  case ScalarType::UInt8:
    return static_cast<quint8>(bytes.at(0));
  case ScalarType::Int16:
    return static_cast<qint16>(bigEndian ? qFromBigEndian<quint16>(data)
                                        : qFromLittleEndian<quint16>(data));
  case ScalarType::UInt16:
    return bigEndian ? qFromBigEndian<quint16>(data)
                     : qFromLittleEndian<quint16>(data);
  case ScalarType::Int32:
    return static_cast<qint32>(bigEndian ? qFromBigEndian<quint32>(data)
                                        : qFromLittleEndian<quint32>(data));
  case ScalarType::UInt32:
    return bigEndian ? qFromBigEndian<quint32>(data)
                     : qFromLittleEndian<quint32>(data);
  case ScalarType::Float32:
  case ScalarType::Float64:
  case ScalarType::Invalid:
    return std::nullopt;
  }
  return std::nullopt;
}

bool appendExactBytes(QFile &file, const qint64 byteCount, QByteArray &record,
                      QString &error) {
  if (byteCount < 0 || byteCount > kMaximumRecordBytes) {
    error = QStringLiteral("PLY record is too large to process safely.");
    return false;
  }
  const QByteArray bytes = file.read(byteCount);
  if (bytes.size() != byteCount) {
    error = QStringLiteral("Unexpected end of binary PLY data while exporting.");
    return false;
  }
  record.append(bytes);
  return true;
}

bool readBinaryRawRecord(QFile &file, const ElementDefinition &element,
                         const PlyFormat format, QByteArray &record,
                         QString &error) {
  record.clear();
  for (const PropertyDefinition &property : element.properties) {
    if (!property.isList) {
      if (!appendExactBytes(file, scalarByteSize(property.valueType), record, error)) {
        return false;
      }
      continue;
    }

    const qsizetype countBytes = scalarByteSize(property.listCountType);
    const QByteArray rawCount = file.read(countBytes);
    if (rawCount.size() != countBytes) {
      error = QStringLiteral("Unexpected end of binary PLY list data while exporting.");
      return false;
    }
    record.append(rawCount);
    const std::optional<qint64> count =
        binaryListCount(rawCount, property.listCountType, format);
    if (!count.has_value() || *count < 0) {
      error = QStringLiteral("PLY list counts must use a non-negative integer type.");
      return false;
    }
    const qint64 valueSize = scalarByteSize(property.valueType);
    if (valueSize <= 0 || *count > kMaximumRecordBytes / valueSize ||
        !appendExactBytes(file, *count * valueSize, record, error)) {
      if (error.isEmpty()) {
        error = QStringLiteral("PLY list record is too large to process safely.");
      }
      return false;
    }
  }
  return true;
}

bool readAsciiRawRecord(QFile &file, QByteArray &record, QString &error) {
  record.clear();
  while (!file.atEnd()) {
    const QByteArray line = file.readLine();
    record.append(line);
    if (record.size() > kMaximumRecordBytes) {
      error = QStringLiteral("ASCII PLY record is too large to process safely.");
      return false;
    }
    if (!line.trimmed().isEmpty()) {
      return true;
    }
  }
  error = QStringLiteral("Unexpected end of ASCII PLY data while exporting.");
  return false;
}

bool writeBytes(QIODevice &destination, const QByteArray &bytes, QString &error) {
  if (destination.write(bytes) == bytes.size()) {
    return true;
  }
  error = QStringLiteral("Unable to write the cropped PLY file.");
  return false;
}

template <typename UnsignedType>
std::optional<UnsignedType> readEndian(QFile &file, const PlyFormat format) {
  char bytes[sizeof(UnsignedType)]{};
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) {
    return std::nullopt;
  }
  const auto *data = reinterpret_cast<const uchar *>(bytes);
  return format == PlyFormat::BinaryBigEndian
             ? qFromBigEndian<UnsignedType>(data)
             : qFromLittleEndian<UnsignedType>(data);
}

bool readBinaryScalar(QFile &file, const ScalarType type,
                      const PlyFormat format, double &value) {
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
    const auto raw = readEndian<quint16>(file, format);
    if (!raw.has_value()) {
      return false;
    }
    value = static_cast<qint16>(*raw);
    return true;
  }
  case ScalarType::UInt16: {
    const auto raw = readEndian<quint16>(file, format);
    if (!raw.has_value()) {
      return false;
    }
    value = *raw;
    return true;
  }
  case ScalarType::Int32: {
    const auto raw = readEndian<quint32>(file, format);
    if (!raw.has_value()) {
      return false;
    }
    value = static_cast<qint32>(*raw);
    return true;
  }
  case ScalarType::UInt32: {
    const auto raw = readEndian<quint32>(file, format);
    if (!raw.has_value()) {
      return false;
    }
    value = *raw;
    return true;
  }
  case ScalarType::Float32: {
    const auto raw = readEndian<quint32>(file, format);
    if (!raw.has_value()) {
      return false;
    }
    value = std::bit_cast<float>(*raw);
    return true;
  }
  case ScalarType::Float64: {
    const auto raw = readEndian<quint64>(file, format);
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

float activatedOpacity(const double logit) {
  if (std::isnan(logit)) {
    return 0.0F;
  }
  const double bounded = std::clamp(logit, -80.0, 80.0);
  if (bounded >= 0.0) {
    return static_cast<float>(1.0 / (1.0 + std::exp(-bounded)));
  }
  const double exponential = std::exp(bounded);
  return static_cast<float>(exponential / (1.0 + exponential));
}

float activatedScale(const double logarithmicScale) {
  if (std::isnan(logarithmicScale)) {
    return std::exp(-20.0F);
  }
  return static_cast<float>(
      std::exp(std::clamp(logarithmicScale, -20.0, 20.0)));
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
                  const bool sphericalHarmonicColor, const int opacityIndex,
                  const std::array<int, 3> &scaleIndices,
                  const std::array<int, 4> &rotationIndices,
                  const bool hasGaussianAttributes, const qint64 sourceIndex,
                  const bool appendPreview, bool &hasFiniteBounds,
                  PointCloudData &result) {
  const double x = values.at(xIndex);
  const double y = values.at(yIndex);
  const double z = values.at(zIndex);
  result.sourcePositions.append(
      {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }

  const QVector3D position(static_cast<float>(x), static_cast<float>(y),
                           static_cast<float>(z));
  if (!hasFiniteBounds) {
    result.boundsMinimum = position;
    result.boundsMaximum = position;
    hasFiniteBounds = true;
  } else {
    result.boundsMinimum.setX(std::min(result.boundsMinimum.x(), position.x()));
    result.boundsMinimum.setY(std::min(result.boundsMinimum.y(), position.y()));
    result.boundsMinimum.setZ(std::min(result.boundsMinimum.z(), position.z()));
    result.boundsMaximum.setX(std::max(result.boundsMaximum.x(), position.x()));
    result.boundsMaximum.setY(std::max(result.boundsMaximum.y(), position.y()));
    result.boundsMaximum.setZ(std::max(result.boundsMaximum.z(), position.z()));
  }
  if (!appendPreview) {
    return true;
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
  if (hasGaussianAttributes) {
    vertex.opacity = activatedOpacity(values.at(opacityIndex));
    vertex.scaleX = activatedScale(values.at(scaleIndices.at(0)));
    vertex.scaleY = activatedScale(values.at(scaleIndices.at(1)));
    vertex.scaleZ = activatedScale(values.at(scaleIndices.at(2)));

    const double rotationW = values.at(rotationIndices.at(0));
    const double rotationX = values.at(rotationIndices.at(1));
    const double rotationY = values.at(rotationIndices.at(2));
    const double rotationZ = values.at(rotationIndices.at(3));
    const double normSquared = rotationW * rotationW + rotationX * rotationX +
                               rotationY * rotationY + rotationZ * rotationZ;
    if (std::isfinite(normSquared) && normSquared > 1.0e-20) {
      const double inverseNorm = 1.0 / std::sqrt(normSquared);
      vertex.rotationW = static_cast<float>(rotationW * inverseNorm);
      vertex.rotationX = static_cast<float>(rotationX * inverseNorm);
      vertex.rotationY = static_cast<float>(rotationY * inverseNorm);
      vertex.rotationZ = static_cast<float>(rotationZ * inverseNorm);
    }
  }
  vertex.sourceIndex = static_cast<quint32>(sourceIndex);
  result.vertices.append(vertex);
  return true;
}

bool readAsciiElementRecord(QFile &file, const ElementDefinition &element,
                            QVector<double> &values,
                            QVector<QVector<double>> &listValues,
                            QString &error) {
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
  listValues.clear();
  listValues.resize(element.properties.size());
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
    if (!std::isfinite(first) || first < 0.0 || std::floor(first) != first) {
      error = QStringLiteral("ASCII PLY list length is invalid.");
      return false;
    }
    const qint64 listCount = static_cast<qint64>(first);
    if (listCount > 100'000'000 || tokenIndex + listCount > tokens.size()) {
      error = QStringLiteral("ASCII PLY list property is truncated.");
      return false;
    }
    QVector<double> &items = listValues[propertyIndex];
    items.reserve(static_cast<qsizetype>(listCount));
    for (qint64 index = 0; index < listCount; ++index) {
      const double value = tokens.at(tokenIndex++).toDouble(&ok);
      if (!ok) {
        error = QStringLiteral("ASCII PLY list contains an invalid number.");
        return false;
      }
      items.append(value);
    }
  }
  return true;
}

bool readBinaryElementRecord(QFile &file, const ElementDefinition &element,
                             const PlyFormat format, QVector<double> &values,
                             QVector<QVector<double>> &listValues,
                             QString &error) {
  values.fill(0.0, element.properties.size());
  listValues.clear();
  listValues.resize(element.properties.size());
  for (qsizetype propertyIndex = 0; propertyIndex < element.properties.size(); ++propertyIndex) {
    const PropertyDefinition &property = element.properties.at(propertyIndex);
    if (!property.isList) {
      if (!readBinaryScalar(file, property.valueType, format,
                            values[propertyIndex])) {
        error = QStringLiteral("Unexpected end of binary PLY data.");
        return false;
      }
      continue;
    }
    double countValue = 0.0;
    if (!readBinaryScalar(file, property.listCountType, format, countValue)) {
      error = QStringLiteral("Unexpected end of binary PLY list data.");
      return false;
    }
    if (!std::isfinite(countValue) || countValue < 0.0 ||
        std::floor(countValue) != countValue) {
      error = QStringLiteral("Invalid binary PLY list length.");
      return false;
    }
    const qint64 listCount = static_cast<qint64>(countValue);
    if (listCount > 100'000'000) {
      error = QStringLiteral("Invalid binary PLY list length.");
      return false;
    }
    QVector<double> &items = listValues[propertyIndex];
    items.reserve(static_cast<qsizetype>(listCount));
    for (qint64 index = 0; index < listCount; ++index) {
      double value = 0.0;
      if (!readBinaryScalar(file, property.valueType, format, value)) {
        error = QStringLiteral("Unexpected end of binary PLY list data.");
        return false;
      }
      items.append(value);
    }
  }
  return true;
}

std::optional<double> scalarFromMemory(const uchar *data,
                                       const ScalarType type,
                                       const PlyFormat format) {
  const bool bigEndian = format == PlyFormat::BinaryBigEndian;
  switch (type) {
  case ScalarType::Int8:
    return static_cast<qint8>(*data);
  case ScalarType::UInt8:
    return *data;
  case ScalarType::Int16:
    return static_cast<qint16>(bigEndian ? qFromBigEndian<quint16>(data)
                                        : qFromLittleEndian<quint16>(data));
  case ScalarType::UInt16:
    return bigEndian ? qFromBigEndian<quint16>(data)
                     : qFromLittleEndian<quint16>(data);
  case ScalarType::Int32:
    return static_cast<qint32>(bigEndian ? qFromBigEndian<quint32>(data)
                                        : qFromLittleEndian<quint32>(data));
  case ScalarType::UInt32:
    return bigEndian ? qFromBigEndian<quint32>(data)
                     : qFromLittleEndian<quint32>(data);
  case ScalarType::Float32: {
    const quint32 bits = bigEndian ? qFromBigEndian<quint32>(data)
                                   : qFromLittleEndian<quint32>(data);
    return std::bit_cast<float>(bits);
  }
  case ScalarType::Float64: {
    const quint64 bits = bigEndian ? qFromBigEndian<quint64>(data)
                                   : qFromLittleEndian<quint64>(data);
    return std::bit_cast<double>(bits);
  }
  case ScalarType::Invalid:
    return std::nullopt;
  }
  return std::nullopt;
}

quint8 packedColor(const double value, const ScalarType type) {
  return static_cast<quint8>(
      std::clamp(qRound(normalizedColor(value, type) * 255.0F), 0, 255));
}

bool writePointSpoolBatch(QIODevice &spool,
                          QVector<PointPreviewVertex> &vertices,
                          QString &error) {
  const char *data = reinterpret_cast<const char *>(vertices.constData());
  qint64 remaining =
      vertices.size() * static_cast<qint64>(sizeof(PointPreviewVertex));
  while (remaining > 0) {
    const qint64 written = spool.write(data, remaining);
    if (written <= 0) {
      error = QStringLiteral("Unable to write the temporary ASCII point spool: %1")
                  .arg(spool.errorString());
      return false;
    }
    data += written;
    remaining -= written;
  }
  vertices.clear();
  return true;
}

bool loadOutOfCoreMesh(
    QFile &file, const PlyHeader &header,
    const ElementDefinition &vertexElement,
    const ElementDefinition &faceElement, const int faceIndicesProperty,
    const int xIndex, const int yIndex, const int zIndex, const int redIndex,
    const int greenIndex, const int blueIndex,
    const bool sphericalHarmonicColor, PointCloudData &result) {
  const MeshCacheIndex existing = MeshCache::loadForSource(file.fileName());
  if (existing.isValid()) {
    result.meshCache = existing;
    result.sourceVertexCount = existing.fullVertexCount;
    result.sourceFaceCount = existing.fullFaceCount;
    result.sourceTriangleCount = existing.fullTriangleCount;
    result.boundsMinimum = existing.boundsMinimum;
    result.boundsMaximum = existing.boundsMaximum;
    result.previewOnly = true;
    return true;
  }

  const auto vertexIterator = std::find_if(
      header.elements.cbegin(), header.elements.cend(),
      [&vertexElement](const ElementDefinition &element) {
        return &element == &vertexElement;
      });
  const auto faceIterator = std::find_if(
      header.elements.cbegin(), header.elements.cend(),
      [&faceElement](const ElementDefinition &element) {
        return &element == &faceElement;
      });
  if (vertexIterator == header.elements.cend() ||
      faceIterator == header.elements.cend() || vertexIterator > faceIterator) {
    result.error = QStringLiteral(
        "Out-of-core mesh loading requires the PLY vertex element before "
        "the face element.");
    return false;
  }

  MeshCacheBuilder builder(file.fileName(), vertexElement.count);
  if (!builder.begin(&result.error)) {
    return false;
  }
  const qint64 dataOffset = file.pos();
  std::optional<BufferedAsciiScalarReader> asciiReader;
  std::optional<BufferedBinaryScalarReader> binaryReader;
  if (header.format == PlyFormat::Ascii) {
    asciiReader.emplace(file, dataOffset);
  } else {
    binaryReader.emplace(file, dataOffset, header.format);
  }
  const auto readScalar = [&](const ScalarType type, double &value) {
    return asciiReader.has_value() ? asciiReader->next(value)
                                   : binaryReader->next(type, value);
  };

  QVector<double> vertexValues(vertexElement.properties.size());
  QVector<quint32> faceIndices;
  qint64 sourceTriangleIndex = 0;
  bool verticesFinished = false;
  for (const ElementDefinition &element : header.elements) {
    for (qint64 recordIndex = 0; recordIndex < element.count; ++recordIndex) {
      if (&element == &vertexElement) {
        std::fill(vertexValues.begin(), vertexValues.end(), 0.0);
      }
      if (&element == &faceElement) {
        faceIndices.clear();
      }
      for (qsizetype propertyIndex = 0;
           propertyIndex < element.properties.size(); ++propertyIndex) {
        const PropertyDefinition &property =
            element.properties.at(propertyIndex);
        if (!property.isList) {
          double value = 0.0;
          if (!readScalar(property.valueType, value)) {
            result.error =
                QStringLiteral("PLY %1 record %2 has a missing scalar value.")
                    .arg(element.name)
                    .arg(recordIndex);
            return false;
          }
          if (&element == &vertexElement) {
            vertexValues[propertyIndex] = value;
          }
          continue;
        }

        double rawCount = 0.0;
        if (!readScalar(property.listCountType, rawCount) ||
            !std::isfinite(rawCount) || rawCount < 0.0 ||
            std::floor(rawCount) != rawCount ||
            rawCount > static_cast<double>(
                           kMaximumRecordBytes /
                           std::max<qsizetype>(
                               1, scalarByteSize(property.valueType)))) {
          result.error =
              QStringLiteral("PLY %1 record %2 has an invalid list count.")
                  .arg(element.name)
                  .arg(recordIndex);
          return false;
        }
        const qint64 listCount = static_cast<qint64>(rawCount);
        const bool collectFaceIndices =
            &element == &faceElement &&
            propertyIndex == faceIndicesProperty;
        if (collectFaceIndices) {
          faceIndices.reserve(static_cast<qsizetype>(listCount));
        }
        for (qint64 listIndex = 0; listIndex < listCount; ++listIndex) {
          double value = 0.0;
          if (!readScalar(property.valueType, value)) {
            result.error =
                QStringLiteral("PLY %1 record %2 has truncated list data.")
                    .arg(element.name)
                    .arg(recordIndex);
            return false;
          }
          if (!collectFaceIndices) {
            continue;
          }
          if (!std::isfinite(value) || value < 0.0 ||
              std::floor(value) != value ||
              value >= static_cast<double>(vertexElement.count) ||
              value > static_cast<double>(
                          std::numeric_limits<quint32>::max())) {
            result.error = QStringLiteral(
                               "PLY mesh face contains an invalid vertex "
                               "index: %1.")
                               .arg(value, 0, 'g', 16);
            return false;
          }
          faceIndices.append(static_cast<quint32>(value));
        }
      }

      if (&element == &vertexElement) {
        MeshVertex vertex;
        vertex.x = static_cast<float>(vertexValues.at(xIndex));
        vertex.y = static_cast<float>(vertexValues.at(yIndex));
        vertex.z = static_cast<float>(vertexValues.at(zIndex));
        if (redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0 &&
            std::isfinite(vertexValues.at(redIndex)) &&
            std::isfinite(vertexValues.at(greenIndex)) &&
            std::isfinite(vertexValues.at(blueIndex))) {
          if (sphericalHarmonicColor) {
            vertex.red = static_cast<float>(std::clamp(
                0.5 + kSphericalHarmonicDc * vertexValues.at(redIndex), 0.0,
                1.0));
            vertex.green = static_cast<float>(std::clamp(
                0.5 + kSphericalHarmonicDc * vertexValues.at(greenIndex),
                0.0, 1.0));
            vertex.blue = static_cast<float>(std::clamp(
                0.5 + kSphericalHarmonicDc * vertexValues.at(blueIndex), 0.0,
                1.0));
          } else {
            vertex.red = normalizedColor(
                vertexValues.at(redIndex),
                vertexElement.properties.at(redIndex).valueType);
            vertex.green = normalizedColor(
                vertexValues.at(greenIndex),
                vertexElement.properties.at(greenIndex).valueType);
            vertex.blue = normalizedColor(
                vertexValues.at(blueIndex),
                vertexElement.properties.at(blueIndex).valueType);
          }
        }
        if (!builder.appendVertex(vertex, &result.error)) {
          return false;
        }
      } else if (&element == &faceElement && faceIndices.size() >= 3) {
        for (qsizetype index = 1; index + 1 < faceIndices.size(); ++index) {
          if (!builder.appendTriangle(faceIndices.first(),
                                      faceIndices.at(index),
                                      faceIndices.at(index + 1),
                                      sourceTriangleIndex, &result.error)) {
            return false;
          }
          ++sourceTriangleIndex;
        }
      }
    }
    if (&element == &vertexElement) {
      if (!builder.finishVertices(&result.error)) {
        return false;
      }
      verticesFinished = true;
    }
  }
  if (!verticesFinished) {
    result.error = QStringLiteral("The PLY mesh vertex table was not read.");
    return false;
  }
  result.meshCache = builder.finish(faceElement.count, sourceTriangleIndex,
                                    &result.error);
  if (!result.meshCache.isValid()) {
    if (result.error.isEmpty()) {
      result.error = QStringLiteral("Unable to build the mesh-cache index.");
    }
    return false;
  }
  result.sourceVertexCount = result.meshCache.fullVertexCount;
  result.sourceFaceCount = result.meshCache.fullFaceCount;
  result.sourceTriangleCount = result.meshCache.fullTriangleCount;
  result.boundsMinimum = result.meshCache.boundsMinimum;
  result.boundsMaximum = result.meshCache.boundsMaximum;
  result.previewOnly = true;
  return true;
}

bool loadAsciiPointCache(
    QFile &file, const ElementDefinition &vertexElement, const int xIndex,
    const int yIndex, const int zIndex, const int redIndex,
    const int greenIndex, const int blueIndex,
    const bool sphericalHarmonicColor, PointCloudData &result) {
  const qint64 vertexDataOffset = file.pos();
  const QFileInfo sourceBefore(file.fileName());
  QDir cacheDirectory(
      PointCloudCache::cacheDirectoryForSource(file.fileName()));
  if (!cacheDirectory.mkpath(QStringLiteral("."))) {
    result.error = QStringLiteral("Unable to create the point-cache directory %1.")
                       .arg(cacheDirectory.absolutePath());
    return false;
  }
  cacheDirectory.setNameFilters(
      {QStringLiteral("gsw-ascii-spool-*.bin")});
  const qint64 staleSpoolCutoff =
      QDateTime::currentMSecsSinceEpoch() - kStaleAsciiSpoolMilliseconds;
  for (const QFileInfo &entry : cacheDirectory.entryInfoList(QDir::Files)) {
    if (entry.lastModified().toMSecsSinceEpoch() < staleSpoolCutoff) {
      QFile::remove(entry.absoluteFilePath());
    }
  }

  QTemporaryFile spool(cacheDirectory.filePath(
      QStringLiteral("gsw-ascii-spool-XXXXXX.bin")));
  spool.setAutoRemove(true);
  if (!spool.open()) {
    result.error = QStringLiteral("Unable to create the temporary ASCII point spool: %1")
                       .arg(spool.errorString());
    return false;
  }

  QVector<PointPreviewVertex> spoolBuffer;
  spoolBuffer.reserve(kAsciiSpoolBufferPoints);
  bool hasFiniteBounds = false;
  qint64 finitePointCount = 0;
  const auto appendPoint = [&](const double x, const double y, const double z,
                               const double red, const double green,
                               const double blue) -> bool {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      return true;
    }
    PointPreviewVertex vertex;
    vertex.x = static_cast<float>(x);
    vertex.y = static_cast<float>(y);
    vertex.z = static_cast<float>(z);
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
        !std::isfinite(vertex.z)) {
      return true;
    }
    if (redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0 &&
        std::isfinite(red) && std::isfinite(green) &&
        std::isfinite(blue)) {
      if (sphericalHarmonicColor) {
        vertex.red = static_cast<quint8>(std::clamp(
            qRound((0.5 + kSphericalHarmonicDc * red) * 255.0), 0, 255));
        vertex.green = static_cast<quint8>(std::clamp(
            qRound((0.5 + kSphericalHarmonicDc * green) * 255.0), 0, 255));
        vertex.blue = static_cast<quint8>(std::clamp(
            qRound((0.5 + kSphericalHarmonicDc * blue) * 255.0), 0, 255));
      } else {
        vertex.red = packedColor(
            red, vertexElement.properties.at(redIndex).valueType);
        vertex.green = packedColor(
            green, vertexElement.properties.at(greenIndex).valueType);
        vertex.blue = packedColor(
            blue, vertexElement.properties.at(blueIndex).valueType);
      }
    }
    const QVector3D position(vertex.x, vertex.y, vertex.z);
    if (!hasFiniteBounds) {
      result.boundsMinimum = position;
      result.boundsMaximum = position;
      hasFiniteBounds = true;
    } else {
      result.boundsMinimum.setX(
          std::min(result.boundsMinimum.x(), position.x()));
      result.boundsMinimum.setY(
          std::min(result.boundsMinimum.y(), position.y()));
      result.boundsMinimum.setZ(
          std::min(result.boundsMinimum.z(), position.z()));
      result.boundsMaximum.setX(
          std::max(result.boundsMaximum.x(), position.x()));
      result.boundsMaximum.setY(
          std::max(result.boundsMaximum.y(), position.y()));
      result.boundsMaximum.setZ(
          std::max(result.boundsMaximum.z(), position.z()));
    }
    spoolBuffer.append(vertex);
    ++finitePointCount;
    return spoolBuffer.size() < kAsciiSpoolBufferPoints ||
           writePointSpoolBatch(spool, spoolBuffer, result.error);
  };

  BufferedAsciiScalarReader reader(file, vertexDataOffset);
  for (qint64 recordIndex = 0; recordIndex < vertexElement.count;
       ++recordIndex) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (qsizetype propertyIndex = 0;
         propertyIndex < vertexElement.properties.size(); ++propertyIndex) {
      double value = 0.0;
      if (!reader.next(value)) {
        result.error =
            QStringLiteral(
                "ASCII PLY vertex %1 has an invalid or missing value.")
                .arg(recordIndex);
        return false;
      }
      const int property = static_cast<int>(propertyIndex);
      if (property == xIndex) {
        x = value;
      } else if (property == yIndex) {
        y = value;
      } else if (property == zIndex) {
        z = value;
      } else if (property == redIndex) {
        red = value;
      } else if (property == greenIndex) {
        green = value;
      } else if (property == blueIndex) {
        blue = value;
      }
    }
    if (!appendPoint(x, y, z, red, green, blue)) {
      return false;
    }
  }

  if (!hasFiniteBounds || finitePointCount <= 0) {
    result.error = QStringLiteral("The PLY file contains no finite vertices.");
    return false;
  }
  if ((!spoolBuffer.isEmpty() &&
       !writePointSpoolBatch(spool, spoolBuffer, result.error)) ||
      !spool.flush() || !spool.seek(0)) {
    if (result.error.isEmpty()) {
      result.error =
          QStringLiteral("Unable to finalize the temporary ASCII point spool.");
    }
    return false;
  }
  const QFileInfo sourceAfter(file.fileName());
  if (sourceAfter.size() != sourceBefore.size() ||
      sourceAfter.lastModified().toMSecsSinceEpoch() !=
          sourceBefore.lastModified().toMSecsSinceEpoch()) {
    result.error = QStringLiteral(
        "The source point cloud changed while it was being read.");
    return false;
  }

  PointCloudCacheBuilder cacheBuilder(file.fileName(), result.boundsMinimum,
                                      result.boundsMaximum);
  if (!cacheBuilder.begin(&result.error)) {
    return false;
  }
  QVector<PointPreviewVertex> readBuffer(kAsciiSpoolBufferPoints);
  qint64 sourceIndex = 0;
  while (sourceIndex < finitePointCount) {
    const qint64 requestedPoints = std::min<qint64>(
        readBuffer.size(), finitePointCount - sourceIndex);
    const qint64 requestedBytes =
        requestedPoints * static_cast<qint64>(sizeof(PointPreviewVertex));
    const qint64 readBytes = spool.read(
        reinterpret_cast<char *>(readBuffer.data()), requestedBytes);
    if (readBytes != requestedBytes) {
      result.error = QStringLiteral("The temporary ASCII point spool is truncated.");
      return false;
    }
    for (qint64 index = 0; index < requestedPoints; ++index) {
      if (!cacheBuilder.append(readBuffer.at(static_cast<qsizetype>(index)),
                               sourceIndex + index, &result.error)) {
        return false;
      }
    }
    sourceIndex += requestedPoints;
  }
  result.pointCache = cacheBuilder.finish(&result.error);
  if (!result.pointCache.isValid()) {
    if (result.error.isEmpty()) {
      result.error = QStringLiteral("Unable to build the point-cache index.");
    }
    return false;
  }
  result.boundsMinimum = result.pointCache.boundsMinimum;
  result.boundsMaximum = result.pointCache.boundsMaximum;
  result.previewOnly = true;
  return true;
}

bool loadFullResolutionPointPreview(
    QFile &file, const PlyHeader &header, const ElementDefinition &vertexElement,
    const int xIndex, const int yIndex, const int zIndex, const int redIndex,
    const int greenIndex, const int blueIndex,
    const bool sphericalHarmonicColor, PointCloudData &result) {
  const PointCloudCacheIndex existingCache =
      PointCloudCache::loadForSource(file.fileName());
  if (existingCache.isValid()) {
    result.pointCache = existingCache;
    result.boundsMinimum = existingCache.boundsMinimum;
    result.boundsMaximum = existingCache.boundsMaximum;
    result.previewOnly = true;
    return true;
  }

  const auto firstNonEmpty = std::find_if(
      header.elements.cbegin(), header.elements.cend(),
      [](const ElementDefinition &element) { return element.count > 0; });
  if (firstNonEmpty == header.elements.cend() || &(*firstNonEmpty) != &vertexElement) {
    result.error = QStringLiteral(
        "Full-resolution large-cloud preview requires the vertex element to "
        "be the first non-empty PLY element.");
    return false;
  }
  if (std::any_of(vertexElement.properties.cbegin(),
                  vertexElement.properties.cend(),
                  [](const PropertyDefinition &property) {
                    return property.isList;
                  })) {
    result.error = QStringLiteral(
        "Full-resolution large-cloud preview requires scalar PLY vertex "
        "properties.");
    return false;
  }

  if (header.format == PlyFormat::Ascii) {
    return loadAsciiPointCache(file, vertexElement, xIndex, yIndex, zIndex,
                               redIndex, greenIndex, blueIndex,
                               sphericalHarmonicColor, result);
  }

  QVector<qsizetype> offsets;
  offsets.reserve(vertexElement.properties.size());
  qsizetype recordBytes = 0;
  for (const PropertyDefinition &property : vertexElement.properties) {
    offsets.append(recordBytes);
    recordBytes += scalarByteSize(property.valueType);
  }
  if (recordBytes <= 0 || recordBytes > kMaximumRecordBytes) {
    result.error = QStringLiteral("The binary PLY vertex record is too large.");
    return false;
  }

  const auto valueAt = [&](const uchar *record, const int propertyIndex) {
    return scalarFromMemory(record + offsets.at(propertyIndex),
                            vertexElement.properties.at(propertyIndex).valueType,
                            header.format);
  };
  const qint64 vertexDataOffset = file.pos();
  bool hasFiniteBounds = false;
  qint64 sourceFirstVertex = 0;
  while (sourceFirstVertex < vertexElement.count) {
    const qsizetype requestedRecords = static_cast<qsizetype>(
        std::min<qint64>(kFullResolutionPointChunkSize,
                         vertexElement.count - sourceFirstVertex));
    const qint64 requestedBytes =
        static_cast<qint64>(requestedRecords) * recordBytes;
    const QByteArray bytes = file.read(requestedBytes);
    if (bytes.size() != requestedBytes) {
      result.error = QStringLiteral(
          "Unexpected end of binary PLY data while reading the large cloud.");
      return false;
    }
    const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype index = 0; index < requestedRecords; ++index) {
      const uchar *record = raw + index * recordBytes;
      const std::optional<double> x = valueAt(record, xIndex);
      const std::optional<double> y = valueAt(record, yIndex);
      const std::optional<double> z = valueAt(record, zIndex);
      if (!x.has_value() || !y.has_value() || !z.has_value() ||
          !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
        continue;
      }
      const QVector3D position(static_cast<float>(*x), static_cast<float>(*y),
                               static_cast<float>(*z));
      if (!hasFiniteBounds) {
        result.boundsMinimum = position;
        result.boundsMaximum = position;
        hasFiniteBounds = true;
      } else {
        result.boundsMinimum.setX(
            std::min(result.boundsMinimum.x(), position.x()));
        result.boundsMinimum.setY(
            std::min(result.boundsMinimum.y(), position.y()));
        result.boundsMinimum.setZ(
            std::min(result.boundsMinimum.z(), position.z()));
        result.boundsMaximum.setX(
            std::max(result.boundsMaximum.x(), position.x()));
        result.boundsMaximum.setY(
            std::max(result.boundsMaximum.y(), position.y()));
        result.boundsMaximum.setZ(
            std::max(result.boundsMaximum.z(), position.z()));
      }
    }
    sourceFirstVertex += requestedRecords;
  }
  if (!hasFiniteBounds) {
    result.error = QStringLiteral("The PLY file contains no finite vertices.");
    return false;
  }

  PointCloudCacheBuilder cacheBuilder(file.fileName(), result.boundsMinimum,
                                      result.boundsMaximum);
  if (!cacheBuilder.begin(&result.error) || !file.seek(vertexDataOffset)) {
    if (result.error.isEmpty()) {
      result.error = QStringLiteral("Unable to rewind the PLY for cache construction.");
    }
    return false;
  }
  sourceFirstVertex = 0;
  while (sourceFirstVertex < vertexElement.count) {
    const qsizetype requestedRecords = static_cast<qsizetype>(
        std::min<qint64>(kFullResolutionPointChunkSize,
                         vertexElement.count - sourceFirstVertex));
    const qint64 requestedBytes =
        static_cast<qint64>(requestedRecords) * recordBytes;
    const QByteArray bytes = file.read(requestedBytes);
    if (bytes.size() != requestedBytes) {
      result.error = QStringLiteral(
          "Unexpected end of binary PLY data while building its point cache.");
      return false;
    }
    const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype index = 0; index < requestedRecords; ++index) {
      const uchar *record = raw + index * recordBytes;
      const std::optional<double> x = valueAt(record, xIndex);
      const std::optional<double> y = valueAt(record, yIndex);
      const std::optional<double> z = valueAt(record, zIndex);
      if (!x.has_value() || !y.has_value() || !z.has_value() ||
          !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
        continue;
      }
      PointPreviewVertex vertex;
      vertex.x = static_cast<float>(*x);
      vertex.y = static_cast<float>(*y);
      vertex.z = static_cast<float>(*z);
      if (redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0) {
        const double red = valueAt(record, redIndex).value_or(0.72);
        const double green = valueAt(record, greenIndex).value_or(0.75);
        const double blue = valueAt(record, blueIndex).value_or(0.78);
        if (sphericalHarmonicColor) {
          vertex.red = static_cast<quint8>(std::clamp(
              qRound((0.5 + kSphericalHarmonicDc * red) * 255.0), 0, 255));
          vertex.green = static_cast<quint8>(std::clamp(
              qRound((0.5 + kSphericalHarmonicDc * green) * 255.0), 0, 255));
          vertex.blue = static_cast<quint8>(std::clamp(
              qRound((0.5 + kSphericalHarmonicDc * blue) * 255.0), 0, 255));
        } else {
          vertex.red = packedColor(
              red, vertexElement.properties.at(redIndex).valueType);
          vertex.green = packedColor(
              green, vertexElement.properties.at(greenIndex).valueType);
          vertex.blue = packedColor(
              blue, vertexElement.properties.at(blueIndex).valueType);
        }
      }
      if (!cacheBuilder.append(vertex, sourceFirstVertex + index,
                               &result.error)) {
        return false;
      }
    }
    sourceFirstVertex += requestedRecords;
  }
  result.pointCache = cacheBuilder.finish(&result.error);
  if (!result.pointCache.isValid()) {
    if (result.error.isEmpty()) {
      result.error = QStringLiteral("Unable to build the point-cache index.");
    }
    return false;
  }
  result.boundsMinimum = result.pointCache.boundsMinimum;
  result.boundsMaximum = result.pointCache.boundsMaximum;
  result.previewOnly = true;
  return true;
}

bool appendFaceTriangles(const QVector<double> &faceIndices,
                         const qint64 sourceVertexCount,
                         const bool appendPreview, PointCloudData &result) {
  if (faceIndices.size() < 3) {
    return true;
  }

  QVector<quint32> indices;
  indices.reserve(faceIndices.size());
  for (const double value : faceIndices) {
    if (!std::isfinite(value) || value < 0.0 || std::floor(value) != value ||
        value >= static_cast<double>(sourceVertexCount)) {
      result.error = QStringLiteral(
          "PLY mesh face contains an invalid vertex index: %1.")
                         .arg(value, 0, 'g', 16);
      return false;
    }
    indices.append(static_cast<quint32>(value));
  }

  const qint64 triangleCount = indices.size() - 2;
  result.sourceTriangleCount += triangleCount;
  if (!appendPreview) {
    result.meshPreviewDecimated = true;
    return true;
  }

  for (qsizetype index = 1; index + 1 < indices.size(); ++index) {
    if (result.meshIndices.size() / 3 >= kMaximumMeshPreviewTriangles) {
      result.meshPreviewDecimated = true;
      break;
    }
    result.meshIndices.append(indices.first());
    result.meshIndices.append(indices.at(index));
    result.meshIndices.append(indices.at(index + 1));
  }
  return true;
}

bool finitePosition(const PointPosition &position) {
  return std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z);
}

bool finalizeMeshGeometry(PointCloudData &result) {
  result.meshVertices.resize(result.sourcePositions.size());
  for (qsizetype index = 0; index < result.sourcePositions.size(); ++index) {
    const PointPosition &position = result.sourcePositions.at(index);
    MeshVertex &meshVertex = result.meshVertices[index];
    if (finitePosition(position)) {
      meshVertex.x = position.x;
      meshVertex.y = position.y;
      meshVertex.z = position.z;
    }
  }
  for (const PointCloudVertex &pointVertex : result.vertices) {
    const qsizetype index = static_cast<qsizetype>(pointVertex.sourceIndex);
    if (index < 0 || index >= result.meshVertices.size()) {
      continue;
    }
    MeshVertex &meshVertex = result.meshVertices[index];
    meshVertex.red = pointVertex.red;
    meshVertex.green = pointVertex.green;
    meshVertex.blue = pointVertex.blue;
  }

  QVector<QVector3D> normalSums(result.meshVertices.size());
  QVector<quint32> renderableIndices;
  renderableIndices.reserve(result.meshIndices.size());
  for (qsizetype index = 0; index + 2 < result.meshIndices.size(); index += 3) {
    const quint32 a = result.meshIndices.at(index);
    const quint32 b = result.meshIndices.at(index + 1);
    const quint32 c = result.meshIndices.at(index + 2);
    if (a == b || b == c || c == a ||
        !finitePosition(result.sourcePositions.at(a)) ||
        !finitePosition(result.sourcePositions.at(b)) ||
        !finitePosition(result.sourcePositions.at(c))) {
      continue;
    }
    const QVector3D pa = result.sourcePositions.at(a).toVector3D();
    const QVector3D pb = result.sourcePositions.at(b).toVector3D();
    const QVector3D pc = result.sourcePositions.at(c).toVector3D();
    const QVector3D faceNormal = QVector3D::crossProduct(pb - pa, pc - pa);
    if (!std::isfinite(faceNormal.lengthSquared()) ||
        faceNormal.lengthSquared() <= 1.0e-20F) {
      continue;
    }
    renderableIndices.append(a);
    renderableIndices.append(b);
    renderableIndices.append(c);
    normalSums[static_cast<qsizetype>(a)] += faceNormal;
    normalSums[static_cast<qsizetype>(b)] += faceNormal;
    normalSums[static_cast<qsizetype>(c)] += faceNormal;
  }
  result.meshIndices = std::move(renderableIndices);
  for (qsizetype index = 0; index < result.meshVertices.size(); ++index) {
    const QVector3D normal = normalSums.at(index).normalized();
    if (normal.lengthSquared() <= 0.0F) {
      continue;
    }
    result.meshVertices[index].normalX = normal.x();
    result.meshVertices[index].normalY = normal.y();
    result.meshVertices[index].normalZ = normal.z();
  }
  return !result.meshIndices.isEmpty();
}
} // namespace

bool PointCloudData::isValid() const {
  if (!error.isEmpty() || sourceVertexCount <= 0) {
    return false;
  }
  if (previewOnly) {
    return pointCache.isValid() || meshCache.isValid();
  }
  return sourceVertexCount == sourcePositions.size() && !vertices.isEmpty();
}

bool PointCloudData::hasMesh() const {
  return meshCache.isValid() ||
         (meshVertices.size() == sourceVertexCount &&
          meshIndices.size() >= 3 && meshIndices.size() % 3 == 0);
}

qsizetype PointCloudData::previewPointCount() const {
  if (!previewOnly) {
    return vertices.size();
  }
  const qint64 count = pointCache.isValid()
                           ? pointCache.fullPointCount
                           : meshCache.isValid() ? meshCache.fullVertexCount : 0;
  return static_cast<qsizetype>(
      std::min<qint64>(count, std::numeric_limits<qsizetype>::max()));
}

QVector3D PointCloudData::center() const {
  return (boundsMinimum + boundsMaximum) * 0.5F;
}

float PointCloudData::radius() const {
  return std::max((boundsMaximum - boundsMinimum).length() * 0.5F, 0.001F);
}

PointCloudData PlyPointCloudLoader::load(const QString &filePath,
                                         const qsizetype maximumPreviewPoints,
                                         const qint64 maximumEditablePoints,
                                         const qint64 maximumResidentMeshVertices,
                                         const qint64 maximumResidentMeshFaces) {
  PointCloudData result;
  qint64 residentMeshVertexLimit = maximumResidentMeshVertices;
  qint64 residentMeshFaceLimit = maximumResidentMeshFaces;
  bool vertexLimitOverrideValid = false;
  const int vertexLimitOverride = qEnvironmentVariableIntValue(
      "GSW_MESH_RESIDENT_VERTEX_LIMIT", &vertexLimitOverrideValid);
  if (maximumResidentMeshVertices == DefaultMaximumResidentMeshVertices &&
      vertexLimitOverrideValid && vertexLimitOverride > 0) {
    residentMeshVertexLimit = vertexLimitOverride;
  }
  bool faceLimitOverrideValid = false;
  const int faceLimitOverride = qEnvironmentVariableIntValue(
      "GSW_MESH_RESIDENT_FACE_LIMIT", &faceLimitOverrideValid);
  if (maximumResidentMeshFaces == DefaultMaximumResidentMeshFaces &&
      faceLimitOverrideValid && faceLimitOverride > 0) {
    residentMeshFaceLimit = faceLimitOverride;
  }
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
  if (maximumEditablePoints <= 0) {
    result.error = QStringLiteral("The editable point limit must be greater than zero.");
    return result;
  }
  if (maximumResidentMeshVertices <= 0 || maximumResidentMeshFaces <= 0) {
    result.error = QStringLiteral(
        "The resident mesh vertex and face limits must be greater than zero.");
    return result;
  }

  const auto faceElementIterator = std::find_if(
      header.elements.cbegin(), header.elements.cend(),
      [](const ElementDefinition &element) {
        return element.name.compare(QStringLiteral("face"),
                                    Qt::CaseInsensitive) == 0 &&
               element.count > 0;
      });
  const bool containsMeshFaces = faceElementIterator != header.elements.cend();
  int faceVertexIndicesProperty = -1;
  if (containsMeshFaces) {
    result.sourceFaceCount = faceElementIterator->count;
    faceVertexIndicesProperty = findProperty(
        *faceElementIterator,
        {QStringLiteral("vertex_indices"), QStringLiteral("vertex_index")});
    if (faceVertexIndicesProperty < 0 ||
        !faceElementIterator->properties.at(faceVertexIndicesProperty).isList ||
        !isIntegralType(faceElementIterator->properties
                            .at(faceVertexIndicesProperty)
                            .valueType)) {
      result.error = QStringLiteral(
          "The PLY face element must contain an integral list property named "
          "vertex_indices or vertex_index.");
      return result;
    }
  }

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

  const int opacityIndex = findProperty(vertexElement, {QStringLiteral("opacity")});
  const std::array<int, 3> scaleIndices = {
      findProperty(vertexElement, {QStringLiteral("scale_0")}),
      findProperty(vertexElement, {QStringLiteral("scale_1")}),
      findProperty(vertexElement, {QStringLiteral("scale_2")})};
  const std::array<int, 4> rotationIndices = {
      findProperty(vertexElement, {QStringLiteral("rot_0")}),
      findProperty(vertexElement, {QStringLiteral("rot_1")}),
      findProperty(vertexElement, {QStringLiteral("rot_2")}),
      findProperty(vertexElement, {QStringLiteral("rot_3")})};
  const auto isScalarProperty = [&vertexElement](const int index) {
    return index >= 0 && !vertexElement.properties.at(index).isList;
  };
  result.hasGaussianAttributes = isScalarProperty(opacityIndex) &&
      std::all_of(scaleIndices.cbegin(), scaleIndices.cend(),
                  isScalarProperty) &&
      std::all_of(rotationIndices.cbegin(), rotationIndices.cend(),
                  isScalarProperty);

  if (containsMeshFaces &&
      (vertexElement.count > residentMeshVertexLimit ||
       faceElementIterator->count > residentMeshFaceLimit)) {
    loadOutOfCoreMesh(file, header, vertexElement, *faceElementIterator,
                      faceVertexIndicesProperty, xIndex, yIndex, zIndex,
                      redIndex, greenIndex, blueIndex,
                      sphericalHarmonicColor, result);
    return result;
  }

  if (!containsMeshFaces && vertexElement.count > maximumEditablePoints) {
    loadFullResolutionPointPreview(
        file, header, vertexElement, xIndex, yIndex, zIndex, redIndex,
        greenIndex, blueIndex, sphericalHarmonicColor, result);
    return result;
  }

  if (vertexElement.count > std::numeric_limits<quint32>::max()) {
    result.error = QStringLiteral(
        "Editable PLY point clouds currently support up to %1 vertices; "
        "use a fixed-width binary PLY for full-resolution read-only preview.")
                       .arg(std::numeric_limits<quint32>::max());
    return result;
  }

  const qsizetype sampleCount = static_cast<qsizetype>(
      containsMeshFaces
          ? vertexElement.count
          : std::min<qint64>(vertexElement.count, maximumPreviewPoints));
  result.vertices.reserve(sampleCount);
  result.sourcePositions.reserve(static_cast<qsizetype>(vertexElement.count));
  if (containsMeshFaces) {
    result.meshIndices.reserve(static_cast<qsizetype>(
        std::min<qint64>(faceElementIterator->count,
                         kMaximumMeshPreviewTriangles) *
        3));
  }
  qsizetype nextSampleIndex = 0;
  qsizetype nextFaceSampleIndex = 0;
  const qsizetype faceSampleCount =
      containsMeshFaces
          ? static_cast<qsizetype>(std::min<qint64>(
                faceElementIterator->count, kMaximumMeshPreviewFaces))
          : 0;
  bool hasFiniteBounds = false;

  for (const ElementDefinition &element : header.elements) {
    QVector<double> values(element.properties.size());
    QVector<QVector<double>> listValues(element.properties.size());
    for (qint64 recordIndex = 0; recordIndex < element.count; ++recordIndex) {
      const bool read = header.format == PlyFormat::Ascii
                            ? readAsciiElementRecord(file, element, values,
                                                     listValues, result.error)
                            : readBinaryElementRecord(file, element,
                                                      header.format, values,
                                                      listValues, result.error);
      if (!read) {
        result.vertices.clear();
        result.sourcePositions.clear();
        result.meshIndices.clear();
        return result;
      }
      if (containsMeshFaces && &element == &(*faceElementIterator)) {
        const bool appendPreview = shouldSampleVertex(
            recordIndex, faceElementIterator->count, faceSampleCount,
            nextFaceSampleIndex);
        if (!appendFaceTriangles(listValues.at(faceVertexIndicesProperty),
                                 vertexElement.count, appendPreview, result)) {
          result.vertices.clear();
          result.sourcePositions.clear();
          result.meshIndices.clear();
          return result;
        }
        continue;
      }
      if (element.name.compare(QStringLiteral("vertex"),
                               Qt::CaseInsensitive) != 0) {
        continue;
      }
      const bool appendPreview = shouldSampleVertex(
          recordIndex, vertexElement.count, sampleCount, nextSampleIndex);
      appendVertex(element, values, xIndex, yIndex, zIndex, redIndex, greenIndex,
                   blueIndex, sphericalHarmonicColor, opacityIndex,
                   scaleIndices, rotationIndices, result.hasGaussianAttributes,
                   recordIndex, appendPreview, hasFiniteBounds, result);
    }
  }

  if (!hasFiniteBounds || result.vertices.isEmpty()) {
    result.error = QStringLiteral("The PLY file contains no finite vertices.");
    return result;
  }
  if (containsMeshFaces && !finalizeMeshGeometry(result)) {
    result.error = QStringLiteral(
        "The PLY declares mesh faces but contains no renderable triangles.");
  }
  return result;
}

bool PlyPointCloudLoader::writeFiltered(const QString &sourceFilePath,
                                        const QString &destinationFilePath,
                                        const QBitArray &deletedVertices,
                                        QString *errorMessage) {
  QString error;
  const QString sourceAbsolute = QDir::cleanPath(QFileInfo(sourceFilePath).absoluteFilePath());
  const QString destinationAbsolute =
      QDir::cleanPath(QFileInfo(destinationFilePath).absoluteFilePath());
  if (sourceAbsolute.compare(destinationAbsolute, Qt::CaseInsensitive) == 0) {
    error = QStringLiteral("Choose a new file name; cropped export cannot overwrite its source PLY.");
  }

  QFile source(sourceFilePath);
  if (error.isEmpty() && !source.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Unable to open source PLY: %1").arg(source.errorString());
  }

  PlyHeader header;
  if (error.isEmpty() && !parseHeader(source, header, error)) {
    // parseHeader provides the error.
  }

  const auto vertexElementIterator = std::find_if(
      header.elements.cbegin(), header.elements.cend(),
      [](const ElementDefinition &element) {
        return element.name == QStringLiteral("vertex");
      });
  if (error.isEmpty() && vertexElementIterator == header.elements.cend()) {
    error = QStringLiteral("The source PLY does not contain a vertex element.");
  }
  if (error.isEmpty() && vertexElementIterator->count != deletedVertices.size()) {
    error = QStringLiteral("The edit state no longer matches the source PLY vertex count.");
  }
  if (error.isEmpty()) {
    const bool hasIndexedFaces = std::any_of(
        header.elements.cbegin(), header.elements.cend(),
        [](const ElementDefinition &element) {
          if (element.count <= 0) {
            return false;
          }
          if (element.name.compare(QStringLiteral("face"), Qt::CaseInsensitive) == 0) {
            return true;
          }
          return std::any_of(
              element.properties.cbegin(), element.properties.cend(),
              [](const PropertyDefinition &property) {
                return property.name.contains(QStringLiteral("vertex_index"),
                                              Qt::CaseInsensitive) ||
                       property.name.contains(QStringLiteral("vertex_indices"),
                                              Qt::CaseInsensitive);
              });
        });
    if (hasIndexedFaces) {
      error = QStringLiteral(
          "This PLY contains indexed mesh faces. Native crop export currently supports point and Gaussian PLY files only.");
    }
  }

  qsizetype deletedCount = 0;
  if (error.isEmpty()) {
    for (qsizetype index = 0; index < deletedVertices.size(); ++index) {
      deletedCount += deletedVertices.testBit(index) ? 1 : 0;
    }
    if (deletedCount == 0) {
      error = QStringLiteral("No deleted vertices are available to export.");
    } else if (deletedCount >= deletedVertices.size()) {
      error = QStringLiteral("A cropped PLY must retain at least one vertex.");
    }
  }

  QSaveFile destination(destinationFilePath);
  if (error.isEmpty() && !destination.open(QIODevice::WriteOnly)) {
    error = QStringLiteral("Unable to create cropped PLY: %1").arg(destination.errorString());
  }

  if (error.isEmpty()) {
    const qint64 remaining = deletedVertices.size() - deletedCount;
    for (qsizetype lineIndex = 0; lineIndex < header.rawLines.size(); ++lineIndex) {
      QByteArray line = header.rawLines.at(lineIndex);
      if (lineIndex == header.vertexElementLine) {
        QByteArray newline;
        if (line.endsWith("\r\n")) {
          newline = "\r\n";
        } else if (line.endsWith('\n')) {
          newline = "\n";
        }
        line = "element vertex " + QByteArray::number(remaining) + newline;
      }
      if (!writeBytes(destination, line, error)) {
        break;
      }
    }
  }

  qsizetype sourceVertexIndex = 0;
  if (error.isEmpty()) {
    for (const ElementDefinition &element : header.elements) {
      for (qint64 recordIndex = 0; recordIndex < element.count; ++recordIndex) {
        QByteArray record;
        const bool read = header.format == PlyFormat::Ascii
                              ? readAsciiRawRecord(source, record, error)
                              : readBinaryRawRecord(source, element,
                                                    header.format, record,
                                                    error);
        if (!read) {
          break;
        }
        bool keep = true;
        if (element.name == QStringLiteral("vertex")) {
          keep = !deletedVertices.testBit(sourceVertexIndex);
          ++sourceVertexIndex;
        }
        if (keep && !writeBytes(destination, record, error)) {
          break;
        }
      }
      if (!error.isEmpty()) {
        break;
      }
    }
  }

  while (error.isEmpty() && !source.atEnd()) {
    const QByteArray trailing = source.read(1024 * 1024);
    if (trailing.isEmpty() && source.error() != QFileDevice::NoError) {
      error = QStringLiteral("Unable to read trailing PLY data: %1").arg(source.errorString());
      break;
    }
    if (!writeBytes(destination, trailing, error)) {
      break;
    }
  }

  bool succeeded = error.isEmpty();
  if (succeeded && !destination.commit()) {
    error = QStringLiteral("Unable to finalize cropped PLY: %1").arg(destination.errorString());
    succeeded = false;
  } else if (!succeeded) {
    destination.cancelWriting();
  }
  if (errorMessage != nullptr) {
    *errorMessage = error;
  }
  return succeeded;
}

} // namespace gsw
