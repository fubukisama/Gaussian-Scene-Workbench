#include "ModelExport.h"
#include "PlyPointCloudLoader.h"

#include <QBuffer>
#include <QCache>
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
#include <QtEndian>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace gsw {
SceneCoordinate3D exportPosition(const SceneCoordinate3D &p, const ModelExportOptions &o) {
  if (!o.applyTransform) return p;
  // Double-precision source coordinates; viewport recentering is never baked.
  const auto r = o.transform.rotation.normalized().toRotationMatrix();
  const std::array<double, 3> v{(p.x - o.pivot.x) * o.transform.scale.x(),
      (p.y - o.pivot.y) * o.transform.scale.y(), (p.z - o.pivot.z) * o.transform.scale.z()};
  return {o.pivot.x + o.transform.translation.x() + r(0,0)*v[0] + r(0,1)*v[1] + r(0,2)*v[2],
          o.pivot.y + o.transform.translation.y() + r(1,0)*v[0] + r(1,1)*v[1] + r(1,2)*v[2],
          o.pivot.z + o.transform.translation.z() + r(2,0)*v[0] + r(2,1)*v[1] + r(2,2)*v[2]};
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
  if (o.applyTransform && (!o.transform.isValid() || !o.pivot.isFinite()))
    return fail(QCoreApplication::translate("Workbench", "The model transform is invalid."));
  if (o.format == ModelExportFormat::Ply) {
    auto ply = o;
    if (o.transform.translation.isNull() && rotationsEquivalent(o.transform.rotation, QQuaternion()) &&
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
  double metres = o.coordinates.unit == SceneLengthUnit::Millimetres ? 0.001 :
                   o.coordinates.unit == SceneLengthUnit::Centimetres ? 0.01 : 1.0;
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
