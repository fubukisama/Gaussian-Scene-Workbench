#include "ProcessSupervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaMethod>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <memory>

using namespace gsw;

namespace {
QString processOutputFixturePath() {
#ifdef Q_OS_WIN
  const QString helperName = QStringLiteral("gsw_process_output_fixture.exe");
#else
  const QString helperName = QStringLiteral("gsw_process_output_fixture");
#endif
  return QDir(QCoreApplication::applicationDirPath()).filePath(helperName);
}

#ifdef Q_OS_WIN
bool processIsRunning(const qint64 processId) {
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(processId));
  if (process == nullptr) {
    return false;
  }
  DWORD exitCode = 0;
  const bool running = GetExitCodeProcess(process, &exitCode) &&
                       exitCode == STILL_ACTIVE;
  CloseHandle(process);
  return running;
}
#endif
} // namespace

class ProcessSupervisorTests final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void failedLaunchRemainsObservableAsAnAcceptedAttempt();
  void acceptedLaunchPrecedesStartedAndBusyRejectionIsNotReported();
  void pauseIsDistinctFromCancellation();
  void parsesFragmentedWorkerStatusWithoutPollutingLogs();
  void trainingSummaryDoesNotBreakLegacyProgress();
  void stopTerminatesTheEntireProcessTree();
  void gracefulStopCleansChildAfterParentExitsFirst();
  void shutdownTerminatesTheEntireProcessTreeSynchronously();
  void destructionAfterStopDoesNotLeaveChildProcess();
};

void ProcessSupervisorTests::initTestCase() {
  qRegisterMetaType<WorkerStatus>();
}

void ProcessSupervisorTests::failedLaunchRemainsObservableAsAnAcceptedAttempt() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  ProcessSupervisor supervisor;
  QSignalSpy startedSpy(&supervisor, &ProcessSupervisor::taskStarted);
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);
  QSignalSpy outputSpy(&supervisor, &ProcessSupervisor::outputReady);
  const int launchSignalIndex = supervisor.metaObject()->indexOfSignal(
      "taskLaunchRequested(QString,QString)");
  std::unique_ptr<QSignalSpy> launchSpy;
  if (launchSignalIndex >= 0) {
    launchSpy = std::make_unique<QSignalSpy>(
        &supervisor, supervisor.metaObject()->method(launchSignalIndex));
  }
  bool attemptVisibleBeforeCompletion = false;
  connect(&supervisor, &ProcessSupervisor::taskFinished, &supervisor,
      [&launchSpy, &attemptVisibleBeforeCompletion]() {
        attemptVisibleBeforeCompletion = launchSpy && launchSpy->count() == 1;
      });

  const QString task = QStringLiteral("missing-program");
  QVERIFY(supervisor.start(task,
      QDir(temporary.path()).filePath(QStringLiteral("missing-worker.exe")),
      {}, temporary.path()));
  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
  QCOMPARE(startedSpy.count(), 0);
  QCOMPARE(finishedSpy.at(0).at(0).toString(), task);
  QCOMPARE(finishedSpy.at(0).at(1).toInt(), -1);
  QVERIFY(!finishedSpy.at(0).at(2).toBool());
  QVERIFY(!outputSpy.isEmpty());
  QVERIFY2(attemptVisibleBeforeCompletion,
      "An accepted task must be observable before an OS launch failure completes it.");
  QCOMPARE(launchSpy->count(), 1);
  QCOMPARE(launchSpy->at(0).at(0).toString(), task);
  QCOMPARE(launchSpy->at(0).at(1).toString(), temporary.path());
  QVERIFY(!supervisor.isRunning());
  QVERIFY(supervisor.activeTask().isEmpty());
}

