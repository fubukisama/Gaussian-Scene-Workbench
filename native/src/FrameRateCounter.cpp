#include "FrameRateCounter.h"

#include <cmath>

namespace gsw {

void FrameRateCounter::addRenderDurationMilliseconds(
    const double milliseconds) {
  if (!std::isfinite(milliseconds) || milliseconds <= 0.0) {
    return;
  }

  if (mSampleCount == kSmoothingWindow) {
    mRenderDurationSum -= mRenderDurations[mNextSample];
  } else {
    ++mSampleCount;
  }
  mRenderDurations[mNextSample] = milliseconds;
  mRenderDurationSum += milliseconds;
  mNextSample = (mNextSample + 1) % kSmoothingWindow;
}

std::size_t FrameRateCounter::sampleCount() const { return mSampleCount; }

double FrameRateCounter::averageFrameMilliseconds() const {
  return mSampleCount > 0
             ? mRenderDurationSum / static_cast<double>(mSampleCount)
             : 0.0;
}

double FrameRateCounter::framesPerSecond() const {
  const double milliseconds = averageFrameMilliseconds();
  return milliseconds > 0.0 ? 1000.0 / milliseconds : 0.0;
}

} // namespace gsw
