#include <QCoreApplication>
#include "ProcessSupervisor.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QTimer>

#include <cmath>
#include <limits>
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
const QByteArray kTrainingGpuPreviewPrefix =
    QByteArrayLiteral("[gsw-training-gpu-preview] ");

QByteArray jsonObjectPrefix(const QByteArray &payload) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error == QJsonParseError::NoError && document.isObject()) {
    return payload;
  }
  if (parseError.error != QJsonParseError::GarbageAtEnd ||
      parseError.offset <= 0 || parseError.offset > payload.size()) {
    return {};
  }
  const QByteArray prefix = payload.first(parseError.offset).trimmed();
  parseError = {};
  const QJsonDocument prefixDocument =
      QJsonDocument::fromJson(prefix, &parseError);
  return parseError.error == QJsonParseError::NoError &&
                 prefixDocument.isObject()
             ? prefix
             : QByteArray();
}

bool parseOptionalInteger(const QJsonObject &object, const QString &name,
                          std::optional<int> *target, const int minimum = 0) {
  const QJsonValue value = object.value(name);
  if (value.isUndefined()) {
    return true;
  }
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  *target = static_cast<int>(number);
  return true;
}

bool parseOptionalReal(const QJsonObject &object, const QString &name,
                       std::optional<double> *target,
                       const double minimum = 0.0) {
  const QJsonValue value = object.value(name);
  if (value.isUndefined()) {
    return true;
  }
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < minimum) {
    return false;
  }
  *target = number;
  return true;
}