void ProcessSupervisorTests::acceptedLaunchPrecedesStartedAndBusyRejectionIsNotReported() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  ProcessSupervisor supervisor;
  QSignalSpy launchSpy(&supervisor, &ProcessSupervisor::taskLaunchRequested);
  QSignalSpy startedSpy(&supervisor, &ProcessSupervisor::taskStarted);
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);
  QStringList lifecycle;
  connect(&supervisor, &ProcessSupervisor::taskLaunchRequested, &supervisor,
      [&lifecycle]() { lifecycle.append(QStringLiteral("requested")); });
  connect(&supervisor, &ProcessSupervisor::taskStarted, &supervisor,
      [&lifecycle]() { lifecycle.append(QStringLiteral("started")); });
  connect(&supervisor, &ProcessSupervisor::taskFinished, &supervisor,
      [&lifecycle]() { lifecycle.append(QStringLiteral("finished")); });

  const QString task = QStringLiteral("accepted-worker");
  QVERIFY(supervisor.start(task, processOutputFixturePath(),
      {QStringLiteral("pause-worker")}, temporary.path(), {}, true));
  QCOMPARE(launchSpy.count(), 1);
  QCOMPARE(launchSpy.at(0).at(0).toString(), task);
  QCOMPARE(launchSpy.at(0).at(1).toString(), temporary.path());
  QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 5000);
  QCOMPARE(startedSpy.at(0).at(0).toString(), task);
  QCOMPARE(lifecycle, QStringList({QStringLiteral("requested"),
                                  QStringLiteral("started")}));

  QVERIFY(!supervisor.start(QStringLiteral("rejected-while-busy"),
      processOutputFixturePath(), {QStringLiteral("summary-worker")},
      temporary.path()));
  QCOMPARE(launchSpy.count(), 1);
  QCOMPARE(supervisor.activeTask(), task);
  QVERIFY(supervisor.requestPause());
  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
  QCOMPARE(lifecycle, QStringList({QStringLiteral("requested"),
                                  QStringLiteral("started"),
                                  QStringLiteral("finished")}));
  QCOMPARE(launchSpy.count(), 1);
  QCOMPARE(startedSpy.count(), 1);
  QCOMPARE(finishedSpy.at(0).at(0).toString(), task);
}

void ProcessSupervisorTests::trainingSummaryDoesNotBreakLegacyProgress() {
  ProcessSupervisor supervisor;
  QSignalSpy statusSpy(&supervisor, &ProcessSupervisor::workerStatusReady);
  QSignalSpy outputSpy(&supervisor, &ProcessSupervisor::outputReady);
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);
  QVERIFY(supervisor.start(QStringLiteral("summary"), processOutputFixturePath(),
                          {QStringLiteral("summary-worker")}));
  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
  QCOMPARE(statusSpy.size(), 7);
  QCOMPARE(outputSpy.size(), 0);
  for (const auto &arguments : statusSpy)
    QCOMPARE(qvariant_cast<WorkerStatus>(arguments.at(0)).iteration.value(), 3);
  const auto summary = qvariant_cast<WorkerStatus>(statusSpy.at(0).at(0)).trainingSummary;
  QCOMPARE(summary.value(QStringLiteral("trainPixels")).toInteger(), qint64(1928) * 1084 * 2);
  QCOMPARE(summary.value(QStringLiteral("densifyUntil")).toInt(), 0);
  for (int i = 1; i <= 5; ++i)
    QVERIFY(qvariant_cast<WorkerStatus>(statusSpy.at(i).at(0)).trainingSummary.isEmpty());
  const auto surfel = qvariant_cast<WorkerStatus>(statusSpy.at(6).at(0)).trainingSummary;
  QCOMPARE(surfel.value(QStringLiteral("backend")).toString(), QStringLiteral("2dgs"));
  QCOMPARE(surfel.value(QStringLiteral("resolution")).toInt(), 16);
}

void ProcessSupervisorTests::pauseIsDistinctFromCancellation() {
  ProcessSupervisor supervisor;
  QVERIFY(!supervisor.requestPause());
  QSignalSpy statusSpy(&supervisor, &ProcessSupervisor::workerStatusReady);
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);
  QVERIFY(supervisor.start(QStringLiteral("pause-fixture"), processOutputFixturePath(),
      {QStringLiteral("pause-worker")}, {}, {}, true));
  QVERIFY(supervisor.requestPause());
  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
  QCOMPARE(finishedSpy.at(0).at(1).toInt(), 75);
  QCOMPARE(statusSpy.count(), 1);
  QCOMPARE(qvariant_cast<WorkerStatus>(statusSpy.at(0).at(0)).state, QStringLiteral("paused"));
  QVERIFY(!supervisor.wasStopRequested());
  QVERIFY(!supervisor.requestPause());
}

