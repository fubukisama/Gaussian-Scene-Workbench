#include "RecoveryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>

class RecoveryStoreTests final : public QObject {
  Q_OBJECT

private slots:
  void recoversAndDiscardsAnUntitledWorkspace();
  void removesRecoveryMetadataWhenWorkspaceBecomesManaged();
  void retainsAndRestoresProjectSnapshots();
  void rejectsUnsafeRecoveryDeletion();
  void preservesViewportGridValuesWithoutLosingRecoveryModelState();
  void legacyRecoveryOmitsViewportGridUntilExplicitlyProvided();
};

void RecoveryStoreTests::recoversAndDiscardsAnUntitledWorkspace() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString workspaceBase =
      QDir(temporary.path()).filePath(QStringLiteral("recovery"));
  gsw::RecoveryStore store(workspaceBase);

  QString error;
  const std::optional<gsw::RecoveryWorkspace> started =
      store.beginWorkspace(QStringLiteral("现场重建"), &error);
  QVERIFY2(started.has_value(), qPrintable(error));
  QVERIFY(QFileInfo::exists(started->rootPath));

  QFile payload(
      QDir(started->rootPath).filePath(QStringLiteral("frame.jpg")));
  QVERIFY(payload.open(QIODevice::WriteOnly));
  QCOMPARE(payload.write("frame"), qint64(5));
  payload.close();

  gsw::RecoveryWorkspace checkpoint = *started;
  checkpoint.datasetPath =
      QDir(started->rootPath).filePath(QStringLiteral("dataset"));
  checkpoint.sceneTranslation = QVector3D(2.0F, -1.0F, 0.5F);
  checkpoint.sceneRotation = QQuaternion::fromAxisAndAngle(
      QVector3D(0.0F, 0.0F, 1.0F), 42.0F);
  checkpoint.sceneScale = QVector3D(1.25F, 0.8F, 2.0F);
  checkpoint.sceneCollection = {{"activeId", "second"}, {"objects", QJsonArray{
      QJsonObject{{"id", "first"}, {"path", "first.ply"}},
      QJsonObject{{"id", "second"}, {"path", "second.ply"}}}}};
  QVERIFY2(store.checkpoint(checkpoint, &error), qPrintable(error));

  const QList<gsw::RecoveryWorkspace> recovered =
      gsw::RecoveryStore(workspaceBase).recoverableWorkspaces(&error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(recovered.size(), 1);
  QCOMPARE(recovered.constFirst().sessionId, started->sessionId);
  QCOMPARE(recovered.constFirst().displayName, QStringLiteral("现场重建"));
  QCOMPARE(recovered.constFirst().datasetPath,
           QDir::cleanPath(checkpoint.datasetPath));
  QCOMPARE(recovered.constFirst().sceneTranslation,
           checkpoint.sceneTranslation);
  QVERIFY(1.0F - std::abs(QQuaternion::dotProduct(
                      recovered.constFirst().sceneRotation.normalized(),
                      checkpoint.sceneRotation.normalized())) <
          1.0e-6F);
  QCOMPARE(recovered.constFirst().sceneScale, checkpoint.sceneScale);
  QCOMPARE(recovered.constFirst().sceneCollection, checkpoint.sceneCollection);
  QVERIFY(QFileInfo::exists(QDir(recovered.constFirst().rootPath)
                                .filePath(QStringLiteral("frame.jpg"))));

  QVERIFY2(store.discardWorkspace(recovered.constFirst(), &error),
           qPrintable(error));
  QVERIFY(!QFileInfo::exists(started->rootPath));
  QVERIFY(store.recoverableWorkspaces(&error).isEmpty());
}

void RecoveryStoreTests::removesRecoveryMetadataWhenWorkspaceBecomesManaged() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  const QString workspaceBase =
      root.filePath(QStringLiteral("recovery"));
  gsw::RecoveryStore store(workspaceBase);
  QString error;
  const std::optional<gsw::RecoveryWorkspace> started =
      store.beginWorkspace(QStringLiteral("现场重建"), &error);
  QVERIFY2(started.has_value(), qPrintable(error));

  const QString managedRoot =
      root.filePath(QStringLiteral("saved/project.files"));
  QVERIFY(root.mkpath(QStringLiteral("saved/project.files/.gsw")));
  QVERIFY(QFile::copy(
      QDir(started->rootPath)
          .filePath(QStringLiteral(".gsw/recovery-state.json")),
      QDir(managedRoot)
          .filePath(QStringLiteral(".gsw/recovery-state.json"))));

  QVERIFY2(store.completeWorkspace(*started, managedRoot, &error),
           qPrintable(error));
  QVERIFY(!QFileInfo::exists(started->rootPath));
  QVERIFY(!QFileInfo::exists(
      QDir(managedRoot)
          .filePath(QStringLiteral(".gsw/recovery-state.json"))));
}

