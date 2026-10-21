#include <QCoreApplication>
#include "CameraTrajectory.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace gsw {
namespace {
constexpr qint64 kMaximumCameraFileSize = 64LL * 1024LL * 1024LL;
constexpr int kMaximumAncestorSearchDepth = 8;
constexpr float kMinimumAxisLength = 0.000001F;
constexpr float kRotationTolerance = 0.01F;
constexpr float kMinimumFrustumSize = 0.08F;
constexpr qsizetype kMaximumDisplayedFrustumCameras = 1000;
constexpr qsizetype kMaximumDisplayedPathCameras = 5000;

bool readPoint(const QJsonValue &value, SceneCoordinate3D *result) {
  if (!result || !value.isArray()) return false;
  const auto values = value.toArray();
  if (values.size() != 3) return false;
  for (const auto &item : values) if (!item.isDouble() || !std::isfinite(item.toDouble())) return false;
  *result = {values[0].toDouble(), values[1].toDouble(), values[2].toDouble()};
  return true;
}

bool safeMetadataPath(QString path) {
  for (;;) {
    const QFileInfo info(path);
    if (info.isSymLink()) return false;
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    const QString parent = info.absoluteDir().absolutePath();
    if (parent == path) return true;
    path = parent;
  }
}

QString readMetadata(const QString &path, QJsonObject *result, QByteArray *bytes = nullptr) {
  if (!safeMetadataPath(path)) return QStringLiteral("camera_metadata_link_refused");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("camera_metadata_unreadable");
  if (file.size() > kMaximumCameraFileSize) return QStringLiteral("camera_metadata_too_large_64_mib");
  const QByteArray data = file.read(kMaximumCameraFileSize + 1);
  if (data.size() > kMaximumCameraFileSize) return QStringLiteral("camera_metadata_too_large_64_mib");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(data, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return QStringLiteral("camera_metadata_invalid_json");
  *result = document.object();
  if (bytes) *bytes = data;
  return {};
}

bool metadataMayExist(const QString &path) {
  const QFileInfo info(path);
  if(info.exists() || info.isSymLink()) return true;
#ifdef Q_OS_WIN
  const DWORD attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
  if(attributes!=INVALID_FILE_ATTRIBUTES) return true;
  const DWORD error=GetLastError();
  return error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND;
#else
  return false;
#endif
}

QString sha256(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool validDigest(const QString &value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(value).hasMatch();
}

bool hashMatches(const QString &path, const QString &expected) {
  if (!validDigest(expected) || !safeMetadataPath(path)) return false;
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return file.open(QIODevice::ReadOnly) && hash.addData(&file) &&
      QString::fromLatin1(hash.result().toHex()) == expected;
}

bool matrix(const QJsonValue &value, int size, double *values) {
  if (!value.isArray()) return false;
  const auto rows = value.toArray();
  if (rows.size() != size) return false;
  for (int i = 0; i < size; ++i) {
    if (!rows[i].isArray()) return false;
    const auto row = rows[i].toArray();
    if (row.size() != size) return false;
    for (int j = 0; j < size; ++j) {
      if (!row[j].isDouble() || !std::isfinite(row[j].toDouble())) return false;
      values[i * size + j] = row[j].toDouble();
    }
  }
  return true;
}

bool properRotation(const double *r, int stride) {
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
    double dot = 0.;
    for (int k = 0; k < 3; ++k) dot += r[k * stride + i] * r[k * stride + j];
    if (std::abs(dot - (i == j ? 1. : 0.)) > 2e-5) return false;
  }
  const double determinant = r[0] * (r[stride+1] * r[2*stride+2] - r[stride+2] * r[2*stride+1]) -
      r[1] * (r[stride] * r[2*stride+2] - r[stride+2] * r[2*stride]) +
      r[2] * (r[stride] * r[2*stride+1] - r[stride+1] * r[2*stride]);
  return std::abs(determinant - 1.) <= 2e-5;
}

bool parseBoundCamera(const QJsonObject &object, const QString &frame, const QString &unit, CameraPose *camera) {
  const QString id = object.value(QStringLiteral("view_id")).toString();
  const QString split = object.value(QStringLiteral("split")).toString();
  const QString projection = object.value(QStringLiteral("projection_model")).toString(QStringLiteral("PINHOLE"));
  const QString pixels = object.value(QStringLiteral("pixel_coordinates")).toString(QStringLiteral("pixel_border"));
  if (id.isEmpty() || (split != QStringLiteral("train") && split != QStringLiteral("val") && split != QStringLiteral("test")) ||
      (projection != QStringLiteral("PINHOLE") && projection != QStringLiteral("SIMPLE_PINHOLE")) ||
      (pixels != QStringLiteral("pixel_border") && pixels != QStringLiteral("opencv_integer"))) return false;
  const auto widthValue = object.value(QStringLiteral("width")), heightValue = object.value(QStringLiteral("height"));
  const int width = widthValue.toInt(-1), height = heightValue.toInt(-1);
  if (!widthValue.isDouble() || !heightValue.isDouble() || width < 1 || height < 1 ||
      widthValue.toDouble() != width || heightValue.toDouble() != height) return false;
  double k[9], pose[16];
  if (!matrix(object.value(QStringLiteral("K")), 3, k) || k[0] <= 0. || k[4] <= 0. ||
      std::abs(k[3]) > 1e-12 || std::abs(k[6]) > 1e-12 || std::abs(k[7]) > 1e-12 || std::abs(k[8]-1.) > 1e-12 ||
      !matrix(object.value(QStringLiteral("c2w_opencv")), 4, pose) || !properRotation(pose, 4) ||
      std::abs(pose[12]) > 1e-12 || std::abs(pose[13]) > 1e-12 || std::abs(pose[14]) > 1e-12 || std::abs(pose[15]-1.) > 1e-12) return false;
  camera->imageName = object.value(QStringLiteral("image_path")).toString();
  camera->viewId = id; camera->split = split;
  camera->coordinateFrame = frame; camera->coordinateUnit = unit;
  camera->pixelCoordinates = pixels;
  camera->intrinsics = object.value(QStringLiteral("K")).toArray();
  camera->cameraToWorld = object.value(QStringLiteral("c2w_opencv")).toArray();
  camera->globalPosition = {pose[3], pose[7], pose[11]};
  camera->position = QVector3D(float(pose[3]), float(pose[7]), float(pose[11]));
  camera->right = QVector3D(float(pose[0]), float(pose[4]), float(pose[8]));
  camera->imageDown = QVector3D(float(pose[1]), float(pose[5]), float(pose[9]));
  camera->forward = QVector3D(float(pose[2]), float(pose[6]), float(pose[10]));
  camera->width = width; camera->height = height;
  const double fov = 2. * std::atan(height / (2. * k[4])) * 180. / 3.14159265358979323846;
  camera->verticalFovDegrees = float(fov);
  camera->focalAspectCorrection = float(k[0] / k[4]);
  return true;
}

struct BoundTrajectory {
  bool present = false;
  QString source, error, frame, unit;
  QList<CameraPose> cameras;
  QJsonObject clipping;
  qsizetype invalid = 0;
};

bool readClipping(const QJsonValue &value,double scale,const QString &inputFrame,QJsonObject *result) {
  if(value.isUndefined()) return true;
  if(!value.isObject()) return false;
  const auto object=value.toObject();
  if(object.contains(QStringLiteral("frame")) && object.value(QStringLiteral("frame")).toString()!=inputFrame) return false;
  for(const auto &name:{QStringLiteral("znear"),QStringLiteral("zfar"),QStringLiteral("backend_near")}) {
    if(!object.contains(name)) continue;
    const auto raw=object.value(name);const double length=raw.toDouble(-1.)*scale;
    if(!raw.isDouble() || !std::isfinite(length) || length<=0.) return false;
    result->insert(name,length);
  }
  const double nearValue=std::max(result->value(QStringLiteral("znear")).toDouble(0.),result->value(QStringLiteral("backend_near")).toDouble(0.));
  return !result->contains(QStringLiteral("zfar")) || nearValue<result->value(QStringLiteral("zfar")).toDouble();
}

struct Similarity {
  double scale = 1.;
  double rotation[9]{1.,0.,0.,0.,1.,0.,0.,0.,1.};
  SceneCoordinate3D translation;
};

bool readSimilarityCore(const QJsonObject &value, Similarity *result) {
  if (!value.value(QStringLiteral("s")).isDouble()) return false;
  result->scale = value.value(QStringLiteral("s")).toDouble();
  return std::isfinite(result->scale) && result->scale > 0. &&
      matrix(value.value(QStringLiteral("R")),3,result->rotation) && properRotation(result->rotation,3) &&
      readPoint(value.value(QStringLiteral("t")),&result->translation);
}

bool readSimilarity(const QJsonObject &value, Similarity *result) {
  if(!readSimilarityCore(value,result) || !std::isfinite(1./result->scale)) return false;
  const double t[3]{result->translation.x,result->translation.y,result->translation.z};
  double expectedT[3],roundoff[3];
  for(int i=0;i<3;++i) {
    expectedT[i]=0.;roundoff[i]=0.;
    for(int j=0;j<3;++j) {
      const double term=result->rotation[j*3+i]*t[j]/result->scale;
      expectedT[i]-=term;roundoff[i]+=std::abs(term);
    }
    if(!std::isfinite(expectedT[i]) || !std::isfinite(roundoff[i])) return false;
  }
  if(!value.contains(QStringLiteral("inverse"))) return true;
  Similarity inverse;
  if(!readSimilarityCore(value.value(QStringLiteral("inverse")).toObject(),&inverse)) return false;
  const double expectedScale=1./result->scale;
  if(std::abs(inverse.scale-expectedScale)>1e-12+16.*std::numeric_limits<double>::epsilon()*std::abs(expectedScale)) return false;
  const double inverseT[3]{inverse.translation.x,inverse.translation.y,inverse.translation.z};
  for(int i=0;i<3;++i) {
    if(std::abs(inverseT[i]-expectedT[i])>1e-12+32.*std::numeric_limits<double>::epsilon()*std::max(1.,roundoff[i])) return false;
    for(int j=0;j<3;++j) if(std::abs(inverse.rotation[i*3+j]-result->rotation[j*3+i])>1e-12) return false;
  }
  return true;
}

QJsonArray transformedPose(const QJsonArray &value, const Similarity &transform) {
  double input[16];
  if (!matrix(value,4,input) || !properRotation(input,4) || std::abs(input[12])>1e-12 ||
      std::abs(input[13])>1e-12 || std::abs(input[14])>1e-12 || std::abs(input[15]-1.)>1e-12) return {};
  QJsonArray result;
  const double t[3]{transform.translation.x,transform.translation.y,transform.translation.z};
  for (int i=0;i<3;++i) {
    QJsonArray row;
    for(int j=0;j<3;++j) {
      double rotation=0.;
      for(int k=0;k<3;++k) rotation+=transform.rotation[i*3+k]*input[k*4+j];
      row.append(rotation);
    }
    double position=t[i];
    for(int k=0;k<3;++k) position+=transform.scale*transform.rotation[i*3+k]*input[k*4+3];
    row.append(position); result.append(row);
  }
  result.append(QJsonArray{0.,0.,0.,1.});
  return result;
}

void readLegacyBoundViews(const QJsonObject &root, BoundTrajectory &result) {
  const bool training = root.value(QStringLiteral("schema")).toString() == QStringLiteral("gsw.training_camera_contract/1");
  const QJsonObject contract = training ? root : root.value(QStringLiteral("training_contract")).toObject();
  if (contract.value(QStringLiteral("schema")).toString() != QStringLiteral("gsw.training_camera_contract/1") ||
      !validDigest(contract.value(QStringLiteral("input_data_identity")).toString()) ||
      !contract.value(QStringLiteral("views")).isArray()) {
    result.error = QStringLiteral("camera_metadata_contract_invalid"); return;
  }
  const auto chain=contract.value(QStringLiteral("transforms")).toObject();
  const auto normalization=chain.value(QStringLiteral("canonical_to_training")).toObject();
  Similarity canonicalToTraining, trainingToOutput;
  if (!readSimilarity(normalization,&canonicalToTraining) || normalization.value(QStringLiteral("applied")) != QJsonValue(true)) {
    result.error=QStringLiteral("camera_metadata_normalization_invalid"); return;
  }
  result.frame=training?QStringLiteral("training"):root.value(QStringLiteral("output_frame")).toString();
  result.unit=training?QStringLiteral("training"):root.value(QStringLiteral("output_unit")).toString();
  if (!training) {
    const auto applied=root.value(QStringLiteral("applied_transform")).toObject();
    if (root.value(QStringLiteral("schema")).toString() != QStringLiteral("gsw.coordinate_export/1") ||
        !readSimilarity(applied,&trainingToOutput) ||
        applied.value(QStringLiteral("input_frame")).toString(QStringLiteral("training")) != QStringLiteral("training") ||
        applied.value(QStringLiteral("output_frame")).toString(result.frame) != result.frame ||
        root.value(QStringLiteral("transform_applied")) == QJsonValue(false) ||
        !QStringList{QStringLiteral("source"),QStringLiteral("canonical"),QStringLiteral("training")}.contains(result.frame) ||
        result.unit.isEmpty() ||
        root.value(QStringLiteral("input_data_identity")).toString(contract.value(QStringLiteral("input_data_identity")).toString()) !=
            contract.value(QStringLiteral("input_data_identity")).toString()) {
      result.error=QStringLiteral("camera_metadata_output_transform_invalid"); return;
    }
  }
  if(!readClipping(contract.value(QStringLiteral("clipping")),training?1.:trainingToOutput.scale,
      QStringLiteral("training"),&result.clipping)) {
    result.error=QStringLiteral("camera_metadata_clipping_invalid");return;
  }
  QSet<QString> seen;
  for (const auto &value:contract.value(QStringLiteral("views")).toArray()) {
    if (!value.isObject()) { result.error=QStringLiteral("camera_metadata_view_invalid"); break; }
    auto object=value.toObject();
    QJsonArray pose=object.value(QStringLiteral("training_c2w_opencv")).toArray();
    if (pose.isEmpty()) pose=transformedPose(object.value(QStringLiteral("c2w_opencv")).toArray(),canonicalToTraining);
    if (!training) pose=transformedPose(pose,trainingToOutput);
    object.insert(QStringLiteral("c2w_opencv"),pose);
    if(object.contains(QStringLiteral("training_K"))) object.insert(QStringLiteral("K"),object.value(QStringLiteral("training_K")));
    if(object.contains(QStringLiteral("training_size"))) {
      const auto size=object.value(QStringLiteral("training_size")).toArray();
      if(size.size()!=2) { result.error=QStringLiteral("camera_metadata_view_invalid"); break; }
      object.insert(QStringLiteral("width"),size[0]);object.insert(QStringLiteral("height"),size[1]);
    }
    CameraPose camera;
    if(!parseBoundCamera(object,result.frame,result.unit,&camera) || seen.contains(camera.viewId)) {
      result.error=QStringLiteral("camera_metadata_view_invalid");break;
    }
    seen.insert(camera.viewId);result.cameras.append(camera);
  }
  if(!result.error.isEmpty()) { ++result.invalid; result.cameras.clear(); }
}

QByteArray asciiJsonString(const QString &value) {
  QByteArray result="\"";
  for(const QChar character:value) {
    const ushort code=character.unicode();
    if(code=='"' || code=='\\') { result+='\\';result+=char(code); }
    else if(code=='\b') result+="\\b";
    else if(code=='\f') result+="\\f";
    else if(code=='\n') result+="\\n";
    else if(code=='\r') result+="\\r";
    else if(code=='\t') result+="\\t";
    else if(code<32 || code>=127) result+="\\u"+QByteArray::number(code,16).rightJustified(4,'0');
    else result+=char(code);
  }
  return result+'"';
}

QString bundleBinding(const QString &model,const QString &camera,const QString &identity,const QString &frame,const QString &unit) {
  return sha256("{\"camera_sha256\":"+asciiJsonString(camera)+",\"coordinate_frame\":"+asciiJsonString(frame)+
      ",\"coordinate_unit\":"+asciiJsonString(unit)+",\"input_data_identity\":"+asciiJsonString(identity)+
      ",\"model_sha256\":"+asciiJsonString(model)+'}');
}

BoundTrajectory readBoundTrajectory(const QString &scenePath) {
  BoundTrajectory result;
  const QFileInfo scene(scenePath);
  if (scenePath.trimmed().isEmpty() || scene.isDir()) return result;
  const QString metadata = scene.dir().filePath(scene.completeBaseName() + QStringLiteral(".camera-scale.json"));
  if (!metadataMayExist(metadata)) {
    const QString cameras=scene.dir().filePath(scene.completeBaseName()+QStringLiteral(".cameras.json"));
    QFile model(scenePath);
    QByteArray header;
    if(model.open(QIODevice::ReadOnly)) header=model.read(65536);
    if(metadataMayExist(cameras) || header.contains("comment gsw_coordinate_frame source") ||
        header.contains("comment gsw_coordinate_frame canonical")) {
      result.present=true;result.source=metadata;result.error=QStringLiteral("camera_metadata_binding_missing");
    }
    return result;
  }
  result.present = true; result.source = metadata;
  QJsonObject root;
  result.error = readMetadata(metadata, &root);
  if (!result.error.isEmpty()) return result;
  const QString modelHash = root.value(QStringLiteral("ply_sha256")).toString();
  if (!hashMatches(scenePath, modelHash)) { result.error = QStringLiteral("camera_model_hash_mismatch"); return result; }
  if(root.contains(QStringLiteral("output_sha256")) && root.value(QStringLiteral("output_sha256")).toString()!=modelHash) {
    result.error=QStringLiteral("camera_model_hash_mismatch");return result;
  }
  const QString schema=root.value(QStringLiteral("schema")).toString();
  if(!QStringList{QStringLiteral("gsw.training_camera_contract/1"),QStringLiteral("gsw.coordinate_export/1"),
      QStringLiteral("gsw.model_scale_export/1")}.contains(schema)) {
    result.error=QStringLiteral("camera_metadata_schema_unsupported");return result;
  }
  const bool training=schema==QStringLiteral("gsw.training_camera_contract/1");
  const auto contract=training?root:root.value(QStringLiteral("training_contract")).toObject();
  if(!training && root.contains(QStringLiteral("export_version")) &&
      root.value(QStringLiteral("export_version"))!=QJsonValue(1) && root.value(QStringLiteral("export_version"))!=QJsonValue(2) &&
      root.value(QStringLiteral("export_version"))!=QJsonValue(3)) {
    result.error=QStringLiteral("camera_metadata_version_unsupported");return result;
  }
  const QString contractIdentity=contract.value(QStringLiteral("input_data_identity")).toString();
  const auto transforms=contract.value(QStringLiteral("transforms")).toObject();
  Similarity sourceTransform,normalization;
  if(contract.value(QStringLiteral("schema")).toString()!=QStringLiteral("gsw.training_camera_contract/1") ||
      !validDigest(contractIdentity) || (!training && root.value(QStringLiteral("input_data_identity")).toString()!=contractIdentity) ||
      !readSimilarity(transforms.value(QStringLiteral("source_to_canonical")).toObject(),&sourceTransform) ||
      transforms.value(QStringLiteral("source_to_canonical")).toObject().value(QStringLiteral("applied"))!=QJsonValue(true) ||
      !readSimilarity(transforms.value(QStringLiteral("canonical_to_training")).toObject(),&normalization) ||
      transforms.value(QStringLiteral("canonical_to_training")).toObject().value(QStringLiteral("applied"))!=QJsonValue(true)) {
    result.error=QStringLiteral("camera_metadata_contract_invalid");return result;
  }
  std::optional<RecordedFrameTransforms> editedFrames;
  if(root.value(QStringLiteral("export_version"))==3) {
    editedFrames=recordedExportFrameTransforms(root,&result.error);
    if(!editedFrames||!validateRecordedExportFiles(root,scenePath,&result.error))return result;
  }
  const auto reference = root.value(QStringLiteral("camera_reference")).toObject();
  if (!root.contains(QStringLiteral("camera_reference"))) { readLegacyBoundViews(root,result);return result; }
  if (reference.isEmpty()) { result.error = QStringLiteral("camera_reference_invalid"); return result; }
  const QString relative = reference.value(QStringLiteral("path")).toString();
  if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains('/') || relative.contains('\\') ||
      relative.contains(':') || relative == QStringLiteral(".") || relative == QStringLiteral("..") ||
      reference.value(QStringLiteral("schema")).toString() != QStringLiteral("gsw.model_cameras/1")) {
    result.error = QStringLiteral("camera_reference_invalid"); return result;
  }
  const QString cameraPath = scene.dir().filePath(relative);
  QJsonObject bundle; QByteArray bytes;
  result.error = readMetadata(cameraPath, &bundle, &bytes);
  if (!result.error.isEmpty()) return result;
  const QString cameraHash = reference.value(QStringLiteral("sha256")).toString();
  const QString identity = root.value(QStringLiteral("input_data_identity")).toString();
  result.frame = root.value(QStringLiteral("current_frame")).toString(root.value(QStringLiteral("output_frame")).toString());
  result.unit = root.value(QStringLiteral("current_unit")).toString(root.value(QStringLiteral("output_unit")).toString());
  if (!validDigest(cameraHash) || sha256(bytes) != cameraHash) { result.error = QStringLiteral("camera_reference_hash_mismatch"); return result; }
  if (bundle.value(QStringLiteral("schema")).toString() != QStringLiteral("gsw.model_cameras/1") || bundle.value(QStringLiteral("version")).toInt() != 1 ||
      bundle.value(QStringLiteral("model_sha256")).toString() != modelHash || !validDigest(identity) ||
      bundle.value(QStringLiteral("input_data_identity")).toString() != identity ||
      (result.frame != QStringLiteral("training") && result.frame != QStringLiteral("canonical") && result.frame != QStringLiteral("source")) ||
      result.unit.isEmpty() || result.unit.size()>128 ||
      bundle.value(QStringLiteral("coordinate_frame")).toString() != result.frame || bundle.value(QStringLiteral("coordinate_unit")).toString() != result.unit ||
      root.value(QStringLiteral("output_frame")).toString() != result.frame || root.value(QStringLiteral("output_unit")).toString() != result.unit ||
      root.value(QStringLiteral("transform_applied")) != QJsonValue(true) || bundle.value(QStringLiteral("transform_applied")) != QJsonValue(true)) {
    result.error = QStringLiteral("camera_bundle_frame_or_identity_mismatch"); return result;
  }
  if (!editedFrames && root.value(QStringLiteral("bundle_sha256")).toString() != bundleBinding(modelHash,cameraHash,identity,result.frame,result.unit)) {
    result.error = QStringLiteral("camera_bundle_binding_mismatch"); return result;
  }
  if (!bundle.value(QStringLiteral("views")).isArray()) { result.error = QStringLiteral("camera_metadata_views_invalid"); return result; }
  if(!readClipping(bundle.value(QStringLiteral("clipping")),1.,result.frame,&result.clipping)) {
    result.error=QStringLiteral("camera_metadata_clipping_invalid");return result;
  }
  QSet<QString> identifiers;
  for (const auto &value : bundle.value(QStringLiteral("views")).toArray()) {
    CameraPose camera;
    if (!value.isObject() || !parseBoundCamera(value.toObject(), result.frame, result.unit, &camera) || identifiers.contains(camera.viewId)) {
      ++result.invalid; result.cameras.clear(); result.error = QStringLiteral("camera_metadata_view_invalid"); return result;
    }
    identifiers.insert(camera.viewId); result.cameras.append(camera);
  }
  if(editedFrames) {
    // Compare the published cameras with the immutable root transformed by W
    // exactly once, not with a second independently trusted camera payload.
    Similarity trainingToCurrent;
    if(!readSimilarity(editedFrames->trainingToCurrent,&trainingToCurrent)||!contract.value(QStringLiteral("views")).isArray()) {
      result.cameras.clear();result.error=QStringLiteral("camera_metadata_scene_chain_invalid");return result;
    }
    QJsonObject expectedClip;
    if(!readClipping(contract.value(QStringLiteral("clipping")),trainingToCurrent.scale,QStringLiteral("training"),&expectedClip)) {
      result.cameras.clear();result.error=QStringLiteral("camera_metadata_clipping_invalid");return result;
    }
    const auto close=[](double a,double b,double absolute=2e-7) { return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=absolute+1e-12*std::abs(b); };
    bool consistent=expectedClip.size()==result.clipping.size();
    for(auto it=expectedClip.begin();consistent&&it!=expectedClip.end();++it)consistent=result.clipping.contains(it.key())&&close(result.clipping.value(it.key()).toDouble(),it.value().toDouble(),1e-12);
    const auto originalViews=contract.value(QStringLiteral("views")).toArray();
    consistent=consistent&&originalViews.size()==result.cameras.size();
    QSet<QString> originalIds;
    for(qsizetype index=0;consistent&&index<originalViews.size();++index) {
      auto original=originalViews[index].toObject();
      auto pose=original.value(QStringLiteral("training_c2w_opencv")).toArray();
      if(pose.isEmpty())pose=transformedPose(original.value(QStringLiteral("c2w_opencv")).toArray(),normalization);
      original.insert(QStringLiteral("c2w_opencv"),transformedPose(pose,trainingToCurrent));
      if(original.contains(QStringLiteral("training_K")))original.insert(QStringLiteral("K"),original.value(QStringLiteral("training_K")));
      if(original.contains(QStringLiteral("training_size"))) {
        const auto size=original.value(QStringLiteral("training_size")).toArray();
        if(size.size()!=2) {consistent=false;break;}
        original.insert(QStringLiteral("width"),size[0]);original.insert(QStringLiteral("height"),size[1]);
      }
      CameraPose expected;
      if(!parseBoundCamera(original,result.frame,result.unit,&expected)||originalIds.contains(expected.viewId)) {consistent=false;break;}
      originalIds.insert(expected.viewId);const auto &actual=result.cameras[index];
      consistent=expected.viewId==actual.viewId&&expected.split==actual.split&&expected.imageName==actual.imageName&&
          expected.pixelCoordinates==actual.pixelCoordinates&&expected.width==actual.width&&expected.height==actual.height&&expected.intrinsics==actual.intrinsics;
      for(int i=0;consistent&&i<4;++i)for(int j=0;consistent&&j<4;++j)consistent=close(actual.cameraToWorld[i].toArray()[j].toDouble(),expected.cameraToWorld[i].toArray()[j].toDouble(),j==3?2e-7:1e-11);
    }
    if(!consistent) {result.cameras.clear();result.error=QStringLiteral("camera_metadata_scene_camera_mismatch");return result;}
  }
  result.source = cameraPath;
  return result;
}

bool readVector3(const QJsonValue &value, QVector3D *result) {
  if (result == nullptr || !value.isArray()) {
    return false;
  }
  const QJsonArray values = value.toArray();
  if (values.size() != 3 || !values.at(0).isDouble() ||
      !values.at(1).isDouble() || !values.at(2).isDouble()) {
    return false;
  }
  const double x = values.at(0).toDouble();
  const double y = values.at(1).toDouble();
  const double z = values.at(2).toDouble();
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }
  *result = QVector3D(static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(z));
  return std::isfinite(result->x()) && std::isfinite(result->y()) &&
         std::isfinite(result->z());
}

