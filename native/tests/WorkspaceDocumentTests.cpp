#include "CameraTrajectory.h"
#include "CameraDatasetInfo.h"
#include "ColmapSupport.h"
#include "PlyPointCloudLoader.h"
#include "ResourceBudget.h"
#include "SceneEditModel.h"
#include "ScreenSpaceSelection.h"
#include "TrainingOutputLocator.h"
#include "WorkspaceDocument.h"

#include <QBitArray>
#include <QtEndian>

#include <QDir>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <bit>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class WorkspaceDocumentTests final : public QObject {
  Q_OBJECT

private slots:
  void parsesGaussianPlyHeader();
  void removesMultipleModelsWithoutDeletingFiles();
  void loadsAsciiPointColorsAndSamplesDeterministically();
  void loadsOversizedBinaryPointCloudAtFullResolution();
  void diagnosticPointPagingIsOptIn();
  void diagnosticPagingDoesNotReplaceGaussianOrSurfelAttributes_data();
  void diagnosticPagingDoesNotReplaceGaussianOrSurfelAttributes();
  void loadsOversizedAsciiPointCloudIntoDiskCache();
  void preservesGlobalCoordinatesWhilePagingLocalDisplayData();
  void parsesUnitsAndExportsCoordinateReport();
  void recognizesSingularDeclaredLengthUnits_data();
  void recognizesSingularDeclaredLengthUnits();
  void buildsAndInvalidatesDiskResidentPointOctree();
  void loadsAsciiPolygonMeshAndTriangulates();
  void loadsAdaptiveAsciiMeshEfficientlyAndPreservesChunkedRecords();
  void loadsBinaryBigEndianMesh();
  void loadsOversizedAsciiMeshIntoDiskCache();
  void preservesResidentMeshUvSeams();
  void loadsModelScaleVersionWithElementLocalTextureAndRewrittenUnits();
  void buildsPagesAndInvalidatesDiskResidentMesh();
  void loadsConfiguredRealMeshFixture();
  void loadsConfiguredRealCoordinateFixture();
  void loadsBinaryGaussianSphericalHarmonicColors();
  void retainsFullSphericalHarmonics();
  void activatesGaussianScaleRotationAndOpacity();
  void loadsTwoScaleSurfelsWithoutChangingSource();
  void detectsColmapDatasetLayoutAndExecutable();
  void selectsNewestVersionedColmapExecutable();
  void locatesVersionedColmapOnRepositoryVolume();
  void locatesNewestCompletedTrainingScene();
  void persistsActiveTrainingJobForCrashRecovery();
  void portableTrainingResumeRecordAndManifest();
  void detectsCompleteAndIncompleteColmapModels();
  void selectsBrushStrokeAndHonorsVisibility();
  void tracksSelectionDeletionUndoAndRedo();
  void exportsFilteredAsciiPlyWithOriginalFields();
  void exportsFilteredBinaryPlyWithoutReencoding();
  void createsUntitledProjectWithoutAProjectFile();
  void preservesIndependentSceneObjectsAcrossImportAndSave();
  void publishesGeneratedSceneWithoutReplacingReferences();
  void rejectsInvalidGeneratedSceneWithoutChangingTheCollection();
  void commitsObjectTransformsAtomically();
  void serializesCurrentUnsavedStateForRecovery();
  void savesRecoverableManifestWithoutMovingActiveWorkspace();
  void finalizesPendingMigrationWithDataWrittenAfterManifestSave();
  void resumesMigrationAfterDataDirectoryWasPublished();
  void savesAndLoadsPortableProject();
  void persistsCanonicalCameraIdentityAndUnitsAcrossSaveAs();
  void countsOnlyCanonicalViewsAndRejectsChangedSummaryIdentity();
  void refusesChangedInitializerVersionBeforeDatasetAssociation();
  void registersPortableWorkflowArtifactsWithoutChangingSceneOrStartingWork();
  void persistsWorkflowArtifactsAcrossRecoveryMigrationAndSaveAs();
  void rejectsUnsafeWorkflowArtifactsAndRetainsMissingReferences();
  void refusesWorkflowJunctionsWithoutFollowingExternalPayloads();
  void rejectsCanonicalMeshBackendsBeforeChangingTheScene();
  void saveAsCopiesManagedDataIntoANewLinkedDirectory();
  void firstSaveKeepsExternalDatasetAsALink();
  void clearsManagedDatasetAndPersistsSceneAssociation();
  void clearsExternalDatasetWithoutDeletingSourceFiles();
  void clearsSceneAssociationWithoutDeletingSceneFile();
  void rollsBackManagedCleanupWhenManifestSaveFails();
  void clearsOnlyManagedReconstructionData();
  void refusesToModifyExternalReconstructionData();
  void loadsStandardCameraSidecarAndBuildsLegacyAxes();
  void retainsSourceCameraCalibrationAndLegacyDefaults();
  void skipsMalformedCameraEntries();
  void reportsMalformedCameraDocument();
  void treatsMissingCameraSidecarAsOptional();
  void reloadsCameraSidecarAfterRepair();
  void decimatesLargeCameraVisualization();
};

void WorkspaceDocumentTests::retainsFullSphericalHarmonics() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  for (int format = 0; format < 3; ++format) {
    for (int degree = 0; degree <= 4; ++degree) {
      const int rest = (degree + 1) * (degree + 1) - 1;
      const auto path = QDir(temporary.path()).filePath("sh.ply");
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QByteArray header = "ply\nformat " + QByteArray(format == 0 ? "ascii" :
          format == 1 ? "binary_little_endian" : "binary_big_endian") + " 1.0\nelement vertex 7\n";
      // RGB coexisting with f_dc must not suppress SH, and shuffled property
      // order must not be mistaken for the conventional training layout.
      for (const char *name : {"x", "y", "z", "red", "green", "blue", "f_dc_2", "f_dc_0", "f_dc_1",
              "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
        header += QByteArray("property float ") + name + '\n';
      for (int i = rest * 3 - 1; i >= 0; --i)
        header += "property float f_rest_" + QByteArray::number(i) + '\n';
      file.write(header + "end_header\n");
      QDataStream stream(&file);
      stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
      stream.setByteOrder(format == 2 ? QDataStream::BigEndian : QDataStream::LittleEndian);
      for (int p = 0; p < 7; ++p) {
        QVector<float> values{float(p), 1, 2, 128, 200, 255, 3, 1, 2, 4, -2, -2, -2, 1, 0, 0, 0};
        for (int i = rest * 3 - 1; i >= 0; --i) values.append(float(p * 100 + i) / 100.0F);
        for (float value : values) {
          if (format == 0) file.write(QByteArray::number(value, 'g', 9) + ' ');
          else stream << value;
        }
        if (format == 0) file.write("\n");
      }
      file.close();
      for (int sampleCount : {3, 7}) {
        const auto data = gsw::PlyPointCloudLoader::load(path, sampleCount);
        QVERIFY2(data.isValid(), qPrintable(data.error));
        const auto &sh = data.sphericalHarmonics;
        QCOMPARE(sh.degree, degree);
        QCOMPARE(sh.sourceIndices.size(), data.vertices.size());
        QCOMPARE(sh.coefficients.size(), data.vertices.size() * (rest + 1) * 3);
        for (qsizetype p = 0; p < data.vertices.size(); ++p) {
          QCOMPARE(sh.sourceIndices[p], data.vertices[p].sourceIndex);
          for (int k = 0; k <= rest; ++k)
            for (int c = 0; c < 3; ++c) {
              const float expected = k == 0 ? float(c + 1) :
                  float(sh.sourceIndices[p] * 100 + c * rest + k - 1) / 100.0F;
              QCOMPARE(sh.coefficients[(p * (rest + 1) + k) * 3 + c], expected);
            }
        }
      }
    }
  }
  // Invalid/duplicate fields and non-finite coefficients fall back to DC,
  // preserving usable geometry. NaN coordinates must not shift SH indices.
  const QByteArray base = "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\n"
      "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\n"
      "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
      "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
  for (int variant = 0; variant < 4; ++variant) {
    QFile file(QDir(temporary.path()).filePath("invalid.ply"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(base);
    if (variant == 0) file.write("property float f_rest_0\n");
    if (variant == 1) file.write("property float f_dc_0\n");
    file.write("end_header\n");
    file.write(variant == 3 ? "nan 0 0 " : "0 0 0 ");
    file.write(variant == 2 ? "nan 0 0 " : "0 0 0 ");
    file.write("1 -2 -2 -2 1 0 0 0");
    if (variant < 2) file.write(" 0");
    file.write("\n1 0 0 2 0 0 1 -2 -2 -2 1 0 0 0");
    if (variant < 2) file.write(" 0");
    file.write("\n");
    file.close();
    const auto data = gsw::PlyPointCloudLoader::load(file.fileName());
    QVERIFY2(data.isValid(), qPrintable(data.error));
    if (variant == 3) {
      QCOMPARE(data.sphericalHarmonics.sourceIndices, QVector<quint32>{1});
      QCOMPARE(data.sphericalHarmonics.coefficients[0], 2.0F);
    } else {
      QCOMPARE(data.sphericalHarmonics.degree, -1);
      QVERIFY(data.sphericalHarmonics.coefficients.isEmpty());
    }
  }
}

void WorkspaceDocumentTests::preservesIndependentSceneObjectsAcrossImportAndSave() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString working = QDir(temporary.path()).filePath("working");
  QVERIFY(QDir().mkpath(working));
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.createUntitled(working, "Multiple objects", &error));
  QStringList files;
  for (const auto &name : {"one.ply", "two.ply", "three.ply"}) {
    QFile file(QDir(working).filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n");
    files.append(file.fileName());
  }
  QVERIFY(document.setScenePath(files[0], &error));
  const QString firstId = document.activeSceneId();
  QVERIFY(document.setSceneTransform({4, 5, 6}, QQuaternion::fromAxisAndAngle(0, 0, 1, 30),
                                      {2, 3, 4}, &error));
  QVERIFY(document.addScenePath(files[1], &error));
  const QString secondId = document.activeSceneId();
  QVERIFY(firstId != secondId);
  QCOMPARE(document.sceneObjects().size(), 2);
  QCOMPARE(document.sceneObjects()[0].translation, QVector3D(4, 5, 6));
  QCOMPARE(document.sceneObjects()[0].scale, QVector3D(2, 3, 4));
  QVERIFY(document.setSceneTransform({7, 8, 9}, {}, {1, 1, 1}, &error));
  QVERIFY(!document.setScenePath(QDir(working).filePath("missing.ply"), &error));
  QCOMPARE(document.sceneObjects().size(), 2);
  QCOMPARE(document.scenePath(), files[1]);
  QVERIFY(document.activateSceneObject(firstId));
  QVERIFY(document.setScenePath(files[2], &error));
  QCOMPARE(document.sceneObjects().size(), 2);
  QCOMPARE(document.sceneObjects()[1].translation, QVector3D(7, 8, 9));
  QCOMPARE(document.sceneTranslation(), QVector3D());
  const QString saved = QDir(temporary.path()).filePath("multi.gsw");
  QVERIFY2(document.saveManifest(saved, &error), qPrintable(error));
  QVERIFY2(document.finalizeDataMigration(&error), qPrintable(error));
  gsw::WorkspaceDocument reopened;
  QVERIFY2(reopened.load(saved, &error), qPrintable(error));
  QCOMPARE(reopened.sceneObjects().size(), 2);
  QCOMPARE(reopened.activeSceneId(), firstId);
  QCOMPARE(QFileInfo(reopened.sceneObjects()[0].path).fileName(), QString("three.ply"));
  for (const auto &object : reopened.sceneObjects()) QVERIFY(QFileInfo::exists(object.path));
  QVERIFY(reopened.activateSceneObject(secondId));
  QCOMPARE(reopened.sceneTranslation(), QVector3D(7, 8, 9));
  const QString copied = QDir(temporary.path()).filePath("copied.gsw");
  QVERIFY2(reopened.save(copied, &error), qPrintable(error));
  gsw::WorkspaceDocument copiedDocument;
  QVERIFY(copiedDocument.load(copied, &error));
  QCOMPARE(copiedDocument.sceneObjects().size(), 2);
  QCOMPARE(copiedDocument.activeSceneId(), secondId);
  QCOMPARE(copiedDocument.sceneTranslation(), QVector3D(7, 8, 9));
  const auto recovery = QJsonDocument::fromJson(copiedDocument.recoveryManifestJson()).object();
  QCOMPARE(recovery["sceneCollection"].toObject()["objects"].toArray().size(), 2);
  gsw::ImportCleanupOptions cleanup;
  cleanup.clearDataset = false;
  cleanup.clearScene = true;
  QVERIFY(copiedDocument.clearImportedData(cleanup, nullptr, &error));
  QCOMPARE(copiedDocument.sceneObjects().size(), 1);
  QCOMPARE(copiedDocument.activeSceneId(), firstId);
  for (const auto &file : files) QVERIFY(QFileInfo::exists(file));
}

void WorkspaceDocumentTests::publishesGeneratedSceneWithoutReplacingReferences() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(temporary.path(), "Generation isolation", &error), qPrintable(error));
  QStringList paths;
  for (const QString &name : {QStringLiteral("reference-one.ply"),
                             QStringLiteral("reference-two.ply"),
                             QStringLiteral("new-result.ply"),
                             QStringLiteral("resumed-result.ply")}) {
    QFile file(QDir(temporary.path()).filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray ply("ply\nformat ascii 1.0\nelement vertex 1\n"
                         "property float x\nproperty float y\nproperty float z\n"
                         "end_header\n1 2 3\n");
    QCOMPARE(file.write(ply), qint64(ply.size()));
    file.close();
    paths.append(file.fileName());
  }
  QVERIFY2(document.addScenePath(paths[0], &error), qPrintable(error));
  QVERIFY2(document.setSceneTransform({4, -5, 6},
      QQuaternion::fromAxisAndAngle(0, 0, 1, 30), {2, 3, 4}, &error), qPrintable(error));
  QVERIFY2(document.addScenePath(paths[1], &error), qPrintable(error));
  QVERIFY2(document.setSceneTransform({-7, 8, 9},
      QQuaternion::fromAxisAndAngle(1, 0, 0, -20), {.5F, .75F, 1.25F}, &error), qPrintable(error));
  const auto references = document.sceneObjects();
  const QString resultId = QStringLiteral("generation-task-result");
  QVERIFY2(document.publishGeneratedScene(paths[2], resultId, &error), qPrintable(error));
  QCOMPARE(document.sceneObjects().size(), 3);
  const auto published = document.sceneObjects();
  for (int index = 0; index < 2; ++index) {
    QCOMPARE(published[index].id, references[index].id);
    QCOMPARE(published[index].path, references[index].path);
    QCOMPARE(published[index].translation, references[index].translation);
    QCOMPARE(published[index].rotation, references[index].rotation);
    QCOMPARE(published[index].scale, references[index].scale);
  }
  QCOMPARE(published[2].id, resultId);
  QCOMPARE(published[2].path, paths[2]);
  QCOMPARE(document.activeSceneId(), resultId);

  // A resumed/recovered task reuses its own result slot, regardless of which
  // reference the user selected after its previous publication.
  const QVector3D resultTranslation(1, 2, 3);
  const auto resultRotation = QQuaternion::fromAxisAndAngle(0, 1, 0, 15);
  const QVector3D resultScale(.5F, .5F, .5F);
  QVERIFY2(document.setSceneTransform(resultTranslation, resultRotation, resultScale, &error), qPrintable(error));
  QVERIFY(document.activateSceneObject(references[0].id));
  QVERIFY2(document.publishGeneratedScene(paths[3], resultId, &error), qPrintable(error));
  QCOMPARE(document.sceneObjects().size(), 3);
  const auto resumed = document.sceneObjects();
  for (int index = 0; index < 2; ++index) {
    QCOMPARE(resumed[index].id, references[index].id);
    QCOMPARE(resumed[index].path, references[index].path);
    QCOMPARE(resumed[index].translation, references[index].translation);
    QCOMPARE(resumed[index].rotation, references[index].rotation);
    QCOMPARE(resumed[index].scale, references[index].scale);
  }
  QCOMPARE(resumed[2].id, resultId);
  QCOMPARE(resumed[2].path, paths[3]);
  QCOMPARE(resumed[2].translation, resultTranslation);
  QCOMPARE(resumed[2].rotation, resultRotation);
  QCOMPARE(resumed[2].scale, resultScale);
  QCOMPARE(document.activeSceneId(), resultId);
  QVERIFY(document.activateSceneObject(references[1].id));
  QVERIFY2(document.publishGeneratedScene(paths[3], resultId, &error), qPrintable(error));
  QCOMPARE(document.sceneObjects().size(), 3);
  QCOMPARE(document.sceneObjects()[1].path, references[1].path);
  QCOMPARE(document.sceneObjects()[1].translation, references[1].translation);
  QCOMPARE(document.activeSceneId(), resultId);
}

void WorkspaceDocumentTests::rejectsInvalidGeneratedSceneWithoutChangingTheCollection() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(temporary.path(), "Invalid generation", &error), qPrintable(error));
  QFile valid(QDir(temporary.path()).filePath("reference.ply"));
  QVERIFY(valid.open(QIODevice::WriteOnly));
  QVERIFY(valid.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n"
                      "property float y\nproperty float z\nend_header\n1 2 3\n") > 0);
  valid.close();
  QVERIFY2(document.addScenePath(valid.fileName(), &error), qPrintable(error));
  QVERIFY2(document.setSceneTransform({2, -3, 4},
      QQuaternion::fromAxisAndAngle(0, 0, 1, 25), {2, .5F, 3}, &error), qPrintable(error));
  const auto before = document.sceneCollectionJson();
  QFile malformed(QDir(temporary.path()).filePath("invalid.ply"));
  QVERIFY(malformed.open(QIODevice::WriteOnly));
  QVERIFY(malformed.write("not a PLY model\n") > 0);
  malformed.close();
  for (const auto &invalidPath : {QDir(temporary.path()).filePath("missing.ply"), malformed.fileName()}) {
    QVERIFY(!document.publishGeneratedScene(invalidPath, "new-result-id", &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(document.sceneCollectionJson(), before);
  }
  QVERIFY(!document.publishGeneratedScene(valid.fileName(), {}, &error));
  QCOMPARE(document.sceneCollectionJson(), before);
}

void WorkspaceDocumentTests::commitsObjectTransformsAtomically() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  gsw::WorkspaceDocument document;
  QString error;
  const QString working = QDir(temporary.path()).filePath("working");
  QVERIFY(QDir().mkpath(working));
  QVERIFY(document.createUntitled(working, "Group transforms", &error));
  for (int i = 0; i < 3; ++i) {
    QFile file(QDir(temporary.path()).filePath(QString("group-%1.ply").arg(i)));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n");
    file.close();
    QVERIFY(document.addScenePath(file.fileName(), &error));
  }
  const auto originals = document.sceneObjects();
  auto changes = originals.mid(1);
  changes[0].translation = {5, 6, 7};
  changes[1].translation = {-2, 3, 4};
  changes[0].rotation = changes[1].rotation = QQuaternion::fromAxisAndAngle(0, 0, 1, 40);
  changes[0].scale = {2, 3, 4};
  changes[1].scale = {3, 3, 3};
  QSignalSpy changed(&document, &gsw::WorkspaceDocument::changed);
  QVERIFY(document.setSceneObjectTransforms(changes, &error));
  QCOMPARE(changed.count(), 1);
  QCOMPARE(document.sceneObjects()[0].translation, originals[0].translation);
  QCOMPARE(document.sceneObjects()[1].translation, changes[0].translation);
  QCOMPARE(document.sceneTranslation(), changes[1].translation);
  const auto committed = document.sceneCollectionJson();
  for (int invalid = 0; invalid < 4; ++invalid) {
    auto bad = changes;
    bad[0].translation = {99, 99, 99};
    if (invalid == 0) bad[1].id = "missing-object";
    if (invalid == 1) bad[1].id = bad[0].id;
    if (invalid == 2) bad[1].scale = {0, 1, 1};
    if (invalid == 3) bad[1].scale = {1.0e5F, 1, 1};
    QVERIFY(!document.setSceneObjectTransforms(bad, &error));
    QCOMPARE(document.sceneCollectionJson(), committed);
    QCOMPARE(changed.count(), 1);
  }
  QVERIFY(document.setSceneObjectTransforms({}, &error));
  QCOMPARE(changed.count(), 1);
  const QString saved = QDir(temporary.path()).filePath("group.gsw.json");
  QVERIFY2(document.saveManifest(saved, &error), qPrintable(error));
  gsw::WorkspaceDocument reopened;
  QVERIFY2(reopened.load(saved, &error), qPrintable(error));
  for (int i = 0; i < 2; ++i) {
    QCOMPARE(reopened.sceneObjects()[i + 1].translation, changes[i].translation);
    QCOMPARE(reopened.sceneObjects()[i + 1].scale, changes[i].scale);
    QVERIFY(std::abs(QQuaternion::dotProduct(reopened.sceneObjects()[i + 1].rotation,
                                            changes[i].rotation)) > 0.99999F);
  }
}

