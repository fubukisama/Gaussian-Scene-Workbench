#include "ModelExport.h"
#include "PlyPointCloudLoader.h"
#include "CameraTrajectory.h"
#include "SpzIO.h"
#include "RuntimeComponentEnvironment.h"

#include <QBuffer>
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtEndian>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {
QString cameraScaleMetadataPath(const QString &source) {
  const QFileInfo info(source);
  return info.dir().filePath(info.completeBaseName() + QStringLiteral(".camera-scale.json"));
}

CameraScaleRecoveryCapability cameraScaleRecoveryCapability(const QString &source) {
  CameraScaleRecoveryCapability capability;
  QFile file(cameraScaleMetadataPath(source));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024) return capability;
  const auto document = QJsonDocument::fromJson(file.readAll());
  const auto object = document.object();
  if (!document.isObject() || object.value(QStringLiteral("ply_sha256")).toString().size() != 64) return capability;
  const QString schema = object.value(QStringLiteral("schema")).toString();
  const bool restored = schema == QStringLiteral("gsw.coordinate_export/1");
  if (!restored && schema != QStringLiteral("gsw.training_camera_contract/1")) return capability;
  const auto verified = parsePlyCoordinateMetadata({},source,false);
  if (!verified.validationError.isEmpty() || verified.provenance.isEmpty()) {
    capability.validationError = verified.validationError;
    return capability;
  }
  if(restored && (object.contains(QStringLiteral("camera_reference")) ||
      object.value(QStringLiteral("export_version")).toInt()>=2)) {
    const auto cameras=CameraTrajectory::loadForScene(source);
    if(!cameras.metadataVerified() || !cameras.error().isEmpty()) {
      capability.validationError=QStringLiteral("camera_scale_export_camera_binding_invalid");
      return capability;
    }
  }
  QJsonObject contract = object;
  for (int depth=0; contract.value(QStringLiteral("schema")).toString()==QStringLiteral("gsw.coordinate_export/1") && depth<8; ++depth)
    contract=contract.value(QStringLiteral("training_contract")).toObject();
  if (contract.value(QStringLiteral("schema")).toString()!=QStringLiteral("gsw.training_camera_contract/1")) return capability;
  capability.currentFrame = restored ? object.value(QStringLiteral("output_frame")).toString() : QStringLiteral("training");
  capability.currentUnit = restored ? object.value(QStringLiteral("output_unit")).toString() : QStringLiteral("training");
  capability.scaleStatus = verified.scaleStatus;
  const auto transforms = contract.value(QStringLiteral("transforms")).toObject();
  for (const QString &name : {QStringLiteral("source_to_canonical"), QStringLiteral("canonical_to_training")}) {
    const auto transform = transforms.value(name).toObject();
    const double scale = transform.value(QStringLiteral("s")).toDouble(-1);
    const auto rotation = transform.value(QStringLiteral("R")).toArray();
    const auto translation = transform.value(QStringLiteral("t")).toArray();
    if (!std::isfinite(scale) || scale <= 0 || !transform.value(QStringLiteral("applied")).toBool() ||
        rotation.size() != 3 || translation.size() != 3) return capability;
    double r[3][3]{};
    for (int row = 0; row < 3; ++row) {
      const auto values = rotation[row].toArray();
      if (values.size() != 3 || !translation[row].isDouble() || !std::isfinite(translation[row].toDouble())) return capability;
      for (int column = 0; column < 3; ++column) {
        if (!values[column].isDouble() || !std::isfinite(values[column].toDouble())) return capability;
        r[row][column] = values[column].toDouble();
      }
    }
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
      double dot = 0;
      for (int k = 0; k < 3; ++k) dot += r[k][a] * r[k][b];
      if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 2e-5) return capability;
    }
    const double determinant = r[0][0] * (r[1][1]*r[2][2] - r[1][2]*r[2][1]) -
        r[0][1] * (r[1][0]*r[2][2] - r[1][2]*r[2][0]) + r[0][2] * (r[1][0]*r[2][1] - r[1][1]*r[2][0]);
    if (std::abs(determinant - 1.0) > 2e-5) return capability;
  }
  capability.source = true;
  capability.preserve = true;
  const auto scale = contract.value(QStringLiteral("scale")).toObject();
  capability.sourceUnit = scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("unknown"));
  const double meters = scale.value(QStringLiteral("meters_per_unit")).toDouble(-1);
  capability.metric = scale.value(QStringLiteral("canonical_unit")).toString() == QStringLiteral("m") &&
      scale.value(QStringLiteral("status")).toString() != QStringLiteral("arbitrary") &&
      std::isfinite(meters) && meters > 0 && !scale.value(QStringLiteral("evidence")).toArray().isEmpty();
  return capability;
}

