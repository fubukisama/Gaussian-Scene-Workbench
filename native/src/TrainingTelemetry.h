#pragma once

#include "ProcessSupervisor.h"

#include <QVector>

#include <optional>

namespace gsw {

struct TrainingSample final {
  int iteration = 0;
  std::optional<double> loss;
  std::optional<double> psnr;
};

class TrainingTelemetry final {
public:
  static constexpr qsizetype MaximumSamples = 200;

  void reset(int expectedIterations = 0);
  bool ingest(const WorkerStatus &status);

  [[nodiscard]] const QVector<TrainingSample> &samples() const;
  [[nodiscard]] int expectedIterations() const;
  [[nodiscard]] std::optional<int> iteration() const;
  [[nodiscard]] std::optional<double> loss() const;
  [[nodiscard]] std::optional<double> psnr() const;
  [[nodiscard]] std::optional<qint64> gaussianCount() const;
  [[nodiscard]] std::optional<double> iterationMilliseconds() const;
  [[nodiscard]] std::optional<double> elapsedSeconds() const;

private:
  QVector<TrainingSample> mSamples;
  int mExpectedIterations = 0;
  std::optional<int> mIteration;
  std::optional<double> mLoss;
  std::optional<double> mPsnr;
  std::optional<qint64> mGaussianCount;
  std::optional<double> mIterationMilliseconds;
  std::optional<double> mElapsedSeconds;
};

} // namespace gsw