void RecoveryStoreTests::retainsAndRestoresProjectSnapshots() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("project.files")));
  const QString projectFile =
      root.filePath(QStringLiteral("project.gsw.json"));
  const QString projectData =
      root.filePath(QStringLiteral("project.files"));
  gsw::RecoveryStore store(
      root.filePath(QStringLiteral("recovery-catalog")));

  const auto writeRevision = [&projectFile](const int revision) {
    QFile file(projectFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return false;
    }
    return file.write(
               QStringLiteral("{\"revision\":%1}").arg(revision).toUtf8()) >
           0;
  };

  QString error;
  QVERIFY(writeRevision(1));
  const std::optional<gsw::ProjectSnapshot> first =
      store.createProjectSnapshot(projectFile, projectData, 2, &error);
  QVERIFY2(first.has_value(), qPrintable(error));

  QVERIFY(writeRevision(2));
  QVERIFY2(store.createProjectSnapshot(projectFile, projectData, 2, &error)
               .has_value(),
           qPrintable(error));
  QVERIFY(writeRevision(3));
  QVERIFY2(store.createProjectSnapshot(projectFile, projectData, 2, &error)
               .has_value(),
           qPrintable(error));
  const std::optional<gsw::ProjectSnapshot> duplicate =
      store.createProjectSnapshot(projectFile, projectData, 2, &error);
  QVERIFY2(duplicate.has_value(), qPrintable(error));

  const QList<gsw::ProjectSnapshot> snapshots =
      store.projectSnapshots(projectData, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(snapshots.size(), 2);
  QVERIFY(std::none_of(snapshots.cbegin(), snapshots.cend(),
                       [&first](const gsw::ProjectSnapshot &snapshot) {
                         return snapshot.snapshotId == first->snapshotId;
                       }));
  QCOMPARE(duplicate->snapshotId, snapshots.constFirst().snapshotId);

  const QString restored =
      root.filePath(QStringLiteral("restored.gsw.json"));
  QVERIFY2(store.restoreProjectSnapshot(snapshots.constLast(), restored,
                                        &error),
           qPrintable(error));
  QFile restoredFile(restored);
  QVERIFY(restoredFile.open(QIODevice::ReadOnly));
  QCOMPARE(restoredFile.readAll(), QByteArray("{\"revision\":2}"));
  auto bad = snapshots.first();
  bad.snapshotId = QStringLiteral("wrong-id");
  QVERIFY(!store.discardProjectSnapshot(bad, projectData, &error));
  QVERIFY(!store.discardProjectSnapshot(snapshots.first(), temporary.path() + "/outside", &error));
  QVERIFY(store.discardProjectSnapshot(snapshots.first(), projectData, &error));
  QCOMPARE(store.projectSnapshots(projectData).size(), 1);
  QVERIFY(QFileInfo::exists(projectFile));
  QVERIFY(QFileInfo::exists(restored));
  QVERIFY(QFileInfo::exists(snapshots.last().snapshotPath));
}

void RecoveryStoreTests::rejectsUnsafeRecoveryDeletion() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  gsw::RecoveryStore store(QDir(temporary.path()).filePath("catalog"));
  QString error;
  const auto first = store.beginWorkspace("First", &error);
  const auto second = store.beginWorkspace("Second", &error);
  QVERIFY(first && second);
  auto mismatch = *first;
  mismatch.sessionId = second->sessionId;
  QVERIFY(!store.discardWorkspace(mismatch, &error));
  gsw::RecoveryStore outside(QDir(temporary.path()).filePath("outside"));
  QVERIFY(!outside.discardWorkspace(*first, &error));
  auto rootTarget = *first;
  rootTarget.rootPath = QDir(temporary.path()).filePath("catalog");
  QVERIFY(!store.discardWorkspace(rootTarget, &error));
  QCOMPARE(store.recoverableWorkspaces().size(), 2);
  QVERIFY(store.discardWorkspace(*first, &error));
  QCOMPARE(store.recoverableWorkspaces().size(), 1);
  QVERIFY(QFileInfo::exists(second->rootPath));
}

