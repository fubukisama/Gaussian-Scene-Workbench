#include <QCoreApplication>
#include "SceneCoordinates.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextStream>
#include <QSet>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gsw {
namespace {

QJsonArray vectorToJson(const SceneCoordinate3D &value) {
  return {value.x, value.y, value.z};
}

bool vectorFromJson(const QJsonValue &value, SceneCoordinate3D &result) {
  const QJsonArray values = value.toArray();
  if (values.size() != 3 || std::any_of(values.cbegin(), values.cend(),
        [](const QJsonValue &item) { return !item.isDouble(); })) {
    return false;
  }
  result = {values.at(0).toDouble(), values.at(1).toDouble(),
            values.at(2).toDouble()};
  return result.isFinite();
}

QString unitKey(const SceneLengthUnit unit) {
  switch (unit) {
  case SceneLengthUnit::Millimetres:
    return QStringLiteral("millimetres");
  case SceneLengthUnit::Centimetres:
    return QStringLiteral("centimetres");
  case SceneLengthUnit::Metres:
    return QStringLiteral("metres");
  case SceneLengthUnit::Unknown:
    return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

SceneLengthUnit unitFromKey(const QString &key) {
  const QString normalized = key.trimmed().toLower();
  if (normalized == QStringLiteral("millimetres") ||
      normalized == QStringLiteral("millimeters") ||
      normalized == QStringLiteral("millimetre") ||
      normalized == QStringLiteral("millimeter") ||
      normalized == QStringLiteral("mm")) {
    return SceneLengthUnit::Millimetres;
  }
  if (normalized == QStringLiteral("centimetres") ||
      normalized == QStringLiteral("centimeters") ||
      normalized == QStringLiteral("centimetre") ||
      normalized == QStringLiteral("centimeter") ||
      normalized == QStringLiteral("cm")) {
    return SceneLengthUnit::Centimetres;
  }
  if (normalized == QStringLiteral("metres") ||
      normalized == QStringLiteral("meters") ||
      normalized == QStringLiteral("metre") ||
      normalized == QStringLiteral("meter") ||
      normalized == QStringLiteral("m")) {
    return SceneLengthUnit::Metres;
  }
  return SceneLengthUnit::Unknown;
}

double metresPerUnit(const SceneLengthUnit unit) {
  switch (unit) {
  case SceneLengthUnit::Millimetres:
    return 0.001;
  case SceneLengthUnit::Centimetres:
    return 0.01;
  case SceneLengthUnit::Metres:
    return 1.0;
  case SceneLengthUnit::Unknown:
    return 1.0;
  }
  return 1.0;
}

bool validSimilarity(const QJsonObject &transform) {
  const auto scaleValue = transform.value(QStringLiteral("s"));
  const double s = scaleValue.toDouble(-1.);
  const auto rotation = transform.value(QStringLiteral("R")).toArray();
  const auto translation = transform.value(QStringLiteral("t")).toArray();
  if (!scaleValue.isDouble() || !std::isfinite(s) || s <= 0. || rotation.size() != 3 || translation.size() != 3)
    return false;
  double r[3][3];
  for (int i = 0; i < 3; ++i) {
    const auto row = rotation[i].toArray();
    if (row.size() != 3 || !translation[i].isDouble() || !std::isfinite(translation[i].toDouble())) return false;
    for (int j = 0; j < 3; ++j) {
      if (!row[j].isDouble() || !std::isfinite(row[j].toDouble())) return false;
      r[i][j] = row[j].toDouble();
    }
  }
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
    double product = 0.;
    for (int k = 0; k < 3; ++k) product += r[k][i] * r[k][j];
    if (std::abs(product - (i == j ? 1. : 0.)) > 2e-5) return false;
  }
  const double determinant = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
      r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
      r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  return std::abs(determinant - 1.) <= 2e-5;
}

bool validRecordedInverse(const QJsonObject &transform) {
  const auto inverse = transform.value(QStringLiteral("inverse")).toObject();
  if (!validSimilarity(transform) || !validSimilarity(inverse)) return false;
  const double s = transform.value(QStringLiteral("s")).toDouble();
  const double inverseScale = 1. / s;
  const auto close = [](double a, double b, double absolute = 1e-10) {
    return std::isfinite(a) && std::isfinite(b) &&
        std::abs(a - b) <= absolute + 1e-10 * std::max(std::abs(a), std::abs(b));
  };
  if (!close(inverse.value(QStringLiteral("s")).toDouble(), inverseScale, 0.)) return false;
  const auto r = transform.value(QStringLiteral("R")).toArray();
  const auto ir = inverse.value(QStringLiteral("R")).toArray();
  const auto t = transform.value(QStringLiteral("t")).toArray();
  const auto it = inverse.value(QStringLiteral("t")).toArray();
  for (int i = 0; i < 3; ++i) {
    double expectedTranslation = 0.;
    for (int j = 0; j < 3; ++j) {
      const double value = r[j].toArray()[i].toDouble();
      if (!close(ir[i].toArray()[j].toDouble(), value)) return false;
      expectedTranslation -= value * t[j].toDouble() * inverseScale;
    }
    if (!close(it[i].toDouble(), expectedTranslation)) return false;
  }
  return true;
}

struct DisplaySimilarity {
  double s = 1.;
  double r[3][3]{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}};
  double t[3]{0.,0.,0.};
};

DisplaySimilarity displaySimilarity(const QJsonObject &object) {
  DisplaySimilarity result; result.s=object.value(QStringLiteral("s")).toDouble();
  for(int i=0;i<3;++i) {
    result.t[i]=object.value(QStringLiteral("t")).toArray()[i].toDouble();
    for(int j=0;j<3;++j)result.r[i][j]=object.value(QStringLiteral("R")).toArray()[i].toArray()[j].toDouble();
  }
  return result;
}

DisplaySimilarity composeSimilarity(const DisplaySimilarity &a,const DisplaySimilarity &b) {
  DisplaySimilarity result;result.s=a.s*b.s;
  for(int i=0;i<3;++i) {
    result.t[i]=a.t[i];
    for(int j=0;j<3;++j) {
      result.t[i]+=a.s*a.r[i][j]*b.t[j];result.r[i][j]=0.;
      for(int k=0;k<3;++k)result.r[i][j]+=a.r[i][k]*b.r[k][j];
    }
  }
  return result;
}

DisplaySimilarity inverseSimilarity(const DisplaySimilarity &value) {
  DisplaySimilarity result;result.s=1./value.s;
  for(int i=0;i<3;++i)for(int j=0;j<3;++j) {
    result.r[i][j]=value.r[j][i];result.t[i]-=value.r[j][i]*value.t[j]/value.s;
  }
  return result;
}

QJsonObject similarityJson(const DisplaySimilarity &value) {
  QJsonArray rotation,translation;
  for(int i=0;i<3;++i) {
    rotation.append(QJsonArray{value.r[i][0],value.r[i][1],value.r[i][2]});translation.append(value.t[i]);
  }
  return {{QStringLiteral("s"),value.s},{QStringLiteral("R"),rotation},{QStringLiteral("t"),translation}};
}

bool similaritiesClose(const DisplaySimilarity &a,const DisplaySimilarity &b) {
  if(!std::isfinite(a.s)||!std::isfinite(b.s)||a.s<=0.||b.s<=0.||
      std::abs(a.s-b.s)>1e-12*std::max(std::abs(a.s),std::abs(b.s)))return false;
  for(int i=0;i<3;++i) {
    if(!std::isfinite(a.t[i])||!std::isfinite(b.t[i])||
        std::abs(a.t[i]-b.t[i])>2e-7+1e-12*std::abs(b.t[i]))return false;
    for(int j=0;j<3;++j)if(!std::isfinite(a.r[i][j])||!std::isfinite(b.r[i][j])||
        std::abs(a.r[i][j]-b.r[i][j])>1e-11)return false;
  }
  return true;
}

bool frameToCanonical(const QJsonObject &root,const QString &frame,const QString &unit,DisplaySimilarity &result) {
  const auto chain=root.value(QStringLiteral("transforms")).toObject();
  const auto source=chain.value(QStringLiteral("source_to_canonical")).toObject();
  const auto normalization=chain.value(QStringLiteral("canonical_to_training")).toObject();
  const auto scale=root.value(QStringLiteral("scale")).toObject();
  const bool known=scale.value(QStringLiteral("status"))!=QStringLiteral("arbitrary")&&scale.value(QStringLiteral("meters_per_unit")).isDouble();
  QString nativeUnit;double nativeMeters=0.;
  result={};
  if(frame==QStringLiteral("source")) {
    result=displaySimilarity(source);nativeUnit=scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("unknown"));
    nativeMeters=known?scale.value(QStringLiteral("meters_per_unit")).toDouble():0.;
  } else if(frame==QStringLiteral("training")) {
    result=inverseSimilarity(displaySimilarity(normalization));nativeUnit=QStringLiteral("training");nativeMeters=known?result.s:0.;
  } else if(frame==QStringLiteral("canonical")) {
    nativeUnit=scale.value(QStringLiteral("canonical_unit")).toString(QStringLiteral("arbitrary"));nativeMeters=known?1.:0.;
  } else return false;
  if(unit!=nativeUnit) {
    const auto parsed=unitFromKey(unit);
    if(!known||parsed==SceneLengthUnit::Unknown||!std::isfinite(nativeMeters)||nativeMeters<=0.)return false;
    result.s*=metresPerUnit(parsed)/nativeMeters;
  }
  return validSimilarity(similarityJson(result));
}

