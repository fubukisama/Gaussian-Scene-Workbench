#include "CameraTrajectory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTranslator>
#include <QScopeGuard>
#include <QtTest>

namespace {
QJsonArray vector3(double x, double y, double z) { return {x, y, z}; }
QJsonArray identityRotation() { return {vector3(1,0,0), vector3(0,1,0), vector3(0,0,1)}; }
QJsonArray pose(double x, double y, double z) {
  return {QJsonArray{1,0,0,x}, QJsonArray{0,1,0,y}, QJsonArray{0,0,1,z}, QJsonArray{0,0,0,1}};
}
QJsonObject view(const QString &id, double x = 3., double y = 4., double z = 5.) {
  return {{"view_id", id}, {"image_path", "not-present/image.png"}, {"split", "train"},
      {"width", 1200}, {"height", 800},
      {"K", QJsonArray{QJsonArray{900.,25.,470.}, QJsonArray{0.,850.,315.}, QJsonArray{0.,0.,1.}}},
      {"c2w_opencv", pose(x,y,z)}, {"projection_model", "PINHOLE"}, {"pixel_coordinates", "pixel_border"}};
}
bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool writeJson(const QString &path, const QJsonObject &value) {
  return write(path, QJsonDocument(value).toJson(QJsonDocument::Compact));
}
QString digest(const QByteArray &value) {
  return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
QByteArray fileBytes(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
QString bindingHash(const QString &model, const QString &cameras, const QString &frame, const QString &unit) {
  const QJsonObject record{{"camera_sha256", cameras}, {"coordinate_frame", frame},
      {"coordinate_unit", unit}, {"input_data_identity", QString(64, 'a')}, {"model_sha256", model}};
  return digest(QJsonDocument(record).toJson(QJsonDocument::Compact));
}
QJsonObject contract(const QJsonArray &views) {
  const QJsonObject normalization{{"s", 2.}, {"R", identityRotation()}, {"t", vector3(1,2,3)},
      {"input_frame", "canonical"}, {"output_frame", "training"}, {"applied", true}};
  const QJsonObject source{{"s", 1.}, {"R", identityRotation()}, {"t", vector3(0,0,0)},
      {"input_frame", "source"}, {"output_frame", "canonical"}, {"applied", true}};
  return {{"schema", "gsw.training_camera_contract/1"}, {"input_data_identity", QString(64,'a')},
      {"scale", QJsonObject{{"status","source_declared"},{"source_unit","m"},{"canonical_unit","m"},{"meters_per_unit",1.}}},
      {"transforms",QJsonObject{{"source_to_canonical",source},{"canonical_to_training",normalization}}}, {"views", views}};
}
QJsonObject bundleRecord(const QString &modelHash, const QJsonArray &views, const QString &frame = "canonical", const QString &unit = "m") {
  return {{"schema", "gsw.model_cameras/1"}, {"version", 1}, {"model_sha256", modelHash},
      {"input_data_identity", QString(64,'a')}, {"coordinate_frame", frame}, {"coordinate_unit", unit},
      {"transform_applied", true}, {"views", views}};
}
QJsonObject publishBundle(const QString &scene, const QJsonArray &views, const QString &frame = "canonical", const QString &unit = "m") {
  const QFileInfo info(scene);
  write(scene, "ply\nformat ascii 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\nend_header\n1 2 3\n");
  const QString modelHash = digest(fileBytes(scene));
  const QString cameraName = info.completeBaseName() + ".cameras.json";
  const QByteArray cameras = QJsonDocument(bundleRecord(modelHash, views, frame, unit)).toJson(QJsonDocument::Compact);
  write(info.dir().filePath(cameraName), cameras);
  QJsonObject sidecar{{"schema","gsw.coordinate_export/1"},{"export_version",2},
      {"ply_sha256",modelHash},{"output_sha256",modelHash},{"input_data_identity",QString(64,'a')},
      {"output_frame",frame},{"output_unit",unit},{"current_frame",frame},{"current_unit",unit},{"transform_applied",true},
      {"training_contract",contract(views)},
      {"camera_reference",QJsonObject{{"path",cameraName},{"sha256",digest(cameras)},{"schema","gsw.model_cameras/1"}}},
      {"bundle_sha256",bindingHash(modelHash,digest(cameras),frame,unit)}};
  writeJson(info.dir().filePath(info.completeBaseName()+".camera-scale.json"),sidecar);
  return sidecar;
}
} // namespace

class CameraTrajectoryTests final : public QObject {
  Q_OBJECT
private slots:
  void loadsBoundCameraBundleWithoutImages();
  void retainsFullCalibrationStableIdsAndAllSplits();
  void readsHashBoundLegacyTrainingViewsWithoutBorrowingNeighbors();
  void usesFullIntrinsicsAfterDoublePrecisionDisplayShift();
  void rejectsMismatchedMetadataWithoutBorrowingNeighbors_data();
  void rejectsMismatchedMetadataWithoutBorrowingNeighbors();
  void keepsSameDirectoryModelsSeparateAndPortable();
  void appliesLegacyRecordedRotationScaleAndTranslationExactlyOnce();
  void boundsMetadataReadsAndRefusesIncompleteBundle();
  void keepsLegacyUnboundDisplayExplicitAndCompatible();
  void metadataFailureUsesCurrentLanguageWithoutReloadingModel();
  void clippingLengthsUseTheModelFrame();
  void loadsConfiguredNativeBundleSample();
};

void CameraTrajectoryTests::loadsBoundCameraBundleWithoutImages() {
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QString scene = QDir(temporary.path()).filePath("arbitrary-model.ply");
  publishBundle(scene, {view("stable-id")});
  const auto trajectory = gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(), qPrintable(trajectory.error()));
  QCOMPARE(trajectory.cameras().size(), qsizetype(1));
  QCOMPARE(trajectory.cameras().first().imageName, QString("not-present/image.png"));
}

void CameraTrajectoryTests::retainsFullCalibrationStableIdsAndAllSplits() {
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QString scene = QDir(temporary.path()).filePath("model.ply");
  auto first = view("train-view");
  auto validation = view("val-view"); validation.insert("split", "val");
  auto test = view("test-view"); test.insert("split", "test");
  publishBundle(scene, {first,validation,test});
  const auto trajectory = gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(), qPrintable(trajectory.error()));
  QVERIFY(trajectory.metadataVerified());
  QCOMPARE(trajectory.coordinateFrame(), QString("canonical"));
  QCOMPARE(trajectory.coordinateUnit(), QString("m"));
  QCOMPARE(trajectory.cameras().size(), qsizetype(3));
  QCOMPARE(trajectory.cameras()[1].split, QString("val"));
  QCOMPARE(trajectory.cameras()[2].viewId, QString("test-view"));
  QCOMPARE(trajectory.cameras()[0].intrinsics, first["K"].toArray());
  QCOMPARE(trajectory.cameras()[0].cameraToWorld, first["c2w_opencv"].toArray());
}

