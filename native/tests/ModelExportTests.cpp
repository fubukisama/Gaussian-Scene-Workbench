#include "ModelExport.h"
#include "PlyPointCloudLoader.h"
#include "SpzIO.h"
#include <load-spz.h>
#include <QDataStream>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

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
// Independent Cartesian real-SH polynomials, rather than the exporter's basis/rotation helper.
std::array<double, 25> cartesianSh(const std::array<double, 3> &direction) {
  const double x = direction[0], y = direction[1], z = direction[2];
  const double xx = x*x, yy = y*y, zz = z*z;
  return {0.28209479177387814,
      -0.4886025119029199*y, 0.4886025119029199*z, -0.4886025119029199*x,
      1.0925484305920792*x*y, -1.0925484305920792*y*z, 0.31539156525252005*(2*zz-xx-yy),
      -1.0925484305920792*x*z, 0.5462742152960396*(xx-yy),
      -0.5900435899266435*y*(3*xx-yy), 2.890611442640554*x*y*z,
      -0.4570457994644658*y*(4*zz-xx-yy), 0.3731763325901154*z*(2*zz-3*xx-3*yy),
      -0.4570457994644658*x*(4*zz-xx-yy), 1.445305721320277*z*(xx-yy),
      -0.5900435899266435*x*(xx-3*yy),
      2.5033429417967046*x*y*(xx-yy), -1.7701307697799304*y*z*(3*xx-yy),
      0.9461746957575601*x*y*(7*zz-1), -0.6690465435572892*y*z*(7*zz-3),
      0.10578554691520431*(zz*(35*zz-30)+3), -0.6690465435572892*x*z*(7*zz-3),
      0.47308734787878004*(xx-yy)*(7*zz-1), -1.7701307697799304*x*z*(xx-3*yy),
      0.6258357354491761*(xx*(xx-3*yy)-yy*(3*xx-yy))};
}
}

class ModelExportTests final : public QObject {
  Q_OBJECT
private slots:
  void preservesPlyAndCroppedGaussianAttributes();
  void bakesCroppedAscii3dGaussianTransformAndPreservesAttributes();
  void bakesBinary2dGaussianNonUniformTransformWithoutInventingThickness();
  void recordedCameraMetadataCannotBeBypassedByPlainGaussianBake();
  void gaussianTranslationPreservesDoublePrecisionNormalValues();
  void nonUniformGaussianBakeRejectsOnlyRetainedDirectionalHarmonics();
  void gaussianBakeRejectsDuplicateScaleAndRotationProperties();
  void mirrorsBinary3dGaussiansAndDirectionalDegreeThreeAndFourAppearance();
  void bakesNonUniformDcOnly3dGaussianCovariance();
  void gaussianBakeCancellationKeepsDestinationAndBothSourceKinds();
  void realGaussianPlyBakeWhenRequested();
  void exportsCoordinatesAndBakesDoublePrecisionTransform();
  void exportsSceneUnitsAndPreservesSourceCoordinates();
  void sceneUnitGlbUsesPhysicalMetresOnce();
  void sceneUnitsCoverMeshFormatsAndMetadata();
  void sceneUnitConversionBakesGaussianPlyAndKeepsSpzSourceProtection();
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
  void spzCodecRejectsInvalidHeaderMetadata();
  void spzCodecChecksPackedIndicesAndBuffers();
  void spzCodecSaturatesExtremeFiniteSh();
  void spzCodecChecksLegacyPayloadBeforeAllocation();
  void spzCodecRejectsIncompleteLargeCloudMetadata();
  void spzCodecRejectsConstructedFractionalBits();
  void spzRealModelWhenRequested();
  void trainingCoordinateScaleReachesMeasurementAndJsonReport();
  void arbitraryCameraScaleDoesNotInventMetres();
  void cameraScaleSidecarIsBoundToExactPlyBytes();
  void cameraScaleMetadataRefreshesAnExistingGeometryCache();
  void unknownScaleCannotBeExportedAsImplicitMetreGlb();
  void recordedCameraCoordinatesUseRealNativeExportProcess();
  void recordedPointSceneEditUsesPivotOnceAndRetainsFrameHistory();
  void recordedKindsUseSharedNativeExportProcess();
  void realGpuTrainingModelMeasurementAndCoordinateExports();
};

void ModelExportTests::recordedKindsUseSharedNativeExportProcess() {
  const QString python=qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_PYTHON"),root=qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_ROOT");
  if(python.isEmpty()||root.isEmpty())QSKIP("Real native export helper runtime not selected");
  QTemporaryDir directory;QVERIFY(directory.isValid());
  QImage image(3,2,QImage::Format_RGB32);image.fill(QColor(10,90,180));QVERIFY(image.save(directory.filePath("texture.png")));
  const auto originalTexture=read(directory.filePath("texture.png"));
  const QJsonArray identity{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}};
  const auto sim=[&](double s,QJsonArray t){return QJsonObject{{"s",s},{"R",identity},{"t",t},{"applied",true}};};
  const QJsonObject camera{{"view_id","stable-train"},{"split","train"},{"width",640},{"height",480},
      {"K",QJsonArray{QJsonArray{700,12,301},QJsonArray{0,710,222},QJsonArray{0,0,1}}},
      {"c2w_opencv",QJsonArray{QJsonArray{1,0,0,3},QJsonArray{0,1,0,4},QJsonArray{0,0,1,5},QJsonArray{0,0,0,1}}}};
  for(const QString &kind:QStringList{"point_cloud","mesh","3dgs","2dgs"}) {
    const QString source=directory.filePath(kind+".ply");
    QByteArray bytes=kind=="point_cloud"?pointPly():kind=="mesh"?meshPly():gaussianPly(1);
    bytes.replace("comment units cm\n","");
    if(kind=="2dgs") {
      bytes.replace("property double scale_2\n","");
      bytes.replace("0.75 -2 -3 -4 ","0.75 -2 -3 ");
    }
    QVERIFY(save(source,bytes));
    const QJsonObject record{{"schema","gsw.training_camera_contract/1"},{"input_data_identity",QString(64,'b')},
        {"ply_sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
        {"scale",QJsonObject{{"status","source_declared"},{"source_unit","m"},{"canonical_unit","m"},{"meters_per_unit",1.},
            {"evidence",QJsonArray{QJsonObject{{"kind","user_declaration"}}}}}},
        {"transforms",QJsonObject{{"source_to_canonical",sim(1.,{0,0,0})},{"canonical_to_training",sim(2.,{1,2,3})}}},
        {"views",QJsonArray{camera}},{"clipping",QJsonObject{{"frame","training"},{"znear",.2},{"zfar",100.},{"backend_near",.4}}}};
    QVERIFY(save(cameraScaleMetadataPath(source),QJsonDocument(record).toJson()));
    const auto before=PlyPointCloudLoader::load(source);QVERIFY2(before.isValid(),qPrintable(before.error));
    ModelExportOptions options;options.sourcePath=source;options.destinationPath=directory.filePath(kind+"-edited.ply");
    options.cameraCoordinateRecovery="metric";options.recordedRepresentation=(kind=="3dgs"||kind=="2dgs")?QString("gaussian"):kind;
    options.recoveryPythonPath=python;options.recoveryRepositoryRoot=root;options.applyTransform=true;
    options.transform.scale={2,2,2};options.transform.rotation=QQuaternion::fromAxisAndAngle({0,0,1},90);options.transform.translation={10,20,30};
    const auto result=exportModelFile(options);QVERIFY2(result.success,qPrintable(kind+": "+result.error));
    const auto after=PlyPointCloudLoader::load(options.destinationPath);QVERIFY2(after.isValid(),qPrintable(after.error));
    QVERIFY(std::abs(after.coordinates.globalMinimum.x-(4.5-before.coordinates.globalMaximum.y))<1e-6);
    QVERIFY(std::abs(after.coordinates.globalMaximum.y-(9.+before.coordinates.globalMaximum.x))<1e-6);
    QVERIFY(std::abs(after.coordinates.globalMinimum.z-(13.5+before.coordinates.globalMinimum.z))<1e-6);
    QCOMPARE(after.sourceVertexCount,before.sourceVertexCount);QCOMPARE(after.sourceFaceCount,before.sourceFaceCount);
    QCOMPARE(after.hasGaussianAttributes,before.hasGaussianAttributes);QCOMPARE(after.hasSurfelAttributes,before.hasSurfelAttributes);
    QVERIFY(cameraScaleRecoveryCapability(options.destinationPath).preserve);
    const auto metadata=QJsonDocument::fromJson(read(cameraScaleMetadataPath(options.destinationPath))).object();
    QCOMPARE(metadata["export_version"].toInt(),3);QCOMPARE(metadata["scene_scale_multiplier"].toDouble(),2.);
    if(kind=="mesh") {
      const auto assets=metadata["assets"].toArray();QCOMPARE(assets.size(),1);
      QCOMPARE(read(directory.filePath(assets[0].toObject()["path"].toString())),originalTexture);QVERIFY(!after.meshTextureImage.isNull());
    }
    QCOMPARE(read(source),bytes);QCOMPARE(read(directory.filePath("texture.png")),originalTexture);
  }
}

