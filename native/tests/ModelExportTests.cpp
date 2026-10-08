#include "ModelExport.h"
#include "PlyPointCloudLoader.h"
#include "SpzIO.h"
#include <load-spz.h>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <bit>

using namespace gsw;
namespace {
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
bool save(const QString &path, const QByteArray &data) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size(); }
QByteArray pointPly() {
  return "ply\nformat ascii 1.0\ncomment units cm\nelement vertex 4\nproperty double x\nproperty double y\nproperty double z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n"
         "1000000.125 2 3 255 0 0\n1000001.125 2 3 0 255 0\n1000001.125 3 3 0 0 255\n1000000.125 3 3 255 255 255\n";
}
QByteArray meshPly() {
  auto data = pointPly();
  data.replace("end_header\n", "element face 1\nproperty list uchar int vertex_indices\nproperty list uchar float texcoord\ncomment TextureFile texture.png\nend_header\n");
  return data + "4 0 1 2 3 8 0 0 1 0 1 1 0 1\n";
}
quint32 uintAt(const QByteArray &data, qsizetype offset) {
  return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
float floatAt(const QByteArray &data, qsizetype offset) { return std::bit_cast<float>(uintAt(data, offset)); }
QByteArray gaussianPly(int degree = 3, double x = 1.12345, bool invalidRotation = false) {
  QByteArray h = "ply\nformat ascii 1.0\nelement vertex 3\n";
  for (const char *p : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
    h += QByteArray("property double ") + p + '\n';
  const int dim = (degree + 1) * (degree + 1) - 1;
  for (int j = 0; j < 3 * dim; ++j) h += "property float f_rest_" + QByteArray::number(j) + '\n';
  h += "end_header\n";
  for (int i = 0; i < 3; ++i) {
    h += QByteArray::number(x, 'g', 12) + " -2.25 " + QByteArray::number(i + 0.125) + " 0.3 -0.2 0.6 0.75 -2 -3 -4 " +
        (invalidRotation ? QByteArray("0 0 0 0") : QByteArray("0.7 0.2 -0.3 0.4"));
    for (int j = 0; j < 3 * dim; ++j) h += ' ' + QByteArray::number((j - 20) * 0.011, 'g', 8);
    h += '\n';
  }
  return h;
}
}

class ModelExportTests final : public QObject {
  Q_OBJECT
private slots:
  void preservesPlyAndCroppedGaussianAttributes();
  void exportsCoordinatesAndBakesDoublePrecisionTransform();
  void exportsSceneUnitsAndPreservesSourceCoordinates();
  void sceneUnitGlbUsesPhysicalMetresOnce();
  void sceneUnitsCoverMeshFormatsAndMetadata();
  void sceneUnitConversionKeepsGaussianSourceProtection();
  void preservesProjectionSidecarMetadataInSourcePly();
  void scenePlyDoesNotPublishSourceCrs();
  void protectsExistingProjectionSidecars();
  void exportsCompleteMeshAndEmbeddedGlb();
  void exportsPointGlbWithoutInventingFaces();
  void keepsExistingFilesOnCancellationAndFailure();
  void transformsBinaryEndianPly();
  void usesFullSourceAcrossDiskPages();
  void rejectsSourceChangesBeforeCommit();
  void spzRoundTripAndDegreeOptions();
  void spzRejectsInvalidAndCancelledWithoutPublishing();
  void spzRealModelWhenRequested();
};

void ModelExportTests::preservesPlyAndCroppedGaussianAttributes() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("copy.ply"));
  auto data = pointPly();
  data.replace("end_header\n", "property float scale_0\nproperty float rot_0\nproperty float f_rest_0\nend_header\n");
  data.replace("255 0 0\n", "255 0 0 -2 1 0.12345\n");
  data.replace("0 255 0\n", "0 255 0 -3 1 0.98765\n");
  data.replace("0 0 255\n", "0 0 255 -4 1 0.54321\n");
  data.replace("255 255 255\n", "255 255 255 -5 1 0.11223\n");
  QVERIFY(save(o.sourcePath, data));
  auto result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
  QCOMPARE(read(o.destinationPath), data);
  o.deletedVertices.resize(4); o.deletedVertices.setBit(1);
  result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
  auto expected = data; expected.replace("element vertex 4", "element vertex 3");
  expected.replace("1000001.125 2 3 0 255 0 -3 1 0.98765\n", "");
  QCOMPARE(read(o.destinationPath), expected);
  o.applyTransform = true;
  o.transform.rotation = QQuaternion::fromAxisAndAngle({0,0,1}, 45);
  QVERIFY(!exportModelFile(o).success); QCOMPARE(read(o.destinationPath), expected);
  QCOMPARE(read(o.sourcePath), data);
}