void WorkspaceDocumentTests::locatesNewestCompletedTrainingScene() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("model/point_cloud/iteration_20")));
  QVERIFY(root.mkpath(QStringLiteral("model/point_cloud/iteration_100")));
  QVERIFY(root.mkpath(QStringLiteral("model/point_cloud/iteration_invalid")));

  QFile older(root.filePath(
      QStringLiteral("model/point_cloud/iteration_20/point_cloud.ply")));
  QVERIFY(older.open(QIODevice::WriteOnly));
  QCOMPARE(older.write("ply\n"), 4);
  older.close();

  const gsw::TrainingOutputScene latest = gsw::findLatestTrainingOutputScene(
      root.filePath(QStringLiteral("model")));
  QVERIFY(latest.isValid());
  QCOMPARE(latest.iteration, 20);
  QCOMPARE(latest.path, QDir::cleanPath(older.fileName()));

  QFile newer(root.filePath(
      QStringLiteral("model/point_cloud/iteration_100/point_cloud.ply")));
  QVERIFY(newer.open(QIODevice::WriteOnly));
  QCOMPARE(newer.write("ply\n"), 4);
  newer.close();

  const gsw::TrainingOutputScene updated = gsw::findLatestTrainingOutputScene(
      root.filePath(QStringLiteral("model")));
  QVERIFY(updated.isValid());
  QCOMPARE(updated.iteration, 100);
  QCOMPARE(updated.path, QDir::cleanPath(newer.fileName()));
}

void WorkspaceDocumentTests::persistsActiveTrainingJobForCrashRecovery() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("project")));
  QVERIFY(root.mkpath(QStringLiteral("output/scene")));

  const QString projectRoot = root.filePath(QStringLiteral("project"));
  const gsw::ActiveTrainingJob expected{
      root.filePath(QStringLiteral("project/.gsw/jobs/training-job.json")),
      root.filePath(QStringLiteral("output/scene")), true,
      QStringLiteral("training-result-stable-id")};
  QString error;
  QVERIFY2(gsw::saveActiveTrainingJob(projectRoot, expected, &error),
           qPrintable(error));

  const gsw::ActiveTrainingJob restored =
      gsw::loadActiveTrainingJob(projectRoot, &error);
  QVERIFY2(restored.isValid(), qPrintable(error));
  QCOMPARE(restored.configurationPath,
           QDir::cleanPath(QFileInfo(expected.configurationPath)
                               .absoluteFilePath()));
  QCOMPARE(restored.outputSceneRoot,
           QDir::cleanPath(
               QFileInfo(expected.outputSceneRoot).absoluteFilePath()));
  QCOMPARE(restored.previewRecovered, true);
  QCOMPARE(restored.resultSceneId, expected.resultSceneId);

  QVERIFY2(gsw::clearActiveTrainingJob(projectRoot, &error),
           qPrintable(error));
  QVERIFY(!gsw::loadActiveTrainingJob(projectRoot, &error).isValid());
  QVERIFY2(error.isEmpty(), qPrintable(error));
}

void WorkspaceDocumentTests::portableTrainingResumeRecordAndManifest() {
  QTemporaryDir temporary;
  const QString original = QDir(temporary.path()).filePath(QStringLiteral("original"));
  const QString moved = QDir(temporary.path()).filePath(QStringLiteral("moved"));
  const QString output = QDir(original).filePath(QStringLiteral("output/test"));
  QVERIFY(QDir().mkpath(output));
  const QString resultId = QStringLiteral("portable-training-result-id");
  QVERIFY(gsw::saveActiveTrainingJob(original, {
      QDir(original).filePath(QStringLiteral(".gsw/jobs/config.json")), output, true, resultId}));
  QVERIFY(QDir().rename(original, moved));
  const auto restored = gsw::loadActiveTrainingJob(moved);
  QCOMPARE(restored.outputSceneRoot, QDir(moved).filePath(QStringLiteral("output/test")));
  QCOMPARE(restored.configurationPath, QDir(moved).filePath(QStringLiteral(".gsw/jobs/config.json")));
  QCOMPARE(restored.previewRecovered, true);
  QCOMPARE(restored.resultSceneId, resultId);
  QCOMPARE(gsw::nativeResumeIteration(restored.outputSceneRoot), -1);
  const QDir root(QDir(restored.outputSceneRoot).filePath(QStringLiteral(".gsw-resume")));
  QVERIFY(QDir().mkpath(root.path()));
  QFile state(root.filePath(QStringLiteral("state-0123456789abcdef0123456789abcdef.pth")));
  QVERIFY(state.open(QIODevice::WriteOnly)); state.write("fixture"); state.close();
  QFile manifest(root.filePath(QStringLiteral("ready.json")));
  QVERIFY(manifest.open(QIODevice::WriteOnly));
  manifest.write("{\"version\":1,\"iteration\":3,\"total\":10,\"file\":\"state-0123456789abcdef0123456789abcdef.pth\"}");
  manifest.close();
  QCOMPARE(gsw::nativeResumeIteration(restored.outputSceneRoot), 3);
  QVERIFY(state.remove());
  QCOMPARE(gsw::nativeResumeIteration(restored.outputSceneRoot), -1);
}

namespace {
void writeLittleEndianFloat(QFile &file, const float value) {
  const quint32 bits = qToLittleEndian(std::bit_cast<quint32>(value));
  QCOMPARE(file.write(reinterpret_cast<const char *>(&bits), sizeof(bits)),
           static_cast<qint64>(sizeof(bits)));
}

void writeBigEndianFloat(QFile &file, const float value) {
  const quint32 bits = qToBigEndian(std::bit_cast<quint32>(value));
  QCOMPARE(file.write(reinterpret_cast<const char *>(&bits), sizeof(bits)),
           static_cast<qint64>(sizeof(bits)));
}

void writeBigEndianInt32(QFile &file, const qint32 value) {
  const quint32 bits = qToBigEndian(static_cast<quint32>(value));
  QCOMPARE(file.write(reinterpret_cast<const char *>(&bits), sizeof(bits)),
           static_cast<qint64>(sizeof(bits)));
}

void appendLittleEndianFloat(QByteArray &bytes, const float value) {
  const quint32 bits = qToLittleEndian(std::bit_cast<quint32>(value));
  bytes.append(reinterpret_cast<const char *>(&bits), sizeof(bits));
}

QJsonArray vector3(const double x, const double y, const double z) {
  return {x, y, z};
}

QJsonArray rotation(const QJsonArray &row0, const QJsonArray &row1,
                    const QJsonArray &row2) {
  return {row0, row1, row2};
}

QJsonObject camera(const QString &name, const QJsonArray &position,
                   const QJsonArray &cameraRotation, const int width = 400,
                   const int height = 200) {
  return {{QStringLiteral("img_name"), name},
          {QStringLiteral("position"), position},
          {QStringLiteral("rotation"), cameraRotation},
          {QStringLiteral("width"), width},
          {QStringLiteral("height"), height}};
}

bool writeJson(const QString &path, const QJsonDocument &document) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(document.toJson(QJsonDocument::Compact)) >= 0;
}

void compareVector(const QVector3D &actual, const QVector3D &expected,
                   const float tolerance = 0.0001F) {
  QVERIFY2((actual - expected).length() <= tolerance,
           qPrintable(QStringLiteral("actual=(%1,%2,%3), expected=(%4,%5,%6)")
                          .arg(actual.x())
                          .arg(actual.y())
                          .arg(actual.z())
                          .arg(expected.x())
                          .arg(expected.y())
                          .arg(expected.z())));
}
} // namespace

void WorkspaceDocumentTests::parsesGaussianPlyHeader() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("scene.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex 1234\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property float opacity\n"
            "property float scale_0\n"
            "property float rot_0\n"
            "end_header\n");
  ply.close();

  QString error;
  const gsw::PlyMetadata metadata =
      gsw::WorkspaceDocument::inspectPly(plyPath, &error);
  QVERIFY2(metadata.valid, qPrintable(error));
  QCOMPARE(metadata.format, QStringLiteral("binary_little_endian"));
  QCOMPARE(metadata.vertexCount, 1234);
  QVERIFY(metadata.looksLikeGaussianSplat());
}

void WorkspaceDocumentTests::
    loadsAsciiPointColorsAndSamplesDeterministically() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("colored.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format ascii 1.0\n"
            "element vertex 4\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property uchar red\n"
            "property uchar green\n"
            "property uchar blue\n"
            "end_header\n"
            "0 0 0 255 0 0\n"
            "1 0 0 0 255 0\n"
            "2 0 0 0 0 255\n"
            "3 0 0 255 255 255\n");
  ply.close();

  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(plyPath, 2);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QCOMPARE(data.sourceVertexCount, 4);
  QCOMPARE(data.sourcePositions.size(), 4);
  QCOMPARE(data.vertices.size(), 2);
  QCOMPARE(data.vertices.at(0).x, 0.0F);
  QCOMPARE(data.vertices.at(1).x, 2.0F);
  QCOMPARE(data.vertices.at(0).sourceIndex, 0U);
  QCOMPARE(data.vertices.at(1).sourceIndex, 2U);
  QCOMPARE(data.vertices.at(0).red, 1.0F);
  QCOMPARE(data.vertices.at(1).blue, 1.0F);
  QCOMPARE(data.boundsMinimum, QVector3D(0.0F, 0.0F, 0.0F));
  QCOMPARE(data.boundsMaximum, QVector3D(3.0F, 0.0F, 0.0F));
}

void WorkspaceDocumentTests::loadsOversizedBinaryPointCloudAtFullResolution() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("large-cloud.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex 4\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property float nx\n"
            "property float ny\n"
            "property float nz\n"
            "property uchar red\n"
            "property uchar green\n"
            "property uchar blue\n"
            "property uchar class\n"
            "end_header\n");
  for (int index = 0; index < 4; ++index) {
    writeLittleEndianFloat(ply, static_cast<float>(index));
    writeLittleEndianFloat(ply, static_cast<float>(index * 2));
    writeLittleEndianFloat(ply, static_cast<float>(-index));
    writeLittleEndianFloat(ply, 0.0F);
    writeLittleEndianFloat(ply, 0.0F);
    writeLittleEndianFloat(ply, 1.0F);
    const char attributes[4] = {static_cast<char>(10 + index),
                                static_cast<char>(20 + index),
                                static_cast<char>(30 + index),
                                static_cast<char>(index)};
    QCOMPARE(ply.write(attributes, sizeof(attributes)),
             static_cast<qint64>(sizeof(attributes)));
  }
  ply.close();

  const gsw::PointCloudData data =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 2);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.previewOnly);
  QCOMPARE(data.sourceVertexCount, 4);
  QCOMPARE(data.sourcePositions.size(), 0);
  QCOMPARE(data.vertices.size(), 0);
  QCOMPARE(data.previewPointCount(), 4);
  QVERIFY(data.pointCache.isValid());
  QCOMPARE(data.pointCache.fullPointCount, 4);
  QCOMPARE(data.pointCache.nodes.constFirst().sourcePointCount, 4);
  const gsw::PointCloudCachePage rootPage =
      gsw::PointCloudCache::readNode(data.pointCache,
                                     data.pointCache.rootNode);
  QVERIFY2(rootPage.isValid(), qPrintable(rootPage.error));
  QCOMPARE(rootPage.vertices.size(), 4);
  QCOMPARE(rootPage.vertices.at(0).red, static_cast<quint8>(10));
  QCOMPARE(rootPage.vertices.at(3).blue, static_cast<quint8>(33));
  qint64 leafSourcePoints = 0;
  for (const gsw::PointCloudCacheNode &node : data.pointCache.nodes) {
    if (node.isLeaf()) {
      leafSourcePoints += node.sourcePointCount;
    }
  }
  QCOMPARE(leafSourcePoints, 4);
  QCOMPARE(data.boundsMinimum, QVector3D(0.0F, 0.0F, -3.0F));
  QCOMPARE(data.boundsMaximum, QVector3D(3.0F, 6.0F, 0.0F));

  const QString cacheDataPath = data.pointCache.dataPath;
  const gsw::PointCloudData reused =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 2);
  QVERIFY2(reused.isValid(), qPrintable(reused.error));
  QCOMPARE(reused.pointCache.dataPath, cacheDataPath);
}

void WorkspaceDocumentTests::diagnosticPointPagingIsOptIn() {
  const bool wasSet = qEnvironmentVariableIsSet("GSW_FORCE_POINT_PAGING");
  const QByteArray previous = qgetenv("GSW_FORCE_POINT_PAGING");
  const auto restoreEnvironment = qScopeGuard([&]() {
    if (wasSet) qputenv("GSW_FORCE_POINT_PAGING", previous);
    else qunsetenv("GSW_FORCE_POINT_PAGING");
  });
  qputenv("GSW_FORCE_POINT_PAGING", "1");

  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QFile source(temporary.filePath(QStringLiteral("diagnostic-colored.ply")));
  QVERIFY(source.open(QIODevice::WriteOnly));
  const QByteArray header(
      "ply\nformat binary_little_endian 1.0\nelement vertex 4\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
  QCOMPARE(source.write(header), qint64(header.size()));
  for (int index = 0; index < 4; ++index) {
    writeLittleEndianFloat(source, float(index));
    writeLittleEndianFloat(source, float(index * 2));
    writeLittleEndianFloat(source, float(-index));
    const auto color = QByteArray::fromHex("dc23b4");
    QCOMPARE(source.write(color), qint64(3));
  }
  source.close();
  QVERIFY(source.open(QIODevice::ReadOnly));
  const auto originalHash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
  source.close();

  const auto paged = gsw::PlyPointCloudLoader::load(source.fileName());
  QVERIFY2(paged.isValid(), qPrintable(paged.error));
  QVERIFY2(paged.previewOnly && paged.pointCache.isValid(),
           "An exact opt-in diagnostic flag must exercise the real read-only point cache.");
  QCOMPARE(paged.sourceVertexCount, 4);
  QCOMPARE(paged.pointCache.fullPointCount, 4);
  QVERIFY(paged.sourcePositions.isEmpty());
  QVERIFY(paged.vertices.isEmpty());
  QVERIFY(source.open(QIODevice::ReadOnly));
  QCOMPARE(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256), originalHash);
  source.close();

  for (const auto &flag : {QByteArray(), QByteArray("0"), QByteArray("true"),
                          QByteArray("01"), QByteArray(" 1"), QByteArray(""),
                          QByteArray("invalid")}) {
    if (flag.isNull()) qunsetenv("GSW_FORCE_POINT_PAGING");
    else qputenv("GSW_FORCE_POINT_PAGING", flag);
    const auto resident = gsw::PlyPointCloudLoader::load(source.fileName());
    QVERIFY2(resident.isValid(), qPrintable(resident.error));
    QVERIFY(!resident.previewOnly);
    QVERIFY(!resident.pointCache.isValid());
    QCOMPARE(resident.sourcePositions.size(), 4);
    QCOMPARE(resident.vertices.size(), 4);
    QCOMPARE(resident.sourceVertexCount, 4);
  }
  qputenv("GSW_FORCE_POINT_PAGING", "1");
  QFile mesh(temporary.filePath(QStringLiteral("triangle.ply")));
  QVERIFY(mesh.open(QIODevice::WriteOnly));
  const QByteArray meshBytes(
      "ply\nformat ascii 1.0\nelement vertex 3\n"
      "property float x\nproperty float y\nproperty float z\n"
      "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
      "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n");
  QCOMPARE(mesh.write(meshBytes), qint64(meshBytes.size()));
  mesh.close();
  const auto unchangedMesh = gsw::PlyPointCloudLoader::load(mesh.fileName());
  QVERIFY2(unchangedMesh.isValid(), qPrintable(unchangedMesh.error));
  QVERIFY(unchangedMesh.hasMesh());
  QVERIFY(!unchangedMesh.pointCache.isValid());
  QVERIFY(!unchangedMesh.meshCache.isValid());
  QCOMPARE(unchangedMesh.meshIndices.size(), 3);
}

void WorkspaceDocumentTests::diagnosticPagingDoesNotReplaceGaussianOrSurfelAttributes_data() {
  QTest::addColumn<bool>("surfel");
  QTest::newRow("3DGS") << false;
  QTest::newRow("2DGS") << true;
}

