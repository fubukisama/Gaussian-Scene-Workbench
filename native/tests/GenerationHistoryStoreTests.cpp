#include "GenerationHistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace gsw;

namespace {
bool writeJson(const QString &path, const QJsonObject &object) {
  QFile file(path);
  const auto bytes = QJsonDocument(object).toJson();
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

GenerationExperiment pausedFixture(const QString &root, const QString &backend) {
  QDir project(root);
  project.mkpath(QStringLiteral("datasets/input"));
  project.mkpath(QStringLiteral(".gsw/jobs"));
  project.mkpath(QStringLiteral("output/owned/.gsw-resume"));
  GenerationExperiment record;
  record.id = QStringLiteral("experiment-") + backend;
  record.pipeline = QStringLiteral("training");
  record.backend = backend;
  record.displayName = QStringLiteral("本机实验 · ") + backend;
  record.datasetPath = project.filePath(QStringLiteral("datasets/input"));
  record.configurationPath = project.filePath(QStringLiteral(".gsw/jobs/training-owned.json"));
  record.outputRoot = project.filePath(QStringLiteral("output/owned"));
  record.resultSceneId = record.id;
  record.status = QStringLiteral("paused");
  record.parameters = {{QStringLiteral("projectRoot"), root},
      {QStringLiteral("backend"), backend}, {QStringLiteral("nativeCheckpoint"), true},
      {QStringLiteral("datasetPath"), record.datasetPath},
      {QStringLiteral("outputRoot"), project.filePath(QStringLiteral("output"))},
      {QStringLiteral("outputScene"), QStringLiteral("owned")},
      {QStringLiteral("outputDisplayName"), record.displayName},
      {QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 30000}}}};
  writeJson(record.configurationPath, record.parameters);
  const QString stateName = QStringLiteral("state-0123456789abcdef0123456789abcdef.pth");
  QFile state(QDir(record.outputRoot).filePath(QStringLiteral(".gsw-resume/") + stateName));
  state.open(QIODevice::WriteOnly);
  state.write("opaque-state-fixture");
  state.close();
  writeJson(QDir(record.outputRoot).filePath(QStringLiteral(".gsw-resume/ready.json")),
      {{QStringLiteral("version"), 1}, {QStringLiteral("backend"), backend},
       {QStringLiteral("file"), stateName}, {QStringLiteral("iteration"), 4407},
       {QStringLiteral("total"), 30000}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
       {QStringLiteral("identity"), QString(64, QLatin1Char('b'))}});
  return record;
}
} // namespace