QString verifiedTrainingDeliveryPath(const QString &outputDirectory, const int expectedIteration) {
  const QDir root(QFileInfo(outputDirectory).absoluteFilePath());
  QFile receipt(root.filePath(QStringLiteral("model-delivery.json")));
  if (!receipt.open(QIODevice::ReadOnly) || receipt.size()>16*1024*1024) return {};
  const auto record=QJsonDocument::fromJson(receipt.readAll()).object();
  if (record.value(QStringLiteral("schema"))!=QStringLiteral("gsw.model_delivery/1") ||
      record.value(QStringLiteral("training_status"))!=QStringLiteral("done") ||
      record.value(QStringLiteral("export_status"))!=QStringLiteral("ready") ||
      (expectedIteration>=0 && record.value(QStringLiteral("iteration")).toInt(-1)!=expectedIteration)) return {};
  const auto child=[&root](const QString &value)->QString {
    if(value.isEmpty() || QDir::fromNativeSeparators(value).split('/').contains(QStringLiteral(".."))) return {};
    const QString path=QDir::cleanPath(QDir::isAbsolutePath(value)?value:root.filePath(value));
    const QString relative=root.relativeFilePath(path);
    if(relative==QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) return {};
    QFileInfo current(path);
    while(current.absoluteFilePath()!=root.absolutePath()) {
      if(current.isSymbolicLink()) return {};
#ifdef Q_OS_WIN
      const DWORD attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.absoluteFilePath().utf16()));
      if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return {};
#endif
      const QString parent=current.absolutePath();
      if(parent==current.absoluteFilePath()) return {};
      current.setFile(parent);
    }
    return path;
  };
  const QString model=child(record.value(QStringLiteral("model_relative_path")).toString(
      record.value(QStringLiteral("model_path")).toString()));
  if(model.isEmpty() || !QFileInfo(model).isFile()) return {};
  const auto metadata=parsePlyCoordinateMetadata({},model,true);
  if(!metadata.validationError.isEmpty() || metadata.provenance.isEmpty() ||
      !metadata.metersPerCoordinate || *metadata.metersPerCoordinate!=1.) return {};
  const auto cameras=CameraTrajectory::loadForScene(model);
  if(!cameras.metadataVerified() || !cameras.error().isEmpty()) return {};
  QFile sidecar(cameraScaleMetadataPath(model));
  if(!sidecar.open(QIODevice::ReadOnly) || sidecar.size()>64*1024*1024) return {};
  const auto exported=QJsonDocument::fromJson(sidecar.readAll()).object();
  if(exported.value(QStringLiteral("output_frame"))!=QStringLiteral("canonical") ||
      exported.value(QStringLiteral("output_unit"))!=QStringLiteral("m")) return {};
  const auto digest=[](const QString &path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file)?QString::fromLatin1(hash.result().toHex()):QString();
  };
  const QString internal=child(record.value(QStringLiteral("internal_model_relative_path")).toString(
      record.value(QStringLiteral("internal_model_path")).toString()));
  const QString inputHash=exported.value(QStringLiteral("input_sha256")).toString();
  if(internal.isEmpty() || inputHash.size()!=64 || digest(internal)!=inputHash) return {};
  const auto camera=exported.value(QStringLiteral("camera_reference")).toObject();
  const QString cameraName=camera.value(QStringLiteral("path")).toString();
  if(cameraName.isEmpty() || QFileInfo(cameraName).fileName()!=cameraName) return {};
  const QString cameraPath=child(QFileInfo(model).dir().filePath(cameraName));
  if(cameraPath.isEmpty() || camera.value(QStringLiteral("sha256")).toString().size()!=64 ||
      digest(cameraPath)!=camera.value(QStringLiteral("sha256")).toString()) return {};
  return model;
}

SceneCoordinate3D exportPosition(const SceneCoordinate3D &p, const ModelExportOptions &o) {
  if (!o.applyTransform) return p;
  // Double-precision source coordinates; viewport recentering is never baked.
  const auto &q = o.transform.rotation;
  const double length = std::hypot(std::hypot(double(q.scalar()), double(q.x())),
                                   std::hypot(double(q.y()), double(q.z())));
  const double w = q.scalar() / length, x = q.x() / length,
               y = q.y() / length, z = q.z() / length;
  const std::array<std::array<double, 3>, 3> r{{
      {1-2*(y*y+z*z), 2*(x*y-w*z), 2*(x*z+w*y)},
      {2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x)},
      {2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y)}}};
  const std::array<double, 3> v{(p.x - o.pivot.x) * o.transform.scale.x(),
      (p.y - o.pivot.y) * o.transform.scale.y(), (p.z - o.pivot.z) * o.transform.scale.z()};
  return {(o.pivot.x + o.transform.translation.x() + r[0][0]*v[0] + r[0][1]*v[1] + r[0][2]*v[2]) * o.sceneUnitScale,
          (o.pivot.y + o.transform.translation.y() + r[1][0]*v[0] + r[1][1]*v[1] + r[1][2]*v[2]) * o.sceneUnitScale,
          (o.pivot.z + o.transform.translation.z() + r[2][0]*v[0] + r[2][1]*v[1] + r[2][2]*v[2]) * o.sceneUnitScale};
}

QVector3D exportNormal(const QVector3D &n, const ModelExportOptions &o) {
  const QVector3D transformed = o.applyTransform
      ? o.transform.rotation.normalized().rotatedVector({n.x()/o.transform.scale.x(), n.y()/o.transform.scale.y(), n.z()/o.transform.scale.z()}) : n;
  if (!std::isfinite(transformed.lengthSquared()) || transformed.lengthSquared() < 1e-20F) return {};
  return transformed.normalized();
}

QString modelExportSuffix(ModelExportFormat format) {
  switch (format) {
  case ModelExportFormat::Ply: return QStringLiteral("ply");
  case ModelExportFormat::Xyz: return QStringLiteral("xyz");
  case ModelExportFormat::Csv: return QStringLiteral("csv");
  case ModelExportFormat::Obj: return QStringLiteral("obj");
  case ModelExportFormat::Stl: return QStringLiteral("stl");
  case ModelExportFormat::Glb: return QStringLiteral("glb");
  case ModelExportFormat::Spz: return QStringLiteral("spz");
  }
  return {};
}

