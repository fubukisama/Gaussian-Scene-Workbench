#include "FrameRateCounter.h"

#include <QtTest>

#include <limits>

using namespace gsw;

class FrameRateCounterTests final : public QObject {
  Q_OBJECT

private slots:
  void averagesWarmupFramesWithoutZeroPadding();
  void keepsOnlyTheLatestSixtyFrames();
  void ignoresInvalidFrameIntervals();
  void reportsRenderThroughputAboveDisplayRefreshRate();
};

void FrameRateCounterTests::averagesWarmupFramesWithoutZeroPadding() {
  FrameRateCounter counter;

  counter.addRenderDurationMilliseconds(10.0);
  counter.addRenderDurationMilliseconds(20.0);

  QCOMPARE(counter.sampleCount(), std::size_t(2));
  QCOMPARE(counter.averageFrameMilliseconds(), 15.0);
  QVERIFY(qAbs(counter.framesPerSecond() - (1000.0 / 15.0)) < 1.0e-9);
}

void FrameRateCounterTests::keepsOnlyTheLatestSixtyFrames() {
  FrameRateCounter counter;
  for (std::size_t index = 0; index < FrameRateCounter::kSmoothingWindow;
       ++index) {
    counter.addRenderDurationMilliseconds(10.0);
  }
  counter.addRenderDurationMilliseconds(20.0);

  QCOMPARE(counter.sampleCount(), FrameRateCounter::kSmoothingWindow);
  QCOMPARE(counter.averageFrameMilliseconds(), 610.0 / 60.0);
  QVERIFY(qAbs(counter.framesPerSecond() - (60000.0 / 610.0)) < 1.0e-9);
}

void FrameRateCounterTests::ignoresInvalidFrameIntervals() {
  FrameRateCounter counter;

  counter.addRenderDurationMilliseconds(0.0);
  counter.addRenderDurationMilliseconds(-1.0);
  counter.addRenderDurationMilliseconds(
      std::numeric_limits<double>::quiet_NaN());
  counter.addRenderDurationMilliseconds(
      std::numeric_limits<double>::infinity());

  QCOMPARE(counter.sampleCount(), std::size_t(0));
  QCOMPARE(counter.averageFrameMilliseconds(), 0.0);
  QCOMPARE(counter.framesPerSecond(), 0.0);
}

void FrameRateCounterTests::reportsRenderThroughputAboveDisplayRefreshRate() {
  FrameRateCounter counter;
  for (std::size_t index = 0; index < FrameRateCounter::kSmoothingWindow;
       ++index) {
    counter.addRenderDurationMilliseconds(2.0);
  }

  QCOMPARE(counter.averageFrameMilliseconds(), 2.0);
  QCOMPARE(counter.framesPerSecond(), 500.0);
}

QTEST_GUILESS_MAIN(FrameRateCounterTests)

#include "FrameRateCounterTests.moc"
