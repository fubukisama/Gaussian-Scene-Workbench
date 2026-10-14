#include "TrainingTelemetry.h"

#include <algorithm>
#include <cmath>

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
  mPerformance = {};
  mGrowth = {};
}

bool TrainingTelemetry::ingest(const WorkerStatus &status) {
  bool changed = false;
  const auto updateObserver = [&changed](QJsonObject &target, const QJsonObject &source) {
    if (source.isEmpty() || (!target.isEmpty() &&
        source.value(QStringLiteral("iteration")).toInteger() <
            target.value(QStringLiteral("iteration")).toInteger())) return;
    if (source != target) {
      target = source;
      changed = true;
    }
  };
  // These streams have their own sampling intervals. A delayed metric snapshot
  // must not suppress a newer wall-time sample or a growth admission decision.
  updateObserver(mPerformance, normalizeTrainingPerformance(status.trainingPerformance));
  updateObserver(mGrowth, normalizeTrainingGrowth(status.trainingGrowth));
  if (status.iteration.has_value() && (*status.iteration < 0 ||
      (mIteration.has_value() && *status.iteration < *mIteration))) return changed;
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

const QJsonObject &TrainingTelemetry::performance() const { return mPerformance; }
const QJsonObject &TrainingTelemetry::growth() const { return mGrowth; }
std::optional<double> TrainingTelemetry::wallStepMilliseconds() const {
  if (mPerformance.isEmpty()) return std::nullopt;
  return mPerformance.value(QStringLiteral("wall_step_ms")).toDouble();
}

std::optional<double> TrainingTelemetry::iterationsPerSecond() const {
  const auto wall = wallStepMilliseconds();
  if (!wall.has_value() || *wall <= 0.0) return std::nullopt;
  const double rate = 1000.0 / *wall;
  return std::isfinite(rate) ? std::optional<double>(rate) : std::nullopt;
}

std::optional<double> TrainingTelemetry::estimatedRemainingSeconds() const {
  const auto wall = wallStepMilliseconds();
  if (!wall.has_value() || *wall <= 0.0 || !mIteration.has_value() || mExpectedIterations <= 0)
    return std::nullopt;
  const int remaining = std::max(mExpectedIterations - *mIteration, 0);
  const double seconds = static_cast<double>(remaining) * (*wall / 1000.0);
  return std::isfinite(seconds) ? std::optional<double>(seconds) : std::nullopt;
}

} // namespace gsw