void ModelExportTests::exportsCoordinatesAndBakesDoublePrecisionTransform() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
  QVERIFY(save(o.sourcePath, pointPly()));
  o.applyTransform = true; o.pivot = {1000000.125, 2, 3};
  o.transform.translation = {10, 20, 30}; o.transform.scale = {2, 3, 4};
  o.transform.rotation = QQuaternion::fromAxisAndAngle({0,0,1}, 90);
  o.deletedVertices.resize(4); o.deletedVertices.setBit(2);
  for (auto format : {ModelExportFormat::Csv, ModelExportFormat::Xyz, ModelExportFormat::Ply}) {
    o.format = format; o.destinationPath = dir.filePath(modelExportSuffix(format));
    const auto result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
    if (format == ModelExportFormat::Ply) {
      const auto output = PlyPointCloudLoader::load(o.destinationPath);
      QVERIFY(output.isValid()); QCOMPARE(output.sourceVertexCount, 3);
      QVERIFY(std::abs(output.coordinates.globalMinimum.x - 1000007.125) < 1e-5);
      QVERIFY(std::abs(output.coordinates.globalMaximum.y - 24) < 1e-5);
      QCOMPARE(output.coordinates.globalMinimum.z, 33);
    } else {
      const auto lines = read(o.destinationPath).trimmed().split('\n');
      QCOMPARE(lines.size(), format == ModelExportFormat::Csv ? 4 : 3);
      const auto line = lines[format == ModelExportFormat::Csv ? 1 : 0];
      QVERIFY(line.startsWith(format == ModelExportFormat::Csv ? "1000010.125,22,33," : "1000010.125 22 33 "));
    }
  }
  QCOMPARE(read(o.sourcePath), pointPly());
}

void ModelExportTests::exportsSceneUnitsAndPreservesSourceCoordinates() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("centimetres.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("scene-metres.ply"));
  QVERIFY(save(o.sourcePath, pointPly()));
  o.coordinates.unit = SceneLengthUnit::Centimetres;
  o.coordinates.unitDeclared = true;
  o.coordinates.displayShift = {-1000000, 0, 0};
  o.coordinates.displayScale = 0.25;
  o.sceneUnitScale = 0.01;
  o.sceneUnit = SceneLengthUnit::Metres;
  o.sceneUnitDeclared = true;
  o.applyTransform = true;
  // Identity object TRS is not an unchanged export when scene units differ.
  auto result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  auto loaded = PlyPointCloudLoader::load(o.destinationPath);
  QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
  QVERIFY(std::abs(loaded.coordinates.globalMinimum.x - 10000.00125) < 1e-9);
  QVERIFY(std::abs(loaded.coordinates.globalMaximum.y - 0.03) < 1e-12);
  QCOMPARE(loaded.coordinates.unit, SceneLengthUnit::Metres);
  QVERIFY(loaded.coordinates.unitDeclared);
  QVERIFY(!read(o.destinationPath).contains("units cm"));
  // A source-coordinate export ignores both the object TRS and scene unit.
  o.applyTransform = false;
  o.transform.translation = {10, 20, 30};
  result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  QCOMPARE(read(o.destinationPath), pointPly());
  QCOMPARE(read(o.sourcePath), pointPly());
}

void ModelExportTests::sceneUnitGlbUsesPhysicalMetresOnce() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("centimetres.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("scene.glb"));
  QVERIFY(save(o.sourcePath, pointPly()));
  o.format = ModelExportFormat::Glb;
  o.coordinates.unit = SceneLengthUnit::Centimetres;
  o.coordinates.unitDeclared = true;
  o.pivot = {1000000.125, 2, 3};
  o.transform.translation = {10, 20, 30};
  o.applyTransform = true;
  for (const auto unit : {SceneLengthUnit::Metres, SceneLengthUnit::Millimetres}) {
    o.sceneUnit = unit;
    o.sceneUnitDeclared = true;
    o.sceneUnitScale = unit == SceneLengthUnit::Metres ? 0.01 : 10.0;
    const auto result = exportModelFile(o);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto bytes = read(o.destinationPath);
    const auto json = QJsonDocument::fromJson(bytes.mid(20, uintAt(bytes, 12))).object();
    const auto translation = json["nodes"].toArray()[0].toObject()["translation"].toArray();
    // The source 10 cm translation must still become 0.1 m in glTF,
    // irrespective of whether the common scene frame uses metres or mm.
    QVERIFY(std::abs(translation[0].toDouble() - 10000.10125) < 1e-8);
    QVERIFY(std::abs(translation[1].toDouble() - 0.33) < 1e-12);
    QVERIFY(std::abs(translation[2].toDouble() + 0.22) < 1e-12);
    const qsizetype binStart = 28 + uintAt(bytes, 12);
    QVERIFY(std::abs(floatAt(bytes, binStart + 48) - 0.01F) < 1e-7);
  }
  QCOMPARE(read(o.sourcePath), pointPly());
}

