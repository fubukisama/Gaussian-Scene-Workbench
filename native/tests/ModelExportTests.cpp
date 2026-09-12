#include "ModelExport.h"
#include "PlyPointCloudLoader.h"
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
}

class ModelExportTests final : public QObject {
  Q_OBJECT
private slots:
  void preservesPlyAndCroppedGaussianAttributes();
  void exportsCoordinatesAndBakesDoublePrecisionTransform();
  void exportsCompleteMeshAndEmbeddedGlb();
  void exportsPointGlbWithoutInventingFaces();
  void keepsExistingFilesOnCancellationAndFailure();
  void transformsBinaryEndianPly();
  void usesFullSourceAcrossDiskPages();
  void rejectsSourceChangesBeforeCommit();
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

QTEST_GUILESS_MAIN(ModelExportTests)
#include "ModelExportTests.moc"
