#include "CameraTrajectory.h"
#include "ColmapSupport.h"
#include "PlyPointCloudLoader.h"
#include "SceneEditModel.h"
#include "ScreenSpaceSelection.h"
#include "TrainingOutputLocator.h"
#include "WorkspaceDocument.h"

#include <QBitArray>
#include <QtEndian>

#include <bit>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class WorkspaceDocumentTests final : public QObject {
  Q_OBJECT

private slots:
  void parsesGaussianPlyHeader();
  void loadsAsciiPointColorsAndSamplesDeterministically();
  void loadsBinaryGaussianSphericalHarmonicColors();
  void activatesGaussianScaleRotationAndOpacity();
  void detectsColmapDatasetLayoutAndExecutable();
  void selectsNewestVersionedColmapExecutable();
  void locatesVersionedColmapOnRepositoryVolume();
  void locatesNewestCompletedTrainingScene();
  void detectsCompleteAndIncompleteColmapModels();
  void selectsBrushStrokeAndHonorsVisibility();
  void tracksSelectionDeletionUndoAndRedo();
  void exportsFilteredAsciiPlyWithOriginalFields();
  void exportsFilteredBinaryPlyWithoutReencoding();
  void savesAndLoadsPortableProject();
  void loadsStandardCameraSidecarAndBuildsLegacyAxes();
  void skipsMalformedCameraEntries();
  void reportsMalformedCameraDocument();
  void treatsMissingCameraSidecarAsOptional();
  void reloadsCameraSidecarAfterRepair();
  void decimatesLargeCameraVisualization();
};

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