void ModelExportTests::sceneUnitsCoverMeshFormatsAndMetadata() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
  auto mesh = pointPly();
  mesh.replace("end_header\n", "element face 1\nproperty list uchar int vertex_indices\nend_header\n");
  mesh += "4 0 1 2 3\n";
  QVERIFY(save(o.sourcePath, mesh));
  o.coordinates.unit = SceneLengthUnit::Centimetres;
  o.coordinates.unitDeclared = true;
  o.sceneUnitScale = 0.01;
  o.sceneUnit = SceneLengthUnit::Metres;
  o.sceneUnitDeclared = true;
  o.applyTransform = true;
  o.transform.translation = {10, 20, 30};
  for (auto format : {ModelExportFormat::Xyz, ModelExportFormat::Csv,
                      ModelExportFormat::Obj, ModelExportFormat::Stl}) {
    o.format = format;
    o.destinationPath = dir.filePath(QStringLiteral("scene.") + modelExportSuffix(format));
    const auto result = exportModelFile(o);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto bytes = read(o.destinationPath);
    if (format == ModelExportFormat::Stl) {
      QCOMPARE(uintAt(bytes, 80), 2);
      QVERIFY(std::abs(floatAt(bytes, 96) - 10000.10125F) < 0.001F);
      QVERIFY(std::abs(floatAt(bytes, 100) - 0.22F) < 1e-6F);
      QVERIFY(std::abs(floatAt(bytes, 104) - 0.33F) < 1e-6F);
    } else {
      auto lines = bytes.trimmed().split('\n');
      QByteArray line = lines[format == ModelExportFormat::Csv ? 1 : 0];
      if (format == ModelExportFormat::Obj) line = line.mid(2);
      const auto values = line.split(format == ModelExportFormat::Csv ? ',' : ' ');
      QVERIFY(std::abs(values[0].toDouble() - 10000.10125) < 1e-9);
      QVERIFY(std::abs(values[1].toDouble() - 0.22) < 1e-12);
      QVERIFY(std::abs(values[2].toDouble() - 0.33) < 1e-12);
    }
  }
  o.format = ModelExportFormat::Ply;
  o.destinationPath = dir.filePath(QStringLiteral("metadata.ply"));
  for (const QByteArray &declarations : {
           QByteArray(),
           QByteArray("comment units cm\nobj_info coordinate_unit=centimetres CRS:LOCAL\ncomment UNIT:CM\n")}) {
    auto source = pointPly();
    source.replace("comment units cm\n", declarations);
    QVERIFY(save(o.sourcePath, source));
    const auto result = exportModelFile(o);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto loaded = PlyPointCloudLoader::load(o.destinationPath);
    QVERIFY(loaded.isValid());
    QCOMPARE(loaded.coordinates.unit, SceneLengthUnit::Metres);
    QVERIFY(!read(o.destinationPath).contains("centimetres"));
    QVERIFY(!read(o.destinationPath).contains("UNIT:CM"));
    QVERIFY(loaded.coordinates.coordinateReferenceSystem.isEmpty());
    QCOMPARE(read(o.sourcePath), source);
  }
}

void ModelExportTests::sceneUnitConversionKeepsGaussianSourceProtection() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("gaussians.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("destination"));
  const auto original = gaussianPly();
  QVERIFY(save(o.sourcePath, original));
  o.sceneUnitScale = 0.01;
  o.sceneUnit = SceneLengthUnit::Metres;
  o.sceneUnitDeclared = true;
  for (auto format : {ModelExportFormat::Ply, ModelExportFormat::Spz}) {
    o.format = format;
    o.applyTransform = true;
    QVERIFY(save(o.destinationPath, "existing data"));
    const auto rejected = exportModelFile(o);
    QVERIFY(!rejected.success);
    QCOMPARE(read(o.destinationPath), QByteArray("existing data"));
    o.applyTransform = false;
    const auto preserved = exportModelFile(o);
    QVERIFY2(preserved.success, qPrintable(preserved.error));
    if (format == ModelExportFormat::Ply) QCOMPARE(read(o.destinationPath), original);
    QCOMPARE(read(o.sourcePath), original);
  }
  QVERIFY(save(o.sourcePath, pointPly()));
  o.format = ModelExportFormat::Ply;
  o.applyTransform = true;
  for (double invalidScale : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
    o.sceneUnitScale = invalidScale;
    QVERIFY(save(o.destinationPath, "existing data"));
    QVERIFY(!exportModelFile(o).success);
    QCOMPARE(read(o.destinationPath), QByteArray("existing data"));
  }
}

void ModelExportTests::preservesProjectionSidecarMetadataInSourcePly() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
  QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("export"))));
  o.destinationPath = dir.filePath(QStringLiteral("export/standalone.ply"));
  auto original = pointPly();
  original.replace("comment units cm\n", "");
  const auto projection = QStringLiteral("LOCAL_CS[\"研究室 / 建築\",UNIT[\"centimetre\",0.01]]").toUtf8();
  const QString prj = dir.filePath(QStringLiteral("source.prj"));
  QVERIFY(save(o.sourcePath, original));
  QVERIFY(save(prj, projection));
  const auto source = PlyPointCloudLoader::load(o.sourcePath);
  QVERIFY(source.isValid());
  QCOMPARE(source.coordinates.unit, SceneLengthUnit::Centimetres);
  o.coordinates = source.coordinates;
  const auto result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  const auto output = PlyPointCloudLoader::load(o.destinationPath);
  QVERIFY2(output.isValid(), qPrintable(output.error));
  QCOMPARE(output.coordinates.unit, source.coordinates.unit);
  QVERIFY(output.coordinates.unitDeclared);
  QCOMPARE(output.coordinates.coordinateReferenceSystem, source.coordinates.coordinateReferenceSystem);
  QCOMPARE(output.coordinates.globalMinimum.x, source.coordinates.globalMinimum.x);
  const auto exported = read(o.destinationPath);
  QCOMPARE(exported.mid(exported.indexOf("end_header\n") + 11), original.mid(original.indexOf("end_header\n") + 11));
  QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("export/standalone.prj"))));
  QCOMPARE(read(o.sourcePath), original);
  QCOMPARE(read(prj), projection);

  // The same source-coordinate contract includes full 3DGS/2DGS attributes.
  const auto gaussians = gaussianPly();
  QVERIFY(save(o.sourcePath, gaussians));
  o.coordinates = PlyPointCloudLoader::load(o.sourcePath).coordinates;
  const auto gaussianResult = exportModelFile(o);
  QVERIFY2(gaussianResult.success, qPrintable(gaussianResult.error));
  const auto gaussianOutput = read(o.destinationPath);
  QCOMPARE(gaussianOutput.mid(gaussianOutput.indexOf("end_header\n") + 11),
           gaussians.mid(gaussians.indexOf("end_header\n") + 11));
  const auto loadedGaussians = PlyPointCloudLoader::load(o.destinationPath);
  QVERIFY(loadedGaussians.isValid());
  QCOMPARE(loadedGaussians.coordinates.unit, SceneLengthUnit::Centimetres);
  QCOMPARE(loadedGaussians.coordinates.coordinateReferenceSystem, source.coordinates.coordinateReferenceSystem);
  QCOMPARE(read(o.sourcePath), gaussians);
  QCOMPARE(read(prj), projection);
}

