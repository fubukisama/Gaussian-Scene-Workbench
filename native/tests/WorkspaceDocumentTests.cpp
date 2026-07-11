#include "PlyPointCloudLoader.h"
#include "SceneEditModel.h"
#include "WorkspaceDocument.h"

#include <QBitArray>
#include <QtEndian>

#include <bit>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class WorkspaceDocumentTests final : public QObject {
  Q_OBJECT

private slots:
  void parsesGaussianPlyHeader();
  void loadsAsciiPointColorsAndSamplesDeterministically();
  void loadsBinaryGaussianSphericalHarmonicColors();
  void tracksSelectionDeletionUndoAndRedo();
  void exportsFilteredAsciiPlyWithOriginalFields();
  void exportsFilteredBinaryPlyWithoutReencoding();
  void savesAndLoadsPortableProject();
};

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

QTEST_GUILESS_MAIN(WorkspaceDocumentTests)
#include "WorkspaceDocumentTests.moc"