void WorkspaceDocumentTests::diagnosticPagingDoesNotReplaceGaussianOrSurfelAttributes() {
  QFETCH(bool, surfel);
  const bool wasSet = qEnvironmentVariableIsSet("GSW_FORCE_POINT_PAGING");
  const QByteArray previous = qgetenv("GSW_FORCE_POINT_PAGING");
  const auto restoreEnvironment = qScopeGuard([&]() {
    if (wasSet) qputenv("GSW_FORCE_POINT_PAGING", previous);
    else qunsetenv("GSW_FORCE_POINT_PAGING");
  });
  qputenv("GSW_FORCE_POINT_PAGING", "1");
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QFile source(temporary.filePath(QStringLiteral("diagnostic-gaussian.ply")));
  QVERIFY(source.open(QIODevice::WriteOnly));
  QByteArray header(
      "ply\nformat binary_little_endian 1.0\nelement vertex 2\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
      "property float opacity\nproperty float scale_0\nproperty float scale_1\n");
  if (!surfel) header += "property float scale_2\n";
  header += "property float rot_0\nproperty float rot_1\nproperty float rot_2\n"
            "property float rot_3\nend_header\n";
  QCOMPARE(source.write(header), qint64(header.size()));
  for (int index = 0; index < 2; ++index) {
    for (const float value : {float(index + 1), 2.0F, 3.0F, 0.0F, 0.0F,
                             0.0F, 0.0F, 0.0F, 0.69314718056F}) {
      writeLittleEndianFloat(source, value);
    }
    if (!surfel) writeLittleEndianFloat(source, -0.69314718056F);
    for (const float value : {2.0F, 0.0F, 0.0F, 0.0F}) {
      writeLittleEndianFloat(source, value);
    }
  }
  source.close();
  QVERIFY(source.open(QIODevice::ReadOnly));
  const auto originalHash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
  source.close();

  const auto resident = gsw::PlyPointCloudLoader::load(source.fileName());
  QVERIFY2(resident.isValid(), qPrintable(resident.error));
  QVERIFY2(!resident.previewOnly && !resident.pointCache.isValid(),
           "The ordinary-point diagnostic must not replace Gaussian/surfel attributes with RGB pages.");
  QVERIFY(resident.hasGaussianAttributes);
  QCOMPARE(resident.hasSurfelAttributes, surfel);
  QCOMPARE(resident.sourcePositions.size(), 2);
  QCOMPARE(resident.vertices.size(), 2);
  QCOMPARE(resident.sphericalHarmonics.degree, 0);
  const auto &vertex = resident.vertices.constFirst();
  QVERIFY(qAbs(vertex.opacity - 0.5F) < 1.0e-6F);
  QVERIFY(qAbs(vertex.scaleX - 1.0F) < 1.0e-6F);
  QVERIFY(qAbs(vertex.scaleY - 2.0F) < 1.0e-5F);
  QVERIFY(qAbs(vertex.rotationW - 1.0F) < 1.0e-6F);

  // The pre-existing count ceiling remains active even with the diagnostic
  // override ignored; a small literal sample exercises the same branch safely.
  const auto limited = gsw::PlyPointCloudLoader::load(source.fileName(), 1, 1);
  QVERIFY2(limited.isValid(), qPrintable(limited.error));
  QVERIFY(limited.previewOnly && limited.pointCache.isValid());
  QCOMPARE(limited.pointCache.fullPointCount, 2);
  QVERIFY(source.open(QIODevice::ReadOnly));
  QCOMPARE(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256), originalHash);
}

void WorkspaceDocumentTests::loadsOversizedAsciiPointCloudIntoDiskCache() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("large-ascii.ply"));
  QFile file(plyPath);
  QVERIFY(file.open(QIODevice::WriteOnly));
  constexpr int pointCount = 300'000;
  QByteArray payload =
      "ply\n"
      "format ascii 1.0\n"
      "element vertex 300000\n"
      "property float x\n"
      "property float y\n"
      "property float z\n"
      "property uchar red\n"
      "property uchar green\n"
      "property uchar blue\n"
      "end_header\n";
  const std::array<QByteArray, 4> records = {
      "+0.000000 0.000000 0.000000 10 20 30\n",
      "1e0\t2.000000 3.000000 11 21 31\n",
      "2.000000 4.000000 6.000000 12 22 32\n",
      "3.000000 6.000000 9.000000 13 23 33\n"};
  payload.reserve(12 * 1024 * 1024);
  for (int index = 0; index < pointCount; ++index) {
    payload.append(records.at(static_cast<std::size_t>(index % 4)));
  }
  QVERIFY(payload.size() > 8 * 1024 * 1024);
  QCOMPARE(file.write(payload), payload.size());
  file.close();

  const gsw::PointCloudData data =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 2);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.previewOnly);
  QVERIFY(data.pointCache.isValid());
  QCOMPARE(data.previewPointCount(), pointCount);
  QCOMPARE(data.boundsMinimum, QVector3D(0.0F, 0.0F, 0.0F));
  QCOMPARE(data.boundsMaximum, QVector3D(3.0F, 6.0F, 9.0F));
  const gsw::PointCloudCachePage rootPage =
      gsw::PointCloudCache::readNode(data.pointCache,
                                     data.pointCache.rootNode);
  QVERIFY2(rootPage.isValid(), qPrintable(rootPage.error));
  QCOMPARE(rootPage.vertices.size(), 32'768);
  QVERIFY(std::any_of(
      rootPage.vertices.cbegin(), rootPage.vertices.cend(),
      [](const gsw::PointPreviewVertex &vertex) { return vertex.red == 10; }));
  QVERIFY(std::any_of(
      rootPage.vertices.cbegin(), rootPage.vertices.cend(),
      [](const gsw::PointPreviewVertex &vertex) { return vertex.blue == 33; }));

  const gsw::PointCloudData reused =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 2);
  QVERIFY2(reused.isValid(), qPrintable(reused.error));
  QCOMPARE(reused.pointCache.dataPath, data.pointCache.dataPath);
}

void WorkspaceDocumentTests::preservesGlobalCoordinatesWhilePagingLocalDisplayData() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("georeferenced.ply"));
  QFile file(plyPath);
  QVERIFY(file.open(QIODevice::WriteOnly));
  const QByteArray payload =
      "ply\n"
      "format ascii 1.0\n"
      "comment coordinate_unit m\n"
      "comment crs=EPSG:6677\n"
      "element vertex 3\n"
      "property double x\n"
      "property double y\n"
      "property double z\n"
      "property uchar red\n"
      "property uchar green\n"
      "property uchar blue\n"
      "end_header\n"
      "637000.123456 4000000.654321 120.25 255 0 0\n"
      "637010.123456 4000005.654321 122.25 0 255 0\n"
      "636995.123456 3999990.654321 119.25 0 0 255\n";
  QCOMPARE(file.write(payload), payload.size());
  file.close();

  const gsw::PointCloudData data =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 2);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.previewOnly);
  QVERIFY(data.coordinates.valid);
  QVERIFY(data.coordinates.automaticDisplayShift);
  QVERIFY(data.coordinates.unitDeclared);
  QCOMPARE(data.coordinates.unit, gsw::SceneLengthUnit::Metres);
  QCOMPARE(data.coordinates.coordinateReferenceSystem,
           QStringLiteral("EPSG:6677"));
  QVERIFY(data.coordinates.sourceUsesFloat64);
  QVERIFY(std::abs(data.coordinates.globalMinimum.x - 636995.123456) < 1e-9);
  QVERIFY(std::abs(data.coordinates.globalMinimum.y - 3999990.654321) < 1e-9);
  QVERIFY(std::abs(data.coordinates.globalMinimum.z - 119.25) < 1e-12);
  QVERIFY(std::abs(data.coordinates.globalMaximum.x - 637010.123456) < 1e-9);
  QVERIFY(std::abs(data.coordinates.globalMaximum.y - 4000005.654321) < 1e-9);
  QVERIFY(std::abs(data.coordinates.globalMaximum.z - 122.25) < 1e-12);
  QCOMPARE(data.boundsMinimum, QVector3D(-5.0F, -10.0F, -1.0F));
  QCOMPARE(data.boundsMaximum, QVector3D(10.0F, 5.0F, 2.0F));

  const gsw::PointCloudCachePage root = gsw::PointCloudCache::readNode(
      data.pointCache, data.pointCache.rootNode);
  QVERIFY2(root.isValid(), qPrintable(root.error));
  QVERIFY(std::all_of(
      root.vertices.cbegin(), root.vertices.cend(),
      [](const gsw::PointPreviewVertex &vertex) {
        return std::abs(vertex.x) <= 10.0F && std::abs(vertex.y) <= 10.0F &&
               std::abs(vertex.z) <= 2.0F;
      }));

  const gsw::PointCloudData reused =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 2);
  QVERIFY2(reused.isValid(), qPrintable(reused.error));
  QCOMPARE(reused.pointCache.dataPath, data.pointCache.dataPath);
  QVERIFY(std::abs(reused.coordinates.globalMaximum.y - 4000005.654321) <
          1e-9);
}

void WorkspaceDocumentTests::recognizesSingularDeclaredLengthUnits_data() {
  QTest::addColumn<QString>("name");
  QTest::addColumn<QString>("symbol");
  for (const auto &name : {"millimetre", "millimeter", "millimetres", "millimeters"})
    QTest::newRow(name) << QString::fromLatin1(name) << QStringLiteral("mm");
  for (const auto &name : {"centimetre", "centimeter", "centimetres", "centimeters"})
    QTest::newRow(name) << QString::fromLatin1(name) << QStringLiteral("cm");
  for (const auto &name : {"metre", "meter", "metres", "meters", "MeTrE"})
    QTest::newRow(name) << QString::fromLatin1(name) << QStringLiteral("m");
}

void WorkspaceDocumentTests::recognizesSingularDeclaredLengthUnits() {
  QFETCH(QString, name);
  QFETCH(QString, symbol);
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString sourcePath = temporary.filePath(QStringLiteral("metric.ply"));
  const QByteArray original = "ply\nformat ascii 1.0\ncomment coordinate_unit " + name.toUtf8() +
      "\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\nend_header\n123.125 2 3\n";
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  QCOMPARE(source.write(original), original.size());
  source.close();
  const auto loaded = gsw::PlyPointCloudLoader::load(sourcePath);
  QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
  QVERIFY(loaded.coordinates.unitDeclared);
  QCOMPARE(gsw::sceneLengthUnitSymbol(loaded.coordinates.unit), symbol);
  QCOMPARE(loaded.coordinates.globalMinimum.x, 123.125);
  QVERIFY(source.open(QIODevice::ReadOnly));
  QCOMPARE(source.readAll(), original);
}

void WorkspaceDocumentTests::parsesUnitsAndExportsCoordinateReport() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString sourcePath =
      QDir(temporary.path()).filePath(QStringLiteral("metric.ply"));
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write("ply\n");
  source.close();

  const gsw::SceneCoordinateMetadata metadata =
      gsw::parsePlyCoordinateMetadata(
          {QStringLiteral("units: cm"), QStringLiteral("EPSG:4978")},
          sourcePath, true);
  QCOMPARE(metadata.unit, gsw::SceneLengthUnit::Centimetres);
  QVERIFY(metadata.unitDeclared);
  QCOMPARE(metadata.coordinateReferenceSystem, QStringLiteral("EPSG:4978"));
  QVERIFY(metadata.sourceUsesFloat64);

  gsw::SceneCoordinateTracker tracker(metadata);
  QVector3D local;
  QVERIFY(tracker.observeAndMap(-20.0, 10.0, 5.0, local));
  QCOMPARE(local, QVector3D(-20.0F, 10.0F, 5.0F));
  QVERIFY(tracker.observeAndMap(80.0, 210.0, 35.0, local));
  const gsw::SceneCoordinateInfo coordinates = tracker.info();
  QCOMPARE(gsw::formatSceneSize(coordinates),
           QStringLiteral("100 × 200 × 30 cm"));

  QString error;
  const QString jsonPath =
      QDir(temporary.path()).filePath(QStringLiteral("coordinates.json"));
  QVERIFY2(gsw::writeSceneCoordinateReport(
               jsonPath, sourcePath, coordinates, 5.0,
               QStringLiteral("modelBase"), &error),
           qPrintable(error));
  QFile jsonFile(jsonPath);
  QVERIFY(jsonFile.open(QIODevice::ReadOnly));
  const QJsonObject report =
      QJsonDocument::fromJson(jsonFile.readAll()).object();
  QCOMPARE(report.value(QStringLiteral("referencePlaneMode")).toString(),
           QStringLiteral("modelBase"));
  QCOMPARE(report.value(QStringLiteral("globalSize")).toArray().at(1).toDouble(),
           200.0);
  QCOMPARE(report.value(QStringLiteral("coordinates"))
               .toObject()
               .value(QStringLiteral("unit"))
               .toString(),
           QStringLiteral("centimetres"));

  const QString csvPath =
      QDir(temporary.path()).filePath(QStringLiteral("coordinates.csv"));
  QVERIFY2(gsw::writeSceneCoordinateReport(
               csvPath, sourcePath, coordinates, 0.0,
               QStringLiteral("worldZero"), &error),
           qPrintable(error));
  QFile csvFile(csvPath);
  QVERIFY(csvFile.open(QIODevice::ReadOnly));
  const QByteArray csv = csvFile.readAll();
  QVERIFY(csv.contains("size,100,200,30,,cm"));
  QVERIFY(csv.contains("reference_plane_mode"));
}

void WorkspaceDocumentTests::loadsConfiguredRealCoordinateFixture() {
  const QString fixture =
      qEnvironmentVariable("GSW_REAL_COORDINATE_PLY").trimmed();
  if (fixture.isEmpty()) {
    QSKIP("GSW_REAL_COORDINATE_PLY is not configured.");
  }
  QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
  QElapsedTimer timer;
  timer.start();
  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(fixture);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.coordinates.valid);
  QVERIFY(data.sourceVertexCount > 0);
  const gsw::SceneCoordinate3D size = data.coordinates.globalSize();
  QVERIFY(size.x >= 0.0 && size.y >= 0.0 && size.z >= 0.0);
  qInfo().noquote()
      << QStringLiteral(
             "REAL_COORDINATE_PLY vertices=%1 min=(%2,%3,%4) max=(%5,%6,%7) "
             "size=(%8,%9,%10) unit=%11 elapsedMs=%12")
             .arg(data.sourceVertexCount)
             .arg(data.coordinates.globalMinimum.x, 0, 'g', 16)
             .arg(data.coordinates.globalMinimum.y, 0, 'g', 16)
             .arg(data.coordinates.globalMinimum.z, 0, 'g', 16)
             .arg(data.coordinates.globalMaximum.x, 0, 'g', 16)
             .arg(data.coordinates.globalMaximum.y, 0, 'g', 16)
             .arg(data.coordinates.globalMaximum.z, 0, 'g', 16)
             .arg(size.x, 0, 'g', 16)
             .arg(size.y, 0, 'g', 16)
             .arg(size.z, 0, 'g', 16)
             .arg(gsw::sceneLengthUnitDescription(data.coordinates))
             .arg(timer.elapsed());
}

void WorkspaceDocumentTests::buildsAndInvalidatesDiskResidentPointOctree() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString sourcePath =
      QDir(temporary.path()).filePath(QStringLiteral("source.bin"));
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  QCOMPARE(source.write("source"), 6);
  source.close();

  gsw::PointCloudCacheBuilder builder(
      sourcePath, QVector3D(-1.0F, -1.0F, -1.0F),
      QVector3D(1.0F, 1.0F, 1.0F));
  QString error;
  QVERIFY2(builder.begin(&error), qPrintable(error));
  constexpr int pointCount = 40'000;
  for (int index = 0; index < pointCount; ++index) {
    gsw::PointPreviewVertex vertex;
    vertex.x = static_cast<float>((index % 97) - 48) / 48.0F;
    vertex.y = static_cast<float>(((index / 97) % 89) - 44) / 44.0F;
    vertex.z = static_cast<float>(((index / (97 * 89)) % 7) - 3) / 3.0F;
    vertex.red = static_cast<quint8>(index % 256);
    QVERIFY2(builder.append(vertex, index, &error), qPrintable(error));
  }
  const gsw::PointCloudCacheIndex built = builder.finish(&error);
  QVERIFY2(built.isValid(), qPrintable(error));
  QCOMPARE(built.fullPointCount, static_cast<qint64>(pointCount));
  QCOMPARE(built.nodes.constFirst().pointCount, 32'768);
  qint64 leafPoints = 0;
  int populatedLeaf = -1;
  for (const gsw::PointCloudCacheNode &node : built.nodes) {
    if (!node.isLeaf()) {
      continue;
    }
    leafPoints += node.sourcePointCount;
    if (populatedLeaf < 0 && node.pointCount > 0) {
      populatedLeaf = node.id;
    }
  }
  QCOMPARE(leafPoints, static_cast<qint64>(pointCount));
  QVERIFY(populatedLeaf >= 0);
  const gsw::PointCloudCachePage leafPage =
      gsw::PointCloudCache::readNode(built, populatedLeaf);
  QVERIFY2(leafPage.isValid(), qPrintable(leafPage.error));
  QCOMPARE(leafPage.vertices.size(),
           static_cast<qsizetype>(built.nodes.at(populatedLeaf).pointCount));

  const gsw::PointCloudCacheIndex reused =
      gsw::PointCloudCache::loadForSource(sourcePath, &error);
  QVERIFY2(reused.isValid(), qPrintable(error));
  QCOMPARE(reused.dataPath, built.dataPath);

  QFile indexFile(built.indexPath);
  QVERIFY(indexFile.open(QIODevice::ReadOnly));
  const QByteArray originalIndex = indexFile.readAll();
  indexFile.close();
  QJsonObject corruptedRoot =
      QJsonDocument::fromJson(originalIndex).object();
  QJsonArray corruptedNodes =
      corruptedRoot.value(QStringLiteral("nodes")).toArray();
  QJsonObject corruptedRootNode = corruptedNodes.at(0).toObject();
  corruptedRootNode.insert(QStringLiteral("id"), 99);
  corruptedNodes.replace(0, corruptedRootNode);
  corruptedRoot.insert(QStringLiteral("nodes"), corruptedNodes);
  QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QVERIFY(indexFile.write(
              QJsonDocument(corruptedRoot).toJson(QJsonDocument::Compact)) >
          0);
  indexFile.close();
  QVERIFY(!gsw::PointCloudCache::loadForSource(sourcePath).isValid());

  QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QCOMPARE(indexFile.write(originalIndex), originalIndex.size());
  indexFile.close();
  QVERIFY(gsw::PointCloudCache::loadForSource(sourcePath).isValid());

  QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Append));
  QCOMPARE(source.write("changed"), 7);
  source.close();
  QVERIFY(!gsw::PointCloudCache::loadForSource(sourcePath).isValid());
}

void WorkspaceDocumentTests::loadsAsciiPolygonMeshAndTriangulates() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("quad-mesh.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format ascii 1.0\n"
            "element vertex 4\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property uchar red\n"
            "property uchar green\n"
            "property uchar blue\n"
            "element face 1\n"
            "property list uchar int vertex_indices\n"
            "end_header\n"
            "0 0 0 255 0 0\n"
            "1 0 0 0 255 0\n"
            "1 1 0 0 0 255\n"
            "0 1 0 255 255 255\n"
            "4 0 1 2 3\n");
  ply.close();

  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(plyPath, 2);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.hasMesh());
  QCOMPARE(data.sourceVertexCount, 4);
  QCOMPARE(data.sourceFaceCount, 1);
  QCOMPARE(data.sourceTriangleCount, 2);
  QCOMPARE(data.vertices.size(), 4);
  QCOMPARE(data.meshVertices.size(), 4);
  QCOMPARE(data.meshIndices,
           QVector<quint32>({0U, 1U, 2U, 0U, 2U, 3U}));
  QCOMPARE(data.meshVertices.at(0).red, 1.0F);
  QVERIFY(data.meshVertices.at(0).normalZ > 0.99F);
}