void ModelExportTests::recordedPointSceneEditUsesPivotOnceAndRetainsFrameHistory() {
  const QString python=qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_PYTHON"),root=qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_ROOT");
  if(python.isEmpty()||root.isEmpty())QSKIP("Real native export helper runtime not selected");
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const QString source=directory.filePath("points.ply");
  const QByteArray original="ply\nformat ascii 1.0\nelement vertex 2\nproperty double x\nproperty double y\nproperty double z\nproperty uchar red\nend_header\n3 6 9 23\n5 8 11 42\n";
  QVERIFY(save(source,original));
  const QJsonArray identity{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}};
  const auto sim=[&](double s,QJsonArray t){return QJsonObject{{"s",s},{"R",identity},{"t",t},{"applied",true}};};
  const auto pose=[](double x,double y,double z){return QJsonArray{QJsonArray{1,0,0,x},QJsonArray{0,1,0,y},QJsonArray{0,0,1,z},QJsonArray{0,0,0,1}};};
  const QJsonObject camera{{"view_id","pose-without-image"},{"split","train"},{"width",640},{"height",480},
      {"K",QJsonArray{QJsonArray{700,12,301},QJsonArray{0,710,222},QJsonArray{0,0,1}}},{"c2w_opencv",pose(3,4,5)}};
  QJsonObject record{{"schema","gsw.training_camera_contract/1"},{"ply_sha256",QString::fromLatin1(QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex())},
      {"input_data_identity",QString(64,'a')},{"scale",QJsonObject{{"status","source_declared"},{"source_unit","m"},{"canonical_unit","m"},{"meters_per_unit",1.},
        {"evidence",QJsonArray{QJsonObject{{"kind","user_declaration"}}}}}},{"views",QJsonArray{camera}},
      {"transforms",QJsonObject{{"source_to_canonical",sim(1.,{0,0,0})},{"canonical_to_training",sim(2.,{1,2,3})}}}};
  QVERIFY(save(cameraScaleMetadataPath(source),QJsonDocument(record).toJson()));
  ModelExportOptions options;options.sourcePath=source;options.destinationPath=directory.filePath("edited.ply");
  options.cameraCoordinateRecovery="canonical";options.recoveryPythonPath=python;options.recoveryRepositoryRoot=root;
  options.applyTransform=true;options.pivot={1e12+.25,-2e12+.5,3e12+.75};
  options.transform.scale={-2,-2,2};options.transform.translation={4,5,6};
  // The even negative signs form a proper half-turn. A large pivot makes an
  // accidental float matrix path observable in the actual exported bytes.
  options.sceneUnitScale=.001; // Legacy scene-unit conversion must not enter D.
  const auto result=exportModelFile(options);QVERIFY2(result.success,qPrintable(result.error));
  const auto metadata=QJsonDocument::fromJson(read(cameraScaleMetadataPath(options.destinationPath))).object();
  QCOMPARE(metadata["export_version"].toInt(),3);QCOMPARE(metadata["representation"].toString(),QString("point_cloud"));
  const auto edit=metadata["scene_transform"].toObject()["transform"].toObject();
  QCOMPARE(edit["s"].toDouble(),2.);
  QCOMPARE(edit["R"].toArray(),QJsonArray({QJsonArray{-1,0,0},QJsonArray{0,-1,0},QJsonArray{0,0,1}}));
  const auto t=edit["t"].toArray();
  QCOMPARE(t[0].toDouble(),3.*options.pivot.x+4.);QCOMPARE(t[1].toDouble(),3.*options.pivot.y+5.);QCOMPARE(t[2].toDouble(),-options.pivot.z+6.);
  const auto loaded=PlyPointCloudLoader::load(options.destinationPath);QVERIFY2(loaded.isValid(),qPrintable(loaded.error));
  QCOMPARE(loaded.coordinates.globalMinimum.x,(-10.+t[0].toDouble()-1.)/2.);
  QCOMPARE(loaded.coordinates.globalMaximum.z,(22.+t[2].toDouble()-3.)/2.);
  QCOMPARE(metadata["physical_scale_modified"].toBool(),true);QCOMPARE(metadata["scene_edit_independent_validation"].toBool(),false);
  QVERIFY(!cameraScaleRecoveryCapability(options.destinationPath).validationError.size());
  auto repeated=options;repeated.sourcePath=options.destinationPath;repeated.destinationPath=directory.filePath("repeated.ply");repeated.applyTransform=false;repeated.cameraCoordinateRecovery="preserve";
  const auto repeatedResult=exportModelFile(repeated);QVERIFY2(repeatedResult.success,qPrintable(repeatedResult.error));
  const auto repeatedMetadata=QJsonDocument::fromJson(read(cameraScaleMetadataPath(repeated.destinationPath))).object();
  QCOMPARE(repeatedMetadata["canonical_scene_transform"],metadata["canonical_scene_transform"]);
  const auto repeatedLoaded=PlyPointCloudLoader::load(repeated.destinationPath);QVERIFY2(repeatedLoaded.isValid(),qPrintable(repeatedLoaded.error));
  QCOMPARE(repeatedLoaded.coordinates.globalMinimum.x,loaded.coordinates.globalMinimum.x);QCOMPARE(repeatedLoaded.coordinates.globalMaximum.z,loaded.coordinates.globalMaximum.z);
  QCOMPARE(read(source),original);
  for(const QVector3D invalid:{QVector3D(1,2,1),QVector3D(-1,1,1),QVector3D(0,0,0)}) {
    auto rejected=options;rejected.destinationPath=directory.filePath("invalid.ply");rejected.transform.scale=invalid;
    const auto failed=exportModelFile(rejected);QVERIFY(!failed.success);QVERIFY(!QFileInfo::exists(rejected.destinationPath));QVERIFY(!QFileInfo::exists(cameraScaleMetadataPath(rejected.destinationPath)));
  }
}

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

void ModelExportTests::bakesCroppedAscii3dGaussianTransformAndPreservesAttributes() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("source-3dgs.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("baked-3dgs.ply"));
  QByteArray source = "ply\nformat ascii 1.0\ncomment independent TRS acceptance fixture\nelement vertex 3\n";
  for (const char *name : {"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                          "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
    source += QByteArray("property double ") + name + '\n';
  for (int index = 0; index < 9; ++index)
    source += "property double f_rest_" + QByteArray::number(index) + '\n';
  source += "property uint research_id\nproperty double confidence_raw\nend_header\n";
  const QByteArray attributes = " 1 0 0 0.3 -0.2 0.6 0.75 0 -1 -2 1 0 0 0 1 2 3 4 5 6 7 8 9 4294967291 1.2345678901234567\n";
  source += "1 2 3" + attributes;
  source += "4 5 6" + attributes;
  source += "-1 0 2" + attributes;
  QVERIFY(save(o.sourcePath, source));
  const auto input = PlyPointCloudLoader::load(o.sourcePath);
  QVERIFY2(input.isValid(), qPrintable(input.error));
  QVERIFY(input.hasGaussianAttributes);
  QVERIFY(!input.hasSurfelAttributes);
  o.applyTransform = true;
  o.transform.translation = {10, 20, 30};
  o.transform.scale = {2, 2, 2};
  o.transform.rotation = QQuaternion::fromAxisAndAngle({0, 0, 1}, 90);
  o.deletedVertices.resize(3);
  o.deletedVertices.setBit(1);
  const auto result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));

  const auto output = PlyPointCloudLoader::load(o.destinationPath);
  QVERIFY2(output.isValid(), qPrintable(output.error));
  QVERIFY(output.hasGaussianAttributes);
  QVERIFY(!output.hasSurfelAttributes);
  QCOMPARE(output.sourceVertexCount, 2);
  // Known example: S2 then Rz90 then T(10,20,30), with the middle point removed.
  const std::array<std::array<double, 3>, 2> positions{{{6, 22, 36}, {10, 18, 34}}};
  const std::array<double, 3> logScales{0.6931471805599453, -0.3068528194400547, -1.3068528194400546};
  // Degree-one real SH uses [-y,z,-x]; +90 degrees about Z maps [a,b,c] to [c,b,-a].
  const std::array<double, 9> harmonics{3, 2, -1, 6, 5, -4, 9, 8, -7};
  int seen = 0;
  PlyGaussianVisitor gaussians;
  gaussians.begin = [](qint64 count, int degree, bool) { return count == 2 && degree == 1; };
  gaussians.gaussian = [&](qint64 index, const std::array<double, 86> &value) {
    if (index < 0 || index >= 2) return false;
    ++seen;
    for (int axis = 0; axis < 3; ++axis) {
      if (std::abs(value[axis] - positions[index][axis]) > 1e-6) return false;
      if (std::abs(value[7 + axis] - logScales[axis]) > 1e-6) return false;
    }
    if (value[3] != 0.3 || value[4] != -0.2 || value[5] != 0.6 || value[6] != 0.75) return false;
    // Identity source quaternion becomes Rz90, up to the equivalent quaternion sign.
    if (std::abs(std::abs(value[10]) - 0.7071067811865476) > 1e-6 ||
        std::abs(value[11]) > 1e-6 || std::abs(value[12]) > 1e-6 ||
        std::abs(value[10] - value[13]) > 1e-6) return false;
    for (int coefficient = 0; coefficient < 9; ++coefficient)
      if (std::abs(value[14 + coefficient] - harmonics[coefficient]) > 1e-6) return false;
    return true;
  };
  QString error;
  QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(o.destinationPath, gaussians, error), qPrintable(error));
  QCOMPARE(seen, 2);
  int normalsSeen = 0;
  PlyGeometryVisitor geometry;
  geometry.vertex = [&](qint64, const PlySourceVertex &vertex) {
    ++normalsSeen;
    return std::abs(vertex.normal.x()) < 1e-6 && std::abs(vertex.normal.y() - 1.0F) < 1e-6 &&
        std::abs(vertex.normal.z()) < 1e-6;
  };
  QVERIFY2(PlyPointCloudLoader::visitSourceGeometry(o.destinationPath, geometry, error), qPrintable(error));
  QCOMPARE(normalsSeen, 2);
  const QByteArray exported = read(o.destinationPath);
  QVERIFY(exported.startsWith("ply\nformat ascii 1.0\n"));
  QVERIFY(exported.contains("property uint research_id\nproperty double confidence_raw\n"));
  QCOMPARE(exported.count("property "), source.count("property "));
  const auto rows = exported.mid(exported.indexOf("end_header\n") + 11).trimmed().split('\n');
  QCOMPARE(rows.size(), 2);
  for (const auto &row : rows) {
    const auto values = row.simplified().split(' ');
    QCOMPARE(values.size(), 28);
    QCOMPARE(values[26], QByteArray("4294967291"));
    QCOMPARE(values[27], QByteArray("1.2345678901234567"));
  }
  QCOMPARE(read(o.sourcePath), source);
}