void CameraTrajectoryTests::readsHashBoundLegacyTrainingViewsWithoutBorrowingNeighbors() {
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString scene = root.filePath("training.ply");
  QVERIFY(write(scene, "original-model"));
  auto camera = view("known-source", 1,2,3);
  camera.insert("training_c2w_opencv", pose(3,6,9));
  camera.insert("training_size", QJsonArray{600,400});
  const QJsonArray actualK{QJsonArray{450.,0.,235.},QJsonArray{0.,425.,157.5},QJsonArray{0.,0.,1.}};
  camera.insert("training_K",actualK);
  auto sidecar = contract({camera}); sidecar.insert("ply_sha256",digest(fileBytes(scene)));
  QVERIFY(writeJson(root.filePath("training.camera-scale.json"),sidecar));
  QVERIFY(write(root.filePath("cameras.json"),"[{\"img_name\":\"wrong-neighbor\",\"position\":[9,9,9],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]]}]"));
  const auto trajectory = gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(),qPrintable(trajectory.error()));
  QCOMPARE(trajectory.cameras().size(),qsizetype(1));
  QCOMPARE(trajectory.cameras()[0].viewId,QString("known-source"));
  QCOMPARE(trajectory.cameras()[0].globalPosition.x,3.);
  QCOMPARE(trajectory.cameras()[0].globalPosition.y,6.);
  QCOMPARE(trajectory.cameras()[0].intrinsics,actualK);
  QCOMPARE(trajectory.cameras()[0].width,600);
  QCOMPARE(trajectory.coordinateFrame(),QString("training"));
  QVERIFY(trajectory.metadataVerified());
}