class GenerationHistoryStoreTests final : public QObject {
  Q_OBJECT
private slots:
  void experimentsRetainTheirIndependentParametersAfterReopening() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    GenerationHistoryStore first(temporary.path());
    GenerationExperiment left;
    left.id = QStringLiteral("experiment-left");
    left.pipeline = QStringLiteral("training");
    left.backend = QStringLiteral("3dgs");
    left.displayName = QStringLiteral("研究 A / 原名");
    left.datasetPath = QDir(temporary.path()).filePath(QStringLiteral("datasets/input"));
    left.outputRoot = QDir(temporary.path()).filePath(QStringLiteral("output/left"));
    left.status = QStringLiteral("paused");
    left.parameters = {{QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 30000}}}};
    GenerationExperiment right = left;
    right.id = QStringLiteral("experiment-right");
    right.backend = QStringLiteral("2dgs");
    right.displayName = QStringLiteral("研究 B");
    right.outputRoot = QDir(temporary.path()).filePath(QStringLiteral("output/right"));
    right.parameters = {{QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 12000}}}};
    QString error;
    QVERIFY2(first.upsert(left, &error), qPrintable(error));
    QVERIFY2(first.upsert(right, &error), qPrintable(error));
    GenerationHistoryStore reopened(temporary.path());
    QCOMPARE(reopened.records(&error).size(), 2);
    QCOMPARE(reopened.record(left.id).displayName, QStringLiteral("研究 A / 原名"));
    QCOMPARE(reopened.record(left.id).parameters.value(QStringLiteral("trainOptions")).toObject()
                 .value(QStringLiteral("iterations")).toInt(), 30000);
    QCOMPARE(reopened.record(right.id).parameters.value(QStringLiteral("trainOptions")).toObject()
                 .value(QStringLiteral("iterations")).toInt(), 12000);
  }

  void fullStateResumeAvailabilityIsIndependentForEachNativeBackend_data() {
    QTest::addColumn<QString>("backend");
    QTest::newRow("3dgs") << QStringLiteral("3dgs");
    QTest::newRow("2dgs") << QStringLiteral("2dgs");
  }

  void fullStateResumeAvailabilityIsIndependentForEachNativeBackend() {
    QFETCH(QString, backend);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto record = pausedFixture(temporary.path(), backend);
    GenerationHistoryStore store(temporary.path());
    QString error;
    QVERIFY2(store.upsert(record, &error), qPrintable(error));
    GenerationHistoryStore reopened(temporary.path());
    const auto availability = reopened.resumeAvailability(record.id);
    QVERIFY2(availability.available, qPrintable(availability.reason));
    QCOMPARE(availability.iteration, 4407);
    const auto resume = reopened.resumeJob(record.id, &error);
    QVERIFY2(resume.isValid(), qPrintable(error));
    QCOMPARE(resume.outputSceneRoot, record.outputRoot);
    QCOMPARE(resume.resultSceneId, record.id);
  }

  void legacyPausedEntryMigratesWithoutStartingOrReplacingOtherExperiments() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto legacy = pausedFixture(temporary.path(), QStringLiteral("2dgs"));
    QString error;
    QVERIFY2(saveActiveTrainingJob(temporary.path(),
        {legacy.configurationPath, legacy.outputRoot, true, legacy.resultSceneId}, &error), qPrintable(error));
    GenerationHistoryStore store(temporary.path());
    QVERIFY2(store.migrateActiveTraining(&error), qPrintable(error));
    QCOMPARE(store.records().size(), 1);
    const auto archived = store.record(legacy.id);
    QCOMPARE(archived.displayName, QStringLiteral("本机实验 · 2dgs"));
    QCOMPARE(archived.status, QStringLiteral("paused"));
    QCOMPARE(archived.parameters.value(QStringLiteral("trainOptions")).toObject()
                 .value(QStringLiteral("iterations")).toInt(), 30000);
    QVERIFY(store.migrateActiveTraining(&error));
    QCOMPARE(store.records().size(), 1);
    QCOMPARE(loadActiveTrainingJob(temporary.path()).resultSceneId, legacy.id);
  }

  void projectRelocationMovesManagedPathsButRetainsExternalInputs() {
    QTemporaryDir oldProject, newProject, externalDataset;
    QVERIFY(oldProject.isValid() && newProject.isValid() && externalDataset.isValid());
    auto original = pausedFixture(oldProject.path(), QStringLiteral("3dgs"));
    original.datasetPath = externalDataset.path();
    original.parameters.insert(QStringLiteral("datasetPath"), externalDataset.path());
    QVERIFY(writeJson(original.configurationPath, original.parameters));
    GenerationHistoryStore before(oldProject.path());
    QString error;
    QVERIFY2(before.upsert(original, &error), qPrintable(error));
    const auto destination = pausedFixture(newProject.path(), QStringLiteral("3dgs"));
    QVERIFY(QDir(newProject.path()).mkpath(QStringLiteral(".gsw/experiments")));
    const QString archiveName = QStringLiteral(".gsw/experiments/") + original.id + QStringLiteral(".json");
    QVERIFY(QFile::copy(QDir(oldProject.path()).filePath(archiveName), QDir(newProject.path()).filePath(archiveName)));
    GenerationHistoryStore reopened(newProject.path());
    const auto moved = reopened.record(original.id);
    QCOMPARE(moved.datasetPath, externalDataset.path());
    QCOMPARE(moved.outputRoot, destination.outputRoot);
    QCOMPARE(moved.configurationPath, destination.configurationPath);
    QCOMPARE(moved.parameters.value(QStringLiteral("projectRoot")).toString(), newProject.path());
    QCOMPARE(moved.parameters.value(QStringLiteral("datasetPath")).toString(), externalDataset.path());
    QCOMPARE(moved.parameters.value(QStringLiteral("outputRoot")).toString(), QDir(newProject.path()).filePath(QStringLiteral("output")));
    QVERIFY(reopened.resumeAvailability(original.id).available);
  }

  void meshStagesNeverAdvertisePreviewAsOptimizerStateResume_data() {
    QTest::addColumn<QString>("mode");
    for (const auto &mode : {QStringLiteral("bounded"), QStringLiteral("unbounded"),
                            QStringLiteral("sugar"), QStringLiteral("gs2mesh")})
      QTest::newRow(qPrintable(mode)) << mode;
  }

  void meshStagesNeverAdvertisePreviewAsOptimizerStateResume() {
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    auto record = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    record.pipeline = QStringLiteral("mesh");
    record.backend = mode;
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.upsert(record));
    const auto availability = store.resumeAvailability(record.id);
    QVERIFY(!availability.available);
    QVERIFY(!availability.reason.isEmpty());
    QVERIFY(!store.resumeJob(record.id).isValid());
  }

  void completedTrainingDoesNotOfferAnEarlierPausedCheckpoint() {
    QTemporaryDir temporary;
    auto record = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    record.status = QStringLiteral("completed");
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.upsert(record));
    QVERIFY(!store.resumeAvailability(record.id).available);
    QVERIFY(!store.resumeJob(record.id).isValid());
  }

  void invalidCheckpointMetadataNeverOffersResume_data() {
    QTest::addColumn<QString>("defect");
    for (const auto &defect : {QStringLiteral("missing-state"), QStringLiteral("wrong-backend"),
        QStringLiteral("wrong-total"), QStringLiteral("traversal"), QStringLiteral("bad-digest"),
        QStringLiteral("invalid-json"), QStringLiteral("missing-config"), QStringLiteral("missing-dataset")})
      QTest::newRow(qPrintable(defect)) << defect;
  }

  void invalidCheckpointMetadataNeverOffersResume() {
    QFETCH(QString, defect);
    QTemporaryDir temporary;
    auto record = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.upsert(record));
    const auto checkpointRoot = QDir(record.outputRoot).filePath(QStringLiteral(".gsw-resume"));
    const auto manifestPath = QDir(checkpointRoot).filePath(QStringLiteral("ready.json"));
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    auto object = QJsonDocument::fromJson(manifest.readAll()).object();
    manifest.close();
    if (defect == QStringLiteral("missing-state")) QVERIFY(QFile::remove(QDir(checkpointRoot).filePath(object.value(QStringLiteral("file")).toString())));
    else if (defect == QStringLiteral("missing-config")) QVERIFY(QFile::remove(record.configurationPath));
    else if (defect == QStringLiteral("missing-dataset")) QVERIFY(QDir(record.datasetPath).removeRecursively());
    else if (defect == QStringLiteral("invalid-json")) {
      QVERIFY(manifest.open(QIODevice::WriteOnly));
      manifest.write("not json");
      manifest.close();
    } else {
      if (defect == QStringLiteral("wrong-backend")) object.insert(QStringLiteral("backend"), QStringLiteral("2dgs"));
      if (defect == QStringLiteral("wrong-total")) object.insert(QStringLiteral("total"), 12000);
      if (defect == QStringLiteral("traversal")) object.insert(QStringLiteral("file"), QStringLiteral("../foreign.pth"));
      if (defect == QStringLiteral("bad-digest")) object.insert(QStringLiteral("sha256"), QStringLiteral("not a checksum"));
      QVERIFY(writeJson(manifestPath, object));
    }
    const auto availability = store.resumeAvailability(record.id);
    QVERIFY(!availability.available);
    QVERIFY(!availability.reason.isEmpty());
    QString error;
    QVERIFY(!store.resumeJob(record.id, &error).isValid());
    QVERIFY(!error.isEmpty());
  }

  void updatesKeepExperimentIdentityAndOriginalCreationTime() {
    QTemporaryDir temporary;
    auto record = pausedFixture(temporary.path(), QStringLiteral("2dgs"));
    record.createdAt = QDateTime::fromString(QStringLiteral("2026-10-08T10:00:00.000Z"), Qt::ISODateWithMs);
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.upsert(record));
    const auto created = store.record(record.id).createdAt;
    record.createdAt = QDateTime::fromString(QStringLiteral("2026-10-09T10:00:00.000Z"), Qt::ISODateWithMs);
    record.status = QStringLiteral("running");
    QVERIFY(store.upsert(record));
    QCOMPARE(store.records().size(), 1);
    QCOMPARE(store.record(record.id).createdAt, created);
    QCOMPARE(store.record(record.id).resultSceneId, record.resultSceneId);
    QCOMPARE(store.record(record.id).parameters.value(QStringLiteral("trainOptions")).toObject()
                 .value(QStringLiteral("iterations")).toInt(), 30000);
  }

  void foreignConfigurationCannotBecomeAnExecutableResumeEntry() {
    QTemporaryDir project, outside;
    QVERIFY(project.isValid() && outside.isValid());
    auto record = pausedFixture(project.path(), QStringLiteral("3dgs"));
    record.configurationPath = outside.filePath(QStringLiteral("foreign-config.json"));
    QVERIFY(writeJson(record.configurationPath, record.parameters));
    GenerationHistoryStore store(project.path());
    QVERIFY(store.upsert(record));
    QVERIFY(!store.resumeAvailability(record.id).available);
    QVERIFY(!store.resumeJob(record.id).isValid());
  }

  void missingLegacyConfigurationDoesNotHideAnAlreadyArchivedExperiment() {
    QTemporaryDir temporary;
    const auto record = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.upsert(record));
    QVERIFY(saveActiveTrainingJob(temporary.path(),
        {record.configurationPath, record.outputRoot, true, record.id}));
    QVERIFY(QFile::remove(record.configurationPath));
    QString error;
    QVERIFY2(store.migrateActiveTraining(&error), qPrintable(error));
    QCOMPARE(store.records().size(), 1);
    QCOMPARE(store.record(record.id).parameters.value(QStringLiteral("outputDisplayName")).toString(), record.displayName);
    QVERIFY(!store.resumeAvailability(record.id).available);
  }

  void archivedControlStoreCannotRedirectResumeWritesOutsideTheProject() {
    QTemporaryDir project, external;
    auto record = pausedFixture(project.path(), QStringLiteral("3dgs"));
    record.parameters.insert(QStringLiteral("jobStore"), external.path());
    QVERIFY(writeJson(record.configurationPath, record.parameters));
    GenerationHistoryStore store(project.path());
    QVERIFY(store.upsert(record));
    QVERIFY(!store.resumeAvailability(record.id).available);
    QVERIFY(!store.resumeJob(record.id).isValid());
  }

  void legacyMigrationPrefersItsProtectedPausedResult_data() {
    QTest::addColumn<bool>("protectedValid");
    QTest::newRow("valid-protected") << true;
    QTest::newRow("invalid-protected-falls-back") << false;
  }

  void legacyMigrationPrefersItsProtectedPausedResult() {
    QFETCH(bool, protectedValid);
    QTemporaryDir temporary;
    const auto record = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    const QString protectedPath = QDir(temporary.path()).filePath(
        QStringLiteral(".gsw/checkpoints/training/owned/iteration_4407/point_cloud.ply"));
    QVERIFY(QDir().mkpath(QFileInfo(protectedPath).absolutePath()));
    QFile protectedFile(protectedPath);
    QVERIFY(protectedFile.open(QIODevice::WriteOnly));
    const QByteArray gaussian("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nend_header\n0 0 0 0 0 0 1 -3 -3 -3 1 0 0 0\n");
    protectedFile.write(protectedValid ? gaussian : QByteArray("not a PLY"));
    protectedFile.close();
    const QString originalOutput = QDir(record.outputRoot).filePath(QStringLiteral("point_cloud/iteration_3000/point_cloud.ply"));
    QVERIFY(QDir().mkpath(QFileInfo(originalOutput).absolutePath()));
    QFile originalFile(originalOutput);
    QVERIFY(originalFile.open(QIODevice::WriteOnly));
    QCOMPARE(originalFile.write(gaussian), gaussian.size());
    originalFile.close();
    QVERIFY(saveActiveTrainingJob(temporary.path(),
        {record.configurationPath, record.outputRoot, true, record.id}));
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.migrateActiveTraining());
    QCOMPARE(store.record(record.id).resultPath, protectedValid ? protectedPath : originalOutput);
    QCOMPARE(store.record(record.id).resultSceneId, record.id);
    QVERIFY(store.resumeAvailability(record.id).available);
  }

  void changingAResultObjectDoesNotCreateASecondExperiment() {
    QTemporaryDir temporary;
    auto archived = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.upsert(archived));
    archived.resultSceneId = QStringLiteral("new-user-safe-model-slot");
    QVERIFY(store.upsert(archived));
    QVERIFY(saveActiveTrainingJob(temporary.path(),
        {archived.configurationPath, archived.outputRoot, true, archived.resultSceneId}));
    QVERIFY(store.migrateActiveTraining());
    QCOMPARE(store.records().size(), 1);
    QCOMPARE(store.record(archived.id).resultSceneId, QStringLiteral("new-user-safe-model-slot"));
    QCOMPARE(store.record(archived.id).parameters.value(QStringLiteral("outputDisplayName")).toString(), archived.displayName);
  }

  void migrationPrefersExperimentScopedCopiesAndPreservesLegacyCopies_data() {
    QTest::addColumn<QString>("scopedState");
    QTest::newRow("scoped-copy-preferred") << QStringLiteral("valid");
    QTest::newRow("damaged-scoped-copy-keeps-legacy") << QStringLiteral("damaged");
    QTest::newRow("missing-scoped-copy-keeps-legacy") << QStringLiteral("missing");
  }

  void migrationPrefersExperimentScopedCopiesAndPreservesLegacyCopies() {
    QFETCH(QString, scopedState);
    QTemporaryDir temporary;
    const auto record = pausedFixture(temporary.path(), QStringLiteral("3dgs"));
    const QString legacyPath = QDir(temporary.path()).filePath(
        QStringLiteral(".gsw/checkpoints/training/owned/iteration_4407/point_cloud.ply"));
    // This independently computed digest belongs to the fixture's public result
    // identity "experiment-3dgs", rather than its shared output basename.
    const QString scopedPath = QDir(temporary.path()).filePath(QStringLiteral(
        ".gsw/checkpoints/training/owned-bce2869d0825b368190b58f8b798ef59/iteration_4407/point_cloud.ply"));
    const QByteArray gaussian("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nend_header\n0 0 0 0 0 0 1 -3 -3 -3 1 0 0 0\n");
    QVERIFY(QDir().mkpath(QFileInfo(legacyPath).absolutePath()));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QCOMPARE(legacyFile.write(gaussian), gaussian.size());
    legacyFile.close();
    if (scopedState != QStringLiteral("missing")) {
      QVERIFY(QDir().mkpath(QFileInfo(scopedPath).absolutePath()));
      QFile scopedFile(scopedPath);
      QVERIFY(scopedFile.open(QIODevice::WriteOnly));
      const QByteArray bytes = scopedState == QStringLiteral("valid")
          ? gaussian : QByteArray("damaged protected copy");
      QCOMPARE(scopedFile.write(bytes), bytes.size());
    }
    QVERIFY(saveActiveTrainingJob(temporary.path(),
        {record.configurationPath, record.outputRoot, true, record.resultSceneId}));
    GenerationHistoryStore store(temporary.path());
    QVERIFY(store.migrateActiveTraining());
    QCOMPARE(store.record(record.id).resultPath,
        scopedState == QStringLiteral("valid") ? scopedPath : legacyPath);
    QVERIFY(legacyFile.open(QIODevice::ReadOnly));
    QCOMPARE(legacyFile.readAll(), gaussian);
    QCOMPARE(store.records().size(), 1);
    QVERIFY(store.resumeAvailability(record.id).available);
  }
};

QTEST_GUILESS_MAIN(GenerationHistoryStoreTests)
#include "GenerationHistoryStoreTests.moc"
