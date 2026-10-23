#include "ProcessSupervisor.h"
#include "TrainingOutputLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QSysInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QMetaMethod>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <memory>
#include <cstdio>

using namespace gsw;

namespace {
bool writeJsonFixture(const QString &path, const QJsonObject &value) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) &&
      file.write(QJsonDocument(value).toJson(QJsonDocument::Compact)) > 0 && file.commit();
}

int persistentFixture(QCoreApplication &application) {
#ifndef Q_OS_WIN
  return 2;
#else
  const auto args = application.arguments();
  if (args.size() != 5) return 3;
  const QString store = args.at(2), ready = args.at(3), release = args.at(4);
  const QString id(32, QLatin1Char('a')), session(32, QLatin1Char('b'));
  const QString output = QDir(store).filePath(QStringLiteral("owned-output"));
  if (!QDir().mkpath(QDir(store).filePath(QStringLiteral(".c")))) return 4;
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 5;
  const quint64 token = (quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  const QJsonObject identity{{"pid", QCoreApplication::applicationPid()},
      {"startToken", QStringLiteral("win-filetime:") + QString::number(token)}};
  const QJsonObject control{{"session", session}, {"backend", "3dgs"}, {"output", output}};
  const QJsonObject job{{"id", id}, {"backend", "3dgs"}, {"status", "running"},
      {"stage", "train"}, {"native_control", control}};
  if (!writeJsonFixture(QDir(store).filePath(id + QStringLiteral(".json")), job)) return 6;
  const QString leasePath = QDir(store).filePath(QStringLiteral(".c/") + session + QStringLiteral(".json"));
  const auto heartbeat = [&] {
    return writeJsonFixture(leasePath, {{"version", 1}, {"jobId", id},
        {"session", session}, {"backend", "3dgs"}, {"output", output},
        {"workerIdentity", identity}, {"closed", false},
        {"heartbeat", QDateTime::currentMSecsSinceEpoch() / 1000.0}});
  };
  if (!heartbeat()) return 7;
  QFile readyFile(ready);
  if (!readyFile.open(QIODevice::WriteOnly) || readyFile.write(QByteArray::number(QCoreApplication::applicationPid())) <= 0) return 8;
  readyFile.close();
  std::puts("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"running\",\"stage\":\"train\",\"iteration\":3}");
  std::fflush(stdout);
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &application, [&] {
    if (QFileInfo::exists(release)) { application.exit(0); return; }
    heartbeat();
    QFile command(QDir(store).filePath(QStringLiteral(".c/") + session + QStringLiteral("-cmd.json")));
    if (!command.open(QIODevice::ReadOnly)) return;
    const auto request = QJsonDocument::fromJson(command.readAll()).object();
    command.close(); command.remove();
    if (request.value("session") != session || request.value("jobId") != id ||
        request.value("workerIdentity").toObject() != identity) { application.exit(9); return; }
    const bool paused = request.value("command").toString() == QStringLiteral("pause");
    const QByteArray event = QJsonDocument(QJsonObject{{"version", 1}, {"type", "status"},
        {"state", paused ? "paused" : "cancelled"}, {"stage", paused ? "paused" : "cancelled"}}).toJson(QJsonDocument::Compact);
    std::printf("[worker-event] %s\n", event.constData()); std::fflush(stdout);
    application.exit(paused ? 75 : 130);
  });
  timer.start(25);
  QTimer::singleShot(15000, &application, [&] { application.exit(10); });
  return application.exec();
#endif
}