void CameraTrajectoryTests::usesFullIntrinsicsAfterDoublePrecisionDisplayShift() {
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QString scene = QDir(temporary.path()).filePath("large-origin.ply");
  const double origin = 1.e9;
  publishBundle(scene,{view("a",origin+.03125,origin+.0625,origin),view("b",origin+.09375,origin+.0625,origin)});
  const auto trajectory = gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(),qPrintable(trajectory.error()));
  gsw::SceneCoordinateInfo coordinates;
  coordinates.valid=true; coordinates.displayShift={-origin,-origin,-origin};
  const auto geometry = trajectory.geometry(1.F,coordinates);
  QCOMPARE(geometry.path.size(),qsizetype(1));
  QCOMPARE(geometry.path[0].start.x(),.03125F);
  QCOMPARE(geometry.path[0].end.x(),.09375F);
  const auto ray=geometry.frustums[0].end-geometry.frustums[0].start;
  const double expectedY=-315./850.;
  const double expectedX=(0.-470.-25.*expectedY)/900.;
  QVERIFY(std::abs(ray.x()/ray.z()-expectedX)<1.e-5);
  QVERIFY(std::abs(ray.y()/ray.z()-expectedY)<1.e-5);
}

void CameraTrajectoryTests::rejectsMismatchedMetadataWithoutBorrowingNeighbors_data() {
  QTest::addColumn<QString>("change");
  for (const char *value : {"model_hash","reference_hash","bundle_model_hash","frame","unit","binding",
                           "identity","contract_identity","normalization","reference_path","invalid_json","empty_reference",
                           "version","invalid_pose","duplicate_view","unsupported_projection","inverse","output_hash"})
    QTest::newRow(value) << QString::fromLatin1(value);
}

void CameraTrajectoryTests::rejectsMismatchedMetadataWithoutBorrowingNeighbors() {
  QFETCH(QString,change);
  QTemporaryDir temporary; QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString scene=root.filePath("model.ply"), metadata=root.filePath("model.camera-scale.json"), cameraFile=root.filePath("model.cameras.json");
  auto record=publishBundle(scene,{view("one")});
  auto bundle=QJsonDocument::fromJson(fileBytes(cameraFile)).object();
  bool rewriteBundle=false;
  if(change=="model_hash") QVERIFY(write(scene,"changed same-named model"));
  else if(change=="reference_hash") {auto reference=record["camera_reference"].toObject();reference["sha256"]=QString(64,'b');record["camera_reference"]=reference;}
  else if(change=="bundle_model_hash") {bundle["model_sha256"]=QString(64,'b');rewriteBundle=true;}
  else if(change=="frame") {bundle["coordinate_frame"]="source";rewriteBundle=true;}
  else if(change=="unit") {bundle["coordinate_unit"]="mm";rewriteBundle=true;}
  else if(change=="binding") record["bundle_sha256"]=QString(64,'b');
  else if(change=="identity") record["input_data_identity"]=QString(64,'b');
  else if(change=="contract_identity") {auto value=record["training_contract"].toObject();value["input_data_identity"]=QString(64,'b');record["training_contract"]=value;}
  else if(change=="normalization") {
    auto value=record["training_contract"].toObject();auto transforms=value["transforms"].toObject();auto normalization=transforms["canonical_to_training"].toObject();
    normalization["s"]=-2.;transforms["canonical_to_training"]=normalization;value["transforms"]=transforms;record["training_contract"]=value;
  }
  else if(change=="inverse") {
    auto value=record["training_contract"].toObject();auto transforms=value["transforms"].toObject();auto normalization=transforms["canonical_to_training"].toObject();
    normalization["inverse"]=QJsonObject{{"s",.5},{"R",identityRotation()},{"t",vector3(99,99,99)}};
    transforms["canonical_to_training"]=normalization;value["transforms"]=transforms;record["training_contract"]=value;
  }
  else if(change=="output_hash") record["output_sha256"]=QString(64,'b');
  else if(change=="reference_path") {auto reference=record["camera_reference"].toObject();reference["path"]="../borrowed.cameras.json";record["camera_reference"]=reference;}
  else if(change=="empty_reference") record["camera_reference"]=QJsonObject{};
  else if(change=="version") record["export_version"]=99;
  else if(change=="invalid_pose") {auto views=bundle["views"].toArray();auto camera=views[0].toObject();camera["c2w_opencv"]=QJsonArray{QJsonArray{-1,0,0,1},QJsonArray{0,1,0,2},QJsonArray{0,0,1,3},QJsonArray{0,0,0,1}};views[0]=camera;bundle["views"]=views;rewriteBundle=true;}
  else if(change=="duplicate_view") {auto views=bundle["views"].toArray();views.append(views[0]);bundle["views"]=views;rewriteBundle=true;}
  else if(change=="unsupported_projection") {auto views=bundle["views"].toArray();auto camera=views[0].toObject();camera["projection_model"]="FISHEYE";views[0]=camera;bundle["views"]=views;rewriteBundle=true;}
  if(rewriteBundle) {
    QVERIFY(writeJson(cameraFile,bundle));
    auto reference=record["camera_reference"].toObject();const QString hash=digest(fileBytes(cameraFile));reference["sha256"]=hash;record["camera_reference"]=reference;
    record["bundle_sha256"]=bindingHash(record["ply_sha256"].toString(),hash,"canonical","m");
  }
  if(change=="invalid_json") QVERIFY(write(metadata,"{broken"));else QVERIFY(writeJson(metadata,record));
  QVERIFY(write(root.filePath("cameras.json"),"[{\"img_name\":\"wrong-neighbor\",\"position\":[9,9,9],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]]}]"));
  const QByteArray modelBefore=fileBytes(scene),metadataBefore=fileBytes(metadata),camerasBefore=fileBytes(cameraFile);
  const auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY(trajectory.cameras().isEmpty());
  QVERIFY(!trajectory.error().isEmpty());
  QVERIFY(!trajectory.metadataVerified());
  QCOMPARE(fileBytes(scene),modelBefore);QCOMPARE(fileBytes(metadata),metadataBefore);QCOMPARE(fileBytes(cameraFile),camerasBefore);
}

