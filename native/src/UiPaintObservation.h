#pragma once

#include <QtGlobal>
#include <array>

namespace gsw {

// Passive, explicitly enabled QA observation. No GL query, signal, timer or
// allocation occurs per sample. These are CPU host spans, never GPU timings.
struct UiPaintSample final {
  quint64 frameSerial = 0;
  qint64 hostNanoseconds = 0;
  int logicalWidth = 0;
  int logicalHeight = 0;
  qreal devicePixelRatio = 0;
  quint64 ownedCpuBytes = 0;
  quint64 requestedGpuBytes = 0;
  quint64 cacheGeneration = 0;
  bool cacheHit = false;
  bool diagnosticOverlap = false;
};

struct UiPaintObservation final {
  static constexpr qsizetype capacity = 512;
  std::array<UiPaintSample, capacity> samples{};
  qsizetype count = 0;
  bool overflow = false;
  bool enabled = false;
};

} // namespace gsw