void ModelExportTests::bakesBinary2dGaussianNonUniformTransformWithoutInventingThickness() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  for (const auto endian : {QDataStream::LittleEndian, QDataStream::BigEndian}) {
    ModelExportOptions o;
    o.sourcePath = dir.filePath(QStringLiteral("surfel-source.ply"));
    o.destinationPath = dir.filePath(QStringLiteral("surfel-baked.ply"));
    QFile file(o.sourcePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray format = endian == QDataStream::LittleEndian ? "binary_little_endian" : "binary_big_endian";
    file.write("ply\nformat " + format + " 1.0\nelement vertex 3\n");
    file.write("property double x\nproperty double y\nproperty double z\n");
    for (const char *name : {"nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                            "scale_0", "scale_1", "rot_0", "rot_1", "rot_2", "rot_3"})
      file.write(QByteArray("property float ") + name + '\n');
    file.write("property uint research_id\nproperty double confidence_raw\nend_header\n");
    QDataStream sourceStream(&file);
    sourceStream.setByteOrder(endian);
    for (int index = 0; index < 3; ++index) {
      sourceStream.setFloatingPointPrecision(QDataStream::DoublePrecision);
      sourceStream << 1000000000.125 + index << 2.0 + index << 3.0 + index;
      sourceStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
      // A zero-thickness ellipse: principal radii 2 and 3, rotated 45 degrees around Z.
      sourceStream << 0.0F << 0.0F << 1.0F << 0.3F << -0.2F << 0.6F << 0.75F
                   << 0.6931471805599453F << 1.0986122886681098F
                   << 0.9238795325112867F << 0.0F << 0.0F << 0.3826834323650898F
                   << quint32(4294967291U);
      sourceStream.setFloatingPointPrecision(QDataStream::DoublePrecision);
      sourceStream << 1.2345678901234567;
    }
    QCOMPARE(sourceStream.status(), QDataStream::Ok);
    file.close();
    const auto source = read(o.sourcePath);
    const auto original = PlyPointCloudLoader::load(o.sourcePath);
    QVERIFY2(original.isValid(), qPrintable(original.error));
    QVERIFY(original.hasGaussianAttributes);
    QVERIFY(original.hasSurfelAttributes);
    o.applyTransform = true;
    o.pivot = {1000000000.125, 2, 3};
    o.transform.translation = {10, 20, 30};
    o.transform.scale = {2, 3, 4};
    o.transform.rotation = QQuaternion::fromAxisAndAngle({0, 1, 0}, 90);
    o.deletedVertices.resize(3);
    o.deletedVertices.setBit(1);
    const auto result = exportModelFile(o);
    QVERIFY2(result.success, qPrintable(result.error));

    const auto reloaded = PlyPointCloudLoader::load(o.destinationPath);
    QVERIFY2(reloaded.isValid(), qPrintable(reloaded.error));
    QVERIFY(reloaded.hasGaussianAttributes);
    QVERIFY(reloaded.hasSurfelAttributes);
    QCOMPARE(reloaded.sourceVertexCount, 2);
    const auto output = read(o.destinationPath);
    const auto headerEnd = output.indexOf("end_header\n") + 11;
    const auto sourceHeaderEnd = source.indexOf("end_header\n") + 11;
    QVERIFY(output.startsWith("ply\nformat " + format + " 1.0\n"));
    QVERIFY(!output.left(headerEnd).contains("property float scale_2\n"));
    QCOMPARE(output.left(headerEnd).count("property "), 18);
    constexpr qsizetype recordBytes = 88;
    QCOMPARE(output.size() - headerEnd, 2 * recordBytes);
    QDataStream decoded(output.mid(headerEnd));
    decoded.setByteOrder(endian);
    const std::array<std::array<double, 3>, 2> positions{{
        {1000000010.125, 22, 33}, {1000000018.125, 28, 29}}};
    // R_y(90) S(2,3,4) transforms the source XY ellipse covariance into YZ.
    const std::array<std::array<double, 3>, 3> covariance{{{0, 0, 0}, {0, 58.5, 15}, {0, 15, 26}}};
    for (int record = 0; record < 2; ++record) {
      decoded.setFloatingPointPrecision(QDataStream::DoublePrecision);
      double x, y, z;
      decoded >> x >> y >> z;
      QVERIFY(std::abs(x - positions[record][0]) < 1e-6);
      QVERIFY(std::abs(y - positions[record][1]) < 1e-6);
      QVERIFY(std::abs(z - positions[record][2]) < 1e-6);
      decoded.setFloatingPointPrecision(QDataStream::SinglePrecision);
      float nx, ny, nz, dc0, dc1, dc2, opacity, s0, s1, qw, qx, qy, qz;
      quint32 researchId;
      decoded >> nx >> ny >> nz >> dc0 >> dc1 >> dc2 >> opacity >> s0 >> s1 >> qw >> qx >> qy >> qz >> researchId;
      decoded.setFloatingPointPrecision(QDataStream::DoublePrecision);
      double confidence;
      decoded >> confidence;
      QVERIFY(std::abs(nx - 1.0F) < 1e-6 && std::abs(ny) < 1e-6 && std::abs(nz) < 1e-6);
      QCOMPARE(dc0, 0.3F); QCOMPARE(dc1, -0.2F); QCOMPARE(dc2, 0.6F); QCOMPARE(opacity, 0.75F);
      QCOMPARE(researchId, 4294967291U);
      QCOMPARE(confidence, 1.2345678901234567);
      const QQuaternion orientation(qw, qx, qy, qz);
      QVERIFY(std::abs(orientation.length() - 1.0F) < 1e-6);
      const auto first = orientation.rotatedVector({1, 0, 0});
      const auto second = orientation.rotatedVector({0, 1, 0});
      const double firstVariance = std::exp(2.0 * s0), secondVariance = std::exp(2.0 * s1);
      for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column)
        QVERIFY(std::abs(firstVariance * first[row] * first[column] +
                         secondVariance * second[row] * second[column] - covariance[row][column]) < 3e-5);
      // The raw source DC/opacity, custom uint and custom double bytes survive both byte orders.
      const auto outputRecord = output.mid(headerEnd + record * recordBytes, recordBytes);
      const auto sourceRecord = source.mid(sourceHeaderEnd + (record == 0 ? 0 : 2) * recordBytes, recordBytes);
      QCOMPARE(outputRecord.mid(36, 16), sourceRecord.mid(36, 16));
      QCOMPARE(outputRecord.mid(76, 12), sourceRecord.mid(76, 12));
    }
    QCOMPARE(decoded.status(), QDataStream::Ok);
    QVERIFY(decoded.atEnd());
    QCOMPARE(read(o.sourcePath), source);
  }
}

void ModelExportTests::recordedCameraMetadataCannotBeBypassedByPlainGaussianBake() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("recorded-gaussians.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("baked-gaussians.ply"));
  const auto original = gaussianPly(0);
  QVERIFY(save(o.sourcePath, original));
  const QJsonArray identity{QJsonArray{1, 0, 0}, QJsonArray{0, 1, 0}, QJsonArray{0, 0, 1}};
  const auto transform = [&](double scale, QJsonArray translation) {
    return QJsonObject{{"s", scale}, {"R", identity}, {"t", translation}, {"applied", true}};
  };
  const QJsonObject record{{"schema", "gsw.training_camera_contract/1"},
      {"ply_sha256", QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex())},
      {"input_data_identity", QString(64, 'b')},
      {"scale", QJsonObject{{"status", "source_declared"}, {"source_unit", "m"}, {"canonical_unit", "m"},
          {"meters_per_unit", 1.0}, {"evidence", QJsonArray{QJsonObject{{"kind", "user_declaration"}}}}}},
      {"transforms", QJsonObject{{"source_to_canonical", transform(1.0, {0, 0, 0})},
          {"canonical_to_training", transform(2.0, {1, 2, 3})}}}};
  const auto sidecar = QJsonDocument(record).toJson();
  QVERIFY(save(cameraScaleMetadataPath(o.sourcePath), sidecar));
  QVERIFY(cameraScaleRecoveryCapability(o.sourcePath).preserve);
  o.applyTransform = true;
  o.transform.translation = {10, 20, 30};
  QVERIFY(save(o.destinationPath, "existing destination"));
  // A scene-coordinate write must not silently discard a verified recorded-frame contract.
  const auto result = exportModelFile(o);
  QVERIFY(!result.success);
  QVERIFY(!result.error.isEmpty());
  QCOMPARE(read(o.destinationPath), QByteArray("existing destination"));
  QVERIFY(!QFileInfo::exists(cameraScaleMetadataPath(o.destinationPath)));
  QCOMPARE(read(o.sourcePath), original);
  QCOMPARE(read(cameraScaleMetadataPath(o.sourcePath)), sidecar);
}

