#include "SceneCoordinates.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
bool write(const QString &path,const QByteArray &bytes){QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size();}
QByteArray read(const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
QString sidecar(const QString &path){QFileInfo info(path);return info.dir().filePath(info.completeBaseName()+QStringLiteral(".camera-scale.json"));}
}
int main(int argc,char **argv){
  QCoreApplication app(argc,argv);bool passed=true;
  const auto check=[&](bool ok,const char *message){std::fprintf(stderr,"Model scale coordinates %s: %s\n",ok?"PASS":"FAIL",message);std::fflush(stderr);passed=passed&&ok;return ok;};
  if(!check(argc==3,"repository and CPU Python arguments are explicit"))return 2;
  QTemporaryDir fixture;if(!check(fixture.isValid(),"owned fixture directory exists"))return 2;
  const QString input=fixture.filePath(QStringLiteral("input.ply"));
  const QByteArray original="ply\nformat ascii 1.0\ncomment units mm\ncomment crs EPSG:32654\nelement vertex 4\nproperty double x\nproperty double y\nproperty double z\nend_header\n0 0 0\n2 0 0\n0 1 0\n0 0 1\n";
  if(!check(write(input,original),"write a generated model with an explicit input-unit comment"))return 2;
  QProcess helper;
#ifdef Q_OS_WIN
  helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;args->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;args->startupInfo->wShowWindow=SW_HIDE;});
