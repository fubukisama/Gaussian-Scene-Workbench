#include "WorkspaceDocument.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
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
  void rejectsInvalidViewportGridBeforeReplacingTheCurrentProject();
  void updatesViewportGridWithoutReloadingSceneState();
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

void WorkspaceSceneFormatTests::rejectsInvalidViewportGridBeforeReplacingTheCurrentProject() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString working = QDir(temporary.path()).filePath("working");
  const QString rejectedRoot = QDir(temporary.path()).filePath("different-project");
  QVERIFY(QDir().mkpath(working));
  QVERIFY(QDir().mkpath(rejectedRoot));
  const QString path = QDir(temporary.path()).filePath("atomic-source.ply");
  const QByteArray source("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0.5 1.25 -2\n");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(source), source.size());
  file.close();
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(working, "Keep current project", &error), qPrintable(error));
  QVERIFY2(document.addScenePath(path, &error), qPrintable(error));
  QVERIFY2(document.setSceneTransform({0.5F, -0.75F, 1.25F},
      QQuaternion::fromEulerAngles(4.0F, 9.0F, 12.0F), {1.2F, 0.8F, 1.1F}, &error), qPrintable(error));
  const QJsonObject settings{{"spacingMode", "fixed"}, {"majorSpacing", 0.75}};
  QVERIFY2(document.setViewportGridSettings(settings, &error), qPrintable(error));
  const QString manifest = QDir(temporary.path()).filePath("current.gsw.json");
  QVERIFY2(document.saveManifest(manifest, &error), qPrintable(error));
  QFile currentManifest(manifest);
  QVERIFY(currentManifest.open(QIODevice::ReadOnly));
  const QByteArray manifestBytes = currentManifest.readAll();
  currentManifest.close();
  const auto baseline = document.sceneCollectionJson();
  const auto rootPath = document.rootPath();
  const auto filePath = document.projectFilePath();
  const auto name = document.projectName();
  const auto camera = document.cameraDatasetInfo();
  const auto artifacts = document.workflowArtifacts();
  const auto pendingMigration = document.hasPendingDataMigration();
  QVERIFY(!document.isModified());
  QSignalSpy changed(&document, &gsw::WorkspaceDocument::changed);
  QSignalSpy modified(&document, &gsw::WorkspaceDocument::modifiedChanged);
  QSignalSpy gridChanged(&document, &gsw::WorkspaceDocument::viewportGridSettingsChanged);
  QList<QJsonValue> invalid{QJsonValue(QJsonValue::Null), QJsonValue(true), QJsonValue(1),
      QJsonValue(QStringLiteral("fixed")), QJsonValue(QJsonArray{})};
  const auto invalidField = [&](const QString &key, const QJsonValue &value) {
    auto object = settings;
    object.insert(key, value);
    invalid.append(object);
  };
  invalidField("version", 2);
  invalidField("version", "1");
  invalidField("spacingMode", "auto");
  invalidField("displayUnit", "feet");
  invalidField("majorSpacing", 0);
  invalidField("majorSpacing", -1);
  invalidField("majorSpacing", "1.25");
  invalidField("subdivisions", 3);
  invalidField("subdivisions", 5.5);
  invalidField("density", 0.24);
  invalidField("density", 4.01);
  invalidField("gridVisible", 1);
  invalidField("axesVisible", "true");
  invalidField("physicalScale", 0.05);
  int index = 0;
  for (const auto &value : invalid) {
    auto rejected = QJsonDocument::fromJson(manifestBytes).object();
    rejected.insert("projectName", "Must not replace the current project");
    rejected.insert("rootPath", rejectedRoot);
    rejected.insert("viewportGrid", value);
    const QString rejectedPath = QDir(temporary.path()).filePath(QString("invalid-%1.gsw.json").arg(index++));
    QFile rejectedFile(rejectedPath);
    QVERIFY(rejectedFile.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(rejected).toJson(QJsonDocument::Indented);
    QCOMPARE(rejectedFile.write(bytes), bytes.size());
    rejectedFile.close();
    error.clear();
    QVERIFY(!document.load(rejectedPath, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(document.viewportGridSettings(), settings);
    QCOMPARE(document.sceneCollectionJson(), baseline);
    QCOMPARE(document.rootPath(), rootPath);
    QCOMPARE(document.projectFilePath(), filePath);
    QCOMPARE(document.projectName(), name);
    QCOMPARE(document.cameraDatasetInfo(), camera);
    QCOMPARE(document.workflowArtifacts(), artifacts);
    QCOMPARE(document.hasPendingDataMigration(), pendingMigration);
    QVERIFY(!document.isModified());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(modified.count(), 0);
    QCOMPARE(gridChanged.count(), 0);
  }
  QVERIFY(currentManifest.open(QIODevice::ReadOnly));
  QCOMPARE(currentManifest.readAll(), manifestBytes);
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), source);
}

