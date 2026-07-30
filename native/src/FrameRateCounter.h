#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

namespace gsw {

class FrameRateCounter final {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  static constexpr std::size_t kSmoothingWindow = 60;

  void frameCompleted(TimePoint completionTime);

  [[nodiscard]] std::size_t sampleCount() const;
  [[nodiscard]] double averageFrameMilliseconds() const;
  [[nodiscard]] double framesPerSecond() const;

private:
  void addFrameIntervalMilliseconds(double milliseconds);

  std::array<double, kSmoothingWindow> mFrameIntervals{};
  std::size_t mNextSample = 0;
  std::size_t mSampleCount = 0;
  double mFrameIntervalSum = 0.0;
  std::optional<TimePoint> mLastCompletionTime;
};

} // namespace gsw