bool noPathLinks(QString path) {
  for(;;) {
    const QFileInfo info(path);if(info.isSymLink())return false;
#ifdef Q_OS_WIN
    const DWORD attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
#endif
    const QString parent=info.absoluteDir().absolutePath();if(parent==path)return true;path=parent;
  }
}

QString fileDigest(const QString &path) {
  if(!noPathLinks(path))return {};
  QFile file(path);QCryptographicHash hash(QCryptographicHash::Sha256);
  return file.open(QIODevice::ReadOnly)&&hash.addData(&file)?QString::fromLatin1(hash.result().toHex()):QString{};
}

bool safeAssetReference(const QString &relative) {
  if(relative.isEmpty()||QDir::isAbsolutePath(relative)||relative.contains('\\')||relative.contains(':')||relative.contains(QChar(0)))return false;
  const auto parts=relative.split('/');
  static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),QRegularExpression::CaseInsensitiveOption);
  return std::none_of(parts.cbegin(),parts.cend(),[](const QString &part){return part==QStringLiteral("..")||part.endsWith('.')||part.endsWith(' ')||reserved.match(part).hasMatch();});
}

bool displayFrameContext(const SceneCoordinateInfo &info, DisplaySimilarity &transform,
                         QJsonObject &lineage) {
  const auto &provenance=info.provenance;
  if(provenance.value(QStringLiteral("native_hash_verified"))!=true)return false;
  const auto root=provenance.value(QStringLiteral("training_contract")).toObject();
  const auto transforms=root.value(QStringLiteral("transforms")).toObject();
  const auto source=transforms.value(QStringLiteral("source_to_canonical")).toObject();
  const auto normalization=transforms.value(QStringLiteral("canonical_to_training")).toObject();
  if(root.value(QStringLiteral("schema"))!=QStringLiteral("gsw.training_camera_contract/1") ||
      !validSimilarity(source) || source.value(QStringLiteral("applied"))!=true ||
      !validSimilarity(normalization) || normalization.value(QStringLiteral("applied"))!=true ||
      root.value(QStringLiteral("input_data_identity")).toString()!=info.dataIdentity)return false;
  const auto scale=root.value(QStringLiteral("scale")).toObject();
  const bool known=scale.value(QStringLiteral("status"))!=QStringLiteral("arbitrary");
  const auto frame=provenance.value(QStringLiteral("coordinate_frame")).toString();
  const auto unit=provenance.value(QStringLiteral("coordinate_unit")).toString();
  QString nativeUnit; double nativeMeters=0.;
  if(frame==QStringLiteral("training")) {
    const auto forward=displaySimilarity(normalization);
    transform.s=1./forward.s;
    for(int i=0;i<3;++i)for(int j=0;j<3;++j) {
      transform.r[i][j]=forward.r[j][i];
      transform.t[i]-=forward.r[j][i]*forward.t[j]/forward.s;
    }
    nativeUnit=QStringLiteral("training"); nativeMeters=1./forward.s;
  } else if(frame==QStringLiteral("source")) {
    transform=displaySimilarity(source);nativeUnit=scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("unknown"));
    nativeMeters=scale.value(QStringLiteral("meters_per_unit")).toDouble();
  } else if(frame==QStringLiteral("canonical")) {
    nativeUnit=known?QStringLiteral("m"):QStringLiteral("unknown");nativeMeters=1.;
  } else return false;
  if(unit!=nativeUnit && !(unit==QStringLiteral("arbitrary")&&!known&&frame==QStringLiteral("canonical"))) {
    const auto parsed=unitFromKey(unit);
    if(!known || parsed==SceneLengthUnit::Unknown || !std::isfinite(nativeMeters)||nativeMeters<=0.)return false;
    transform.s*=metresPerUnit(parsed)/nativeMeters;
  }
  // Numerical training normalization is intentionally not part of canonical
  // identity: two runs may normalize the same canonical data differently.
  lineage={{QStringLiteral("identity"),info.dataIdentity.toLower()},
      {QStringLiteral("source_to_canonical"),QJsonObject{{QStringLiteral("s"),source.value(QStringLiteral("s"))},
          {QStringLiteral("R"),source.value(QStringLiteral("R"))},{QStringLiteral("t"),source.value(QStringLiteral("t"))}}},
      {QStringLiteral("canonical_unit"),scale.value(QStringLiteral("canonical_unit"))}};
  return true;
}