int persistentBatchFixture(QCoreApplication &application) {
#ifndef Q_OS_WIN
  return 2;
#else
  const auto args=application.arguments();
  if(args.size()!=5) return 3;
  const bool leasePublicationOpen=args.at(1)==QStringLiteral("--persistent-batch-publication-fixture");
  const QString plan=args.at(2),ready=args.at(3),release=args.at(4);
  QFile planFile(plan);
  if(!planFile.open(QIODevice::ReadOnly)) return 4;
  const auto bytes=planFile.readAll();
  const auto document=QJsonDocument::fromJson(bytes).object();
  const QString root=QFileInfo(plan).absolutePath(),session(32,QLatin1Char('d'));
  const auto fileHash=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
  FILETIME creation{},exit{},kernel{},user{};
  if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)) return 5;
  const auto token=(quint64(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
  const QJsonObject identity{{"pid",QCoreApplication::applicationPid()},
    {"startToken",QStringLiteral("win-filetime:")+QString::number(token)}};
  const QString inbox=QDir(root).filePath(QStringLiteral(".coordinators/")+session+QStringLiteral("-cmd.json"));
  if(!QDir().mkpath(QFileInfo(inbox).absolutePath())) return 6;
  QJsonObject lease{{"schema","gsw.training_batch_connection/1"},{"version",1},
    {"batchId",QFileInfo(root).fileName()},{"planPath",QDir::toNativeSeparators(plan)},
    {"planSha256",document.value("plan_sha256")},{"planFileSha256",fileHash},
    {"session",session},{"host",QSysInfo::machineHostName()},
    {"coordinatorIdentity",identity},{"closed",false},{"outcome","running"}};
  const auto heartbeat=[&]{lease.insert("heartbeat",QDateTime::currentMSecsSinceEpoch()/1000.0);
    return writeJsonFixture(QDir(root).filePath(QStringLiteral(".coordinator.json")),lease);};
  if(!heartbeat()) return 7;
  QFile readyFile(ready);if(!readyFile.open(QIODevice::WriteOnly)) return 8;
  readyFile.write(QByteArray::number(QCoreApplication::applicationPid()));readyFile.close();
  std::puts("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"running\",\"stage\":\"batch_run\",\"progressPercent\":25}");std::fflush(stdout);
  QTimer timer;
  QObject::connect(&timer,&QTimer::timeout,&application,[&]{
    if(QFileInfo::exists(release)){application.exit(0);return;}
    if(!leasePublicationOpen)heartbeat();
    QFile file(inbox);if(!file.open(QIODevice::ReadOnly))return;
    const auto command=QJsonDocument::fromJson(file.readAll()).object();file.close();file.remove();
    if(command.value("schema")!="gsw.training_batch_connection/1"||command.value("session")!=session||
      command.value("coordinatorIdentity").toObject()!=identity||command.value("planSha256")!=lease.value("planSha256")||
      command.value("planFileSha256")!=fileHash){application.exit(9);return;}
    const bool pause=command.value("command")=="pause";
    if(!pause&&command.value("command")!="cancel"){application.exit(9);return;}
    lease.insert("closed",true);lease.insert("outcome",pause?"paused":"cancelled");heartbeat();
    std::puts("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"paused\",\"stage\":\"batch_paused\"}");std::fflush(stdout);
    application.exit(pause?75:130);
  });timer.start(25);QTimer::singleShot(15000,&application,[&]{application.exit(10);});return application.exec();
#endif
}

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
  void lifetimeFixtureWaitsForOwnedStdinRelease_data();
  void lifetimeFixtureWaitsForOwnedStdinRelease();
  void failedLaunchRemainsObservableAsAnAcceptedAttempt();
  void failedLaunchRestoresThePreviousTrainingResumeRecord_data();
  void failedLaunchRestoresThePreviousTrainingResumeRecord();
  void acceptedLaunchPrecedesStartedAndBusyRejectionIsNotReported();
  void pauseIsDistinctFromCancellation();
  void parsesFragmentedWorkerStatusWithoutPollutingLogs();
  void trainingSummaryDoesNotBreakLegacyProgress();
  void stopTerminatesTheEntireProcessTree();
  void gracefulStopCleansChildAfterParentExitsFirst();
  void shutdownTerminatesTheEntireProcessTreeSynchronously();
  void destructionAfterStopDoesNotLeaveChildProcess();
  void persistentObserverDestructionLeavesWorkerAlive();
  void persistentExplicitControlsAreSessionBound_data();
  void persistentExplicitControlsAreSessionBound();
  void persistentUnavailableControlsDoNotPretendToStop();
  void decodesIndependentRuntimeAndTypedFailureWithoutDiscardingProgress();
  void growthBypassIsExplicitlyUnmeasured();
  void migrationProtectsUnverifiedActiveRecords();
  void migrationProtectsMalformedPersistentCreationTokens();
  void persistentBatchObserverDetachesAndReattaches_data();
  void persistentBatchObserverDetachesAndReattaches();
  void batchMigrationRejectsDamagedLaunchMarker();
  void batchMigrationProtectsOrphanedChildUntilItsCreationIdentityEnds();
};