void WorkspaceDocumentTests::loadsAdaptiveAsciiMeshEfficientlyAndPreservesChunkedRecords() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  for (const bool longRecords : {false, true}) {
    const QString path = QDir(temporary.path()).filePath(
        longRecords ? QStringLiteral("chunked-records.ply") : QStringLiteral("short-records.ply"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray payload = "ply\nformat ascii 1.0\nelement vertex ";
    payload += longRecords ? "4" : "3";
    payload += "\nproperty float x\nproperty float y\nproperty float z\nelement face ";
    payload += longRecords ? "1" : "4000";
    payload += "\nproperty list uchar int vertex_indices\nend_header\n";
    if (longRecords) {
      const QByteArray padding(9000, ' ');
      payload += padding + "\r\n0" + padding + "0" + padding + "0\r\n";
      payload += "1 0 0\r\n1 1 0\r\n0 1 0\r\n" + padding + "4 0 1 2 3";
    } else {
      payload += "0 0 0\n1 0 0\n0 1 0\n";
      for (int index = 0; index < 4000; ++index) payload += "3 0 1 2\n";
    }
    QCOMPARE(file.write(payload), qint64(payload.size()));
    file.close();

    gsw::ResourceBudgetController budget;
    gsw::ResourceBudgetPolicy policy;
    policy.mode = gsw::ResourceBudgetPolicy::Mode::Manual;
    policy.ramLimitMiB = 64;
    policy.gpuLimitMiB = 64;
    budget.setPolicy(policy);
    gsw::LoaderResourcePolicy loadingPolicy;
    loadingPolicy.reservation = budget.createReservation();
    QElapsedTimer elapsed;
    elapsed.start();
    const auto data = gsw::PlyPointCloudLoader::load(path,
        gsw::PlyPointCloudLoader::DefaultMaximumPreviewPoints,
        gsw::PlyPointCloudLoader::DefaultMaximumEditablePoints,
        gsw::PlyPointCloudLoader::DefaultMaximumResidentMeshVertices,
        gsw::PlyPointCloudLoader::DefaultMaximumResidentMeshFaces,
        gsw::PlyPointCloudLoader::DefaultMaximumResidentMeshBytes, loadingPolicy);
    const qint64 milliseconds = elapsed.elapsed();
    QVERIFY2(data.isValid(), qPrintable(data.error));
    QVERIFY(data.hasMesh());
    QVERIFY(!data.meshCache.isValid());
    QCOMPARE(data.sourceVertexCount, longRecords ? qint64(4) : qint64(3));
    QCOMPARE(data.sourceFaceCount, longRecords ? qint64(1) : qint64(4000));
    QCOMPARE(data.meshIndices.size() / 3, longRecords ? qsizetype(2) : qsizetype(4000));
    QVERIFY(data.residentRamPeakBytes <= 64LL * 1024 * 1024);
    QVERIFY(data.residentGpuBytes <= 64LL * 1024 * 1024);
    qInfo() << "Adaptive ASCII public loader:" << (longRecords ? "chunked CRLF/EOF" : "4003 short records")
            << "elapsed milliseconds" << milliseconds << "file bytes" << payload.size();
    QVERIFY2(milliseconds < 2000, "A bounded ASCII mesh must not allocate the maximum record size for every short line.");
    loadingPolicy.reservation.reset();
    QCOMPARE(budget.status().reservedRamBytes, qint64(0));
    QCOMPARE(budget.status().reservedGpuBytes, qint64(0));
  }
}

void WorkspaceDocumentTests::loadsBinaryBigEndianMesh() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("big-endian-mesh.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_big_endian 1.0\n"
            "element vertex 3\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "element face 1\n"
            "property list uchar int vertex_index\n"
            "end_header\n");
  for (const QVector3D &position :
       {QVector3D(0.0F, 0.0F, 0.0F), QVector3D(1.0F, 0.0F, 0.0F),
        QVector3D(0.0F, 1.0F, 0.0F)}) {
    writeBigEndianFloat(ply, position.x());
    writeBigEndianFloat(ply, position.y());
    writeBigEndianFloat(ply, position.z());
  }
  const char faceSize = 3;
  QCOMPARE(ply.write(&faceSize, 1), qint64(1));
  writeBigEndianInt32(ply, 0);
  writeBigEndianInt32(ply, 1);
  writeBigEndianInt32(ply, 2);
  ply.close();

  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(plyPath);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.hasMesh());
  QCOMPARE(data.sourceFaceCount, 1);
  QCOMPARE(data.meshIndices, QVector<quint32>({0U, 1U, 2U}));
  QCOMPARE(data.meshVertices.at(1).x, 1.0F);
  QVERIFY(data.meshVertices.at(0).normalZ > 0.99F);
}

void WorkspaceDocumentTests::loadsOversizedAsciiMeshIntoDiskCache() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("paged-mesh.ply"));
  const QString texturePath =
      QDir(temporary.path()).filePath(QStringLiteral("paged-texture.ppm"));
  QFile texture(texturePath);
  QVERIFY(texture.open(QIODevice::WriteOnly));
  const QByteArray texturePayload =
      "P3\n2 2\n255\n255 0 0  0 255 0\n0 0 255  255 255 0\n";
  QCOMPARE(texture.write(texturePayload), texturePayload.size());
  texture.close();
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  const QByteArray payload =
      "ply\n"
      "format ascii 1.0\n"
      "comment TextureFile paged-texture.ppm\n"
      "comment coordinate_unit m\n"
      "element vertex 5\n"
      "property double x\n"
      "property double y\n"
      "property double z\n"
      "property uchar red\n"
      "property uchar green\n"
      "property uchar blue\n"
      "element face 2\n"
      "property list uchar int vertex_indices\n"
      "property list uchar float texcoord\n"
      "end_header\n"
      "637000 4000000 100 255 0 0\n"
      "637001 4000000 100 0 255 0\n"
      "637001 4000001 100 0 0 255\n"
      "637000 4000001 100 255 255 255\n"
      "637000.5 4000000.5 101 128 128 128\n"
      "4 0 1 2 3 8 0 0 1 0 1 1 0 1\n"
      "3 0 2 4 6 0 0 1 1 0.5 0.5\n";
  QCOMPARE(ply.write(payload), payload.size());
  ply.close();

  const gsw::PointCloudData data =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 100, 4, 1);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.previewOnly);
  QVERIFY(data.hasMesh());
  QVERIFY(data.meshCache.isValid());
  QVERIFY(data.meshCache.hasTextureCoordinates);
  QVERIFY(data.meshHasTextureCoordinates);
  QCOMPARE(data.meshTexturePath, texturePath);
  QCOMPARE(data.meshTextureImage.size(), QSize(2, 2));
  QVERIFY(data.meshTextureError.isEmpty());
  QVERIFY(data.meshVertices.isEmpty());
  QVERIFY(data.meshIndices.isEmpty());
  QCOMPARE(data.sourceVertexCount, 5);
  QCOMPARE(data.sourceFaceCount, 2);
  QCOMPARE(data.sourceTriangleCount, 3);
  QCOMPARE(data.meshCache.renderableTriangleCount, qint64(3));
  QVERIFY(data.coordinates.valid);
  QVERIFY(data.coordinates.automaticDisplayShift);
  QCOMPARE(data.coordinates.globalMinimum.x, 637000.0);
  QCOMPARE(data.coordinates.globalMaximum.y, 4000001.0);
  QCOMPARE(data.boundsMinimum, QVector3D(0.0F, 0.0F, 0.0F));
  QCOMPARE(data.boundsMaximum, QVector3D(1.0F, 1.0F, 1.0F));
  const gsw::MeshCachePage rootPage =
      gsw::MeshCache::readNode(data.meshCache, data.meshCache.rootNode);
  QVERIFY2(rootPage.isValid(), qPrintable(rootPage.error));
  QCOMPARE(rootPage.indices.size() / 3, qsizetype(3));
  QCOMPARE(rootPage.vertices.size(), qsizetype(5));
  QVERIFY(std::all_of(rootPage.vertices.cbegin(), rootPage.vertices.cend(),
                      [](const gsw::MeshVertex &vertex) {
                        return vertex.textureWeight > 0.5F;
                      }));

  const gsw::PointCloudData reused =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 100, 4, 1);
  QVERIFY2(reused.isValid(), qPrintable(reused.error));
  QCOMPARE(reused.meshCache.dataPath, data.meshCache.dataPath);

  QVERIFY(QFile::remove(texturePath));
  const gsw::PointCloudData fallback =
      gsw::PlyPointCloudLoader::load(plyPath, 2, 100, 4, 1);
  QVERIFY2(fallback.isValid(), qPrintable(fallback.error));
  QVERIFY(fallback.meshHasTextureCoordinates);
  QVERIFY(fallback.meshTextureImage.isNull());
  QVERIFY(!fallback.meshTextureError.isEmpty());
}

void WorkspaceDocumentTests::preservesResidentMeshUvSeams() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString texturePath =
      QDir(temporary.path()).filePath(QStringLiteral("seam texture.ppm"));
  QFile texture(texturePath);
  QVERIFY(texture.open(QIODevice::WriteOnly));
  const QByteArray texturePayload =
      "P3\n2 2\n255\n255 0 0  0 255 0\n0 0 255  255 255 0\n";
  QCOMPARE(texture.write(texturePayload), texturePayload.size());
  texture.close();

  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("seam-mesh.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  const QByteArray payload =
      "ply\n"
      "format ascii 1.0\n"
      "comment TextureFile \"seam texture.ppm\"\n"
      "element vertex 4\n"
      "property float x\n"
      "property float y\n"
      "property float z\n"
      "property uchar red\n"
      "property uchar green\n"
      "property uchar blue\n"
      "element face 2\n"
      "property list uchar int vertex_indices\n"
      "property list uchar float texcoord\n"
      "end_header\n"
      "0 0 0 255 255 255\n"
      "1 0 0 255 255 255\n"
      "1 1 0 255 255 255\n"
      "0 1 0 255 255 255\n"
      "3 0 1 2 6 0 0 1 0 1 1\n"
      "3 0 2 3 6 0.25 0.25 1 1 0 1\n";
  QCOMPARE(ply.write(payload), payload.size());
  ply.close();

  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(plyPath);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.hasMesh());
  QVERIFY(data.meshHasTextureCoordinates);
  QCOMPARE(data.meshTextureImage.size(), QSize(2, 2));
  QCOMPARE(data.meshIndices.size(), qsizetype(6));
  QCOMPARE(data.meshVertices.size(), qsizetype(5));
  QVector<QVector2D> originTextureCoordinates;
  for (const gsw::MeshVertex &vertex : data.meshVertices) {
    QVERIFY(vertex.textureWeight > 0.5F);
    if (qFuzzyIsNull(vertex.x) && qFuzzyIsNull(vertex.y) &&
        qFuzzyIsNull(vertex.z)) {
      originTextureCoordinates.append(
          QVector2D(vertex.textureU, vertex.textureV));
    }
  }
  QCOMPARE(originTextureCoordinates.size(), qsizetype(2));
  QVERIFY(originTextureCoordinates.contains(QVector2D(0.0F, 0.0F)));
  QVERIFY(originTextureCoordinates.contains(QVector2D(0.25F, 0.25F)));
}

void WorkspaceDocumentTests::buildsPagesAndInvalidatesDiskResidentMesh() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString sourcePath =
      QDir(temporary.path()).filePath(QStringLiteral("source-mesh.ply"));
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  QCOMPARE(source.write("source-mesh"), 11);
  source.close();

  gsw::MeshCacheBuilder builder(sourcePath, 4);
  QString error;
  QVERIFY2(builder.begin(&error), qPrintable(error));
  const std::array<gsw::MeshVertex, 4> vertices = {
      gsw::MeshVertex{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F},
      gsw::MeshVertex{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F},
      gsw::MeshVertex{0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
      gsw::MeshVertex{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F}};
  for (const gsw::MeshVertex &vertex : vertices) {
    QVERIFY2(builder.appendVertex(vertex, &error), qPrintable(error));
  }
  QVERIFY2(builder.finishVertices(&error), qPrintable(error));
  constexpr int triangleCount = 70'000;
  for (int index = 0; index < triangleCount; ++index) {
    QVERIFY2(builder.appendTriangle(0, 1, 2, index, &error),
             qPrintable(error));
  }
  const gsw::MeshCacheIndex built =
      builder.finish(triangleCount, triangleCount, &error);
  QVERIFY2(built.isValid(), qPrintable(error));
  QCOMPARE(built.fullVertexCount, qint64(4));
  QCOMPARE(built.fullFaceCount, qint64(triangleCount));
  QCOMPARE(built.renderableTriangleCount, qint64(triangleCount));
  QCOMPARE(built.nodes.constFirst().indexCount / 3, qint64(32'768));

  const auto splitLeaf = std::find_if(
      built.nodes.cbegin(), built.nodes.cend(),
      [](const gsw::MeshCacheNode &node) {
        return node.depth == gsw::MeshCacheIndex::SpatialOctreeDepth &&
               !node.children.isEmpty();
      });
  QVERIFY(splitLeaf != built.nodes.cend());
  QCOMPARE(splitLeaf->children.size(), qsizetype(2));
  qint64 pagedTriangles = 0;
  for (const int childId : splitLeaf->children) {
    const gsw::MeshCacheNode &child = built.nodes.at(childId);
    QVERIFY(child.isLeaf());
    QVERIFY(child.indexCount / 3 <= 65'536);
    pagedTriangles += child.sourceTriangleCount;
    const gsw::MeshCachePage page = gsw::MeshCache::readNode(built, childId);
    QVERIFY2(page.isValid(), qPrintable(page.error));
    QVERIFY(page.vertices.constFirst().normalZ > 0.99F);
  }
  QCOMPARE(pagedTriangles, qint64(triangleCount));

  const gsw::MeshCacheIndex reused =
      gsw::MeshCache::loadForSource(sourcePath, &error);
  QVERIFY2(reused.isValid(), qPrintable(error));
  QCOMPARE(reused.dataPath, built.dataPath);
  QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Append));
  QCOMPARE(source.write("changed"), 7);
  source.close();
  QVERIFY(!gsw::MeshCache::loadForSource(sourcePath).isValid());
}

void WorkspaceDocumentTests::loadsConfiguredRealMeshFixture() {
  const QString fixture = qEnvironmentVariable("GSW_REAL_MESH_FIXTURE");
  if (fixture.isEmpty()) {
    QSKIP("GSW_REAL_MESH_FIXTURE is not configured.");
  }
  QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
  QElapsedTimer timer;
  timer.start();
  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(fixture);
  const qint64 elapsedMilliseconds = timer.elapsed();
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.hasMesh());
  QVERIFY(data.meshCache.isValid());
  QVERIFY(data.meshCache.hasTextureCoordinates);
  QVERIFY(data.meshHasTextureCoordinates);
  QVERIFY2(!data.meshTextureImage.isNull(),
           qPrintable(data.meshTextureError));
  QVERIFY(data.meshTextureImage.width() > 0);
  QVERIFY(data.meshTextureImage.height() > 0);
  QVERIFY2(data.meshTextureError.isEmpty(),
           qPrintable(data.meshTextureError));
  const gsw::MeshCachePage rootPage = gsw::MeshCache::readNode(
      data.meshCache, data.meshCache.rootNode);
  QVERIFY2(rootPage.isValid(), qPrintable(rootPage.error));
  QVERIFY(std::any_of(rootPage.vertices.cbegin(), rootPage.vertices.cend(),
                      [](const gsw::MeshVertex &vertex) {
                        return vertex.textureWeight > 0.5F;
                      }));
  const auto deepestPage = std::max_element(
      data.meshCache.nodes.cbegin(), data.meshCache.nodes.cend(),
      [](const gsw::MeshCacheNode &left, const gsw::MeshCacheNode &right) {
        const int leftDepth = left.isValid() ? left.depth : -1;
        const int rightDepth = right.isValid() ? right.depth : -1;
        return leftDepth < rightDepth;
      });
  QVERIFY(deepestPage != data.meshCache.nodes.cend());
  QVERIFY(deepestPage->isValid());
  const gsw::MeshCachePage exactPage =
      gsw::MeshCache::readNode(data.meshCache, deepestPage->id);
  QVERIFY2(exactPage.isValid(), qPrintable(exactPage.error));
  if (deepestPage->depth > gsw::MeshCacheIndex::SpatialOctreeDepth) {
    QVERIFY(exactPage.indices.size() / 3 <= 65'536);
  }
  qInfo().noquote()
      << QStringLiteral(
             "REAL_MESH_CACHE elapsedMs=%1 vertices=%2 faces=%3 "
             "triangles=%4 nodes=%5 dataBytes=%6 texture=%7x%8")
             .arg(elapsedMilliseconds)
             .arg(data.meshCache.fullVertexCount)
             .arg(data.meshCache.fullFaceCount)
             .arg(data.meshCache.renderableTriangleCount)
             .arg(data.meshCache.nodes.size())
             .arg(QFileInfo(data.meshCache.dataPath).size())
             .arg(data.meshTextureImage.width())
             .arg(data.meshTextureImage.height());
}

void WorkspaceDocumentTests::loadsBinaryGaussianSphericalHarmonicColors() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("gaussian.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex 1\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property float f_dc_0\n"
            "property float f_dc_1\n"
            "property float f_dc_2\n"
            "end_header\n");
  writeLittleEndianFloat(ply, 1.0F);
  writeLittleEndianFloat(ply, 2.0F);
  writeLittleEndianFloat(ply, 3.0F);
  writeLittleEndianFloat(ply, 0.0F);
  writeLittleEndianFloat(ply, 0.0F);
  writeLittleEndianFloat(ply, 0.0F);
  ply.close();

  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(plyPath);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QCOMPARE(data.vertices.size(), 1);
  QCOMPARE(data.vertices.first().x, 1.0F);
  QCOMPARE(data.vertices.first().y, 2.0F);
  QCOMPARE(data.vertices.first().z, 3.0F);
  QCOMPARE(data.vertices.first().red, 0.5F);
  QCOMPARE(data.vertices.first().green, 0.5F);
  QCOMPARE(data.vertices.first().blue, 0.5F);
  QVERIFY(!data.hasGaussianAttributes);
}

void WorkspaceDocumentTests::activatesGaussianScaleRotationAndOpacity() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path())
          .filePath(QStringLiteral("gaussian-attributes.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex 1\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property float f_dc_0\n"
            "property float f_dc_1\n"
            "property float f_dc_2\n"
            "property float opacity\n"
            "property float scale_0\n"
            "property float scale_1\n"
            "property float scale_2\n"
            "property float rot_0\n"
            "property float rot_1\n"
            "property float rot_2\n"
            "property float rot_3\n"
            "end_header\n");
  for (const float value :
       {1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.69314718056F,
        -0.69314718056F, 2.0F, 0.0F, 0.0F, 0.0F}) {
    writeLittleEndianFloat(ply, value);
  }
  ply.close();

  const gsw::PointCloudData data = gsw::PlyPointCloudLoader::load(plyPath);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.hasGaussianAttributes);
  const gsw::PointCloudVertex &vertex = data.vertices.first();
  QVERIFY(qAbs(vertex.opacity - 0.5F) < 1.0e-6F);
  QVERIFY(qAbs(vertex.scaleX - 1.0F) < 1.0e-6F);
  QVERIFY(qAbs(vertex.scaleY - 2.0F) < 1.0e-5F);
  QVERIFY(qAbs(vertex.scaleZ - 0.5F) < 1.0e-5F);
  QVERIFY(qAbs(vertex.rotationW - 1.0F) < 1.0e-6F);
  QCOMPARE(vertex.rotationX, 0.0F);
  QCOMPARE(vertex.rotationY, 0.0F);
  QCOMPARE(vertex.rotationZ, 0.0F);
}

void WorkspaceDocumentTests::detectsColmapDatasetLayoutAndExecutable() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("images")));
  QFile image(root.filePath(QStringLiteral("images/frame.JPG")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  image.write("fixture");
  image.close();

  QCOMPARE(gsw::datasetImageDirectory(root.absolutePath()),
           QDir::cleanPath(root.filePath(QStringLiteral("images"))));

  QFile executable(root.filePath(QStringLiteral("colmap.exe")));
  QVERIFY(executable.open(QIODevice::WriteOnly));
  executable.write("fixture");
  executable.close();
  QCOMPARE(gsw::findColmapExecutable({}, executable.fileName()),
           QDir::toNativeSeparators(QFileInfo(executable).absoluteFilePath()));
}

void WorkspaceDocumentTests::selectsNewestVersionedColmapExecutable() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("bin")));
  QFile direct(root.filePath(QStringLiteral("bin/colmap.exe")));
  QVERIFY(direct.open(QIODevice::WriteOnly));
  direct.write("older direct fixture");
  for (const QString &version :
       {QStringLiteral("3.13.0"), QStringLiteral("4.1.0")}) {
    QVERIFY(root.mkpath(version + QStringLiteral("/bin")));
    QFile executable(
        root.filePath(version + QStringLiteral("/bin/colmap.exe")));
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write("fixture");
  }
  QVERIFY(root.mkpath(QStringLiteral("nightly/bin")));
  QFile nightly(root.filePath(QStringLiteral("nightly/bin/colmap.exe")));
  QVERIFY(nightly.open(QIODevice::WriteOnly));
  nightly.write("fixture");
  QCOMPARE(gsw::findVersionedColmapExecutable(root.absolutePath()),
           QDir::toNativeSeparators(
               root.filePath(QStringLiteral("4.1.0/bin/colmap.exe"))));
}

