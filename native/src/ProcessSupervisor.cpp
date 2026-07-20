#include "ProcessSupervisor.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QTimer>

#include <cmath>
#include <utility>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {

namespace {

const QByteArray kWorkerEventPrefix = QByteArrayLiteral("[worker-event] ");

bool parseWorkerStatus(const QByteArray &payload, WorkerStatus *status) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return false;
  }

  const QJsonObject object = document.object();
  const QJsonValue versionValue = object.value(QStringLiteral("version"));
  const QJsonValue typeValue = object.value(QStringLiteral("type"));
  const QJsonValue stateValue = object.value(QStringLiteral("state"));
  const QJsonValue stageValue = object.value(QStringLiteral("stage"));
  if (!versionValue.isDouble() || versionValue.toDouble() != 1.0 ||
      !typeValue.isString() || typeValue.toString() != QStringLiteral("status") ||
      !stateValue.isString() || stateValue.toString().trimmed().isEmpty() ||
      !stageValue.isString() || stageValue.toString().trimmed().isEmpty()) {
    return false;
  }

  WorkerStatus parsedStatus;
  parsedStatus.state = stateValue.toString();
  parsedStatus.stage = stageValue.toString();

  const QJsonValue progressValue = object.value(QStringLiteral("progressPercent"));
  if (!progressValue.isUndefined()) {
    if (!progressValue.isDouble()) {
      return false;
    }
    const double progress = progressValue.toDouble();
    if (!std::isfinite(progress) || std::floor(progress) != progress || progress < 0.0 ||
        progress > 100.0) {
      return false;
    }
    parsedStatus.progressPercent = static_cast<int>(progress);
  }

  *status = std::move(parsedStatus);
  return true;
}

} // namespace

ProcessSupervisor::ProcessSupervisor(QObject *parent) : QObject(parent) {
  mProcess.setProcessChannelMode(QProcess::MergedChannels);

  connect(&mProcess, &QProcess::readyReadStandardOutput, this, &ProcessSupervisor::drainOutput);
  connect(&mProcess, &QProcess::started, this, [this]() {
    if (!attachProcessToJob()) {
      emit outputReady(
          tr("Warning: the task could not be attached to the shutdown job; "
             "process-tree cleanup will use the PID fallback.\n"));
    }
    emit taskStarted(mActiveTask);
    emit runningChanged(true);
  });
  connect(&mProcess, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      emit outputReady(tr("Failed to start process: %1\n").arg(mProcess.errorString()));
      const QString failedTask = mActiveTask;
      mActiveTask.clear();
      mAcceptsCancelCommand = false;
      terminateAndReleaseProcessJob();
      emit taskFinished(failedTask, -1, false);
      emit runningChanged(false);
    }
  });
  connect(&mProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
            drainOutput();
            processBufferedOutput(true);
            const QString finishedTask = mActiveTask;
            const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
            mActiveTask.clear();
            mAcceptsCancelCommand = false;
            terminateAndReleaseProcessJob();
            emit taskFinished(finishedTask, exitCode, succeeded);
            emit runningChanged(false);
          });
}

ProcessSupervisor::~ProcessSupervisor() {
  shutdown();
}

bool ProcessSupervisor::isRunning() const { return mProcess.state() != QProcess::NotRunning; }
QString ProcessSupervisor::activeTask() const { return mActiveTask; }
bool ProcessSupervisor::wasStopRequested() const { return mStopRequested; }

bool ProcessSupervisor::start(const QString &taskName, const QString &program,
                              const QStringList &arguments, const QString &workingDirectory,
                              const QProcessEnvironment &environment,
                              const bool acceptsCancelCommand) {
  if (mShutdownStarted || isRunning() || program.isEmpty()) {
    return false;
  }

  mOutputBuffer.clear();
  mStopRequested = false;
  mActiveTask = taskName;
  prepareProcessJob();
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
  if (mShutdownStarted || !isRunning()) {
    return;
  }

  mStopRequested = true;
  const qint64 processId = mProcess.processId();
  if (mAcceptsCancelCommand) {
    emit outputReady(tr("Requesting graceful task cancellation...\n"));
    mProcess.write("cancel\n");
    mProcess.waitForBytesWritten(500);
  } else {
    emit outputReady(tr("Terminating the task process tree...\n"));
#ifdef Q_OS_WIN
    terminateAndReleaseProcessJob();
    if (mProcess.state() != QProcess::NotRunning) {
      mProcess.waitForFinished(1000);
    }
    if (isRunning()) {
      QProcess::execute(QStringLiteral("taskkill.exe"),
                        {QStringLiteral("/PID"), QString::number(processId),
                         QStringLiteral("/T"), QStringLiteral("/F")});
    }
#else
    mProcess.terminate();
#endif
    if (isRunning() && mProcess.processId() == processId) {
      mProcess.kill();
    }
    return;
  }

  QTimer::singleShot(6500, this, [this, processId]() {
    if (!isRunning() || mProcess.processId() != processId) {
      return;
    }
    emit outputReady(tr("Task did not stop in time; terminating its process tree.\n"));
#ifdef Q_OS_WIN
    terminateAndReleaseProcessJob();
    if (mProcess.state() != QProcess::NotRunning) {
      mProcess.waitForFinished(1000);
    }
    if (isRunning()) {
      QProcess::execute(QStringLiteral("taskkill.exe"),
                        {QStringLiteral("/PID"), QString::number(processId),
                         QStringLiteral("/T"), QStringLiteral("/F")});
    }
#endif
    if (isRunning() && mProcess.processId() == processId) {
      mProcess.kill();
    }
  });
}