void ProcessSupervisorTests::batchMigrationProtectsOrphanedChildUntilItsCreationIdentityEnds(){
#ifndef Q_OS_WIN
  QSKIP("Native Windows creation-identity writer gate");
#else
  const auto identity=[](qint64 pid){
    const HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(pid));
    if(!handle)return QJsonObject{};FILETIME creation{},exit{},kernel{},user{};
    const bool read=GetProcessTimes(handle,&creation,&exit,&kernel,&user);CloseHandle(handle);
    return read?QJsonObject{{"pid",pid},{"startToken",QStringLiteral("win-filetime:")+
      QString::number((quint64(creation.dwHighDateTime)<<32)|creation.dwLowDateTime)}}:QJsonObject{};
  };
  QProcess owner,child;owner.start(processOutputFixturePath(),{"pause-worker"});QVERIFY(owner.waitForStarted());
  const auto ownerIdentity=identity(owner.processId());QVERIFY(!ownerIdentity.isEmpty());
  owner.write("pause\n");QVERIFY(owner.waitForFinished());
  child.start(processOutputFixturePath(),{"pause-worker"});QVERIFY(child.waitForStarted());
  const auto childIdentity=identity(child.processId());QVERIFY(!childIdentity.isEmpty());
  const auto cleanup=qScopeGuard([&]{if(child.state()!=QProcess::NotRunning){child.write("pause\n");child.waitForFinished(5000);}});
  QTemporaryDir temporary;QVERIFY(temporary.isValid());
  const auto root=QDir(temporary.path()).filePath(QStringLiteral(".gsw/training-batches/")+QString(32,QLatin1Char('a')));
  QVERIFY(QDir().mkpath(root));const QString plan=QDir(root).filePath("plan.json");
  QVERIFY(writeJsonFixture(plan,{{"schema","gsw.training_batch/1"},{"plan_sha256",QString(64,QLatin1Char('b'))}}));
  QFile input(plan);QVERIFY(input.open(QIODevice::ReadOnly));
  const auto hash=QString::fromLatin1(QCryptographicHash::hash(input.readAll(),QCryptographicHash::Sha256).toHex());input.close();
  QVERIFY(writeJsonFixture(QDir(root).filePath("state.json"),{{"schema","gsw.training_batch_state/1"},
    {"plan_sha256",QString(64,QLatin1Char('b'))},{"plan_file_sha256",hash},{"status","running"}}));
  const QString leasePath=QDir(root).filePath(".coordinator.json");
  QJsonObject lease{{"schema","gsw.training_batch_connection/1"},{"planSha256",QString(64,QLatin1Char('b'))},
    {"planFileSha256",hash},{"host",QSysInfo::machineHostName()},{"closed",true},{"coordinatorIdentity",ownerIdentity},
    {"activeChild",QJsonObject{{"identity",childIdentity}}}};
  QVERIFY(writeJsonFixture(leasePath,lease));
  QVERIFY(ProcessSupervisor::hasUnfinishedBatches(temporary.path()));QVERIFY(processIsRunning(child.processId()));
  lease.insert("activeChild",QJsonObject{{"identity",QJsonObject{}}});QVERIFY(writeJsonFixture(leasePath,lease));
  QVERIFY(ProcessSupervisor::hasUnfinishedBatches(temporary.path()));QVERIFY(processIsRunning(child.processId()));
  child.write("pause\n");QVERIFY(child.waitForFinished());
  lease.insert("activeChild",QJsonObject{{"identity",childIdentity}});QVERIFY(writeJsonFixture(leasePath,lease));
  QVERIFY(!ProcessSupervisor::hasUnfinishedBatches(temporary.path()));
#endif
}

void ProcessSupervisorTests::batchMigrationRejectsDamagedLaunchMarker(){
  QTemporaryDir temporary;QVERIFY(temporary.isValid());
  const auto root=QDir(temporary.path()).filePath(QStringLiteral(".gsw/training-batches/")+QString(32,QLatin1Char('a')));
  QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral(".p"))));
  QVERIFY(writeJsonFixture(QDir(root).filePath(QStringLiteral("state.json")),{{"status","ready"}}));
  const QString marker=QDir(root).filePath(QStringLiteral(".p/owned.json"));
  QVERIFY(writeJsonFixture(marker,{{"closed",true}}));
  QFile input(marker);QVERIFY(input.open(QIODevice::ReadOnly));const auto original=input.readAll();input.close();
  QVERIFY(ProcessSupervisor::hasUnfinishedBatches(temporary.path()));
  QVERIFY(input.open(QIODevice::ReadOnly));QCOMPARE(input.readAll(),original);
}