void WorkspaceSceneFormatTests::updatesViewportGridWithoutReloadingSceneState() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString working = QDir(temporary.path()).filePath("working");
  QVERIFY(QDir().mkpath(working));
  const QString path = QDir(temporary.path()).filePath("presentation-source.ply");
  const QByteArray source("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1.5 -0.75 2.25\n");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(source), source.size());
  file.close();
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(working, "Display only", &error), qPrintable(error));
  QVERIFY2(document.addScenePath(path, &error), qPrintable(error));
  QVERIFY2(document.setSceneTranslation({0.5F, 0.75F, -1.0F}, &error), qPrintable(error));
  const QString manifest = QDir(temporary.path()).filePath("display.gsw.json");
  QVERIFY2(document.saveManifest(manifest, &error), qPrintable(error));
  const auto collection = document.sceneCollectionJson();
  const auto camera = document.cameraDatasetInfo();
  QVERIFY(document.viewportGridSettings().isEmpty());
  QVERIFY(!QJsonDocument::fromJson(document.recoveryManifestJson()).object().contains("viewportGrid"));
  QSignalSpy changed(&document, &gsw::WorkspaceDocument::changed);
  QSignalSpy modified(&document, &gsw::WorkspaceDocument::modifiedChanged);
  QSignalSpy gridChanged(&document, &gsw::WorkspaceDocument::viewportGridSettingsChanged);
  // Valid partial objects are stored exactly, not silently canonicalised to
  // a full defaults object or mixed into the scene's physical transform.
  const QJsonObject settings{{"spacingMode", "fixed"}, {"majorSpacing", 2.5}, {"gridVisible", false}};
  QVERIFY2(document.setViewportGridSettings(settings, &error), qPrintable(error));
  QCOMPARE(document.viewportGridSettings(), settings);
  QCOMPARE(QJsonDocument::fromJson(document.recoveryManifestJson()).object().value("viewportGrid").toObject(), settings);
  QVERIFY(document.isModified());
  QCOMPARE(changed.count(), 0);
  QCOMPARE(modified.count(), 1);
  QCOMPARE(gridChanged.count(), 1);
  QCOMPARE(document.sceneCollectionJson(), collection);
  QCOMPARE(document.cameraDatasetInfo(), camera);
  QVERIFY(document.setViewportGridSettings(settings, &error));
  QCOMPARE(changed.count(), 0);
  QCOMPARE(modified.count(), 1);
  QCOMPARE(gridChanged.count(), 1);
  QVERIFY(!document.setViewportGridSettings({{"majorSpacing", 0}}, &error));
  QVERIFY(!error.isEmpty());
  QCOMPARE(document.viewportGridSettings(), settings);
  QCOMPARE(changed.count(), 0);
  QCOMPARE(modified.count(), 1);
  QCOMPARE(gridChanged.count(), 1);
  QVERIFY2(document.save({}, &error), qPrintable(error));
  QFile manifestFile(manifest);
  QVERIFY(manifestFile.open(QIODevice::ReadOnly));
  const auto saved = QJsonDocument::fromJson(manifestFile.readAll()).object();
  manifestFile.close();
  QCOMPARE(saved.value("viewportGrid").toObject(), settings);
  QCOMPARE(document.sceneCollectionJson(), collection);
  QCOMPARE(document.cameraDatasetInfo(), camera);
  QVERIFY(document.setViewportGridSettings({}, &error));
  const auto explicitEmpty = QJsonDocument::fromJson(document.recoveryManifestJson()).object();
  QVERIFY(explicitEmpty.contains("viewportGrid"));
  QVERIFY(explicitEmpty.value("viewportGrid").isObject());
  QVERIFY(explicitEmpty.value("viewportGrid").toObject().isEmpty());
  QVERIFY2(document.saveManifest({}, &error), qPrintable(error));
  gsw::WorkspaceDocument emptyRestored;
  QVERIFY2(emptyRestored.load(manifest, &error), qPrintable(error));
  QVERIFY(QJsonDocument::fromJson(emptyRestored.recoveryManifestJson()).object().contains("viewportGrid"));
  auto legacy = saved;
  legacy.remove("viewportGrid");
  const QString legacyPath = QDir(temporary.path()).filePath("legacy-grid.gsw.json");
  QFile legacyFile(legacyPath);
  QVERIFY(legacyFile.open(QIODevice::WriteOnly));
  const QByteArray legacyBytes = QJsonDocument(legacy).toJson(QJsonDocument::Indented);
  QCOMPARE(legacyFile.write(legacyBytes), legacyBytes.size());
  legacyFile.close();
  QVERIFY(document.setViewportGridSettings(settings, &error));
  QVERIFY2(document.load(legacyPath, &error), qPrintable(error));
  QVERIFY(document.viewportGridSettings().isEmpty());
  QVERIFY(!QJsonDocument::fromJson(document.recoveryManifestJson()).object().contains("viewportGrid"));
  QCOMPARE(document.sceneCollectionJson(), collection);
  QVERIFY2(document.saveManifest({}, &error), qPrintable(error));
  QVERIFY(legacyFile.open(QIODevice::ReadOnly));
  QVERIFY(!QJsonDocument::fromJson(legacyFile.readAll()).object().contains("viewportGrid"));
  legacyFile.close();
  QVERIFY(document.setViewportGridSettings(settings, &error));
  QVERIFY(document.create(working, &error));
  QVERIFY(document.viewportGridSettings().isEmpty());
  QVERIFY(!QJsonDocument::fromJson(document.recoveryManifestJson()).object().contains("viewportGrid"));
  QVERIFY(document.setViewportGridSettings(settings, &error));
  QVERIFY(document.createUntitled(working, "New presentation project", &error));
  QVERIFY(document.viewportGridSettings().isEmpty());
  QVERIFY(!QJsonDocument::fromJson(document.recoveryManifestJson()).object().contains("viewportGrid"));
  QVERIFY(file.open(QIODevice::ReadOnly));
  QCOMPARE(file.readAll(), source);
}

QTEST_GUILESS_MAIN(WorkspaceSceneFormatTests)

#include "WorkspaceSceneFormatTests.moc"
