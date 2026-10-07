#pragma once

#include "SystemResourceProbe.h"

#include <memory>

namespace gsw {

struct ResourceBudgetPolicy final {
  enum class Mode { Automatic, Manual };
  Mode mode = Mode::Automatic;
  qint64 ramLimitMiB = 32768;
  qint64 gpuLimitMiB = 8192;
  bool operator==(const ResourceBudgetPolicy &) const = default;
};

struct ResourceBudgetStatus final {
  SystemResourceSnapshot snapshot;
  qint64 managedRamBytes = 0;
  qint64 managedGpuBytes = 0;
  qint64 reservedRamBytes = 0;
  qint64 reservedGpuBytes = 0;
  qint64 effectiveRamBudgetBytes = 0;
  qint64 effectiveGpuBudgetBytes = 0;
  int residentMeshCount = 0;
  int pagedMeshCount = 0;
  int loadingMeshCount = 0;
};

struct ResourceBudgetState;
class ResourceBudgetController;

// The lease remains valid after a controller handle or scene is removed. Keep
// it with the worker, result and pending uploads until their allocations really
// end; abandoning a UI request does not stop a QtConcurrent worker.
class ResourceReservation final {
public:
  ~ResourceReservation();
  ResourceReservation(const ResourceReservation &) = delete;
  ResourceReservation &operator=(const ResourceReservation &) = delete;

  // Replaces this lease's cost atomically. On refusal its existing cost stays
  // reserved. Component-wise reductions are always permitted, even under
  // pressure, so work can release capacity rather than being stuck over budget.
  [[nodiscard]] bool tryResize(qint64 ramPeakBytes, qint64 gpuStorageBytes);
  // Semantic result of the last admission attempt, independent of translated
  // error text. A successful fallback clears an earlier complete-load refusal.
  [[nodiscard]] bool admissionWasRefused() const;
  void clearRam();
  void clearGpu();

private:
  friend class ResourceBudgetController;
  ResourceReservation(std::shared_ptr<ResourceBudgetState> state, quint64 token);
  std::shared_ptr<ResourceBudgetState> mState;
  quint64 mToken = 0;
  bool mLastAdmissionRefused = false;
};

// A thread-safe ledger, not a hardware probe. Its mutex only guards policy,
// snapshots, usage and reservations; it never performs I/O, emits signals or
// calls a graphics API. Pending RAM retains the conservative full peak while a
// worker is building, even if some of it is already reflected in live RAM free.
class ResourceBudgetController final {
public:
  ResourceBudgetController();
  void setPolicy(const ResourceBudgetPolicy &policy);
  [[nodiscard]] ResourceBudgetPolicy policy() const;
  void updateSnapshot(const SystemResourceSnapshot &snapshot);
  void setManagedUsage(qint64 ramBytes, qint64 gpuBytes);
  [[nodiscard]] ResourceBudgetStatus status() const;
  [[nodiscard]] std::shared_ptr<ResourceReservation> createReservation();

private:
  std::shared_ptr<ResourceBudgetState> mState;
};

} // namespace gsw