void ModelExportTests::scenePlyDoesNotPublishSourceCrs() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("scene.ply"));
  const auto projection = QByteArray("LOCAL_CS[\"Source\",UNIT[\"centimetre\",0.01]]");
  QVERIFY(save(dir.filePath(QStringLiteral("source.prj")), projection));
  for (const auto &metadata : {QByteArray(), QByteArray("comment crs: EPSG:6677\n"),
                              QByteArray("obj_info coordinate_unit=cm CRS:LOCAL\n")}) {
    auto original = pointPly();
    original.replace("comment units cm\n", metadata);
    QVERIFY(save(o.sourcePath, original));
    o.coordinates = PlyPointCloudLoader::load(o.sourcePath).coordinates;
    o.applyTransform = true;
    o.transform.translation = {10, 20, 30};
    o.sceneUnitScale = 0.01;
    o.sceneUnit = SceneLengthUnit::Metres;
    o.sceneUnitDeclared = true;
    const auto result = exportModelFile(o);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto output = PlyPointCloudLoader::load(o.destinationPath);
    QVERIFY(output.isValid());
    QCOMPARE(output.coordinates.unit, SceneLengthUnit::Metres);
    QVERIFY(output.coordinates.coordinateReferenceSystem.isEmpty());
    QVERIFY(std::abs(output.coordinates.globalMinimum.x - 10000.10125) < 1e-9);
    QCOMPARE(read(o.sourcePath), original);
    QCOMPARE(read(dir.filePath(QStringLiteral("source.prj"))), projection);
  }
}

void ModelExportTests::protectsExistingProjectionSidecars() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("output.ply"));
  const auto projection = QByteArray("LOCAL_CS[\"Source\",UNIT[\"centimetre\",0.01]]");
  const auto conflict = QByteArray("LOCAL_CS[\"Other\",UNIT[\"metre\",1]]");
  const QString sourcePrj = dir.filePath(QStringLiteral("source.prj"));
  const QString targetPrj = dir.filePath(QStringLiteral("output.prj"));
  QVERIFY(save(o.sourcePath, pointPly()));
  QVERIFY(save(sourcePrj, projection));
  o.coordinates = PlyPointCloudLoader::load(o.sourcePath).coordinates;
  QVERIFY(save(o.destinationPath, "existing model"));
  QVERIFY(save(targetPrj, conflict));
  auto result = exportModelFile(o);
  QVERIFY(!result.success);
  QVERIFY(!result.error.isEmpty());
  QCOMPARE(read(o.destinationPath), QByteArray("existing model"));
  QCOMPARE(read(targetPrj), conflict);
  QVERIFY(save(targetPrj, projection));
  result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  QCOMPARE(read(targetPrj), projection);
  QVERIFY(save(o.destinationPath, "existing model"));
  o.progress = [&](int progress) {
    if (progress == 99) save(targetPrj, conflict);
    return false;
  };
  QVERIFY(!exportModelFile(o).success);
  QCOMPARE(read(o.destinationPath), QByteArray("existing model"));
  QCOMPARE(read(targetPrj), conflict);
  QVERIFY(QFile::remove(targetPrj));
  o.progress = [&](int progress) {
    if (progress == 99) save(sourcePrj, conflict);
    return false;
  };
  QVERIFY(!exportModelFile(o).success);
  QCOMPARE(read(o.destinationPath), QByteArray("existing model"));
  o.progress = {};
  const QByteArray oversizedHeaderProjection = "LOCAL_CS[\"" + QByteArray(1024 * 1024 - 80, 'x') +
      "\",UNIT[\"centimetre\",0.01]]";
  QVERIFY(save(sourcePrj, oversizedHeaderProjection));
  QVERIFY(!exportModelFile(o).success);
  QCOMPARE(read(o.destinationPath), QByteArray("existing model"));
  QCOMPARE(read(sourcePrj), oversizedHeaderProjection);
}