namespace {
struct DiskVertex {
  double xyz[3];
  float normal[3];
  float color[3];
  qint64 outputIndex;
};
struct DiskCorner { quint32 index; float u; float v; };

// Source order and face order need not agree. Keep a bounded disk-page cache,
// not a second full-size editable point cloud in RAM.
class VertexPages {
public:
  explicit VertexPages(QFile &file) : mFile(file), mPages(64) {}
  bool get(quint32 index, DiskVertex &v) {
    constexpr qint64 pageBytes = 1024 * 1024;
    const qint64 offset = static_cast<qint64>(index) * sizeof(DiskVertex);
    char *output = reinterpret_cast<char *>(&v);
    qint64 copied = 0;
    while (copied < sizeof(DiskVertex)) {
      const qint64 pageId = (offset + copied) / pageBytes;
      auto *page = mPages.object(pageId);
      if (!page) {
        if (!mFile.seek(pageId * pageBytes)) return false;
        page = new QByteArray(mFile.read(pageBytes));
        mPages.insert(pageId, page);
      }
      const qint64 inPage = (offset + copied) % pageBytes;
      const qint64 count = std::min<qint64>(sizeof(DiskVertex) - copied, page->size() - inPage);
      if (count <= 0) return false;
      std::memcpy(output + copied, page->constData() + inPage, count);
      copied += count;
    }
    return true;
  }
private:
  QFile &mFile;
  QCache<qint64, QByteArray> mPages;
};

void appendFloat(QByteArray &bytes, float value) {
  const quint32 le = qToLittleEndian(std::bit_cast<quint32>(value));
  bytes.append(reinterpret_cast<const char *>(&le), 4);
}
void appendUint(QByteArray &bytes, quint32 value) {
  const quint32 le = qToLittleEndian(value);
  bytes.append(reinterpret_cast<const char *>(&le), 4);
}
QByteArray number(double n) { return QByteArray::number(n, 'g', 17); }
QByteArray triple(const double *v, char separator = ' ') {
  return number(v[0]) + separator + number(v[1]) + separator + number(v[2]);
}
bool sameFile(const QString &a, const QString &b) {
  const auto canonical = [](const QString &path) {
    QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
  };
  return canonical(a).compare(canonical(b), Qt::CaseInsensitive) == 0;
}

std::optional<QJsonObject> recordedSceneEdit(const ModelExportOptions &options,const CameraScaleRecoveryCapability &capability) {
  const double axes[3]{options.transform.scale.x(),options.transform.scale.y(),options.transform.scale.z()};
  const double scale=std::abs(axes[0]);
  const int negativeAxes=int(axes[0]<0.)+int(axes[1]<0.)+int(axes[2]<0.);
  if(!options.pivot.isFinite()||!std::isfinite(scale)||scale<=0.||std::abs(axes[1])!=scale||std::abs(axes[2])!=scale||negativeAxes%2)return std::nullopt;
  const double translation[3]{options.transform.translation.x(),options.transform.translation.y(),options.transform.translation.z()};
  const double pivot[3]{options.pivot.x,options.pivot.y,options.pivot.z};
  double w=options.transform.rotation.scalar(),x=options.transform.rotation.x(),y=options.transform.rotation.y(),z=options.transform.rotation.z();
  const double norm=std::hypot(std::hypot(w,x),std::hypot(y,z));
  if(!std::isfinite(norm)||norm<=0.)return std::nullopt;
  w/=norm;x/=norm;y/=norm;z/=norm;
  double rotation[3][3]{{1.-2.*(y*y+z*z),2.*(x*y-w*z),2.*(x*z+w*y)},
      {2.*(x*y+w*z),1.-2.*(x*x+z*z),2.*(y*z-w*x)},
      {2.*(x*z-w*y),2.*(y*z+w*x),1.-2.*(x*x+y*y)}};
  QJsonArray rows,offset;
  for(int i=0;i<3;++i) {
    QJsonArray row;double t=pivot[i]+translation[i];
    for(int j=0;j<3;++j) {rotation[i][j]*=axes[j]<0.?-1.:1.;row.append(rotation[i][j]);t-=scale*rotation[i][j]*pivot[j];}
    if(!std::isfinite(t))return std::nullopt;
    rows.append(row);offset.append(t);
  }
  return QJsonObject{{QStringLiteral("schema"),QStringLiteral("gsw.scene_transform/1")},
      {QStringLiteral("frame"),capability.currentFrame},{QStringLiteral("unit"),capability.currentUnit},
      {QStringLiteral("transform"),QJsonObject{{QStringLiteral("s"),scale},{QStringLiteral("R"),rows},{QStringLiteral("t"),offset}}}};
}

QString recordedExportErrorCaption(const QString &code) {
  if(code==QStringLiteral("camera_scale_export_invalid_geometry_provenance"))return QCoreApplication::translate("Workbench", "几何来源声明无效或与原提取坐标链不一致；原始文件保持不变。");
  if(code==QStringLiteral("camera_scale_export_asset_hash_mismatch"))return QCoreApplication::translate("Workbench", "贴图资源的字节、大小或哈希与记录不一致。");
  if(code==QStringLiteral("camera_scale_export_asset_missing"))return QCoreApplication::translate("Workbench", "引用的贴图资源不存在。");
  if(code==QStringLiteral("camera_scale_export_asset_reference_mismatch"))return QCoreApplication::translate("Workbench", "PLY 贴图引用的路径或顺序与资源清单不一致。");
  if(code==QStringLiteral("camera_scale_export_invalid_asset_header"))return QCoreApplication::translate("Workbench", "PLY 资源头无效或超过允许大小。");
  if(code==QStringLiteral("camera_scale_export_invalid_assets_manifest"))return QCoreApplication::translate("Workbench", "资源清单或其哈希无效。");
  if(code==QStringLiteral("camera_scale_export_invalid_material_index"))return QCoreApplication::translate("Workbench", "网格材质索引超出贴图引用范围。");
  if(code==QStringLiteral("camera_scale_export_invalid_mesh_indices"))return QCoreApplication::translate("Workbench", "网格面索引必须是范围内的整数多边形。");
  if(code==QStringLiteral("camera_scale_export_invalid_representation"))return QCoreApplication::translate("Workbench", "记录坐标导出的模型类型无效。");
  if(code==QStringLiteral("camera_scale_export_invalid_scene_history"))return QCoreApplication::translate("Workbench", "场景编辑历史与实际坐标变换链不一致。");
  if(code==QStringLiteral("camera_scale_export_invalid_scene_transform"))return QCoreApplication::translate("Workbench", "场景变换必须匹配当前框架与单位，并为正等比相似变换。");
  if(code==QStringLiteral("camera_scale_export_invalid_vertex_positions"))return QCoreApplication::translate("Workbench", "PLY 缺少有效的数值 XYZ 坐标。");
  if(code==QStringLiteral("camera_scale_export_unsafe_asset_reference"))return QCoreApplication::translate("Workbench", "贴图引用必须为安全相对路径；拒绝绝对路径、遍历、链接和 URI。");
  if(code==QStringLiteral("camera_scale_export_unsupported_index_relation"))return QCoreApplication::translate("Workbench", "裁剪涉及未支持的索引关联；无法安全重映射。");
  if(code==QStringLiteral("camera_scale_export_unsupported_material_reference"))return QCoreApplication::translate("Workbench", "该外部材质引用方式尚未适配；未丢弃原始材质。");
  return {};
}

ModelExportResult exportRecordedCameraCoordinates(const ModelExportOptions &o) {
  ModelExportResult result;
  if (o.progress && o.progress(0)) {
    result.cancelled = true;
    return result;
  }
  const auto fail = [&](const char *message) {
    result.error = QCoreApplication::translate("Workbench", message);
    return result;
  };
  if (o.format != ModelExportFormat::Ply ||
      (o.cameraCoordinateRecovery != QStringLiteral("source") && o.cameraCoordinateRecovery != QStringLiteral("metric") &&
       o.cameraCoordinateRecovery != QStringLiteral("canonical") && o.cameraCoordinateRecovery != QStringLiteral("training") && o.cameraCoordinateRecovery != QStringLiteral("preserve")))
    return fail("共享相机坐标恢复目前仅支持另存 PLY；其他格式保持原有导出限制。");
  const auto capability = cameraScaleRecoveryCapability(o.sourcePath);
  const bool preserve = o.cameraCoordinateRecovery == QStringLiteral("training") || o.cameraCoordinateRecovery == QStringLiteral("preserve");
  if ((preserve && !capability.preserve) || (!preserve && !capability.source) ||
      (o.cameraCoordinateRecovery == QStringLiteral("metric") && !capability.metric))
    return fail("缺少有效的相机尺度记录，或绝对尺度未知；不能恢复为米制。");
  const auto edit=o.applyTransform?recordedSceneEdit(o,capability):std::optional<QJsonObject>{};
  if(o.applyTransform&&!edit)return fail("相机坐标导出的场景变换必须为非零等比缩放和正旋转；不支持非等比缩放或镜像。");
  if(!QStringList{QStringLiteral("auto"),QStringLiteral("gaussian"),QStringLiteral("point_cloud"),QStringLiteral("mesh")}.contains(o.recordedRepresentation))
    return fail("记录坐标导出的模型类型无效。");
  if (!QFileInfo::exists(o.recoveryPythonPath) ||
      !QFileInfo::exists(QDir(o.recoveryRepositoryRoot).filePath(QStringLiteral("native/backend/camera_scale_export.py"))))
    return fail("坐标恢复需要有效的原生后端与 Python 运行环境。");
  if (QFileInfo::exists(o.destinationPath) || QFileInfo::exists(cameraScaleMetadataPath(o.destinationPath)))
    return fail("坐标恢复必须另存为新文件，不能覆盖已有模型或尺度记录。");
  QTemporaryDir control(QFileInfo(o.destinationPath).dir().filePath(QStringLiteral(".gsw-coordinate-export-XXXXXX")));
  if (!control.isValid()) return fail("无法创建坐标恢复的临时控制目录。");
  const QString cancelPath = control.filePath(QStringLiteral("cancel.request"));
  QStringList arguments{QStringLiteral("-B"), QStringLiteral("-m"), QStringLiteral("native.backend.camera_scale_export"),
      QStringLiteral("--input"), o.sourcePath, QStringLiteral("--output"), o.destinationPath,
      QStringLiteral("--coordinates"), o.cameraCoordinateRecovery,
      QStringLiteral("--representation"),o.recordedRepresentation,
      QStringLiteral("--cancel-file"), cancelPath, QStringLiteral("--progress-json")};
  if (!o.cameraOutputUnit.isEmpty()) arguments << QStringLiteral("--unit") << o.cameraOutputUnit;
  if(edit) {
    const QString path=control.filePath(QStringLiteral("scene-transform.json"));QSaveFile file(path);
    const QByteArray bytes=QJsonDocument(*edit).toJson(QJsonDocument::Compact);
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return fail("无法写入场景变换的坐标导出记录。");
    arguments<<QStringLiteral("--scene-transform")<<path;
  }
  if (!o.deletedVertices.isEmpty())
    arguments << QStringLiteral("--deleted-vertex-count") << QString::number(o.deletedVertices.size());
  if (o.deletedVertices.count(true)) {
    QJsonArray indices;
    for (qsizetype index = 0; index < o.deletedVertices.size(); ++index)
      if (o.deletedVertices.testBit(index)) indices.append(static_cast<qint64>(index));
    const QString path = control.filePath(QStringLiteral("deleted.json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(indices).toJson(QJsonDocument::Compact)) < 0 || !file.commit())
      return fail("无法写入坐标恢复的裁剪记录。");
    arguments << QStringLiteral("--deleted-indices") << path;
  }
  QProcess process;
  auto environment = QProcessEnvironment::systemEnvironment();
  const auto componentEnvironment = resolveRuntimeComponents(
      runtimeComponentInstallationRoot(QCoreApplication::applicationDirPath()), environment);
  if (!componentEnvironment.ready()) {
    result.error = runtimeComponentEnvironmentError(componentEnvironment);
    return result;
  }
  environment = componentEnvironment.environment;
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
  environment.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONPATH"), o.recoveryRepositoryRoot);
  process.setProcessEnvironment(environment);
  process.setWorkingDirectory(o.recoveryRepositoryRoot);
  process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *processArguments) {
    processArguments->flags |= CREATE_NO_WINDOW;
    processArguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    processArguments->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  process.start(o.recoveryPythonPath, arguments);
  if (!process.waitForStarted(10000)) return fail("无法启动坐标恢复子进程。");
  process.closeWriteChannel();
  QByteArray pending;
  QJsonObject response;
  int percent = 0;
  bool requested = false;
  const auto consume = [&] {
    pending += process.readAllStandardOutput();
    while (true) {
      const auto end = pending.indexOf('\n');
      if (end < 0) break;
      const QByteArray line = pending.left(end).trimmed();
      pending.remove(0, end + 1);
      if (line.startsWith("[gsw-coordinate-export-progress] ")) {
        const auto progress = QJsonDocument::fromJson(line.mid(33)).object();
        percent = std::clamp(progress.value(QStringLiteral("progress")).toInt(), 0, 100);
      } else {
        const auto document = QJsonDocument::fromJson(line);
        if (document.isObject()) response = document.object();
      }
    }
  };
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(100);
    consume();
    if (!requested && o.progress && o.progress(percent)) {
      QSaveFile file(cancelPath);
      if (!file.open(QIODevice::WriteOnly) || file.write("cancel\n") != 7 || !file.commit()) {
        process.terminate();
        process.waitForFinished(5000);
        return fail("无法发送坐标恢复取消请求。");
      }
      requested = true;
    }
  }
  consume();
  const QByteArray errorText = process.readAllStandardError().right(4096);
  if (process.exitCode() == 3 || response.value(QStringLiteral("code")).toString() == QStringLiteral("camera_scale_export_cancelled")) {
    result.cancelled = true;
    return result;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !response.value(QStringLiteral("success")).toBool()) {
    const QString code = response.value(QStringLiteral("code")).toString();
    if (code == QStringLiteral("camera_scale_export_rotation_SH_unsupported"))
      return fail("该坐标链包含旋转和方向相关球谐；尚未支持正确烘焙，请保留训练坐标与变换记录。");
    const QString raw=response.isEmpty()?QString::fromUtf8(errorText):response.value(QStringLiteral("detail")).toString(code);
    const QString caption=recordedExportErrorCaption(code);
    result.error = QCoreApplication::translate("Workbench", "坐标恢复失败：%1")
        .arg(caption.isEmpty()?raw:caption+QLatin1Char('\n')+raw);
    return result;
  }
  result.coordinateRecovery = response;
  const auto published=cameraScaleRecoveryCapability(o.destinationPath);
  if(!published.preserve||!published.validationError.isEmpty())return fail("导出文件组的模型、相机、变换或资源校验失败；原始模型保持不变。");
  result.success = true;
  return result;
}
float linearColor(float v) {
  v = std::isfinite(v) ? std::clamp(v, 0.0F, 1.0F) : 0.0F;
  return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
}
} // namespace

ModelExportResult exportModelFile(const ModelExportOptions &o) {
  ModelExportResult result;
  auto fail = [&](const QString &error) { result.error = error; return result; };
  if (sameFile(o.sourcePath, o.destinationPath))
    return fail(QCoreApplication::translate("Workbench", "Choose a new file name; model export cannot overwrite its source."));
  if (!o.cameraCoordinateRecovery.isEmpty()) return exportRecordedCameraCoordinates(o);
  const QFileInfo coordinateRecord(cameraScaleMetadataPath(o.sourcePath));
  if (o.format == ModelExportFormat::Ply && o.applyTransform &&
      (coordinateRecord.exists() || coordinateRecord.isSymLink()))
    return fail(QCoreApplication::translate("Workbench", "This PLY has a coordinate record. Bake transforms through its recorded-coordinate export options; the record cannot be bypassed."));
  if (o.applyTransform && (!o.transform.isValid() || !o.pivot.isFinite() ||
      !std::isfinite(o.sceneUnitScale) || o.sceneUnitScale <= 0.0))
    return fail(QCoreApplication::translate("Workbench", "The model transform is invalid."));
  if (o.format == ModelExportFormat::Spz) return exportSpz(o);
  if (o.format == ModelExportFormat::Ply) {
    auto ply = o;
    const bool sameUnit = !o.sceneUnitDeclared ||
        (o.coordinates.unitDeclared && o.coordinates.unit == o.sceneUnit);
    if (o.sceneUnitScale == 1.0 && sameUnit &&
        o.transform.translation.isNull() && o.transform.rotation.vector().isNull() &&
        o.transform.scale == QVector3D(1, 1, 1)) ply.applyTransform = false;
    return PlyPointCloudLoader::exportSourcePly(ply);
  }
  const QFileInfo sourceBefore(o.sourcePath);
  const qint64 sourceSize = sourceBefore.size();
  const QDateTime sourceModified = sourceBefore.lastModified();
  const QDir directory = QFileInfo(o.destinationPath).absoluteDir();
  QTemporaryFile vertices(directory.filePath(QStringLiteral(".gsw-export-vertices-XXXXXX")));
  QTemporaryFile corners(directory.filePath(QStringLiteral(".gsw-export-faces-XXXXXX")));
  QTemporaryFile bin(directory.filePath(QStringLiteral(".gsw-export-bin-XXXXXX")));
  QTemporaryDir assets(directory.filePath(QStringLiteral("gsw-assets-XXXXXX")));
  QSaveFile output(o.destinationPath);
  if (!output.open(QIODevice::WriteOnly) || !vertices.open() || !corners.open() || !bin.open())
    return fail(QCoreApplication::translate("Workbench", "Unable to create export files in the selected directory."));
  const auto write = [&](QIODevice &device, const QByteArray &bytes) {
    if (device.write(bytes) == bytes.size()) return true;
    result.error = QCoreApplication::translate("Workbench", "Unable to write model data: %1").arg(device.errorString());
    return false;
  };
  const auto cancelled = [&](int progress) {
    if (o.progress && o.progress(progress)) result.cancelled = true;
    return result.cancelled;
  };
  PlySourceGeometry info;
  qint64 retained = 0, triangleCount = 0;
  QString textureName;
  QImage sourceTexture;
  const bool textPoints = o.format == ModelExportFormat::Xyz || o.format == ModelExportFormat::Csv;
  const bool obj = o.format == ModelExportFormat::Obj;
  const bool glb = o.format == ModelExportFormat::Glb;
  QByteArray vertexBuffer, cornerBuffer;
  constexpr qsizetype bufferLimit = 1024 * 1024;
  const auto flush = [&](QIODevice &device, QByteArray &buffer) {
    if (!write(device, buffer)) return false;
    buffer.clear();
    return true;
  };
  PlyGeometryVisitor visitor;
  visitor.cancelled = [&](int p) { return cancelled(p * 55 / 100); };
  visitor.begin = [&](const PlySourceGeometry &source) {
    info = source;
    if ((!o.deletedVertices.isEmpty() && o.deletedVertices.size() != info.vertexCount) ||
        o.deletedVertices.count(true) >= info.vertexCount) {
      result.error = QCoreApplication::translate("Workbench", "The edit state no longer matches the source PLY vertex count.");
      return false;
    }
    if ((obj || o.format == ModelExportFormat::Stl) && !info.faceCount) {
      result.error = QCoreApplication::translate("Workbench", "OBJ and STL export require mesh faces; point clouds cannot be exported as a surface mesh.");
      return false;
    }
    if ((obj || glb) && info.textureCoordinates) {
      if (info.texturePath.isEmpty()) {
        result.error = QCoreApplication::translate("Workbench", "The source texture is missing or the export asset directory cannot be created.");
        return false;
      }
      sourceTexture.load(info.texturePath);
      if (sourceTexture.isNull()) {
        result.error = QCoreApplication::translate("Workbench", "Unable to decode the model texture.");
        return false;
      }
      if (obj) {
        textureName = QStringLiteral("texture.png");
        if (!assets.isValid() || !sourceTexture.save(QDir(assets.path()).filePath(textureName), "PNG")) return false;
        sourceTexture = {};
        QSaveFile material(QDir(assets.path()).filePath(QStringLiteral("material.mtl")));
        if (!material.open(QIODevice::WriteOnly) || !write(material, "newmtl material\nKd 1 1 1\nmap_Kd texture.png\n") || !material.commit()) return false;
        if (!write(output, "mtllib " + QDir(assets.path()).dirName().toUtf8() + "/material.mtl\nusemtl material\n")) return false;
      }
    }
    if (o.format == ModelExportFormat::Csv) return write(output, "x,y,z,red,green,blue\n");
    return true;
  };
  visitor.vertex = [&](qint64 index, const PlySourceVertex &source) {
    const bool keep = o.deletedVertices.isEmpty() || !o.deletedVertices.testBit(index);
    const auto p = exportPosition(source.position, o);
    if (!p.isFinite()) {
      result.error = QCoreApplication::translate("Workbench", "The model contains non-finite coordinates; export was not written.");
      return false;
    }
    const auto n = exportNormal(source.normal, o);
    DiskVertex v{{p.x, p.y, p.z}, {n.x(), n.y(), n.z()},
        {source.color.x(), source.color.y(), source.color.z()}, keep ? retained++ : -1};
    if (!textPoints) vertexBuffer.append(reinterpret_cast<const char *>(&v), sizeof(v));
    if (keep && (textPoints || obj)) {
      const char sep = o.format == ModelExportFormat::Csv ? ',' : ' ';
      QByteArray line = (obj ? QByteArray("v ") : QByteArray()) + triple(v.xyz, sep);
      for (float c : v.color) {
        c = std::isfinite(c) ? std::clamp(c, 0.0F, 1.0F) : 0.0F;
        line += sep;
        line += obj ? number(c) : QByteArray::number(static_cast<int>(std::lround(c * 255)));
      }
      line += '\n';
      if (obj && info.normals) line += "vn " + number(n.x()) + ' ' + number(n.y()) + ' ' + number(n.z()) + '\n';
      if (!write(output, line)) return false;
    }
    return vertexBuffer.size() < bufferLimit || flush(vertices, vertexBuffer);
  };
  visitor.face = [&](const QVector<quint32> &indices, const QVector<QVector2D> &uv) {
    if (textPoints) return true;
    // Keep the loader's source-face winding and fan triangulation, never LOD faces.
    for (qsizetype j = 1; j + 1 < indices.size(); ++j) {
      const bool reflected = o.applyTransform &&
          o.transform.scale.x() * o.transform.scale.y() * o.transform.scale.z() < 0;
      const std::array<qsizetype, 3> triangle = reflected
          ? std::array<qsizetype, 3>{0, j + 1, j} : std::array<qsizetype, 3>{0, j, j + 1};
      bool keep = true;
      for (qsizetype k : triangle) if (!o.deletedVertices.isEmpty() && o.deletedVertices.testBit(indices[k])) keep = false;
      if (!keep) continue;
      ++triangleCount;
      for (qsizetype k : triangle) {
        const auto t = uv.isEmpty() ? QVector2D() : uv[k];
        const DiskCorner corner{indices[k], t.x(), t.y()};
        cornerBuffer.append(reinterpret_cast<const char *>(&corner), sizeof(corner));
      }
      if (cornerBuffer.size() >= bufferLimit && !flush(corners, cornerBuffer)) return false;
    }
    return true;
  };
  QString readError;
  if (!PlyPointCloudLoader::visitSourceGeometry(o.sourcePath, visitor, readError)) {
    if (result.error.isEmpty() && !result.cancelled) result.error = readError.isEmpty()
        ? QCoreApplication::translate("Workbench", "Unable to prepare model export assets.") : readError;
    return result;
  }
  if (!flush(vertices, vertexBuffer) || !flush(corners, cornerBuffer)) return result;
  if (!retained || (info.faceCount && !textPoints && !triangleCount))
    return fail(QCoreApplication::translate("Workbench", "No geometry remains to export."));
  vertices.flush(); corners.flush(); corners.seek(0);
  VertexPages pages(vertices);
  const auto readVertex = [&](quint32 index, DiskVertex &v) {
    if (pages.get(index, v)) return true;
    result.error = QCoreApplication::translate("Workbench", "Unable to read temporary export geometry.");
    return false;
  };
  auto metricRatio = sceneMetresPerCoordinate(o.coordinates);
  if (o.applyTransform && o.sceneUnitDeclared) {
    metricRatio = o.sceneUnit == SceneLengthUnit::Millimetres ? std::optional<double>(0.001) :
                  o.sceneUnit == SceneLengthUnit::Centimetres ? std::optional<double>(0.01) :
                  o.sceneUnit == SceneLengthUnit::Metres ? std::optional<double>(1.0) : std::nullopt;
  }
  if (glb && !metricRatio.has_value())
    return fail(QCoreApplication::translate("Workbench", "绝对尺度未知，不能将任意单位静默导出为米制 GLB。请先提供可追溯的尺度依据。"));
  const double metres = metricRatio.value_or(1.0);
  const auto anchor = exportPosition(o.pivot, o);
  std::array<double, 3> minimum{INFINITY, INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY, -INFINITY};
  QByteArray binaryBuffer;
  const auto glbVertex = [&](const DiskVertex &v, QVector3D normal, const DiskCorner &corner) {
    const std::array<double, 3> offset{anchor.x, anchor.y, anchor.z};
    for (int j = 0; j < 3; ++j) {
      const float coordinate = static_cast<float>((v.xyz[j] - offset[j]) * metres);
      if (!std::isfinite(coordinate)) return false;
      minimum[j] = std::min(minimum[j], static_cast<double>(coordinate));
      maximum[j] = std::max(maximum[j], static_cast<double>(coordinate));
      appendFloat(binaryBuffer, coordinate);
    }
    if (normal.lengthSquared() < 1e-20F) normal = {0, 0, 1};
    appendFloat(binaryBuffer, normal.x()); appendFloat(binaryBuffer, normal.y()); appendFloat(binaryBuffer, normal.z());
    for (float c : v.color) appendFloat(binaryBuffer, linearColor(c));
    appendFloat(binaryBuffer, 1.0F);
    appendFloat(binaryBuffer, corner.u); appendFloat(binaryBuffer, 1.0F - corner.v);
    return binaryBuffer.size() < bufferLimit || flush(bin, binaryBuffer);
  };
  if (o.format == ModelExportFormat::Stl) {
    if (triangleCount > std::numeric_limits<quint32>::max())
      return fail(QCoreApplication::translate("Workbench", "The model exceeds the selected format's size limit. Use PLY or OBJ instead."));
    QByteArray header(80, '\0'); header.replace(0, 9, "GSW mesh "); appendUint(header, static_cast<quint32>(triangleCount));
    if (!write(output, header)) return result;
  }
  const qint64 glbVertexCount = info.faceCount ? triangleCount * 3 : retained;
  if (glb && (static_cast<quint64>(glbVertexCount) > (std::numeric_limits<quint32>::max() - 65536ULL) / 48))
    return fail(QCoreApplication::translate("Workbench", "The model exceeds the selected format's size limit. Use PLY or OBJ instead."));
  if (!textPoints && info.faceCount) {
    for (qint64 triangle = 0; triangle < triangleCount; ++triangle) {
      if ((triangle & 4095) == 0 && cancelled(55 + static_cast<int>(triangle * 35.0 / triangleCount))) return result;
      std::array<DiskCorner, 3> face;
      std::array<DiskVertex, 3> v;
      if (corners.read(reinterpret_cast<char *>(face.data()), sizeof(face)) != sizeof(face))
        return fail(QCoreApplication::translate("Workbench", "Unable to read temporary export geometry."));
      for (int i = 0; i < 3; ++i) if (!readVertex(face[i].index, v[i])) return result;
      const QVector3D a(v[1].xyz[0]-v[0].xyz[0], v[1].xyz[1]-v[0].xyz[1], v[1].xyz[2]-v[0].xyz[2]);
      const QVector3D b(v[2].xyz[0]-v[0].xyz[0], v[2].xyz[1]-v[0].xyz[1], v[2].xyz[2]-v[0].xyz[2]);
      const QVector3D faceNormal = QVector3D::crossProduct(a, b).normalized();
      if (obj) {
        QByteArray text;
        if (info.textureCoordinates) for (const auto &c : face) text += "vt " + number(c.u) + ' ' + number(c.v) + '\n';
        text += 'f';
        for (int i = 0; i < 3; ++i) {
          text += ' ' + QByteArray::number(v[i].outputIndex + 1);
          if (info.textureCoordinates || info.normals) {
            text += '/';
            if (info.textureCoordinates) text += QByteArray::number(triangle * 3 + i + 1);
            if (info.normals) text += '/' + QByteArray::number(v[i].outputIndex + 1);
          }
        }
        if (!write(output, text + '\n')) return result;
      } else if (glb) {
        for (int i = 0; i < 3; ++i) {
          const QVector3D n = info.normals ? QVector3D(v[i].normal[0], v[i].normal[1], v[i].normal[2]) : faceNormal;
          if (!glbVertex(v[i], n, face[i])) return fail(QCoreApplication::translate("Workbench", "Unable to encode GLB geometry."));
        }
      } else {
        QByteArray record;
        for (float n : {faceNormal.x(), faceNormal.y(), faceNormal.z()}) appendFloat(record, n);
        for (const auto &point : v) for (double c : point.xyz) {
          if (!std::isfinite(static_cast<float>(c))) return fail(QCoreApplication::translate("Workbench", "Unable to encode STL coordinates."));
          appendFloat(record, static_cast<float>(c));
        }
        record.append(2, '\0');
        if (!write(output, record)) return result;
      }
    }
  } else if (glb) {
    vertices.seek(0);
    for (qint64 i = 0; i < info.vertexCount; ++i) {
      if ((i & 4095) == 0 && cancelled(55 + static_cast<int>(i * 35.0 / info.vertexCount))) return result;
      DiskVertex v;
      if (vertices.read(reinterpret_cast<char *>(&v), sizeof(v)) != sizeof(v))
        return fail(QCoreApplication::translate("Workbench", "Unable to read temporary export geometry."));
      if (v.outputIndex >= 0 && !glbVertex(v, {v.normal[0], v.normal[1], v.normal[2]}, {}))
        return fail(QCoreApplication::translate("Workbench", "Unable to encode GLB geometry."));
    }
  }
  if (glb) {
    if (!flush(bin, binaryBuffer)) return result;
    const qint64 geometryBytes = bin.pos();
    QJsonArray views{QJsonObject{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", geometryBytes}, {"byteStride", 48}, {"target", 34962}}};
    QJsonArray accessors;
    const auto accessor = [&](int offset, const char *type) {
      QJsonObject value{{"bufferView", 0}, {"byteOffset", offset}, {"componentType", 5126}, {"count", glbVertexCount}, {"type", QString::fromLatin1(type)}};
      if (offset == 0) {
        value["min"] = QJsonArray{minimum[0], minimum[1], minimum[2]};
        value["max"] = QJsonArray{maximum[0], maximum[1], maximum[2]};
      }
      accessors.append(value);
      return accessors.size() - 1;
    };
    QJsonObject attributes{{"POSITION", accessor(0, "VEC3")}, {"COLOR_0", accessor(24, "VEC4")}};
    if (info.faceCount) attributes["NORMAL"] = accessor(12, "VEC3");
    if (info.textureCoordinates) attributes["TEXCOORD_0"] = accessor(40, "VEC2");
    QJsonObject pbr{{"baseColorFactor", QJsonArray{1, 1, 1, 1}}, {"metallicFactor", 0}, {"roughnessFactor", 1}};
    QJsonObject root{{"asset", QJsonObject{{"version", "2.0"}, {"generator", "Gaussian Scene Workbench"}}}};
    if (info.textureCoordinates) {
      const qint64 imageOffset = bin.pos();
      if (!sourceTexture.save(&bin, "PNG")) return fail(QCoreApplication::translate("Workbench", "Unable to encode the GLB texture."));
      views.append(QJsonObject{{"buffer", 0}, {"byteOffset", imageOffset}, {"byteLength", bin.pos() - imageOffset}});
      root["images"] = QJsonArray{QJsonObject{{"bufferView", 1}, {"mimeType", "image/png"}}};
      root["textures"] = QJsonArray{QJsonObject{{"source", 0}}};
      pbr["baseColorTexture"] = QJsonObject{{"index", 0}};
    }
    while (bin.pos() % 4) if (!write(bin, QByteArray(1, '\0'))) return result;
    QJsonObject material{{"pbrMetallicRoughness", pbr}, {"doubleSided", true}};
    if (!info.faceCount) {
      material["extensions"] = QJsonObject{{"KHR_materials_unlit", QJsonObject{}}};
      root["extensionsUsed"] = QJsonArray{QStringLiteral("KHR_materials_unlit")};
    }
    root["materials"] = QJsonArray{material};
    root["meshes"] = QJsonArray{QJsonObject{{"primitives", QJsonArray{QJsonObject{{"attributes", attributes}, {"material", 0}, {"mode", info.faceCount ? 4 : 0}}}}}};
    // glTF is right-handed, Y-up, metres. Keep local float positions and a
    // separate double JSON translation for large survey coordinates.
    root["nodes"] = QJsonArray{QJsonObject{{"mesh", 0}, {"name", QFileInfo(o.sourcePath).completeBaseName()},
        {"translation", QJsonArray{anchor.x*metres, anchor.z*metres, -anchor.y*metres}},
        {"rotation", QJsonArray{-std::sqrt(0.5), 0, 0, std::sqrt(0.5)}}}};
    root["scenes"] = QJsonArray{QJsonObject{{"nodes", QJsonArray{0}}}}; root["scene"] = 0;
    root["buffers"] = QJsonArray{QJsonObject{{"byteLength", bin.pos()}}};
    root["bufferViews"] = views; root["accessors"] = accessors;
    root["extras"] = QJsonObject{{"gswSourceCoordinates", sceneCoordinateInfoToJson(o.coordinates)},
        {"gswAppliedModelTransform", o.applyTransform}, {"gswGaussianCentersOnly", info.gaussian}};
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json += ' ';
    const quint64 total = 12ULL + 8 + json.size() + 8 + bin.pos();
    if (total > std::numeric_limits<quint32>::max())
      return fail(QCoreApplication::translate("Workbench", "The model exceeds the selected format's size limit. Use PLY or OBJ instead."));
    QByteArray prefix; appendUint(prefix, 0x46546C67); appendUint(prefix, 2); appendUint(prefix, static_cast<quint32>(total));
    appendUint(prefix, json.size()); appendUint(prefix, 0x4E4F534A);
    if (!write(output, prefix) || !write(output, json)) return result;
    prefix.clear(); appendUint(prefix, bin.pos()); appendUint(prefix, 0x004E4942);
    if (!write(output, prefix) || !bin.seek(0)) return result;
    while (!bin.atEnd()) {
      if (cancelled(90 + static_cast<int>(bin.pos() * 9.0 / std::max<qint64>(1, bin.size())))) return result;
      const QByteArray data = bin.read(bufferLimit);
      if (data.isEmpty() || !write(output, data)) return fail(QCoreApplication::translate("Workbench", "Unable to read temporary export geometry."));
    }
  }
  if (cancelled(99)) return result;
  const QFileInfo sourceAfter(o.sourcePath);
  if (sourceSize != sourceAfter.size() || sourceModified != sourceAfter.lastModified())
    return fail(QCoreApplication::translate("Workbench", "The source model changed during export. Please try again after processing finishes."));
  if (!output.commit()) return fail(QCoreApplication::translate("Workbench", "Unable to finalize model file: %1").arg(output.errorString()));
  if (!textureName.isEmpty()) assets.setAutoRemove(false);
  result.success = true;
  return result;
}
} // namespace gsw
