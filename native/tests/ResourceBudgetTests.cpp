#include "ResourceBudget.h"

#include <QtTest>

#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace gsw;

namespace {
constexpr qint64 kMiB = 1024LL * 1024;

SystemResourceSnapshot knownFreeSnapshot() {
  SystemResourceSnapshot snapshot;
  snapshot.systemMemoryKnown = true;
  snapshot.physicalTotalBytes = 16'384 * kMiB;
  snapshot.physicalAvailableBytes = 4096 * kMiB;
  snapshot.commitAvailableBytes = 4096 * kMiB;
  snapshot.gpuSource = GpuProbeSource::NvxApproximate;
  snapshot.gpuDedicatedBytes = 8192 * kMiB;
  snapshot.gpuAvailableBytes = 4096 * kMiB;
  return snapshot;
}

ResourceBudgetPolicy manualPolicy(const qint64 ramMiB, const qint64 gpuMiB) {
  ResourceBudgetPolicy policy;
  policy.mode = ResourceBudgetPolicy::Mode::Manual;
  policy.ramLimitMiB = ramMiB;
  policy.gpuLimitMiB = gpuMiB;
  return policy;
}
} // namespace

class ResourceBudgetTests final : public QObject {
  Q_OBJECT

private slots:
  void concurrentReservationsShareOneBudgetAtomically();
  void refusedResizeRetainsThePreviousReservation();
  void runningWorkerKeepsItsLeaseAfterTheUiOwnerDropsIt();
  void reservationsCanOutliveTheirController();
  void knownZeroHeadroomIsNotTheUnknownFallback();
  void nvxReserveShortfallRequestsTrimmingOfManagedStorage();
  void dxgiOverBudgetUsageRequestsAdditionalTrimming();
  void managedGrowthCannotReuseAnOldFreeMemorySnapshot();
  void manualUnknownGpuUsesTheCeilingWithoutInventingMeasurements();
  void ramAdmissionRespectsTheCommitAllowance();
  void clearingRamAndGpuAreIndependent();
  void existingReservationsCanShrinkAfterTheCeilingIsLowered();
};

void ResourceBudgetTests::concurrentReservationsShareOneBudgetAtomically() {
  ResourceBudgetController controller;
  controller.updateSnapshot(knownFreeSnapshot());
  controller.setPolicy(manualPolicy(1024, 1024));
  const auto firstLease = controller.createReservation();
  const auto secondLease = controller.createReservation();
  std::barrier simultaneousStart(3);
  auto first = std::async(std::launch::async, [&simultaneousStart, firstLease]() {
    simultaneousStart.arrive_and_wait();
    return firstLease->tryResize(700 * kMiB, 700 * kMiB);
  });
  auto second = std::async(std::launch::async, [&simultaneousStart, secondLease]() {
    simultaneousStart.arrive_and_wait();
    return secondLease->tryResize(700 * kMiB, 700 * kMiB);
  });
  simultaneousStart.arrive_and_wait();
  const bool firstAccepted = first.get();
  const bool secondAccepted = second.get();

  // Two 700 MiB models cannot both reserve the same 1024 MiB allowance.
  QCOMPARE(int(firstAccepted) + int(secondAccepted), 1);
  QCOMPARE(controller.status().reservedRamBytes, 700 * kMiB);
  QCOMPARE(controller.status().reservedGpuBytes, 700 * kMiB);
}

