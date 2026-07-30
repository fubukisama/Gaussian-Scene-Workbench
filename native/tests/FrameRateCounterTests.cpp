#include "FrameRateCounter.h"

#include <QtTest>

#include <chrono>

using namespace gsw;

class FrameRateCounterTests final : public QObject {
  Q_OBJECT

private slots:
  void averagesWarmupFramesWithoutZeroPadding();
  void keepsOnlyTheLatestSixtyFrames();
  void ignoresNonIncreasingFrameCompletions();
  void reportsActualFrameCadence();
};

void FrameRateCounterTests::averagesWarmupFramesWithoutZeroPadding() {
  FrameRateCounter counter;
  const FrameRateCounter::TimePoint origin{};

  counter.frameCompleted(origin);
  counter.frameCompleted(origin + std::chrono::milliseconds(10));
  counter.frameCompleted(origin + std::chrono::milliseconds(30));

  QCOMPARE(counter.sampleCount(), std::size_t(2));
  QCOMPARE(counter.averageFrameMilliseconds(), 15.0);
  QVERIFY(qAbs(counter.framesPerSecond() - (1000.0 / 15.0)) < 1.0e-9);
}

void FrameRateCounterTests::keepsOnlyTheLatestSixtyFrames() {
  FrameRateCounter counter;
  FrameRateCounter::TimePoint completion{};
  counter.frameCompleted(completion);
  for (std::size_t index = 0; index < FrameRateCounter::kSmoothingWindow;
       ++index) {
    completion += std::chrono::milliseconds(10);
    counter.frameCompleted(completion);
  }
  completion += std::chrono::milliseconds(20);
  counter.frameCompleted(completion);

  QCOMPARE(counter.sampleCount(), FrameRateCounter::kSmoothingWindow);
  QCOMPARE(counter.averageFrameMilliseconds(), 610.0 / 60.0);
  QVERIFY(qAbs(counter.framesPerSecond() - (60000.0 / 610.0)) < 1.0e-9);
}

void FrameRateCounterTests::ignoresNonIncreasingFrameCompletions() {
  FrameRateCounter counter;
  const FrameRateCounter::TimePoint origin{};

  counter.frameCompleted(origin);
  counter.frameCompleted(origin);
  counter.frameCompleted(origin - std::chrono::milliseconds(1));

  QCOMPARE(counter.sampleCount(), std::size_t(0));
  QCOMPARE(counter.averageFrameMilliseconds(), 0.0);
  QCOMPARE(counter.framesPerSecond(), 0.0);
}

void FrameRateCounterTests::reportsActualFrameCadence() {
  FrameRateCounter counter;
  FrameRateCounter::TimePoint completion{};
  counter.frameCompleted(completion);
  for (std::size_t index = 0; index < FrameRateCounter::kSmoothingWindow;
       ++index) {
    completion += std::chrono::milliseconds(8);
    counter.frameCompleted(completion);
  }

  QCOMPARE(counter.averageFrameMilliseconds(), 8.0);
  QCOMPARE(counter.framesPerSecond(), 125.0);
}

QTEST_GUILESS_MAIN(FrameRateCounterTests)

#include "FrameRateCounterTests.moc"