void WorkspaceDocumentTests::locatesVersionedColmapOnRepositoryVolume() {
  const bool hadColmapPath = qEnvironmentVariableIsSet("COLMAP_PATH");
  const bool hadColmapExe = qEnvironmentVariableIsSet("COLMAP_EXE");
  const QByteArray originalColmapPath = qgetenv("COLMAP_PATH");
  const QByteArray originalColmapExe = qgetenv("COLMAP_EXE");
  const auto restoreEnvironment = qScopeGuard([&]() {
    if (hadColmapPath) qputenv("COLMAP_PATH", originalColmapPath);
    else qunsetenv("COLMAP_PATH");
    if (hadColmapExe) qputenv("COLMAP_EXE", originalColmapExe);
    else qunsetenv("COLMAP_EXE");
  });
  qunsetenv("COLMAP_PATH");
  qunsetenv("COLMAP_EXE");
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir fixture(temporary.path());
  const QString volumeRoot = fixture.filePath(QStringLiteral("data-volume"));
  const QString repositoryRoot =
      QDir(volumeRoot).filePath(QStringLiteral("apps/gsw"));
  const QString executablePath =
      QDir(volumeRoot)
          .filePath(QStringLiteral("Tools/COLMAP/4.1.0/bin/colmap.exe"));
  QVERIFY(QDir().mkpath(repositoryRoot));
  QVERIFY(QDir().mkpath(QFileInfo(executablePath).absolutePath()));
  QFile executable(executablePath);
  QVERIFY(executable.open(QIODevice::WriteOnly));
  QCOMPARE(executable.write("fixture"), qint64(7));
  executable.close();

  QCOMPARE(gsw::findColmapExecutable(repositoryRoot, {}, {volumeRoot}),
           QDir::toNativeSeparators(executablePath));
}

void WorkspaceDocumentTests::detectsCompleteAndIncompleteColmapModels() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("sparse/0")));
  const QDir sparse(root.filePath(QStringLiteral("sparse/0")));
  for (const QString &name :
       {QStringLiteral("cameras.bin"), QStringLiteral("images.bin")}) {
    QFile file(sparse.filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
  }
  QVERIFY(!gsw::hasRecognizedColmapScene(root.absolutePath()));
  QFile points(sparse.filePath(QStringLiteral("points3D.bin")));
  QVERIFY(points.open(QIODevice::WriteOnly));
  points.close();
  QVERIFY(gsw::hasRecognizedColmapScene(root.absolutePath()));
  QVERIFY(gsw::hasRecognizedTrainingScene(root.absolutePath()));
  QVERIFY(gsw::hasColmapWorkingData(root.absolutePath()));
}

void WorkspaceDocumentTests::selectsBrushStrokeAndHonorsVisibility() {
  const QVector<gsw::PointPosition> positions = {{-0.5F, 0.0F, -0.3F},
                                                 {0.0F, 0.0F, -0.4F},
                                                 {0.5F, 0.0F, -0.2F},
                                                 {0.0F, 0.6F, 0.0F},
                                                 {0.0F, 0.0F, 0.6F}};
  QBitArray deleted(positions.size(), false);
  QMatrix4x4 identity;
  identity.setToIdentity();

  gsw::ScreenSelectionRequest request;
  request.shape = gsw::ScreenSelectionShape::Brush;
  request.path = {QPointF(45.0, 100.0), QPointF(155.0, 100.0)};
  request.brushRadius = 8.0;
  request.visibleOnly = false;

  QCOMPARE(gsw::selectSourcePoints(positions, deleted, identity,
                                   QSize(200, 200), request),
           QVector<quint32>({0U, 1U, 2U, 4U}));

  request.visibleOnly = true;
  QCOMPARE(gsw::selectSourcePoints(positions, deleted, identity,
                                   QSize(200, 200), request),
           QVector<quint32>({0U, 1U, 2U}));

  deleted.setBit(1);
  QCOMPARE(gsw::selectSourcePoints(positions, deleted, identity,
                                   QSize(200, 200), request),
           QVector<quint32>({0U, 2U, 4U}));
}

void WorkspaceDocumentTests::tracksSelectionDeletionUndoAndRedo() {
  gsw::SceneEditModel edits;
  edits.reset(5);
  edits.applySelection({1U, 3U}, gsw::SelectionOperation::Replace);
  QCOMPARE(edits.selectedCount(), 2);
  QCOMPARE(edits.deleteSelection(), 2);
  QCOMPARE(edits.selectedCount(), 0);
  QCOMPARE(edits.deletedCount(), 2);
  QVERIFY(edits.canUndo());
  QVERIFY(edits.hasUnsavedChanges());

  QCOMPARE(edits.undo(), 2);
  QCOMPARE(edits.selectedCount(), 2);
  QCOMPARE(edits.deletedCount(), 0);
  QVERIFY(edits.canRedo());

  QCOMPARE(edits.redo(), 2);
  QCOMPARE(edits.selectedCount(), 0);
  QCOMPARE(edits.deletedCount(), 2);
  edits.markExported();
  QVERIFY(!edits.hasUnsavedChanges());

  edits.applySelection({0U, 2U, 4U}, gsw::SelectionOperation::Replace);
  edits.applySelection({2U}, gsw::SelectionOperation::Subtract);
  QCOMPARE(edits.selectedCount(), 2);
  edits.invertSelection();
  QCOMPARE(edits.selectedCount(), 1);
}

void WorkspaceDocumentTests::exportsFilteredAsciiPlyWithOriginalFields() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString sourcePath =
      QDir(temporary.path()).filePath(QStringLiteral("source-ascii.ply"));
  const QString targetPath =
      QDir(temporary.path()).filePath(QStringLiteral("cropped-ascii.ply"));
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write("ply\n"
               "format ascii 1.0\n"
               "comment keep-this-metadata\n"
               "element vertex 4\n"
               "property float x\n"
               "property float y\n"
               "property float z\n"
               "property float opacity\n"
               "element metadata 1\n"
               "property int id\n"
               "end_header\n"
               "0 0 0 0.11\n"
               "1 0 0 0.22\n"
               "2 0 0 0.33\n"
               "3 0 0 0.44\n"
               "42\n");
  source.close();

  QBitArray deleted(4, false);
  deleted.setBit(1);
  deleted.setBit(3);
  QString error;
  QVERIFY2(gsw::PlyPointCloudLoader::writeFiltered(sourcePath, targetPath,
                                                   deleted, &error),
           qPrintable(error));

  QFile output(targetPath);
  QVERIFY(output.open(QIODevice::ReadOnly));
  const QByteArray bytes = output.readAll();
  QVERIFY(bytes.contains("comment keep-this-metadata\n"));
  QVERIFY(bytes.contains("element vertex 2\n"));
  QVERIFY(bytes.contains("0 0 0 0.11\n"));
  QVERIFY(bytes.contains("2 0 0 0.33\n"));
  QVERIFY(!bytes.contains("1 0 0 0.22\n"));
  QVERIFY(!bytes.contains("3 0 0 0.44\n"));
  QVERIFY(bytes.endsWith("42\n"));

  const gsw::PointCloudData cropped =
      gsw::PlyPointCloudLoader::load(targetPath);
  QVERIFY2(cropped.isValid(), qPrintable(cropped.error));
  QCOMPARE(cropped.sourcePositions.size(), 2);
  QCOMPARE(cropped.sourcePositions.at(0).x, 0.0F);
  QCOMPARE(cropped.sourcePositions.at(1).x, 2.0F);
}

void WorkspaceDocumentTests::exportsFilteredBinaryPlyWithoutReencoding() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString sourcePath =
      QDir(temporary.path()).filePath(QStringLiteral("source-binary.ply"));
  const QString targetPath =
      QDir(temporary.path()).filePath(QStringLiteral("cropped-binary.ply"));
  const QByteArray header = "ply\n"
                            "format binary_little_endian 1.0\n"
                            "element vertex 3\n"
                            "property float x\n"
                            "property float y\n"
                            "property float z\n"
                            "property float opacity\n"
                            "end_header\n";
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write(header);
  for (int index = 0; index < 3; ++index) {
    writeLittleEndianFloat(source, static_cast<float>(index));
    writeLittleEndianFloat(source, 1.0F);
    writeLittleEndianFloat(source, 2.0F);
    writeLittleEndianFloat(source, 0.1F + static_cast<float>(index));
  }
  source.close();

  QBitArray deleted(3, false);
  deleted.setBit(1);
  QString error;
  QVERIFY2(gsw::PlyPointCloudLoader::writeFiltered(sourcePath, targetPath,
                                                   deleted, &error),
           qPrintable(error));

  QFile output(targetPath);
  QVERIFY(output.open(QIODevice::ReadOnly));
  const QByteArray bytes = output.readAll();
  const qsizetype payloadOffset =
      bytes.indexOf("end_header\n") + QByteArray("end_header\n").size();
  QVERIFY(payloadOffset > 0);
  QByteArray expectedPayload;
  for (const int index : {0, 2}) {
    appendLittleEndianFloat(expectedPayload, static_cast<float>(index));
    appendLittleEndianFloat(expectedPayload, 1.0F);
    appendLittleEndianFloat(expectedPayload, 2.0F);
    appendLittleEndianFloat(expectedPayload, 0.1F + static_cast<float>(index));
  }
  QCOMPARE(bytes.mid(payloadOffset), expectedPayload);
  QVERIFY(bytes.left(payloadOffset).contains("element vertex 2\n"));
}

void WorkspaceDocumentTests::createsUntitledProjectWithoutAProjectFile() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working")));

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.createUntitled(root.filePath(QStringLiteral("working")),
                                  QStringLiteral("未命名工程"), &error),
           qPrintable(error));

  QVERIFY(project.hasProject());
  QVERIFY(project.isUntitled());
  QVERIFY(!project.isModified());
  QCOMPARE(project.projectName(), QStringLiteral("未命名工程"));
  QCOMPARE(project.projectFilePath(), QString());
  QCOMPARE(project.rootPath(),
           QDir::cleanPath(root.filePath(QStringLiteral("working"))));
}

void WorkspaceDocumentTests::serializesCurrentUnsavedStateForRecovery() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/images")));
  gsw::WorkspaceDocument project;
  QString error;
  const QString workingRoot = root.filePath(QStringLiteral("working"));
  const QString dataset = root.filePath(QStringLiteral("working/images"));
  QVERIFY2(project.createUntitled(workingRoot, QStringLiteral("现场工程"),
                                  &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(dataset, &error), qPrintable(error));

  const QJsonDocument recovery =
      QJsonDocument::fromJson(project.recoveryManifestJson());
  QVERIFY(recovery.isObject());
  QCOMPARE(recovery.object().value(QStringLiteral("projectName")).toString(),
           QStringLiteral("现场工程"));
  QCOMPARE(recovery.object().value(QStringLiteral("rootPath")).toString(),
           QDir::cleanPath(workingRoot));
  QCOMPARE(recovery.object().value(QStringLiteral("datasetPath")).toString(),
           QStringLiteral("images"));
  QVERIFY(project.projectFilePath().isEmpty());
  QVERIFY(project.isModified());
}

void WorkspaceDocumentTests::
    savesRecoverableManifestWithoutMovingActiveWorkspace() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/images")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  QFile image(root.filePath(
      QStringLiteral("working/datasets/capture/images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("image"), qint64(5));
  image.close();

  gsw::WorkspaceDocument project;
  QString error;
  const QString workingRoot = root.filePath(QStringLiteral("working"));
  QVERIFY2(
      project.createUntitled(workingRoot, QStringLiteral("未命名工程"), &error),
      qPrintable(error));
  QVERIFY2(
      project.setDatasetPath(
          root.filePath(QStringLiteral("working/datasets/capture")), &error),
      qPrintable(error));

  const QString projectPath =
      root.filePath(QStringLiteral("saved/capture.gsw.json"));
  QVERIFY2(project.saveManifest(projectPath, &error), qPrintable(error));

  QCOMPARE(project.rootPath(), QDir::cleanPath(workingRoot));
  QVERIFY(project.hasPendingDataMigration());
  QVERIFY(QFileInfo::exists(projectPath));
  QVERIFY(
      !QFileInfo::exists(root.filePath(QStringLiteral("saved/capture.files"))));

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectPath, &error), qPrintable(error));
  QCOMPARE(restored.rootPath(), QDir::cleanPath(workingRoot));
  QVERIFY(restored.hasPendingDataMigration());
  QCOMPARE(restored.datasetPath(), QDir::cleanPath(root.filePath(QStringLiteral(
                                       "working/datasets/capture"))));
}

void WorkspaceDocumentTests::
    finalizesPendingMigrationWithDataWrittenAfterManifestSave() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/images")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  gsw::WorkspaceDocument active;
  QString error;
  const QString workingRoot = root.filePath(QStringLiteral("working"));
  const QString datasetPath =
      root.filePath(QStringLiteral("working/datasets/capture"));
  QVERIFY2(
      active.createUntitled(workingRoot, QStringLiteral("未命名工程"), &error),
      qPrintable(error));
  QVERIFY2(active.setDatasetPath(datasetPath, &error), qPrintable(error));

  const QString projectPath =
      root.filePath(QStringLiteral("saved/capture.gsw.json"));
  QVERIFY2(active.saveManifest(projectPath, &error), qPrintable(error));

  QFile completedFrame(
      QDir(datasetPath).filePath(QStringLiteral("images/completed.jpg")));
  QVERIFY(completedFrame.open(QIODevice::WriteOnly));
  QCOMPARE(completedFrame.write("completed"), qint64(9));
  completedFrame.close();

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectPath, &error), qPrintable(error));
  QVERIFY(restored.hasPendingDataMigration());
  QVERIFY2(restored.finalizeDataMigration(&error), qPrintable(error));

  const QString savedRoot =
      root.filePath(QStringLiteral("saved/capture.files"));
  QCOMPARE(restored.rootPath(), QDir::cleanPath(savedRoot));
  QCOMPARE(restored.datasetPath(), QDir::cleanPath(QDir(savedRoot).filePath(
                                       QStringLiteral("datasets/capture"))));
  QVERIFY(!restored.hasPendingDataMigration());
  QVERIFY(QFileInfo::exists(QDir(savedRoot).filePath(
      QStringLiteral("datasets/capture/images/completed.jpg"))));

  QFile serialized(projectPath);
  QVERIFY(serialized.open(QIODevice::ReadOnly));
  const QJsonObject projectJson =
      QJsonDocument::fromJson(serialized.readAll()).object();
  QCOMPARE(projectJson.value(QStringLiteral("rootPath")).toString(),
           QStringLiteral("capture.files"));
  QVERIFY(!projectJson.contains(QStringLiteral("pendingDataRoot")));
}

void WorkspaceDocumentTests::resumesMigrationAfterDataDirectoryWasPublished() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  gsw::WorkspaceDocument active;
  QString error;
  const QString workingRoot = root.filePath(QStringLiteral("working"));
  QVERIFY2(active.createUntitled(workingRoot, QStringLiteral("未命名工程"),
                                 &error),
           qPrintable(error));
  const QString projectPath =
      root.filePath(QStringLiteral("saved/recovered.gsw.json"));
  QVERIFY2(active.saveManifest(projectPath, &error), qPrintable(error));

  const QString publishedRoot =
      root.filePath(QStringLiteral("saved/recovered.files"));
  QVERIFY(root.mkpath(QStringLiteral("saved/recovered.files/output")));
  QFile checkpoint(
      QDir(publishedRoot).filePath(QStringLiteral("output/checkpoint.ply")));
  QVERIFY(checkpoint.open(QIODevice::WriteOnly));
  QCOMPARE(checkpoint.write("checkpoint"), qint64(10));
  checkpoint.close();

  QFile marker(QDir(publishedRoot).filePath(
      QStringLiteral(".gsw-data-migration.json")));
  QVERIFY(marker.open(QIODevice::WriteOnly));
  const QJsonObject markerJson{
      {QStringLiteral("version"), 1},
      {QStringLiteral("projectFilePath"),
       QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath())},
      {QStringLiteral("sourceRoot"),
       QDir::cleanPath(QFileInfo(workingRoot).absoluteFilePath())}};
  QCOMPARE(marker.write(
               QJsonDocument(markerJson).toJson(QJsonDocument::Compact)),
           QJsonDocument(markerJson).toJson(QJsonDocument::Compact).size());
  marker.close();

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectPath, &error), qPrintable(error));
  QVERIFY(restored.hasPendingDataMigration());
  QVERIFY2(restored.finalizeDataMigration(&error), qPrintable(error));

  QCOMPARE(restored.rootPath(), QDir::cleanPath(publishedRoot));
  QVERIFY(!restored.hasPendingDataMigration());
  QVERIFY(QFileInfo::exists(QDir(publishedRoot).filePath(
      QStringLiteral("output/checkpoint.ply"))));
  QVERIFY(!QFileInfo::exists(QDir(publishedRoot).filePath(
      QStringLiteral(".gsw-data-migration.json"))));
}

void WorkspaceDocumentTests::loadsTwoScaleSurfelsWithoutChangingSource() {
  QTemporaryDir temporary;
  const auto path = QDir(temporary.path()).filePath(QStringLiteral("surfel.ply"));
  const QByteArray contents = "ply\nformat ascii 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\nproperty float f_dc_0\n"
      "property float f_dc_1\nproperty float f_dc_2\nproperty float opacity\n"
      "property float scale_0\nproperty float scale_1\nproperty float rot_0\n"
      "property float rot_1\nproperty float rot_2\nproperty float rot_3\nend_header\n"
      "0 0 0 0 0 0 0 0 0 1 0 0 0\n";
  QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(contents); file.close();
  const auto data = gsw::PlyPointCloudLoader::load(path);
  QVERIFY2(data.isValid(), qPrintable(data.error));
  QVERIFY(data.hasGaussianAttributes);
  QVERIFY(data.hasSurfelAttributes);
  QCOMPARE(data.vertices.size(), 1);
  QCOMPARE(data.vertices.first().scaleX, 1.0f);
  QCOMPARE(data.vertices.first().scaleY, 1.0f);
  QCOMPARE(data.vertices.first().scaleZ, 0.001f);
  QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), contents);
}

