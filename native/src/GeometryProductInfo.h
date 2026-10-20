#pragma once
#include <QString>
#include <QJsonObject>

namespace gsw {
// Verifies immutable depth geometry before the UI associates its PLY. This is
// artifact consistency, not a physical surface-accuracy or source attestation.
struct GeometryProductInfo {
  bool valid = false;
  QString error;
  QString geometryPath;
  qint64 pointCount = 0;
  QJsonObject manifest;
  static GeometryProductInfo inspect(const QString &manifestPath,
      const QString &geometryPath, const QString &ownedRoot);
};
}
