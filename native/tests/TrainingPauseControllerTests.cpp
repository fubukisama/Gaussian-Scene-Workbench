#include "TrainingOutputLocator.h"
#include "TrainingPauseController.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace gsw;

namespace {
bool writeJson(const QString &path, const QJsonObject &object) {
  QFile file(path);
  const QByteArray bytes = QJsonDocument(object).toJson();
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool pathLink(const QString &target, const QString &link, bool directory = false) {
#ifdef Q_OS_WIN
  constexpr DWORD allowUnprivileged = 0x2;
  return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()),
                            reinterpret_cast<LPCWSTR>(target.utf16()),
                            (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | allowUnprivileged);
#else
  Q_UNUSED(directory)
  return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
#endif
}

bool directoryLink(const QString &target, const QString &link) {
  if (pathLink(target, link, true)) return true;
#ifdef Q_OS_WIN
  // A junction fixture does not require enabling symbolic-link privileges.
  QProcess process;
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
    arguments->flags |= CREATE_NO_WINDOW;
  });
  process.start(qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe")),
                {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"),
                 QStringLiteral("/J"), QDir::toNativeSeparators(link),
                 QDir::toNativeSeparators(target)});
  return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0 && QFileInfo(link).isJunction();
#else
  return false;
#endif
}

struct Fixture {
  QTemporaryDir temporary;
  QDir root{temporary.path()};
  QString backend;
  const QString session = QStringLiteral("abcdef0123456789abcdef0123456789");
  const QString jobId = QStringLiteral("0123456789abcdef0123456789abcdef");
  QString output = root.filePath(QStringLiteral("output/owned-result"));
  QString jobs = root.filePath(QStringLiteral(".gsw/jobs/training"));
  QString configuration = root.filePath(QStringLiteral(".gsw/jobs/training-owned.json"));
  QString jobPath = QDir(jobs).filePath(jobId + QStringLiteral(".json"));
  QString request = QDir(jobs).filePath(QStringLiteral("pause-%1.json").arg(session));
  QJsonObject config;
  QJsonObject job;

  bool create(const QString &method) {
    backend = method;
    if (!temporary.isValid() || !root.mkpath(QStringLiteral("output/owned-result")) ||
        !QDir().mkpath(jobs) || !root.mkpath(QStringLiteral("dataset"))) return false;
    config = {{QStringLiteral("projectRoot"), root.path()},
              {QStringLiteral("backend"), backend}, {QStringLiteral("nativeCheckpoint"), true},
              {QStringLiteral("outputRoot"), root.filePath(QStringLiteral("output"))},
              {QStringLiteral("outputScene"), QStringLiteral("owned-result")},
              {QStringLiteral("datasetPath"), root.filePath(QStringLiteral("dataset"))},
              {QStringLiteral("jobStore"), jobs}};
    job = {{QStringLiteral("id"), jobId}, {QStringLiteral("status"), QStringLiteral("running")},
           {QStringLiteral("stage"), QStringLiteral("train")},
           {QStringLiteral("backend"), backend},
           {QStringLiteral("output_scene"), QStringLiteral("owned-result")},
           {QStringLiteral("dataset_path"), root.filePath(QStringLiteral("dataset"))},
           {QStringLiteral("native_control"), QJsonObject{
                {QStringLiteral("output"), output}, {QStringLiteral("request"), request},
                {QStringLiteral("session"), session}, {QStringLiteral("backend"), backend}}}};
    return writeJson(configuration, config) && writeJson(jobPath, job) &&
           saveActiveTrainingJob(root.path(), {configuration, output});
  }
};
} // namespace

class TrainingPauseControllerTests final : public QObject {
  Q_OBJECT
private slots:
  void publishesPauseWithoutWorkerStdin_data();
  void publishesPauseWithoutWorkerStdin();
  void rejectsForeignOrInconsistentControl_data();
  void rejectsForeignOrInconsistentControl();
  void rejectsAmbiguousRunningJobs();
  void preservesAnExistingForeignSessionRequest();
  void rejectsLinkedRequest();
  void rejectsLinkedJobStore();
};