void readModelScaleRecord(const QJsonObject &root, const QString &sidecarPath,
                         SceneCoordinateMetadata &metadata) {
  const auto fail = [&metadata](const char *code) {
    metadata.validationError = QCoreApplication::translate("Workbench",
        "模型定尺度记录无效：%1。未采用该尺度，原文件保持不变。")
            .arg(QString::fromLatin1(code));
  };
  static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
  const auto hash = [](const QJsonValue &value) { return hashPattern.match(value.toString()).hasMatch(); };
  const auto scale = root.value(QStringLiteral("scale")).toObject();
  const auto input = root.value(QStringLiteral("input_model")).toObject();
  const auto derivation = scale.value(QStringLiteral("derivation")).toObject();
  const auto solution = derivation.value(QStringLiteral("solution")).toObject();
  const auto request = derivation.value(QStringLiteral("request")).toObject();
  const auto transform = root.value(QStringLiteral("applied_transform")).toObject();
  const auto sourceTransform = solution.value(QStringLiteral("source_to_metric")).toObject();
  const QString identity = input.value(QStringLiteral("data_identity")).toString();
  const QString status = scale.value(QStringLiteral("status")).toString();
  const auto checks = solution.value(QStringLiteral("independent_checks")).toObject();
  const QString checkStatus = checks.value(QStringLiteral("status")).toString();
  const QString mode = request.value(QStringLiteral("mode")).toString();
  constexpr const char *Scope = "scale_and_reference_constraint_agreement";
  if (root.value(QStringLiteral("coordinate_frame")) != QStringLiteral("metric") ||
      root.value(QStringLiteral("representation")) != QStringLiteral("baked_model") ||
      root.value(QStringLiteral("original_model_retained")) != true ||
      root.value(QStringLiteral("model_geometry_accuracy")) != QStringLiteral("not_measured")) {
    fail("model_scale_invalid_output_frame"); return;
  }
  if (!hash(input.value(QStringLiteral("sha256"))) || !hash(input.value(QStringLiteral("data_identity"))) ||
      input.value(QStringLiteral("coordinate_frame")) != QStringLiteral("source") ||
      derivation.value(QStringLiteral("input_data_identity")) != identity ||
      request.value(QStringLiteral("input_data_identity")) != identity ||
      solution.value(QStringLiteral("input_data_identity")) != identity) {
    fail("model_scale_invalid_identity"); return;
  }
  if (derivation.value(QStringLiteral("schema")) != QStringLiteral("gsw.scale_derivation/1") ||
      derivation.value(QStringLiteral("input_kind")) != QStringLiteral("model") ||
      request.value(QStringLiteral("schema")) != QStringLiteral("gsw.scale_constraints/1") ||
      request.value(QStringLiteral("input_frame")) != QStringLiteral("source") ||
      (mode != QStringLiteral("scale_only") && mode != QStringLiteral("sim3")) ||
      solution.value(QStringLiteral("canonical_request")).toObject().value(QStringLiteral("mode")) != mode ||
      solution.value(QStringLiteral("canonical_request")).toObject().value(QStringLiteral("input_data_identity")) != identity ||
      !request.value(QStringLiteral("constraints")).isArray() || request.value(QStringLiteral("constraints")).toArray().isEmpty() ||
      solution.value(QStringLiteral("schema")) != QStringLiteral("gsw.scale_solution/1") ||
      solution.value(QStringLiteral("solver")) != QStringLiteral("gsw.weighted_scale_constraints") ||
      solution.value(QStringLiteral("solver_version")) != 1 ||
      solution.value(QStringLiteral("request")).toObject() != request ||
      !hash(root.value(QStringLiteral("solution_sha256"))) ||
      solution.value(QStringLiteral("solution_sha256")) != root.value(QStringLiteral("solution_sha256")) ||
      !hash(solution.value(QStringLiteral("request_sha256"))) ||
      !hash(solution.value(QStringLiteral("canonical_request_sha256"))) ||
      solution.value(QStringLiteral("scope")) != QString::fromLatin1(Scope) ||
      solution.value(QStringLiteral("model_geometry_accuracy")) != QStringLiteral("not_measured") ||
      solution.value(QStringLiteral("fit")).toObject().value(QStringLiteral("accepted")) != true) {
    fail("model_scale_invalid_solution_provenance"); return;
  }
  if (!validRecordedInverse(transform) || !transform.value(QStringLiteral("applied")).isBool() ||
      !transform.value(QStringLiteral("applied")).toBool() ||
      transform.value(QStringLiteral("input_frame")) != QStringLiteral("source") ||
      transform.value(QStringLiteral("output_frame")) != QStringLiteral("metric")) {
    fail("model_scale_invalid_applied_transform"); return;
  }
  if (mode == QStringLiteral("scale_only")) {
    const auto rotation = transform.value(QStringLiteral("R")).toArray();
    const auto translation = transform.value(QStringLiteral("t")).toArray();
    for (int i = 0; i < 3; ++i) {
      if (std::abs(translation[i].toDouble()) > 1e-10) { fail("model_scale_scale_only_alignment_conflict"); return; }
      for (int j = 0; j < 3; ++j)
        if (std::abs(rotation[i].toArray()[j].toDouble() - (i == j ? 1. : 0.)) > 1e-10) {
          fail("model_scale_scale_only_alignment_conflict"); return;
        }
    }
  }
  auto comparedTransform = transform; auto comparedSource = sourceTransform;
  comparedTransform.remove(QStringLiteral("applied")); comparedSource.remove(QStringLiteral("applied"));
  const auto metersValue = scale.value(QStringLiteral("meters_per_unit"));
  const double meters = metersValue.toDouble(-1.);
  if (comparedTransform != comparedSource || !metersValue.isDouble() || !std::isfinite(meters) || meters <= 0. ||
      std::abs(meters - transform.value(QStringLiteral("s")).toDouble()) > 1e-10 * std::max(meters, transform.value(QStringLiteral("s")).toDouble()) ||
      scale.value(QStringLiteral("canonical_unit")) != QStringLiteral("m") ||
      scale.value(QStringLiteral("verification_origin")) != QStringLiteral("gsw_replay_verified")) {
    fail("model_scale_transform_binding_mismatch"); return;
  }
  if ((status != QStringLiteral("constraint_derived") && status != QStringLiteral("independently_checked")) ||
      (checkStatus != QStringLiteral("not_provided") && checkStatus != QStringLiteral("unverified") && checkStatus != QStringLiteral("pass")) ||
      (status == QStringLiteral("independently_checked")) != (checkStatus == QStringLiteral("pass")) ||
      checks.value(QStringLiteral("scope")) != QString::fromLatin1(Scope) ||
      checks.value(QStringLiteral("model_geometry_accuracy")) != QStringLiteral("not_measured") ||
      (checkStatus == QStringLiteral("pass") && (checks.value(QStringLiteral("disjoint")) != true ||
          checks.value(QStringLiteral("declared_independent")) != true))) {
    fail("model_scale_invalid_check_status"); return;
  }
  bool evidenceBound = false;
  for (const auto &value : scale.value(QStringLiteral("evidence")).toArray()) {
    const auto evidence = value.toObject();
    if (evidence.value(QStringLiteral("kind")) == QStringLiteral("scale_constraints") &&
        evidence.value(QStringLiteral("input_data_identity")) == identity &&
        evidence.value(QStringLiteral("solution_sha256")) == root.value(QStringLiteral("solution_sha256")) &&
        evidence.value(QStringLiteral("scope")) == QString::fromLatin1(Scope) &&
        evidence.value(QStringLiteral("model_geometry_accuracy")) == QStringLiteral("not_measured")) evidenceBound = true;
  }
  const auto lineage = root.value(QStringLiteral("lineage")).toArray();
  if (!evidenceBound || lineage.isEmpty() || lineage.size() > 8 ||
      derivation.value(QStringLiteral("lineage")).toArray() != lineage ||
      lineage.last().toObject().value(QStringLiteral("input_data_identity")) != identity ||
      lineage.last().toObject().value(QStringLiteral("model_sha256")) != input.value(QStringLiteral("sha256")) ||
      !hash(derivation.value(QStringLiteral("input_manifest")).toObject().value(QStringLiteral("sha256")))) {
    fail("model_scale_invalid_lineage"); return;
  }
  for (const auto &value : lineage) {
    const auto item = value.toObject();
    const auto oldSidecar = item.value(QStringLiteral("sidecar_sha256"));
    if (!hash(item.value(QStringLiteral("input_data_identity"))) || !hash(item.value(QStringLiteral("model_sha256"))) ||
        (!oldSidecar.isNull() && !oldSidecar.isUndefined() && !hash(oldSidecar))) {
      fail("model_scale_invalid_lineage"); return;
    }
  }
  if (metadata.unitDeclared && metadata.unit != SceneLengthUnit::Metres) {
    fail("model_scale_conflicting_output_units"); return;
  }
  // The worker replays the shared solver before publishing. This native reader
  // checks structure, cross-record bindings and the actual PLY checksum, not a
  // second SVD/fit or physical model accuracy. Archives are intentionally not
  // mandatory: a complete PLY + embedded sidecar remains a portable artifact.
  metadata.unit = SceneLengthUnit::Metres; metadata.unitDeclared = true;
  metadata.metersPerCoordinate = 1.; // Already baked metric output, NOT input scale.meters_per_unit.
  metadata.scaleStatus = status; metadata.sourceUnit = scale.value(QStringLiteral("source_unit")).toString();
  metadata.dataIdentity = identity; metadata.provenance = root;
  if (!metadata.coordinateReferenceSystem.isEmpty())
    metadata.provenance.insert(QStringLiteral("ignored_output_crs"), metadata.coordinateReferenceSystem);
  // A local Sim3 does not establish a CRS. Old PLY comments or an adjacent PRJ
  // must not label this newly fitted metric reference frame as georeferenced.
  // input_declared_crs, when supplied by the worker, remains source provenance.
  metadata.coordinateReferenceSystem.clear();
  metadata.provenance.insert(QStringLiteral("sidecar"), QFileInfo(sidecarPath).fileName());
  metadata.provenance.insert(QStringLiteral("verification_level"), QStringLiteral("native_structural_and_ply_hash"));
  metadata.provenance.insert(QStringLiteral("native_solver_replayed"), false);
}