void ModelExportTests::exportsCompleteMeshAndEmbeddedGlb() {
  QTemporaryDir temporary;
  const QString fixtureDir = qEnvironmentVariable("GSW_MODEL_EXPORT_FIXTURE_DIR");
  const QDir dir(fixtureDir.isEmpty() ? temporary.path() : fixtureDir);
  QVERIFY(QDir().mkpath(dir.path()));
  QImage texture(4, 4, QImage::Format_RGBA8888); texture.fill(Qt::red);
  QVERIFY(texture.save(dir.filePath(QStringLiteral("texture.png"))));
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("mesh.ply"));
  o.coordinates.unit = SceneLengthUnit::Centimetres; o.coordinates.unitDeclared = true;
  o.pivot = {1000000.125, 2, 3};
  QVERIFY(save(o.sourcePath, meshPly()));
  for (auto format : {ModelExportFormat::Ply, ModelExportFormat::Obj, ModelExportFormat::Stl, ModelExportFormat::Glb}) {
    o.format = format; o.destinationPath = dir.filePath(QStringLiteral("export.") + modelExportSuffix(format));
    const auto result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
    const auto bytes = read(o.destinationPath);
    if (format == ModelExportFormat::Ply) {
      const auto loaded = PlyPointCloudLoader::load(o.destinationPath, 1, 1, 1, 1);
      QVERIFY2(loaded.isValid(), qPrintable(loaded.error)); QCOMPARE(loaded.sourceVertexCount, 4);
      QCOMPARE(loaded.sourceFaceCount, 1); QVERIFY(!loaded.meshTextureImage.isNull());
    } else if (format == ModelExportFormat::Obj) {
      QCOMPARE(bytes.count("\nv ") + bytes.startsWith("v "), 4);
      QCOMPARE(bytes.count("\nf "), 2); QCOMPARE(bytes.count("\nvt "), 6);
      QVERIFY(bytes.contains("f 1/1 2/2 3/3")); QVERIFY(bytes.contains("f 1/4 3/5 4/6"));
      const auto materialPath = bytes.split('\n').first().mid(7);
      QVERIFY(QFileInfo::exists(dir.filePath(QString::fromUtf8(materialPath))));
    } else if (format == ModelExportFormat::Stl) {
      QCOMPARE(bytes.size(), 84 + 2 * 50); QCOMPARE(uintAt(bytes, 80), 2);
      QVERIFY(std::abs(floatAt(bytes, 84 + 12) - 1000000.125F) < 1e-5);
    } else {
      QCOMPARE(uintAt(bytes, 0), 0x46546C67U); QCOMPARE(uintAt(bytes, 4), 2);
      QCOMPARE(uintAt(bytes, 8), bytes.size());
      const auto json = QJsonDocument::fromJson(bytes.mid(20, uintAt(bytes, 12))).object();
      const auto primitive = json["meshes"].toArray()[0].toObject()["primitives"].toArray()[0].toObject();
      QCOMPARE(primitive["mode"].toInt(), 4);
      const int positionIndex = primitive["attributes"].toObject()["POSITION"].toInt();
      QCOMPARE(json["accessors"].toArray()[positionIndex].toObject()["count"].toInt(), 6);
      const auto translation = json["nodes"].toArray()[0].toObject()["translation"].toArray();
      QVERIFY(std::abs(translation[0].toDouble() - 10000.00125) < 1e-8);
      QCOMPARE(translation[1].toDouble(), 0.03); QCOMPARE(translation[2].toDouble(), -0.02);
      const qsizetype binStart = 28 + uintAt(bytes, 12);
      QCOMPARE(floatAt(bytes, binStart), 0.0F);
      QVERIFY(std::abs(floatAt(bytes, binStart + 48) - 0.01F) < 1e-7);
      QCOMPARE(floatAt(bytes, binStart + 44), 1.0F); // glTF top-left texture origin
      const auto imageView = json["bufferViews"].toArray()[1].toObject();
      QImage embedded;
      QVERIFY(embedded.loadFromData(bytes.mid(binStart + imageView["byteOffset"].toInteger(), imageView["byteLength"].toInteger())));
      QCOMPARE(embedded.pixelColor(0,0), QColor(Qt::red));
      QVERIFY(!json["images"].toArray()[0].toObject().contains("uri"));
    }
  }
}

void ModelExportTests::exportsPointGlbWithoutInventingFaces() {
  QTemporaryDir dir;
  ModelExportOptions o; o.format = ModelExportFormat::Glb;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply")); o.destinationPath = dir.filePath(QStringLiteral("points.glb"));
  QVERIFY(save(o.sourcePath, pointPly()));
  o.deletedVertices.resize(4); o.deletedVertices.setBit(0);
  const auto result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
  const auto bytes = read(o.destinationPath);
  const auto json = QJsonDocument::fromJson(bytes.mid(20, uintAt(bytes, 12))).object();
  QCOMPARE(json["accessors"].toArray()[0].toObject()["count"].toInt(), 3);
  const auto primitive = json["meshes"].toArray()[0].toObject()["primitives"].toArray()[0].toObject();
  QCOMPARE(primitive["mode"].toInt(), 0); QVERIFY(!primitive.contains("indices"));
}