namespace {
void writeLittleEndianFloat(QFile &file, const float value) {
  const quint32 bits = qToLittleEndian(std::bit_cast<quint32>(value));
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
  const QString plyPath = QDir(temporary.path()).filePath(QStringLiteral("scene.ply"));
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
  const gsw::PlyMetadata metadata = gsw::WorkspaceDocument::inspectPly(plyPath, &error);
  QVERIFY2(metadata.valid, qPrintable(error));
  QCOMPARE(metadata.format, QStringLiteral("binary_little_endian"));
  QCOMPARE(metadata.vertexCount, 1234);
  QVERIFY(metadata.looksLikeGaussianSplat());
}

void WorkspaceDocumentTests::loadsAsciiPointColorsAndSamplesDeterministically() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath = QDir(temporary.path()).filePath(QStringLiteral("colored.ply"));
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

void WorkspaceDocumentTests::loadsBinaryGaussianSphericalHarmonicColors() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath = QDir(temporary.path()).filePath(QStringLiteral("gaussian.ply"));
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
      QDir(temporary.path()).filePath(QStringLiteral("gaussian-attributes.ply"));
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
  for (const float value : {1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                            0.0F, 0.69314718056F, -0.69314718056F,
                            2.0F, 0.0F, 0.0F, 0.0F}) {
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
  for (const QString &version : {QStringLiteral("3.13.0"),
                                 QStringLiteral("4.1.0")}) {
    QVERIFY(root.mkpath(version + QStringLiteral("/bin")));
    QFile executable(root.filePath(version + QStringLiteral("/bin/colmap.exe")));
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
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir fixture(temporary.path());
  const QString volumeRoot = fixture.filePath(QStringLiteral("data-volume"));
  const QString repositoryRoot =
      QDir(volumeRoot).filePath(QStringLiteral("apps/gsw"));
  const QString executablePath = QDir(volumeRoot).filePath(
      QStringLiteral("Tools/COLMAP/4.1.0/bin/colmap.exe"));
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
  for (const QString &name : {QStringLiteral("cameras.bin"),
                              QStringLiteral("images.bin")}) {
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
  const QVector<gsw::PointPosition> positions = {
      {-0.5F, 0.0F, -0.3F}, {0.0F, 0.0F, -0.4F},
      {0.5F, 0.0F, -0.2F},  {0.0F, 0.6F, 0.0F},
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
  const QByteArray header =
      "ply\n"
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
  const qsizetype payloadOffset = bytes.indexOf("end_header\n") +
                                  QByteArray("end_header\n").size();
  QVERIFY(payloadOffset > 0);
  QByteArray expectedPayload;
  for (const int index : {0, 2}) {
    appendLittleEndianFloat(expectedPayload, static_cast<float>(index));
    appendLittleEndianFloat(expectedPayload, 1.0F);
    appendLittleEndianFloat(expectedPayload, 2.0F);
    appendLittleEndianFloat(expectedPayload,
                            0.1F + static_cast<float>(index));
  }
  QCOMPARE(bytes.mid(payloadOffset), expectedPayload);
  QVERIFY(bytes.left(payloadOffset).contains("element vertex 2\n"));
}

void WorkspaceDocumentTests::savesAndLoadsPortableProject() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("images")));
  QVERIFY(root.mkpath(QStringLiteral("models")));

  QFile image(root.filePath(QStringLiteral("images/frame_0001.jpg")));
  QVERIFY(image.open(QIODevice::WriteOnly));
  image.close();

  const QString plyPath = root.filePath(QStringLiteral("models/scene.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nend_header\n0\n1\n");
  ply.close();

  gsw::WorkspaceDocument source;
  QString error;
  QVERIFY2(source.create(root.path(), &error), qPrintable(error));
  QVERIFY2(source.setDatasetPath(root.filePath(QStringLiteral("images")), &error), qPrintable(error));
  QVERIFY2(source.setScenePath(plyPath, &error), qPrintable(error));
  const QString projectPath = root.filePath(QStringLiteral("test.gsw.json"));
  QVERIFY2(source.save(projectPath, &error), qPrintable(error));
  QVERIFY(!source.isModified());

  QFile serialized(projectPath);
  QVERIFY(serialized.open(QIODevice::ReadOnly));
  const QJsonObject projectJson = QJsonDocument::fromJson(serialized.readAll()).object();
  QCOMPARE(projectJson.value(QStringLiteral("rootPath")).toString(), QStringLiteral("."));
  QCOMPARE(projectJson.value(QStringLiteral("datasetPath")).toString(), QStringLiteral("images"));
  QCOMPARE(projectJson.value(QStringLiteral("scenePath")).toString(), QStringLiteral("models/scene.ply"));

  gsw::WorkspaceDocument restored;
  QVERIFY2(restored.load(projectPath, &error), qPrintable(error));
  QCOMPARE(restored.rootPath(), QDir::cleanPath(root.absolutePath()));
  QCOMPARE(restored.datasetPath(), QDir::cleanPath(root.filePath(QStringLiteral("images"))));
  QCOMPARE(restored.scenePath(), QDir::cleanPath(plyPath));
  QCOMPARE(restored.imageCount(), 1);
  QCOMPARE(restored.sceneMetadata().vertexCount, 2);
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
                          vector3(0.0, 1.0, 0.0))) })));

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
  compareVector(geometry.path.constFirst().start,
                QVector3D(1.0F, 2.0F, 3.0F));
  compareVector(geometry.path.constFirst().end,
                QVector3D(3.0F, 2.0F, 3.0F));
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
  const QJsonObject valid = camera(
      QStringLiteral("valid.jpg"), vector3(0.0, 0.0, 0.0),
      rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 1.0, 0.0),
               vector3(0.0, 0.0, 1.0)));
  const QJsonObject invalid = camera(
      QStringLiteral("broken.jpg"), vector3(0.0, 1.0, 0.0),
      rotation(vector3(1.0, 1.0, 0.0), vector3(0.0, 0.0, 0.0),
               vector3(0.0, 0.0, 1.0)));
  QVERIFY(writeJson(
      QDir(temporary.path()).filePath(QStringLiteral("cameras.json")),
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
  QFile sidecar(QDir(temporary.path()).filePath(QStringLiteral("cameras.json")));
  QVERIFY(sidecar.open(QIODevice::WriteOnly));
  QCOMPARE(sidecar.write("{not-json"), qint64(9));
  sidecar.close();

  const gsw::CameraTrajectory trajectory =
      gsw::CameraTrajectory::loadForScene(
          QDir(temporary.path()).filePath(QStringLiteral("scene.ply")));

  QVERIFY(trajectory.cameras().isEmpty());
  QVERIFY(!trajectory.sourcePath().isEmpty());
  QVERIFY(!trajectory.error().isEmpty());
}

void WorkspaceDocumentTests::treatsMissingCameraSidecarAsOptional() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());

  const gsw::CameraTrajectory trajectory =
      gsw::CameraTrajectory::loadForScene(
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

  QVERIFY(writeJson(
      sidecar,
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
    cameras.append(camera(
        QStringLiteral("frame_%1.jpg").arg(index),
        vector3(static_cast<double>(index), 0.0, 0.0),
        rotation(vector3(1.0, 0.0, 0.0), vector3(0.0, 1.0, 0.0),
                 vector3(0.0, 0.0, 1.0))));
  }
  QVERIFY(writeJson(root.filePath(QStringLiteral("cameras.json")),
                    QJsonDocument(cameras)));
  const gsw::CameraTrajectory trajectory =
      gsw::CameraTrajectory::loadForScene(
          root.filePath(QStringLiteral("scene.ply")));

  QCOMPARE(trajectory.cameras().size(), qsizetype(cameraCount));
  const gsw::CameraTrajectoryGeometry geometry = trajectory.geometry(1.0F);
  QVERIFY(geometry.decimated);
  QCOMPARE(geometry.displayedFrustumCameraCount, qsizetype(1000));
  QCOMPARE(geometry.displayedPathCameraCount, qsizetype(5000));
  QCOMPARE(geometry.frustums.size(), qsizetype(8000));
  QCOMPARE(geometry.path.size(), qsizetype(4999));
  compareVector(geometry.path.constFirst().start,
                QVector3D(0.0F, 0.0F, 0.0F));
  compareVector(geometry.path.constLast().end,
                QVector3D(static_cast<float>(cameraCount - 1), 0.0F, 0.0F));
}

QTEST_GUILESS_MAIN(WorkspaceDocumentTests)
#include "WorkspaceDocumentTests.moc"