void readCameraScaleSidecar(const QString &sourcePath,
                           SceneCoordinateMetadata &metadata) {
  const QFileInfo source(sourcePath);
  const QString path = source.dir().filePath(source.completeBaseName() +
                                              QStringLiteral(".camera-scale.json"));
  if (!QFileInfo::exists(path)) return;
  const auto fail = [&](const char *code) { metadata.validationError = QString::fromLatin1(code); };
  QFile sidecar(path);
  if (!sidecar.open(QIODevice::ReadOnly) || sidecar.size() > 64LL * 1024 * 1024) {
    fail("camera_scale_invalid_sidecar"); return;
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(sidecar.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail("camera_scale_invalid_sidecar"); return;
  }
  const auto root = document.object();
  const QString schema = root.value(QStringLiteral("schema")).toString();
  const bool training = schema == QStringLiteral("gsw.training_camera_contract/1");
  const bool exported = schema == QStringLiteral("gsw.coordinate_export/1");
  const bool modelScaled = schema == QStringLiteral("gsw.model_scale_export/1");
  if (!training && !exported && !modelScaled) { fail("camera_scale_unsupported_schema"); return; }
  const QString expected = root.value(QStringLiteral("ply_sha256")).toString().toLower();
  static const QRegularExpression digestExpression(QStringLiteral("^[0-9a-f]{64}$"));
  QFile ply(sourcePath);
  QCryptographicHash digest(QCryptographicHash::Sha256);
  if (!digestExpression.match(expected).hasMatch() || !ply.open(QIODevice::ReadOnly) ||
      !digest.addData(&ply) || QString::fromLatin1(digest.result().toHex()) != expected) {
    fail("camera_scale_ply_hash_mismatch"); return;
  }
  if (modelScaled) { readModelScaleRecord(root, path, metadata); return; }
  auto contract = root;
  for(int depth=0;contract.value(QStringLiteral("schema"))==QStringLiteral("gsw.coordinate_export/1")&&depth<8;++depth)
    contract=contract.value(QStringLiteral("training_contract")).toObject();
  if (contract.value(QStringLiteral("schema")).toString() != QStringLiteral("gsw.training_camera_contract/1")) {
    fail("camera_scale_invalid_contract"); return;
  }
  const auto scale = contract.value(QStringLiteral("scale")).toObject();
  const QString status = scale.value(QStringLiteral("status")).toString();
  if (status != QStringLiteral("arbitrary") && status != QStringLiteral("source_declared") &&
      status != QStringLiteral("constraint_derived") && status != QStringLiteral("independently_checked")) {
    fail("camera_scale_invalid_status"); return;
  }
  metadata.scaleStatus = status;
  metadata.sourceUnit = scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("unknown"));
  metadata.dataIdentity = contract.value(QStringLiteral("input_data_identity")).toString();
  if (!digestExpression.match(metadata.dataIdentity.toLower()).hasMatch()) {
    fail("camera_scale_invalid_identity"); return;
  }
  if(exported && root.contains(QStringLiteral("input_data_identity")) &&
      root.value(QStringLiteral("input_data_identity")).toString()!=metadata.dataIdentity) {
    fail("camera_scale_invalid_identity");return;
  }
  const auto transforms = contract.value(QStringLiteral("transforms")).toObject();
  const auto normalization = transforms.value(QStringLiteral("canonical_to_training")).toObject();
  if (!validSimilarity(normalization) || !normalization.value(QStringLiteral("applied")).isBool() ||
      !normalization.value(QStringLiteral("applied")).toBool() ||
      (normalization.contains(QStringLiteral("inverse"))&&!validRecordedInverse(normalization))) {
    fail("camera_scale_invalid_normalization"); return;
  }
  if (transforms.contains(QStringLiteral("source_to_canonical")) &&
      (!validSimilarity(transforms.value(QStringLiteral("source_to_canonical")).toObject()) ||
       (transforms.value(QStringLiteral("source_to_canonical")).toObject().contains(QStringLiteral("inverse"))&&
        !validRecordedInverse(transforms.value(QStringLiteral("source_to_canonical")).toObject())))) {
    fail("camera_scale_invalid_source_transform"); return;
  }
  QString frame = training ? QStringLiteral("training") : root.value(QStringLiteral("output_frame")).toString();
  if (frame != QStringLiteral("training") && frame != QStringLiteral("source") && frame != QStringLiteral("canonical")) {
    fail("camera_scale_invalid_output_frame"); return;
  }
  std::optional<double> ratio;
  if (status != QStringLiteral("arbitrary")) {
    const double sourceRatio = scale.value(QStringLiteral("meters_per_unit")).toDouble(-1.);
    if (!std::isfinite(sourceRatio) || sourceRatio <= 0. ||
        scale.value(QStringLiteral("canonical_unit")).toString() != QStringLiteral("m")) {
      fail("camera_scale_conflicting_units"); return;
    }
    const auto declaredUnit = unitFromKey(metadata.sourceUnit);
    if (declaredUnit != SceneLengthUnit::Unknown &&
        std::abs(sourceRatio - metresPerUnit(declaredUnit)) > 1e-12 * std::max(1., sourceRatio)) {
      fail("camera_scale_conflicting_units"); return;
    }
    if(transforms.contains(QStringLiteral("source_to_canonical")) &&
        std::abs(transforms.value(QStringLiteral("source_to_canonical")).toObject().value(QStringLiteral("s")).toDouble()-sourceRatio)>
          1e-12*std::max(1.,sourceRatio)) {
      fail("camera_scale_conflicting_source_scale");return;
    }
    if (frame == QStringLiteral("source")) ratio = sourceRatio;
    else if (frame == QStringLiteral("canonical")) ratio = 1.;
    else {
      const double s = normalization.value(QStringLiteral("s")).toDouble(-1.);
      if (!std::isfinite(s) || s <= 0. || !normalization.value(QStringLiteral("applied")).toBool()) {
        fail("camera_scale_invalid_normalization"); return;
      }
      ratio = 1. / s;
    }
  } else if (scale.value(QStringLiteral("meters_per_unit")).isDouble() ||
             unitFromKey(metadata.sourceUnit) != SceneLengthUnit::Unknown) {
    fail("camera_scale_conflicting_units"); return;
  }
  if (ratio && (!std::isfinite(*ratio) || *ratio <= 0.)) {
    fail("camera_scale_invalid_normalization"); return;
  }
  if (exported) {
    const QString outputUnit = root.value(QStringLiteral("output_unit")).toString();
    const int version=root.value(QStringLiteral("export_version")).toInt(1);
    if(version<1||version>3) { fail("camera_scale_unsupported_version");return; }
    if(version==3) {
      QString reason;
      if(!noPathLinks(path)||!recordedExportFrameTransforms(root,&reason)||!validateRecordedExportFiles(root,sourcePath,&reason)) {
        metadata.validationError=reason.isEmpty()?QStringLiteral("camera_metadata_link_refused"):reason;return;
      }
    }
    const bool v2=root.value(QStringLiteral("export_version")).toInt(1)>=2;
    if(v2 && (root.value(QStringLiteral("current_frame"))!=frame || root.value(QStringLiteral("current_unit"))!=outputUnit ||
        root.value(QStringLiteral("transform_applied"))!=true ||
        root.value(QStringLiteral("applied_transform")).toObject().value(QStringLiteral("applied"))!=true ||
        !validSimilarity(root.value(QStringLiteral("applied_transform")).toObject()))) {
      fail("camera_scale_invalid_applied_transform");return;
    }
    const auto outputMetric=unitFromKey(outputUnit);
    if(v2 && ratio && outputMetric!=SceneLengthUnit::Unknown) ratio=metresPerUnit(outputMetric);
    else if ((ratio && frame == QStringLiteral("canonical") && outputUnit != QStringLiteral("m")) ||
        (ratio && frame == QStringLiteral("source") && outputUnit != metadata.sourceUnit) ||
        (ratio && frame==QStringLiteral("training") && outputUnit!=QStringLiteral("training")) ||
        (!ratio && outputMetric != SceneLengthUnit::Unknown)) {
      fail("camera_scale_conflicting_units"); return;
    }
  }
  if (metadata.unitDeclared && (!ratio ||
      std::abs(metresPerUnit(metadata.unit) - *ratio) > 1e-12 * std::max(1., *ratio))) {
    fail("camera_scale_conflicting_units"); return;
  }
  metadata.metersPerCoordinate = ratio;
  metadata.unitDeclared = ratio.has_value();
  metadata.unit = SceneLengthUnit::Unknown;
  if (ratio) {
    for (const auto unit : {SceneLengthUnit::Millimetres, SceneLengthUnit::Centimetres, SceneLengthUnit::Metres})
      if (std::abs(*ratio - metresPerUnit(unit)) <= 1e-12 * std::max(1., *ratio)) metadata.unit = unit;
  }
  metadata.provenance = QJsonObject{{QStringLiteral("schema"), schema},
      {QStringLiteral("sidecar"), QFileInfo(path).fileName()}, {QStringLiteral("ply_sha256"), expected},
      {QStringLiteral("coordinate_frame"), frame}, {QStringLiteral("scale"), scale},
      {QStringLiteral("transforms"), transforms}, {QStringLiteral("input_data_identity"), metadata.dataIdentity}};
  metadata.provenance.insert(QStringLiteral("training_contract"),contract);
  metadata.provenance.insert(QStringLiteral("native_hash_verified"),true);
  metadata.provenance.insert(QStringLiteral("coordinate_unit"),training?QStringLiteral("training"):root.value(QStringLiteral("output_unit")));
  if (exported) {
    metadata.provenance.insert(QStringLiteral("applied_transform"), root.value(QStringLiteral("applied_transform")));
    metadata.provenance.insert(QStringLiteral("input_sha256"), root.value(QStringLiteral("input_sha256")));
    metadata.provenance.insert(QStringLiteral("output_unit"), root.value(QStringLiteral("output_unit")));
    if(root.value(QStringLiteral("export_version"))==3) {
      for(const auto &key:{QStringLiteral("export_version"),QStringLiteral("canonical_scene_transform"),
          QStringLiteral("input_canonical_scene_transform"),QStringLiteral("scene_transform"),QStringLiteral("scene_scale_multiplier"),
          QStringLiteral("physical_scale_modified"),QStringLiteral("scale_evidence_scope"),QStringLiteral("scene_edit_independent_validation"),
          QStringLiteral("full_optimizer_state_transformed")})if(root.contains(key))metadata.provenance.insert(key,root.value(key));
    }
  }
}

QString conciseNumber(const double value, const int significantDigits = 12) {
  if (!std::isfinite(value)) {
    return QStringLiteral("-");
  }
  return QString::number(value, 'g', significantDigits);
}

QString csvEscape(QString value) {
  value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  return QStringLiteral("\"%1\"").arg(value);
}

} // namespace

