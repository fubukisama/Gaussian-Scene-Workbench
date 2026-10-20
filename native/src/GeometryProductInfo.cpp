#include "GeometryProductInfo.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace gsw {
namespace {
bool safeFile(const QString &path, const QString &root) {
  const QFileInfo base(root), file(path);
  if (!base.isDir() || base.isSymLink() || base.isJunction() || !file.isFile()) return false;
  const QString relative = QDir(base.canonicalFilePath()).relativeFilePath(file.absoluteFilePath());
  if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) return false;
  QString current = base.canonicalFilePath();
  for (const QString &part : relative.split('/')) {
    current = QDir(current).filePath(part);
    const QFileInfo item(current);
    if (!item.exists() || item.isSymLink() || item.isJunction()) return false;
  }
  return true;
}
QString digest(const QString &path) {
  QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
  return file.open(QIODevice::ReadOnly) && hash.addData(&file)
      ? QString::fromLatin1(hash.result().toHex()) : QString();
}
}
GeometryProductInfo GeometryProductInfo::inspect(const QString &manifestPath,
    const QString &geometryPath, const QString &ownedRoot) {
  GeometryProductInfo result;
  const auto fail = [&](const char *code) { result.error = QString::fromLatin1(code); return result; };
  if (!safeFile(manifestPath, ownedRoot) || !safeFile(geometryPath, ownedRoot))
    return fail("geometry_product_unsafe_path");
  QFile file(manifestPath);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
    return fail("geometry_product_manifest_unreadable");
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail("geometry_product_invalid_json");
  const auto root = document.object(), geometry = root.value("geometry").toObject();
  const QString relative = geometry.value("path").toString();
  if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(':') ||
      relative.contains('\\') || relative.split('/').contains("..")) return fail("geometry_product_unsafe_path");
  const QString selected = QFileInfo(QDir(QFileInfo(manifestPath).absolutePath()).filePath(relative)).canonicalFilePath();
  if (selected.isEmpty() || selected != QFileInfo(geometryPath).canonicalFilePath()) return fail("geometry_product_path_mismatch");
  static const QRegularExpression sha("^[0-9a-f]{64}$");
  const QString expected = geometry.value("sha256").toString();
  const auto source = root.value("source").toObject();
  const QString backend = source.value("backend").toString();
  if (root.value("schema") != "gsw.gs_geometry/1" || geometry.value("kind") != "depth_point_cloud" ||
      geometry.value("fusion") != "none" || geometry.value("surface_semantics") != "appearance_proxy_not_material_boundary" ||
      geometry.value("frame_id").toString().isEmpty() || !sha.match(expected).hasMatch() ||
      (backend != "3dgs" && backend != "2dgs")) return fail("geometry_product_contract_mismatch");
  const double count = geometry.value("point_count").toDouble(-1.);
  if (!std::isfinite(count) || count <= 0 || count != std::floor(count) || count > 9007199254740991.)
    return fail("geometry_product_invalid_count");
  if (digest(selected) != expected) return fail("geometry_product_hash_mismatch");
  const auto sidecar = geometry.value("coordinate_sidecar").toObject();
  const QString sidecarRelative = sidecar.value("path").toString();
  if (sidecarRelative.isEmpty() || QDir::isAbsolutePath(sidecarRelative) || sidecarRelative.contains(':') ||
      sidecarRelative.contains('\\') || sidecarRelative.split('/').contains(".."))
    return fail("geometry_product_coordinate_sidecar_missing");
  const QString sidecarPath = QDir(QFileInfo(manifestPath).absolutePath()).filePath(sidecarRelative);
  const auto expectedSidecarPath = QFileInfo(selected).absoluteDir().filePath(
      QFileInfo(selected).completeBaseName() + ".camera-scale.json");
  if (!safeFile(sidecarPath, ownedRoot) || QFileInfo(sidecarPath).canonicalFilePath() != QFileInfo(expectedSidecarPath).canonicalFilePath() ||
      !sha.match(sidecar.value("sha256").toString()).hasMatch() || digest(sidecarPath) != sidecar.value("sha256").toString())
    return fail("geometry_product_coordinate_sidecar_mismatch");
  result.valid = true; result.geometryPath = selected; result.pointCount = qint64(count); result.manifest = root;
  return result;
}
}