void ModelExportTests::gaussianTranslationPreservesDoublePrecisionNormalValues() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("double-normals.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("translated.ply"));
  auto source = gaussianPly(0);
  source.replace("end_header\n", "property double nx\nproperty double ny\nproperty double nz\nend_header\n");
  const auto headerEnd = source.indexOf("end_header\n") + 11;
  QByteArray complete = source.left(headerEnd);
  for (const auto &row : source.mid(headerEnd).trimmed().split('\n'))
    complete += row + " 1e-80 0 0.123456789012345\n";
  QVERIFY(save(o.sourcePath, complete));
  o.applyTransform = true;
  o.transform.translation = {1, 2, 3};
  const auto result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  const auto output = read(o.destinationPath);
  const auto rows = output.mid(output.indexOf("end_header\n") + 11).trimmed().split('\n');
  QCOMPARE(rows.size(), 3);
  for (const auto &row : rows) {
    const auto values = row.simplified().split(' ');
    QCOMPARE(values.size(), 17);
    QCOMPARE(values[14].toDouble(), 1e-80);
    QCOMPARE(values[15].toDouble(), 0.0);
    QCOMPARE(values[16].toDouble(), 0.123456789012345);
  }
  QCOMPARE(read(o.sourcePath), complete);
}

void ModelExportTests::nonUniformGaussianBakeRejectsOnlyRetainedDirectionalHarmonics() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("directional.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("strict.ply"));
  const auto fixture = gaussianPly(1);
  const auto headerEnd = fixture.indexOf("end_header\n") + 11;
  QByteArray source = fixture.left(headerEnd);
  const auto rows = fixture.mid(headerEnd).trimmed().split('\n');
  for (int row = 0; row < rows.size(); ++row) {
    auto values = rows[row].simplified().split(' ');
    for (int index = 14; index < values.size(); ++index) values[index] = "0";
    // Even a tiny but non-zero coefficient must not silently enter an approximation.
    if (row == 1) values[14] = "1e-40";
    source += values.join(' ') + '\n';
  }
  QVERIFY(save(o.sourcePath, source));
  QVERIFY(save(o.destinationPath, "existing destination"));
  o.applyTransform = true;
  o.transform.scale = {2, 3, 4};
  const auto rejected = exportModelFile(o);
  QVERIFY(!rejected.success);
  QVERIFY(!rejected.error.isEmpty());
  QCOMPARE(read(o.destinationPath), QByteArray("existing destination"));
  QCOMPARE(read(o.sourcePath), source);
  // Once the sole directional point is removed, the retained DC-only ellipsoids are exact.
  o.deletedVertices.resize(3);
  o.deletedVertices.setBit(1);
  const auto cropped = exportModelFile(o);
  QVERIFY2(cropped.success, qPrintable(cropped.error));
  const auto output = PlyPointCloudLoader::load(o.destinationPath);
  QVERIFY2(output.isValid(), qPrintable(output.error));
  QVERIFY(output.hasGaussianAttributes);
  QCOMPARE(output.sourceVertexCount, 2);
  int checked = 0;
  PlyGaussianVisitor visitor;
  visitor.begin = [](qint64 count, int degree, bool) { return count == 2 && degree == 1; };
  visitor.gaussian = [&](qint64, const std::array<double, 86> &values) {
    ++checked;
    for (int coefficient = 14; coefficient < 23; ++coefficient)
      if (values[coefficient] != 0.0) return false;
    return true;
  };
  QString error;
  QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(o.destinationPath, visitor, error), qPrintable(error));
  QCOMPARE(checked, 2);
  QCOMPARE(read(o.sourcePath), source);
}

void ModelExportTests::gaussianBakeRejectsDuplicateScaleAndRotationProperties() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("ambiguous.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("protected.ply"));
  o.applyTransform = true;
  o.transform.translation = {1, 2, 3};
  for (const QByteArray &name : {QByteArray("scale_0"), QByteArray("rot_0")}) {
    auto fixture = gaussianPly(0);
    fixture.replace("end_header\n", "property double " + name + "\nend_header\n");
    const auto headerEnd = fixture.indexOf("end_header\n") + 11;
    QByteArray source = fixture.left(headerEnd);
    for (const auto &row : fixture.mid(headerEnd).trimmed().split('\n')) source += row + " 1\n";
    QVERIFY(save(o.sourcePath, source));
    QVERIFY(save(o.destinationPath, "existing destination"));
    const auto result = exportModelFile(o);
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(read(o.destinationPath), QByteArray("existing destination"));
    QCOMPARE(read(o.sourcePath), source);
  }
}

void ModelExportTests::mirrorsBinary3dGaussiansAndDirectionalDegreeThreeAndFourAppearance() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  for (int degree : {3, 4}) {
    ModelExportOptions o;
    o.sourcePath = dir.filePath(QStringLiteral("directional-binary.ply"));
    o.destinationPath = dir.filePath(QStringLiteral("mirrored.ply"));
    QFile file(o.sourcePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ply\nformat binary_little_endian 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\n");
    for (const char *name : {"f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2",
                            "rot_0", "rot_1", "rot_2", "rot_3"})
      file.write(QByteArray("property float ") + name + '\n');
    const int dim = (degree + 1) * (degree + 1) - 1;
    for (int index = 0; index < 3*dim; ++index)
      file.write("property float f_rest_" + QByteArray::number(index) + '\n');
    file.write("property uchar research_class\nproperty double confidence_raw\nend_header\n");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << 1.0 << 2.0 << 3.0;
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << 0.3F << -0.2F << 0.6F << 0.75F << 0.0F << 0.6931471805599453F << 1.0986122886681098F
           << 0.9238795325112867F << 0.0F << 0.0F << 0.3826834323650898F;
    std::array<double, 72> sourceCoefficients{};
    for (int index = 0; index < 3*dim; ++index) {
      const float coefficient = (index % 11 - 5) * 0.07F;
      sourceCoefficients[index] = coefficient;
      stream << coefficient;
    }
    stream << quint8(237);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << 1.2345678901234567;
    QCOMPARE(stream.status(), QDataStream::Ok);
    file.close();
    const auto source = read(o.sourcePath);
    o.applyTransform = true;
    o.transform.scale = {-2, 2, 2};
    o.transform.translation = {10, 20, 30};
    const auto result = exportModelFile(o);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto reloaded = PlyPointCloudLoader::load(o.destinationPath);
    QVERIFY2(reloaded.isValid(), qPrintable(reloaded.error));
    QVERIFY(reloaded.hasGaussianAttributes);
    QVERIFY(!reloaded.hasSurfelAttributes);
    QCOMPARE(reloaded.sourceVertexCount, 1);
    const std::array<std::array<double, 3>, 10> directions{{
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1},
        {.6, .8, 0}, {.36, .48, .8}, {-2.0/3, 1.0/3, 2.0/3}, {.7071067811865476, 0, -.7071067811865476}}};
    const std::array<std::array<double, 3>, 3> covariance{{{10, 6, 0}, {6, 10, 0}, {0, 0, 36}}};
    int checked = 0;
    PlyGaussianVisitor visitor;
    visitor.begin = [degree](qint64 count, int outputDegree, bool) { return count == 1 && outputDegree == degree; };
    visitor.gaussian = [&](qint64, const std::array<double, 86> &value) {
      ++checked;
      if (std::abs(value[0]-8) > 1e-9 || std::abs(value[1]-24) > 1e-9 || std::abs(value[2]-36) > 1e-9) return false;
      const QQuaternion rotation(value[10], value[11], value[12], value[13]);
      const std::array<QVector3D, 3> axes{rotation.rotatedVector({1, 0, 0}),
          rotation.rotatedVector({0, 1, 0}), rotation.rotatedVector({0, 0, 1})};
      for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        double actual = 0;
        for (int axis = 0; axis < 3; ++axis) actual += std::exp(2*value[7+axis])*axes[axis][row]*axes[axis][column];
        if (std::abs(actual-covariance[row][column]) > 3e-5) return false;
      }
      for (const auto &direction : directions) {
        const auto outputBasis = cartesianSh(direction);
        const auto inputBasis = cartesianSh({-direction[0], direction[1], direction[2]});
        for (int channel = 0; channel < 3; ++channel) {
          double actual = 0, expected = 0;
          for (int coefficient = 0; coefficient < dim; ++coefficient) {
            actual += value[14+channel*dim+coefficient]*outputBasis[coefficient+1];
            expected += sourceCoefficients[channel*dim+coefficient]*inputBasis[coefficient+1];
          }
          if (std::abs(actual-expected) > 2e-6) return false;
        }
      }
      return true;
    };
    QString error;
    QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(o.destinationPath, visitor, error), qPrintable(error));
    QCOMPARE(checked, 1);
    const auto output = read(o.destinationPath);
    const auto outputHeaderEnd = output.indexOf("end_header\n") + 11;
    const auto sourceHeaderEnd = source.indexOf("end_header\n") + 11;
    QCOMPARE(output.left(outputHeaderEnd).count("property "), 16+3*dim);
    QCOMPARE(output.size()-outputHeaderEnd, source.size()-sourceHeaderEnd);
    QCOMPARE(output.mid(outputHeaderEnd+24, 16), source.mid(sourceHeaderEnd+24, 16));
    QCOMPARE(output.right(9), source.right(9));
    QCOMPARE(read(o.sourcePath), source);
  }
}