std::optional<RecordedFrameTransforms> recordedExportFrameTransforms(const QJsonObject &record,QString *error) {
  const auto fail=[&](const char *reason)->std::optional<RecordedFrameTransforms>{if(error)*error=QString::fromLatin1(reason);return std::nullopt;};
  if(error)error->clear();
  const auto root=record.value(QStringLiteral("training_contract")).toObject();
  const auto chain=root.value(QStringLiteral("transforms")).toObject();
  const auto source=chain.value(QStringLiteral("source_to_canonical")).toObject();
  const auto normalization=chain.value(QStringLiteral("canonical_to_training")).toObject();
  if(record.value(QStringLiteral("schema"))!=QStringLiteral("gsw.coordinate_export/1")||record.value(QStringLiteral("export_version"))!=3||
      root.value(QStringLiteral("schema"))!=QStringLiteral("gsw.training_camera_contract/1")||
      record.value(QStringLiteral("input_data_identity"))!=root.value(QStringLiteral("input_data_identity"))||
      !validSimilarity(source)||!validSimilarity(normalization)||source.value(QStringLiteral("applied"))!=true||normalization.value(QStringLiteral("applied"))!=true||
      source.value(QStringLiteral("input_frame")).toString(QStringLiteral("source"))!=QStringLiteral("source")||
      source.value(QStringLiteral("output_frame")).toString(QStringLiteral("canonical"))!=QStringLiteral("canonical")||
      normalization.value(QStringLiteral("input_frame")).toString(QStringLiteral("canonical"))!=QStringLiteral("canonical")||
      normalization.value(QStringLiteral("output_frame")).toString(QStringLiteral("training"))!=QStringLiteral("training")||
      (source.contains(QStringLiteral("inverse"))&&!validRecordedInverse(source))||
      (normalization.contains(QStringLiteral("inverse"))&&!validRecordedInverse(normalization)))return fail("camera_scale_invalid_v3_contract");
  const auto scale=root.value(QStringLiteral("scale")).toObject();
  const QString status=scale.value(QStringLiteral("status")).toString();
  const bool known=status!=QStringLiteral("arbitrary");
  const double meters=scale.value(QStringLiteral("meters_per_unit")).toDouble(-1.);
  if(!QStringList{QStringLiteral("arbitrary"),QStringLiteral("source_declared"),QStringLiteral("constraint_derived"),QStringLiteral("independently_checked")}.contains(status)||
      (known&&(!std::isfinite(meters)||meters<=0.||scale.value(QStringLiteral("canonical_unit"))!=QStringLiteral("m")||
          std::abs(source.value(QStringLiteral("s")).toDouble()-meters)>1e-12*std::abs(meters)))||
      (!known&&(scale.value(QStringLiteral("meters_per_unit")).isDouble()||unitFromKey(scale.value(QStringLiteral("source_unit")).toString())!=SceneLengthUnit::Unknown||
          unitFromKey(scale.value(QStringLiteral("canonical_unit")).toString())!=SceneLengthUnit::Unknown)))return fail("camera_scale_conflicting_units");
  const QString beforeFrame=record.value(QStringLiteral("input_frame")).toString(),beforeUnit=record.value(QStringLiteral("input_unit")).toString();
  const QString afterFrame=record.value(QStringLiteral("output_frame")).toString(),afterUnit=record.value(QStringLiteral("output_unit")).toString();
  DisplaySimilarity before,after;
  if(record.value(QStringLiteral("current_frame"))!=afterFrame||record.value(QStringLiteral("current_unit"))!=afterUnit||
      record.value(QStringLiteral("transform_applied"))!=true||!frameToCanonical(root,beforeFrame,beforeUnit,before)||
      !frameToCanonical(root,afterFrame,afterUnit,after))return fail("camera_scale_invalid_current_frame");
  const auto oldJson=record.value(QStringLiteral("input_canonical_scene_transform")).toObject();
  const auto newJson=record.value(QStringLiteral("canonical_scene_transform")).toObject();
  for(const auto &value:{oldJson,newJson})if(!validSimilarity(value)||value.value(QStringLiteral("input_frame"))!=QStringLiteral("canonical")||
      value.value(QStringLiteral("output_frame"))!=QStringLiteral("canonical")||value.value(QStringLiteral("applied"))!=true)return fail("camera_scale_invalid_scene_history");
  const auto oldScene=displaySimilarity(oldJson),newScene=displaySimilarity(newJson);
  auto expectedScene=oldScene;
  if(record.contains(QStringLiteral("scene_transform"))) {
    const auto request=record.value(QStringLiteral("scene_transform")).toObject();
    const auto edit=request.value(QStringLiteral("transform")).toObject();
    if(request.size()!=4||request.value(QStringLiteral("schema"))!=QStringLiteral("gsw.scene_transform/1")||
        request.value(QStringLiteral("frame"))!=beforeFrame||request.value(QStringLiteral("unit"))!=beforeUnit||edit.size()!=3||!validSimilarity(edit))return fail("camera_scale_invalid_scene_transform");
    expectedScene=composeSimilarity(before,composeSimilarity(displaySimilarity(edit),composeSimilarity(inverseSimilarity(before),oldScene)));
  }
  if(!similaritiesClose(newScene,expectedScene)||!record.value(QStringLiteral("scene_scale_multiplier")).isDouble()||
      std::abs(record.value(QStringLiteral("scene_scale_multiplier")).toDouble()-newScene.s)>1e-12*std::abs(newScene.s)||
      record.value(QStringLiteral("physical_scale_modified"))!=QJsonValue(std::abs(newScene.s-1.)>1e-12)||
      record.value(QStringLiteral("scale_evidence_scope"))!=QStringLiteral("immutable_training_contract")||
      record.value(QStringLiteral("scene_edit_independent_validation"))!=false||record.value(QStringLiteral("full_optimizer_state_transformed"))!=false)return fail("camera_scale_invalid_scene_history");
  const auto applied=record.value(QStringLiteral("applied_transform")).toObject();
  const auto expected=composeSimilarity(inverseSimilarity(after),composeSimilarity(newScene,composeSimilarity(inverseSimilarity(oldScene),before)));
  if(!validSimilarity(applied)||applied.value(QStringLiteral("applied"))!=true||applied.value(QStringLiteral("input_frame"))!=beforeFrame||
      applied.value(QStringLiteral("output_frame"))!=afterFrame||!similaritiesClose(displaySimilarity(applied),expected))return fail("camera_scale_applied_chain_mismatch");
  RecordedFrameTransforms result;
  result.currentToCanonical=similarityJson(after);
  result.trainingToCurrent=similarityJson(composeSimilarity(inverseSimilarity(after),composeSimilarity(newScene,inverseSimilarity(displaySimilarity(normalization)))));
  result.canonicalSceneTransform=newJson;result.inputCanonicalSceneTransform=oldJson;result.trainingContract=root;
  result.sceneScaleMultiplier=newScene.s;result.physicalScaleModified=std::abs(newScene.s-1.)>1e-12;
  return result;
}

bool validateRecordedExportFiles(const QJsonObject &record,const QString &modelPath,QString *error) {
  const auto fail=[&](const char *reason){if(error)*error=QString::fromLatin1(reason);return false;};
  if(error)error->clear();
  static const QRegularExpression digestPattern(QStringLiteral("^[0-9a-f]{64}$"));
  const auto digest=[](const QByteArray &bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());};
  const QString modelHash=record.value(QStringLiteral("ply_sha256")).toString();
  if(!digestPattern.match(modelHash).hasMatch()||fileDigest(modelPath)!=modelHash||record.value(QStringLiteral("output_sha256"))!=modelHash)return fail("camera_model_hash_mismatch");
  const auto assets=record.value(QStringLiteral("assets")).toArray();
  const QString assetsHash=record.value(QStringLiteral("assets_sha256")).toString();
  if(!record.value(QStringLiteral("assets")).isArray()||!digestPattern.match(assetsHash).hasMatch()||
      digest(QJsonDocument(assets).toJson(QJsonDocument::Compact))!=assetsHash)return fail("camera_assets_manifest_mismatch");
  const QDir directory=QFileInfo(modelPath).absoluteDir();QSet<QString> paths;
  for(const auto &value:assets) {
    const auto asset=value.toObject();const QString relative=asset.value(QStringLiteral("path")).toString();
    const QString hash=asset.value(QStringLiteral("sha256")).toString();const auto count=asset.value(QStringLiteral("bytes"));
    const double bytes=count.toDouble(-1.);const QString path=directory.filePath(relative);
    if(!value.isObject()||asset.size()!=5||asset.value(QStringLiteral("kind"))!=QStringLiteral("texture_file")||
        !safeAssetReference(relative)||!safeAssetReference(asset.value(QStringLiteral("source_reference")).toString())||paths.contains(relative)||
        !count.isDouble()||!std::isfinite(bytes)||bytes<0.||std::floor(bytes)!=bytes||bytes>9007199254740991.||
        !QFileInfo(path).isFile()||QFileInfo(path).size()!=bytes||!digestPattern.match(hash).hasMatch()||fileDigest(path)!=hash)return fail("camera_asset_hash_or_path_mismatch");
    paths.insert(relative);
  }
  // The manifest binds the actual PLY TextureFile sequence, not merely a set
  // of unrelated valid files next to a model with unverified references.
  QFile model(modelPath);if(!model.open(QIODevice::ReadOnly))return fail("camera_asset_header_invalid");
  QStringList textureReferences;bool ended=false;qint64 headerBytes=0;
  static const QRegularExpression texture(QStringLiteral("^TextureFile\\s+(.+)$"),QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression external(QStringLiteral("^(TextureFile|Texture|MaterialFile|MaterialLibrary|mtllib|map_Kd)\\b"),QRegularExpression::CaseInsensitiveOption);
  while(!model.atEnd()&&headerBytes<=16LL*1024*1024) {
    const auto line=model.readLine(1024*1024);headerBytes+=line.size();
    if(line.isEmpty()||headerBytes>16LL*1024*1024||std::any_of(line.cbegin(),line.cend(),[](char c){return static_cast<unsigned char>(c)>=128;}))return fail("camera_asset_header_invalid");
    const auto text=QString::fromLatin1(line).trimmed();if(text==QStringLiteral("end_header")){ended=true;break;}
    if(!text.startsWith(QStringLiteral("comment ")))continue;
    const auto comment=text.mid(8);const auto match=texture.match(comment);
    if(match.hasMatch())textureReferences.append(match.captured(1).trimmed());
    else if(external.match(comment).hasMatch())return fail("camera_asset_reference_mismatch");
  }
  if(!ended||textureReferences.size()!=assets.size())return fail("camera_asset_reference_mismatch");
  for(qsizetype index=0;index<assets.size();++index)if(textureReferences[index]!=assets[index].toObject().value(QStringLiteral("path")).toString())return fail("camera_asset_reference_mismatch");
  const auto reference=record.value(QStringLiteral("camera_reference")).toObject();
  const QString relative=reference.value(QStringLiteral("path")).toString(),cameraHash=reference.value(QStringLiteral("sha256")).toString();
  if(!safeAssetReference(relative)||relative.contains('/')||reference.value(QStringLiteral("schema"))!=QStringLiteral("gsw.model_cameras/1")||
      !digestPattern.match(cameraHash).hasMatch()||fileDigest(directory.filePath(relative))!=cameraHash)return fail("camera_reference_hash_mismatch");
  const QJsonObject binding{{QStringLiteral("assets_sha256"),assetsHash},{QStringLiteral("camera_sha256"),cameraHash},
      {QStringLiteral("coordinate_frame"),record.value(QStringLiteral("current_frame"))},
      {QStringLiteral("coordinate_unit"),record.value(QStringLiteral("current_unit"))},
      {QStringLiteral("input_data_identity"),record.value(QStringLiteral("input_data_identity"))},{QStringLiteral("model_sha256"),modelHash}};
  if(record.value(QStringLiteral("bundle_sha256"))!=digest(QJsonDocument(binding).toJson(QJsonDocument::Compact)))return fail("camera_bundle_binding_mismatch");
  return true;
}