void CameraTrajectoryTests::keepsSameDirectoryModelsSeparateAndPortable() {
  QTemporaryDir temporary;QVERIFY(temporary.isValid());
  const QDir root(temporary.path());QVERIFY(root.mkpath("original"));QVERIFY(root.mkpath("moved"));
  const QDir original(root.filePath("original")),moved(root.filePath("moved"));
  const QString one=original.filePath("one.ply"),two=original.filePath("two.ply");
  publishBundle(one,{view("one",1,2,3)});publishBundle(two,{view("two",7,8,9)},"source","mm");
  QCOMPARE(gsw::CameraTrajectory::loadForScene(one).cameras()[0].viewId,QString("one"));
  QCOMPARE(gsw::CameraTrajectory::loadForScene(two).cameras()[0].viewId,QString("two"));
  QCOMPARE(gsw::CameraTrajectory::loadForScene(two).coordinateUnit(),QString("mm"));
  for(const auto &name:original.entryList(QDir::Files)) QVERIFY(QFile::copy(original.filePath(name),moved.filePath(name)));
  const auto trajectory=gsw::CameraTrajectory::loadForScene(moved.filePath("two.ply"));
  QVERIFY2(trajectory.error().isEmpty(),qPrintable(trajectory.error()));
  QCOMPARE(trajectory.cameras()[0].globalPosition.x,7.);
  QCOMPARE(trajectory.cameras()[0].viewId,QString("two"));
  QVERIFY(!QFileInfo::exists(moved.filePath("not-present/image.png")));
}

