#include "TrainingGpuPreviewProtocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>

#include <cstring>

using namespace gsw;

namespace {

QByteArray validReadyPayload() {
  QJsonObject object;
  object.insert(QStringLiteral("version"), 2);
  object.insert(QStringLiteral("type"), QStringLiteral("gpu_preview"));
  object.insert(QStringLiteral("state"), QStringLiteral("ready"));
  object.insert(QStringLiteral("sessionId"),
                QStringLiteral("7b29e168-5e64-447d-b262-d3ebbe947c78"));
  object.insert(QStringLiteral("producerPid"), 4242);
  object.insert(QStringLiteral("memoryHandle"), QStringLiteral("0x41c"));
  object.insert(QStringLiteral("memoryHandleType"),
                QStringLiteral("opaque_win32_kmt"));
  object.insert(QStringLiteral("allocationBytes"), 268435456.0);
  object.insert(QStringLiteral("slotBytes"), 134217728.0);
  object.insert(QStringLiteral("slotCount"), 2);
  object.insert(QStringLiteral("strideBytes"), 56);
  object.insert(QStringLiteral("capacity"), 2396745.0);
  object.insert(QStringLiteral("controlMapping"),
                QStringLiteral("Local\\GSW-GPU-preview-control"));
  object.insert(QStringLiteral("frameEvent"),
                QStringLiteral("Local\\GSW-GPU-preview-frame"));
  object.insert(QStringLiteral("releaseEvent0"),
                QStringLiteral("Local\\GSW-GPU-preview-release-0"));
  object.insert(QStringLiteral("releaseEvent1"),
                QStringLiteral("Local\\GSW-GPU-preview-release-1"));
  object.insert(QStringLiteral("device"),
                QStringLiteral("NVIDIA GeForce RTX 4070 Laptop GPU"));
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void writeLe32(QByteArray *bytes, const qsizetype offset, const quint32 value) {
  qToLittleEndian(value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

void writeLe64(QByteArray *bytes, const qsizetype offset, const quint64 value) {
  qToLittleEndian(value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

void writeLeFloat(QByteArray *bytes, const qsizetype offset, const float value) {
  quint32 bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  writeLe32(bytes, offset, bits);
}

} // namespace

class TrainingGpuPreviewProtocolTests final : public QObject {
  Q_OBJECT

private slots:
  void parsesReadyDescriptor();
  void rejectsUnsafeDescriptorGeometry();
  void parsesStableControlSnapshot();
  void rejectsTornControlSnapshot();
};

void TrainingGpuPreviewProtocolTests::parsesReadyDescriptor() {
  TrainingGpuPreviewDescriptor descriptor;
  QString error;
  QVERIFY2(parseTrainingGpuPreviewDescriptor(validReadyPayload(), &descriptor,
                                               &error),
           qPrintable(error));
  QCOMPARE(descriptor.state, TrainingGpuPreviewState::Ready);
  QCOMPARE(descriptor.producerPid, quint32(4242));
  QCOMPARE(descriptor.memoryHandle, quint64(0x41c));
  QCOMPARE(descriptor.memoryHandleType,
           TrainingGpuPreviewHandleType::OpaqueWin32Kmt);
  QCOMPARE(descriptor.allocationBytes, quint64(268435456));
  QCOMPARE(descriptor.slotBytes, quint64(134217728));
  QCOMPARE(descriptor.slotCount, 2);
  QCOMPARE(descriptor.strideBytes, 56);
  QCOMPARE(descriptor.capacity, quint64(2396745));
  QCOMPARE(descriptor.releaseEvents.size(), 2);
  QCOMPARE(descriptor.device,
           QStringLiteral("NVIDIA GeForce RTX 4070 Laptop GPU"));
}

void TrainingGpuPreviewProtocolTests::rejectsUnsafeDescriptorGeometry() {
  QJsonObject object =
      QJsonDocument::fromJson(validReadyPayload()).object();
  object.insert(QStringLiteral("strideBytes"), 60);
  object.insert(QStringLiteral("slotBytes"), 16);
  TrainingGpuPreviewDescriptor descriptor;
  QString error;
  QVERIFY(!parseTrainingGpuPreviewDescriptor(
      QJsonDocument(object).toJson(QJsonDocument::Compact), &descriptor,
      &error));
  QVERIFY(error.contains(QStringLiteral("stride"), Qt::CaseInsensitive) ||
          error.contains(QStringLiteral("slot"), Qt::CaseInsensitive));
}

void TrainingGpuPreviewProtocolTests::parsesStableControlSnapshot() {
  QCOMPARE(kTrainingGpuPreviewHeaderBytes, quint32(156));
  QByteArray bytes(kTrainingGpuPreviewControlBytes, '\0');
  bytes.replace(0, 8, QByteArrayLiteral("GSWGPU2\0"));
  writeLe32(&bytes, 8, kTrainingGpuPreviewProtocolVersion);
  writeLe32(&bytes, 12, kTrainingGpuPreviewHeaderBytes);
  writeLe32(&bytes, 16, 2); // even sequence means stable
  writeLe32(&bytes, 20, 1); // ready
  writeLe32(&bytes, 24, 2);
  writeLe32(&bytes, 28, 56);
  writeLe64(&bytes, 32, 1000);
  writeLe64(&bytes, 40, 56000);
  writeLe64(&bytes, 48, 112000);
  writeLe64(&bytes, 56, 17);
  writeLe64(&bytes, 64, 900);
  writeLe64(&bytes, 72, 80);
  writeLe64(&bytes, 80, 123456789);
  writeLeFloat(&bytes, 88, -2.0F);
  writeLeFloat(&bytes, 92, 1.0F);
  writeLeFloat(&bytes, 96, 4.0F);
  writeLeFloat(&bytes, 100, 8.0F);
  writeLe64(&bytes, 104, 18);
  writeLe64(&bytes, 112, 950);
  writeLe64(&bytes, 120, 81);
  writeLe64(&bytes, 128, 123456999);
  writeLeFloat(&bytes, 136, -1.5F);
  writeLeFloat(&bytes, 140, 1.5F);
  writeLeFloat(&bytes, 144, 4.5F);
  writeLeFloat(&bytes, 148, 9.0F);
  writeLe32(&bytes, 152, 2); // matching trailing sequence

  TrainingGpuPreviewControlSnapshot snapshot;
  QString error;
  QVERIFY2(parseTrainingGpuPreviewControl(bytes, &snapshot, &error),
           qPrintable(error));
  QCOMPARE(snapshot.state, TrainingGpuPreviewControlState::Ready);
  QCOMPARE(snapshot.capacity, quint64(1000));
  QCOMPARE(snapshot.slotSnapshots.at(1).generation, quint64(18));
  QCOMPARE(snapshot.slotSnapshots.at(1).pointCount, quint64(950));
  QCOMPARE(snapshot.slotSnapshots.at(1).iteration, quint64(81));
  QCOMPARE(snapshot.slotSnapshots.at(1).sceneCenterX, -1.5F);
  QCOMPARE(snapshot.slotSnapshots.at(1).sceneCenterY, 1.5F);
  QCOMPARE(snapshot.slotSnapshots.at(1).sceneCenterZ, 4.5F);
  QCOMPARE(snapshot.slotSnapshots.at(1).sceneRadius, 9.0F);
}

void TrainingGpuPreviewProtocolTests::rejectsTornControlSnapshot() {
  QByteArray bytes(kTrainingGpuPreviewControlBytes, '\0');
  bytes.replace(0, 8, QByteArrayLiteral("GSWGPU2\0"));
  writeLe32(&bytes, 8, kTrainingGpuPreviewProtocolVersion);
  writeLe32(&bytes, 12, kTrainingGpuPreviewHeaderBytes);
  writeLe32(&bytes, 16, 3); // odd means writer is active
  writeLe32(&bytes, 152, 2);
  TrainingGpuPreviewControlSnapshot snapshot;
  QString error;
  QVERIFY(!parseTrainingGpuPreviewControl(bytes, &snapshot, &error));
  QVERIFY(error.contains(QStringLiteral("stable"), Qt::CaseInsensitive));
}

QTEST_GUILESS_MAIN(TrainingGpuPreviewProtocolTests)

#include "TrainingGpuPreviewProtocolTests.moc"