void ResourceBudgetTests::refusedResizeRetainsThePreviousReservation() {
  ResourceBudgetController controller;
  controller.updateSnapshot(knownFreeSnapshot());
  controller.setPolicy(manualPolicy(1024, 1024));
  const auto first = controller.createReservation();
  const auto second = controller.createReservation();
  QVERIFY(first->tryResize(500 * kMiB, 500 * kMiB));
  QVERIFY(!first->admissionWasRefused());
  QVERIFY(second->tryResize(300 * kMiB, 300 * kMiB));

  QVERIFY(!first->tryResize(800 * kMiB, 800 * kMiB));
  QVERIFY(first->admissionWasRefused());
  QCOMPARE(controller.status().reservedRamBytes, 800 * kMiB);
  QCOMPARE(controller.status().reservedGpuBytes, 800 * kMiB);
  QVERIFY(!first->tryResize(-1, 0));
  QCOMPARE(controller.status().reservedRamBytes, 800 * kMiB);
  QCOMPARE(controller.status().reservedGpuBytes, 800 * kMiB);

  // Replacing the first lease with 600 MiB must exclude its old 500 MiB cost.
  second->clearGpu();
  QVERIFY(first->tryResize(600 * kMiB, 600 * kMiB));
  QVERIFY(!first->admissionWasRefused());
  QCOMPARE(controller.status().reservedRamBytes, 900 * kMiB);
  QCOMPARE(controller.status().reservedGpuBytes, 600 * kMiB);
}

void ResourceBudgetTests::runningWorkerKeepsItsLeaseAfterTheUiOwnerDropsIt() {
  ResourceBudgetController controller;
  controller.updateSnapshot(knownFreeSnapshot());
  controller.setPolicy(manualPolicy(1024, 1024));
  auto owner = controller.createReservation();
  QVERIFY(owner->tryResize(700 * kMiB, 700 * kMiB));
  const auto nextImport = controller.createReservation();
  std::promise<void> started;
  auto observedStart = started.get_future();
  std::promise<void> finishWork;
  auto finishSignal = finishWork.get_future();
  std::jthread worker([held = owner, started = std::move(started),
                       finishSignal = std::move(finishSignal)]() mutable {
    started.set_value();
    finishSignal.wait();
    held.reset();
  });
  const auto startResult = observedStart.wait_for(std::chrono::seconds(2));
  owner.reset();
  const auto whileWorkerRuns = controller.status();
  const bool prematureAdmission = nextImport->tryResize(400 * kMiB, 400 * kMiB);
  // Release/join before making assertions so an assertion failure cannot leave
  // a real thread blocked on a test-controlled promise during stack unwinding.
  finishWork.set_value();
  worker.join();

  QCOMPARE(startResult, std::future_status::ready);
  QCOMPARE(whileWorkerRuns.reservedRamBytes, 700 * kMiB);
  QCOMPARE(whileWorkerRuns.reservedGpuBytes, 700 * kMiB);
  QVERIFY(!prematureAdmission);
  QCOMPARE(controller.status().reservedRamBytes, qint64(0));
  QCOMPARE(controller.status().reservedGpuBytes, qint64(0));
  QVERIFY(nextImport->tryResize(400 * kMiB, 400 * kMiB));
}

void ResourceBudgetTests::reservationsCanOutliveTheirController() {
  std::shared_ptr<ResourceReservation> held;
  std::shared_ptr<ResourceReservation> survivor;
  {
    ResourceBudgetController controller;
    controller.updateSnapshot(knownFreeSnapshot());
    controller.setPolicy(manualPolicy(1024, 1024));
    held = controller.createReservation();
    survivor = controller.createReservation();
    QVERIFY(held->tryResize(700 * kMiB, 700 * kMiB));
  }

  QVERIFY(!survivor->tryResize(400 * kMiB, 400 * kMiB));
  held.reset();
  QVERIFY(survivor->tryResize(400 * kMiB, 400 * kMiB));
  survivor->clearRam();
  survivor->clearGpu();
}