#endif
  const auto script=QString::fromUtf8(
      "import json,sys;sys.path.insert(0,sys.argv[1]);"
      "from native.backend.model_scale_version import inspect_model_scale_input,create_scaled_model_version;"
      "e=inspect_model_scale_input(sys.argv[2]);"
      "r={'schema':'gsw.scale_constraints/1','input_data_identity':e['data_identity'],'input_frame':'source','mode':'scale_only','fit_tolerance_m':1e-8,"
      "'constraints':[{'id':'fit','kind':'distance','role':'fit','reference_id':'ruler-a','reference_unit':'m','source_a':[0,0,0],'source_b':[2,0,0],'reference_distance':6,"
      "'provenance':{'source':'generated-fit','measurement_id':'ruler-a'}},{'id':'check','kind':'distance','role':'check','reference_id':'ruler-b','reference_unit':'m',"
      "'source_a':[0,0,0],'source_b':[0,1,0],'reference_distance':3,'independent_of_fit':True,'tolerance_m':1e-8,'provenance':{'source':'generated-holdout','measurement_id':'ruler-b'}}]};"
      "o=create_scaled_model_version(sys.argv[2],r,sys.argv[3],e['data_identity']);"
      "cp=[{'id':'cp-'+str(i),'kind':'control','role':'fit','reference_id':'survey-'+str(i),'reference_unit':'m','source_xyz':p,'reference_xyz':[10-2*p[1],20+2*p[0],30+2*p[2]]} for i,p in enumerate(([0,0,0],[1,0,0],[0,1,0],[0,0,1]))];"
      "rr=dict(r,mode='sim3',constraints=cp);rot=create_scaled_model_version(sys.argv[2],rr,sys.argv[3]+'-rotated',e['data_identity']);"
      "print(json.dumps({'modelPath':o['modelPath'],'rotatedModelPath':rot['modelPath']}))");
  helper.setProgram(QString::fromLocal8Bit(argv[2]));helper.setArguments({QStringLiteral("-B"),QStringLiteral("-c"),script,QString::fromLocal8Bit(argv[1]),input,fixture.filePath(QStringLiteral("version"))});
  auto environment=QProcessEnvironment::systemEnvironment();environment.insert(QStringLiteral("PYTHONUTF8"),QStringLiteral("1"));environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"),QStringLiteral("1"));environment.insert(QStringLiteral("TEMP"),fixture.path());environment.insert(QStringLiteral("TMP"),fixture.path());helper.setProcessEnvironment(environment);helper.start();
  if(!check(helper.waitForFinished(30000)&&helper.exitCode()==0,"actual shared Python backend produces a scale version without GUI/GPU")){std::fprintf(stderr,"%s\n",helper.readAllStandardError().constData());return 2;}
  const auto helperOutput=QJsonDocument::fromJson(helper.readAllStandardOutput()).object();
  const QString output=helperOutput.value(QStringLiteral("modelPath")).toString();
  const QByteArray metadataBytes=read(sidecar(output));const auto record=QJsonDocument::fromJson(metadataBytes).object();
  const auto metadata=gsw::parsePlyCoordinateMetadata({QStringLiteral("units m")},output,true);
  check(metadata.validationError.isEmpty(),"actual gsw.model_scale_export sidecar is admitted by the public native metadata reader");
  check(metadata.metersPerCoordinate&&*metadata.metersPerCoordinate==1.&&metadata.unitDeclared&&metadata.unit==gsw::SceneLengthUnit::Metres,
      "baked metric coordinates use one metre per output coordinate, not the input three-metre scale factor");
  check(metadata.scaleStatus==QStringLiteral("independently_checked")&&!metadata.dataIdentity.isEmpty(),"declared independent reference-check scope and source identity survive native loading");
  check(metadata.provenance.value(QStringLiteral("verification_level"))==QStringLiteral("native_structural_and_ply_hash")&&
      metadata.provenance.value(QStringLiteral("model_geometry_accuracy"))==QStringLiteral("not_measured"),"native structural verification never masquerades as solver replay or model accuracy");
  const auto crs=gsw::parsePlyCoordinateMetadata({QStringLiteral("units m"),QStringLiteral("crs EPSG:32654")},output,true);
  check(crs.coordinateReferenceSystem.isEmpty()&&crs.provenance.value(QStringLiteral("ignored_output_crs"))==QStringLiteral("EPSG:32654"),"a source CRS copied into an output header cannot declare the fitted local reference frame");
  check(crs.provenance.value(QStringLiteral("input_declared_crs"))==QStringLiteral("EPSG:32654"),"the actual backend retains original CRS only as input provenance");
  const QString rotatedOutput=helperOutput.value(QStringLiteral("rotatedModelPath")).toString();
  const auto rotatedMetadata=gsw::parsePlyCoordinateMetadata({},rotatedOutput,true);
  check(rotatedMetadata.validationError.isEmpty()&&rotatedMetadata.metersPerCoordinate&&*rotatedMetadata.metersPerCoordinate==1.,"actual nonidentity full-Sim3 output is admitted without reapplying its scale");
  gsw::SceneCoordinateTracker tracker(metadata);QVector3D local;check(tracker.observeAndMap(6.,3.,0.,local),"metric coordinates enter the real native measurement tracker");
  const auto info=tracker.info();check(gsw::sceneMetresPerCoordinate(info)&&*gsw::sceneMetresPerCoordinate(info)==1.,"measurement uses the actual output frame scale");
  gsw::SceneCoordinateInfo reopened;check(gsw::sceneCoordinateInfoFromJson(gsw::sceneCoordinateInfoToJson(info),reopened)&&reopened.provenance==info.provenance,"save/reopen retains complete scale and independent-check provenance");
  const auto rejected=[&](QJsonObject changed,const char *message){write(sidecar(output),QJsonDocument(changed).toJson());check(!gsw::parsePlyCoordinateMetadata({},output,true).validationError.isEmpty(),message);write(sidecar(output),metadataBytes);};
  {auto changed=record;changed.insert(QStringLiteral("ply_sha256"),QString(64,QLatin1Char('0')));rejected(changed,"changed PLY hash is rejected");}
  {auto changed=record;changed.insert(QStringLiteral("coordinate_frame"),QStringLiteral("source"));rejected(changed,"nonmetric output frame cannot silently receive metric units");}
  {auto changed=record;auto transform=changed.value(QStringLiteral("applied_transform")).toObject();transform.insert(QStringLiteral("s"),-3.);changed.insert(QStringLiteral("applied_transform"),transform);rejected(changed,"negative or inconsistent transform is rejected");}
  {auto changed=record;changed.insert(QStringLiteral("solution_sha256"),QString(64,QLatin1Char('0')));rejected(changed,"solution provenance digest disagreement is rejected");}
  {auto changed=record;auto scale=changed.value(QStringLiteral("scale")).toObject();scale.insert(QStringLiteral("status"),QStringLiteral("arbitrary"));changed.insert(QStringLiteral("scale"),scale);rejected(changed,"an arbitrary scale cannot claim baked metric provenance");}
  {auto changed=record;auto transform=changed.value(QStringLiteral("applied_transform")).toObject();auto inverse=transform.value(QStringLiteral("inverse")).toObject();inverse.insert(QStringLiteral("t"),QJsonArray{1.,2.,3.});transform.insert(QStringLiteral("inverse"),inverse);changed.insert(QStringLiteral("applied_transform"),transform);rejected(changed,"a conflicting inverse is rejected instead of being recomputed silently");}
  {auto changed=record;auto scale=changed.value(QStringLiteral("scale")).toObject();scale.insert(QStringLiteral("meters_per_unit"),.003);changed.insert(QStringLiteral("scale"),scale);rejected(changed,"an input scale factor inconsistent with the baked transform is rejected");}
  {auto changed=record;auto scale=changed.value(QStringLiteral("scale")).toObject();auto derivation=scale.value(QStringLiteral("derivation")).toObject();auto solution=derivation.value(QStringLiteral("solution")).toObject();auto checks=solution.value(QStringLiteral("independent_checks")).toObject();checks.insert(QStringLiteral("status"),QStringLiteral("fail"));solution.insert(QStringLiteral("independent_checks"),checks);derivation.insert(QStringLiteral("solution"),solution);scale.insert(QStringLiteral("derivation"),derivation);changed.insert(QStringLiteral("scale"),scale);rejected(changed,"failed reference checks cannot inherit an independently-checked label");}
  {auto changed=record;changed.insert(QStringLiteral("model_geometry_accuracy"),QStringLiteral("verified"));rejected(changed,"a model-accuracy claim cannot replace the explicit not-measured scope");}
  check(!gsw::parsePlyCoordinateMetadata({QStringLiteral("units mm")},output,true).validationError.isEmpty(),"contradictory output header units are rejected");
  {
    const auto rotatedBytes=read(sidecar(rotatedOutput));auto changed=QJsonDocument::fromJson(rotatedBytes).object();
    auto transform=changed.value(QStringLiteral("applied_transform")).toObject();const QJsonArray reflection{QJsonArray{-1.,0.,0.},QJsonArray{0.,1.,0.},QJsonArray{0.,0.,1.}};
    transform.insert(QStringLiteral("R"),reflection);auto inverse=transform.value(QStringLiteral("inverse")).toObject();inverse.insert(QStringLiteral("R"),reflection);inverse.insert(QStringLiteral("t"),QJsonArray{5.,-10.,-15.});transform.insert(QStringLiteral("inverse"),inverse);changed.insert(QStringLiteral("applied_transform"),transform);
    auto scale=changed.value(QStringLiteral("scale")).toObject();auto derivation=scale.value(QStringLiteral("derivation")).toObject();auto solution=derivation.value(QStringLiteral("solution")).toObject();transform.insert(QStringLiteral("applied"),false);solution.insert(QStringLiteral("source_to_metric"),transform);derivation.insert(QStringLiteral("solution"),solution);scale.insert(QStringLiteral("derivation"),derivation);changed.insert(QStringLiteral("scale"),scale);
    write(sidecar(rotatedOutput),QJsonDocument(changed).toJson());check(!gsw::parsePlyCoordinateMetadata({},rotatedOutput,true).validationError.isEmpty(),"a consistently cross-bound reflection is still rejected as an improper similarity");write(sidecar(rotatedOutput),rotatedBytes);
  }
  const QString portable=fixture.filePath(QStringLiteral("portable.ply"));check(QFile::copy(output,portable)&&write(sidecar(portable),metadataBytes)&&
      gsw::parsePlyCoordinateMetadata({},portable,true).validationError.isEmpty(),"self-contained PLY and sidecar remain portable without an adjacent source archive");
  check(read(input)==original,"all native loading and rejection checks leave the original model untouched");
  std::fprintf(stderr,"Model scale coordinate tests: %s\n",passed?"PASS":"FAIL");return passed?0:1;
}
