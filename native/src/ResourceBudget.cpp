#include "ResourceBudget.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <limits>
#include <utility>

namespace gsw {
namespace {

constexpr qint64 kMiB = 1024LL * 1024;
constexpr qint64 kRamSafetyBytes = 512 * kMiB;
constexpr qint64 kGpuSafetyBytes = 256 * kMiB;
constexpr qint64 kUnknownBudgetBytes = 512 * kMiB;

qint64 nonnegative(const qint64 value) {
  return std::max<qint64>(0, value);
}

qint64 saturatedAdd(const qint64 left, const qint64 right) {
  const qint64 a = nonnegative(left);
  const qint64 b = nonnegative(right);
  return a > std::numeric_limits<qint64>::max() - b
             ? std::numeric_limits<qint64>::max()
             : a + b;
}

qint64 subtractAvailable(const qint64 value, const qint64 cost) {
  const qint64 a = nonnegative(value);
  const qint64 b = nonnegative(cost);
  return a > b ? a - b : 0;
}

qint64 bytesFromMiB(const qint64 value) {
  const qint64 megabytes = nonnegative(value);
  return megabytes > std::numeric_limits<qint64>::max() / kMiB
             ? std::numeric_limits<qint64>::max()
             : megabytes * kMiB;
}

qint64 safeTotal(const qint64 managed, const qint64 available,
                const qint64 minimumReserve) {
  const qint64 bytes = nonnegative(available);
  const qint64 reserve = std::max(bytes / 10, minimumReserve);
  // A reserve shortfall is pressure on existing managed storage, not merely
  // zero room for new work. Permit a lower total so page caches can be trimmed.
  if (reserve > bytes) {
    return subtractAvailable(managed, reserve - bytes);
  }
  return saturatedAdd(managed, bytes - reserve);
}

struct ReservationCost final {
  qint64 ramBytes = 0;
  qint64 gpuBytes = 0;
};

} // namespace

struct ResourceBudgetState final {
  mutable QMutex mutex;
  ResourceBudgetPolicy policy;
  SystemResourceSnapshot snapshot;
  qint64 managedRamBytes = 0;
  qint64 managedGpuBytes = 0;
  qint64 snapshotManagedRamBytes = 0;
  qint64 snapshotManagedGpuBytes = 0;
  quint64 nextToken = 1;
  QHash<quint64, ReservationCost> reservations;
};

namespace {

ResourceBudgetStatus statusLocked(const ResourceBudgetState &state) {
  ResourceBudgetStatus result;
  result.snapshot = state.snapshot;
  result.managedRamBytes = state.managedRamBytes;
  result.managedGpuBytes = state.managedGpuBytes;
  for (const auto &cost : state.reservations) {
    result.reservedRamBytes = saturatedAdd(result.reservedRamBytes,
                                          cost.ramBytes);
    result.reservedGpuBytes = saturatedAdd(result.reservedGpuBytes,
                                          cost.gpuBytes);
    if (cost.ramBytes > 0 || cost.gpuBytes > 0) {
      ++result.loadingMeshCount;
    }
  }

  qint64 safeRamTotal = kUnknownBudgetBytes;
  if (state.snapshot.systemMemoryKnown) {
    qint64 available = std::min(
        nonnegative(state.snapshot.physicalAvailableBytes),
        nonnegative(state.snapshot.commitAvailableBytes));
    // Managed growth since the last observation might not yet be represented
    // in its free counters. Debit that growth until a fresh snapshot arrives;
    // reductions do not assume the OS/driver has already reclaimed storage.
    available = subtractAvailable(
        available, subtractAvailable(state.managedRamBytes,
                                     state.snapshotManagedRamBytes));
    safeRamTotal = safeTotal(state.managedRamBytes, available, kRamSafetyBytes);
  }
  result.effectiveRamBudgetBytes = safeRamTotal;

  qint64 safeGpuTotal = kUnknownBudgetBytes;
  if (state.snapshot.gpuSource != GpuProbeSource::Unavailable) {
    qint64 available = subtractAvailable(
        state.snapshot.gpuAvailableBytes,
        subtractAvailable(state.managedGpuBytes,
                          state.snapshotManagedGpuBytes));
    safeGpuTotal = safeTotal(state.managedGpuBytes, available, kGpuSafetyBytes);
    if (state.snapshot.gpuSource == GpuProbeSource::DxgiProcessBudget) {
      // Zero available headroom does not express the amount of an existing
      // Windows process-budget overrun. Debit that separate deficit as well as
      // the safety reserve. This can intentionally request conservative trimming.
      safeGpuTotal = subtractAvailable(
          safeGpuTotal, subtractAvailable(state.snapshot.gpuUsageBytes,
                                          state.snapshot.gpuBudgetBytes));
    }
  } else if (state.policy.mode == ResourceBudgetPolicy::Mode::Manual) {
    // Unknown GPU measurements remain explicitly unknown in the snapshot.
    // A manual user ceiling is not represented as measured available VRAM.
    safeGpuTotal = bytesFromMiB(state.policy.gpuLimitMiB);
  }
  result.effectiveGpuBudgetBytes = safeGpuTotal;

  if (state.policy.mode == ResourceBudgetPolicy::Mode::Manual) {
    result.effectiveRamBudgetBytes = std::min(
        result.effectiveRamBudgetBytes, bytesFromMiB(state.policy.ramLimitMiB));
    result.effectiveGpuBudgetBytes = std::min(
        result.effectiveGpuBudgetBytes, bytesFromMiB(state.policy.gpuLimitMiB));
  }
  return result;
}

} // namespace

ResourceReservation::ResourceReservation(
    std::shared_ptr<ResourceBudgetState> state, const quint64 token)
    : mState(std::move(state)), mToken(token) {}

ResourceReservation::~ResourceReservation() {
  const QMutexLocker locker(&mState->mutex);
  mState->reservations.remove(mToken);
}

bool ResourceReservation::tryResize(const qint64 ramPeakBytes,
                                     const qint64 gpuStorageBytes) {
  const QMutexLocker locker(&mState->mutex);
  if (ramPeakBytes < 0 || gpuStorageBytes < 0) {
    mLastAdmissionRefused = true;
    return false;
  }
  auto own = mState->reservations.find(mToken);
  if (own == mState->reservations.end()) {
    mLastAdmissionRefused = true;
    return false;
  }
  const ResourceBudgetStatus current = statusLocked(*mState);
  qint64 otherRam = 0;
  qint64 otherGpu = 0;
  for (auto entry = mState->reservations.cbegin();
       entry != mState->reservations.cend(); ++entry) {
    if (entry.key() == mToken) {
      continue;
    }
    otherRam = saturatedAdd(otherRam, entry.value().ramBytes);
    otherGpu = saturatedAdd(otherGpu, entry.value().gpuBytes);
  }
  const qint64 remainingRam = subtractAvailable(
      subtractAvailable(current.effectiveRamBudgetBytes,
                        mState->managedRamBytes),
      otherRam);
  const qint64 remainingGpu = subtractAvailable(
      subtractAvailable(current.effectiveGpuBudgetBytes,
                        mState->managedGpuBytes),
      otherGpu);
  const bool ramFits = ramPeakBytes <= own->ramBytes ||
                       ramPeakBytes <= remainingRam;
  const bool gpuFits = gpuStorageBytes <= own->gpuBytes ||
                       gpuStorageBytes <= remainingGpu;
  if (!ramFits || !gpuFits) {
    mLastAdmissionRefused = true;
    return false;
  }
  mLastAdmissionRefused = false;
  own->ramBytes = ramPeakBytes;
  own->gpuBytes = gpuStorageBytes;
  return true;
}

bool ResourceReservation::admissionWasRefused() const {
  const QMutexLocker locker(&mState->mutex);
  return mLastAdmissionRefused;
}

void ResourceReservation::clearRam() {
  const QMutexLocker locker(&mState->mutex);
  auto own = mState->reservations.find(mToken);
  if (own != mState->reservations.end()) {
    own->ramBytes = 0;
  }
}

void ResourceReservation::clearGpu() {
  const QMutexLocker locker(&mState->mutex);
  auto own = mState->reservations.find(mToken);
  if (own != mState->reservations.end()) {
    own->gpuBytes = 0;
  }
}

ResourceBudgetController::ResourceBudgetController()
    : mState(std::make_shared<ResourceBudgetState>()) {}

void ResourceBudgetController::setPolicy(const ResourceBudgetPolicy &policy) {
  const QMutexLocker locker(&mState->mutex);
  mState->policy = policy;
}

ResourceBudgetPolicy ResourceBudgetController::policy() const {
  const QMutexLocker locker(&mState->mutex);
  return mState->policy;
}

void ResourceBudgetController::updateSnapshot(
    const SystemResourceSnapshot &snapshot) {
  const QMutexLocker locker(&mState->mutex);
  mState->snapshot = snapshot;
  mState->snapshotManagedRamBytes = mState->managedRamBytes;
  mState->snapshotManagedGpuBytes = mState->managedGpuBytes;
}

void ResourceBudgetController::setManagedUsage(const qint64 ramBytes,
                                               const qint64 gpuBytes) {
  const QMutexLocker locker(&mState->mutex);
  mState->managedRamBytes = nonnegative(ramBytes);
  mState->managedGpuBytes = nonnegative(gpuBytes);
}

ResourceBudgetStatus ResourceBudgetController::status() const {
  const QMutexLocker locker(&mState->mutex);
  return statusLocked(*mState);
}

std::shared_ptr<ResourceReservation>
ResourceBudgetController::createReservation() {
  quint64 token = 0;
  {
    const QMutexLocker locker(&mState->mutex);
    token = mState->nextToken++;
    while (token == 0 || mState->reservations.contains(token)) {
      token = mState->nextToken++;
    }
    mState->reservations.insert(token, ReservationCost{});
  }
  // shared_ptr control-block construction can fail under memory pressure and
  // delete its raw object. Construct it unlocked because that object's destructor
  // acquires the ledger mutex; a failure must not deadlock while cleaning up.
  try {
    return std::shared_ptr<ResourceReservation>(
        new ResourceReservation(mState, token));
  } catch (...) {
    const QMutexLocker locker(&mState->mutex);
    mState->reservations.remove(token);
    throw;
  }
}

} // namespace gsw
