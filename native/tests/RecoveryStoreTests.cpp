#include "RecoveryStore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

class RecoveryStoreTests final : public QObject {
  Q_OBJECT

private slots:
  void recoversAndDiscardsAnUntitledWorkspace();
  void removesRecoveryMetadataWhenWorkspaceBecomesManaged();
  void retainsAndRestoresProjectSnapshots();
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
  QVERIFY2(store.checkpoint(checkpoint, &error), qPrintable(error));

  const QList<gsw::RecoveryWorkspace> recovered =
      gsw::RecoveryStore(workspaceBase).recoverableWorkspaces(&error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(recovered.size(), 1);
  QCOMPARE(recovered.constFirst().sessionId, started->sessionId);
  QCOMPARE(recovered.constFirst().displayName, QStringLiteral("现场重建"));
  QCOMPARE(recovered.constFirst().datasetPath,
           QDir::cleanPath(checkpoint.datasetPath));
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
}

QTEST_GUILESS_MAIN(RecoveryStoreTests)
#include "RecoveryStoreTests.moc"