bool normalizedAxis(const QVector3D &axis, QVector3D *result) {
  if (result == nullptr || !std::isfinite(axis.lengthSquared()) ||
      axis.lengthSquared() <= kMinimumAxisLength * kMinimumAxisLength) {
    return false;
  }
  *result = axis.normalized();
  return true;
}

bool parseCamera(const QJsonValue &value, CameraPose *camera) {
  if (camera == nullptr || !value.isObject()) {
    return false;
  }
  const QJsonObject object = value.toObject();
  QVector3D position;
  if (!readVector3(object.value(QStringLiteral("position")), &position)) {
    return false;
  }

  const QJsonValue rotationValue = object.value(QStringLiteral("rotation"));
  if (!rotationValue.isArray()) {
    return false;
  }
  const QJsonArray rotation = rotationValue.toArray();
  if (rotation.size() != 3) {
    return false;
  }
  QVector3D rows[3];
  if (!readVector3(rotation.at(0), &rows[0]) ||
      !readVector3(rotation.at(1), &rows[1]) ||
      !readVector3(rotation.at(2), &rows[2])) {
    return false;
  }

  QVector3D right;
  QVector3D imageDown;
  QVector3D forward;
  if (!normalizedAxis(QVector3D(rows[0].x(), rows[1].x(), rows[2].x()),
                      &right) ||
      !normalizedAxis(QVector3D(rows[0].y(), rows[1].y(), rows[2].y()),
                      &imageDown) ||
      !normalizedAxis(QVector3D(rows[0].z(), rows[1].z(), rows[2].z()),
                      &forward)) {
    return false;
  }
  const float determinant =
      QVector3D::dotProduct(QVector3D::crossProduct(right, imageDown),
                            forward);
  if (std::abs(QVector3D::dotProduct(right, imageDown)) >
          kRotationTolerance ||
      std::abs(QVector3D::dotProduct(right, forward)) >
          kRotationTolerance ||
      std::abs(QVector3D::dotProduct(imageDown, forward)) >
          kRotationTolerance ||
      std::abs(determinant - 1.0F) > kRotationTolerance) {
    return false;
  }

  camera->imageName = object.value(QStringLiteral("img_name")).toString();
  camera->position = position;
  readPoint(object.value(QStringLiteral("position")), &camera->globalPosition);
  camera->right = right;
  camera->imageDown = imageDown;
  camera->forward = forward;
  const int width = object.value(QStringLiteral("width")).toInt();
  const int height = object.value(QStringLiteral("height")).toInt();
  camera->width = width > 0 ? width : 0;
  camera->height = height > 0 ? height : 0;
  const double fx = object.value(QStringLiteral("fx")).toDouble();
  const double fy = object.value(QStringLiteral("fy")).toDouble();
  if (height > 0 && std::isfinite(fy) && fy > 0.0) {
    const double fov = 2.0 * std::atan(height / (2.0 * fy)) * 180.0 /
                       3.14159265358979323846;
    if (fov >= 1.0 && fov <= 175.0) camera->verticalFovDegrees = float(fov);
    if (std::isfinite(fx) && fx > 0.0 && fx / fy >= 0.1 && fx / fy <= 10.0)
      camera->focalAspectCorrection = float(fx / fy);
  }
  return true;
}