bool SceneCoordinate3D::isFinite() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

SceneCoordinate3D SceneCoordinateInfo::globalCenter() const {
  return {(globalMinimum.x + globalMaximum.x) * 0.5,
          (globalMinimum.y + globalMaximum.y) * 0.5,
          (globalMinimum.z + globalMaximum.z) * 0.5};
}

SceneCoordinate3D SceneCoordinateInfo::globalSize() const {
  return {globalMaximum.x - globalMinimum.x,
          globalMaximum.y - globalMinimum.y,
          globalMaximum.z - globalMinimum.z};
}

double SceneCoordinateInfo::globalDiagonal() const {
  const SceneCoordinate3D size = globalSize();
  return std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z);
}

QVector3D SceneCoordinateInfo::localFromGlobal(
    const SceneCoordinate3D &value) const {
  return QVector3D(static_cast<float>((value.x + displayShift.x) * displayScale),
                   static_cast<float>((value.y + displayShift.y) * displayScale),
                   static_cast<float>((value.z + displayShift.z) * displayScale));
}

SceneCoordinate3D SceneCoordinateInfo::globalFromLocal(
    const QVector3D &value) const {
  const double inverseScale = displayScale == 0.0 ? 1.0 : 1.0 / displayScale;
  return {static_cast<double>(value.x()) * inverseScale - displayShift.x,
          static_cast<double>(value.y()) * inverseScale - displayShift.y,
          static_cast<double>(value.z()) * inverseScale - displayShift.z};
}

QVector3D SceneCoordinateInfo::localMinimum() const {
  return localFromGlobal(globalMinimum);
}

QVector3D SceneCoordinateInfo::localMaximum() const {
  return localFromGlobal(globalMaximum);
}

QVector3D SceneCoordinateInfo::localCenter() const {
  return localFromGlobal(globalCenter());
}

SceneCoordinateTracker::SceneCoordinateTracker(
    const SceneCoordinateMetadata &metadata,
    const double automaticShiftThreshold)
    : mAutomaticShiftThreshold(std::max(1.0, automaticShiftThreshold)) {
  mInfo.unit = metadata.unit;
  mInfo.unitDeclared = metadata.unitDeclared;
  mInfo.coordinateReferenceSystem = metadata.coordinateReferenceSystem;
  mInfo.sourceUsesFloat64 = metadata.sourceUsesFloat64;
  mInfo.metersPerCoordinate = metadata.metersPerCoordinate;
  mInfo.scaleStatus = metadata.scaleStatus;
  mInfo.sourceUnit = metadata.sourceUnit;
  mInfo.dataIdentity = metadata.dataIdentity;
  mInfo.provenance = metadata.provenance;
  mInfo.validationError = metadata.validationError;
}

bool SceneCoordinateTracker::observeAndMap(const double x, const double y,
                                           const double z,
                                           QVector3D &localPosition) {
  const SceneCoordinate3D global{x, y, z};
  if (!global.isFinite()) {
    return false;
  }
  if (!mInfo.valid) {
    mInfo.valid = true;
    mInfo.globalMinimum = global;
    mInfo.globalMaximum = global;
    const double largestMagnitude =
        std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (largestMagnitude >= mAutomaticShiftThreshold) {
      mInfo.displayShift = {-x, -y, -z};
      mInfo.automaticDisplayShift = true;
    }
  } else {
    mInfo.globalMinimum.x = std::min(mInfo.globalMinimum.x, x);
    mInfo.globalMinimum.y = std::min(mInfo.globalMinimum.y, y);
    mInfo.globalMinimum.z = std::min(mInfo.globalMinimum.z, z);
    mInfo.globalMaximum.x = std::max(mInfo.globalMaximum.x, x);
    mInfo.globalMaximum.y = std::max(mInfo.globalMaximum.y, y);
    mInfo.globalMaximum.z = std::max(mInfo.globalMaximum.z, z);
  }
  localPosition = mInfo.localFromGlobal(global);
  return std::isfinite(localPosition.x()) && std::isfinite(localPosition.y()) &&
         std::isfinite(localPosition.z());
}

