#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

namespace gsw {

struct WorkerStatus final {
  QString state;
  QString stage;
  std::optional<int> progressPercent;
};

class ProcessSupervisor final : public QObject {
  Q_OBJECT

public:
  explicit ProcessSupervisor(QObject *parent = nullptr);
  ~ProcessSupervisor() override;

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] QString activeTask() const;
  [[nodiscard]] bool wasStopRequested() const;
  bool start(const QString &taskName, const QString &program, const QStringList &arguments,
             const QString &workingDirectory = {},
             const QProcessEnvironment &environment = {},
             bool acceptsCancelCommand = false);
  void stop();
  void shutdown();

signals:
  void taskStarted(const QString &taskName);
  void outputReady(const QString &text);
  void workerStatusReady(const WorkerStatus &status);
  void taskFinished(const QString &taskName, int exitCode, bool succeeded);
  void runningChanged(bool running);

private:
  void prepareProcessJob();
  bool attachProcessToJob();
  void terminateAndReleaseProcessJob();
  void drainOutput();
  void processBufferedOutput(bool flushTail);
  void handleOutputLine(const QByteArray &line);

  QProcess mProcess;
  QByteArray mOutputBuffer;
  QString mActiveTask;
  bool mAcceptsCancelCommand = false;
  bool mStopRequested = false;
  bool mShutdownStarted = false;
  quintptr mProcessJobHandle = 0;
};

} // namespace gsw

Q_DECLARE_METATYPE(gsw::WorkerStatus)