QString findCameraFile(const QString &scenePath) {
  if (scenePath.trimmed().isEmpty()) {
    return {};
  }
  const QFileInfo sceneInfo(scenePath);
  QDir directory = sceneInfo.isDir() ? QDir(sceneInfo.absoluteFilePath())
                                      : sceneInfo.absoluteDir();
  for (int depth = 0; depth <= kMaximumAncestorSearchDepth; ++depth) {
    const QString candidate =
        directory.filePath(QStringLiteral("cameras.json"));
    if (QFileInfo::exists(candidate)) {
      return QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    }
    const QString before = directory.absolutePath();
    if (!directory.cdUp() || directory.absolutePath() == before) {
      break;
    }
  }
  return {};
}
} // namespace

CameraTrajectory CameraTrajectory::loadForScene(const QString &scenePath) {
  CameraTrajectory trajectory;
  const auto bound = readBoundTrajectory(scenePath);
  if (bound.present) {
    trajectory.mSourcePath = bound.source;
    trajectory.mInvalidCameraCount = bound.invalid;
    trajectory.mCoordinateFrame = bound.frame;
    trajectory.mCoordinateUnit = bound.unit;
    trajectory.mClipping = bound.clipping;
    if (!bound.error.isEmpty()) {
      trajectory.mMetadataErrorCode = bound.error;
    } else {
      trajectory.mCameras = bound.cameras;
      trajectory.mMetadataVerified = true;
    }
    return trajectory;
  }
  trajectory.mSourcePath = findCameraFile(scenePath);
  if (trajectory.mSourcePath.isEmpty()) {
    return trajectory;
  }

  const QFileInfo sourceInfo(trajectory.mSourcePath);
  if (sourceInfo.size() > kMaximumCameraFileSize) {
    trajectory.mError = QCoreApplication::translate("Workbench", "cameras.json 过大，已跳过加载（上限 64 MiB）。");
    return trajectory;
  }

  QFile source(trajectory.mSourcePath);
  if (!source.open(QIODevice::ReadOnly)) {
    trajectory.mError =
        QCoreApplication::translate("Workbench", "无法读取 cameras.json：%1").arg(source.errorString());
    return trajectory;
  }
  QJsonParseError parseError;
  const QByteArray bytes=source.read(kMaximumCameraFileSize+1);
  if(bytes.size()>kMaximumCameraFileSize) {
    trajectory.mError=QCoreApplication::translate("Workbench", "cameras.json 过大，已跳过加载（上限 64 MiB）。");return trajectory;
  }
  const QJsonDocument document =
      QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    trajectory.mError = QCoreApplication::translate("Workbench", "cameras.json 格式无效：%1（偏移 %2）")
                            .arg(parseError.errorString())
                            .arg(parseError.offset);
    return trajectory;
  }
  if (!document.isArray()) {
    trajectory.mError = QCoreApplication::translate("Workbench", "cameras.json 顶层必须是相机数组。");
    return trajectory;
  }

  const QJsonArray cameras = document.array();
  trajectory.mCameras.reserve(cameras.size());
  for (const QJsonValue &value : cameras) {
    CameraPose camera;
    if (parseCamera(value, &camera)) {
      trajectory.mCameras.append(camera);
    } else {
      ++trajectory.mInvalidCameraCount;
    }
  }
  if (!cameras.isEmpty() && trajectory.mCameras.isEmpty()) {
    trajectory.mError =
        QCoreApplication::translate("Workbench", "cameras.json 未包含可用相机位姿，请检查 position 与 rotation 字段。");
  }
  return trajectory;
}