SceneCoordinateMetadata parsePlyCoordinateMetadata(
    const QStringList &headerMetadata, const QString &sourcePath,
    const bool sourceUsesFloat64) {
  SceneCoordinateMetadata result;
  result.sourceUsesFloat64 = sourceUsesFloat64;
  static const QRegularExpression unitExpression(
      QStringLiteral(
          R"((?:^|\b)(?:coordinate[_ ]?unit|units?)\s*[:=]?\s*(mm|millimet(?:er|re)s?|cm|centimet(?:er|re)s?|m|met(?:er|re)s?)\b)"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression epsgExpression(
      QStringLiteral(R"(\bEPSG\s*[:=]?\s*(\d{3,8})\b)"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression crsExpression(
      QStringLiteral(R"((?:^|\b)(?:crs|coordinate[_ ]?system)\s*[:=]\s*(.+)$)"),
      QRegularExpression::CaseInsensitiveOption);

  for (const QString &line : headerMetadata) {
    const QRegularExpressionMatch unitMatch = unitExpression.match(line);
    if (!result.unitDeclared && unitMatch.hasMatch()) {
      result.unit = unitFromKey(unitMatch.captured(1));
      result.unitDeclared = result.unit != SceneLengthUnit::Unknown;
    }
    if (result.coordinateReferenceSystem.isEmpty()) {
      const QRegularExpressionMatch epsgMatch = epsgExpression.match(line);
      if (epsgMatch.hasMatch()) {
        result.coordinateReferenceSystem =
            QStringLiteral("EPSG:%1").arg(epsgMatch.captured(1));
      } else {
        const QRegularExpressionMatch crsMatch = crsExpression.match(line);
        if (crsMatch.hasMatch()) {
          result.coordinateReferenceSystem = crsMatch.captured(1).trimmed();
        }
      }
    }
  }

  const QFileInfo source(sourcePath);
  const QString prjPath = source.dir().filePath(source.completeBaseName() +
                                                QStringLiteral(".prj"));
  QFile prj(prjPath);
  if (prj.open(QIODevice::ReadOnly) && prj.size() <= 1024 * 1024) {
    const QString projection = QString::fromUtf8(prj.readAll()).simplified();
    if (result.coordinateReferenceSystem.isEmpty()) {
      result.coordinateReferenceSystem =
          projection;
    }
    if (!result.unitDeclared) {
      static const QRegularExpression projectionUnitExpression(
          QStringLiteral(
              R"regex((?:LENGTHUNIT|UNIT)\s*\[\s*"(millimet(?:er|re)s?|centimet(?:er|re)s?|met(?:er|re)s?)")regex"),
          QRegularExpression::CaseInsensitiveOption);
      const QRegularExpressionMatch unitMatch =
          projectionUnitExpression.match(projection);
      if (unitMatch.hasMatch()) {
        result.unit = unitFromKey(unitMatch.captured(1));
        result.unitDeclared = result.unit != SceneLengthUnit::Unknown;
      }
    }
  }
  readCameraScaleSidecar(sourcePath, result);
  return result;
}

QString sceneLengthUnitSymbol(const SceneLengthUnit unit) {
  switch (unit) {
  case SceneLengthUnit::Millimetres:
    return QStringLiteral("mm");
  case SceneLengthUnit::Centimetres:
    return QStringLiteral("cm");
  case SceneLengthUnit::Metres:
    return QStringLiteral("m");
  case SceneLengthUnit::Unknown:
    return QStringLiteral("u");
  }
  return QStringLiteral("u");
}

SceneDisplayMapping sceneDisplayMapping(const SceneCoordinateInfo &from,
                                        const SceneCoordinateInfo &reference) {
  SceneDisplayMapping result;
  result.reason=QStringLiteral("coordinate_metadata_missing");
  if (!from.valid || !reference.valid) return result;
  const auto a = sceneMetresPerCoordinate(from), b = sceneMetresPerCoordinate(reference);
  const double ratio = a && b ? *a / *b : 1.0;
  DisplaySimilarity source,target,relative;relative.s=ratio;
  QJsonObject sourceLineage,targetLineage;
  const bool sourceVerified=displayFrameContext(from,source,sourceLineage);
  const bool targetVerified=displayFrameContext(reference,target,targetLineage);
  if(sourceVerified && targetVerified) {
    if(from.dataIdentity.toLower()!=reference.dataIdentity.toLower())result.reason=QStringLiteral("data_identity_mismatch");
    else if(sourceLineage!=targetLineage)result.reason=QStringLiteral("canonical_lineage_mismatch");
    else {
      relative.s=source.s/target.s;
      for(int i=0;i<3;++i) {
        relative.t[i]=0.;
        for(int j=0;j<3;++j) {
          relative.r[i][j]=0.;
          for(int k=0;k<3;++k)relative.r[i][j]+=target.r[k][i]*source.r[k][j];
          relative.t[i]+=target.r[j][i]*(source.t[j]-target.t[j])/target.s;
        }
      }
      result.verifiedAlignment=true;result.reason=QStringLiteral("verified_common_canonical");
    }
  }
  const double sourceShift[3]{from.displayShift.x,from.displayShift.y,from.displayShift.z};
  const double targetShift[3]{reference.displayShift.x,reference.displayShift.y,reference.displayShift.z};
  result.lengthScale=relative.s*reference.displayScale/from.displayScale;
  // Cancel physical origins/normalization translations in double. Only the
  // small local-to-local result is narrowed for the GPU's float matrices.
  for(int i=0;i<3;++i) {
    double offset=relative.t[i]+targetShift[i];
    for(int j=0;j<3;++j) {
      const double value=result.lengthScale*relative.r[i][j];
      result.mLinear[3*i+j]=relative.s*relative.r[i][j];
      offset-=relative.s*relative.r[i][j]*sourceShift[j];
      if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())return {};
      result.localToReference(i,j)=static_cast<float>(value);
    }
    offset*=reference.displayScale;
    if(!std::isfinite(offset)||std::abs(offset)>std::numeric_limits<float>::max())return {};
    result.localToReference(i,3)=static_cast<float>(offset);
  }
  result.mTranslation={relative.t[0],relative.t[1],relative.t[2]};
  return result;
}

SceneCoordinate3D SceneDisplayMapping::mapCoordinate(const SceneCoordinate3D &value) const {
  const double p[3]{value.x,value.y,value.z};double q[3]{mTranslation.x,mTranslation.y,mTranslation.z};
  for(int i=0;i<3;++i)for(int j=0;j<3;++j)q[i]+=mLinear[3*i+j]*p[j];
  return {q[0],q[1],q[2]};
}

QString sceneLengthUnitDescription(const SceneCoordinateInfo &coordinates) {
  const auto base=[&]() -> QString {
  if (coordinates.metersPerCoordinate && sceneMetresPerCoordinate(coordinates)) {
    QString status = QCoreApplication::translate("Workbench", "来源声明（不是独立检核）");
    if (coordinates.scaleStatus == QStringLiteral("constraint_derived"))
      status = QCoreApplication::translate("Workbench", "约束求得（保留外部证据）");
    else if (coordinates.scaleStatus == QStringLiteral("independently_checked"))
      status = QCoreApplication::translate("Workbench", "来源声称独立检核（保留外部证据）");
    return QCoreApplication::translate("Workbench", "每坐标单位 %1 m · %2")
        .arg(conciseNumber(*coordinates.metersPerCoordinate), status);
  }
  if (!sceneMetresPerCoordinate(coordinates)) {
    return QCoreApplication::translate("Workbench", "未声明（原始单位 u）");
  }
  return sceneLengthUnitSymbol(coordinates.unit);
  };
  QString result=base();
  const auto edit=coordinates.provenance.value(QStringLiteral("canonical_scene_transform")).toObject();
  if(validSimilarity(edit)&&!similaritiesClose(displaySimilarity(edit),DisplaySimilarity{}))
    result+=QLatin1Char('\n')+QCoreApplication::translate("Workbench", "已应用用户场景编辑（尺度倍率 %1）；原尺度证据仅适用于未编辑的训练合同，不代表编辑后模型的独立精度。")
        .arg(conciseNumber(edit.value(QStringLiteral("s")).toDouble()));
  return result;
}

std::optional<double> sceneMetresPerCoordinate(const SceneCoordinateInfo &coordinates) {
  if (!coordinates.unitDeclared || coordinates.scaleStatus == QStringLiteral("arbitrary")) return std::nullopt;
  if (coordinates.metersPerCoordinate) {
    const double ratio = *coordinates.metersPerCoordinate;
    return std::isfinite(ratio) && ratio > 0. ? std::optional<double>(ratio) : std::nullopt;
  }
  return coordinates.unit == SceneLengthUnit::Unknown ? std::nullopt :
      std::optional<double>(metresPerUnit(coordinates.unit));
}

QString formatSceneCoordinate(const double value,
                              const SceneCoordinateInfo &coordinates) {
  if (coordinates.metersPerCoordinate && sceneMetresPerCoordinate(coordinates))
    return QStringLiteral("%1 m").arg(conciseNumber(value * *coordinates.metersPerCoordinate));
  if (!sceneMetresPerCoordinate(coordinates))
    return QStringLiteral("%1 u").arg(conciseNumber(value));
  return QStringLiteral("%1 %2")
      .arg(conciseNumber(value), sceneLengthUnitSymbol(coordinates.unit));
}

QString formatSceneLength(const double value,
                          const SceneCoordinateInfo &coordinates) {
  const auto ratio = sceneMetresPerCoordinate(coordinates);
  if (!ratio) {
    return QStringLiteral("%1 u").arg(conciseNumber(value, 7));
  }
  const double metres = value * *ratio;
  if (std::abs(metres) < 0.01) {
    return QStringLiteral("%1 mm").arg(conciseNumber(metres * 1000.0, 6));
  }
  if (std::abs(metres) < 1.0) {
    return QStringLiteral("%1 cm").arg(conciseNumber(metres * 100.0, 6));
  }
  return QStringLiteral("%1 m").arg(conciseNumber(metres, 7));
}

QString formatSceneVector(const SceneCoordinate3D &value,
                          const SceneCoordinateInfo &coordinates) {
  if (coordinates.metersPerCoordinate && sceneMetresPerCoordinate(coordinates))
    return QStringLiteral("X %1  Y %2  Z %3 m")
        .arg(conciseNumber(value.x * *coordinates.metersPerCoordinate),
             conciseNumber(value.y * *coordinates.metersPerCoordinate),
             conciseNumber(value.z * *coordinates.metersPerCoordinate));
  const QString symbol = sceneMetresPerCoordinate(coordinates) ? sceneLengthUnitSymbol(coordinates.unit) : QStringLiteral("u");
  return QStringLiteral("X %1  Y %2  Z %3 %4")
      .arg(conciseNumber(value.x), conciseNumber(value.y),
           conciseNumber(value.z), symbol);
}

QString formatSceneSize(const SceneCoordinateInfo &coordinates) {
  const SceneCoordinate3D size = coordinates.globalSize();
  if (coordinates.metersPerCoordinate && sceneMetresPerCoordinate(coordinates))
    return QStringLiteral("%1 × %2 × %3 m")
        .arg(conciseNumber(size.x * *coordinates.metersPerCoordinate),
             conciseNumber(size.y * *coordinates.metersPerCoordinate),
             conciseNumber(size.z * *coordinates.metersPerCoordinate));
  const QString symbol = sceneMetresPerCoordinate(coordinates) ? sceneLengthUnitSymbol(coordinates.unit) : QStringLiteral("u");
  return QStringLiteral("%1 × %2 × %3 %4")
      .arg(conciseNumber(size.x), conciseNumber(size.y), conciseNumber(size.z),
           symbol);
}

QJsonObject sceneCoordinateInfoToJson(
    const SceneCoordinateInfo &coordinates) {
  QJsonObject object;
  object.insert(QStringLiteral("valid"), coordinates.valid);
  object.insert(QStringLiteral("globalMinimum"),
                vectorToJson(coordinates.globalMinimum));
  object.insert(QStringLiteral("globalMaximum"),
                vectorToJson(coordinates.globalMaximum));
  object.insert(QStringLiteral("displayShift"),
                vectorToJson(coordinates.displayShift));
  object.insert(QStringLiteral("displayScale"), coordinates.displayScale);
  object.insert(QStringLiteral("unit"), unitKey(coordinates.unit));
  object.insert(QStringLiteral("unitDeclared"), coordinates.unitDeclared);
  object.insert(QStringLiteral("coordinateReferenceSystem"),
                coordinates.coordinateReferenceSystem);
  object.insert(QStringLiteral("sourceUsesFloat64"),
                coordinates.sourceUsesFloat64);
  object.insert(QStringLiteral("automaticDisplayShift"),
                coordinates.automaticDisplayShift);
  if (coordinates.metersPerCoordinate) object.insert(QStringLiteral("metersPerCoordinate"), *coordinates.metersPerCoordinate);
  object.insert(QStringLiteral("scaleStatus"), coordinates.scaleStatus);
  object.insert(QStringLiteral("sourceUnit"), coordinates.sourceUnit);
  object.insert(QStringLiteral("dataIdentity"), coordinates.dataIdentity);
  object.insert(QStringLiteral("provenance"), coordinates.provenance);
  object.insert(QStringLiteral("validationError"), coordinates.validationError);
  return object;
}

bool sceneCoordinateInfoFromJson(const QJsonObject &object,
                                 SceneCoordinateInfo &coordinates) {
  SceneCoordinateInfo parsed;
  parsed.valid = object.value(QStringLiteral("valid")).toBool(false);
  if (!parsed.valid ||
      !vectorFromJson(object.value(QStringLiteral("globalMinimum")),
                      parsed.globalMinimum) ||
      !vectorFromJson(object.value(QStringLiteral("globalMaximum")),
                      parsed.globalMaximum) ||
      !vectorFromJson(object.value(QStringLiteral("displayShift")),
                      parsed.displayShift)) {
    return false;
  }
  parsed.displayScale = object.value(QStringLiteral("displayScale")).toDouble();
  parsed.unit = unitFromKey(object.value(QStringLiteral("unit")).toString());
  parsed.unitDeclared =
      object.value(QStringLiteral("unitDeclared")).toBool(false);
  parsed.coordinateReferenceSystem =
      object.value(QStringLiteral("coordinateReferenceSystem")).toString();
  parsed.sourceUsesFloat64 =
      object.value(QStringLiteral("sourceUsesFloat64")).toBool(false);
  parsed.automaticDisplayShift =
      object.value(QStringLiteral("automaticDisplayShift")).toBool(false);
  if (object.contains(QStringLiteral("metersPerCoordinate"))) {
    const auto ratio = object.value(QStringLiteral("metersPerCoordinate"));
    if (!ratio.isDouble() || !std::isfinite(ratio.toDouble()) || ratio.toDouble() <= 0.) return false;
    parsed.metersPerCoordinate = ratio.toDouble();
  }
  parsed.scaleStatus = object.value(QStringLiteral("scaleStatus")).toString();
  parsed.sourceUnit = object.value(QStringLiteral("sourceUnit")).toString();
  parsed.dataIdentity = object.value(QStringLiteral("dataIdentity")).toString();
  parsed.provenance = object.value(QStringLiteral("provenance")).toObject();
  // Cached metadata is not a current checksum observation. The viewport
  // revalidates the PLY/sidecar before using a cached full-frame mapping.
  parsed.provenance.remove(QStringLiteral("native_hash_verified"));
  parsed.validationError = object.value(QStringLiteral("validationError")).toString();
  if (parsed.metersPerCoordinate && (!parsed.unitDeclared || parsed.scaleStatus == QStringLiteral("arbitrary"))) return false;
  if (parsed.metersPerCoordinate && parsed.unit != SceneLengthUnit::Unknown &&
      std::abs(*parsed.metersPerCoordinate - metresPerUnit(parsed.unit)) >
        1e-12 * std::max(1., *parsed.metersPerCoordinate)) return false;
  if (parsed.scaleStatus == QStringLiteral("arbitrary") &&
      (parsed.unitDeclared || parsed.unit != SceneLengthUnit::Unknown)) return false;
  if (!std::isfinite(parsed.displayScale) || parsed.displayScale <= 0.0 ||
      parsed.globalMinimum.x > parsed.globalMaximum.x ||
      parsed.globalMinimum.y > parsed.globalMaximum.y ||
      parsed.globalMinimum.z > parsed.globalMaximum.z) {
    return false;
  }
  coordinates = parsed;
  return true;
}

bool writeSceneCoordinateReport(
    const QString &destinationPath, const QString &sourcePath,
    const SceneCoordinateInfo &coordinates,
    const double referencePlaneElevation, const QString &referencePlaneMode,
    QString *errorMessage) {
  QString error;
  if (!coordinates.valid) {
    error = QCoreApplication::translate("Workbench", "当前场景没有可导出的坐标范围。");
  }
  QSaveFile file(destinationPath);
  if (error.isEmpty() && !file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    error = QCoreApplication::translate("Workbench", "无法创建坐标报告：%1").arg(file.errorString());
  }

  const SceneCoordinate3D center = coordinates.globalCenter();
  const SceneCoordinate3D size = coordinates.globalSize();
  const bool csv = QFileInfo(destinationPath).suffix().compare(
                       QStringLiteral("csv"), Qt::CaseInsensitive) == 0;
  if (error.isEmpty() && csv) {
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "field,x,y,z,value,unit\n";
    stream << "minimum," << conciseNumber(coordinates.globalMinimum.x) << ','
           << conciseNumber(coordinates.globalMinimum.y) << ','
           << conciseNumber(coordinates.globalMinimum.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "maximum," << conciseNumber(coordinates.globalMaximum.x) << ','
           << conciseNumber(coordinates.globalMaximum.y) << ','
           << conciseNumber(coordinates.globalMaximum.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "center," << conciseNumber(center.x) << ','
           << conciseNumber(center.y) << ',' << conciseNumber(center.z)
           << ",," << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "size," << conciseNumber(size.x) << ','
           << conciseNumber(size.y) << ',' << conciseNumber(size.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "diagonal,,,," << conciseNumber(coordinates.globalDiagonal())
           << ',' << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "display_shift," << conciseNumber(coordinates.displayShift.x)
           << ',' << conciseNumber(coordinates.displayShift.y) << ','
           << conciseNumber(coordinates.displayShift.z) << ",,"
           << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "display_scale,,,," << conciseNumber(coordinates.displayScale)
           << ",\n";
    stream << "reference_plane,,,," << conciseNumber(referencePlaneElevation)
           << ',' << sceneLengthUnitSymbol(coordinates.unit) << '\n';
    stream << "reference_plane_mode,,,," << csvEscape(referencePlaneMode)
           << ",\n";
    stream << "source,,,," << csvEscape(QDir::toNativeSeparators(sourcePath))
           << ",\n";
    stream << "crs,,,," << csvEscape(coordinates.coordinateReferenceSystem)
           << ",\n";
    stream << "unit_declared,,,,"
           << (coordinates.unitDeclared ? "true" : "false") << ",\n";
    if (const auto ratio = sceneMetresPerCoordinate(coordinates))
      stream << "meters_per_coordinate,,,," << conciseNumber(*ratio) << ",m\n";
    stream << "scale_status,,,," << csvEscape(coordinates.scaleStatus) << ",\n";
    stream << "source_unit,,,," << csvEscape(coordinates.sourceUnit) << ",\n";
    stream << "data_identity,,,," << csvEscape(coordinates.dataIdentity) << ",\n";
    if (!coordinates.provenance.isEmpty()) {
      stream << "provenance_json,,,," << csvEscape(QString::fromUtf8(
          QJsonDocument(coordinates.provenance).toJson(QJsonDocument::Compact))) << ",\n";
    }
    stream.flush();
  } else if (error.isEmpty()) {
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("sourcePath"),
                QDir::toNativeSeparators(sourcePath));
    root.insert(QStringLiteral("coordinates"),
                sceneCoordinateInfoToJson(coordinates));
    root.insert(QStringLiteral("globalCenter"), vectorToJson(center));
    root.insert(QStringLiteral("globalSize"), vectorToJson(size));
    root.insert(QStringLiteral("globalDiagonal"),
                coordinates.globalDiagonal());
    if (const auto ratio = sceneMetresPerCoordinate(coordinates)) {
      root.insert(QStringLiteral("physicalDiagonalMetres"), coordinates.globalDiagonal() * *ratio);
      root.insert(QStringLiteral("physicalSizeMetres"), vectorToJson({size.x * *ratio, size.y * *ratio, size.z * *ratio}));
    }
    root.insert(QStringLiteral("displayTransformFormula"),
                QStringLiteral("local = (global + displayShift) * displayScale"));
    root.insert(QStringLiteral("referencePlaneMode"), referencePlaneMode);
    root.insert(QStringLiteral("referencePlaneElevation"),
                referencePlaneElevation);
    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
      error = QCoreApplication::translate("Workbench", "无法写入坐标报告：%1").arg(file.errorString());
    }
  }

  if (error.isEmpty() && !file.commit()) {
    error = QCoreApplication::translate("Workbench", "无法完成坐标报告：%1").arg(file.errorString());
  } else if (!error.isEmpty()) {
    file.cancelWriting();
  }
  if (errorMessage != nullptr) {
    *errorMessage = error;
  }
  return error.isEmpty();
}

} // namespace gsw