void ResourceBudgetTests::knownZeroHeadroomIsNotTheUnknownFallback() {
  ResourceBudgetController controller;
  auto snapshot = knownFreeSnapshot();
  snapshot.physicalAvailableBytes = 0;
  snapshot.commitAvailableBytes = 0;
  snapshot.gpuAvailableBytes = 0;
  controller.updateSnapshot(snapshot);
  const auto status = controller.status();

  QVERIFY(status.snapshot.systemMemoryKnown);
  QCOMPARE(status.snapshot.gpuSource, GpuProbeSource::NvxApproximate);
  QCOMPARE(status.effectiveRamBudgetBytes, qint64(0));
  QCOMPARE(status.effectiveGpuBudgetBytes, qint64(0));
  const auto lease = controller.createReservation();
  QVERIFY(!lease->tryResize(1, 0));
  QVERIFY(!lease->tryResize(0, 1));

  ResourceBudgetController unknown;
  QCOMPARE(unknown.status().snapshot.gpuSource, GpuProbeSource::Unavailable);
  QCOMPARE(unknown.status().effectiveGpuBudgetBytes, 512 * kMiB);
}

void ResourceBudgetTests::nvxReserveShortfallRequestsTrimmingOfManagedStorage() {
  ResourceBudgetController controller;
  controller.setManagedUsage(1024 * kMiB, 1024 * kMiB);
  auto snapshot = knownFreeSnapshot();
  snapshot.physicalAvailableBytes = 256 * kMiB;
  snapshot.gpuAvailableBytes = 128 * kMiB;
  controller.updateSnapshot(snapshot);

  // 1024 MiB managed with 256 MiB free and a 512 MiB RAM reserve => 768 MiB.
  // 1024 MiB managed with 128 MiB free and a 256 MiB GPU reserve => 896 MiB.
  QCOMPARE(controller.status().effectiveRamBudgetBytes, 768 * kMiB);
  QCOMPARE(controller.status().effectiveGpuBudgetBytes, 896 * kMiB);
  const auto lease = controller.createReservation();
  QVERIFY(!lease->tryResize(1, 1));
}

void ResourceBudgetTests::dxgiOverBudgetUsageRequestsAdditionalTrimming() {
  ResourceBudgetController controller;
  controller.setManagedUsage(0, 1024 * kMiB);
  auto snapshot = knownFreeSnapshot();
  snapshot.gpuSource = GpuProbeSource::DxgiProcessBudget;
  snapshot.gpuBudgetBytes = 1024 * kMiB;
  snapshot.gpuUsageBytes = 1280 * kMiB;
  snapshot.gpuAvailableBytes = 0;
  controller.updateSnapshot(snapshot);

  // Keep 256 MiB safety reserve, then trim the separate 256 MiB DXGI overrun.
  QCOMPARE(controller.status().effectiveGpuBudgetBytes, 512 * kMiB);
  QCOMPARE(controller.status().snapshot.gpuSource,
           GpuProbeSource::DxgiProcessBudget);
  const auto lease = controller.createReservation();
  QVERIFY(!lease->tryResize(0, 1));
}

void ResourceBudgetTests::managedGrowthCannotReuseAnOldFreeMemorySnapshot() {
  ResourceBudgetController controller;
  auto snapshot = knownFreeSnapshot();
  snapshot.physicalAvailableBytes = 1024 * kMiB;
  snapshot.commitAvailableBytes = 1024 * kMiB;
  snapshot.gpuAvailableBytes = 1024 * kMiB;
  controller.updateSnapshot(snapshot);
  const auto loaded = controller.createReservation();
  QVERIFY(loaded->tryResize(256 * kMiB, 256 * kMiB));
  // The worker/upload completes between hardware observations. Materialized
  // storage is managed now, but the old 1024 MiB free observation is unchanged.
  controller.setManagedUsage(256 * kMiB, 256 * kMiB);
  loaded->clearRam();
  loaded->clearGpu();

  QCOMPARE(controller.status().effectiveRamBudgetBytes, 512 * kMiB);
  QCOMPARE(controller.status().effectiveGpuBudgetBytes, 768 * kMiB);
  const auto next = controller.createReservation();
  QVERIFY(!next->tryResize(384 * kMiB, 0));
  QVERIFY(!next->tryResize(0, 640 * kMiB));
  QVERIFY(next->tryResize(256 * kMiB, 512 * kMiB));
}

