#include "TrainingGpuPreviewProtocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QtEndian>

#include <cmath>
#include <limits>

namespace gsw {

namespace {

void setError(QString *target, const QString &message) {
  if (target != nullptr) {
    *target = message;
  }
}

bool parseExactInteger(const QJsonObject &object, const QString &name,
                       quint64 *target, const quint64 minimum = 0) {
  const QJsonValue value = object.value(name);
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < static_cast<double>(minimum) ||
      number > 9007199254740991.0 || std::floor(number) != number) {
    return false;
  }
  *target = static_cast<quint64>(number);
  return true;
}

bool parseSmallInteger(const QJsonObject &object, const QString &name,
                       int *target, const int minimum = 0) {
  quint64 value = 0;
  if (!parseExactInteger(object, name, &value,
                         static_cast<quint64>(minimum)) ||
      value > static_cast<quint64>(std::numeric_limits<int>::max())) {
    return false;
  }
  *target = static_cast<int>(value);
  return true;
}

bool safeSessionId(const QString &value) {
  static const QRegularExpression expression(
      QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]{0,79}$"));
  return expression.match(value).hasMatch();
}

bool safeKernelObjectName(const QString &value) {
  static const QRegularExpression expression(
      QStringLiteral("^Local\\\\GSW-GPU-[A-Za-z0-9-]{1,160}$"));
  return expression.match(value).hasMatch();
}

quint32 readLe32(QByteArrayView bytes, const qsizetype offset) {
  return qFromLittleEndian<quint32>(
      reinterpret_cast<const uchar *>(bytes.data() + offset));
}

quint64 readLe64(QByteArrayView bytes, const qsizetype offset) {
  return qFromLittleEndian<quint64>(
      reinterpret_cast<const uchar *>(bytes.data() + offset));
}

bool geometryIsSafe(const quint64 capacity, const quint64 slotBytes,
                    const quint64 allocationBytes) {
  if (capacity == 0 || capacity > 20000000ULL || slotBytes == 0 ||
      allocationBytes == 0) {
    return false;
  }
  if (capacity >
      std::numeric_limits<quint64>::max() /
          static_cast<quint64>(kTrainingGpuPreviewStrideBytes)) {
    return false;
  }
  const quint64 payloadBytes =
      capacity * static_cast<quint64>(kTrainingGpuPreviewStrideBytes);
  return payloadBytes <= slotBytes &&
         slotBytes <= std::numeric_limits<quint64>::max() /
                          kTrainingGpuPreviewSlotCount &&
         slotBytes * kTrainingGpuPreviewSlotCount <= allocationBytes;
}

} // namespace