void ModelExportTests::bakesNonUniformDcOnly3dGaussianCovariance() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ModelExportOptions o;
  o.sourcePath = dir.filePath(QStringLiteral("dc-ellipsoid.ply"));
  o.destinationPath = dir.filePath(QStringLiteral("nonuniform.ply"));
  const auto fixture = gaussianPly(0);
  QByteArray source = fixture.left(fixture.indexOf("end_header\n") + 11);
  source.replace("element vertex 3\n", "element vertex 1\n");
  // Rz45 principal radii (1,2,3), so its source XY covariance has diagonal 2.5 and off-diagonal -1.5.
  source += "1 2 3 0.3 -0.2 0.6 0.75 0 0.6931471805599453 1.0986122886681098 0.9238795325112867 0 0 0.3826834323650898\n";
  QVERIFY(save(o.sourcePath, source));
  o.applyTransform = true;
  o.transform.scale = {2, 3, 4};
  o.transform.rotation = QQuaternion::fromAxisAndAngle({0, 1, 0}, 90);
  o.transform.translation = {10, 20, 30};
  const auto result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  const auto output = PlyPointCloudLoader::load(o.destinationPath);
  QVERIFY2(output.isValid(), qPrintable(output.error));
  QVERIFY(output.hasGaussianAttributes);
  QVERIFY(!output.hasSurfelAttributes);
  QCOMPARE(output.sourceVertexCount, 1);
  const std::array<std::array<double, 3>, 3> covariance{{{144, 0, 0}, {0, 22.5, 9}, {0, 9, 10}}};
  int checked = 0;
  PlyGaussianVisitor visitor;
  visitor.begin = [](qint64 count, int degree, bool) { return count == 1 && degree == 0; };
  visitor.gaussian = [&](qint64, const std::array<double, 86> &value) {
    ++checked;
    if (std::abs(value[0]-22) > 1e-9 || std::abs(value[1]-26) > 1e-9 || std::abs(value[2]-28) > 1e-9) return false;
    const QQuaternion orientation(value[10], value[11], value[12], value[13]);
    const std::array<QVector3D, 3> axes{orientation.rotatedVector({1, 0, 0}),
        orientation.rotatedVector({0, 1, 0}), orientation.rotatedVector({0, 0, 1})};
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
      double actual = 0;
      for (int axis = 0; axis < 3; ++axis) actual += std::exp(2*value[7+axis])*axes[axis][row]*axes[axis][column];
      if (std::abs(actual-covariance[row][column]) > 3e-5) return false;
    }
    return value[3] == .3 && value[4] == -.2 && value[5] == .6 && value[6] == .75;
  };
  QString error;
  QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(o.destinationPath, visitor, error), qPrintable(error));
  QCOMPARE(checked, 1);
  QCOMPARE(read(o.sourcePath), source);
}

void ModelExportTests::gaussianBakeCancellationKeepsDestinationAndBothSourceKinds() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  for (bool surfel : {false, true}) {
    ModelExportOptions o;
    o.sourcePath = dir.filePath(QStringLiteral("source.ply"));
    o.destinationPath = dir.filePath(QStringLiteral("existing.ply"));
    auto source = gaussianPly(3);
    if (surfel) {
      source.replace("property double scale_2\n", "");
      source.replace("0.75 -2 -3 -4 ", "0.75 -2 -3 ");
    }
    QVERIFY(save(o.sourcePath, source));
    QVERIFY(save(o.destinationPath, "existing destination"));
    o.applyTransform = true;
    o.transform.scale = {2, 2, 2};
    o.transform.rotation = QQuaternion::fromAxisAndAngle({0, 0, 1}, 90);
    bool reachedCommitBoundary = false;
    o.progress = [&](int percent) {
      if (percent != 99) return false;
      reachedCommitBoundary = true;
      return true;
    };
    const auto result = exportModelFile(o);
    QVERIFY(reachedCommitBoundary);
    QVERIFY(result.cancelled);
    QVERIFY(!result.success);
    QCOMPARE(read(o.destinationPath), QByteArray("existing destination"));
    QCOMPARE(read(o.sourcePath), source);
  }
}

void ModelExportTests::realGaussianPlyBakeWhenRequested() {
  const QString source = qEnvironmentVariable("GSW_GAUSSIAN_BAKE_SOURCE");
  if (source.isEmpty()) QSKIP("Set GSW_GAUSSIAN_BAKE_SOURCE for an explicitly selected read-only real-model bake.");
  const QString outputDirectory = qEnvironmentVariable("GSW_GAUSSIAN_BAKE_OUTPUT_DIR");
  QVERIFY2(!outputDirectory.isEmpty(), "Select a separate acceptance output directory.");
  QVERIFY(QFileInfo::exists(source));
  QVERIFY(QDir().mkpath(outputDirectory));
  const QDir outputs(outputDirectory);
  const QString destination = outputs.filePath(QStringLiteral("outputbaked.ply"));
  QVERIFY(QFileInfo(source).absoluteFilePath() != QFileInfo(destination).absoluteFilePath());
  const auto digest = [](const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return QByteArray();
    return hash.result();
  };
  const auto sourceHash = digest(source);
  QVERIFY(!sourceHash.isEmpty());
  const QString sidecarPath = cameraScaleMetadataPath(source);
  const bool recordedCoordinates = QFileInfo::exists(sidecarPath);
  const auto originalSidecar = recordedCoordinates ? read(sidecarPath) : QByteArray();
  if (recordedCoordinates) {
    const auto capability = cameraScaleRecoveryCapability(source);
    QVERIFY2(capability.preserve, qPrintable(capability.validationError));
  }
  const auto original = PlyPointCloudLoader::load(source, 64);
  QVERIFY2(original.isValid(), qPrintable(original.error));
  QVERIFY(original.hasGaussianAttributes);
  QVERIFY(original.sourceVertexCount > 0);
  ModelExportOptions o;
  o.sourcePath = source;
  o.destinationPath = destination;
  o.applyTransform = true;
  o.transform.scale = {2, 2, 2};
  o.transform.rotation = QQuaternion::fromAxisAndAngle({0, 0, 1}, 90);
  o.transform.translation = {10, 20, 30};
  if (recordedCoordinates) {
    o.cameraCoordinateRecovery = QStringLiteral("preserve");
    o.recordedRepresentation = QStringLiteral("gaussian");
    o.recoveryPythonPath = qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_PYTHON");
    o.recoveryRepositoryRoot = qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_ROOT");
    QVERIFY(QFileInfo::exists(o.recoveryPythonPath));
    QVERIFY(QFileInfo::exists(QDir(o.recoveryRepositoryRoot).filePath(QStringLiteral("native/backend/camera_scale_export.py"))));
  }
  o.deletedVertices.resize(original.sourceVertexCount);
  for (qsizetype index = 0; index < o.deletedVertices.size(); index += 7) o.deletedVertices.setBit(index);
  const auto result = exportModelFile(o);
  QVERIFY2(result.success, qPrintable(result.error));
  const auto reloaded = PlyPointCloudLoader::load(destination, 64);
  QVERIFY2(reloaded.isValid(), qPrintable(reloaded.error));
  QVERIFY(reloaded.hasGaussianAttributes);
  QCOMPARE(reloaded.hasSurfelAttributes, original.hasSurfelAttributes);
  QCOMPARE(reloaded.sourceVertexCount, original.sourceVertexCount-o.deletedVertices.count(true));
  QCOMPARE(digest(source), sourceHash);
  if (recordedCoordinates) {
    QCOMPARE(read(sidecarPath), originalSidecar);
    QVERIFY(QFileInfo::exists(cameraScaleMetadataPath(destination)));
    QVERIFY(cameraScaleRecoveryCapability(destination).preserve);
  }
  qInfo() << "Real Gaussian PLY bake:" << original.sourceVertexCount << "source points;"
          << reloaded.sourceVertexCount << "retained; output" << destination
          << "source SHA256 unchanged" << sourceHash.toHex();
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

void ModelExportTests::sceneUnitConversionBakesGaussianPlyAndKeepsSpzSourceProtection() {
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
    const auto baked = exportModelFile(o);
    if (format == ModelExportFormat::Ply) {
      QVERIFY2(baked.success, qPrintable(baked.error));
      const auto loaded = PlyPointCloudLoader::load(o.destinationPath);
      QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
      QVERIFY(loaded.hasGaussianAttributes);
      QCOMPARE(loaded.coordinates.unit, SceneLengthUnit::Metres);
      QVERIFY(std::abs(loaded.coordinates.globalMinimum.x - 0.0112345) < 1e-10);
      QVERIFY(std::abs(loaded.coordinates.globalMinimum.y + 0.0225) < 1e-10);
      int checked = 0;
      PlyGaussianVisitor visitor;
      visitor.gaussian = [&](qint64, const std::array<double, 86> &value) {
        ++checked;
        return std::abs(value[7] + 6.605170185988091) < 1e-10 &&
               std::abs(value[8] + 7.605170185988091) < 1e-10 &&
               std::abs(value[9] + 8.605170185988092) < 1e-10;
      };
      QString error;
      QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(o.destinationPath, visitor, error), qPrintable(error));
      QCOMPARE(checked, 3);
    } else {
      QVERIFY(!baked.success);
      QCOMPARE(read(o.destinationPath), QByteArray("existing data"));
    }
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
  o.coordinates.unit = SceneLengthUnit::Centimetres; o.coordinates.unitDeclared = true;
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
  o.coordinates.unit = SceneLengthUnit::Centimetres; o.coordinates.unitDeclared = true;
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
  o.coordinates.unit = SceneLengthUnit::Centimetres; o.coordinates.unitDeclared = true;
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

void ModelExportTests::spzCodecRejectsInvalidHeaderMetadata() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {1, 2, 3}; cloud.colors = {.1F, .2F, .3F};
  cloud.scales = {-2, -3, -4}; cloud.alphas = {.7F}; cloud.rotations = {0, 0, 0, 1};
  spz::PackOptions options; options.version = 4;
  std::vector<uint8_t> bytes;
  QVERIFY(spz::saveSpz(cloud, options, &bytes));
  QCOMPARE(spz::loadSpzPacked(bytes).numPoints, 1);
  for (const auto [offset, value] : {std::pair<size_t, uint8_t>{12, 5}, {13, 31}}) {
    auto invalid = bytes; invalid[offset] = value;
    QCOMPARE(spz::loadSpzPacked(invalid).numPoints, 0);
  }
}

