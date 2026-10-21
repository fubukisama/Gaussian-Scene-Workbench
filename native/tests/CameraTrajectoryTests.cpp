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
  void validatesEditedV3ModelAndCameraFrameTogether();
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

void CameraTrajectoryTests::validatesEditedV3ModelAndCameraFrameTogether() {
  QTemporaryDir directory; QVERIFY(directory.isValid());
  const QString scene=directory.filePath("edited-point-model.ply");
  const QByteArray original="ply\nformat ascii 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\nend_header\n3 6 9\n";
  const QByteArray edited="ply\nformat ascii 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\nend_header\n6 22 36\n";
  QVERIFY(write(directory.filePath("original-training.ply"),original));
  QVERIFY(write(scene,edited));
  const QJsonArray quarterTurn{vector3(0,-1,0),vector3(1,0,0),vector3(0,0,1)};
  const auto sim=[](double scale,const QJsonArray &rotation,const QJsonArray &translation,const QString &from,const QString &to) {
    return QJsonObject{{"s",scale},{"R",rotation},{"t",translation},{"input_frame",from},{"output_frame",to},{"applied",true}};
  };
  const auto oldScene=sim(1.,identityRotation(),{0,0,0},"canonical","canonical");
  const auto newScene=sim(2.,quarterTurn,{10,20,30},"canonical","canonical");
  auto immutable=contract({view("retained-view")});
  auto scale=immutable["scale"].toObject();scale["evidence"]=QJsonArray{QJsonObject{{"kind","user_declaration"}}};
  immutable["scale"]=scale;immutable["ply_sha256"]=digest(original);
  immutable["clipping"]=QJsonObject{{"frame","training"},{"znear",.2},{"backend_near",.4},{"zfar",100.}};
  auto camera=view("retained-view");
  camera["c2w_opencv"]=QJsonArray{QJsonArray{0,-1,0,2},QJsonArray{1,0,0,26},QJsonArray{0,0,1,40},QJsonArray{0,0,0,1}};
  const QString modelHash=digest(edited),assetsHash=digest("[]");
  auto bundle=bundleRecord(modelHash,{camera});
  bundle["clipping"]=QJsonObject{{"frame","canonical"},{"znear",.2},{"backend_near",.4},{"zfar",100.}};
  const QByteArray cameraBytes=QJsonDocument(bundle).toJson(QJsonDocument::Compact);
  QVERIFY(write(directory.filePath("edited-point-model.cameras.json"),cameraBytes));
  const QJsonObject requestTransform{{"s",2.},{"R",quarterTurn},{"t",QJsonArray{25,40,57}}};
  QJsonObject sidecar{{"schema","gsw.coordinate_export/1"},{"export_version",3},
      {"representation","point_cloud"},{"ply_sha256",modelHash},{"output_sha256",modelHash},{"input_sha256",digest(original)},
      {"input_data_identity",QString(64,'a')},{"input_frame","training"},{"input_unit","training"},
      {"output_frame","canonical"},{"output_unit","m"},{"current_frame","canonical"},{"current_unit","m"},
      {"transform_applied",true},{"applied_transform",sim(1.,quarterTurn,{12,19,27},"training","canonical")},
      {"input_canonical_scene_transform",oldScene},{"canonical_scene_transform",newScene},
      {"scene_transform",QJsonObject{{"schema","gsw.scene_transform/1"},{"frame","training"},{"unit","training"},{"transform",requestTransform}}},
      {"scene_scale_multiplier",2.},{"physical_scale_modified",true},{"scale_evidence_scope","immutable_training_contract"},
      {"scene_edit_independent_validation",false},{"full_optimizer_state_transformed",false},
      {"assets",QJsonArray{}},{"assets_sha256",assetsHash},{"training_contract",immutable},
      {"camera_reference",QJsonObject{{"schema","gsw.model_cameras/1"},{"path","edited-point-model.cameras.json"},{"sha256",digest(cameraBytes)}}}};
  sidecar["bundle_sha256"]=digest(QJsonDocument(QJsonObject{{"assets_sha256",assetsHash},{"camera_sha256",digest(cameraBytes)},
      {"coordinate_frame","canonical"},{"coordinate_unit","m"},{"input_data_identity",QString(64,'a')},{"model_sha256",modelHash}}).toJson(QJsonDocument::Compact));
  QVERIFY(writeJson(directory.filePath("edited-point-model.camera-scale.json"),sidecar));
  const auto trajectory=gsw::CameraTrajectory::loadForScene(scene);
  QVERIFY2(trajectory.error().isEmpty(),qPrintable(trajectory.error()));
  QVERIFY(trajectory.metadataVerified());QCOMPARE(trajectory.cameras().size(),qsizetype(1));
  const auto &actual=trajectory.cameras().first();
  QCOMPARE(actual.globalPosition.x,2.);QCOMPARE(actual.globalPosition.y,26.);QCOMPARE(actual.globalPosition.z,40.);
  QCOMPARE(actual.cameraToWorld,camera["c2w_opencv"].toArray());
  QCOMPARE(actual.intrinsics,view("retained-view")["K"].toArray());
  QCOMPARE(actual.viewId,QString("retained-view"));QCOMPARE(actual.split,QString("train"));
  QCOMPARE(trajectory.clipping()["znear"].toDouble(),.2);QCOMPARE(trajectory.clipping()["zfar"].toDouble(),100.);
  const auto metadata=gsw::parsePlyCoordinateMetadata({},scene,true);
  QVERIFY2(metadata.validationError.isEmpty(),qPrintable(metadata.validationError));
  QCOMPARE(metadata.scaleStatus,QString("source_declared"));QCOMPARE(metadata.metersPerCoordinate,std::optional<double>(1.));
  QCOMPARE(metadata.provenance["canonical_scene_transform"].toObject(),newScene);
  QCOMPARE(fileBytes(scene),edited);QCOMPARE(fileBytes(directory.filePath("original-training.ply")),original);
  // Current geometry already contains W. Common-frame display must not undo
  // the active edit while converting this canonical file to training units.
  QVERIFY(writeJson(directory.filePath("original-training.camera-scale.json"),immutable));
  const auto trainingMetadata=gsw::parsePlyCoordinateMetadata({},directory.filePath("original-training.ply"),true);
  QVERIFY2(trainingMetadata.validationError.isEmpty(),qPrintable(trainingMetadata.validationError));
  gsw::SceneCoordinateTracker editedTracker(metadata),trainingTracker(trainingMetadata);QVector3D local;
  QVERIFY(editedTracker.observeAndMap(6,22,36,local));QVERIFY(trainingTracker.observeAndMap(3,6,9,local));
  const auto mapping=gsw::sceneDisplayMapping(editedTracker.info(),trainingTracker.info());QVERIFY(mapping.verifiedAlignment);
  const auto mapped=mapping.mapCoordinate({6,22,36});QCOMPARE(mapped.x,13.);QCOMPARE(mapped.y,46.);QCOMPARE(mapped.z,75.);
  QVERIFY(gsw::sceneLengthUnitDescription(editedTracker.info()).contains("2"));
  const QString metadataPath=directory.filePath("edited-point-model.camera-scale.json");
  auto badHistory=sidecar;auto changedW=newScene;changedW["t"]=QJsonArray{11,20,30};badHistory["canonical_scene_transform"]=changedW;
  QVERIFY(writeJson(metadataPath,badHistory));
  QVERIFY(!gsw::CameraTrajectory::loadForScene(scene).error().isEmpty());
  QVERIFY(!gsw::parsePlyCoordinateMetadata({},scene,true).validationError.isEmpty());
  QVERIFY(writeJson(metadataPath,sidecar));
  auto shiftedCamera=camera;shiftedCamera["c2w_opencv"]=pose(2,26,41);
  auto badBundle=bundle;badBundle["views"]=QJsonArray{shiftedCamera};
  const QByteArray badCameraBytes=QJsonDocument(badBundle).toJson(QJsonDocument::Compact);
  QVERIFY(write(directory.filePath("edited-point-model.cameras.json"),badCameraBytes));
  auto rehashed=sidecar;auto reference=rehashed["camera_reference"].toObject();reference["sha256"]=digest(badCameraBytes);rehashed["camera_reference"]=reference;
  const auto rebind=[&](QJsonObject &record,const QString &cameraHash,const QString &assetHash) {
    record["bundle_sha256"]=digest(QJsonDocument(QJsonObject{{"assets_sha256",assetHash},{"camera_sha256",cameraHash},
        {"coordinate_frame","canonical"},{"coordinate_unit","m"},{"input_data_identity",QString(64,'a')},{"model_sha256",record["ply_sha256"]}}).toJson(QJsonDocument::Compact));
  };
  rebind(rehashed,digest(badCameraBytes),assetsHash);QVERIFY(writeJson(metadataPath,rehashed));
  // A recomputed byte hash cannot legitimize a camera that disagrees with W.
  QVERIFY(gsw::CameraTrajectory::loadForScene(scene).error().contains("camera_metadata_scene_camera_mismatch"));
  QVERIFY(write(directory.filePath("edited-point-model.cameras.json"),cameraBytes));
  const QByteArray texture="owned texture bytes";const QString textureName="texture.png";
  QVERIFY(write(directory.filePath(textureName),texture));
  const QJsonArray assets{QJsonObject{{"kind","texture_file"},{"path",textureName},{"sha256",digest(texture)},{"bytes",texture.size()},{"source_reference",QString::fromUtf8("元画像.png")}}};
  auto textured=sidecar;const QString textureManifest=digest(QJsonDocument(assets).toJson(QJsonDocument::Compact));
  auto texturedBytes=edited;texturedBytes.replace("end_header\n","comment TextureFile texture.png\nend_header\n");QVERIFY(write(scene,texturedBytes));
  const auto texturedHash=digest(texturedBytes);textured["ply_sha256"]=texturedHash;textured["output_sha256"]=texturedHash;
  auto texturedCamera=bundle;texturedCamera["model_sha256"]=texturedHash;
  const auto texturedCameraBytes=QJsonDocument(texturedCamera).toJson(QJsonDocument::Compact);
  QVERIFY(write(directory.filePath("edited-point-model.cameras.json"),texturedCameraBytes));
  auto texturedReference=textured["camera_reference"].toObject();texturedReference["sha256"]=digest(texturedCameraBytes);textured["camera_reference"]=texturedReference;
  textured["assets"]=assets;textured["assets_sha256"]=textureManifest;rebind(textured,digest(texturedCameraBytes),textureManifest);
  QVERIFY(writeJson(metadataPath,textured));QVERIFY2(gsw::CameraTrajectory::loadForScene(scene).metadataVerified(),qPrintable(gsw::CameraTrajectory::loadForScene(scene).error()));
  QVERIFY(gsw::parsePlyCoordinateMetadata({},scene,true).validationError.isEmpty());
  QVERIFY(write(directory.filePath(textureName),"different texture bytes"));
  QVERIFY(!gsw::CameraTrajectory::loadForScene(scene).error().isEmpty());
  QVERIFY(!gsw::parsePlyCoordinateMetadata({},scene,true).validationError.isEmpty());
  QVERIFY(write(scene,edited));QVERIFY(write(directory.filePath("edited-point-model.cameras.json"),cameraBytes));
  QVERIFY(writeJson(metadataPath,sidecar));
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
