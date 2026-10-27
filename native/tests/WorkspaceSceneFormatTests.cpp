#include "WorkspaceDocument.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

class WorkspaceSceneFormatTests final : public QObject {
  Q_OBJECT

private slots:
  void acceptsBigEndianPlySceneMetadata();
  void countsManagedImagesWithoutCountingArchivedOriginals();
  void fallsBackToInputWhenImagesDirectoryIsEmpty();
  void prefersInputWhenBothStandardDirectoriesContainImages();
  void rejectsNestedOnlyImagesThatTrainingCannotConsume();
  void persistsPerObjectVisibilityWithoutChangingModelData();
  void oldSceneCollectionsDefaultToVisible();
  void preservesOptionalViewportGridWithoutChangingSceneTransforms();
};

void WorkspaceSceneFormatTests::acceptsBigEndianPlySceneMetadata() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("big-endian.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_big_endian 1.0\n"
            "element vertex 1\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "element face 1\n"
            "property list uchar int vertex_indices\n"
            "end_header\n");
  ply.close();

  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.create(temporary.path(), &error));
  QVERIFY2(document.setScenePath(plyPath, &error), qPrintable(error));
  QCOMPARE(document.scenePath(), QDir::cleanPath(plyPath));
  QCOMPARE(document.sceneMetadata().format,
           QStringLiteral("binary_big_endian"));
  QCOMPARE(document.sceneMetadata().vertexCount, 1);
  QCOMPARE(document.sceneMetadata().faceCount, 1);
  QVERIFY(document.sceneMetadata().looksLikeMesh());
}

void WorkspaceSceneFormatTests::countsManagedImagesWithoutCountingArchivedOriginals() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images")));
  QVERIFY(dataset.mkpath(QStringLiteral("source/originals")));

  QFile trainingImage(dataset.filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(trainingImage.open(QIODevice::WriteOnly));
  QCOMPARE(trainingImage.write("training"), qint64(8));
  trainingImage.close();

  QFile archivedOriginal(
      dataset.filePath(QStringLiteral("source/originals/frame.jpg")));
  QVERIFY(archivedOriginal.open(QIODevice::WriteOnly));
  QCOMPARE(archivedOriginal.write("original"), qint64(8));
  archivedOriginal.close();

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(1));
}

void WorkspaceSceneFormatTests::fallsBackToInputWhenImagesDirectoryIsEmpty() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images")));
  QVERIFY(dataset.mkpath(QStringLiteral("input")));

  QFile inputImage(dataset.filePath(QStringLiteral("input/frame.jpg")));
  QVERIFY(inputImage.open(QIODevice::WriteOnly));
  QCOMPARE(inputImage.write("input"), qint64(5));
  inputImage.close();

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(1));
}

void WorkspaceSceneFormatTests::prefersInputWhenBothStandardDirectoriesContainImages() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images")));
  QVERIFY(dataset.mkpath(QStringLiteral("input")));

  QFile imagesFrame(dataset.filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(imagesFrame.open(QIODevice::WriteOnly));
  QCOMPARE(imagesFrame.write("images"), qint64(6));
  imagesFrame.close();
  for (const QString &name : {QStringLiteral("first.jpg"),
                              QStringLiteral("second.png")}) {
    QFile inputFrame(dataset.filePath(QStringLiteral("input/") + name));
    QVERIFY(inputFrame.open(QIODevice::WriteOnly));
    QCOMPARE(inputFrame.write("input"), qint64(5));
  }

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(2));
}

void WorkspaceSceneFormatTests::rejectsNestedOnlyImagesThatTrainingCannotConsume() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images/nested")));

  QFile nestedFrame(dataset.filePath(QStringLiteral("images/nested/frame.jpg")));
  QVERIFY(nestedFrame.open(QIODevice::WriteOnly));
  QCOMPARE(nestedFrame.write("nested"), qint64(6));
  nestedFrame.close();

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(0));
}

