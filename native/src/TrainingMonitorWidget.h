#pragma once

#include "TrainingTelemetry.h"

#include <QWidget>
#include <array>

class QLabel;
class QProgressBar;
class QGridLayout;
class QScrollArea;

namespace gsw {

class TrainingCurvesWidget;

class TrainingMonitorWidget final : public QWidget {
  Q_OBJECT

public:
  explicit TrainingMonitorWidget(QWidget *parent = nullptr);

  void beginTraining(const QString &taskName, const QString &backend,
                     int expectedIterations);
  void updateStatus(const WorkerStatus &status);
  void finishTraining(bool succeeded, bool cancelled, bool paused = false);

  [[nodiscard]] const TrainingTelemetry &telemetry() const;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void refreshMetrics();
  void retranslateStatus();
  void refreshTaskTitle();
  void relayoutMetrics();
  QString mTaskTitle;
  QString mLastStage;
  bool mHasTraining = false;
  bool mFinished = false;
  bool mSucceeded = false;
  bool mCancelled = false;
  bool mPaused = false;
  bool mSparsePreview = false;
  int mDensityGuardIteration = 0;
  int mDensityGuardDeferred = 0;
  WorkerStatus mReconstructionStatus;
  QJsonObject mTrainingSummary;

  TrainingTelemetry mTelemetry;
  QScrollArea *mScrollArea = nullptr;
  QGridLayout *mMetricsGrid = nullptr;
  std::array<QLabel *, 7> mMetricCaptions{};
  std::array<QLabel *, 7> mMetricValues{};
  int mMetricColumns = 0;
  TrainingCurvesWidget *mCurves = nullptr;
  QLabel *mTitle = nullptr;
  QLabel *mState = nullptr;
  QLabel *mIteration = nullptr;
  QLabel *mLoss = nullptr;
  QLabel *mPsnr = nullptr;
  QLabel *mPrimitiveCountCaption = nullptr;
  QLabel *mGaussianCount = nullptr;
  QLabel *mSpeed = nullptr;
  QLabel *mElapsed = nullptr;
  QLabel *mRemaining = nullptr;
  QLabel *mDensityWarning = nullptr;
  QLabel *mReconstructionQuality = nullptr;
  QLabel *mEffectiveParameters = nullptr;
  QProgressBar *mProgress = nullptr;
};

} // namespace gsw