void ProcessSupervisorTests::persistentBatchObserverDetachesAndReattaches_data(){
  QTest::addColumn<bool>("cancel");QTest::addColumn<bool>("leasePublicationOpen");
  QTest::addColumn<bool>("rejectClosedLease");
  QTest::newRow("pause")<<false<<false<<false;QTest::newRow("cancel-checkpoint")<<true<<false<<false;
  QTest::newRow("pause-during-lease-publication")<<false<<true<<false;
  QTest::newRow("closed-lease-during-publication")<<false<<true<<true;
}
void ProcessSupervisorTests::persistentBatchObserverDetachesAndReattaches(){
#ifndef Q_OS_WIN
  QSKIP("Persistent Windows coordinator process identity boundary");
#else
  QFETCH(bool,cancel);QFETCH(bool,leasePublicationOpen);QFETCH(bool,rejectClosedLease);
  QTemporaryDir temporary;QVERIFY(temporary.isValid());
  const QString root=QDir(temporary.path()).filePath(QStringLiteral(".gsw/training-batches/")+QString(32,QLatin1Char('a')));
  QVERIFY(QDir().mkpath(root));const QString plan=QDir(root).filePath("plan.json"),log=QDir(root).filePath("stdout.log");
  QVERIFY(writeJsonFixture(plan,{{"schema","gsw.training_batch/1"},{"plan_sha256",QString(64,QLatin1Char('b'))}}));
  QFile planFile(plan);QVERIFY(planFile.open(QIODevice::ReadOnly));
  const auto fileHash=QString::fromLatin1(QCryptographicHash::hash(planFile.readAll(),QCryptographicHash::Sha256).toHex());planFile.close();
  QVERIFY(writeJsonFixture(QDir(root).filePath("state.json"),{{"schema","gsw.training_batch_state/1"},
    {"plan_sha256",QString(64,QLatin1Char('b'))},{"plan_file_sha256",fileHash},{"status","running"}}));
  const QString ready=QDir(root).filePath("ready"),release=QDir(root).filePath("release");
  auto supervisor=std::make_unique<ProcessSupervisor>();
  QVERIFY(supervisor->startPersistentBatch("batch",QCoreApplication::applicationFilePath(),
    {leasePublicationOpen?"--persistent-batch-publication-fixture":"--persistent-batch-fixture",plan,ready,release},temporary.path(),{},log,plan));
  QVERIFY(supervisor->isPersistentTask());QVERIFY(supervisor->isPersistentBatch());QVERIFY(!supervisor->isPersistentTraining());
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready),5000);
  QFile readyFile(ready);QVERIFY(readyFile.open(QIODevice::ReadOnly));const auto pid=readyFile.readAll().toLongLong();readyFile.close();
  const auto releaseOwned=qScopeGuard([&]{QFile file(release);if(file.open(QIODevice::WriteOnly))file.write("release");});
  supervisor->shutdown();supervisor.reset();QVERIFY(processIsRunning(pid));
  QVERIFY(ProcessSupervisor::hasUnfinishedBatches(temporary.path()));
  ProcessSupervisor observer;QSignalSpy finished(&observer,&ProcessSupervisor::taskFinished);
  QSignalSpy output(&observer,&ProcessSupervisor::outputReady);
  QVERIFY(observer.attachPersistentBatch("batch-observe",plan,log));
  // Atomic Windows replacement owns DELETE access to the current lease while
  // still allowing observers to read its complete, unchanged metadata. Keep
  // that real filesystem boundary open across the public pause request.
  const QString leasePath=QDir(root).filePath(QStringLiteral(".coordinator.json"));
  QJsonObject validLease;
  if(rejectClosedLease) {
    QFile lease(leasePath);QVERIFY(lease.open(QIODevice::ReadOnly));
    validLease=QJsonDocument::fromJson(lease.readAll()).object();lease.close();
    QVERIFY(!validLease.isEmpty());auto closed=validLease;closed.insert(QStringLiteral("closed"),true);
    QVERIFY(writeJsonFixture(leasePath,closed));
  }
  HANDLE publication=leasePublicationOpen?
    CreateFileW(reinterpret_cast<LPCWSTR>(leasePath.utf16()),DELETE,
      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,nullptr):INVALID_HANDLE_VALUE;
  const auto closePublication=qScopeGuard([&]{if(publication!=INVALID_HANDLE_VALUE)CloseHandle(publication);});
  if(leasePublicationOpen)QVERIFY(publication!=INVALID_HANDLE_VALUE);
  if(leasePublicationOpen) {
    QFile legacyRead(leasePath);SetLastError(ERROR_SUCCESS);
    const bool opened=legacyRead.open(QIODevice::ReadOnly);const DWORD error=GetLastError();
    qInfo("fixture-publication-read-probe: opened=%d,fileError=%d,winError=%lu",
      opened,static_cast<int>(legacyRead.error()),static_cast<unsigned long>(error));
  }
  if(rejectClosedLease) {
    QVERIFY(!observer.requestPause());QVERIFY(!observer.wasStopRequested());
    QVERIFY(processIsRunning(pid));QCOMPARE(finished.count(),0);
    QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral(".coordinators/")+QString(32,QLatin1Char('d'))+QStringLiteral("-cmd.json"))));
    CloseHandle(publication);publication=INVALID_HANDLE_VALUE;
    QVERIFY(writeJsonFixture(leasePath,validLease));
    publication=CreateFileW(reinterpret_cast<LPCWSTR>(leasePath.utf16()),DELETE,
      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,nullptr);
    QVERIFY(publication!=INVALID_HANDLE_VALUE);
  }
  if(cancel){observer.stop();QVERIFY(observer.wasStopRequested());}
  else {
    const bool requested=observer.requestPause();
    QString diagnostic;
    if(!requested) {
      for(const auto &event:output) diagnostic+=event.first().toString();
      QFile lease(QDir(root).filePath(QStringLiteral(".coordinator.json")));
      SetLastError(ERROR_SUCCESS);const bool opened=lease.open(QIODevice::ReadOnly);
      const DWORD error=GetLastError();
      diagnostic+=QStringLiteral("\nfixtureLeaseOpened=%1,fileError=%2,winError=%3")
        .arg(opened).arg(static_cast<int>(lease.error())).arg(error);
      if(opened)diagnostic+=QStringLiteral("\nfixtureLease=")+QString::fromUtf8(lease.readAll());
    }
    QVERIFY2(requested,qPrintable(diagnostic));
  }
  QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,5000);
  QCOMPARE(finished.first().at(1).toInt(),cancel?130:75);QVERIFY(!processIsRunning(pid));
  QVERIFY(!ProcessSupervisor::hasUnfinishedBatches(temporary.path()));
