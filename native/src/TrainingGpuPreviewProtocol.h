#pragma once

#include <QByteArrayView>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace gsw {

inline constexpr qsizetype kTrainingGpuPreviewControlBytes = 4096;
inline constexpr quint32 kTrainingGpuPreviewHeaderBytes = 124;
inline constexpr int kTrainingGpuPreviewSlotCount = 2;
inline constexpr int kTrainingGpuPreviewStrideBytes = 14 * sizeof(float);

enum class TrainingGpuPreviewState { Ready, Closing, Failed };
enum class TrainingGpuPreviewHandleType { OpaqueWin32, OpaqueWin32Kmt };

struct TrainingGpuPreviewDescriptor final {
  TrainingGpuPreviewState state = TrainingGpuPreviewState::Failed;
  QString sessionId;
  quint32 producerPid = 0;
  quint64 memoryHandle = 0;
  TrainingGpuPreviewHandleType memoryHandleType =
      TrainingGpuPreviewHandleType::OpaqueWin32;
  quint64 allocationBytes = 0;
  quint64 slotBytes = 0;
  int slotCount = 0;
  int strideBytes = 0;
  quint64 capacity = 0;
  QString controlMapping;
  QString frameEvent;
  QStringList releaseEvents;
  QString device;
  QString deviceLuid;
  quint32 deviceNodeMask = 0;
  QString error;
};

enum class TrainingGpuPreviewControlState : quint32 {
  Initializing = 0,
  Ready = 1,
  Closing = 2,
  Failed = 3,
};

struct TrainingGpuPreviewSlotSnapshot final {
  quint64 generation = 0;
  quint64 pointCount = 0;
  quint64 iteration = 0;
  quint64 timestampNanoseconds = 0;
};

struct TrainingGpuPreviewControlSnapshot final {
  TrainingGpuPreviewControlState state =
      TrainingGpuPreviewControlState::Initializing;
  quint64 capacity = 0;
  quint64 slotBytes = 0;
  quint64 allocationBytes = 0;
  QVector<TrainingGpuPreviewSlotSnapshot> slotSnapshots;
  quint32 sequence = 0;
};

[[nodiscard]] bool parseTrainingGpuPreviewDescriptor(
    QByteArrayView payload, TrainingGpuPreviewDescriptor *descriptor,
    QString *errorMessage = nullptr);

[[nodiscard]] bool parseTrainingGpuPreviewControl(
    QByteArrayView bytes, TrainingGpuPreviewControlSnapshot *snapshot,
    QString *errorMessage = nullptr);

} // namespace gsw

Q_DECLARE_METATYPE(gsw::TrainingGpuPreviewDescriptor)