void ModelExportTests::spzCodecChecksPackedIndicesAndBuffers() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {1, 2, 3}; cloud.colors = {.1F, .2F, .3F};
  cloud.scales = {-2, -3, -4}; cloud.alphas = {.7F}; cloud.rotations = {0, 0, 0, 1};
  spz::PackOptions options;
  std::vector<uint8_t> bytes;
  QVERIFY(spz::saveSpz(cloud, options, &bytes));
  auto packed = spz::loadSpzPacked(bytes);
  const auto first = packed.at(0);
  for (const auto *coefficients : {&first.shR, &first.shG, &first.shB})
    for (const auto coefficient : *coefficients) QCOMPARE(coefficient, uint8_t(128));
  QVERIFY_THROWS_EXCEPTION(std::out_of_range, packed.at(packed.numPoints));
  QVERIFY_THROWS_EXCEPTION(std::out_of_range, packed.at(-1));
  packed.colors.resize(2);
  QVERIFY_THROWS_EXCEPTION(std::out_of_range, packed.at(0));
  QVERIFY_THROWS_EXCEPTION(std::out_of_range, spz::PackedGaussians{}.at(0));
}

void ModelExportTests::spzCodecSaturatesExtremeFiniteSh() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1; cloud.shDegree = 1;
  cloud.positions = {1, 2, 3}; cloud.colors = {.1F, .2F, .3F};
  cloud.scales = {-2, -3, -4}; cloud.alphas = {.7F}; cloud.rotations = {0, 0, 0, 1};
  cloud.sh.assign(9, 0); cloud.sh[0] = 1e20F; cloud.sh[1] = -1e20F;
  spz::PackOptions options; options.from = spz::CoordinateSystem::RUB;
  std::vector<uint8_t> bytes;
  QVERIFY(spz::saveSpz(cloud, options, &bytes));
  const auto packed = spz::loadSpzPacked(bytes);
  QCOMPARE(packed.numPoints, 1);
  QCOMPARE(packed.sh[0], uint8_t(255));
  QCOMPARE(packed.sh[1], uint8_t(0));
  QCOMPARE(packed.sh[2], uint8_t(128));
}

void ModelExportTests::spzCodecChecksLegacyPayloadBeforeAllocation() {
  QByteArray raw(36, '\0');
  qToLittleEndian<quint32>(spz::NGSP_MAGIC, raw.data());
  qToLittleEndian<quint32>(3, raw.data() + 4);
  qToLittleEndian<quint32>(1, raw.data() + 8);
  raw[13] = 12;
  std::vector<uint8_t> compressed;
  const auto encode = [&] {
    return spz::compressGzipped(reinterpret_cast<const uint8_t *>(raw.constData()), raw.size(), &compressed);
  };
  QVERIFY(encode());
  QCOMPARE(spz::loadSpzPacked(compressed).numPoints, 1);
  raw[13] = 31;
  QVERIFY(encode());
  QCOMPARE(spz::loadSpzPacked(compressed).numPoints, 0);
  raw[13] = 12;
  raw.resize(16);
  // Stay bounded even if the length guard regresses: at most ~20 KiB of attributes.
  for (quint32 count : {quint32(0), quint32(1), quint32(1024)}) {
    qToLittleEndian<quint32>(count, raw.data() + 8);
    QVERIFY(encode());
    QCOMPARE(spz::loadSpzPacked(compressed).numPoints, 0);
  }
  raw.resize(36); raw.fill('\0');
  qToLittleEndian<quint32>(spz::NGSP_MAGIC, raw.data());
  qToLittleEndian<quint32>(4, raw.data() + 4);
  qToLittleEndian<quint32>(1, raw.data() + 8); raw[13] = 12;
  QVERIFY(encode());
  QCOMPARE(spz::loadSpzPacked(compressed).numPoints, 0);
}

void ModelExportTests::spzCodecRejectsIncompleteLargeCloudMetadata() {
  spz::PackOptions options;
  for (int32_t count : {int32_t(-1), std::numeric_limits<int32_t>::max(),
      std::numeric_limits<int32_t>::max() / 9 + 1}) {
    spz::GaussianCloud cloud; cloud.numPoints = count; cloud.shDegree = 4;
    const std::vector<uint8_t> sentinel{0x47, 0x53, 0x57};
    auto output = sentinel;
    QVERIFY(!spz::saveSpz(cloud, options, &output));
    QVERIFY(output == sentinel);
  }
}

void ModelExportTests::spzCodecRejectsConstructedFractionalBits() {
  spz::PackedGaussians packed; packed.numPoints = 1;
  packed.positions.assign(9, 0); packed.scales.assign(3, 0);
  packed.rotations.assign(4, 0); packed.colors.assign(3, 0); packed.alphas.assign(1, 0);
  const auto converter = spz::coordinateConverter(spz::CoordinateSystem::RUB, spz::CoordinateSystem::RDF, 0);
  for (int bits : {-1, 255}) {
    packed.fractionalBits = bits;
    QVERIFY_THROWS_EXCEPTION(std::out_of_range, packed.at(0));
    QVERIFY_THROWS_EXCEPTION(std::out_of_range, packed.unpack(0, converter));
    QVERIFY_THROWS_EXCEPTION(std::out_of_range, spz::PackedGaussian{}.unpack(false, true, bits, converter));
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

void ModelExportTests::trainingCoordinateScaleReachesMeasurementAndJsonReport() {
  QTemporaryDir dir;
  const QString source = dir.filePath("training.ply");
  QVERIFY(save(source, gaussianPly(0, 1.)));
  const QJsonObject scale{{"status", "source_declared"}, {"source_unit", "mm"},
      {"meters_per_unit", .001}, {"canonical_unit", "m"},
      {"evidence", QJsonArray{QJsonObject{{"kind", "source_declaration"}}}}};
  const QJsonObject training{{"s", 8.}, {"R", QJsonArray{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}}},
      {"t", QJsonArray{0,0,0}}, {"applied", true}};
  const QString identity(64, QLatin1Char('a'));
  const QJsonObject sidecar{{"schema", "gsw.training_camera_contract/1"}, {"scale", scale},
      {"transforms", QJsonObject{{"canonical_to_training", training}}},
      {"input_data_identity", identity},
      {"ply_sha256", QString::fromLatin1(QCryptographicHash::hash(read(source), QCryptographicHash::Sha256).toHex())}};
  QVERIFY(save(dir.filePath("training.camera-scale.json"), QJsonDocument(sidecar).toJson()));
  const auto loaded = PlyPointCloudLoader::load(source);
  QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
  QCOMPARE(loaded.coordinates.globalMinimum.x, 1.); // Rendering/source XYZ stays untouched.
  QCOMPARE(formatSceneLength(2., loaded.coordinates), QString("25 cm"));
  QCOMPARE(formatSceneCoordinate(1., loaded.coordinates), QString("0.125 m"));
  QCOMPARE(loaded.coordinates.dataIdentity, identity);
  QVERIFY(loaded.coordinates.metersPerCoordinate.has_value());
  QCOMPARE(*loaded.coordinates.metersPerCoordinate, .125);
  QCOMPARE(loaded.coordinates.scaleStatus, QString("source_declared"));
  QCOMPARE(loaded.coordinates.sourceUnit, QString("mm"));
  SceneCoordinateInfo restored;
  QVERIFY(sceneCoordinateInfoFromJson(sceneCoordinateInfoToJson(loaded.coordinates), restored));
  QCOMPARE(formatSceneLength(2., restored), QString("25 cm"));
  auto persistedProvenance=loaded.coordinates.provenance;
  QVERIFY(persistedProvenance.take(QStringLiteral("native_hash_verified")).toBool());
  // Serialized coordinates retain provenance, not an authority to skip a new
  // model/sidecar hash verification on project/cache reload.
  QCOMPARE(restored.provenance,persistedProvenance);
  QVERIFY(!restored.provenance.contains(QStringLiteral("native_hash_verified")));
  const QString report = dir.filePath("coordinate-report.json");
  QString error;
  QVERIFY2(writeSceneCoordinateReport(report, source, restored, 0., "source", &error), qPrintable(error));
  const auto json = QJsonDocument::fromJson(read(report)).object();
  SceneCoordinateInfo reopened;
  QVERIFY(sceneCoordinateInfoFromJson(json["coordinates"].toObject(), reopened));
  QCOMPARE(formatSceneLength(2., reopened), QString("25 cm"));
  QCOMPARE(reopened.dataIdentity, identity);
}

void ModelExportTests::arbitraryCameraScaleDoesNotInventMetres() {
  QTemporaryDir dir;
  const QString source = dir.filePath("arbitrary.ply");
  QVERIFY(save(source, gaussianPly(0)));
  const QJsonObject sidecar{{"schema", "gsw.training_camera_contract/1"},
      {"scale", QJsonObject{{"status", "arbitrary"}, {"source_unit", "unknown"},
                           {"meters_per_unit", QJsonValue::Null}, {"canonical_unit", "arbitrary"}}},
      {"transforms", QJsonObject{{"canonical_to_training", QJsonObject{{"s", 8.}, {"applied", true},
        {"R", QJsonArray{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}}}, {"t", QJsonArray{0,0,0}}}}}},
      {"input_data_identity", QString(64, QLatin1Char('b'))},
      {"ply_sha256", QString::fromLatin1(QCryptographicHash::hash(read(source), QCryptographicHash::Sha256).toHex())}};
  QVERIFY(save(dir.filePath("arbitrary.camera-scale.json"), QJsonDocument(sidecar).toJson()));
  const auto loaded = PlyPointCloudLoader::load(source);
  QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
  QVERIFY(!loaded.coordinates.metersPerCoordinate.has_value());
  QCOMPARE(loaded.coordinates.unit, SceneLengthUnit::Unknown);
  QCOMPARE(loaded.coordinates.scaleStatus, QString("arbitrary"));
  QCOMPARE(formatSceneLength(2., loaded.coordinates), QString("2 u"));
}

