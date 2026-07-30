#pragma once

#include <array>
#include <cstddef>

namespace gsw {

class FrameRateCounter final {
public:
  static constexpr std::size_t kSmoothingWindow = 60;

  void addRenderDurationMilliseconds(double milliseconds);

  [[nodiscard]] std::size_t sampleCount() const;
  [[nodiscard]] double averageFrameMilliseconds() const;
  [[nodiscard]] double framesPerSecond() const;

private:
  std::array<double, kSmoothingWindow> mRenderDurations{};
  std::size_t mNextSample = 0;
  std::size_t mSampleCount = 0;
  double mRenderDurationSum = 0.0;
};

} // namespace gsw