void WorkspaceDocumentTests::savesAndLoadsPortableProject() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/images")));
  QVERIFY(root.mkpath(QStringLiteral("working/models")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  QDir working(root.filePath(QStringLiteral("working")));

  QFile image(working.filePath(QStringLiteral("images/frame_0001.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  image.close();

  const QString plyPath = working.filePath(QStringLiteral("models/scene.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\nformat ascii 1.0\nelement vertex 2\nproperty float "
            "x\nend_header\n0\n1\n");
  ply.close();

  gsw::WorkspaceDocument source;
  QString error;
  QVERIFY2(source.createUntitled(working.path(), QStringLiteral("未命名工程"),
                                 &error),
           qPrintable(error));
  QVERIFY2(
      source.setDatasetPath(working.filePath(QStringLiteral("images")), &error),
      qPrintable(error));
  QVERIFY2(source.setScenePath(plyPath, &error), qPrintable(error));
  const QVector3D sceneTranslation(1.25F, -2.5F, 0.75F);
  const QQuaternion sceneRotation = QQuaternion::fromEulerAngles(
      QVector3D(12.0F, -24.0F, 37.0F));
  const QVector3D sceneScale(1.5F, 0.75F, 2.25F);
  QVERIFY2(source.setSceneTransform(sceneTranslation, sceneRotation, sceneScale,
                                    &error),
           qPrintable(error));
  const QString projectPath =
      root.filePath(QStringLiteral("saved/test.gsw.json"));
  QVERIFY2(source.save(projectPath, &error), qPrintable(error));
  QVERIFY(!source.isModified());
  QVERIFY(!source.isUntitled());
  QCOMPARE(source.projectName(), QStringLiteral("test"));
  QCOMPARE(source.rootPath(),
           QDir::cleanPath(root.filePath(QStringLiteral("saved/test.files"))));
  QCOMPARE(source.datasetPath(), QDir::cleanPath(root.filePath(QStringLiteral(
                                     "saved/test.files/images"))));
  QCOMPARE(source.scenePath(), QDir::cleanPath(root.filePath(QStringLiteral(
                                   "saved/test.files/models/scene.ply"))));
  QVERIFY(QFileInfo::exists(
      root.filePath(QStringLiteral("saved/test.files/images/frame_0001.jpg"))));

  QFile serialized(projectPath);
  QVERIFY(serialized.open(QIODevice::ReadOnly));
  const QJsonObject projectJson =
      QJsonDocument::fromJson(serialized.readAll()).object();
  QCOMPARE(projectJson.value(QStringLiteral("rootPath")).toString(),
           QStringLiteral("test.files"));
  QCOMPARE(projectJson.value(QStringLiteral("datasetPath")).toString(),
           QStringLiteral("images"));
  QCOMPARE(projectJson.value(QStringLiteral("scenePath")).toString(),
           QStringLiteral("models/scene.ply"));
  const QJsonArray translation =
      projectJson.value(QStringLiteral("sceneTransform"))
          .toObject()
          .value(QStringLiteral("translation"))
          .toArray();
  QCOMPARE(translation.size(), 3);
  QCOMPARE(translation.at(0).toDouble(), 1.25);
  QCOMPARE(translation.at(1).toDouble(), -2.5);
  QCOMPARE(translation.at(2).toDouble(), 0.75);
  const QJsonArray rotation =
      projectJson.value(QStringLiteral("sceneTransform"))
          .toObject()
          .value(QStringLiteral("rotation"))
          .toArray();
  QCOMPARE(rotation.size(), 4);
  const QJsonArray scale = projectJson.value(QStringLiteral("sceneTransform"))
                               .toObject()
                               .value(QStringLiteral("scale"))
                               .toArray();
  QCOMPARE(scale.size(), 3);
  QCOMPARE(scale.at(0).toDouble(), 1.5);
  QCOMPARE(scale.at(1).toDouble(), 0.75);
  QCOMPARE(scale.at(2).toDouble(), 2.25);

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectPath, &error), qPrintable(error));
  QCOMPARE(restored.rootPath(),
           QDir::cleanPath(root.filePath(QStringLiteral("saved/test.files"))));
  QCOMPARE(restored.datasetPath(), QDir::cleanPath(root.filePath(QStringLiteral(
                                       "saved/test.files/images"))));
  QCOMPARE(restored.scenePath(), QDir::cleanPath(root.filePath(QStringLiteral(
                                     "saved/test.files/models/scene.ply"))));
  QCOMPARE(restored.sceneTranslation(), sceneTranslation);
  QVERIFY(1.0F - std::abs(QQuaternion::dotProduct(
                      restored.sceneRotation().normalized(),
                      sceneRotation.normalized())) <
          1.0e-6F);
  QCOMPARE(restored.sceneScale(), sceneScale);
  QCOMPARE(restored.imageCount(), 1);
  QCOMPARE(restored.sceneMetadata().vertexCount, 2);
}

void WorkspaceDocumentTests::saveAsCopiesManagedDataIntoANewLinkedDirectory() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/images")));
  QVERIFY(root.mkpath(QStringLiteral("first")));
  QVERIFY(root.mkpath(QStringLiteral("second")));

  QFile image(root.filePath(
      QStringLiteral("working/datasets/capture/images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("image"), qint64(5));
  image.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.createUntitled(root.filePath(QStringLiteral("working")),
                                  QStringLiteral("未命名工程"), &error),
           qPrintable(error));
  QVERIFY2(
      project.setDatasetPath(
          root.filePath(QStringLiteral("working/datasets/capture")), &error),
      qPrintable(error));

  const QString firstProject =
      root.filePath(QStringLiteral("first/capture.gsw.json"));
  QVERIFY2(project.save(firstProject, &error), qPrintable(error));
  const QString firstData =
      root.filePath(QStringLiteral("first/capture.files"));
  QVERIFY(QFileInfo::exists(QDir(firstData).filePath(
      QStringLiteral("datasets/capture/images/frame.jpg"))));

  const QString secondProject =
      root.filePath(QStringLiteral("second/capture-copy.gsw.json"));
  QVERIFY2(project.save(secondProject, &error), qPrintable(error));
  const QString secondData =
      root.filePath(QStringLiteral("second/capture-copy.files"));
  QCOMPARE(project.rootPath(), QDir::cleanPath(secondData));
  QCOMPARE(project.datasetPath(),
           QDir::cleanPath(
               QDir(secondData).filePath(QStringLiteral("datasets/capture"))));
  QVERIFY(QFileInfo::exists(
      QDir(secondData)
          .filePath(QStringLiteral("datasets/capture/images/frame.jpg"))));
  QVERIFY(QFileInfo::exists(QDir(firstData).filePath(
      QStringLiteral("datasets/capture/images/frame.jpg"))));
}

void WorkspaceDocumentTests::firstSaveKeepsExternalDatasetAsALink() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working")));
  QVERIFY(root.mkpath(QStringLiteral("external/images")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  const QString externalDataset = root.filePath(QStringLiteral("external"));
  QFile image(
      QDir(externalDataset).filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  image.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.createUntitled(root.filePath(QStringLiteral("working")),
                                  QStringLiteral("未命名工程"), &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(externalDataset, &error), qPrintable(error));

  const QString projectPath =
      root.filePath(QStringLiteral("saved/linked.gsw.json"));
  QVERIFY2(project.save(projectPath, &error), qPrintable(error));
  QCOMPARE(project.datasetPath(), QDir::cleanPath(externalDataset));

  QFile serialized(projectPath);
  QVERIFY(serialized.open(QIODevice::ReadOnly));
  const QJsonObject projectJson =
      QJsonDocument::fromJson(serialized.readAll()).object();
  QCOMPARE(projectJson.value(QStringLiteral("datasetPath")).toString(),
           QDir::cleanPath(externalDataset));
  QVERIFY(!QFileInfo::exists(root.filePath(
      QStringLiteral("saved/linked.files/external/images/frame.jpg"))));
}

void WorkspaceDocumentTests::clearsManagedDatasetAndPersistsSceneAssociation() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/images")));
  QVERIFY(root.mkpath(QStringLiteral("working/output")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  const QString dataset =
      root.filePath(QStringLiteral("working/datasets/capture"));
  QFile image(QDir(dataset).filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("image"), qint64(5));
  image.close();
  QVERIFY(QDir(dataset).mkpath(QStringLiteral("sparse/0")));

  const QString scene =
      root.filePath(QStringLiteral("working/output/point_cloud.ply"));
  QFile ply(scene);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  QVERIFY(ply.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float "
                    "x\nend_header\n0\n") > 0);
  ply.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.create(root.filePath(QStringLiteral("working")), &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(dataset, &error), qPrintable(error));
  QVERIFY2(project.setScenePath(scene, &error), qPrintable(error));
  const QString projectFile =
      root.filePath(QStringLiteral("saved/capture.gsw.json"));
  QVERIFY2(project.save(projectFile, &error), qPrintable(error));
  QVERIFY(project.isDatasetManaged());
  const QString activeDataset = project.datasetPath();
  const QString activeScene = project.scenePath();

  gsw::ImportCleanupResult result;
  QVERIFY2(project.clearImportedData({}, &result, &error), qPrintable(error));
  QVERIFY(result.datasetCleared);
  QVERIFY(result.managedDatasetRemoved);
  QVERIFY(!result.sceneCleared);
  QCOMPARE(result.previousDatasetPath, QDir::cleanPath(activeDataset));
  QCOMPARE(project.datasetPath(), QString());
  QCOMPARE(project.imageCount(), qint64(0));
  QCOMPARE(project.scenePath(), QDir::cleanPath(activeScene));
  QVERIFY(!QFileInfo::exists(activeDataset));
  QVERIFY(QFileInfo::exists(activeScene));

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectFile, &error), qPrintable(error));
  QCOMPARE(restored.datasetPath(), QString());
  QCOMPARE(restored.scenePath(), QDir::cleanPath(activeScene));
}

void WorkspaceDocumentTests::clearsExternalDatasetWithoutDeletingSourceFiles() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));
  QVERIFY(root.mkpath(QStringLiteral("external/images")));

  const QString dataset = root.filePath(QStringLiteral("external"));
  const QString imagePath =
      QDir(dataset).filePath(QStringLiteral("images/frame.jpg"));
  QFile image(imagePath);
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("image"), qint64(5));
  image.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.create(root.filePath(QStringLiteral("working")), &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(dataset, &error), qPrintable(error));
  const QString projectFile =
      root.filePath(QStringLiteral("saved/linked.gsw.json"));
  QVERIFY2(project.save(projectFile, &error), qPrintable(error));
  QVERIFY(!project.isDatasetManaged());

  gsw::ImportCleanupResult result;
  QVERIFY2(project.clearImportedData({}, &result, &error), qPrintable(error));
  QVERIFY(result.datasetCleared);
  QVERIFY(!result.managedDatasetRemoved);
  QVERIFY(QFileInfo::exists(imagePath));

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectFile, &error), qPrintable(error));
  QCOMPARE(restored.datasetPath(), QString());
  QVERIFY(QFileInfo::exists(imagePath));
}

void WorkspaceDocumentTests::clearsSceneAssociationWithoutDeletingSceneFile() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/output")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  const QString sourceScene =
      root.filePath(QStringLiteral("working/output/point_cloud.ply"));
  QFile ply(sourceScene);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  QVERIFY(ply.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float "
                    "x\nend_header\n0\n") > 0);
  ply.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.create(root.filePath(QStringLiteral("working")), &error),
           qPrintable(error));
  QVERIFY2(project.setScenePath(sourceScene, &error), qPrintable(error));
  const QString projectFile =
      root.filePath(QStringLiteral("saved/scene.gsw.json"));
  QVERIFY2(project.save(projectFile, &error), qPrintable(error));
  const QString activeScene = project.scenePath();

  gsw::ImportCleanupOptions options;
  options.clearDataset = false;
  options.clearScene = true;
  gsw::ImportCleanupResult result;
  QVERIFY2(project.clearImportedData(options, &result, &error),
           qPrintable(error));
  QVERIFY(!result.datasetCleared);
  QVERIFY(result.sceneCleared);
  QCOMPARE(result.previousScenePath, QDir::cleanPath(activeScene));
  QCOMPARE(project.scenePath(), QString());
  QVERIFY(QFileInfo::exists(activeScene));

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectFile, &error), qPrintable(error));
  QCOMPARE(restored.scenePath(), QString());
  QVERIFY(QFileInfo::exists(activeScene));
}

void WorkspaceDocumentTests::rollsBackManagedCleanupWhenManifestSaveFails() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/images")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  const QString sourceDataset =
      root.filePath(QStringLiteral("working/datasets/capture"));
  QFile image(
      QDir(sourceDataset).filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("image"), qint64(5));
  image.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.create(root.filePath(QStringLiteral("working")), &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(sourceDataset, &error), qPrintable(error));
  const QString projectFile =
      root.filePath(QStringLiteral("saved/rollback.gsw.json"));
  QVERIFY2(project.save(projectFile, &error), qPrintable(error));
  const QString activeDataset = project.datasetPath();
  QVERIFY(QFile::remove(projectFile));
  QVERIFY(QDir().mkpath(projectFile));

  gsw::ImportCleanupResult result;
  QVERIFY(!project.clearImportedData({}, &result, &error));
  QVERIFY(error.contains(QStringLiteral("Unable to save")));
  QCOMPARE(project.datasetPath(), QDir::cleanPath(activeDataset));
  QCOMPARE(project.imageCount(), qint64(1));
  QVERIFY(QFileInfo::exists(
      QDir(activeDataset).filePath(QStringLiteral("images/frame.jpg"))));
  QVERIFY(!result.datasetCleared);
}

void WorkspaceDocumentTests::clearsOnlyManagedReconstructionData() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/images")));
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/sparse/0")));
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/distorted")));
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/capture/stereo")));
  QVERIFY(root.mkpath(
      QStringLiteral("working/datasets/capture/.alignment_cache/sparse/0")));
  QVERIFY(root.mkpath(
      QStringLiteral("working/datasets/capture/.gsw/previews/colmap/session")));
  QVERIFY(root.mkpath(QStringLiteral("working/output")));
  QVERIFY(root.mkpath(QStringLiteral("saved")));

  const QString dataset =
      root.filePath(QStringLiteral("working/datasets/capture"));
  const QString imagePath =
      QDir(dataset).filePath(QStringLiteral("images/frame.jpg"));
  QFile image(imagePath);
  QVERIFY(image.open(QIODevice::WriteOnly));
  QCOMPARE(image.write("image"), qint64(5));
  image.close();
  for (const QString &relative :
       {QStringLiteral("sparse/0/cameras.bin"),
        QStringLiteral("sparse/0/images.bin"),
        QStringLiteral("sparse/0/points3D.bin"),
        QStringLiteral("distorted/database.db"),
        QStringLiteral("stereo/depth.bin"),
        QStringLiteral(".alignment_cache/sparse/0/cameras.bin"),
        QStringLiteral(".gsw/previews/colmap/session/sparse_000001.ply")}) {
    QFile artifact(QDir(dataset).filePath(relative));
    QVERIFY(artifact.open(QIODevice::WriteOnly));
    QVERIFY(artifact.write("fixture") > 0);
  }

  const QString sourceScene =
      root.filePath(QStringLiteral("working/output/point_cloud.ply"));
  QFile ply(sourceScene);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  QVERIFY(ply.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float "
                    "x\nend_header\n0\n") > 0);
  ply.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.create(root.filePath(QStringLiteral("working")), &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(dataset, &error), qPrintable(error));
  QVERIFY2(project.setScenePath(sourceScene, &error), qPrintable(error));
  const QString projectFile =
      root.filePath(QStringLiteral("saved/reconstruction.gsw.json"));
  QVERIFY2(project.save(projectFile, &error), qPrintable(error));
  const QString activeDataset = project.datasetPath();
  const QString activeScene = project.scenePath();
  QVERIFY(project.hasManagedReconstructionData());

  gsw::ReconstructionCleanupResult result;
  QVERIFY2(project.clearReconstructionData(&result, &error),
           qPrintable(error));
  QVERIFY(!result.removedPaths.isEmpty());
  QVERIFY(result.cleanupPendingPath.isEmpty());
  QCOMPARE(project.datasetPath(), QDir::cleanPath(activeDataset));
  QCOMPARE(project.scenePath(), QDir::cleanPath(activeScene));
  QCOMPARE(project.imageCount(), qint64(1));
  QVERIFY(QFileInfo::exists(
      QDir(activeDataset).filePath(QStringLiteral("images/frame.jpg"))));
  QVERIFY(QFileInfo::exists(activeScene));
  QVERIFY(!QFileInfo::exists(
      QDir(activeDataset).filePath(QStringLiteral("sparse"))));
  QVERIFY(!QFileInfo::exists(
      QDir(activeDataset).filePath(QStringLiteral("distorted"))));
  QVERIFY(!QFileInfo::exists(
      QDir(activeDataset).filePath(QStringLiteral("stereo"))));
  QVERIFY(!QFileInfo::exists(
      QDir(activeDataset).filePath(QStringLiteral(".alignment_cache"))));
  QVERIFY(!QFileInfo::exists(QDir(activeDataset).filePath(
      QStringLiteral(".gsw/previews/colmap"))));
  QVERIFY(!project.hasManagedReconstructionData());

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectFile, &error), qPrintable(error));
  QCOMPARE(restored.datasetPath(), QDir::cleanPath(activeDataset));
  QCOMPARE(restored.scenePath(), QDir::cleanPath(activeScene));
  QCOMPARE(restored.imageCount(), qint64(1));
}

void WorkspaceDocumentTests::refusesToModifyExternalReconstructionData() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working")));
  QVERIFY(root.mkpath(QStringLiteral("external/images")));
  QVERIFY(root.mkpath(QStringLiteral("external/sparse/0")));

  const QString externalDataset = root.filePath(QStringLiteral("external"));
  QFile image(
      QDir(externalDataset).filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  QVERIFY(image.write("image") > 0);
  image.close();
  const QString sparseFile =
      QDir(externalDataset).filePath(QStringLiteral("sparse/0/cameras.bin"));
  QFile sparse(sparseFile);
  QVERIFY(sparse.open(QIODevice::WriteOnly));
  QVERIFY(sparse.write("fixture") > 0);
  sparse.close();

  gsw::WorkspaceDocument project;
  QString error;
  QVERIFY2(project.create(root.filePath(QStringLiteral("working")), &error),
           qPrintable(error));
  QVERIFY2(project.setDatasetPath(externalDataset, &error), qPrintable(error));
  QVERIFY(!project.hasManagedReconstructionData());

  gsw::ReconstructionCleanupResult result;
  QVERIFY(!project.clearReconstructionData(&result, &error));
  QVERIFY(error.contains(QStringLiteral("external"), Qt::CaseInsensitive));
  QCOMPARE(project.datasetPath(), QDir::cleanPath(externalDataset));
  QVERIFY(QFileInfo::exists(sparseFile));
}

void WorkspaceDocumentTests::retainsSourceCameraCalibrationAndLegacyDefaults() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  auto calibrated = camera("calibrated", vector3(0, 0, 0),
      rotation(vector3(1, 0, 0), vector3(0, 1, 0), vector3(0, 0, 1)), 1920, 1080);
  calibrated.insert("fx", 1300.0);
  calibrated.insert("fy", 1200.0);
  auto legacy = calibrated;
  legacy.remove("fx"); legacy.remove("fy");
  QVERIFY(writeJson(QDir(temporary.path()).filePath("cameras.json"), QJsonDocument(QJsonArray{calibrated, legacy})));
  const auto trajectory = gsw::CameraTrajectory::loadForScene(temporary.path());
  QCOMPARE(trajectory.cameras().size(), qsizetype(2));
  const auto first = trajectory.cameras().at(0);
  QVERIFY(std::abs(first.verticalFovDegrees - 48.45549F) < 0.001F);
  QVERIFY(std::abs(first.focalAspectCorrection - 1300.0F / 1200.0F) < 1.0e-6F);
  QCOMPARE(trajectory.cameras().at(1).verticalFovDegrees, 46.0F);
  QCOMPARE(trajectory.cameras().at(1).focalAspectCorrection, 1.0F);
}