void ProcessSupervisorTests::parsesFragmentedWorkerStatusWithoutPollutingLogs() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString readyPath = QDir(temporary.path()).filePath(QStringLiteral("ready"));
  const QString releasePath = QDir(temporary.path()).filePath(QStringLiteral("release"));
  const QString helperPath = processOutputFixturePath();
  QVERIFY2(QFileInfo::exists(helperPath), qPrintable(helperPath));

  ProcessSupervisor supervisor;
  QSignalSpy statusSpy(&supervisor, &ProcessSupervisor::workerStatusReady);
  QSignalSpy gpuPreviewSpy(
      &supervisor, &ProcessSupervisor::trainingGpuPreviewReady);
  QSignalSpy outputSpy(&supervisor, &ProcessSupervisor::outputReady);
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);

  QVERIFY(supervisor.start(QStringLiteral("fixture"), helperPath,
                           {readyPath, releasePath}, temporary.path()));
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(readyPath), 5000);
  QCOMPARE(statusSpy.count(), 0);
  QCOMPARE(outputSpy.count(), 0);

  QFile release(releasePath);
  QVERIFY(release.open(QIODevice::WriteOnly));
  QCOMPARE(release.write("release"), qint64(7));
  release.close();

  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
  QCOMPARE(statusSpy.count(), 1);
  QCOMPARE(gpuPreviewSpy.count(), 1);
  const WorkerStatus status = qvariant_cast<WorkerStatus>(statusSpy.takeFirst().at(0));
  QCOMPARE(status.state, QStringLiteral("running"));
  QCOMPARE(status.stage, QStringLiteral("train"));
  QVERIFY(status.progressPercent.has_value());
  QCOMPARE(status.progressPercent.value(), 37);
  QCOMPARE(status.iteration.value(), 11100);
  QCOMPARE(status.totalIterations.value(), 30000);
  QVERIFY(status.loss.has_value());
  QCOMPARE(status.loss.value(), 0.0234);
  QVERIFY(status.psnr.has_value());
  QCOMPARE(status.psnr.value(), 27.5);
  QCOMPARE(status.gaussianCount.value(), qint64(123456));
  QCOMPARE(status.iterationMilliseconds.value(), 12.5);
  QCOMPARE(status.elapsedSeconds.value(), 144.0);
  QCOMPARE(status.previewIteration.value(), 10000);
  QCOMPARE(status.previewPath, QStringLiteral("E:/model/point_cloud.ply"));
  QCOMPARE(status.previewKind, QStringLiteral("colmap_sparse"));
  QCOMPARE(status.densityGuardIteration.value(), 3100);
  QCOMPARE(status.densityGuardDeferred.value(), 3300);
  const TrainingGpuPreviewDescriptor gpuPreview =
      qvariant_cast<TrainingGpuPreviewDescriptor>(
          gpuPreviewSpy.takeFirst().at(0));
  QCOMPARE(gpuPreview.state, TrainingGpuPreviewState::Ready);
  QCOMPARE(gpuPreview.producerPid, quint32(4242));
  QCOMPARE(gpuPreview.memoryHandle, quint64(0x41c));
  QCOMPARE(gpuPreview.memoryHandleType,
           TrainingGpuPreviewHandleType::OpaqueWin32Kmt);
  QCOMPARE(gpuPreview.capacity, quint64(1000));

  QString output;
  for (const QList<QVariant> &arguments : outputSpy) {
    output += arguments.at(0).toString();
  }
  QCOMPARE(output, QStringLiteral("plain log\n"));
}

void ProcessSupervisorTests::stopTerminatesTheEntireProcessTree() {
#ifndef Q_OS_WIN
  QSKIP("Process-tree termination currently targets the native Windows application.");
#else
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString childPidPath =
      QDir(temporary.path()).filePath(QStringLiteral("child-pid"));
  const QString helperPath = processOutputFixturePath();
  QVERIFY2(QFileInfo::exists(helperPath), qPrintable(helperPath));

  ProcessSupervisor supervisor;
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);
  QVERIFY(supervisor.start(QStringLiteral("tree-fixture"), helperPath,
                           {QStringLiteral("tree-parent"), childPidPath},
                           temporary.path()));
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(childPidPath), 5000);
  QFile childPidFile(childPidPath);
  QVERIFY(childPidFile.open(QIODevice::ReadOnly));
  bool validPid = false;
  const qint64 childPid = childPidFile.readAll().trimmed().toLongLong(&validPid);
  QVERIFY(validPid);
  QVERIFY(processIsRunning(childPid));

  supervisor.stop();
  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1500);
  QVERIFY(supervisor.wasStopRequested());
  QTest::qWait(250);
  const bool childStillRunning = processIsRunning(childPid);
  if (childStillRunning) {
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(childPid),
                       QStringLiteral("/T"), QStringLiteral("/F")});
  }
  QVERIFY2(!childStillRunning,
           "Stopping a task must terminate every process in its tree.");
#endif
}

