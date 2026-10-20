#include "GeometryProductInfo.h"
#include "SceneCoordinates.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cmath>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!temporary.isValid()) return 1;
  const QDir root(temporary.path());
  const QString ply = root.filePath("depth.ply"), sidecar = root.filePath("depth.camera-scale.json"),
      manifest = root.filePath("manifest.json");
  const auto write = [](const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  const QByteArray points("ply\nformat ascii 1.0\nelement vertex 1\nproperty double x\nproperty double y\nproperty double z\nend_header\n1 2 3\n");
  const auto hash = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
  const QByteArray sidecarBytes("{\"schema\":\"gsw.coordinate_export/1\"}\n");
  if (!write(ply, points) || !write(sidecar, sidecarBytes)) return 1;
  QJsonObject document{{"schema", "gsw.gs_geometry/1"}, {"source", QJsonObject{{"backend", "3dgs"}}},
      {"geometry", QJsonObject{{"kind", "depth_point_cloud"}, {"path", "depth.ply"}, {"sha256", hash(points)},
          {"point_count", 1}, {"frame_id", "data/canonical"}, {"fusion", "none"},
          {"surface_semantics", "appearance_proxy_not_material_boundary"},
          {"coordinate_sidecar", QJsonObject{{"path", "depth.camera-scale.json"}, {"sha256", hash(sidecarBytes)}}}}}};
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    std::fprintf(stderr, "Depth product %s: %s\n", ok ? "PASS" : "FAIL", message); passed &= ok;
  };
  const auto inspect = [&] {
    write(manifest, QJsonDocument(document).toJson());
    return gsw::GeometryProductInfo::inspect(manifest, ply, root.path());
  };
  const auto valid = inspect();
  check(valid.valid && valid.pointCount == 1, "typed point geometry and sidecar are hash-bound before association");
  write(ply, points + "\n");
  check(inspect().error == "geometry_product_hash_mismatch", "modified PLY cannot retain acceptance");
  write(ply, points);
  write(sidecar, sidecarBytes + "\n");
  check(inspect().error == "geometry_product_coordinate_sidecar_mismatch", "modified scale sidecar cannot retain acceptance");
  write(sidecar, sidecarBytes);
  auto geometry = document.value("geometry").toObject();
  geometry["kind"] = "gaussian_centres"; document["geometry"] = geometry;
  check(!inspect().valid, "Gaussian centres cannot masquerade as extracted surface evidence");
  geometry["kind"] = "depth_point_cloud"; geometry["path"] = "../depth.ply"; document["geometry"] = geometry;
  check(!inspect().valid, "relative paths cannot escape the owned product");
  geometry["path"] = "depth.ply"; geometry["point_count"] = 0; document["geometry"] = geometry;
  check(!inspect().valid, "empty geometry is not reported as a usable display result");
  for (const QString &path : app.arguments().mid(1)) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { check(false, "actual generated manifest is readable"); continue; }
    const auto actual = QJsonDocument::fromJson(file.readAll()).object();
    const auto record = actual.value("geometry").toObject();
    const QString cloud = QDir(QFileInfo(path).absolutePath()).filePath(record.value("path").toString());
    const auto product = gsw::GeometryProductInfo::inspect(path, cloud, QFileInfo(path).absolutePath());
    check(product.valid, "actual generated product passes native manifest/PLY/sidecar binding");
    const auto coordinates = gsw::parsePlyCoordinateMetadata({}, cloud, true);
    check(coordinates.validationError.isEmpty() && coordinates.dataIdentity == actual.value("source").toObject().value("input_data_identity").toString() &&
        coordinates.provenance.value("coordinate_frame") == record.value("frame"),
        "actual generated sidecar preserves the declared frame and source identity in the native coordinate reader");
    const auto ratio = record.value("meters_per_unit");
    check(ratio.isNull() ? !coordinates.metersPerCoordinate : coordinates.metersPerCoordinate &&
        std::abs(*coordinates.metersPerCoordinate - ratio.toDouble()) < 1e-12,
        "native coordinate units match actual geometry length ratios without guessing unknown metres");
  }
  return passed ? 0 : 1;
}
