#include "SceneCoordinates.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cmath>

namespace {
QJsonObject similarity(double scale, const QJsonArray &rotation, const QJsonArray &translation) {
  return {{"s",scale},{"R",rotation},{"t",translation},{"applied",true}};
}
const QJsonArray identity{QJsonArray{1.,0.,0.},QJsonArray{0.,1.,0.},QJsonArray{0.,0.,1.}};
bool write(const QString &path,const QByteArray &bytes) { QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size(); }
}
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);bool passed=true;int count=0;
  const auto check=[&](bool value,const char *message){++count;std::fprintf(stderr,"Display mapping %s: %s\n",value?"PASS":"FAIL",message);passed&=value;return value;};
  QTemporaryDir fixture;if(!check(fixture.isValid(),"owned portable fixture root"))return 2;
  const auto make=[&](const QString &name,const QJsonObject &contract,const QString &frame,const QString &unit,
                      const gsw::SceneCoordinate3D &origin){
    const auto path=fixture.filePath(name+".ply");
    const QByteArray ply="ply\nformat ascii 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\nend_header\n0 0 0\n";
    write(path,ply);auto record=contract;
    if(frame!="training")record={{"schema","gsw.coordinate_export/1"},{"export_version",2},{"output_frame",frame},{"current_frame",frame},
        {"output_unit",unit},{"current_unit",unit},{"transform_applied",true},{"applied_transform",similarity(1.,identity,{0.,0.,0.})},
        {"training_contract",contract},{"input_data_identity",contract.value("input_data_identity")}};
    record.insert("ply_sha256",QString::fromLatin1(QCryptographicHash::hash(ply,QCryptographicHash::Sha256).toHex()));
    write(fixture.filePath(name+".camera-scale.json"),QJsonDocument(record).toJson());
    const auto metadata=gsw::parsePlyCoordinateMetadata({},path,true);
    check(metadata.validationError.isEmpty(),"valid model hash and recorded coordinate contract are admitted");
    gsw::SceneCoordinateTracker tracker(metadata);QVector3D local;tracker.observeAndMap(origin.x,origin.y,origin.z,local);
    return std::make_pair(tracker.info(),path);
  };
  QJsonObject contract{{"schema","gsw.training_camera_contract/1"},{"input_data_identity",QString(64,'a')},
      {"scale",QJsonObject{{"status","source_declared"},{"source_unit","m"},{"canonical_unit","m"},{"meters_per_unit",1.}}},
      {"transforms",QJsonObject{{"source_to_canonical",similarity(1.,identity,{0.,0.,0.})},
          {"canonical_to_training",similarity(2.,QJsonArray{QJsonArray{0.,-1.,0.},QJsonArray{1.,0.,0.},QJsonArray{0.,0.,1.}},{10.,20.,30.})}}}};
  auto training=make("training",contract,"training","training",{10.,20.,30.});
  auto metric=make("metric",contract,"canonical","m",{0.,0.,0.});
  auto mapping=gsw::sceneDisplayMapping(training.first,metric.first);
  check(mapping.verifiedAlignment&&(mapping.localToReference.map(QVector3D(10.,22.,30.))-QVector3D(1.,0.,0.)).length()<1e-6,
      "training coordinates are inverse-rotated, translated and scaled into the common canonical frame");
  check(std::abs(mapping.lengthScale-.5)<1e-12,"metric restoration retains physical size rather than applying an extra visual shrink");
  const auto exactRaw=gsw::sceneDisplayMapping(metric.first,metric.first).mapCoordinate({1.,2.,98.469025});
  check(std::abs(exactRaw.z-98.469025)<1e-12,"numerical coordinate reports never round a precise base elevation through display floats");
  auto otherContract=contract;auto transforms=otherContract.value("transforms").toObject();
  transforms.insert("canonical_to_training",similarity(4.,identity,{-8.,12.,16.}));otherContract.insert("transforms",transforms);
  auto otherTraining=make("other-training",otherContract,"training","training",{-8.,12.,16.});
  mapping=gsw::sceneDisplayMapping(otherTraining.first,training.first);
  check(mapping.verifiedAlignment&&(mapping.localToReference.map(QVector3D(-4.,12.,16.))-QVector3D(10.,22.,30.)).length()<1e-6,
      "different numerical normalizations of the same canonical data compose correctly");
  auto millimetres=make("millimetres",contract,"canonical","mm",{0.,0.,0.});
  mapping=gsw::sceneDisplayMapping(millimetres.first,metric.first);
  check(mapping.verifiedAlignment&&(mapping.localToReference.map(QVector3D(1000.,0.,0.))-QVector3D(1.,0.,0.)).length()<1e-6,
      "explicit millimetre output aligns with metre output without double unit conversion");
  auto bigContract=contract;transforms=bigContract.value("transforms").toObject();
  transforms.insert("canonical_to_training",similarity(1.,identity,{-1e12,-2e12,-3e12}));bigContract.insert("transforms",transforms);
  auto bigMetric=make("big-metric",bigContract,"canonical","m",{1e12,2e12,3e12});
  auto bigTraining=make("big-training",bigContract,"training","training",{0.,0.,0.});
  mapping=gsw::sceneDisplayMapping(bigTraining.first,bigMetric.first);
  check(mapping.verifiedAlignment&&(mapping.localToReference.map(QVector3D(.125F,.25F,.5F))-QVector3D(.125F,.25F,.5F)).length()<1e-6,
      "large origins cancel in double before the local float display matrix is formed");
  auto foreignContract=contract;foreignContract.insert("input_data_identity",QString(64,'b'));
  auto foreign=make("foreign",foreignContract,"canonical","m",{0.,0.,0.});
  mapping=gsw::sceneDisplayMapping(training.first,foreign.first);
  check(!mapping.verifiedAlignment&&!mapping.reason.isEmpty(),"different data identities never imply registered rotation or origin");
  auto changedCanonical=contract;transforms=changedCanonical.value("transforms").toObject();
  transforms.insert("source_to_canonical",similarity(1.,identity,{1.,0.,0.}));changedCanonical.insert("transforms",transforms);
  auto incompatible=make("different-lineage",changedCanonical,"canonical","m",{0.,0.,0.});
  check(!gsw::sceneDisplayMapping(training.first,incompatible.first).verifiedAlignment,
      "matching data IDs alone cannot authorize a conflicting canonical origin");
  auto mmContract=contract;mmContract.insert("scale",QJsonObject{{"status","source_declared"},{"source_unit","mm"},{"canonical_unit","m"},{"meters_per_unit",.001}});
  transforms=mmContract.value("transforms").toObject();
  transforms.insert("source_to_canonical",similarity(.001,QJsonArray{QJsonArray{0.,-1.,0.},QJsonArray{1.,0.,0.},QJsonArray{0.,0.,1.}},{100.,-200.,300.}));
  mmContract.insert("transforms",transforms);
  auto mmSource=make("source-mm",mmContract,"source","mm",{0.,0.,0.});
  auto mmMetric=make("source-mm-metric",mmContract,"canonical","m",{100.,-200.,300.});
  mapping=gsw::sceneDisplayMapping(mmSource.first,mmMetric.first);
  check(mapping.verifiedAlignment&&(mapping.localToReference.map(QVector3D(1000.,0.,0.))-QVector3D(100.,-199.,300.)).length()<1e-5,
      "source millimetres apply their declared rotation, origin and unit conversion exactly once");
  auto centimetres=make("centimetres",mmContract,"canonical","cm",{0.,0.,0.});
  mapping=gsw::sceneDisplayMapping(centimetres.first,mmMetric.first);
  check(mapping.verifiedAlignment&&(mapping.localToReference.map(QVector3D(100.,0.,0.))-QVector3D(1.,0.,0.)).length()<1e-6,
      "centimetre output is a current-frame unit choice, not a second source conversion");
  gsw::SceneCoordinateInfo cached;
  check(gsw::sceneCoordinateInfoFromJson(gsw::sceneCoordinateInfoToJson(training.first),cached)&&
      !gsw::sceneDisplayMapping(cached,metric.first).verifiedAlignment,
      "a serialized cache cannot impersonate a fresh model checksum observation");
  auto arbitrary=contract;arbitrary.insert("scale",QJsonObject{{"status","arbitrary"},{"source_unit","unknown"},{"canonical_unit","unknown"}});
  auto arbitraryTraining=make("arbitrary-training",arbitrary,"training","training",{0.,0.,0.});
  auto arbitraryCanonical=make("arbitrary-canonical",arbitrary,"canonical","unknown",{0.,0.,0.});
  check(gsw::sceneDisplayMapping(arbitraryTraining.first,arbitraryCanonical.first).verifiedAlignment&&
      !gsw::sceneMetresPerCoordinate(arbitraryTraining.first)&&!gsw::sceneMetresPerCoordinate(arbitraryCanonical.first),
      "a verified relative frame chain never promotes arbitrary length units to metres");
  write(training.second,QByteArray("changed model bytes"));
  const auto bad=gsw::parsePlyCoordinateMetadata({},training.second,true);
  check(!bad.validationError.isEmpty()&&bad.provenance.isEmpty(),"a model hash mismatch cannot enter verified alignment provenance");
  std::fprintf(stderr,"Display mapping checks: %d; result %s\n",count,passed?"PASS":"FAIL");return passed?0:1;
}
