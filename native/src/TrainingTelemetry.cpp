#include "TrainingTelemetry.h"

#include <algorithm>

namespace gsw {

void TrainingTelemetry::reset(const int expectedIterations) {
  mSamples.clear();
  mExpectedIterations = std::max(expectedIterations, 0);
  mIteration.reset();
  mLoss.reset();
  mPsnr.reset();
  mGaussianCount.reset();
  mIterationMilliseconds.reset();
  mElapsedSeconds.reset();
}

bool TrainingTelemetry::ingest(const WorkerStatus &status) {
  bool changed = false;
  const auto update = [&changed](auto &target, const auto &source) {
    if (source.has_value() && target != source) {
      target = source;
      changed = true;
    }
  };

  update(mIteration, status.iteration);
  update(mLoss, status.loss);
  update(mPsnr, status.psnr);
  update(mGaussianCount, status.gaussianCount);
  update(mIterationMilliseconds, status.iterationMilliseconds);
  update(mElapsedSeconds, status.elapsedSeconds);
  if (status.totalIterations.has_value() &&
      mExpectedIterations != status.totalIterations.value()) {
    mExpectedIterations = status.totalIterations.value();
    changed = true;
  }

  if (!status.iteration.has_value() ||
      (!status.loss.has_value() && !status.psnr.has_value())) {
    return changed;
  }

  const int iteration = status.iteration.value();
  if (!mSamples.isEmpty() && iteration < mSamples.back().iteration) {
    return changed;
  }

  if (!mSamples.isEmpty() && iteration == mSamples.back().iteration) {
    TrainingSample &sample = mSamples.back();
    if (status.loss.has_value()) {
      sample.loss = status.loss;
    }
    if (status.psnr.has_value()) {
      sample.psnr = status.psnr;
    }
    return true;
  }

  mSamples.push_back(TrainingSample{iteration, status.loss, status.psnr});
  if (mSamples.size() > MaximumSamples) {
    mSamples.remove(0, mSamples.size() - MaximumSamples);
  }
  return true;
}

const QVector<TrainingSample> &TrainingTelemetry::samples() const {
  return mSamples;
}

int TrainingTelemetry::expectedIterations() const { return mExpectedIterations; }
std::optional<int> TrainingTelemetry::iteration() const { return mIteration; }
std::optional<double> TrainingTelemetry::loss() const { return mLoss; }
std::optional<double> TrainingTelemetry::psnr() const { return mPsnr; }
std::optional<qint64> TrainingTelemetry::gaussianCount() const {
  return mGaussianCount;
}
std::optional<double> TrainingTelemetry::iterationMilliseconds() const {
  return mIterationMilliseconds;
}
std::optional<double> TrainingTelemetry::elapsedSeconds() const {
  return mElapsedSeconds;
}

} // namespace gsw