void TrainingPauseControllerTests::publishesPauseWithoutWorkerStdin_data() {
  QTest::addColumn<QString>("backend");
  QTest::newRow("3dgs") << QStringLiteral("3dgs");
  QTest::newRow("2dgs") << QStringLiteral("2dgs");
}

void TrainingPauseControllerTests::publishesPauseWithoutWorkerStdin() {
  QFETCH(QString, backend);
  Fixture fixture;
  QVERIFY(fixture.create(backend));
  QString error;
  QVERIFY2(requestActiveTrainingPause(fixture.root.path(), &error), qPrintable(error));
  QVERIFY(error.isEmpty());
  QFile request(fixture.request);
  QVERIFY(request.open(QIODevice::ReadOnly));
  const QJsonObject payload = QJsonDocument::fromJson(request.readAll()).object();
  QCOMPARE(payload.value(QStringLiteral("session")).toString(), fixture.session);
  QCOMPARE(payload.size(), 1);
  QCOMPARE(QDir(fixture.jobs).entryList({QStringLiteral("pause-*.json")}, QDir::Files).size(), 1);
  QVERIFY(requestActiveTrainingPause(fixture.root.path(), &error));
}

void TrainingPauseControllerTests::rejectsForeignOrInconsistentControl_data() {
  QTest::addColumn<QString>("mutation");
  for (const char *name : {"session", "invalid-session", "output", "request", "request-traversal",
                           "output-traversal", "store", "backend", "job-id", "dataset", "done", "stage"})
    QTest::newRow(name) << QString::fromLatin1(name);
}