bool parseTrainingGpuPreviewDescriptor(
    const QByteArrayView payload, TrainingGpuPreviewDescriptor *descriptor,
    QString *errorMessage) {
  if (descriptor == nullptr) {
    setError(errorMessage, QStringLiteral("Descriptor destination is null."));
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(payload.toByteArray(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    setError(errorMessage, QStringLiteral("GPU preview event is not valid JSON."));
    return false;
  }
  const QJsonObject object = document.object();
  const QJsonValue version = object.value(QStringLiteral("version"));
  const QJsonValue type = object.value(QStringLiteral("type"));
  const QJsonValue state = object.value(QStringLiteral("state"));
  const QJsonValue session = object.value(QStringLiteral("sessionId"));
  if (!version.isDouble() || version.toDouble() != 1.0 || !type.isString() ||
      type.toString() != QStringLiteral("gpu_preview") || !state.isString() ||
      !session.isString() || !safeSessionId(session.toString())) {
    setError(errorMessage,
             QStringLiteral("GPU preview event header is invalid."));
    return false;
  }

  TrainingGpuPreviewDescriptor parsed;
  parsed.sessionId = session.toString();
  const QString stateName = state.toString();
  if (stateName == QStringLiteral("closing")) {
    parsed.state = TrainingGpuPreviewState::Closing;
    *descriptor = std::move(parsed);
    return true;
  }
  if (stateName == QStringLiteral("failed")) {
    parsed.state = TrainingGpuPreviewState::Failed;
    const QJsonValue reason = object.value(QStringLiteral("error"));
    if (reason.isString()) {
      parsed.error = reason.toString().left(1000);
    }
    *descriptor = std::move(parsed);
    return true;
  }
  if (stateName != QStringLiteral("ready")) {
    setError(errorMessage, QStringLiteral("GPU preview state is unsupported."));
    return false;
  }
  parsed.state = TrainingGpuPreviewState::Ready;

  quint64 producerPid = 0;
  if (!parseExactInteger(object, QStringLiteral("producerPid"), &producerPid,
                         1) ||
      producerPid > std::numeric_limits<quint32>::max()) {
    setError(errorMessage, QStringLiteral("GPU preview producer PID is invalid."));
    return false;
  }
  parsed.producerPid = static_cast<quint32>(producerPid);

  const QJsonValue handle = object.value(QStringLiteral("memoryHandle"));
  bool handleOk = false;
  if (handle.isString()) {
    QString handleText = handle.toString().trimmed();
    if (handleText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
      handleText.remove(0, 2);
    }
    parsed.memoryHandle = handleText.toULongLong(&handleOk, 16);
  }
  if (!handleOk || parsed.memoryHandle == 0) {
    setError(errorMessage,
             QStringLiteral("GPU preview memory handle is invalid."));
    return false;
  }
  const QJsonValue handleType =
      object.value(QStringLiteral("memoryHandleType"));
  if (!handleType.isString()) {
    setError(errorMessage,
             QStringLiteral("GPU preview memory handle type is invalid."));
    return false;
  }
  if (handleType.toString() == QStringLiteral("opaque_win32")) {
    parsed.memoryHandleType = TrainingGpuPreviewHandleType::OpaqueWin32;
  } else if (handleType.toString() ==
             QStringLiteral("opaque_win32_kmt")) {
    parsed.memoryHandleType = TrainingGpuPreviewHandleType::OpaqueWin32Kmt;
  } else {
    setError(errorMessage,
             QStringLiteral("GPU preview memory handle type is unsupported."));
    return false;
  }

  if (!parseExactInteger(object, QStringLiteral("allocationBytes"),
                         &parsed.allocationBytes, 1) ||
      !parseExactInteger(object, QStringLiteral("slotBytes"),
                         &parsed.slotBytes, 1) ||
      !parseSmallInteger(object, QStringLiteral("slotCount"),
                         &parsed.slotCount, 1) ||
      !parseSmallInteger(object, QStringLiteral("strideBytes"),
                         &parsed.strideBytes, 1) ||
      !parseExactInteger(object, QStringLiteral("capacity"), &parsed.capacity,
                         1)) {
    setError(errorMessage,
             QStringLiteral("GPU preview buffer geometry is invalid."));
    return false;
  }
  if (parsed.slotCount != kTrainingGpuPreviewSlotCount ||
      parsed.strideBytes != kTrainingGpuPreviewStrideBytes) {
    setError(errorMessage,
             QStringLiteral("GPU preview slot count or stride is incompatible."));
    return false;
  }
  if (!geometryIsSafe(parsed.capacity, parsed.slotBytes,
                      parsed.allocationBytes)) {
    setError(errorMessage,
             QStringLiteral("GPU preview slot geometry is unsafe."));
    return false;
  }

  const auto parseName = [&object](const QString &field, QString *target) {
    const QJsonValue value = object.value(field);
    if (!value.isString() || !safeKernelObjectName(value.toString())) {
      return false;
    }
    *target = value.toString();
    return true;
  };
  if (!parseName(QStringLiteral("controlMapping"), &parsed.controlMapping) ||
      !parseName(QStringLiteral("frameEvent"), &parsed.frameEvent)) {
    setError(errorMessage,
             QStringLiteral("GPU preview control object name is invalid."));
    return false;
  }
  for (int index = 0; index < kTrainingGpuPreviewSlotCount; ++index) {
    QString eventName;
    if (!parseName(QStringLiteral("releaseEvent%1").arg(index), &eventName)) {
      setError(errorMessage,
               QStringLiteral("GPU preview release event name is invalid."));
      return false;
    }
    parsed.releaseEvents.append(eventName);
  }
  const QJsonValue device = object.value(QStringLiteral("device"));
  if (device.isString()) {
    parsed.device = device.toString().left(240);
  }
  const QJsonValue deviceLuid = object.value(QStringLiteral("deviceLuid"));
  if (!deviceLuid.isUndefined()) {
    static const QRegularExpression luidExpression(
        QStringLiteral("^[0-9A-Fa-f]{16}$"));
    quint64 nodeMask = 0;
    if (!deviceLuid.isString() ||
        !luidExpression.match(deviceLuid.toString()).hasMatch() ||
        !parseExactInteger(object, QStringLiteral("deviceNodeMask"),
                           &nodeMask) ||
        nodeMask > std::numeric_limits<quint32>::max()) {
      setError(errorMessage,
               QStringLiteral("GPU preview device identity is invalid."));
      return false;
    }
    parsed.deviceLuid = deviceLuid.toString().toLower();
    parsed.deviceNodeMask = static_cast<quint32>(nodeMask);
  }
  *descriptor = std::move(parsed);
  return true;
}

bool parseTrainingGpuPreviewControl(
    const QByteArrayView bytes, TrainingGpuPreviewControlSnapshot *snapshot,
    QString *errorMessage) {
  if (snapshot == nullptr ||
      bytes.size() < static_cast<qsizetype>(kTrainingGpuPreviewHeaderBytes)) {
    setError(errorMessage,
             QStringLiteral("GPU preview control block is too small."));
    return false;
  }
  if (QByteArrayView(bytes.data(), 8) != QByteArrayView("GSWGPU1\0", 8) ||
      readLe32(bytes, 8) != 1 ||
      readLe32(bytes, 12) != kTrainingGpuPreviewHeaderBytes) {
    setError(errorMessage,
             QStringLiteral("GPU preview control header is incompatible."));
    return false;
  }
  const quint32 sequence = readLe32(bytes, 16);
  const quint32 trailingSequence = readLe32(bytes, 120);
  if ((sequence & 1U) != 0U || sequence != trailingSequence) {
    setError(errorMessage,
             QStringLiteral("GPU preview control snapshot is not stable."));
    return false;
  }
  const quint32 stateValue = readLe32(bytes, 20);
  if (stateValue >
      static_cast<quint32>(TrainingGpuPreviewControlState::Failed) ||
      readLe32(bytes, 24) != kTrainingGpuPreviewSlotCount ||
      readLe32(bytes, 28) != kTrainingGpuPreviewStrideBytes) {
    setError(errorMessage,
             QStringLiteral("GPU preview control geometry is incompatible."));
    return false;
  }

  TrainingGpuPreviewControlSnapshot parsed;
  parsed.sequence = sequence;
  parsed.state = static_cast<TrainingGpuPreviewControlState>(stateValue);
  parsed.capacity = readLe64(bytes, 32);
  parsed.slotBytes = readLe64(bytes, 40);
  parsed.allocationBytes = readLe64(bytes, 48);
  if (!geometryIsSafe(parsed.capacity, parsed.slotBytes,
                      parsed.allocationBytes)) {
    setError(errorMessage,
             QStringLiteral("GPU preview control geometry is unsafe."));
    return false;
  }
  parsed.slotSnapshots.reserve(kTrainingGpuPreviewSlotCount);
  for (int index = 0; index < kTrainingGpuPreviewSlotCount; ++index) {
    const qsizetype offset = 56 + index * 32;
    TrainingGpuPreviewSlotSnapshot slot;
    slot.generation = readLe64(bytes, offset);
    slot.pointCount = readLe64(bytes, offset + 8);
    slot.iteration = readLe64(bytes, offset + 16);
    slot.timestampNanoseconds = readLe64(bytes, offset + 24);
    if (slot.pointCount > parsed.capacity) {
      setError(errorMessage,
               QStringLiteral("GPU preview point count exceeds capacity."));
      return false;
    }
    parsed.slotSnapshots.append(slot);
  }
  *snapshot = std::move(parsed);
  return true;
}

} // namespace gsw