void WorkspaceSceneFormatTests::persistsPerObjectVisibilityWithoutChangingModelData() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString working = QDir(temporary.path()).filePath("working");
  QVERIFY(QDir().mkpath(working));
  const QString path = QDir(temporary.path()).filePath("source.ply");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  const QByteArray source("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n");
  QCOMPARE(file.write(source), source.size());
  file.close();
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(working, "Visibility", &error), qPrintable(error));
  QVERIFY2(document.addScenePath(path, &error), qPrintable(error));
  const QString first = document.activeSceneId();
  QVERIFY2(document.setSceneTranslation({1, 2, 3}, &error), qPrintable(error));
  QVERIFY2(document.addScenePath(path, &error), qPrintable(error));
  const QString second = document.activeSceneId();
  const auto before = document.sceneObjects();
  QVERIFY(document.setSceneObjectVisibility({first, second}, false));
  QCOMPARE(document.activeSceneId(), second);
  QCOMPARE(document.sceneObjects().size(), 2);
  QVERIFY(!document.sceneObjects()[0].visible);
  QVERIFY(!document.sceneObjects()[1].visible);
  const auto hidden = document.sceneCollectionJson();
  QVERIFY(!document.setSceneObjectVisibility({first, "unknown-scene"}, true));
  QCOMPARE(document.sceneCollectionJson(), hidden);
  const auto recovery = QJsonDocument::fromJson(document.recoveryManifestJson()).object();
  QCOMPARE(recovery.value("sceneCollection").toObject(), hidden);
  const QString manifest = QDir(temporary.path()).filePath("visibility.gsw.json");
  QVERIFY2(document.saveManifest(manifest, &error), qPrintable(error));
  QVERIFY(!document.isModified());
  QVERIFY(document.setSceneObjectVisibility({first}, false));
  QVERIFY(!document.isModified());
  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(manifest, &error), qPrintable(error));
  QCOMPARE(restored.activeSceneId(), second);
  QCOMPARE(restored.sceneObjects().size(), 2);
  const auto restoredObjects = restored.sceneObjects();
  for (int index = 0; index < 2; ++index) {
    const auto &actual = restoredObjects[index];
    QCOMPARE(actual.id, before[index].id);
    QCOMPARE(actual.path, before[index].path);
    QCOMPARE(actual.translation, before[index].translation);
    QCOMPARE(actual.rotation, before[index].rotation);
    QCOMPARE(actual.scale, before[index].scale);
    QVERIFY(!actual.visible);
  }
  QVERIFY(restored.setSceneObjectVisibility({first}, true));
  QVERIFY(restored.sceneObjects()[0].visible);
  QVERIFY(!restored.sceneObjects()[1].visible);
  const auto partial = restored.sceneCollectionJson();
  QVERIFY(!restored.isolateSceneObjects({second, "unknown-scene"}));
  QCOMPARE(restored.sceneCollectionJson(), partial);
  QVERIFY(restored.isolateSceneObjects({second}));
  QVERIFY(!restored.sceneObjects()[0].visible);
  QVERIFY(restored.sceneObjects()[1].visible);
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), source);
}

void WorkspaceSceneFormatTests::oldSceneCollectionsDefaultToVisible() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString path = QDir(temporary.path()).filePath("legacy.ply");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n");
  file.close();
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.create(temporary.path(), &error));
  QVERIFY(document.addScenePath(path, &error));
  auto legacy = document.sceneCollectionJson();
  QJsonArray objects;
  for (const auto &value : legacy.value("objects").toArray()) {
    auto object = value.toObject();
    object.remove("visible");
    objects.append(object);
  }
  legacy.insert("objects", objects);
  QVERIFY(document.setSceneObjectVisibility({document.activeSceneId()}, false));
  document.restoreSceneCollection(legacy);
  QCOMPARE(document.sceneObjects().size(), 1);
  QVERIFY(document.sceneObjects()[0].visible);
}

