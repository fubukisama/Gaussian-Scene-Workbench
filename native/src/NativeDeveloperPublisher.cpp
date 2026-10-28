// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"
#include "NativeActivationFiles.h"
namespace gsw {
namespace {
void requireDeveloperRoot(const QString &root) {
  using namespace activation;
  for (const auto &relative : {QStringLiteral(".update-trust-anchor"), QStringLiteral("updates/trust")}) {
    const auto path = QDir(root).filePath(relative);
#ifdef Q_OS_WIN
    const auto api = nativeApiPath(path);
    if (GetFileAttributesW(reinterpret_cast<LPCWSTR>(api.utf16())) != INVALID_FILE_ATTRIBUTES)
      fail("developer_trusted_root_forbidden");
    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
      fail("developer_trusted_root_forbidden");
#else
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) fail("developer_trusted_root_forbidden");
#endif
  }
  const auto bootstrap = parse(read(QDir(root).filePath(QStringLiteral("bootstrap.json")), 16384), 16384);
  if (string(bootstrap.at("schema")) != QLatin1String("gsw.native_bootstrap/1"))
    fail("activation_bootstrap_invalid");
  const auto protocol = integer(bootstrap.at("protocol"), 1, 2);
  if (protocol != 1) return;
  // Only this private unsigned adapter admits an old managed developer root.
  // Its immutable launcher/runtime rows are independently verified, not merely
  // trusted because a local JSON document claims protocol 1.
  exactKeys(bootstrap, {"schema", "protocol", "files"});
  const auto &rows = bootstrap.at("files");
  if (!rows.is_array() || rows.empty() || rows.size() > size_t(maxFiles)) fail("activation_bootstrap_invalid");
  QSet<QString> expected, actual;
  QMap<QString, QPair<QString, bool>> aliases;
  for (const auto &row : rows) {
    exactKeys(row, {"path", "size", "sha256"});
    const auto path = safePath(string(row.at("path")));
    claim(aliases, path);
    if (!path.startsWith(QLatin1String("bin/")) || path == QLatin1String("bin/Gaussian Scene Workbench.exe"))
      fail("activation_bootstrap_invalid");
    const auto checksum = string(row.at("sha256")); sha(checksum);
    QFile file; heldRead(file, QDir(root).filePath(path));
    if (file.size() != integer(row.at("size"), 0, maxArchive) || hash(file) != checksum)
      fail("activation_bootstrap_digest_mismatch");
    expected.insert(path);
  }
  if (!expected.contains(QStringLiteral("bin/Gaussian Scene Workbench Launcher.exe")))
    fail("activation_bootstrap_invalid");
  QDirIterator iterator(QDir(root).filePath(QStringLiteral("bin")),
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    iterator.next();
    if (!ordinary(iterator.filePath())) fail("activation_bootstrap_invalid");
    if (iterator.fileInfo().isFile()) actual.insert(QDir(root).relativeFilePath(iterator.filePath()));
    else if (!iterator.fileInfo().isDir()) fail("activation_bootstrap_invalid");
  }
  if (actual != expected) fail("activation_bootstrap_file_set_differs");
}
}
NativeActivationResult NativeDeveloperPublisher::publish(const QString &root,
    const QString &archive, const QByteArray &offer, const NativeInstallationLock &lock) {
  try {
    if (!lock.locked() || lock.installationRoot() != QDir::cleanPath(root))
      activation::fail("activation_lock_required");
    requireDeveloperRoot(root);
    return NativeActivationCore::publish(root, archive, offer, lock, true);
  } catch (const activation::Failure &error) { return {false, error.code, {}}; }
    catch (const activation::Json::exception &) { return {false, QStringLiteral("activation_json_invalid"), {}}; }
    catch (...) { return {false, QStringLiteral("developer_publication_failed"), {}}; }
}
} // namespace gsw