void CameraTrajectoryTests::appliesLegacyRecordedRotationScaleAndTranslationExactlyOnce() {
  QTemporaryDir temporary;QVERIFY(temporary.isValid());const QDir root(temporary.path());
  const QString scene=root.filePath("restored.ply");QVERIFY(write(scene,"preserved model"));
  auto camera=view("legacy-export",1,2,3);camera.insert("training_c2w_opencv",pose(3,6,9));
  const QJsonObject applied{{"s",.5},{"R",QJsonArray{vector3(0,-1,0),vector3(1,0,0),vector3(0,0,1)}},
      {"t",vector3(10,20,30)},{"input_frame","training"},{"output_frame","source"}};
  const QJsonObject record{{"schema","gsw.coordinate_export/1"},{"ply_sha256",digest(fileBytes(scene))},
      {"output_frame","source"},{"output_unit","mm"},{"input_data_identity",QString(64,'a')},
      {"applied_transform",applied},{"training_contract",contract({camera})}};
  QVERIFY(writeJson(root.filePath("restored.camera-scale.json"),record));
  const auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(),qPrintable(trajectory.error()));
  const auto result=trajectory.cameras()[0];
  QCOMPARE(result.globalPosition.x,7.);QCOMPARE(result.globalPosition.y,21.5);QCOMPARE(result.globalPosition.z,34.5);
  QCOMPARE(result.right,QVector3D(0,1,0));QCOMPARE(result.imageDown,QVector3D(-1,0,0));
  QCOMPARE(result.forward,QVector3D(0,0,1));QCOMPARE(result.coordinateUnit,QString("mm"));
  QCOMPARE(gsw::CameraTrajectory::loadForScene(scene).cameras()[0].globalPosition.x,7.);
  auto invalid=record;auto rootContract=invalid["training_contract"].toObject();auto views=rootContract["views"].toArray();
  auto broken=views[0].toObject();auto poseValue=broken["training_c2w_opencv"].toArray();poseValue[3]=QJsonArray{1,0,0,1};
  broken["training_c2w_opencv"]=poseValue;views[0]=broken;rootContract["views"]=views;invalid["training_contract"]=rootContract;
  QVERIFY(writeJson(root.filePath("restored.camera-scale.json"),invalid));
  QVERIFY(gsw::CameraTrajectory::loadForScene(scene).cameras().isEmpty());
}

void CameraTrajectoryTests::boundsMetadataReadsAndRefusesIncompleteBundle() {
  QTemporaryDir temporary;QVERIFY(temporary.isValid());const QDir root(temporary.path());
  const QString scene=root.filePath("model.ply"),metadata=root.filePath("model.camera-scale.json");
  publishBundle(scene,{view("one")});
  QVERIFY(write(root.filePath("cameras.json"),"[{\"img_name\":\"wrong-neighbor\",\"position\":[9,9,9],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]]}]"));
  const QByteArray before=fileBytes(scene);
  {QFile file(metadata);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(64LL*1024*1024+1));}
  auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY(trajectory.cameras().isEmpty());QVERIFY(trajectory.error().contains("too_large"));
  QVERIFY(QFile::remove(metadata));
  trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY(trajectory.cameras().isEmpty());QVERIFY(!trajectory.error().isEmpty());
  QCOMPARE(fileBytes(scene),before);
}

void CameraTrajectoryTests::keepsLegacyUnboundDisplayExplicitAndCompatible() {
  QTemporaryDir temporary;QVERIFY(temporary.isValid());const QDir root(temporary.path());
  QVERIFY(root.mkpath("model/point_cloud/iteration_75"));
  const QString scene=root.filePath("model/point_cloud/iteration_75/point_cloud.ply");
  QVERIFY(write(root.filePath("model/cameras.json"),"[{\"img_name\":\"legacy\",\"position\":[1,2,3],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]],\"width\":1200,\"height\":600}]"));
  const auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY(trajectory.error().isEmpty());QVERIFY(!trajectory.metadataVerified());
  QCOMPARE(trajectory.cameras().size(),qsizetype(1));QCOMPARE(trajectory.cameras()[0].imageName,QString("legacy"));
  QCOMPARE(trajectory.cameras()[0].globalPosition.x,1.);
  const auto geometry=trajectory.geometry(2.F);QCOMPARE(geometry.frustums.size(),qsizetype(8));
  const auto corner=geometry.frustums[0].end;
  QVERIFY((corner-QVector3D(.92F,1.96F,3.128F)).length()<1.e-6F);
}

void CameraTrajectoryTests::metadataFailureUsesCurrentLanguageWithoutReloadingModel() {
  QTemporaryDir temporary;QVERIFY(temporary.isValid());const QDir root(temporary.path());
  const QString scene=root.filePath("model.ply");publishBundle(scene,{view("one")});
  QVERIFY(write(root.filePath("model.camera-scale.json"),"{broken"));
  const auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  class Translator final : public QTranslator {
  public:
    QString prefix;
    bool isEmpty() const override {return false;}
    QString translate(const char *context,const char *source,const char *,int) const override {
      if(QByteArray(context)=="Workbench" && QString::fromUtf8(source)==QStringLiteral("模型相机记录无效：%1。未借用附近相机记录。")) return prefix+QStringLiteral(":%1");
      return {};
    }
  } translator;
  translator.prefix="English";QVERIFY(QCoreApplication::installTranslator(&translator));
  const auto cleanup=qScopeGuard([&] {QCoreApplication::removeTranslator(&translator);});
  QVERIFY(trajectory.error().startsWith("English:"));
  translator.prefix=QStringLiteral("日本語");
  QVERIFY(trajectory.error().startsWith(QStringLiteral("日本語:")));
}