bool parseWorkerStatus(const QByteArray &payload, WorkerStatus *status) {
  const QByteArray jsonPayload = jsonObjectPrefix(payload);
  if (jsonPayload.isEmpty()) {
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(jsonPayload, &parseError);
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

  if (!parseOptionalInteger(object, QStringLiteral("iteration"),
                            &parsedStatus.iteration) ||
      !parseOptionalInteger(object, QStringLiteral("totalIterations"),
                            &parsedStatus.totalIterations, 1) ||
      !parseOptionalReal(object, QStringLiteral("loss"), &parsedStatus.loss) ||
      !parseOptionalReal(object, QStringLiteral("psnr"), &parsedStatus.psnr) ||
      !parseOptionalReal(object, QStringLiteral("iterationMilliseconds"),
                         &parsedStatus.iterationMilliseconds) ||
      !parseOptionalReal(object, QStringLiteral("elapsedSeconds"),
                         &parsedStatus.elapsedSeconds) ||
      !parseOptionalInteger(object, QStringLiteral("previewIteration"),
                            &parsedStatus.previewIteration) ||
      !parseOptionalInteger(object, QStringLiteral("densityGuardIteration"),
                            &parsedStatus.densityGuardIteration, 1) ||
      !parseOptionalInteger(object, QStringLiteral("densityGuardDeferred"),
                            &parsedStatus.densityGuardDeferred, 1) ||
      !parseOptionalInteger(object, QStringLiteral("reconstructionViews"), &parsedStatus.reconstructionViews) ||
      !parseOptionalInteger(object, QStringLiteral("reconstructionInputs"), &parsedStatus.reconstructionInputs) ||
      !parseOptionalInteger(object, QStringLiteral("reconstructionPoints"), &parsedStatus.reconstructionPoints)) {
    return false;
  }
  const QJsonValue gaussianCount =
      object.value(QStringLiteral("gaussianCount"));
  if (!gaussianCount.isUndefined()) {
    if (!gaussianCount.isDouble()) {
      return false;
    }
    const double count = gaussianCount.toDouble();
    if (!std::isfinite(count) || std::floor(count) != count || count < 0.0 ||
        count > static_cast<double>(std::numeric_limits<qint64>::max())) {
      return false;
    }
    parsedStatus.gaussianCount = static_cast<qint64>(count);
  }
  const QJsonValue previewPath = object.value(QStringLiteral("previewPath"));
  if (!previewPath.isUndefined()) {
    if (!previewPath.isString()) {
      return false;
    }
    parsedStatus.previewPath = previewPath.toString();
  }
  const QJsonValue previewKind = object.value(QStringLiteral("previewKind"));
  if (!previewKind.isUndefined()) {
    if (!previewKind.isString()) {
      return false;
    }
    parsedStatus.previewKind = previewKind.toString();
  }
  for (const QString &key : {QStringLiteral("reconstructionQuality"), QStringLiteral("generationIssue")}) {
    const auto value = object.value(key);
    if (value.isUndefined()) continue;
    const QStringList allowed = key == QStringLiteral("reconstructionQuality")
        ? QStringList{QStringLiteral("accepted"), QStringLiteral("partial"), QStringLiteral("repairing"), QStringLiteral("rejected")}
        : QStringList{QStringLiteral("reconstruction_quality"), QStringLiteral("source_frames_missing")};
    if (!value.isString() || !allowed.contains(value.toString())) return false;
    if (key == QStringLiteral("reconstructionQuality")) parsedStatus.reconstructionQuality = value.toString();
    else parsedStatus.generationIssue = value.toString();
  }

  // Summary metadata is optional for old workers. Reject malformed metadata
  // independently: a bad caption must never discard valid progress samples.
  const QJsonObject summary = object.value(QStringLiteral("trainingSummary")).toObject();
  const auto positiveInteger = [](const QJsonValue &value, double maximum) {
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number) && number >= 1.0 &&
           number <= maximum && std::floor(number) == number;
  };
  const auto boundedReal = [](const QJsonValue &value, double maximum) {
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number) && number >= 0.0 && number <= maximum;
  };
  const QString backend = summary.value(QStringLiteral("backend")).toString();
  const QString phase = summary.value(QStringLiteral("phase")).toString();
  const QString optimizer = summary.value(QStringLiteral("optimizer")).toString();
  const int resolution = summary.value(QStringLiteral("resolution")).toInt();
  bool summaryValid = summary.value(QStringLiteral("version")).toDouble(-1) == 1.0 &&
      (backend == QStringLiteral("3dgs") || backend == QStringLiteral("2dgs")) &&
      (phase == QStringLiteral("configured") || phase == QStringLiteral("loaded")) &&
      (optimizer == QStringLiteral("default") || optimizer == QStringLiteral("adam") ||
       optimizer == QStringLiteral("sparse_adam")) &&
      positiveInteger(summary.value(QStringLiteral("iterations")), 2147483647.0) &&
      positiveInteger(summary.value(QStringLiteral("resolution")), 16.0) && resolution >= 1 &&
      boundedReal(summary.value(QStringLiteral("densifyUntil")), 2147483647.0) &&
      std::floor(summary.value(QStringLiteral("densifyUntil")).toDouble()) == summary.value(QStringLiteral("densifyUntil")).toDouble() &&
      positiveInteger(summary.value(QStringLiteral("densificationInterval")), 2147483647.0) &&
      boundedReal(summary.value(QStringLiteral("densifyGradient")), 1.0);
  if (backend == QStringLiteral("3dgs")) {
    summaryValid = summaryValid && summary.value(QStringLiteral("antialiasing")).isBool() &&
        summary.value(QStringLiteral("exposureCompensation")).isBool();
  } else {
    summaryValid = summaryValid && boundedReal(summary.value(QStringLiteral("depthRatio")), 1.0);
  }
  if (phase == QStringLiteral("loaded")) {
    const QJsonArray dimensions = summary.value(QStringLiteral("trainDimensions")).toArray();
    summaryValid = summaryValid && !dimensions.isEmpty() && dimensions.size() <= 8 &&
        positiveInteger(summary.value(QStringLiteral("trainImageCount")), 2147483647.0) &&
        positiveInteger(summary.value(QStringLiteral("trainDimensionKinds")), 2147483647.0) &&
        summary.value(QStringLiteral("trainDimensionKinds")).toInt() >= dimensions.size() &&
        positiveInteger(summary.value(QStringLiteral("trainPixels")), 9007199254740991.0);
    qint64 listedImages = 0;
    qint64 listedPixels = 0;
    QPair<int, int> previousSize;
    for (const QJsonValue &value : dimensions) {
      const QJsonArray size = value.toArray();
      if (size.size() != 3 || !positiveInteger(size.at(0), 1000000.0) ||
          !positiveInteger(size.at(1), 1000000.0) || !positiveInteger(size.at(2), 2147483647.0)) {
        summaryValid = false;
        break;
      }
      const QPair<int, int> currentSize{size.at(0).toInt(), size.at(1).toInt()};
      const qint64 count = size.at(2).toInt();
      const qint64 pixels = qint64(currentSize.first) * currentSize.second;
      if (currentSize <= previousSize || pixels > 9007199254740991LL / count ||
          listedPixels > 9007199254740991LL - pixels * count) {
        summaryValid = false;
        break;
      }
      previousSize = currentSize;
      listedImages += count;
      listedPixels += pixels * count;
    }
    const qint64 totalImages = summary.value(QStringLiteral("trainImageCount")).toInteger();
    const qint64 totalPixels = summary.value(QStringLiteral("trainPixels")).toInteger();
    summaryValid = summaryValid && listedImages <= totalImages && listedPixels <= totalPixels;
    if (summary.value(QStringLiteral("trainDimensionKinds")).toInt() == dimensions.size())
      summaryValid = summaryValid && listedImages == totalImages && listedPixels == totalPixels;
  }
  if (summaryValid) parsedStatus.trainingSummary = summary;

  *status = std::move(parsedStatus);
  return true;
}

} // namespace