void ModelExportTests::cameraScaleSidecarIsBoundToExactPlyBytes() {
  QTemporaryDir dir;
  const QString source = dir.filePath("mismatch.ply");
  QVERIFY(save(source, gaussianPly(0)));
  const QJsonObject sidecar{{"schema", "gsw.training_camera_contract/1"},
      {"scale", QJsonObject{{"status", "source_declared"}, {"source_unit", "m"}, {"meters_per_unit", 1.}, {"canonical_unit", "m"}}},
      {"transforms", QJsonObject{{"canonical_to_training", QJsonObject{{"s", 3.}, {"applied", true}}}}},
      {"input_data_identity", QString(64, QLatin1Char('c'))}, {"ply_sha256", QString(64, QLatin1Char('0'))}};
  QVERIFY(save(dir.filePath("mismatch.camera-scale.json"), QJsonDocument(sidecar).toJson()));
  const auto loaded = PlyPointCloudLoader::load(source);
  QVERIFY(!loaded.isValid());
  QVERIFY(loaded.error.contains("camera_scale_ply_hash_mismatch"));
}

void ModelExportTests::cameraScaleMetadataRefreshesAnExistingGeometryCache() {
  QTemporaryDir dir;
  const QString source = dir.filePath("cached-points.ply");
  auto bytes = pointPly(); bytes.replace("comment units cm\n", "");
  QVERIFY(save(source, bytes));
  const auto before = PlyPointCloudLoader::load(source, 1, 1);
  QVERIFY2(before.isValid(), qPrintable(before.error));
  QVERIFY(before.pointCache.isValid());
  QVERIFY(!sceneMetresPerCoordinate(before.coordinates));
  const QJsonObject sidecar{{"schema", "gsw.training_camera_contract/1"},
      {"scale", QJsonObject{{"status", "source_declared"}, {"source_unit", "m"},
                           {"meters_per_unit", 1.}, {"canonical_unit", "m"}}},
      {"transforms", QJsonObject{{"canonical_to_training", QJsonObject{{"s", 8.}, {"applied", true},
        {"R", QJsonArray{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}}}, {"t", QJsonArray{0,0,0}}}}}},
      {"input_data_identity", QString(64, QLatin1Char('d'))},
      {"ply_sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
  QVERIFY(save(dir.filePath("cached-points.camera-scale.json"), QJsonDocument(sidecar).toJson()));
  const auto after = PlyPointCloudLoader::load(source, 1, 1);
  QVERIFY2(after.isValid(), qPrintable(after.error));
  QVERIFY(after.pointCache.isValid());
  QCOMPARE(after.coordinates.globalMinimum.x, before.coordinates.globalMinimum.x);
  QCOMPARE(formatSceneLength(2., after.coordinates), QString("25 cm"));
  QVERIFY(after.pointCache.coordinates.metersPerCoordinate.has_value());
  QCOMPARE(*after.pointCache.coordinates.metersPerCoordinate, *after.coordinates.metersPerCoordinate);
}

void ModelExportTests::unknownScaleCannotBeExportedAsImplicitMetreGlb() {
  QTemporaryDir dir;
  ModelExportOptions options;
  options.sourcePath = dir.filePath("unknown.ply");
  options.destinationPath = dir.filePath("unknown.glb");
  options.format = ModelExportFormat::Glb;
  auto bytes = pointPly(); bytes.replace("comment units cm\n", "");
  QVERIFY(save(options.sourcePath, bytes));
  QVERIFY(save(options.destinationPath, "existing destination"));
  const auto result = exportModelFile(options);
  QVERIFY(!result.success);
  QCOMPARE(read(options.destinationPath), QByteArray("existing destination"));
  QCOMPARE(read(options.sourcePath), bytes);
}

void ModelExportTests::recordedCameraCoordinatesUseRealNativeExportProcess() {
  const QString python = qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_PYTHON");
  const QString root = qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_ROOT");
  if (python.isEmpty() || root.isEmpty()) QSKIP("Real native export helper runtime not selected");
  QVERIFY(QFileInfo::exists(python));
  QVERIFY(QFileInfo::exists(QDir(root).filePath("native/backend/camera_scale_export.py")));
  QTemporaryDir dir;
  const QString source = dir.filePath("actual-training.ply");
  QVERIFY(save(source, gaussianPly(0, 2.)));
  const QJsonArray identity{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}};
  const QJsonObject sidecar{{"schema", "gsw.training_camera_contract/1"},
      {"scale", QJsonObject{{"status", "source_declared"}, {"source_unit", "cm"},
                           {"meters_per_unit", .01}, {"canonical_unit", "m"},
                           {"evidence", QJsonArray{QJsonObject{{"kind", "user_declaration"}}}}}},
      {"transforms", QJsonObject{
        {"source_to_canonical", QJsonObject{{"s", .01}, {"R", identity}, {"t", QJsonArray{0,0,0}}, {"applied", true}}},
        {"canonical_to_training", QJsonObject{{"s", 4.}, {"R", identity}, {"t", QJsonArray{1,2,3}}, {"applied", true}}}}},
      {"input_data_identity", QString(64, QLatin1Char('e'))},
      {"ply_sha256", QString::fromLatin1(QCryptographicHash::hash(read(source), QCryptographicHash::Sha256).toHex())}};
  QVERIFY(save(dir.filePath("actual-training.camera-scale.json"), QJsonDocument(sidecar).toJson()));
  const QByteArray original = read(source);
  for (const QString &mode : {QStringLiteral("metric"), QStringLiteral("source")}) {
    ModelExportOptions options;
    options.sourcePath = source;
    options.destinationPath = dir.filePath(mode + ".ply");
    options.cameraCoordinateRecovery = mode;
    options.recoveryPythonPath = python;
    options.recoveryRepositoryRoot = root;
    const auto result = exportModelFile(options);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto loaded = PlyPointCloudLoader::load(options.destinationPath);
    QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
    const bool metric = mode == QStringLiteral("metric");
    QVERIFY(std::abs(loaded.coordinates.globalMinimum.x - (metric ? .25 : 25.)) < 1e-10);
    QVERIFY(std::abs(loaded.coordinates.globalMinimum.y - (metric ? -1.0625 : -106.25)) < 1e-10);
    QCOMPARE(formatSceneLength(loaded.coordinates.globalSize().z, loaded.coordinates), QString("50 cm"));
    double logScale = std::numeric_limits<double>::quiet_NaN();
    PlyGaussianVisitor visitor;
    visitor.gaussian = [&](qint64 index, const std::array<double,86> &values) {
      if (index == 0) logScale = values[7];
      return true;
    };
    QString error;
    QVERIFY2(PlyPointCloudLoader::visitSourceGaussians(options.destinationPath, visitor, error), qPrintable(error));
    QVERIFY(std::abs(logScale - (-2. + std::log(metric ? .25 : 25.))) < 1e-10);
    QVERIFY(loaded.coordinates.metersPerCoordinate.has_value());
    QCOMPARE(*loaded.coordinates.metersPerCoordinate, metric ? 1. : .01);
    QCOMPARE(loaded.coordinates.scaleStatus, QString("source_declared"));
    QCOMPARE(read(source), original);
    if (metric) {
      const QJsonObject delivery{{"schema","gsw.model_delivery/1"},{"training_status","done"},
        {"export_status","ready"},{"iteration",8},{"model_path",options.destinationPath},
        {"internal_model_path",source},{"report",result.coordinateRecovery}};
      QVERIFY(save(dir.filePath("model-delivery.json"),QJsonDocument(delivery).toJson()));
      QCOMPARE(verifiedTrainingDeliveryPath(dir.path(),8),options.destinationPath);
      QVERIFY(verifiedTrainingDeliveryPath(dir.path(),9).isEmpty());
      const auto reference=QJsonDocument::fromJson(read(cameraScaleMetadataPath(options.destinationPath)))
          .object().value("camera_reference").toObject();
      const QString camera=dir.filePath(reference.value("path").toString());
      const auto cameras=read(camera); QVERIFY(!cameras.isEmpty());
      QVERIFY(save(camera,cameras+"tampered"));
      QVERIFY(verifiedTrainingDeliveryPath(dir.path(),8).isEmpty());
      QVERIFY(save(camera,cameras));
      const QString sidecarPath=cameraScaleMetadataPath(options.destinationPath);
      const auto originalSidecar=read(sidecarPath);
      auto malformed=QJsonDocument::fromJson(originalSidecar).object();
      malformed["bundle_sha256"]=QString(64,'f');
      QVERIFY(save(sidecarPath,QJsonDocument(malformed).toJson()));
      QVERIFY(verifiedTrainingDeliveryPath(dir.path(),8).isEmpty());
      QVERIFY(!cameraScaleRecoveryCapability(options.destinationPath).metric);
      QVERIFY(save(sidecarPath,originalSidecar));
    }
  }
}