void RecoveryStoreTests::preservesViewportGridValuesWithoutLosingRecoveryModelState() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString base = QDir(temporary.path()).filePath("recovery");
  gsw::RecoveryStore store(base);
  QString error;
  const auto started = store.beginWorkspace("Grid recovery", &error);
  QVERIFY2(started.has_value(), qPrintable(error));
  auto checkpoint = *started;
  checkpoint.projectFilePath = QDir(checkpoint.rootPath).filePath("saved.gsw.json");
  checkpoint.datasetPath = QDir(checkpoint.rootPath).filePath("dataset");
  checkpoint.scenePath = QDir(checkpoint.rootPath).filePath("source.ply");
  checkpoint.sceneTranslation = {2.0F, -3.0F, 0.75F};
  checkpoint.sceneRotation = QQuaternion::fromAxisAndAngle({0.0F, 0.0F, 1.0F}, 42.0F);
  checkpoint.sceneScale = {1.25F, 0.8F, 2.0F};
  checkpoint.sceneCollection = {{"activeId", "retained-model"}, {"objects", QJsonArray{
      QJsonObject{{"id", "retained-model"}, {"path", checkpoint.scenePath}, {"visible", false}}}}};
  const QByteArray source("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n");
  QFile payload(checkpoint.scenePath);
  QVERIFY(payload.open(QIODevice::WriteOnly));
  QCOMPARE(payload.write(source), source.size());
  payload.close();
  const QJsonObject full{{"version", 1}, {"spacingMode", "fixed"}, {"majorSpacing", 2.5},
      {"subdivisions", 5}, {"density", 1.0}, {"displayUnit", "scene"},
      {"gridVisible", true}, {"axesVisible", true}};
  const QList<QJsonValue> values{full, QJsonObject{{"majorSpacing", 0.25}, {"gridVisible", false}},
      QJsonObject{}, QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("unexpected")),
      QJsonValue(42), QJsonValue(true), QJsonArray{1, 2},
      QJsonObject{{"version", 99}, {"density", "malformed"}}};
  for (const auto &value : values) {
    // Recovery is a lossless transport for the optional presentation value.
    // Its caller validates/warns; bad view preferences must not drop models.
    checkpoint.viewportGrid = value;
    QVERIFY2(store.checkpoint(checkpoint, &error), qPrintable(error));
    QFile state(QDir(checkpoint.rootPath).filePath(".gsw/recovery-state.json"));
    QVERIFY(state.open(QIODevice::ReadOnly));
    const auto serialized = QJsonDocument::fromJson(state.readAll()).object();
    state.close();
    QVERIFY(serialized.contains("viewportGrid"));
    QCOMPARE(serialized.value("viewportGrid"), value);
    const auto recovered = gsw::RecoveryStore(base).recoverableWorkspaces(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(recovered.size(), 1);
    const auto actual = recovered.constFirst();
    QCOMPARE(actual.viewportGrid, value);
    QCOMPARE(actual.sessionId, checkpoint.sessionId);
    QCOMPARE(actual.displayName, checkpoint.displayName);
    QCOMPARE(actual.rootPath, QDir::cleanPath(checkpoint.rootPath));
    QCOMPARE(actual.projectFilePath, QDir::cleanPath(checkpoint.projectFilePath));
    QCOMPARE(actual.datasetPath, QDir::cleanPath(checkpoint.datasetPath));
    QCOMPARE(actual.scenePath, QDir::cleanPath(checkpoint.scenePath));
    QCOMPARE(actual.sceneCollection, checkpoint.sceneCollection);
    QCOMPARE(actual.sceneTranslation, checkpoint.sceneTranslation);
    QCOMPARE(actual.sceneScale, checkpoint.sceneScale);
    QVERIFY(1.0F - std::abs(QQuaternion::dotProduct(actual.sceneRotation.normalized(),
        checkpoint.sceneRotation.normalized())) < 1.0e-6F);
    QVERIFY(actual.updatedUtc.isValid() && actual.updatedUtc >= started->updatedUtc);
    QVERIFY(payload.open(QIODevice::ReadOnly));
    QCOMPARE(payload.readAll(), source);
    payload.close();
  }
}

void RecoveryStoreTests::legacyRecoveryOmitsViewportGridUntilExplicitlyProvided() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString base = QDir(temporary.path()).filePath("legacy-recovery");
  gsw::RecoveryStore store(base);
  QString error;
  const auto started = store.beginWorkspace("Legacy view preferences", &error);
  QVERIFY2(started.has_value(), qPrintable(error));
  QVERIFY(started->viewportGrid.isUndefined());
  const QString statePath = QDir(started->rootPath).filePath(".gsw/recovery-state.json");
  QFile state(statePath);
  QVERIFY(state.open(QIODevice::ReadOnly));
  QVERIFY(!QJsonDocument::fromJson(state.readAll()).object().contains("viewportGrid"));
  state.close();
  auto recovered = store.recoverableWorkspaces(&error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(recovered.size(), 1);
  QVERIFY(recovered.constFirst().viewportGrid.isUndefined());
  auto checkpoint = recovered.constFirst();
  checkpoint.viewportGrid = QJsonObject{};
  QVERIFY(store.checkpoint(checkpoint, &error));
  recovered = store.recoverableWorkspaces(&error);
  QCOMPARE(recovered.size(), 1);
  QVERIFY(recovered.constFirst().viewportGrid.isObject());
  QVERIFY(recovered.constFirst().viewportGrid.toObject().isEmpty());
  checkpoint.viewportGrid = QJsonValue(QJsonValue::Undefined);
  QVERIFY(store.checkpoint(checkpoint, &error));
  QVERIFY(state.open(QIODevice::ReadOnly));
  QVERIFY(!QJsonDocument::fromJson(state.readAll()).object().contains("viewportGrid"));
  state.close();
  recovered = store.recoverableWorkspaces(&error);
  QCOMPARE(recovered.size(), 1);
  QVERIFY(recovered.constFirst().viewportGrid.isUndefined());
  QCOMPARE(recovered.constFirst().sessionId, started->sessionId);
  QCOMPARE(recovered.constFirst().rootPath, QDir::cleanPath(started->rootPath));
}

QTEST_GUILESS_MAIN(RecoveryStoreTests)
#include "RecoveryStoreTests.moc"