void ModelExportTests::keepsExistingFilesOnCancellationAndFailure() {
  QTemporaryDir dir;
  ModelExportOptions o; o.sourcePath = dir.filePath(QStringLiteral("source.ply")); o.destinationPath = dir.filePath(QStringLiteral("out"));
  QVERIFY(save(o.sourcePath, pointPly())); QVERIFY(save(o.destinationPath, "existing data"));
  for (auto format : {ModelExportFormat::Ply, ModelExportFormat::Csv, ModelExportFormat::Glb}) {
    o.format = format; o.progress = [](int) { return true; };
    const auto result = exportModelFile(o); QVERIFY(result.cancelled); QVERIFY(!result.success);
    QCOMPARE(read(o.destinationPath), QByteArray("existing data"));
  }
  o.progress = {}; o.format = ModelExportFormat::Obj;
  QVERIFY(!exportModelFile(o).success); QCOMPARE(read(o.destinationPath), QByteArray("existing data"));
  o.format = ModelExportFormat::Ply; o.destinationPath = o.sourcePath;
  QVERIFY(!exportModelFile(o).success); QCOMPARE(read(o.sourcePath), pointPly());
  o.destinationPath = dir.filePath(QStringLiteral("out")); o.format = ModelExportFormat::Glb;
  auto invalid = meshPly(); invalid.replace("4 0 1 2 3", "4 0 1 2 999");
  // Remove UV requirements to exercise topology failure, not missing texture.
  invalid.replace("comment TextureFile texture.png\n", "");
  invalid.replace("property list uchar float texcoord\n", "");
  QVERIFY(save(o.sourcePath, invalid));
  QVERIFY(!exportModelFile(o).success); QCOMPARE(read(o.destinationPath), QByteArray("existing data"));
  QCOMPARE(QDir(dir.path()).entryList({QStringLiteral(".gsw-export-*"), QStringLiteral("gsw-assets-*")}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
}

void ModelExportTests::transformsBinaryEndianPly() {
  QTemporaryDir dir;
  for (const auto endian : {QDataStream::LittleEndian, QDataStream::BigEndian}) {
    ModelExportOptions o; o.sourcePath = dir.filePath(QStringLiteral("binary.ply")); o.destinationPath = dir.filePath(QStringLiteral("out.ply"));
    QFile source(o.sourcePath); QVERIFY(source.open(QIODevice::WriteOnly));
    source.write(endian == QDataStream::LittleEndian ? "ply\nformat binary_little_endian 1.0\n" : "ply\nformat binary_big_endian 1.0\n");
    source.write("element vertex 1\nproperty double x\nproperty double y\nproperty double z\nproperty float nx\nproperty float ny\nproperty float nz\nproperty uchar label\nend_header\n");
    QDataStream data(&source); data.setByteOrder(endian); data.setFloatingPointPrecision(QDataStream::DoublePrecision);
    data << 1000000.125 << 2.0 << 3.0;
    data.setFloatingPointPrecision(QDataStream::SinglePrecision); data << 1.0F << 0.0F << 0.0F << quint8(42); source.close();
    o.applyTransform = true; o.pivot = {1000000.125, 2, 3}; o.transform.translation = {1,2,3};
    o.transform.rotation = QQuaternion::fromAxisAndAngle({0,0,1}, 90);
    const auto result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
    const auto loaded = PlyPointCloudLoader::load(o.destinationPath); QVERIFY(loaded.isValid());
    QCOMPARE(loaded.coordinates.globalMinimum.x, 1000001.125); QCOMPARE(loaded.coordinates.globalMinimum.y, 4.0);
    QCOMPARE(static_cast<quint8>(read(o.destinationPath).back()), quint8(42));
    PlyGeometryVisitor visitor; bool checked = false;
    visitor.vertex = [&](qint64, const PlySourceVertex &v) { checked = true; return std::abs(v.normal.y() - 1) < 1e-6; };
    QString error; QVERIFY(PlyPointCloudLoader::visitSourceGeometry(o.destinationPath, visitor, error)); QVERIFY(checked);
    o.coordinates.unit = SceneLengthUnit::Centimetres;
    o.coordinates.unitDeclared = true;
    o.sceneUnitScale = 0.01;
    o.sceneUnit = SceneLengthUnit::Metres;
    o.sceneUnitDeclared = true;
    const auto unitResult = exportModelFile(o);
    QVERIFY2(unitResult.success, qPrintable(unitResult.error));
    const auto unitLoaded = PlyPointCloudLoader::load(o.destinationPath);
    QVERIFY(unitLoaded.isValid());
    QVERIFY(std::abs(unitLoaded.coordinates.globalMinimum.x - 10000.01125) < 1e-9);
    QVERIFY(std::abs(unitLoaded.coordinates.globalMinimum.y - 0.04) < 1e-12);
    QCOMPARE(unitLoaded.coordinates.unit, SceneLengthUnit::Metres);
    QCOMPARE(static_cast<quint8>(read(o.destinationPath).back()), quint8(42));
  }
}

void ModelExportTests::usesFullSourceAcrossDiskPages() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("paged.ply")); o.destinationPath = dir.filePath(QStringLiteral("paged.stl"));
  o.format = ModelExportFormat::Stl;
  QFile source(o.sourcePath); QVERIFY(source.open(QIODevice::WriteOnly));
  constexpr int count = 20000;
  source.write("ply\nformat ascii 1.0\nelement vertex 20000\nproperty double x\nproperty double y\nproperty double z\nelement face 2\nproperty list uchar int vertex_indices\nend_header\n");
  for (int i = 0; i < count; ++i) source.write(QByteArray::number(i) + " 0 " + QByteArray::number(i % 2) + '\n');
  // DiskVertex record 18724 straddles a 1 MiB page boundary.
  source.write("3 0 18724 19999\n3 1 19999 18724\n"); source.close();
  const auto preview = PlyPointCloudLoader::load(o.sourcePath, 1, 1, 1, 1);
  QVERIFY2(preview.isValid(), qPrintable(preview.error)); QCOMPARE(preview.sourceVertexCount, count);
  const auto result = exportModelFile(o); QVERIFY2(result.success, qPrintable(result.error));
  const auto bytes = read(o.destinationPath);
  QCOMPARE(uintAt(bytes, 80), 2); QCOMPARE(floatAt(bytes, 84 + 12 + 12), 18724.0F);
  QCOMPARE(floatAt(bytes, 84 + 12 + 24), 19999.0F);
  o.format = ModelExportFormat::Csv; o.destinationPath = dir.filePath(QStringLiteral("paged.csv"));
  QVERIFY(exportModelFile(o).success); QCOMPARE(read(o.destinationPath).count('\n'), count + 1);
}