void ResourceBudgetTests::manualUnknownGpuUsesTheCeilingWithoutInventingMeasurements() {
  ResourceBudgetController controller;
  auto snapshot = knownFreeSnapshot();
  snapshot.gpuSource = GpuProbeSource::Unavailable;
  snapshot.gpuDedicatedBytes = 0;
  snapshot.gpuAvailableBytes = 0;
  controller.updateSnapshot(snapshot);
  controller.setPolicy(manualPolicy(2048, 1536));
  const auto status = controller.status();

  QCOMPARE(status.effectiveRamBudgetBytes, 2048 * kMiB);
  QCOMPARE(status.effectiveGpuBudgetBytes, 1536 * kMiB);
  QCOMPARE(status.snapshot.gpuSource, GpuProbeSource::Unavailable);
  QCOMPARE(status.snapshot.gpuAvailableBytes, qint64(0));
  QCOMPARE(status.snapshot.gpuBudgetBytes, qint64(0));
  QCOMPARE(status.snapshot.gpuUsageBytes, qint64(0));
  const auto lease = controller.createReservation();
  QVERIFY(!lease->tryResize(0, 1600 * kMiB));
  QVERIFY(lease->tryResize(0, 1536 * kMiB));
}

void ResourceBudgetTests::ramAdmissionRespectsTheCommitAllowance() {
  ResourceBudgetController controller;
  auto snapshot = knownFreeSnapshot();
  snapshot.commitAvailableBytes = 768 * kMiB;
  controller.updateSnapshot(snapshot);

  // Physical free is 4096 MiB, but only 768 MiB can be committed; leave 512 MiB.
  QCOMPARE(controller.status().effectiveRamBudgetBytes, 256 * kMiB);
  const auto lease = controller.createReservation();
  QVERIFY(!lease->tryResize(257 * kMiB, 0));
  QVERIFY(lease->tryResize(256 * kMiB, 0));
}

void ResourceBudgetTests::clearingRamAndGpuAreIndependent() {
  ResourceBudgetController controller;
  controller.updateSnapshot(knownFreeSnapshot());
  controller.setPolicy(manualPolicy(1024, 1024));
  const auto first = controller.createReservation();
  QVERIFY(first->tryResize(256 * kMiB, 512 * kMiB));
  first->clearRam();
  QCOMPARE(controller.status().reservedRamBytes, qint64(0));
  QCOMPARE(controller.status().reservedGpuBytes, 512 * kMiB);
  const auto second = controller.createReservation();
  QVERIFY(second->tryResize(1024 * kMiB, 0));
  first->clearGpu();
  QCOMPARE(controller.status().reservedRamBytes, 1024 * kMiB);
  QCOMPARE(controller.status().reservedGpuBytes, qint64(0));
}

void ResourceBudgetTests::existingReservationsCanShrinkAfterTheCeilingIsLowered() {
  ResourceBudgetController controller;
  controller.updateSnapshot(knownFreeSnapshot());
  controller.setPolicy(manualPolicy(1024, 1024));
  const auto held = controller.createReservation();
  QVERIFY(held->tryResize(700 * kMiB, 700 * kMiB));
  controller.setPolicy(manualPolicy(128, 128));

  QVERIFY(held->tryResize(300 * kMiB, 300 * kMiB));
  QCOMPARE(controller.status().reservedRamBytes, 300 * kMiB);
  QCOMPARE(controller.status().reservedGpuBytes, 300 * kMiB);
  const auto another = controller.createReservation();
  QVERIFY(!another->tryResize(1, 1));
  held->clearRam();
  held->clearGpu();
  QVERIFY(another->tryResize(128 * kMiB, 128 * kMiB));
}

QTEST_APPLESS_MAIN(ResourceBudgetTests)

#include "ResourceBudgetTests.moc"