#endif
}

void ProcessSupervisorTests::initTestCase() {
  qRegisterMetaType<WorkerStatus>();
}

void ProcessSupervisorTests::migrationProtectsUnverifiedActiveRecords() {
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QString store = QDir(temporary.path()).filePath("jobs");
  QVERIFY(!ProcessSupervisor::hasUnfinishedTraining(store));
  QVERIFY(QDir().mkpath(store));
  const QString record = QDir(store).filePath(QString(32, QLatin1Char('a')) + ".json");
  QVERIFY(writeJsonFixture(record, {{"id",QString(32,QLatin1Char('a'))},
      {"backend","3dgs"},{"status","running"}}));
  const QByteArray before = [&] { QFile file(record); file.open(QIODevice::ReadOnly); return file.readAll(); }();
  QVERIFY(ProcessSupervisor::hasUnfinishedTraining(store));
  QFile unchanged(record); QVERIFY(unchanged.open(QIODevice::ReadOnly));
  QCOMPARE(unchanged.readAll(), before); unchanged.close();
  QVERIFY(writeJsonFixture(record, {{"id",QString(32,QLatin1Char('a'))},
      {"backend","3dgs"},{"status","paused"}}));
  QVERIFY(!ProcessSupervisor::hasUnfinishedTraining(store));
}

void ProcessSupervisorTests::migrationProtectsMalformedPersistentCreationTokens() {
#ifndef Q_OS_WIN
  QSKIP("Persistent process identity currently targets native Windows.");
#else
  QTemporaryDir temporary;QVERIFY(temporary.isValid());
  const QString store=QDir(temporary.path()).filePath(QStringLiteral("jobs"));
  QVERIFY(QDir().mkpath(QDir(store).filePath(QStringLiteral(".p"))));
  const QString path=QDir(store).filePath(QStringLiteral(".p/owned-launch.json"));
  for(const auto &token:QStringList{QStringLiteral("unknown"),QStringLiteral("linux:other-platform:10"),
      QStringLiteral("win-filetime:-1"),QStringLiteral("win-filetime:0"),QStringLiteral("win-filetime:01"),
      QStringLiteral("win-filetime:1.5"),QStringLiteral("win-filetime:18446744073709551616")}) {
    const QJsonObject record{{"version",1},{"closed",false},{"workerIdentity",QJsonObject{
        {"pid",QCoreApplication::applicationPid()},{"startToken",token}}}};
    QVERIFY(writeJsonFixture(path,record));
    const QByteArray before=[&] {QFile file(path);file.open(QIODevice::ReadOnly);return file.readAll();}();
    QVERIFY2(ProcessSupervisor::hasUnfinishedTraining(store),qPrintable(token));
    QFile after(path);QVERIFY(after.open(QIODevice::ReadOnly));QCOMPARE(after.readAll(),before);
  }
  FILETIME creation{},exit{},kernel{},user{};
  QVERIFY(GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user));
  const quint64 token=(quint64(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
  QJsonObject identity{{"pid",QCoreApplication::applicationPid()},{"startToken",QStringLiteral("win-filetime:")+QString::number(token)}};
  QVERIFY(writeJsonFixture(path,{{"version",1},{"closed",false},{"workerIdentity",identity}}));
  QVERIFY(ProcessSupervisor::hasUnfinishedTraining(store));
  identity.insert(QStringLiteral("startToken"),QStringLiteral("win-filetime:")+QString::number(token+1));
  QVERIFY(writeJsonFixture(path,{{"version",1},{"closed",false},{"workerIdentity",identity}}));
  QVERIFY(!ProcessSupervisor::hasUnfinishedTraining(store));
  identity.insert(QStringLiteral("unknown"),true);
  QVERIFY(writeJsonFixture(path,{{"version",1},{"closed",false},{"workerIdentity",identity}}));
  QVERIFY(ProcessSupervisor::hasUnfinishedTraining(store));
  QVERIFY(writeJsonFixture(path,{{"version",1},{"closed",true},{"workerIdentity",QJsonObject{}}}));
  QVERIFY(!ProcessSupervisor::hasUnfinishedTraining(store));
#endif
}

void ProcessSupervisorTests::persistentUnavailableControlsDoNotPretendToStop() {
#ifndef Q_OS_WIN
  QSKIP("Persistent process identity currently targets native Windows.");
#else
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QString release=QDir(temporary.path()).filePath("release");
  const auto cleanup=qScopeGuard([&] { QFile file(release); if(file.open(QIODevice::WriteOnly)) file.write("release"); });
  ProcessSupervisor supervisor;
  QSignalSpy statuses(&supervisor,&ProcessSupervisor::workerStatusReady);
  QSignalSpy finished(&supervisor,&ProcessSupervisor::taskFinished);
  QVERIFY(supervisor.startPersistentTraining("unregistered",processOutputFixturePath(),
      {"completion-worker",QDir(temporary.path()).filePath("result.ply"),release,"training_checkpoint"},
      temporary.path(),{},QDir(temporary.path()).filePath("worker.log"),
      QDir(temporary.path()).filePath("jobs")));
  // The detached worker has not registered a job/observer lease yet.
  QVERIFY(ProcessSupervisor::hasUnfinishedTraining(QDir(temporary.path()).filePath("jobs")));
  QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty(),5000);
  QVERIFY(!supervisor.requestPause());
  supervisor.stop();
  QVERIFY(!supervisor.wasStopRequested());
  QVERIFY(supervisor.isRunning());
  QFile file(release); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("release"); file.close();
  QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,5000);
  QVERIFY(!ProcessSupervisor::hasUnfinishedTraining(QDir(temporary.path()).filePath("jobs")));