ProcessSupervisor::ProcessSupervisor(QObject *parent) : QObject(parent) {
  qRegisterMetaType<TrainingGpuPreviewDescriptor>();
  mProcess.setProcessChannelMode(QProcess::MergedChannels);

  connect(&mProcess, &QProcess::readyReadStandardOutput, this, &ProcessSupervisor::drainOutput);
  connect(&mProcess, &QProcess::started, this, [this]() {
    if (!attachProcessToJob()) {
      emit outputReady(
          QCoreApplication::translate("Workbench", "Warning: the task could not be attached to the shutdown job; "
             "process-tree cleanup will use the PID fallback.\n"));
    }
    emit taskStarted(mActiveTask);
    emit runningChanged(true);
  });
  connect(&mProcess, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      emit outputReady(QCoreApplication::translate("Workbench", "Failed to start process: %1\n").arg(mProcess.errorString()));
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

bool ProcessSupervisor::requestPause() {
  return isRunning() && mAcceptsCancelCommand && !mStopRequested &&
         mProcess.write("pause\n") == 6;
}

void ProcessSupervisor::stop() {
  if (mShutdownStarted || !isRunning()) {
    return;
  }

  mStopRequested = true;
  const qint64 processId = mProcess.processId();
  if (mAcceptsCancelCommand) {
    emit outputReady(QCoreApplication::translate("Workbench", "Requesting graceful task cancellation...\n"));
    mProcess.write("cancel\n");
    mProcess.waitForBytesWritten(500);
  } else {
    emit outputReady(QCoreApplication::translate("Workbench", "Terminating the task process tree...\n"));
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
    emit outputReady(QCoreApplication::translate("Workbench", "Task did not stop in time; terminating its process tree.\n"));
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

  if (content.startsWith(kTrainingGpuPreviewPrefix)) {
    TrainingGpuPreviewDescriptor descriptor;
    const QByteArray payload = jsonObjectPrefix(
        content.mid(kTrainingGpuPreviewPrefix.size()));
    if (parseTrainingGpuPreviewDescriptor(payload, &descriptor)) {
      emit trainingGpuPreviewReady(descriptor);
      return;
    }
  }

  emit outputReady(QString::fromUtf8(line));
}

} // namespace gsw
