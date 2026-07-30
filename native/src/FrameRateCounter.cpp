#include "FrameRateCounter.h"

#include <cmath>

namespace gsw {

void FrameRateCounter::frameCompleted(const TimePoint completionTime) {
  if (!mLastCompletionTime.has_value()) {
    mLastCompletionTime = completionTime;
    return;
  }

  const double milliseconds =
      std::chrono::duration<double, std::milli>(completionTime -
                                                *mLastCompletionTime)
          .count();
  if (!std::isfinite(milliseconds) || milliseconds <= 0.0) {
    return;
  }
  mLastCompletionTime = completionTime;
  addFrameIntervalMilliseconds(milliseconds);
}

void FrameRateCounter::addFrameIntervalMilliseconds(
    const double milliseconds) {
  if (mSampleCount == kSmoothingWindow) {
    mFrameIntervalSum -= mFrameIntervals[mNextSample];
  } else {
    ++mSampleCount;
  }
  mFrameIntervals[mNextSample] = milliseconds;
  mFrameIntervalSum += milliseconds;
  mNextSample = (mNextSample + 1) % kSmoothingWindow;
}

std::size_t FrameRateCounter::sampleCount() const { return mSampleCount; }

double FrameRateCounter::averageFrameMilliseconds() const {
  return mSampleCount > 0
             ? mFrameIntervalSum / static_cast<double>(mSampleCount)
             : 0.0;
}

double FrameRateCounter::framesPerSecond() const {
  const double milliseconds = averageFrameMilliseconds();
  return milliseconds > 0.0 ? 1000.0 / milliseconds : 0.0;
}

} // namespace gsw