#endif
}

void ProcessSupervisorTests::decodesIndependentRuntimeAndTypedFailureWithoutDiscardingProgress() {
  const QJsonObject guard{{"enabled",false},{"mode","pause"},{"usage_fraction",.9},
      {"minimum_spare_mib",512},{"max_consecutive_denials",3}};
  const QJsonObject execution{{"image_staging",true},{"camera_prefetch",true},
      {"capacity_storage",true},{"surfel_auxiliary",true},{"loss_mode","auto"},
      {"engine","stock"},{"gpu_slots",2},{"pinned_mib",256},{"gpu_mib",512}};
  const QJsonObject options{{"version",2},{"resourceGuard",guard},{"execution",execution},
      {"monitoring",QJsonObject{{"profile_interval",30}}}};
  const QJsonObject runtime{{"requested",options},{"effective",options},
      {"reasons",QJsonArray{}},{"migration",QJsonObject{{"source","runtimeOptions"},{"from_version",2},{"to_version",2},{"notes",QJsonArray{}}}}};
  for(const auto &reason:QStringList{"cuda_oom","cuda_oom_reported","training_exception",
      "resource_probe_unavailable","resource_estimate_unavailable","vram_pressure","manual_pause"}) {
    const auto decoded=ProcessSupervisor::decodeStatusEvent(QJsonDocument(QJsonObject{
        {"version",1},{"type","status"},{"state","running"},{"stage","train"},{"iteration",4},
        {"runtimeConfiguration",runtime},{"failureReason",reason}}).toJson(QJsonDocument::Compact));
    QVERIFY(decoded.has_value()); QCOMPARE(decoded->iteration.value(),4);
    QCOMPARE(decoded->runtimeConfiguration,runtime); QCOMPARE(decoded->failureReason,reason);
  }
}

void ProcessSupervisorTests::growthBypassIsExplicitlyUnmeasured() {
  QJsonObject payload{{"version",1},{"backend","3dgs"},{"iteration",4},
      {"state","bypassed"},{"reason","guard_disabled"},{"allowed",true},
      {"guard_enabled",false},{"observation_valid",false},{"candidate_counts_known",false}};
  for(const auto &key:QStringList{"point_count","transient_point_count","final_point_count", "parameter_bytes",
      "optimizer_state_bytes","additional_peak_bytes","free_bytes","total_bytes","spare_bytes",
      "available_bytes","cache_credit_bytes","clone_count","split_count"}) payload.insert(key,0);
  const auto bypass=normalizeTrainingGrowth(payload);
  QVERIFY(!bypass.isEmpty()); QCOMPARE(bypass.value("state").toString(),QStringLiteral("bypassed"));
  payload.insert("free_bytes",100); QVERIFY(normalizeTrainingGrowth(payload).isEmpty());
}

