#pragma once

#include <QString>
#include <QtGlobal>

class QOpenGLContext;

namespace gsw {

enum class GpuProbeSource {
  Unavailable,
  DxgiProcessBudget,
  NvxApproximate
};

struct SystemResourceSnapshot final {
  qint64 physicalTotalBytes = 0;
  qint64 physicalAvailableBytes = 0;
  qint64 commitAvailableBytes = 0;
  bool systemMemoryKnown = false;

  qint64 gpuDedicatedBytes = 0;
  qint64 gpuBudgetBytes = 0;
  qint64 gpuUsageBytes = 0;
  qint64 gpuAvailableBytes = 0;
  GpuProbeSource gpuSource = GpuProbeSource::Unavailable;
  QString adapterName;
  quint32 nodeMask = 0;
};

class SystemResourceProbe final {
public:
  // GPU queries require this exact context to be current on the calling thread.
  // A null or non-current context still permits the system-memory probe.
  // Values are volatile observations, not reservations or allocation guarantees.
  [[nodiscard]] static SystemResourceSnapshot
  probe(QOpenGLContext *context = nullptr);
};

} // namespace gsw
