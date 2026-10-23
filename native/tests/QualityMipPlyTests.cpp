#include "QualityMipModelContract.h"
#include "PlyPointCloudLoader.h"
#include "ModelExport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

namespace {
bool write(const QString &path,const QByteArray &bytes) {
  QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
}
QByteArray read(const QString &path) {
  QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();
}
QString hash(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
QByteArray row(int index) {
  return QByteArray::number(index)+" 0 -2 .1 .2 .3 0 -3 -3 -3 1 0 0 0\n";
}
QByteArray ply(const QByteArray &marker={}) {
  QByteArray bytes="ply\nformat ascii 1.0\n";
  if(!marker.isEmpty())bytes+="comment GSW_QUALITY_REQUIRES_SIDECAR "+marker+'\n';
  bytes+="element vertex 3\n";
  for(const char *name:{"x","y","z","f_dc_0","f_dc_1","f_dc_2","opacity","scale_0","scale_1","scale_2","rot_0","rot_1","rot_2","rot_3"})
    bytes+="property float "+QByteArray(name)+'\n';
  return bytes+"end_header\n"+row(0)+row(1)+row(2);
}
bool contract(const QString &path,const QByteArray &bytes,const QString &representation="fused") {
  const QFileInfo info(path);
  QJsonObject metadata{{"schema","gsw.quality_model/1"},{"representation",representation},{"filter","full_mip"},
      {"filter_3d_applied",representation=="fused"},
      {"filter_2d",QJsonObject{{"variance",.3},{"opacity_compensation",true}}},
      {"raw_source_sha256",representation=="raw"?hash(bytes):QString(64,'a')},
      {"fused_sha256",representation=="fused"?hash(bytes):QString(64,'c')},{"plan_hash",QString(64,'b')}};
  return write(info.absoluteDir().filePath(info.completeBaseName()+".quality.json"),QJsonDocument(metadata).toJson());
}
}

class QualityMipPlyTests final:public QObject {
  Q_OBJECT
private slots:
  void actualLoaderRejectsMissingRawOrChangedContractAndKeepsLegacy() {
    const QString base=qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT","D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));QTemporaryDir temporary(QDir(base).filePath("loader-XXXXXX"));QVERIFY(temporary.isValid());
    const QString path=temporary.filePath("model.ply");
    QVERIFY(write(path,ply()));
    const auto legacy=gsw::PlyPointCloudLoader::load(path);QVERIFY2(legacy.isValid(),qPrintable(legacy.error));QVERIFY(!legacy.qualityMip.enabled);
    const QByteArray fused=ply("full_mip_fused_v1");QVERIFY(write(path,fused));
    QCOMPARE(gsw::PlyPointCloudLoader::load(path).error,QStringLiteral("quality_mip_sidecar_required"));
    QVERIFY(contract(path,fused));
    const auto loaded=gsw::PlyPointCloudLoader::load(path);QVERIFY2(loaded.isValid(),qPrintable(loaded.error));QVERIFY(loaded.qualityMip.enabled);
    QCOMPARE(loaded.sourceVertexCount,qint64(3));QVERIFY(!loaded.hasSurfelAttributes);
    QVERIFY(write(path,fused+"changed\n"));
    QCOMPARE(gsw::PlyPointCloudLoader::load(path).error,QStringLiteral("quality_mip_model_hash_mismatch"));
    const QByteArray raw=ply("full_mip_raw_v1");QVERIFY(write(path,raw));QVERIFY(contract(path,raw,"raw"));
    QCOMPARE(gsw::PlyPointCloudLoader::load(path).error,QStringLiteral("quality_mip_raw_requires_fusion"));
  }

  void bothPlyWritersPublishBoundFreshPairAndPreserveRetainedRows() {
    const QString base=qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT","D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));QTemporaryDir temporary(QDir(base).filePath("writers-XXXXXX"));
    const QString path=temporary.filePath("source.ply");const QByteArray bytes=ply("full_mip_fused_v1");
    QVERIFY(write(path,bytes));QVERIFY(contract(path,bytes));
    QBitArray deleted(3);deleted.setBit(1);QString error;
    const QString cropped=temporary.filePath("cropped.ply");
    QVERIFY2(gsw::PlyPointCloudLoader::writeFiltered(path,cropped,deleted,&error),qPrintable(error));
    const auto loaded=gsw::PlyPointCloudLoader::load(cropped);QVERIFY2(loaded.isValid(),qPrintable(loaded.error));
    QVERIFY(loaded.qualityMip.enabled);QCOMPARE(loaded.sourceVertexCount,qint64(2));
    QVERIFY(read(cropped).endsWith(row(0)+row(2)));
    QCOMPARE(read(cropped).count("GSW_QUALITY_REQUIRES_SIDECAR"),1);
    gsw::ModelExportOptions options;options.sourcePath=path;options.destinationPath=temporary.filePath("copied.ply");
    options.format=gsw::ModelExportFormat::Ply;options.applyTransform=false;
    auto result=gsw::PlyPointCloudLoader::exportSourcePly(options);QVERIFY2(result.success,qPrintable(result.error));
    const auto copied=gsw::PlyPointCloudLoader::load(options.destinationPath);QVERIFY2(copied.isValid(),qPrintable(copied.error));QVERIFY(copied.qualityMip.enabled);
    const QByteArray before=read(options.destinationPath);
    result=gsw::PlyPointCloudLoader::exportSourcePly(options);QVERIFY(!result.success);
    QCOMPARE(result.error,QStringLiteral("quality_mip_export_requires_fresh_destination"));
    QCOMPARE(read(options.destinationPath),before);QCOMPARE(read(path),bytes);
    const QString blocked=temporary.filePath("blocked.ply");QVERIFY(write(temporary.filePath("blocked.quality.json"),"foreign"));
    QVERIFY(!gsw::PlyPointCloudLoader::writeFiltered(path,blocked,deleted,&error));
    QCOMPARE(error,QStringLiteral("quality_mip_export_requires_fresh_destination"));QVERIFY(!QFileInfo::exists(blocked));
    QCOMPARE(read(temporary.filePath("blocked.quality.json")),QByteArray("foreign"));
  }

  void unsupportedMipFormatsCannotSilentlyLoseTheFilteringContract() {
    const QString base=qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT","D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));QTemporaryDir temporary(QDir(base).filePath("format-XXXXXX"));
    const QString path=temporary.filePath("source.ply");
    gsw::PlyGaussianVisitor visitor;gsw::PlyGeometryVisitor geometry;QString error;
    QVERIFY(write(path,ply()));
    QVERIFY(gsw::PlyPointCloudLoader::visitSourceGaussians(path,visitor,error));
    QVERIFY(gsw::PlyPointCloudLoader::visitSourceGeometry(path,geometry,error));
    const QByteArray bytes=ply("full_mip_fused_v1");
    QVERIFY(write(path,bytes));QVERIFY(contract(path,bytes));
    QVERIFY(!gsw::PlyPointCloudLoader::visitSourceGaussians(path,visitor,error));
    QCOMPARE(error,QStringLiteral("quality_mip_export_format_unsupported"));
    QVERIFY(!gsw::PlyPointCloudLoader::visitSourceGeometry(path,geometry,error));
    QCOMPARE(error,QStringLiteral("quality_mip_export_format_unsupported"));
  }

  void fusedMipSimilarityBakeRebindsContractWithoutRefilteringOrChangingSource() {
    const QString base=qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT","D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));QTemporaryDir temporary(QDir(base).filePath("similarity-XXXXXX"));
    QVERIFY(temporary.isValid());
    const QString source=temporary.filePath("source.ply");
    QByteArray original=ply("full_mip_fused_v1");
    // Reuse the owned ASCII fixture, but make all three principal radii
    // different so the Gaussian rotation is observable, not just an isotropic
    // position-only transform. These are already fused log-scales.
    original.replace("0 -3 -3 -3 1 0 0 0\n","0 -2 -3 -4 1 0 0 0\n");
    QVERIFY(write(source,original));QVERIFY(contract(source,original));
    const QString sourceSidecar=temporary.filePath("source.quality.json");
    const QByteArray originalSidecar=read(sourceSidecar);
    const auto input=gsw::PlyPointCloudLoader::load(source);
    QVERIFY2(input.isValid(),qPrintable(input.error));QVERIFY(input.qualityMip.enabled);
    QCOMPARE(input.sourceVertexCount,qint64(3));QCOMPARE(input.vertices.size(),3);

    gsw::ModelExportOptions options;options.sourcePath=source;
    options.destinationPath=temporary.filePath("baked.ply");
    options.format=gsw::ModelExportFormat::Ply;options.applyTransform=true;
    options.pivot={.25,-.5,1.};options.transform.scale={2,2,2};
    const float halfRoot=static_cast<float>(std::sqrt(.5));
    options.transform.rotation=QQuaternion(halfRoot,0,0,halfRoot); // proper Rz90
    options.transform.translation={10,-5,7};
    const auto result=gsw::PlyPointCloudLoader::exportSourcePly(options);
    QVERIFY2(result.success,qPrintable(result.error));
    const QByteArray bakedBytes=read(options.destinationPath);
    const QByteArray bakedSidecar=read(temporary.filePath("baked.quality.json"));
    QVERIFY(!bakedBytes.isEmpty());QVERIFY(bakedBytes!=original);
    QVERIFY(!bakedSidecar.isEmpty());
    QCOMPARE(bakedBytes.count("GSW_QUALITY_REQUIRES_SIDECAR"),1);
    QVERIFY(bakedBytes.indexOf("GSW_QUALITY_REQUIRES_SIDECAR")<bakedBytes.indexOf("end_header\n"));
    const auto output=gsw::PlyPointCloudLoader::load(options.destinationPath);
    QVERIFY2(output.isValid(),qPrintable(output.error));QVERIFY(output.qualityMip.enabled);
    QVERIFY(output.hasGaussianAttributes);QVERIFY(!output.hasSurfelAttributes);
    QCOMPARE(output.sourceVertexCount,qint64(3));QCOMPARE(output.vertices.size(),3);

    const auto rows=bakedBytes.mid(bakedBytes.indexOf("end_header\n")+11).trimmed().split('\n');
    QCOMPARE(rows.size(),3);
    for(int index=0;index<3;++index) {
      const auto values=rows[index].simplified().split(' ');
      QCOMPARE(values.size(),14);
      // Independently worked: pivot + t + Rz90*2*(p-pivot), for p=(i,0,-2).
      const std::array<double,3> expectedPosition{9.25,2.*index-6.,2.};
      for(int axis=0;axis<3;++axis) {
        QVERIFY(std::abs(values[axis].toDouble()-expectedPosition[axis])<1e-12);
        QVERIFY(std::abs(values[7+axis].toDouble()-(-2.-axis+std::log(2.)))<1e-12);
      }
      QVERIFY(std::abs(values[10].toDouble()-std::sqrt(.5))<1e-12);
      QCOMPARE(values[11].toDouble(),0.);QCOMPARE(values[12].toDouble(),0.);
      QVERIFY(std::abs(values[13].toDouble()-std::sqrt(.5))<1e-12);
      // A second 3D filter/opacity compensation would change these values.
      QCOMPARE(values[3],QByteArray(".1"));QCOMPARE(values[4],QByteArray(".2"));
      QCOMPARE(values[5],QByteArray(".3"));QCOMPARE(values[6],QByteArray("0"));
      const auto &vertex=output.vertices[index];
      QCOMPARE(vertex.sourceIndex,quint32(index));
      const auto position=output.coordinates.globalFromLocal({vertex.x,vertex.y,vertex.z});
      QVERIFY(std::abs(position.x-expectedPosition[0])<1e-6);
      QVERIFY(std::abs(position.y-expectedPosition[1])<1e-6);
      QVERIFY(std::abs(position.z-expectedPosition[2])<1e-6);
      QVERIFY(std::abs(vertex.scaleX-2.*std::exp(-2.))<1e-7);
      QVERIFY(std::abs(vertex.scaleY-2.*std::exp(-3.))<1e-7);
      QVERIFY(std::abs(vertex.scaleZ-2.*std::exp(-4.))<1e-7);
      QVERIFY(std::abs(vertex.rotationW-std::sqrt(.5))<1e-6);
      QCOMPARE(vertex.rotationX,0.F);QCOMPARE(vertex.rotationY,0.F);
      QVERIFY(std::abs(vertex.rotationZ-std::sqrt(.5))<1e-6);
      QCOMPARE(vertex.opacity,.5F);
    }
    const auto metadata=output.qualityMip.metadata;
    QCOMPARE(metadata.value("schema").toString(),QStringLiteral("gsw.quality_model/1"));
    QCOMPARE(metadata.value("representation").toString(),QStringLiteral("fused"));
    QCOMPARE(metadata.value("filter").toString(),QStringLiteral("full_mip"));
    QVERIFY(metadata.value("filter_3d_applied").toBool());
    QCOMPARE(metadata.value("fused_sha256").toString(),hash(bakedBytes));
    QCOMPARE(metadata.value("fused_file").toString(),QStringLiteral("baked.ply"));
    QCOMPARE(metadata.value("derived_from_fused_sha256").toString(),hash(original));
    QCOMPARE(metadata.value("raw_source_sha256"),input.qualityMip.metadata.value("raw_source_sha256"));
    QCOMPARE(output.qualityMip.planHash,input.qualityMip.planHash);
    QCOMPARE(metadata.value("filter_2d"),input.qualityMip.metadata.value("filter_2d"));
    QCOMPARE(metadata.value("filter_2d").toObject().value("variance").toDouble(),.3);
    QVERIFY(metadata.value("filter_2d").toObject().value("opacity_compensation").toBool());
    QCOMPARE(output.qualityMip.pixelVariance,.3F);QVERIFY(output.qualityMip.opacityCompensation);
    QVERIFY(!metadata.contains("filter_widths"));

    // A source-coordinate re-export of the derived fused artifact must not
    // bake the TRS or 3D filter again, and must remain a valid fresh pair.
    auto repeated=options;repeated.sourcePath=options.destinationPath;
    repeated.destinationPath=temporary.filePath("repeated.ply");repeated.applyTransform=false;
    const auto repeatedResult=gsw::PlyPointCloudLoader::exportSourcePly(repeated);
    QVERIFY2(repeatedResult.success,qPrintable(repeatedResult.error));
    QCOMPARE(read(repeated.destinationPath),bakedBytes);
    const auto reloaded=gsw::PlyPointCloudLoader::load(repeated.destinationPath);
    QVERIFY2(reloaded.isValid(),qPrintable(reloaded.error));QVERIFY(reloaded.qualityMip.enabled);
    QCOMPARE(reloaded.sourceVertexCount,qint64(3));
    QCOMPARE(reloaded.qualityMip.metadata.value("fused_sha256").toString(),hash(bakedBytes));
    QCOMPARE(reloaded.qualityMip.metadata.value("fused_file").toString(),QStringLiteral("repeated.ply"));
    QCOMPARE(reloaded.qualityMip.metadata.value("derived_from_fused_sha256").toString(),hash(bakedBytes));
    QCOMPARE(reloaded.qualityMip.metadata.value("filter_2d"),metadata.value("filter_2d"));
    QCOMPARE(read(options.destinationPath),bakedBytes);
    QCOMPARE(read(temporary.filePath("baked.quality.json")),bakedSidecar);
    QCOMPARE(read(source),original);QCOMPARE(read(sourceSidecar),originalSidecar);
  }

  void actualTrainingArtifactLoadsAndReexportsWithoutSourceMutation() {
    const QString source=qEnvironmentVariable("GSW_QUALITY_MIP_REAL_FUSED_PLY");
    if(source.isEmpty()) QSKIP("real trainer artifact not_run: set GSW_QUALITY_MIP_REAL_FUSED_PLY");
    const QByteArray before=read(source);QVERIFY(!before.isEmpty());
    const auto loaded=gsw::PlyPointCloudLoader::load(source);
    QVERIFY2(loaded.isValid(),qPrintable(loaded.error));QVERIFY(loaded.qualityMip.enabled);
    QVERIFY(loaded.sourceVertexCount>0);QVERIFY(!loaded.hasSurfelAttributes);
    const QString base=qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT","D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));QTemporaryDir temporary(QDir(base).filePath("real-trainer-XXXXXX"));QVERIFY(temporary.isValid());
    gsw::ModelExportOptions options;options.sourcePath=source;options.destinationPath=temporary.filePath("delivered.ply");
    options.format=gsw::ModelExportFormat::Ply;options.applyTransform=false;
    const auto result=gsw::PlyPointCloudLoader::exportSourcePly(options);
    QVERIFY2(result.success,qPrintable(result.error));
    const auto delivered=gsw::PlyPointCloudLoader::load(options.destinationPath);
    QVERIFY2(delivered.isValid(),qPrintable(delivered.error));QVERIFY(delivered.qualityMip.enabled);
    QCOMPARE(delivered.sourceVertexCount,loaded.sourceVertexCount);
    QCOMPARE(delivered.qualityMip.planHash,loaded.qualityMip.planHash);
    QCOMPARE(read(source),before);
    qInfo().noquote()<<"Actual trainer fused source:"<<source<<"vertices:"<<loaded.sourceVertexCount;
  }
};
QTEST_GUILESS_MAIN(QualityMipPlyTests)
#include "QualityMipPlyTests.moc"