void ProcessSupervisorTests::persistentObserverDestructionLeavesWorkerAlive() {
#ifndef Q_OS_WIN
  QSKIP("Persistent process identity currently targets native Windows.");
#else
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString store=QDir(temporary.path()).filePath("jobs");
  const QString ready=QDir(temporary.path()).filePath("ready");
  const QString release=QDir(temporary.path()).filePath("release");
  const auto cleanup=qScopeGuard([&] { QFile file(release); if(file.open(QIODevice::WriteOnly)) file.write("release"); });
  auto supervisor=std::make_unique<ProcessSupervisor>();
  QVERIFY(supervisor->startPersistentTraining("persistent", QCoreApplication::applicationFilePath(),
      {"--persistent-fixture",store,ready,release},temporary.path(),{},
      QDir(temporary.path()).filePath("worker.log"),store));
  QVERIFY(supervisor->isPersistentTraining());
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready),5000);
  QFile file(ready); QVERIFY(file.open(QIODevice::ReadOnly));
  const auto pid=file.readAll().toLongLong(); file.close();
  QVERIFY(processIsRunning(pid));
  QVERIFY(ProcessSupervisor::hasUnfinishedTraining(store));
  supervisor.reset();
  QTest::qWait(150);
  QVERIFY(processIsRunning(pid));
  QFile releaseFile(release); QVERIFY(releaseFile.open(QIODevice::WriteOnly));
  releaseFile.write("release"); releaseFile.close();
  QTRY_VERIFY_WITH_TIMEOUT(!processIsRunning(pid),5000);
  QVERIFY(!ProcessSupervisor::hasUnfinishedTraining(store));
#endif
}

void ProcessSupervisorTests::persistentExplicitControlsAreSessionBound_data() {
  QTest::addColumn<bool>("pause");
  QTest::newRow("pause") << true;
  QTest::newRow("stop") << false;
}

void ProcessSupervisorTests::persistentExplicitControlsAreSessionBound() {
#ifndef Q_OS_WIN
  QSKIP("Persistent process identity currently targets native Windows.");
#else
  QFETCH(bool,pause);
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QString store=QDir(temporary.path()).filePath("jobs");
  const QString ready=QDir(temporary.path()).filePath("ready");
  const QString release=QDir(temporary.path()).filePath("release");
  const auto cleanup=qScopeGuard([&] { QFile file(release); if(file.open(QIODevice::WriteOnly)) file.write("release"); });
  ProcessSupervisor supervisor;
  QSignalSpy finished(&supervisor,&ProcessSupervisor::taskFinished);
  QSignalSpy statuses(&supervisor,&ProcessSupervisor::workerStatusReady);
  QVERIFY(supervisor.startPersistentTraining("persistent",QCoreApplication::applicationFilePath(),
      {"--persistent-fixture",store,ready,release},temporary.path(),{},
      QDir(temporary.path()).filePath("worker.log"),store));
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready),5000);
  QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty(),5000);
  const QString jobPath=QDir(store).filePath(QString(32,QLatin1Char('a'))+QStringLiteral(".json"));
  QFile jobFile(jobPath); QVERIFY(jobFile.open(QIODevice::ReadOnly));
  const QJsonObject originalJob=QJsonDocument::fromJson(jobFile.readAll()).object(); jobFile.close();
  auto invalidJob=originalJob;
  auto invalidControl=invalidJob.value("native_control").toObject();
  invalidControl.insert("session",QString(32,QLatin1Char('c')));
  invalidJob.insert("native_control",invalidControl);
  QVERIFY(writeJsonFixture(jobPath,invalidJob));
  QVERIFY(!supervisor.requestPause());
  QVERIFY(!supervisor.wasStopRequested());
  QVERIFY(supervisor.isRunning());
  QVERIFY(writeJsonFixture(jobPath,originalJob));
  if(pause) QVERIFY(supervisor.requestPause()); else supervisor.stop();
  QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,5000);
  QCOMPARE(finished.first().at(1).toInt(),pause?75:130);
  QCOMPARE(supervisor.wasStopRequested(),!pause);
  QCOMPARE(qvariant_cast<WorkerStatus>(statuses.last().at(0)).state,
           pause?QStringLiteral("paused"):QStringLiteral("cancelled"));
  QVERIFY(!supervisor.isRunning());
#endif
}

void ProcessSupervisorTests::lifetimeFixtureWaitsForOwnedStdinRelease_data() {
  QTest::addColumn<bool>("explicitRelease");
  QTest::newRow("release-line") << true;
  QTest::newRow("write-channel-eof") << false;
}

