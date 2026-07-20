#include "ProcessSupervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
  void parsesFragmentedWorkerStatusWithoutPollutingLogs();
  void stopTerminatesTheEntireProcessTree();
  void gracefulStopCleansChildAfterParentExitsFirst();
  void shutdownTerminatesTheEntireProcessTreeSynchronously();
  void destructionAfterStopDoesNotLeaveChildProcess();
};

void ProcessSupervisorTests::initTestCase() {
  qRegisterMetaType<WorkerStatus>();
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
  const WorkerStatus status = qvariant_cast<WorkerStatus>(statusSpy.takeFirst().at(0));
  QCOMPARE(status.state, QStringLiteral("running"));
  QCOMPARE(status.stage, QStringLiteral("train"));
  QVERIFY(status.progressPercent.has_value());
  QCOMPARE(status.progressPercent.value(), 37);

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
