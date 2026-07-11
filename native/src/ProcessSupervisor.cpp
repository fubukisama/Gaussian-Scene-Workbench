#include "ProcessSupervisor.h"

#include <QProcessEnvironment>
#include <QTimer>

namespace gsw {

ProcessSupervisor::ProcessSupervisor(QObject *parent) : QObject(parent) {
  mProcess.setProcessChannelMode(QProcess::MergedChannels);

  connect(&mProcess, &QProcess::readyReadStandardOutput, this, &ProcessSupervisor::drainOutput);
  connect(&mProcess, &QProcess::started, this, [this]() {
    emit taskStarted(mActiveTask);
    emit runningChanged(true);
  });
  connect(&mProcess, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      emit outputReady(tr("Failed to start process: %1\n").arg(mProcess.errorString()));
      const QString failedTask = mActiveTask;
      mActiveTask.clear();
      mAcceptsCancelCommand = false;
      emit taskFinished(failedTask, -1, false);
      emit runningChanged(false);
    }
  });
  connect(&mProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
            drainOutput();
            const QString finishedTask = mActiveTask;
            const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
            mActiveTask.clear();
            mAcceptsCancelCommand = false;
            emit taskFinished(finishedTask, exitCode, succeeded);
            emit runningChanged(false);
          });
}

bool ProcessSupervisor::isRunning() const { return mProcess.state() != QProcess::NotRunning; }
QString ProcessSupervisor::activeTask() const { return mActiveTask; }

bool ProcessSupervisor::start(const QString &taskName, const QString &program,
                              const QStringList &arguments, const QString &workingDirectory,
                              const QProcessEnvironment &environment,
                              const bool acceptsCancelCommand) {
  if (isRunning() || program.isEmpty()) {
    return false;
  }

  mActiveTask = taskName;
  if (!workingDirectory.isEmpty()) {
    mProcess.setWorkingDirectory(workingDirectory);
  }
  mAcceptsCancelCommand = acceptsCancelCommand;
  mProcess.setProcessEnvironment(environment.isEmpty()
                                     ? QProcessEnvironment::systemEnvironment()
                                     : environment);
  mProcess.start(program, arguments,
                 acceptsCancelCommand ? QIODevice::ReadWrite : QIODevice::ReadOnly);
  return true;
}

void ProcessSupervisor::stop() {
  if (!isRunning()) {
    return;
  }

  const qint64 processId = mProcess.processId();
  if (mAcceptsCancelCommand) {
    emit outputReady(tr("Requesting graceful task cancellation...\n"));
    mProcess.write("cancel\n");
    mProcess.waitForBytesWritten(500);
  } else {
    emit outputReady(tr("Requesting task termination...\n"));
    mProcess.terminate();
  }

  const int timeout = mAcceptsCancelCommand ? 6500 : 2500;
  QTimer::singleShot(timeout, this, [this, processId]() {
    if (!isRunning() || mProcess.processId() != processId) {
      return;
    }
    emit outputReady(tr("Task did not stop in time; terminating its process tree.\n"));
#ifdef Q_OS_WIN
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(processId),
                       QStringLiteral("/T"), QStringLiteral("/F")});
#endif
    if (isRunning() && mProcess.processId() == processId) {
      mProcess.kill();
    }
  });
}

void ProcessSupervisor::drainOutput() {
  const QByteArray bytes = mProcess.readAllStandardOutput();
  if (!bytes.isEmpty()) {
    emit outputReady(QString::fromUtf8(bytes));
  }
}

} // namespace gsw