void WorkspaceDocumentTests::loadsStandardCameraSidecarAndBuildsLegacyAxes() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("model/point_cloud/iteration_30000")));
  const QString model = root.filePath(QStringLiteral("model"));
  const QString scene = root.filePath(
      QStringLiteral("model/point_cloud/iteration_30000/point_cloud.ply"));
  QVERIFY(writeJson(
      QDir(model).filePath(QStringLiteral("cameras.json")),
      QJsonDocument(QJsonArray{
          camera(QStringLiteral("left.jpg"), vector3(1.0, 2.0, 3.0),
                 rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 0.0, -1.0),
                          vector3(0.0, 1.0, 0.0))),
          camera(QStringLiteral("right.jpg"), vector3(3.0, 2.0, 3.0),
                 rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 0.0, -1.0),
                          vector3(0.0, 1.0, 0.0)))})));

  const gsw::CameraTrajectory trajectory =
      gsw::CameraTrajectory::loadForScene(scene);

  QCOMPARE(trajectory.error(), QString());
  QCOMPARE(trajectory.cameras().size(), qsizetype(2));
  QCOMPARE(trajectory.invalidCameraCount(), qsizetype(0));
  QCOMPARE(QFileInfo(trajectory.sourcePath()).canonicalFilePath(),
           QFileInfo(QDir(model).filePath(QStringLiteral("cameras.json")))
               .canonicalFilePath());
  const gsw::CameraPose &first = trajectory.cameras().constFirst();
  compareVector(first.right, QVector3D(1.0F, 0.0F, 0.0F));
  compareVector(first.imageDown, QVector3D(0.0F, 0.0F, 1.0F));
  compareVector(first.forward, QVector3D(0.0F, -1.0F, 0.0F));

  const gsw::CameraTrajectoryGeometry geometry = trajectory.geometry(2.0F);
  QCOMPARE(geometry.frustums.size(), qsizetype(16));
  QCOMPARE(geometry.path.size(), qsizetype(1));
  compareVector(geometry.path.constFirst().start, QVector3D(1.0F, 2.0F, 3.0F));
  compareVector(geometry.path.constFirst().end, QVector3D(3.0F, 2.0F, 3.0F));
  compareVector(geometry.frustums.constFirst().start,
                QVector3D(1.0F, 2.0F, 3.0F));
  compareVector(geometry.frustums.constFirst().end,
                QVector3D(0.92F, 1.872F, 2.96F));
}

void WorkspaceDocumentTests::skipsMalformedCameraEntries() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString scene =
      QDir(temporary.path()).filePath(QStringLiteral("scene.ply"));
  const QJsonObject valid =
      camera(QStringLiteral("valid.jpg"), vector3(0.0, 0.0, 0.0),
             rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 1.0, 0.0),
                      vector3(0.0, 0.0, 1.0)));
  const QJsonObject invalid =
      camera(QStringLiteral("broken.jpg"), vector3(0.0, 1.0, 0.0),
             rotation(vector3(1.0, 1.0, 0.0), vector3(0.0, 0.0, 0.0),
                      vector3(0.0, 0.0, 1.0)));
  QVERIFY(
      writeJson(QDir(temporary.path()).filePath(QStringLiteral("cameras.json")),
                QJsonDocument(QJsonArray{valid, invalid})));

  const gsw::CameraTrajectory trajectory =
      gsw::CameraTrajectory::loadForScene(scene);

  QCOMPARE(trajectory.error(), QString());
  QCOMPARE(trajectory.cameras().size(), qsizetype(1));
  QCOMPARE(trajectory.invalidCameraCount(), qsizetype(1));
  QCOMPARE(trajectory.cameras().constFirst().imageName,
           QStringLiteral("valid.jpg"));
  compareVector(trajectory.cameras().constFirst().right,
                QVector3D(1.0F, 0.0F, 0.0F));
  compareVector(trajectory.cameras().constFirst().imageDown,
                QVector3D(0.0F, 1.0F, 0.0F));
  compareVector(trajectory.cameras().constFirst().forward,
                QVector3D(0.0F, 0.0F, 1.0F));
}

void WorkspaceDocumentTests::reportsMalformedCameraDocument() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QFile sidecar(
      QDir(temporary.path()).filePath(QStringLiteral("cameras.json")));
  QVERIFY(sidecar.open(QIODevice::WriteOnly));
  QCOMPARE(sidecar.write("{not-json"), qint64(9));
  sidecar.close();

  const gsw::CameraTrajectory trajectory = gsw::CameraTrajectory::loadForScene(
      QDir(temporary.path()).filePath(QStringLiteral("scene.ply")));

  QVERIFY(trajectory.cameras().isEmpty());
  QVERIFY(!trajectory.sourcePath().isEmpty());
  QVERIFY(!trajectory.error().isEmpty());
}

void WorkspaceDocumentTests::treatsMissingCameraSidecarAsOptional() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());

  const gsw::CameraTrajectory trajectory = gsw::CameraTrajectory::loadForScene(
      QDir(temporary.path()).filePath(QStringLiteral("scene.ply")));

  QVERIFY(trajectory.cameras().isEmpty());
  QCOMPARE(trajectory.sourcePath(), QString());
  QCOMPARE(trajectory.error(), QString());
  QVERIFY(trajectory.geometry(1.0F).frustums.isEmpty());
  QVERIFY(trajectory.geometry(1.0F).path.isEmpty());
}

void WorkspaceDocumentTests::reloadsCameraSidecarAfterRepair() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString scene = root.filePath(QStringLiteral("scene.ply"));
  const QString sidecar = root.filePath(QStringLiteral("cameras.json"));

  QCOMPARE(gsw::CameraTrajectory::loadForScene(scene).cameras().size(),
           qsizetype(0));

  QFile damaged(sidecar);
  QVERIFY(damaged.open(QIODevice::WriteOnly));
  QCOMPARE(damaged.write("{not-json"), qint64(9));
  damaged.close();
  QVERIFY(!gsw::CameraTrajectory::loadForScene(scene).error().isEmpty());

  QVERIFY(writeJson(sidecar,
                    QJsonDocument(QJsonArray{camera(
                        QStringLiteral("repaired.jpg"), vector3(0.0, 0.0, 0.0),
                        rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 1.0, 0.0),
                                 vector3(0.0, 0.0, 1.0)))})));
  const gsw::CameraTrajectory repaired =
      gsw::CameraTrajectory::loadForScene(scene);
  QCOMPARE(repaired.error(), QString());
  QCOMPARE(repaired.cameras().size(), qsizetype(1));
  QCOMPARE(repaired.cameras().constFirst().imageName,
           QStringLiteral("repaired.jpg"));
}

void WorkspaceDocumentTests::decimatesLargeCameraVisualization() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QJsonArray cameras;
  constexpr int cameraCount = 5002;
  for (int index = 0; index < cameraCount; ++index) {
    cameras.append(
        camera(QStringLiteral("frame_%1.jpg").arg(index),
               vector3(static_cast<double>(index), 0.0, 0.0),
               rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 1.0, 0.0),
                        vector3(0.0, 0.0, 1.0))));
  }
  QVERIFY(writeJson(root.filePath(QStringLiteral("cameras.json")),
                    QJsonDocument(cameras)));
  const gsw::CameraTrajectory trajectory = gsw::CameraTrajectory::loadForScene(
      root.filePath(QStringLiteral("scene.ply")));

  QCOMPARE(trajectory.cameras().size(), qsizetype(cameraCount));
  const gsw::CameraTrajectoryGeometry geometry = trajectory.geometry(1.0F);
  QVERIFY(geometry.decimated);
  QCOMPARE(geometry.displayedFrustumCameraCount, qsizetype(1000));
  QCOMPARE(geometry.displayedPathCameraCount, qsizetype(5000));
  QCOMPARE(geometry.frustums.size(), qsizetype(8000));
  QCOMPARE(geometry.path.size(), qsizetype(4999));
  compareVector(geometry.path.constFirst().start, QVector3D(0.0F, 0.0F, 0.0F));
  compareVector(geometry.path.constLast().end,
                QVector3D(static_cast<float>(cameraCount - 1), 0.0F, 0.0F));
}

void WorkspaceDocumentTests::removesMultipleModelsWithoutDeletingFiles() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.createUntitled(temporary.path(), "Batch", &error));
  QStringList files;
  for (int i = 0; i < 3; ++i) {
    QFile file(QDir(temporary.path()).filePath(QString("model-%1.ply").arg(i)));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n");
    file.close();
    QVERIFY(document.addScenePath(file.fileName(), &error));
    files.append(file.fileName());
  }
  const auto objects = document.sceneObjects();
  QSignalSpy changed(&document, &gsw::WorkspaceDocument::changed);
  QVERIFY(!document.removeSceneObjects({objects.first().id, "missing"}));
  QCOMPARE(document.sceneObjects().size(), 3);
  QCOMPARE(changed.count(), 0);
  QVERIFY(document.removeSceneObjects({objects.first().id, objects.last().id}));
  QCOMPARE(changed.count(), 1);
  QCOMPARE(document.sceneObjects().size(), 1);
  QCOMPARE(document.activeSceneId(), objects[1].id);
  QVERIFY(document.removeSceneObjects({objects[1].id, objects[1].id}));
  QVERIFY(document.sceneObjects().isEmpty());
  QVERIFY(document.scenePath().isEmpty());
  QVERIFY(document.activeSceneId().isEmpty());
  for (const auto &path : files) QVERIFY(QFileInfo::exists(path));
}

void WorkspaceDocumentTests::persistsCanonicalCameraIdentityAndUnitsAcrossSaveAs() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("working/datasets/known/nested")));
  const QString dataset = root.filePath(QStringLiteral("working/datasets/known"));
  QFile photo(QDir(dataset).filePath(QStringLiteral("nested/a.png")));
  QVERIFY(photo.open(QIODevice::WriteOnly));
  photo.write("fixture"); photo.close();
  QVERIFY(writeJson(QDir(dataset).filePath(QStringLiteral("camera_dataset.json")), QJsonDocument(QJsonObject{
      {QStringLiteral("schema"), QStringLiteral("gsw.camera_dataset/1")},
      {QStringLiteral("views"), QJsonArray{QJsonObject{{QStringLiteral("view_id"), QStringLiteral("view-a")},
          {QStringLiteral("image_path"), QStringLiteral("nested/a.png")}, {QStringLiteral("split"), QStringLiteral("train")}}}}})));
  const QString identity(64, QLatin1Char('a'));
  const QJsonObject info{{QStringLiteral("version"), 1}, {QStringLiteral("schema"), QStringLiteral("gsw.camera_dataset/1")},
      {QStringLiteral("metadataPath"), QStringLiteral("camera_dataset.json")}, {QStringLiteral("dataIdentity"), identity},
      {QStringLiteral("posePolicy"), QStringLiteral("locked")}, {QStringLiteral("blocked"), false},
      {QStringLiteral("scale"), QJsonObject{{QStringLiteral("status"), QStringLiteral("source_declared")},
          {QStringLiteral("source_unit"), QStringLiteral("cm")}, {QStringLiteral("meters_per_unit"), .01}}},
      {QStringLiteral("counts"), QJsonObject{{QStringLiteral("train"), 1}, {QStringLiteral("val"), 0}, {QStringLiteral("test"), 0}}}};
  QVERIFY(writeJson(QDir(dataset).filePath(QStringLiteral("camera_dataset_info.json")), QJsonDocument(info)));
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(root.filePath(QStringLiteral("working")), QStringLiteral("known"), &error), qPrintable(error));
  QVERIFY2(document.setDatasetPath(dataset, &error), qPrintable(error));
  QCOMPARE(document.cameraPosePolicy(), QStringLiteral("locked"));
  QCOMPARE(document.cameraDatasetIdentity(), identity);
  QCOMPARE(document.imageCount(), qint64(1));
  QVERIFY2(document.save(root.filePath(QStringLiteral("saved.gsw.json")), &error), qPrintable(error));
  const QString newMetadata = root.filePath(QStringLiteral("saved.files/datasets/known/camera_dataset.json"));
  QCOMPARE(document.cameraDatasetPath(), newMetadata);
  gsw::WorkspaceDocument reopened;
  QVERIFY2(reopened.load(root.filePath(QStringLiteral("saved.gsw.json")), &error), qPrintable(error));
  QCOMPARE(reopened.cameraDatasetPath(), newMetadata);
  QCOMPARE(reopened.cameraDatasetIdentity(), identity);
  QCOMPARE(reopened.cameraPosePolicy(), QStringLiteral("locked"));
  QCOMPARE(reopened.cameraDatasetInfo().value(QStringLiteral("scale")).toObject().value(QStringLiteral("source_unit")).toString(), QStringLiteral("cm"));
  QCOMPARE(reopened.cameraDatasetInfo().value(QStringLiteral("scale")).toObject().value(QStringLiteral("meters_per_unit")).toDouble(), .01);
  QVERIFY(!reopened.cameraDatasetInfo().value(QStringLiteral("blocked")).toBool());
}

void WorkspaceDocumentTests::countsOnlyCanonicalViewsAndRejectsChangedSummaryIdentity() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("project/dataset/a")));
  QVERIFY(root.mkpath(QStringLiteral("project/dataset/b")));
  QVERIFY(root.mkpath(QStringLiteral("project/dataset/images")));
  const QString dataset = root.filePath(QStringLiteral("project/dataset"));
  for (const QString &relative : {QStringLiteral("a/same.png"), QStringLiteral("b/same.png"), QStringLiteral("images/not-a-view.png")}) {
    QFile image(QDir(dataset).filePath(relative));
    QVERIFY(image.open(QIODevice::WriteOnly)); image.write("fixture");
  }
  QVERIFY(writeJson(QDir(dataset).filePath(QStringLiteral("camera_dataset.json")), QJsonDocument(QJsonObject{
      {QStringLiteral("schema"), QStringLiteral("gsw.camera_dataset/1")},
      {QStringLiteral("views"), QJsonArray{
          QJsonObject{{QStringLiteral("view_id"), QStringLiteral("train-a")}, {QStringLiteral("image_path"), QStringLiteral("a/same.png")}, {QStringLiteral("split"), QStringLiteral("train")}},
          QJsonObject{{QStringLiteral("view_id"), QStringLiteral("test-b")}, {QStringLiteral("image_path"), QStringLiteral("b/same.png")}, {QStringLiteral("split"), QStringLiteral("test")}}}}})));
  QJsonObject info{{QStringLiteral("version"), 1}, {QStringLiteral("schema"), QStringLiteral("gsw.camera_dataset/1")},
      {QStringLiteral("metadataPath"), QStringLiteral("camera_dataset.json")}, {QStringLiteral("dataIdentity"), QString(64, QLatin1Char('b'))},
      {QStringLiteral("posePolicy"), QStringLiteral("locked")}, {QStringLiteral("blocked"), false}};
  const QString infoPath = QDir(dataset).filePath(QStringLiteral("camera_dataset_info.json"));
  QVERIFY(writeJson(infoPath, QJsonDocument(info)));
  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset), qint64(2));
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.create(root.filePath(QStringLiteral("project")), &error));
  QVERIFY(document.setDatasetPath(dataset, &error));
  QVERIFY(document.saveManifest(root.filePath(QStringLiteral("current.gsw.json")), &error));
  info.insert(QStringLiteral("dataIdentity"), QString(64, QLatin1Char('c')));
  QVERIFY(writeJson(infoPath, QJsonDocument(info)));
  gsw::WorkspaceDocument reopened;
  QVERIFY(reopened.load(root.filePath(QStringLiteral("current.gsw.json")), &error));
  QCOMPARE(reopened.cameraDatasetIdentity(), QString(64, QLatin1Char('b')));
  QCOMPARE(reopened.cameraPosePolicy(), QStringLiteral("locked"));
  QVERIFY(reopened.cameraDatasetInfo().value(QStringLiteral("blocked")).toBool());
}

