#pragma once
#include <QDir>
#include <QFileInfo>

namespace gsw {
// Verify both lexical and resolved paths; reject symlinks/junctions at every
// component below the configured catalog, and never allow deleting the catalog.
inline bool safeCatalogChild(const QString &root, const QString &path) {
  const QFileInfo base(root), target(path);
  if (!base.isDir() || !target.exists()) return false;
  auto inside = [](const QString &r, const QString &p) {
    const QString relative = QDir(r).relativeFilePath(p);
    return !QDir::isAbsolutePath(relative) && relative != QStringLiteral(".") &&
        relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../"));
  };
  if (!inside(base.absoluteFilePath(), target.absoluteFilePath()) ||
      !inside(base.canonicalFilePath(), target.canonicalFilePath())) return false;
  QFileInfo node(target);
  const QString basePath = QDir::cleanPath(base.absoluteFilePath());
  while (QDir::cleanPath(node.absoluteFilePath()) != basePath) {
    if (node.isSymLink() || node.isJunction()) return false;
    const QString parent = node.absolutePath();
    if (parent == node.absoluteFilePath()) return false;
    node = QFileInfo(parent);
  }
  return true;
}
} // namespace gsw