void ModelExportTests::rejectsSourceChangesBeforeCommit() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply")); o.destinationPath = dir.filePath(QStringLiteral("out"));
  for (auto format : {ModelExportFormat::Ply, ModelExportFormat::Glb}) {
    QVERIFY(save(o.sourcePath, pointPly())); QVERIFY(save(o.destinationPath, "original destination"));
    bool changed = false; o.format = format;
    o.progress = [&](int value) {
      if (value == 99 && !changed) {
        QFile source(o.sourcePath);
        if (source.open(QIODevice::Append)) { source.write("\n"); changed = true; }
      }
      return false;
    };
    const auto result = exportModelFile(o);
    QVERIFY(changed); QVERIFY(!result.success); QVERIFY(!result.cancelled);
    QCOMPARE(read(o.destinationPath), QByteArray("original destination"));
  }
}

void ModelExportTests::spzRoundTripAndDegreeOptions() {
  QTemporaryDir dir;
  ModelExportOptions o;
  o.format = ModelExportFormat::Spz; o.spzQuality = 2;
  o.sourcePath = dir.filePath(QStringLiteral("源 日 model.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("圧縮 model.spz"));
  const auto source = gaussianPly(); QVERIFY(save(o.sourcePath, source));
  o.deletedVertices.resize(3); o.deletedVertices.setBit(1);
  for (int version : {3, 4}) for (int degree : {0, 1, 2, 3}) {
    o.spzVersion = version; o.spzMaximumShDegree = degree;
    auto r = exportModelFile(o); QVERIFY2(r.success, qPrintable(r.error));
    const auto bytes = read(o.destinationPath);
    QVERIFY(version == 3 ? bytes.startsWith(QByteArray::fromHex("1f8b")) : bytes.startsWith("NGSP"));
    const QString decoded = dir.filePath(QStringLiteral("decoded.ply"));
    r = importSpzToPly(o.destinationPath, decoded); QVERIFY2(r.success, qPrintable(r.error));
    int seen = 0;
    PlyGaussianVisitor visitor;
    visitor.begin = [&](qint64 n, int d, bool aa) { return n == 2 && d == degree && !aa; };
    visitor.gaussian = [&](qint64, const std::array<double, 86> &v) {
      const int originalIndex = seen++ == 0 ? 0 : 2;
      if (std::abs(v[0] - 1.12345) > 0.00013 || std::abs(v[1] + 2.25) > 0.00013 ||
          std::abs(v[2] - originalIndex - 0.125) > 0.00013) return false;
      if (std::abs(v[3] - 0.3) > 0.04 || std::abs(v[6] - 0.75) > 0.04 || std::abs(v[8] + 3) > 0.07) return false;
      const QQuaternion q(v[10], v[11], v[12], v[13]);
      if (std::abs(QQuaternion::dotProduct(q.normalized(), QQuaternion(0.7F, 0.2F, -0.3F, 0.4F).normalized())) < 0.99999) return false;
      const int dim = (degree + 1) * (degree + 1) - 1;
      for (int c = 0; c < 3; ++c) for (int k = 0; k < dim; ++k)
        if (std::abs(v[14+c*dim+k] - (c*15+k-20)*0.011) > 0.009) return false;
      return true;
    };
    QString error;
    QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(decoded, visitor, error), qPrintable(error)); QCOMPARE(seen, 2);
    const auto preview = PlyPointCloudLoader::load(decoded);
    QVERIFY2(preview.isValid(), qPrintable(preview.error)); QVERIFY(preview.hasGaussianAttributes);
    QCOMPARE(preview.sourceVertexCount, 2);
    QCOMPARE(read(o.sourcePath), source);
  }
  QVERIFY(save(o.sourcePath, gaussianPly(4)));
  o.spzMaximumShDegree = -1;
  QVERIFY(exportModelFile(o).success);
  const auto degree4 = dir.filePath(QStringLiteral("degree4.ply"));
  QVERIFY(importSpzToPly(o.destinationPath, degree4).success);
  PlyGaussianVisitor degreeVisitor;
  degreeVisitor.begin = [](qint64 n, int d, bool) { return n == 2 && d == 4; };
  QString degreeError;
  QVERIFY(PlyPointCloudLoader::visitSourceGaussians(degree4, degreeVisitor, degreeError));
  // Legacy v2 created by the actual upstream encoder (not our PLY bridge).
  spz::GaussianCloud g; g.numPoints = 1;
  g.positions = {1,2,3}; g.colors = {.1F,.2F,.3F}; g.scales = {-2,-3,-4}; g.alphas = {.7F}; g.rotations = {0,0,0,1};
  spz::PackOptions pack; pack.version = 2; pack.from = spz::CoordinateSystem::RDF;
  std::vector<uint8_t> bytes; QVERIFY(spz::saveSpz(g, pack, &bytes));
  QVERIFY(save(o.destinationPath, QByteArray(reinterpret_cast<const char *>(bytes.data()), bytes.size())));
  QVERIFY(importSpzToPly(o.destinationPath, dir.filePath(QStringLiteral("legacy.ply"))).success);
  // The official decoder returns infinite logits at alpha endpoints. Our
  // working PLY must stay finite and remain exportable, for both 0 and 255.
  for (float alpha : {-1000.0F, 1000.0F}) {
    g.alphas = {alpha}; pack.version = 4;
    QVERIFY(spz::saveSpz(g, pack, &bytes));
    QVERIFY(save(o.destinationPath, QByteArray(reinterpret_cast<const char *>(bytes.data()), bytes.size())));
    const auto working = dir.filePath(QStringLiteral("saturated.ply"));
    QVERIFY(importSpzToPly(o.destinationPath, working).success);
    ModelExportOptions again; again.sourcePath = working; again.destinationPath = dir.filePath(QStringLiteral("again.spz"));
    again.format = ModelExportFormat::Spz;
    QVERIFY(exportModelFile(again).success);
  }
}

void ModelExportTests::spzRejectsInvalidAndCancelledWithoutPublishing() {
  QTemporaryDir dir;
  ModelExportOptions o; o.format = ModelExportFormat::Spz;
  o.sourcePath = dir.filePath(QStringLiteral("source.ply")); o.destinationPath = dir.filePath(QStringLiteral("out.spz"));
  QVERIFY(save(o.sourcePath, gaussianPly()));
  QVERIFY(exportModelFile(o).success);
  const auto valid = read(o.destinationPath);
  for (const auto &bad : {pointPly(), gaussianPly(3, 1e6), gaussianPly(3, 1, true)}) {
    QVERIFY(save(o.sourcePath, bad)); QVERIFY(save(o.destinationPath, "sentinel"));
    QVERIFY(!exportModelFile(o).success); QCOMPARE(read(o.destinationPath), QByteArray("sentinel"));
  }
  QVERIFY(save(o.sourcePath, gaussianPly()));
  for (int stage : {0, 65, 90, 99}) {
    o.progress = [stage](int p) { return p >= stage; };
    const auto r = exportModelFile(o); QVERIFY(r.cancelled); QVERIFY(!r.success);
    QCOMPARE(read(o.destinationPath), QByteArray("sentinel"));
  }
  o.progress = {}; o.applyTransform = true; QVERIFY(!exportModelFile(o).success); o.applyTransform = false;
  o.deletedVertices.resize(3); o.deletedVertices.fill(true); QVERIFY(!exportModelFile(o).success); o.deletedVertices.clear();
  bool changed = false;
  o.progress = [&](int p) { if (p == 99) { QFile f(o.sourcePath); if (f.open(QIODevice::Append)) { f.write("\n"); changed = true; } } return false; };
  QVERIFY(!exportModelFile(o).success); QVERIFY(changed); QCOMPARE(read(o.destinationPath), QByteArray("sentinel"));
  const auto decoded = dir.filePath(QStringLiteral("existing.ply")); QVERIFY(save(decoded, "unchanged"));
  QList<QByteArray> invalid{QByteArray("not spz"), valid.left(valid.size()-1), valid + 'x'};
  auto hostile = valid; hostile[12] = 5; invalid << hostile;
  hostile = valid; hostile[13] = 31; invalid << hostile;
  hostile = valid; hostile[14] = 2; invalid << hostile;
  hostile = valid; for (int i = 8; i < 12; ++i) hostile[i] = char(0x7f); invalid << hostile;
  for (const auto &data : invalid) {
    QVERIFY(save(o.destinationPath, data)); QVERIFY(!importSpzToPly(o.destinationPath, decoded).success);
    QCOMPARE(read(decoded), QByteArray("unchanged"));
  }
  QVERIFY(save(o.destinationPath, valid));
  for (int stage : {0, 10, 40, 99}) {
    const auto r = importSpzToPly(o.destinationPath, decoded, [stage](int p) { return p >= stage; });
    QVERIFY(r.cancelled); QVERIFY(!r.success); QCOMPARE(read(decoded), QByteArray("unchanged"));
  }
  QVERIFY(!importSpzToPly(o.destinationPath, o.destinationPath).success); QCOMPARE(read(o.destinationPath), valid);
  QVERIFY(!exportSpz(ModelExportOptions{o.sourcePath, o.sourcePath}).success);
}

void ModelExportTests::spzRealModelWhenRequested() {
  const QString source = qEnvironmentVariable("GSW_SPZ_SOURCE");
  if (source.isEmpty()) QSKIP("Set GSW_SPZ_SOURCE for read-only real-model round-trip QA.");
  const QDir dir(qEnvironmentVariable("GSW_MODEL_EXPORT_FIXTURE_DIR"));
  QVERIFY(!dir.path().isEmpty()); QVERIFY(QDir().mkpath(dir.path()));
  ModelExportOptions o; o.format = ModelExportFormat::Spz; o.sourcePath = source;
  o.destinationPath = dir.filePath(QStringLiteral("real-model.spz"));
  auto r = exportModelFile(o); QVERIFY2(r.success, qPrintable(r.error));
  r = importSpzToPly(o.destinationPath, dir.filePath(QStringLiteral("real-model.ply")));
  QVERIFY2(r.success, qPrintable(r.error));
  const auto a = PlyPointCloudLoader::load(source, 10000);
  const auto b = PlyPointCloudLoader::load(dir.filePath(QStringLiteral("real-model.ply")), 10000);
  QVERIFY(a.isValid()); QVERIFY2(b.isValid(), qPrintable(b.error));
  QCOMPARE(a.sourceVertexCount, b.sourceVertexCount); QVERIFY(b.hasGaussianAttributes);
  QVERIFY((a.boundsMinimum - b.boundsMinimum).length() < 0.001F);
  QVERIFY((a.boundsMaximum - b.boundsMaximum).length() < 0.001F);
  qInfo() << "SPZ real round trip:" << a.sourceVertexCount << "gaussians; PLY bytes" << QFileInfo(source).size()
          << "SPZ bytes" << QFileInfo(o.destinationPath).size();
}

QTEST_GUILESS_MAIN(ModelExportTests)
#include "ModelExportTests.moc"