const QList<CameraPose> &CameraTrajectory::cameras() const { return mCameras; }

QString CameraTrajectory::sourcePath() const { return mSourcePath; }

QString CameraTrajectory::error() const {
  if(!mMetadataErrorCode.isEmpty())
    return QCoreApplication::translate("Workbench", "模型相机记录无效：%1。未借用附近相机记录。").arg(mMetadataErrorCode);
  return mError;
}

qsizetype CameraTrajectory::invalidCameraCount() const {
  return mInvalidCameraCount;
}

bool CameraTrajectory::metadataVerified() const { return mMetadataVerified; }
QString CameraTrajectory::coordinateFrame() const { return mCoordinateFrame; }
QString CameraTrajectory::coordinateUnit() const { return mCoordinateUnit; }
QJsonObject CameraTrajectory::clipping() const { return mClipping; }

CameraTrajectoryGeometry CameraTrajectory::geometry(const float sceneRadius) const {
  CameraTrajectoryGeometry result;
  if (mCameras.isEmpty()) {
    return result;
  }

  QVector3D minimum = mCameras.constFirst().position;
  QVector3D maximum = minimum;
  for (const CameraPose &camera : mCameras) {
    minimum.setX(std::min(minimum.x(), camera.position.x()));
    minimum.setY(std::min(minimum.y(), camera.position.y()));
    minimum.setZ(std::min(minimum.z(), camera.position.z()));
    maximum.setX(std::max(maximum.x(), camera.position.x()));
    maximum.setY(std::max(maximum.y(), camera.position.y()));
    maximum.setZ(std::max(maximum.z(), camera.position.z()));
  }
  const float span = (maximum - minimum).length();
  const float radius = std::isfinite(sceneRadius) ? std::abs(sceneRadius) : 0.0F;
  const float size =
      std::max({span * 0.015F, radius * 0.025F, kMinimumFrustumSize});
  const qsizetype cameraCount = mCameras.size();
  const qsizetype frustumCameraCount =
      std::min(cameraCount, kMaximumDisplayedFrustumCameras);
  const qsizetype pathCameraCount =
      std::min(cameraCount, kMaximumDisplayedPathCameras);
  result.displayedFrustumCameraCount = frustumCameraCount;
  result.displayedPathCameraCount = pathCameraCount;
  result.decimated = frustumCameraCount < cameraCount ||
                     pathCameraCount < cameraCount;

  const auto sourceIndex = [cameraCount](const qsizetype displayedIndex,
                                         const qsizetype displayedCount) {
    if (displayedCount <= 1 || cameraCount <= 1) {
      return qsizetype(0);
    }
    return displayedIndex * (cameraCount - 1) / (displayedCount - 1);
  };

  result.frustums.reserve(frustumCameraCount * 8);
  for (qsizetype displayedIndex = 0;
       displayedIndex < frustumCameraCount; ++displayedIndex) {
    const CameraPose &camera =
        mCameras.at(sourceIndex(displayedIndex, frustumCameraCount));
    const float aspectHeight =
        camera.width > 0 && camera.height > 0
            ? size * static_cast<float>(camera.height) /
                  static_cast<float>(camera.width)
            : size * 0.65F;
    const QVector3D center = camera.position + camera.forward * size * 1.6F;
    QVector3D corners[] = {
        center - camera.right * size - camera.imageDown * aspectHeight,
        center + camera.right * size - camera.imageDown * aspectHeight,
        center + camera.right * size + camera.imageDown * aspectHeight,
        center - camera.right * size + camera.imageDown * aspectHeight};
    double k[9];
    if (matrix(camera.intrinsics, 3, k)) {
      const double offset = camera.pixelCoordinates == QStringLiteral("opencv_integer") ? -.5 : 0.;
      const double pixels[4][2]{{offset,offset},{camera.width+offset,offset},
          {camera.width+offset,camera.height+offset},{offset,camera.height+offset}};
      const double depth = double(size) * 1.6;
      for (int corner=0;corner<4;++corner) {
        const double y = (pixels[corner][1]-k[5])/k[4];
        const double x = (pixels[corner][0]-k[2]-k[1]*y)/k[0];
        corners[corner] = camera.position + camera.right * float(depth*x) +
            camera.imageDown * float(depth*y) + camera.forward * float(depth);
      }
    }
    for (const QVector3D &corner : corners) {
      result.frustums.append({camera.position, corner});
    }
    for (int corner = 0; corner < 4; ++corner) {
      result.frustums.append({corners[corner], corners[(corner + 1) % 4]});
    }
  }

  result.path.reserve(std::max(qsizetype(0), pathCameraCount - 1));
  for (qsizetype displayedIndex = 1; displayedIndex < pathCameraCount;
       ++displayedIndex) {
    const QVector3D start =
        mCameras.at(sourceIndex(displayedIndex - 1, pathCameraCount)).position;
    const QVector3D end =
        mCameras.at(sourceIndex(displayedIndex, pathCameraCount)).position;
    result.path.append({start, end});
  }
  return result;
}

CameraTrajectoryGeometry CameraTrajectory::geometry(const float sceneRadius,
    const SceneCoordinateInfo &coordinates) const {
  CameraTrajectory local = *this;
  for (CameraPose &camera : local.mCameras) camera.position = coordinates.localFromGlobal(camera.globalPosition);
  return local.geometry(sceneRadius);
}

} // namespace gsw