void TrainingPauseControllerTests::rejectsForeignOrInconsistentControl() {
  QFETCH(QString, mutation);
  Fixture fixture;
  QVERIFY(fixture.create(QStringLiteral("3dgs")));
  auto control = fixture.job.value(QStringLiteral("native_control")).toObject();
  if (mutation == QStringLiteral("session")) control.insert(QStringLiteral("session"), QString(32, QLatin1Char('f')));
  if (mutation == QStringLiteral("invalid-session")) control.insert(QStringLiteral("session"), QStringLiteral("../foreign"));
  if (mutation == QStringLiteral("output")) control.insert(QStringLiteral("output"), fixture.root.filePath(QStringLiteral("foreign-output")));
  if (mutation == QStringLiteral("request")) control.insert(QStringLiteral("request"), fixture.root.filePath(QStringLiteral("foreign-request.json")));
  if (mutation == QStringLiteral("request-traversal")) control.insert(QStringLiteral("request"), fixture.jobs + QStringLiteral("/../training/") + QFileInfo(fixture.request).fileName());
  if (mutation == QStringLiteral("output-traversal")) control.insert(QStringLiteral("output"), fixture.root.filePath(QStringLiteral("output/../output/owned-result")));
  if (mutation == QStringLiteral("store")) fixture.config.insert(QStringLiteral("jobStore"), fixture.root.filePath(QStringLiteral("foreign-store")));
  if (mutation == QStringLiteral("backend")) control.insert(QStringLiteral("backend"), QStringLiteral("2dgs"));
  if (mutation == QStringLiteral("job-id")) fixture.job.insert(QStringLiteral("id"), QString(32, QLatin1Char('f')));
  if (mutation == QStringLiteral("dataset")) fixture.job.insert(QStringLiteral("dataset_path"), fixture.root.filePath(QStringLiteral("foreign-dataset")));
  if (mutation == QStringLiteral("done")) fixture.job.insert(QStringLiteral("status"), QStringLiteral("done"));
  if (mutation == QStringLiteral("stage")) fixture.job.insert(QStringLiteral("stage"), QStringLiteral("colmap"));
  fixture.job.insert(QStringLiteral("native_control"), control);
  QVERIFY(writeJson(fixture.configuration, fixture.config));
  QVERIFY(writeJson(fixture.jobPath, fixture.job));
  QString error;
  QVERIFY(!requestActiveTrainingPause(fixture.root.path(), &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(!QFileInfo::exists(fixture.request));
  QVERIFY(!QFileInfo::exists(fixture.root.filePath(QStringLiteral("foreign-request.json"))));
  QCOMPARE(QDir(fixture.jobs).entryList({QStringLiteral("pause-*.json")}, QDir::Files).size(), 0);
}

void TrainingPauseControllerTests::rejectsAmbiguousRunningJobs() {
  Fixture fixture;
  QVERIFY(fixture.create(QStringLiteral("3dgs")));
  auto second = fixture.job;
  const QString id(32, QLatin1Char('f'));
  second.insert(QStringLiteral("id"), id);
  QVERIFY(writeJson(QDir(fixture.jobs).filePath(id + QStringLiteral(".json")), second));
  QString error;
  QVERIFY(!requestActiveTrainingPause(fixture.root.path(), &error));
  QVERIFY(!QFileInfo::exists(fixture.request));
}

void TrainingPauseControllerTests::preservesAnExistingForeignSessionRequest() {
  Fixture fixture;
  QVERIFY(fixture.create(QStringLiteral("2dgs")));
  const QJsonObject foreign{{QStringLiteral("session"), QString(32, QLatin1Char('f'))}};
  QVERIFY(writeJson(fixture.request, foreign));
  QString error;
  QVERIFY(!requestActiveTrainingPause(fixture.root.path(), &error));
  QFile file(fixture.request);
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(), foreign);
}

void TrainingPauseControllerTests::rejectsLinkedRequest() {
  Fixture fixture;
  QVERIFY(fixture.create(QStringLiteral("3dgs")));
  const QString foreign = fixture.root.filePath(QStringLiteral("foreign-control.json"));
  const QJsonObject foreignPayload{{QStringLiteral("session"), QString(32, QLatin1Char('f'))}};
  QVERIFY(writeJson(foreign, foreignPayload));
  if (!pathLink(foreign, fixture.request)) QSKIP("This Windows account cannot create an unprivileged symbolic-link fixture.");
  QString error;
  QVERIFY(!requestActiveTrainingPause(fixture.root.path(), &error));
  QFile foreignFile(foreign);
  QVERIFY(foreignFile.open(QIODevice::ReadOnly));
  QCOMPARE(QJsonDocument::fromJson(foreignFile.readAll()).object(), foreignPayload);
}

void TrainingPauseControllerTests::rejectsLinkedJobStore() {
  Fixture fixture;
  QVERIFY(fixture.create(QStringLiteral("3dgs")));
  const QString moved = fixture.root.filePath(QStringLiteral("owned-store-target"));
  QVERIFY(QDir::cleanPath(QFileInfo(moved).absoluteFilePath()).startsWith(
      QDir::cleanPath(fixture.root.absolutePath()) + QLatin1Char('/')));
  QVERIFY(QDir::cleanPath(QFileInfo(fixture.jobs).absoluteFilePath()).startsWith(
      QDir::cleanPath(fixture.root.absolutePath()) + QLatin1Char('/')));
  QVERIFY(QDir().rename(fixture.jobs, moved));
  if (!directoryLink(moved, fixture.jobs)) QSKIP("This account cannot create a directory-link fixture.");
  QString error;
  QVERIFY(!requestActiveTrainingPause(fixture.root.path(), &error));
  QVERIFY(!QFileInfo::exists(QDir(moved).filePath(QFileInfo(fixture.request).fileName())));
}

QTEST_GUILESS_MAIN(TrainingPauseControllerTests)
#include "TrainingPauseControllerTests.moc"