void WorkspaceSceneFormatTests::preservesOptionalViewportGridWithoutChangingSceneTransforms() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString working = QDir(temporary.path()).filePath("working");
  QVERIFY(QDir().mkpath(working));
  const QString path = QDir(temporary.path()).filePath("grid-reference.ply");
  const QByteArray source("ply\nformat ascii 1.0\nelement vertex 2\nproperty double x\nproperty double y\nproperty double z\nend_header\n0.25 -0.5 1.5\n2.75 1.25 4.0\n");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(source), source.size());
  file.close();
  gsw::WorkspaceDocument original;
  QString error;
  QVERIFY2(original.createUntitled(working, "Project grid settings", &error), qPrintable(error));
  QVERIFY2(original.addScenePath(path, &error), qPrintable(error));
  QVERIFY2(original.setSceneTransform({0.25F, -0.5F, 1.0F},
      QQuaternion::fromEulerAngles(7.0F, 11.0F, 13.0F), {1.1F, 0.9F, 1.2F}, &error), qPrintable(error));
  const QString manifest = QDir(temporary.path()).filePath("grid.gsw.json");
  QVERIFY2(original.saveManifest(manifest, &error), qPrintable(error));
  QFile manifestFile(manifest);
  QVERIFY(manifestFile.open(QIODevice::ReadOnly));
  auto input = QJsonDocument::fromJson(manifestFile.readAll()).object();
  manifestFile.close();
  QVERIFY(!input.isEmpty());
  const QJsonObject grid{{"version", 1}, {"spacingMode", "fixed"}, {"majorSpacing", 1.25},
      {"subdivisions", 5}, {"density", 1.0}, {"displayUnit", "scene"},
      {"gridVisible", true}, {"axesVisible", true}};
  input.insert("viewportGrid", grid);
  QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
  const QByteArray inputBytes = QJsonDocument(input).toJson(QJsonDocument::Indented);
  QCOMPARE(manifestFile.write(inputBytes), inputBytes.size());
  manifestFile.close();

  // Exercise the existing public project loader and persistence channels.
  // No new getter, private member, rendering hook or model mutation is used
  // to manufacture a red result: an older loader naturally drops this field.
  gsw::WorkspaceDocument loaded;
  QVERIFY2(loaded.load(manifest, &error), qPrintable(error));
  const auto collection = loaded.sceneCollectionJson();
  const auto translation = loaded.sceneTranslation();
  const auto rotation = loaded.sceneRotation();
  const auto scale = loaded.sceneScale();
  const auto recovery = QJsonDocument::fromJson(loaded.recoveryManifestJson()).object();
  QCOMPARE(recovery.value("viewportGrid").toObject(), grid);
  QCOMPARE(recovery.value("sceneCollection").toObject(), collection);
  QVERIFY2(loaded.saveManifest({}, &error), qPrintable(error));
  QVERIFY(manifestFile.open(QIODevice::ReadOnly));
  const auto saved = QJsonDocument::fromJson(manifestFile.readAll()).object();
  manifestFile.close();
  QCOMPARE(saved.value("viewportGrid").toObject(), grid);
  QCOMPARE(loaded.sceneCollectionJson(), collection);
  QCOMPARE(loaded.sceneTranslation(), translation);
  QCOMPARE(loaded.sceneRotation(), rotation);
  QCOMPARE(loaded.sceneScale(), scale);
  gsw::WorkspaceDocument reopened;
  QVERIFY2(reopened.load(manifest, &error), qPrintable(error));
  QCOMPARE(QJsonDocument::fromJson(reopened.recoveryManifestJson()).object().value("viewportGrid").toObject(), grid);
  QCOMPARE(reopened.sceneCollectionJson(), collection);
  QCOMPARE(reopened.sceneTranslation(), translation);
  QCOMPARE(reopened.sceneRotation(), rotation);
  QCOMPARE(reopened.sceneScale(), scale);
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), source);
}

QTEST_GUILESS_MAIN(WorkspaceSceneFormatTests)

#include "WorkspaceSceneFormatTests.moc"