void ModelExportTests::realGpuTrainingModelMeasurementAndCoordinateExports() {
  const QString source = qEnvironmentVariable("GSW_CAMERA_SCALE_GPU_PLY");
  if (source.isEmpty()) QSKIP("Set GSW_CAMERA_SCALE_GPU_PLY to an explicitly selected, test-owned real GPU training output");
  const QString python = qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_PYTHON");
  const QString root = qEnvironmentVariable("GSW_CAMERA_SCALE_EXPORT_ROOT");
  QVERIFY(QFileInfo::exists(source));
  QVERIFY(QFileInfo::exists(python));
  QVERIFY(QFileInfo::exists(QDir(root).filePath("native/backend/camera_scale_export.py")));
  const QByteArray original = read(source);
  const QByteArray originalHash = QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex();
  const auto contract = QJsonDocument::fromJson(read(cameraScaleMetadataPath(source))).object();
  QCOMPARE(contract["schema"].toString(), QString("gsw.training_camera_contract/1"));
  QCOMPARE(contract["ply_sha256"].toString().toLatin1(), originalHash);
  const auto loaded = PlyPointCloudLoader::load(source);
  QVERIFY2(loaded.isValid(), qPrintable(loaded.error));
  QVERIFY(loaded.hasGaussianAttributes);
  const auto physical = sceneMetresPerCoordinate(loaded.coordinates);
  QVERIFY(physical.has_value());
  const auto transforms = contract["transforms"].toObject();
  const auto normalization = transforms["canonical_to_training"].toObject();
  const auto sourceToCanonical = transforms["source_to_canonical"].toObject();
  QVERIFY(normalization["s"].toDouble() > 0);
  QVERIFY(std::abs(*physical - 1. / normalization["s"].toDouble()) < 1e-12);
  QCOMPARE(loaded.coordinates.scaleStatus, contract["scale"].toObject()["status"].toString());
  QCOMPARE(loaded.coordinates.dataIdentity, contract["input_data_identity"].toString());
  SceneCoordinateInfo reopened;
  QVERIFY(sceneCoordinateInfoFromJson(sceneCoordinateInfoToJson(loaded.coordinates), reopened));
  QCOMPARE(sceneMetresPerCoordinate(reopened), physical);
  // This reader is independent of the export implementation and supports the
  // actual 2D/3D Gaussian parameter dimensions without SPZ's 3-scale contract.
  const auto firstVertex = [](const QString &path) {
    QHash<QString, double> result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return result;
    QList<QPair<QByteArray,QByteArray>> fields;
    bool vertex = false, little = true, binary = false;
    while (!file.atEnd()) {
      const auto line = file.readLine().trimmed();
      const auto tokens = line.split(' ');
      if (line.startsWith("format ")) { binary = !line.contains(" ascii "); little = !line.contains("binary_big_endian"); }
      if (line.startsWith("element ")) vertex = line.startsWith("element vertex ");
      if (vertex && tokens.size() == 3 && tokens[0] == "property") fields.append({tokens[1], tokens[2]});
      if (line == "end_header") break;
    }
    if (!binary) {
      const auto values = file.readLine().simplified().split(' ');
      if (values.size() != fields.size()) return QHash<QString,double>{};
      for (int index = 0; index < fields.size(); ++index) result.insert(QString::fromLatin1(fields[index].second), values[index].toDouble());
      return result;
    }
    QDataStream data(&file);
    data.setByteOrder(little ? QDataStream::LittleEndian : QDataStream::BigEndian);
    for (const auto &field : fields) {
      double value = 0;
      if (field.first == "float" || field.first == "float32") {
        float single = 0; data.setFloatingPointPrecision(QDataStream::SinglePrecision); data >> single; value = single;
      } else if (field.first == "double" || field.first == "float64") {
        data.setFloatingPointPrecision(QDataStream::DoublePrecision); data >> value;
      } else return QHash<QString,double>{};
      result.insert(QString::fromLatin1(field.second), value);
    }
    return data.status() == QDataStream::Ok ? result : QHash<QString,double>{};
  };
  const auto before = firstVertex(source);
  QVERIFY(before.contains("x") && before.contains("scale_0") && before.contains("scale_1"));
  QTemporaryDir directory(QFileInfo(source).dir().filePath("gsw-native-scale-export-XXXXXX"));
  QVERIFY(directory.isValid());
  directory.setAutoRemove(false); // Retain generated acceptance artifacts, never the input.
  QString error;
  const QString measurement = directory.filePath("training-measurement.json");
  QVERIFY2(writeSceneCoordinateReport(measurement, source, reopened, 0., "source", &error), qPrintable(error));
  const auto reportCoordinates = QJsonDocument::fromJson(read(measurement)).object()["coordinates"].toObject();
  SceneCoordinateInfo reportState;
  QVERIFY(sceneCoordinateInfoFromJson(reportCoordinates, reportState));
  QCOMPARE(sceneMetresPerCoordinate(reportState), physical);
  const auto inversePoint = [](std::array<double,3> p, const QJsonObject &transform) {
    const auto t = transform["t"].toArray();
    const auto r = transform["R"].toArray();
    const double scale = transform["s"].toDouble();
    std::array<double,3> value{};
    for (int column = 0; column < 3; ++column)
      for (int row = 0; row < 3; ++row)
        value[column] += r[row].toArray()[column].toDouble() * (p[row] - t[row].toDouble()) / scale;
    return value;
  };
  for (const QString &mode : {QStringLiteral("source"), QStringLiteral("metric")}) {
    ModelExportOptions options;
    options.sourcePath = source;
    options.destinationPath = directory.filePath(mode + ".ply");
    options.coordinates = loaded.coordinates;
    options.cameraCoordinateRecovery = mode;
    options.recoveryPythonPath = python;
    options.recoveryRepositoryRoot = root;
    const auto result = exportModelFile(options);
    QVERIFY2(result.success, qPrintable(result.error));
    const auto actual = firstVertex(options.destinationPath);
    auto expected = inversePoint({before["x"], before["y"], before["z"]}, normalization);
    if (mode == QStringLiteral("source")) expected = inversePoint(expected, sourceToCanonical);
    for (int axis = 0; axis < 3; ++axis) {
      const QString name = QString::fromLatin1(axis == 0 ? "x" : axis == 1 ? "y" : "z");
      QVERIFY(std::abs(actual[name] - expected[axis]) <= 1e-9 + std::abs(expected[axis]) * 1e-12);
    }
    double multiplier = 1. / normalization["s"].toDouble();
    if (mode == QStringLiteral("source")) multiplier /= sourceToCanonical["s"].toDouble();
    int dimensions = 0;
    for (auto field = before.cbegin(); field != before.cend(); ++field) {
      if (field.key().startsWith("scale_")) {
        ++dimensions;
        QVERIFY(actual.contains(field.key()));
        QVERIFY(std::abs(actual[field.key()] - (field.value() + std::log(multiplier))) < 2e-6);
      } else if (field.key() == "opacity" || field.key().startsWith("f_") || field.key().startsWith("rot_")) {
        QCOMPARE(actual[field.key()], field.value());
      }
    }
    QVERIFY(dimensions == 2 || dimensions == 3);
    QCOMPARE(actual.contains("scale_2"), before.contains("scale_2"));
    const auto exported = PlyPointCloudLoader::load(options.destinationPath);
    QVERIFY2(exported.isValid(), qPrintable(exported.error));
    QCOMPARE(exported.sourceVertexCount, loaded.sourceVertexCount);
    const auto outputRatio = sceneMetresPerCoordinate(exported.coordinates);
    QVERIFY(outputRatio.has_value());
    const double expectedRatio = mode == QStringLiteral("metric") ? 1. : contract["scale"].toObject()["meters_per_unit"].toDouble();
    QVERIFY(std::abs(*outputRatio - expectedRatio) < 1e-12);
    QVERIFY(std::abs(exported.coordinates.globalSize().z * *outputRatio - loaded.coordinates.globalSize().z * *physical) < 2e-6);
    const auto outputMetadata = QJsonDocument::fromJson(read(cameraScaleMetadataPath(options.destinationPath))).object();
    QCOMPARE(outputMetadata["schema"].toString(), QString("gsw.coordinate_export/1"));
    QCOMPARE(outputMetadata["ply_sha256"].toString().toLatin1(), QCryptographicHash::hash(read(options.destinationPath), QCryptographicHash::Sha256).toHex());
    QCOMPARE(result.coordinateRecovery["output_sha256"], outputMetadata["output_sha256"]);
    QCOMPARE(read(source), original);
  }
  qInfo() << "Actual GPU-trained Gaussian PLY native measurement/export accepted:" << source
          << "vertices" << loaded.sourceVertexCount << "generated evidence" << directory.path();
}

QTEST_GUILESS_MAIN(ModelExportTests)
#include "ModelExportTests.moc"
