#include "UploadDrainBudget.h"

#include <QTest>

#include <limits>

class UploadDrainBudgetTests final : public QObject {
  Q_OBJECT

private slots:
  void smallCompletedPagesDrainTogether();
  void wholePagesStopAtByteAllowance();
  void elapsedAllowanceStopsAdditionalWork();
  void byteAccountingCannotWrapIntoMoreAllowance();
  void oversizedFirstPageStillMakesProgress();
  void unlimitedBytesStillHonorsTimeAllowance();
  void failedOrInvalidAttemptsConsumeWorkAllowance();
  void zeroAttemptAllowanceDisablesWork();
  void newBatchCanContinueRemainingPages();
  void defaultByteAllowanceIsThirtyTwoMiB();
};

void UploadDrainBudgetTests::smallCompletedPagesDrainTogether() {
  gsw::lichtfeld::UploadDrainBudget batch(4096, 8, 4'000'000);
  for (int page = 0; page < 3; ++page) {
    QVERIFY2(batch.permits(1024, 200'000),
             "Completed small pages must not wait one rendered frame each.");
    batch.recordPage(1024);
  }
}

void UploadDrainBudgetTests::wholePagesStopAtByteAllowance() {
  gsw::lichtfeld::UploadDrainBudget batch(3072, 8, 4'000'000);
  QVERIFY(batch.permits(1024, 0));
  batch.recordPage(1024);
  QVERIFY(batch.permits(2048, 0));
  batch.recordPage(2048);
  QVERIFY2(!batch.permits(1, 0),
           "The next whole page must remain queued after the byte allowance.");
}

void UploadDrainBudgetTests::elapsedAllowanceStopsAdditionalWork() {
  gsw::lichtfeld::UploadDrainBudget batch(4096, 8, 4'000'000);
  QVERIFY(batch.permits(1024, 0));
  batch.recordPage(1024);
  QVERIFY(batch.permits(1024, 3'999'999));
  QVERIFY2(!batch.permits(1024, 4'000'000),
           "The caller's elapsed-time allowance must stop additional work.");
}

void UploadDrainBudgetTests::byteAccountingCannotWrapIntoMoreAllowance() {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  gsw::lichtfeld::UploadDrainBudget batch(maximum, 8, 0);
  QVERIFY(batch.permits(maximum - 16, 0));
  batch.recordPage(maximum - 16);
  QVERIFY2(!batch.permits(32, 0),
           "Overflow must not manufacture byte allowance.");
  QVERIFY(batch.permits(16, 0));
}

void UploadDrainBudgetTests::oversizedFirstPageStillMakesProgress() {
  gsw::lichtfeld::UploadDrainBudget batch(1024, 8, 4'000'000);
  QVERIFY2(batch.permits(8192, 6'000'000),
           "An indivisible first page cannot starve under a small allowance.");
  batch.recordPage(8192);
  QVERIFY(!batch.permits(1024, 6'000'000));
}

void UploadDrainBudgetTests::unlimitedBytesStillHonorsTimeAllowance() {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  gsw::lichtfeld::UploadDrainBudget batch(0, 8, 4'000'000);
  QVERIFY(batch.permits(maximum, 0));
  batch.recordPage(maximum);
  QVERIFY(batch.permits(maximum, 1));
  batch.recordPage(maximum);
  QVERIFY(batch.permits(1, 2));
  QVERIFY(!batch.permits(1, 4'000'000));

  gsw::lichtfeld::UploadDrainBudget untimed(0, 8, 0);
  QVERIFY(untimed.permits(1024, maximum));
  untimed.recordPage(1024);
  QVERIFY(untimed.permits(1024, maximum));
}

void UploadDrainBudgetTests::failedOrInvalidAttemptsConsumeWorkAllowance() {
  gsw::lichtfeld::UploadDrainBudget batch;
  for (int attempt = 0; attempt < 8; ++attempt) {
    QVERIFY(batch.permits(0, 0));
    // The caller records an attempted page even if it failed or was invalid.
    batch.recordPage(0);
  }
  QVERIFY2(!batch.permits(0, 0),
           "Failed/invalid work must not bypass the attempt-count limit.");
}

void UploadDrainBudgetTests::zeroAttemptAllowanceDisablesWork() {
  gsw::lichtfeld::UploadDrainBudget batch(0, 0, 0);
  QVERIFY(!batch.permits(0, 0));
  QVERIFY(!batch.permits(8192, 0));
}

void UploadDrainBudgetTests::newBatchCanContinueRemainingPages() {
  gsw::lichtfeld::UploadDrainBudget first(1024, 8, 4'000'000);
  QVERIFY(first.permits(1024, 0));
  first.recordPage(1024);
  QVERIFY(!first.permits(512, 0));

  gsw::lichtfeld::UploadDrainBudget next(1024, 8, 4'000'000);
  QVERIFY(next.permits(512, 0));
  next.recordPage(512);
  QVERIFY(next.permits(512, 1));
  next.recordPage(512);
  QVERIFY(!next.permits(1, 2));
}

void UploadDrainBudgetTests::defaultByteAllowanceIsThirtyTwoMiB() {
  gsw::lichtfeld::UploadDrainBudget batch;
  QVERIFY(batch.permits(16ULL * 1024 * 1024, 0));
  batch.recordPage(16ULL * 1024 * 1024);
  QVERIFY(batch.permits(16ULL * 1024 * 1024, 1));
  batch.recordPage(16ULL * 1024 * 1024);
  QVERIFY(!batch.permits(1, 2));
}

QTEST_GUILESS_MAIN(UploadDrainBudgetTests)
#include "UploadDrainBudgetTests.moc"