void CameraTrajectoryTests::clippingLengthsUseTheModelFrame() {
  QTemporaryDir temporary;QVERIFY(temporary.isValid());const QDir root(temporary.path());
  const QString scene=root.filePath("model.ply"),cameraFile=root.filePath("model.cameras.json"),metadata=root.filePath("model.camera-scale.json");
  auto record=publishBundle(scene,{view("one")});
  auto bundle=QJsonDocument::fromJson(fileBytes(cameraFile)).object();
  bundle["clipping"]=QJsonObject{{"znear",.01},{"zfar",8.},{"backend_near",.02}};
  QVERIFY(writeJson(cameraFile,bundle));
  const QString hash=digest(fileBytes(cameraFile));auto reference=record["camera_reference"].toObject();reference["sha256"]=hash;record["camera_reference"]=reference;
  record["bundle_sha256"]=bindingHash(record["ply_sha256"].toString(),hash,"canonical","m");QVERIFY(writeJson(metadata,record));
  auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QCOMPARE(trajectory.clipping()["znear"].toDouble(),.01);QCOMPARE(trajectory.clipping()["backend_near"].toDouble(),.02);
  QCOMPARE(trajectory.clipping()["zfar"].toDouble(),8.);
  auto invalidClip=bundle["clipping"].toObject();invalidClip["znear"]=-1.;bundle["clipping"]=invalidClip;QVERIFY(writeJson(cameraFile,bundle));
  const QString invalidHash=digest(fileBytes(cameraFile));reference["sha256"]=invalidHash;record["camera_reference"]=reference;
  record["bundle_sha256"]=bindingHash(record["ply_sha256"].toString(),invalidHash,"canonical","m");QVERIFY(writeJson(metadata,record));
  trajectory=gsw::CameraTrajectory::loadForScene(scene);QVERIFY(trajectory.cameras().isEmpty());QVERIFY(!trajectory.error().isEmpty());
}

void CameraTrajectoryTests::loadsConfiguredNativeBundleSample() {
  const QString scene=qEnvironmentVariable("GSW_CAMERA_BUNDLE_SAMPLE");
  if(scene.isEmpty()) QSKIP("Optional externally generated bundle; set GSW_CAMERA_BUNDLE_SAMPLE explicitly.");
  const auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(),qPrintable(trajectory.error()));QVERIFY(trajectory.metadataVerified());
  QVERIFY(!trajectory.cameras().isEmpty());
  const auto bundle=QJsonDocument::fromJson(fileBytes(trajectory.sourcePath())).object();
  const auto views=bundle["views"].toArray();QCOMPARE(trajectory.cameras().size(),views.size());
  for(qsizetype i=0;i<views.size();++i) {
    const auto expected=views[i].toObject();const auto &actual=trajectory.cameras()[i];
    QCOMPARE(actual.viewId,expected["view_id"].toString());QCOMPARE(actual.split,expected["split"].toString());
    QCOMPARE(actual.intrinsics,expected["K"].toArray());QCOMPARE(actual.cameraToWorld,expected["c2w_opencv"].toArray());
  }
  const auto metadata=gsw::parsePlyCoordinateMetadata({},scene,true);
  QVERIFY2(metadata.validationError.isEmpty(),qPrintable(metadata.validationError));
  gsw::SceneCoordinateTracker tracker(metadata);QVector3D local;
  for(const auto &camera:trajectory.cameras()) QVERIFY(tracker.observeAndMap(camera.globalPosition.x,camera.globalPosition.y,camera.globalPosition.z,local));
  QVERIFY(!trajectory.geometry(1.F,tracker.info()).frustums.isEmpty());
}

QTEST_GUILESS_MAIN(CameraTrajectoryTests)
#include "CameraTrajectoryTests.moc"