void ProcessSupervisorTests::lifetimeFixtureWaitsForOwnedStdinRelease() {
  QFETCH(bool, explicitRelease);
  const QString helper = processOutputFixturePath();
  QVERIFY2(QFileInfo::exists(helper), qPrintable(helper));
  QProcess worker;
  const auto cleanup = qScopeGuard([&worker] {
    if (worker.state() == QProcess::NotRunning) return;
    worker.closeWriteChannel();
    if (!worker.waitForFinished(1000)) {
      worker.kill();
      worker.waitForFinished(1000);
    }
  });
  worker.start(helper, {QStringLiteral("lifetime-worker")});
  QVERIFY2(worker.waitForStarted(5000), qPrintable(worker.errorString()));
  QCOMPARE(worker.state(), QProcess::Running);
  QVERIFY2(!worker.waitForFinished(150), "An open owned stdin pipe must keep the fixture alive.");
  QCOMPARE(worker.state(), QProcess::Running);

  if (explicitRelease) {
    // A partial command is not a release until its line terminator arrives.
    QCOMPARE(worker.write("release"), qint64(7));
    QVERIFY(worker.waitForBytesWritten(1000));
    QVERIFY2(!worker.waitForFinished(100), "An unterminated release command must not end the fixture.");
    QCOMPARE(worker.write("\n"), qint64(1));
    QVERIFY(worker.waitForBytesWritten(1000));
  } else {
    worker.closeWriteChannel();
  }
  QVERIFY2(worker.waitForFinished(5000), qPrintable(worker.errorString()));
  QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
  QCOMPARE(worker.exitCode(), 0);
  QCOMPARE(worker.state(), QProcess::NotRunning);
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

void ProcessSupervisorTests::failedLaunchRestoresThePreviousTrainingResumeRecord_data() {
  QTest::addColumn<bool>("hasPrevious");
  QTest::newRow("preserve-paused-task") << true;
  QTest::newRow("first-task-no-prior-resume") << false;
}

void ProcessSupervisorTests::failedLaunchRestoresThePreviousTrainingResumeRecord() {
  QFETCH(bool, hasPrevious);
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString project = temporary.path();
  const ActiveTrainingJob previous{
      QDir(project).filePath(QStringLiteral(".gsw/jobs/paused-task.json")),
      QDir(project).filePath(QStringLiteral("output/paused-result")), true,
      QStringLiteral("original-task-result")};
  if (hasPrevious) QVERIFY(saveActiveTrainingJob(project, previous));
  const auto previousRecord = loadActiveTrainingJob(project);
  const ActiveTrainingJob attempt{
      QDir(project).filePath(QStringLiteral(".gsw/jobs/new-attempt.json")),
      QDir(project).filePath(QStringLiteral("output/new-attempt")), false,
      QStringLiteral("new-attempt-result")};
  QVERIFY(saveActiveTrainingJob(project, attempt));
  ProcessSupervisor supervisor;
  QSignalSpy finished(&supervisor, &ProcessSupervisor::taskFinished);
  bool reachedStarted = false;
  bool rolledBack = false;
  QString rollbackError;
  connect(&supervisor, &ProcessSupervisor::taskStarted, &supervisor,
      [&] { reachedStarted = true; });
  connect(&supervisor, &ProcessSupervisor::taskFinished, &supervisor,
      [&](const QString &, int, bool) {
        if (!reachedStarted)
          rolledBack = restoreActiveTrainingJobAfterLaunchFailure(
              project, previousRecord, &rollbackError);
      });
  QVERIFY(supervisor.start(QStringLiteral("failed-training-attempt"),
      QDir(project).filePath(QStringLiteral("missing-training-python.exe")),
      {}, project));
  QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
  QVERIFY(!reachedStarted);
  QCOMPARE(finished.first().at(1).toInt(), -1);
  QVERIFY2(rolledBack, qPrintable(rollbackError));
  QString loadError;
  const auto restored = loadActiveTrainingJob(project, &loadError);
  QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
  QCOMPARE(restored.isValid(), hasPrevious);
  if (hasPrevious) {
    QCOMPARE(restored.configurationPath, previousRecord.configurationPath);
    QCOMPARE(restored.outputSceneRoot, previousRecord.outputSceneRoot);
    QCOMPARE(restored.previewRecovered, true);
    QCOMPARE(restored.resultSceneId, QStringLiteral("original-task-result"));
  } else {
    QVERIFY(restored.configurationPath.isEmpty());
    QVERIFY(restored.outputSceneRoot.isEmpty());
    QVERIFY(restored.resultSceneId.isEmpty());
  }
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

int main(int argc,char **argv) {
  QCoreApplication application(argc,argv);
  if(application.arguments().value(1)==QStringLiteral("--persistent-fixture"))
    return persistentFixture(application);
  if(application.arguments().value(1)==QStringLiteral("--persistent-batch-fixture")||
      application.arguments().value(1)==QStringLiteral("--persistent-batch-publication-fixture"))
    return persistentBatchFixture(application);
  ProcessSupervisorTests tests;
  return QTest::qExec(&tests,argc,argv);
}

#include "ProcessSupervisorTests.moc"
