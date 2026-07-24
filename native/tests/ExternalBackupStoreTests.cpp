#include "ExternalBackupStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class ExternalBackupStoreTests final : public QObject {
  Q_OBJECT

private slots:
  void deduplicatesAndRestoresProjectBackups();
  void restoresExternallyLinkedDatasetAndSceneIntoManagedData();
};

void ExternalBackupStoreTests::deduplicatesAndRestoresProjectBackups() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("source/project.files/images")));
  QVERIFY(root.mkpath(QStringLiteral("backup")));
  const QString projectFile =
      root.filePath(QStringLiteral("source/project.gsw.json"));
  const QString dataRoot =
      root.filePath(QStringLiteral("source/project.files"));
  QFile manifest(projectFile);
  QVERIFY(manifest.open(QIODevice::WriteOnly));
  QCOMPARE(manifest.write("{\"schemaVersion\":1}"), qint64(19));
  manifest.close();
  QFile image(
      QDir(dataRoot).filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("same-image"), qint64(10));
  image.close();

  gsw::ExternalBackupStore store(
      root.filePath(QStringLiteral("backup")));
  QString error;
  const std::optional<gsw::ExternalBackupSnapshot> first =
      store.backupProject(projectFile, dataRoot, &error);
  QVERIFY2(first.has_value(), qPrintable(error));
  QCOMPARE(first->fileCount, qsizetype(2));

  QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QCOMPARE(manifest.write("{\"schemaVersion\":2}"), qint64(19));
  manifest.close();
  const std::optional<gsw::ExternalBackupSnapshot> second =
      store.backupProject(projectFile, dataRoot, &error);
  QVERIFY2(second.has_value(), qPrintable(error));

  qsizetype objectCount = 0;
  QDirIterator objects(store.rootPath(),
                       {QStringLiteral("*.object")},
                       QDir::Files, QDirIterator::Subdirectories);
  while (objects.hasNext()) {
    objects.next();
    ++objectCount;
  }
  QCOMPARE(objectCount, qsizetype(3));
  QCOMPARE(store.snapshots(&error).size(), 2);

  const QString restoredRoot =
      root.filePath(QStringLiteral("restored"));
  QVERIFY2(store.restore(*second, restoredRoot, &error),
           qPrintable(error));
  QFile restoredManifest(
      QDir(restoredRoot).filePath(QStringLiteral("project.gsw.json")));
  QVERIFY(restoredManifest.open(QIODevice::ReadOnly));
  const QJsonObject restoredProject =
      QJsonDocument::fromJson(restoredManifest.readAll()).object();
  QCOMPARE(restoredProject.value(QStringLiteral("schemaVersion")).toInt(), 2);
  QCOMPARE(restoredProject.value(QStringLiteral("rootPath")).toString(),
           QStringLiteral("project.files"));
  QFile restoredImage(QDir(restoredRoot).filePath(
      QStringLiteral("project.files/images/frame.jpg")));
  QVERIFY(restoredImage.open(QIODevice::ReadOnly));
  QCOMPARE(restoredImage.readAll(), QByteArray("same-image"));
}

void ExternalBackupStoreTests::
    restoresExternallyLinkedDatasetAndSceneIntoManagedData() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("source/project.files")));
  QVERIFY(root.mkpath(QStringLiteral("external-dataset/images")));
  QVERIFY(root.mkpath(QStringLiteral("external-scene")));
  QVERIFY(root.mkpath(QStringLiteral("backup")));
  const QString projectFile =
      root.filePath(QStringLiteral("source/project.gsw.json"));
  const QString dataRoot =
      root.filePath(QStringLiteral("source/project.files"));
  const QString dataset =
      root.filePath(QStringLiteral("external-dataset"));
  const QString scene =
      root.filePath(QStringLiteral("external-scene/scene.ply"));
  QFile manifest(projectFile);
  QVERIFY(manifest.open(QIODevice::WriteOnly));
  manifest.write(QJsonDocument(QJsonObject{
                   {QStringLiteral("schemaVersion"), 1},
                   {QStringLiteral("rootPath"),
                    QStringLiteral("project.files")},
                   {QStringLiteral("datasetPath"), dataset},
                   {QStringLiteral("scenePath"), scene}})
                   .toJson(QJsonDocument::Compact));
  manifest.close();
  QFile image(QDir(dataset).filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("external-image"), qint64(14));
  image.close();
  QFile sceneFile(scene);
  QVERIFY(sceneFile.open(QIODevice::WriteOnly));
  QCOMPARE(sceneFile.write("external-scene"), qint64(14));
  sceneFile.close();

  gsw::ExternalBackupStore store(
      root.filePath(QStringLiteral("backup")));
  QString error;
  const std::optional<gsw::ExternalBackupSnapshot> snapshot =
      store.backupProject(projectFile, dataRoot, &error, dataset, scene);
  QVERIFY2(snapshot.has_value(), qPrintable(error));
  const QString restoredRoot =
      root.filePath(QStringLiteral("restored"));
  QVERIFY2(store.restore(*snapshot, restoredRoot, &error),
           qPrintable(error));

  QVERIFY(QFileInfo::exists(QDir(restoredRoot).filePath(
      QStringLiteral(
          "project.files/external/dataset/images/frame.jpg"))));
  QVERIFY(QFileInfo::exists(QDir(restoredRoot).filePath(
      QStringLiteral(
          "project.files/external/scene/scene.ply"))));
  QFile restoredProject(
      QDir(restoredRoot).filePath(QStringLiteral("project.gsw.json")));
  QVERIFY(restoredProject.open(QIODevice::ReadOnly));
  const QJsonObject project =
      QJsonDocument::fromJson(restoredProject.readAll()).object();
  QCOMPARE(project.value(QStringLiteral("datasetPath")).toString(),
           QStringLiteral("external/dataset"));
  QCOMPARE(project.value(QStringLiteral("scenePath")).toString(),
           QStringLiteral("external/scene/scene.ply"));
}

QTEST_GUILESS_MAIN(ExternalBackupStoreTests)
#include "ExternalBackupStoreTests.moc"
