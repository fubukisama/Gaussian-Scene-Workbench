#pragma once

#include "TrainingTelemetry.h"

#include <QWidget>

class QLabel;
class QProgressBar;
class QTabWidget;

namespace gsw {

class TrainingCurvesWidget;

class TrainingMonitorWidget final : public QWidget {
  Q_OBJECT

public:
  explicit TrainingMonitorWidget(QWidget *parent = nullptr);

  void beginTraining(const QString &taskName, const QString &backend,
                     int expectedIterations);
  void updateStatus(const WorkerStatus &status);
  void finishTraining(bool succeeded, bool cancelled);

  [[nodiscard]] const TrainingTelemetry &telemetry() const;

private:
  void refreshMetrics();

  TrainingTelemetry mTelemetry;
  TrainingCurvesWidget *mCurves = nullptr;
  QLabel *mTitle = nullptr;
  QLabel *mState = nullptr;
  QLabel *mIteration = nullptr;
  QLabel *mLoss = nullptr;
  QLabel *mPsnr = nullptr;
  QLabel *mGaussianCount = nullptr;
  QLabel *mSpeed = nullptr;
  QLabel *mElapsed = nullptr;
  QLabel *mRemaining = nullptr;
  QProgressBar *mProgress = nullptr;
};

} // namespace gsw