void ProcessSupervisorTests::gracefulStopCleansChildAfterParentExitsFirst() {
#ifndef Q_OS_WIN
  QSKIP("Process-tree termination currently targets the native Windows application.");
#else
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString childPidPath =
      QDir(temporary.path()).filePath(QStringLiteral("orphan-child-pid"));
  const QString helperPath = processOutputFixturePath();
  QVERIFY2(QFileInfo::exists(helperPath), qPrintable(helperPath));

  ProcessSupervisor supervisor;
  QSignalSpy finishedSpy(&supervisor, &ProcessSupervisor::taskFinished);
  QVERIFY(supervisor.start(QStringLiteral("graceful-orphan-fixture"), helperPath,
                           {QStringLiteral("tree-parent-exits"), childPidPath},
                           temporary.path(), {}, true));
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(childPidPath), 5000);
  QFile childPidFile(childPidPath);
  QVERIFY(childPidFile.open(QIODevice::ReadOnly));
  bool validPid = false;
  const qint64 childPid =
      childPidFile.readAll().trimmed().toLongLong(&validPid);
  QVERIFY(validPid);
  QVERIFY(processIsRunning(childPid));

  supervisor.stop();
  QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1500);
  QVERIFY(!supervisor.isRunning());
  supervisor.shutdown();
  const bool childStillRunning = processIsRunning(childPid);
  if (childStillRunning) {
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(childPid),
                       QStringLiteral("/T"), QStringLiteral("/F")});
  }
  QVERIFY2(!childStillRunning,
           "Graceful shutdown must not orphan a child after its parent exits.");
#endif
}

void ProcessSupervisorTests::shutdownTerminatesTheEntireProcessTreeSynchronously() {
#ifndef Q_OS_WIN
  QSKIP("Process-tree termination currently targets the native Windows application.");
#else
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString childPidPath =
      QDir(temporary.path()).filePath(QStringLiteral("shutdown-child-pid"));
  const QString helperPath = processOutputFixturePath();
  QVERIFY2(QFileInfo::exists(helperPath), qPrintable(helperPath));

  ProcessSupervisor supervisor;
  QVERIFY(supervisor.start(QStringLiteral("shutdown-tree-fixture"), helperPath,
                           {QStringLiteral("tree-parent"), childPidPath},
                           temporary.path(), {}, true));
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(childPidPath), 5000);
  QFile childPidFile(childPidPath);
  QVERIFY(childPidFile.open(QIODevice::ReadOnly));
  bool validPid = false;
  const qint64 childPid =
      childPidFile.readAll().trimmed().toLongLong(&validPid);
  QVERIFY(validPid);
  QVERIFY(processIsRunning(childPid));

  supervisor.shutdown();
  QVERIFY(!supervisor.isRunning());
  const bool childStillRunning = processIsRunning(childPid);
  if (childStillRunning) {
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(childPid),
                       QStringLiteral("/T"), QStringLiteral("/F")});
  }
  QVERIFY2(!childStillRunning,
           "Shutdown must terminate every process before returning.");

  supervisor.shutdown();
  QVERIFY(!supervisor.isRunning());
#endif
}

void ProcessSupervisorTests::destructionAfterStopDoesNotLeaveChildProcess() {
#ifndef Q_OS_WIN
  QSKIP("Process-tree termination currently targets the native Windows application.");
#else
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString childPidPath =
      QDir(temporary.path()).filePath(QStringLiteral("destructor-child-pid"));
  const QString helperPath = processOutputFixturePath();

  std::unique_ptr<ProcessSupervisor> supervisor =
      std::make_unique<ProcessSupervisor>();
  QVERIFY(supervisor->start(QStringLiteral("destructor-tree-fixture"), helperPath,
                            {QStringLiteral("tree-parent"), childPidPath},
                            temporary.path()));
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(childPidPath), 5000);
  QFile childPidFile(childPidPath);
  QVERIFY(childPidFile.open(QIODevice::ReadOnly));
  bool validPid = false;
  const qint64 childPid = childPidFile.readAll().trimmed().toLongLong(&validPid);
  QVERIFY(validPid);
  QVERIFY(processIsRunning(childPid));

  supervisor->stop();
  supervisor.reset();
  QTest::qWait(250);
  const bool childStillRunning = processIsRunning(childPid);
  if (childStillRunning) {
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(childPid),
                       QStringLiteral("/T"), QStringLiteral("/F")});
  }
  QVERIFY2(!childStillRunning,
           "Destroying the supervisor after stop must not orphan child processes.");
#endif
}

QTEST_GUILESS_MAIN(ProcessSupervisorTests)

#include "ProcessSupervisorTests.moc"