void WorkspaceDocumentTests::rejectsCanonicalMeshBackendsBeforeChangingTheScene() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("point_cloud/iteration_12")));
  QVERIFY(root.mkpath(QStringLiteral("point_cloud/iteration_24")));
  const auto write = [](const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  const QJsonObject selected{{QStringLiteral("modelDirectory"), root.absolutePath()},
      {QStringLiteral("iteration"), 12}, {QStringLiteral("mode"), QStringLiteral("sugar")}};
  // A legacy output and unrelated project metadata must remain usable.
  QVERIFY(write(root.filePath(QStringLiteral("cfg_args")), "Namespace(source_path='legacy')"));
  QVERIFY(write(root.filePath(QStringLiteral("project.prj")), "{\"cameraDataset\":true}"));
  QVERIFY(!gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
  const QByteArray contract("{\"schema\":\"gsw.training_camera_contract/1\",\"pose_policy\":\"locked\"}");
  const QString other = root.filePath(QStringLiteral("point_cloud/iteration_24/point_cloud.camera-scale.json"));
  QVERIFY(write(other, contract));
  QVERIFY(!gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
  const QString marker = root.filePath(QStringLiteral("training-camera-contract.json"));
  QVERIFY(write(marker, contract));
  for (const QString mode : {QStringLiteral("bounded"), QStringLiteral("unbounded"),
      QStringLiteral("sugar"), QStringLiteral("gs2mesh")}) {
    QJsonObject configuration = selected;
    configuration.insert(QStringLiteral("mode"), mode);
    QVERIFY(gsw::CameraDatasetInfo::meshGenerationBlocked(configuration));
  }
  // A damaged reserved contract must not bypass the capability gate.
  QVERIFY(write(marker, "{broken"));
  QVERIFY(gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
  QVERIFY(QFile::remove(marker));
  const QString sidecar = root.filePath(QStringLiteral("point_cloud/iteration_12/point_cloud.camera-scale.json"));
  QVERIFY(write(sidecar, contract));
  QVERIFY(gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
  QVERIFY(write(sidecar, "{\"schema\":\"gsw.coordinate_export/1\",\"training_contract\":{\"schema\":\"gsw.training_camera_contract/1\"}}"));
  QVERIFY(gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
  QVERIFY(write(sidecar, "{\"schema\":\"unrelated.legacy.metadata/1\"}"));
  QVERIFY(gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
  QVERIFY(write(sidecar, "not JSON"));
  QVERIFY(gsw::CameraDatasetInfo::meshGenerationBlocked(selected));
}

void WorkspaceDocumentTests::refusesChangedInitializerVersionBeforeDatasetAssociation() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString project = root.filePath(QStringLiteral("workspace"));
  const QString oldDataset = QDir(project).filePath(QStringLiteral("old-dataset"));
  const QString version = QDir(project).filePath(QStringLiteral(".gsw/initialization/new-version"));
  const QString artifactRelative = QStringLiteral("source/initialization/points/fixture.npz");
  QVERIFY(QDir().mkpath(oldDataset));
  QVERIFY(QDir().mkpath(QFileInfo(QDir(version).filePath(artifactRelative)).absolutePath()));
  const auto write = [](const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
  const QByteArray artifact("test-owned-opaque-bytes-native-binding-only-not-NPZ-training-proof");
  const QString identity(64, QLatin1Char('b'));
  const QJsonObject summary{{"version", 1}, {"schema", "gsw.camera_dataset/1"}, {"posePolicy", "locked"},
      {"dataIdentity", identity}, {"metadataPath", "camera_dataset.json"}, {"blocked", false}, {"errors", QJsonArray{}}};
  const QJsonObject manifest{{"schema", "gsw.camera_dataset/1"}, {"pose_policy", "locked"}, {"data_identity", identity},
      {"initialization", QJsonObject{{"kind", "random"}, {"materialization", QJsonObject{
          {"path", artifactRelative}, {"sha256", hash(artifact)}}}}}};
  const QByteArray infoBytes = QJsonDocument(summary).toJson(), manifestBytes = QJsonDocument(manifest).toJson();
  QVERIFY(write(QDir(version).filePath(artifactRelative), artifact));
  QVERIFY(write(QDir(version).filePath(QStringLiteral("camera_dataset_info.json")), infoBytes));
  QVERIFY(write(QDir(version).filePath(QStringLiteral("camera_dataset.json")), manifestBytes));
  QJsonObject expected = summary;
  expected["manifestSha256"] = hash(manifestBytes); expected["infoSha256"] = hash(infoBytes);
  gsw::WorkspaceDocument document; QString error;
  QVERIFY(document.create(project, &error));
  QVERIFY(document.setDatasetPath(oldDataset, &error));
  const auto scene = document.sceneCollectionJson();
  for (const QString &target : {QStringLiteral("camera_dataset_info.json"), QStringLiteral("camera_dataset.json"), artifactRelative}) {
    const QString path = QDir(version).filePath(target);
    const QByteArray original = target == artifactRelative ? artifact : target == QStringLiteral("camera_dataset.json") ? manifestBytes : infoBytes;
    QVERIFY(write(path, original + "\n"));
    QVERIFY(!document.setVerifiedCameraDatasetPath(version, expected, &error));
    QCOMPARE(document.datasetPath(), oldDataset);
    QCOMPARE(document.sceneCollectionJson(), scene);
    QVERIFY(write(path, original));
  }
  QVERIFY2(document.setVerifiedCameraDatasetPath(version, expected, &error), qPrintable(error));
  QCOMPARE(document.datasetPath(), version);
  QCOMPARE(document.cameraDatasetIdentity(), identity);
  QCOMPARE(document.sceneCollectionJson(), scene);
}

void WorkspaceDocumentTests::registersPortableWorkflowArtifactsWithoutChangingSceneOrStartingWork() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString working = QDir(temporary.path()).filePath(QStringLiteral("working"));
  const QString artifact = QDir(working).filePath(QStringLiteral(".gsw/scale-versions/v1"));
  QVERIFY(QDir().mkpath(artifact));
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY2(document.createUntitled(working, QStringLiteral("Workflow registry"), &error), qPrintable(error));
  const auto originalScenes = document.sceneCollectionJson();
  QVERIFY2(document.addWorkflowArtifact(QStringLiteral("scale_version"), artifact, &error), qPrintable(error));
  QCOMPARE(document.workflowArtifacts().size(), 1);
  const auto record = document.workflowArtifacts().first().toObject();
  QCOMPARE(record.value(QStringLiteral("kind")).toString(), QStringLiteral("scale_version"));
  QCOMPARE(record.value(QStringLiteral("path")).toString(), QStringLiteral(".gsw/scale-versions/v1"));
  QVERIFY(record.value(QStringLiteral("available")).toBool());
  QCOMPARE(record.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
  QCOMPARE(document.sceneCollectionJson(), originalScenes);
  QSignalSpy changed(&document, &gsw::WorkspaceDocument::changed);
  QVERIFY2(document.addWorkflowArtifact(QStringLiteral("scale_version"), artifact, &error), qPrintable(error));
  QCOMPARE(document.workflowArtifacts().size(), 1);
  QCOMPARE(changed.count(), 0);
}

void WorkspaceDocumentTests::persistsWorkflowArtifactsAcrossRecoveryMigrationAndSaveAs() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString working = root.filePath(QStringLiteral("working"));
  const QString scaleRelative = QStringLiteral(".gsw/scale-versions/v1");
  const QString batchRelative = QStringLiteral(".gsw/training-batches/b1");
  const QString initializerRelative = QStringLiteral(".gsw/initialization/v1");
  const QString geometryRelative = QStringLiteral("output/geometry/g1");
  QVERIFY(QDir().mkpath(QDir(working).filePath(scaleRelative)));
  QVERIFY(QDir().mkpath(QDir(working).filePath(batchRelative)));
  QVERIFY(QDir().mkpath(QDir(working).filePath(initializerRelative)));
  QVERIFY(QDir().mkpath(QDir(working).filePath(geometryRelative)));
  const QMap<QString, QByteArray> records{
      {scaleRelative + QStringLiteral("/request.json"), "{\"reference_sigma\":0.002}"},
      {scaleRelative + QStringLiteral("/solution.json"), "{\"s\":2.5,\"independent_checks\":\"pass\"}"},
      {scaleRelative + QStringLiteral("/report.json"), "{\"model_geometry_accuracy\":\"not_measured\"}"},
      {batchRelative + QStringLiteral("/plan.json"), "{\"trainingSeed\":17}"},
      {batchRelative + QStringLiteral("/state.json"), "{\"status\":\"paused\"}"},
      {initializerRelative + QStringLiteral("/request.json"), "{\"kind\":\"random\",\"seed\":4294967295}"},
      {geometryRelative + QStringLiteral("/manifest.json"), "{\"surface_semantics\":\"appearance_proxy_not_material_boundary\"}"}};
  for (auto iterator = records.cbegin(); iterator != records.cend(); ++iterator) {
    QFile file(QDir(working).filePath(iterator.key()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(iterator.value()), qint64(iterator.value().size()));
  }
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.createUntitled(working, QStringLiteral("registry"), &error));
  QVERIFY(document.addWorkflowArtifact(QStringLiteral("scale_version"), QDir(working).filePath(scaleRelative), &error));
  QVERIFY(document.addWorkflowArtifact(QStringLiteral("training_batch"), QDir(working).filePath(batchRelative), &error));
  QVERIFY(document.addWorkflowArtifact(QStringLiteral("initialization_version"), QDir(working).filePath(initializerRelative), &error));
  QVERIFY(document.addWorkflowArtifact(QStringLiteral("geometry_product"), QDir(working).filePath(geometryRelative), &error));
  const auto expected = document.workflowArtifacts();
  const auto recovery = QJsonDocument::fromJson(document.recoveryManifestJson()).object();
  QCOMPARE(recovery.value(QStringLiteral("workflowArtifacts")).toArray().size(), 4);
  for (const auto &value : recovery.value(QStringLiteral("workflowArtifacts")).toArray())
    QVERIFY(!QDir::isAbsolutePath(value.toObject().value(QStringLiteral("path")).toString()));
  const auto saved = root.filePath(QStringLiteral("saved.gsw.json"));
  QVERIFY2(document.saveManifest(saved, &error), qPrintable(error));
  QVERIFY2(document.finalizeDataMigration(&error), qPrintable(error));
  QCOMPARE(document.workflowArtifacts(), expected);
  gsw::WorkspaceDocument reopened;
  QVERIFY2(reopened.load(saved, &error), qPrintable(error));
  QCOMPARE(reopened.workflowArtifacts(), expected);
  const auto copied = root.filePath(QStringLiteral("copied.gsw.json"));
  QVERIFY2(reopened.save(copied, &error), qPrintable(error));
  gsw::WorkspaceDocument copiedDocument;
  QVERIFY2(copiedDocument.load(copied, &error), qPrintable(error));
  QCOMPARE(copiedDocument.workflowArtifacts(), expected);
  for (auto iterator = records.cbegin(); iterator != records.cend(); ++iterator) {
    QFile source(QDir(working).filePath(iterator.key()));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256),
             QCryptographicHash::hash(iterator.value(), QCryptographicHash::Sha256));
    QFile output(QDir(copiedDocument.rootPath()).filePath(iterator.key()));
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(output.readAll(), QCryptographicHash::Sha256),
             QCryptographicHash::hash(iterator.value(), QCryptographicHash::Sha256));
  }
}

void WorkspaceDocumentTests::rejectsUnsafeWorkflowArtifactsAndRetainsMissingReferences() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const auto working = root.filePath(QStringLiteral("working"));
  const auto outside = root.filePath(QStringLiteral("working-elsewhere"));
  const auto artifact = QDir(working).filePath(QStringLiteral(".gsw/scale-versions/v1"));
  QVERIFY(QDir().mkpath(artifact));
  QVERIFY(QDir().mkpath(outside));
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.createUntitled(working, QStringLiteral("registry"), &error));
  QVERIFY(!document.addWorkflowArtifact(QStringLiteral("scale_version"), outside, &error));
  QVERIFY(!document.addWorkflowArtifact(QStringLiteral("unknown_kind"), artifact, &error));
  QVERIFY(!document.addWorkflowArtifact(QStringLiteral("scale_version"), working, &error));
  QVERIFY(!document.addWorkflowArtifact(QStringLiteral("scale_version"), QDir(working).filePath(QStringLiteral("missing")), &error));
  QVERIFY(document.workflowArtifacts().isEmpty());
  QVERIFY(document.addWorkflowArtifact(QStringLiteral("scale_version"), artifact, &error));
  const auto saved = root.filePath(QStringLiteral("registry.gsw.json"));
  QVERIFY(document.saveManifest(saved, &error));
  QVERIFY(QDir().rename(artifact, artifact + QStringLiteral("-removed")));
  gsw::WorkspaceDocument reopened;
  QVERIFY2(reopened.load(saved, &error), qPrintable(error));
  QCOMPARE(reopened.workflowArtifacts().size(), 1);
  QVERIFY(!reopened.workflowArtifacts().first().toObject().value(QStringLiteral("available")).toBool());
  QCOMPARE(reopened.workflowArtifacts().first().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("missing"));
  const auto previous = reopened.workflowArtifacts();
  QFile input(saved);
  QVERIFY(input.open(QIODevice::ReadOnly));
  auto malformed = QJsonDocument::fromJson(input.readAll()).object();
  input.close();
  malformed.insert(QStringLiteral("workflowArtifacts"), QJsonArray{QJsonObject{
      {QStringLiteral("kind"), QStringLiteral("scale_version")}, {QStringLiteral("path"), QStringLiteral("../working-elsewhere")}}});
  const auto badPath = root.filePath(QStringLiteral("malformed.gsw.json"));
  QVERIFY(writeJson(badPath, QJsonDocument(malformed)));
  QVERIFY(!reopened.load(badPath, &error));
  QCOMPARE(reopened.workflowArtifacts(), previous);
  QCOMPARE(reopened.projectFilePath(), saved);
  QVERIFY(reopened.createUntitled(outside, QStringLiteral("new project"), &error));
  QVERIFY(reopened.workflowArtifacts().isEmpty());
}

void WorkspaceDocumentTests::refusesWorkflowJunctionsWithoutFollowingExternalPayloads() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const auto working = root.filePath(QStringLiteral("working"));
  const auto real = QDir(working).filePath(QStringLiteral("real"));
  const auto outside = root.filePath(QStringLiteral("outside"));
  const auto alias = QDir(working).filePath(QStringLiteral("alias"));
  QVERIFY(QDir().mkpath(QDir(real).filePath(QStringLiteral("v1"))));
  QVERIFY(QDir().mkpath(QDir(outside).filePath(QStringLiteral("v1"))));
  const auto linkDirectory = [](const QString &target, const QString &link) {
#ifdef Q_OS_WIN
    QString escapedTarget = target, escapedLink = link;
    escapedTarget.replace(QLatin1Char('\''), QStringLiteral("''"));
    escapedLink.replace(QLatin1Char('\''), QStringLiteral("''"));
    QProcess process;
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
      arguments->flags |= CREATE_NO_WINDOW;
      arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
      arguments->startupInfo->wShowWindow = SW_HIDE;
    });
    process.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
        QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
        QStringLiteral("$ErrorActionPreference='Stop'; New-Item -ItemType Junction -Path '%1' -Target '%2' | Out-Null")
            .arg(escapedLink, escapedTarget)});
    return process.waitForStarted(10000) && process.waitForFinished(30000) && process.exitCode() == 0;
#else
    return QFile::link(target, link);
#endif
  };
  QVERIFY(linkDirectory(real, alias));
  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.createUntitled(working, QStringLiteral("registry"), &error));
  QVERIFY(!document.addWorkflowArtifact(QStringLiteral("scale_version"), QDir(alias).filePath(QStringLiteral("v1")), &error));
  QVERIFY(document.addWorkflowArtifact(QStringLiteral("scale_version"), QDir(real).filePath(QStringLiteral("v1")), &error));
  QVERIFY(QDir().rename(real, real + QStringLiteral("-old")));
  QVERIFY(linkDirectory(outside, real));
  QCOMPARE(document.workflowArtifacts().first().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("unsafe"));
  QVERIFY(!document.workflowArtifacts().first().toObject().value(QStringLiteral("available")).toBool());
  QVERIFY(!document.save(root.filePath(QStringLiteral("blocked.gsw.json")), &error));
  QVERIFY(QFileInfo(QDir(outside).filePath(QStringLiteral("v1"))).isDir());
}

void WorkspaceDocumentTests::loadsModelScaleVersionWithElementLocalTextureAndRewrittenUnits() {
  QString python = qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");
  if (python.isEmpty()) python = qEnvironmentVariable("TEST_PYTHON");
  QVERIFY2(QFileInfo(python).isFile(), "Required model-scale CPU acceptance needs GSW_QUALITY_TEST_PYTHON or TEST_PYTHON; it is not an optional skipped case.");
  QString repository = qEnvironmentVariable("GSW_BACKEND_ROOT");
  if (repository.isEmpty()) {
    QDir candidate(QDir::currentPath());
    for (int level = 0; level < 8; ++level) {
      if (QFileInfo(candidate.filePath(QStringLiteral("native/backend/model_scale_version.py"))).isFile()) { repository = candidate.path(); break; }
      if (!candidate.cdUp()) break;
    }
  }
  QVERIFY2(QFileInfo(QDir(repository).filePath(QStringLiteral("native/backend/model_scale_version.py"))).isFile(), "Actual shared model-scale backend must be available.");
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir root(directory.path());
  QVERIFY(root.mkpath(QStringLiteral("nested")));
  const QString texturePath = root.filePath(QStringLiteral("nested/source texture.png"));
  QImage texture(3, 2, QImage::Format_RGB32); texture.fill(QColor(120, 60, 30));
  QVERIFY(texture.save(texturePath, "PNG"));
  QFile imageInput(texturePath); QVERIFY(imageInput.open(QIODevice::ReadOnly));
  const QByteArray imageBytes = imageInput.readAll(); imageInput.close();
  const QString source = root.filePath(QStringLiteral("source.ply"));
  const QByteArray sourceBytes(
      "ply\nformat ascii 1.0\ncomment keep global note\ncomment units=mm\n"
      "element vertex 3\ncomment TextureFile \"nested/source texture.png\"\ncomment coordinate_unit=cm\ncomment crs EPSG:32654\n"
      "property double x\nproperty double y\nproperty double z\n"
      "element face 1\ncomment TextureFile \"nested/source texture.png\"\ncomment unit:mm\ncomment keep face note\n"
      "property list uchar int vertex_indices\nproperty list uchar float texcoord\nend_header\n"
      "0 0 0\n2 0 0\n0 1 0\n3 0 1 2 6 0 0 1 0 0 1\n");
  QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write(sourceBytes), sourceBytes.size()); input.close();
  QProcess creator;
#ifdef Q_OS_WIN
  creator.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW; args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW; args->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  const auto script = QString::fromUtf8(
      "import json,sys;sys.path.insert(0,sys.argv[1]);from native.backend.model_scale_version import inspect_model_scale_input,create_scaled_model_version;"
      "e=inspect_model_scale_input(sys.argv[2]);r={'schema':'gsw.scale_constraints/1','input_data_identity':e['data_identity'],'input_frame':'source','mode':'scale_only','fit_tolerance_m':1e-8,"
      "'constraints':[{'id':'length','kind':'distance','role':'fit','reference_id':'generated-ruler','reference_unit':'m','source_a':[0,0,0],'source_b':[2,0,0],'reference_distance':6}]};"
      "o=create_scaled_model_version(sys.argv[2],r,sys.argv[3],e['data_identity']);print(json.dumps({'modelPath':o['modelPath']}))");
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  environment.insert(QStringLiteral("TEMP"), directory.path()); environment.insert(QStringLiteral("TMP"), directory.path());
  creator.setProcessEnvironment(environment); creator.setWorkingDirectory(repository);
  creator.start(python, {QStringLiteral("-B"), QStringLiteral("-c"), script, repository, source, root.filePath(QStringLiteral("new-version"))});
  QVERIFY2(creator.waitForFinished(30000) && creator.exitCode() == 0, creator.readAllStandardError().constData());
  const QString output = QJsonDocument::fromJson(creator.readAllStandardOutput()).object().value(QStringLiteral("modelPath")).toString();
  QVERIFY(QFileInfo(output).isFile());
  const auto loaded = gsw::PlyPointCloudLoader::load(output);
  QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
  QVERIFY(loaded.hasMesh());
  QVERIFY(loaded.meshHasTextureCoordinates);
  QVERIFY2(loaded.meshTextureError.isEmpty(), qPrintable(loaded.meshTextureError));
  QVERIFY(QFileInfo(loaded.meshTexturePath).isFile());
  QVERIFY(QDir(QFileInfo(output).absolutePath()).relativeFilePath(loaded.meshTexturePath).startsWith(QStringLiteral("assets/")));
  QFile ownedTexture(loaded.meshTexturePath); QVERIFY(ownedTexture.open(QIODevice::ReadOnly));
  QCOMPARE(QCryptographicHash::hash(ownedTexture.readAll(), QCryptographicHash::Sha256), QCryptographicHash::hash(imageBytes, QCryptographicHash::Sha256)); ownedTexture.close();
  QCOMPARE(loaded.meshTextureImage.size(), QSize(3, 2));
  // The loader consumes its temporary per-corner array into render vertices.
  // Assert the actual public geometry the viewport draws, for either residency.
  auto vertices = loaded.meshVertices;
  auto indices = loaded.meshIndices;
  if (loaded.meshCache.isValid()) {
    const auto page = gsw::MeshCache::readNode(loaded.meshCache, loaded.meshCache.rootNode);
    QVERIFY2(page.isValid(), qPrintable(page.error));
    vertices = page.vertices;
    indices = page.indices;
  }
  QCOMPARE(indices.size(), 3);
  const std::array<QVector2D, 3> expectedUv{{{0, 0}, {1, 0}, {0, 1}}};
  for (int corner = 0; corner < 3; ++corner) {
    QVERIFY(indices[corner] < static_cast<quint32>(vertices.size()));
    const auto &vertex = vertices[indices[corner]];
    QCOMPARE(QVector2D(vertex.textureU, vertex.textureV), expectedUv[corner]);
    QVERIFY(vertex.textureWeight > 0.5F);
  }
  QCOMPARE(loaded.sourceFaceCount, qint64(1));
  QVERIFY(loaded.coordinates.metersPerCoordinate.has_value());
  QCOMPARE(*loaded.coordinates.metersPerCoordinate, 1.);
  QVERIFY(loaded.coordinates.coordinateReferenceSystem.isEmpty());
  QCOMPARE(loaded.coordinates.provenance.value(QStringLiteral("input_declared_crs")).toString(), QStringLiteral("EPSG:32654"));
  QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), sourceBytes);
}

QTEST_GUILESS_MAIN(WorkspaceDocumentTests)
#include "WorkspaceDocumentTests.moc"