void ProcessSupervisor::shutdown() {
  if (mShutdownStarted) {
    return;
  }
  mShutdownStarted = true;
  mStopRequested = mStopRequested || isRunning();

  if (!isRunning()) {
    terminateAndReleaseProcessJob();
    return;
  }

  const qint64 processId = mProcess.processId();
  disconnect(&mProcess, nullptr, this, nullptr);
  if (mAcceptsCancelCommand) {
    mProcess.write("cancel\n");
    mProcess.waitForBytesWritten(500);
  }

#ifdef Q_OS_WIN
  terminateAndReleaseProcessJob();
  if (mProcess.state() != QProcess::NotRunning) {
    mProcess.waitForFinished(1000);
  }
  if (mProcess.state() != QProcess::NotRunning && processId > 0) {
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(processId),
                       QStringLiteral("/T"), QStringLiteral("/F")});
  }
#else
  mProcess.terminate();
#endif

  if (mProcess.state() != QProcess::NotRunning &&
      !mProcess.waitForFinished(3000)) {
    mProcess.kill();
    mProcess.waitForFinished(2000);
  }
  mActiveTask.clear();
  mAcceptsCancelCommand = false;
  mOutputBuffer.clear();
}

void ProcessSupervisor::prepareProcessJob() {
#ifdef Q_OS_WIN
  terminateAndReleaseProcessJob();
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    CloseHandle(job);
    return;
  }
  mProcessJobHandle = reinterpret_cast<quintptr>(job);
#endif
}

bool ProcessSupervisor::attachProcessToJob() {
#ifndef Q_OS_WIN
  return true;
#else
  if (mProcessJobHandle == 0 || mProcess.processId() <= 0) {
    return false;
  }

  HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(mProcess.processId()));
  if (process == nullptr) {
    terminateAndReleaseProcessJob();
    return false;
  }
  const BOOL assigned = AssignProcessToJobObject(
      reinterpret_cast<HANDLE>(mProcessJobHandle), process);
  CloseHandle(process);
  if (!assigned) {
    terminateAndReleaseProcessJob();
    return false;
  }
  return true;
#endif
}

void ProcessSupervisor::terminateAndReleaseProcessJob() {
#ifdef Q_OS_WIN
  if (mProcessJobHandle == 0) {
    return;
  }
  HANDLE job = reinterpret_cast<HANDLE>(mProcessJobHandle);
  mProcessJobHandle = 0;
  TerminateJobObject(job, ERROR_CANCELLED);
  WaitForSingleObject(job, 3000);
  CloseHandle(job);
#endif
}

void ProcessSupervisor::drainOutput() {
  const QByteArray bytes = mProcess.readAllStandardOutput();
  if (bytes.isEmpty()) {
    return;
  }

  mOutputBuffer.append(bytes);
  processBufferedOutput(false);
}

void ProcessSupervisor::processBufferedOutput(const bool flushTail) {
  qsizetype newlineIndex = mOutputBuffer.indexOf('\n');
  while (newlineIndex >= 0) {
    const qsizetype lineLength = newlineIndex + 1;
    const QByteArray line = mOutputBuffer.first(lineLength);
    mOutputBuffer.remove(0, lineLength);
    handleOutputLine(line);
    newlineIndex = mOutputBuffer.indexOf('\n');
  }

  if (flushTail && !mOutputBuffer.isEmpty()) {
    const QByteArray tail = std::exchange(mOutputBuffer, {});
    handleOutputLine(tail);
  }
}

void ProcessSupervisor::handleOutputLine(const QByteArray &line) {
  QByteArray content = line;
  if (content.endsWith('\n')) {
    content.chop(1);
  }
  if (content.endsWith('\r')) {
    content.chop(1);
  }

  if (content.startsWith(kWorkerEventPrefix)) {
    WorkerStatus status;
    const QByteArray payload = content.mid(kWorkerEventPrefix.size());
    if (parseWorkerStatus(payload, &status)) {
      emit workerStatusReady(status);
      return;
    }
  }

  emit outputReady(QString::fromUtf8(line));
}

} // namespace gsw
