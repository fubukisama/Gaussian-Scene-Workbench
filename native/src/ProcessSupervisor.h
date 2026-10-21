#pragma once

#include "TrainingGpuPreviewProtocol.h"

#include <QByteArray>
#include <QJsonObject>
#include <QFile>
#include <QMetaType>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <optional>

namespace gsw {

struct WorkerStatus final {
  QString state;
  QString stage;
  std::optional<int> progressPercent;
  std::optional<int> iteration;
  std::optional<int> totalIterations;
  std::optional<double> loss;
  std::optional<double> psnr;
  std::optional<qint64> gaussianCount;
  std::optional<double> iterationMilliseconds;
  std::optional<double> elapsedSeconds;
  std::optional<int> previewIteration;
  QString previewPath;
  QString previewKind;
  QString reconstructionQuality;
  std::optional<int> reconstructionViews;
  std::optional<int> reconstructionInputs;
  std::optional<int> reconstructionPoints;
  QString generationIssue;
  std::optional<int> densityGuardIteration;
  std::optional<int> densityGuardDeferred;
  QJsonObject trainingSummary;
  QJsonObject trainingPerformance;
  QJsonObject trainingGrowth;
  QJsonObject runtimeConfiguration;
  QJsonObject trainingDelivery;
  QString failureReason;
};

[[nodiscard]] QJsonObject normalizeTrainingPerformance(const QJsonObject &payload);
[[nodiscard]] QJsonObject normalizeTrainingGrowth(const QJsonObject &payload);

class ProcessSupervisor final : public QObject {
  Q_OBJECT

public:
  explicit ProcessSupervisor(QObject *parent = nullptr);
  ~ProcessSupervisor() override;

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] QString activeTask() const;
  [[nodiscard]] bool wasStopRequested() const;
  // Pure observer decoding; callers and regression tests use the same seam.
  [[nodiscard]] static std::optional<WorkerStatus> decodeStatusEvent(const QByteArray &payload);
  // Read-only conservative writer gate for project migration/recovery cleanup.
  // Unverified active legacy records block destructive relocation too.
  [[nodiscard]] static bool hasUnfinishedTraining(const QString &jobStore);
  bool start(const QString &taskName, const QString &program, const QStringList &arguments,
             const QString &workingDirectory = {},
             const QProcessEnvironment &environment = {},
             bool acceptsCancelCommand = false);
  // Native Windows training only: detached worker, fresh durable stdout log,
  // query-only process identity and session-bound controls. Shutdown detaches
  // this observer; ordinary start() keeps its owned process-tree lifecycle.
  bool startPersistentTraining(const QString &taskName, const QString &program,
      const QStringList &arguments, const QString &workingDirectory,
      const QProcessEnvironment &environment, const QString &logPath,
      const QString &jobStore);
  [[nodiscard]] bool isPersistentTraining() const;
  void stop();
  bool requestPause();
  void shutdown();

signals:
  void taskLaunchRequested(const QString &taskName,
                           const QString &workingDirectory);
  void taskStarted(const QString &taskName);
  void outputReady(const QString &text);
  void workerStatusReady(const WorkerStatus &status);
  void trainingGpuPreviewReady(
      const gsw::TrainingGpuPreviewDescriptor &descriptor);
  void taskFinished(const QString &taskName, int exitCode, bool succeeded);
  void runningChanged(bool running);

private:
  void prepareProcessJob();
  bool attachProcessToJob();
  void terminateAndReleaseProcessJob();
  void drainOutput();
  void processBufferedOutput(bool flushTail);
  void handleOutputLine(const QByteArray &line);
  void pollPersistentTraining();
  void detachPersistentTraining();
  bool requestPersistentCommand(const QString &command);
  [[nodiscard]] std::optional<QJsonObject> persistentConnection() const;

  QProcess mProcess;
  QByteArray mOutputBuffer;
  QString mActiveTask;
  bool mAcceptsCancelCommand = false;
  bool mStopRequested = false;
  bool mShutdownStarted = false;
  quintptr mProcessJobHandle = 0;
  bool mPersistentTraining = false;
  quintptr mPersistentQueryHandle = 0;
  qint64 mPersistentProcessId = 0;
  QString mPersistentStartToken;
  QString mPersistentJobStore;
  QFile mPersistentLog;
  QTimer mPersistentPoll;
  QString mPersistentFinalState;
};

} // namespace gsw

Q_DECLARE_METATYPE(gsw::WorkerStatus)
