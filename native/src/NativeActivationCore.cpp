// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"
#include "NativeActivationFiles.h"
#include <QStorageInfo>

namespace gsw {
using namespace activation;
namespace {
Json immutableOffer(Json offer) {
  for (const auto &key : {"channel", "releaseSequence", "metadataRevision", "releaseNotes", "revokedBuildIds"}) offer.erase(key);
  return offer;
}
void pointToSlot(const QString &root, const QString &id, const QString &exeHash) {
  const QString pointer = QDir(root).filePath(QStringLiteral("current.json"));
  Json active{{"schema", "gsw.native_active_version/1"}, {"version", id.toStdString()}, {"executableSha256", exeHash.toStdString()}};
  if (QFileInfo::exists(pointer)) {
    // A malformed pointer is backed up as bytes, never parsed as a prerequisite
    // to recovery. This operation cannot reset independent TUF state.
    const auto previous = read(pointer, 16384);
    // Do not replace an already-correct pointer, including while an observer
    // holds it open. Malformed JSON does not obstruct the recovery branch.
    try { if (parse(previous, 16384) == active) return; }
    catch (const Failure &) {} catch (const Json::exception &) {}
    const QString backups = QDir(root).filePath(QStringLiteral("updates/pointer-backups"));
    ensureDirectory(backups);
    durableWrite(QDir(backups).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json")), previous, false);
  }
  durableWrite(pointer, QByteArray::fromStdString(active.dump()), true);
}
}
struct NativeInstallationLock::Impl {
  QString root, error;
#ifdef Q_OS_WIN
  HANDLE handle = INVALID_HANDLE_VALUE;
#endif
  bool acquired = false;
};
NativeInstallationLock::NativeInstallationLock(const QString &root) : m(std::make_unique<Impl>()) {
  m->root = QDir::cleanPath(root);
  if (!ordinary(m->root) || !QFileInfo(m->root).isDir()) { m->error = QStringLiteral("activation_root_invalid"); return; }
#ifdef Q_OS_WIN
  const auto path = QDir::toNativeSeparators(QDir(m->root).filePath(QStringLiteral(".publish.lock")));
  if (!ordinary(path, true)) { m->error = QStringLiteral("activation_lock_invalid"); return; }
  m->handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
      0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (m->handle != INVALID_HANDLE_VALUE) {
    BY_HANDLE_FILE_INFORMATION info{};
    m->acquired = GetFileInformationByHandle(m->handle, &info) && info.nNumberOfLinks == 1 &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
  }
#endif
  if (!m->acquired) m->error = QStringLiteral("activation_busy_or_lock_invalid");
}
NativeInstallationLock::~NativeInstallationLock() {
#ifdef Q_OS_WIN
  if (m->handle != INVALID_HANDLE_VALUE) CloseHandle(m->handle);
#endif
}
bool NativeInstallationLock::locked() const { return m->acquired; }
QString NativeInstallationLock::error() const { return m->error; }
QString NativeInstallationLock::installationRoot() const { return m->root; }
NativeActivationResult NativeActivationCore::install(const QString &root, const QString &archive,
    const NativeVerifiedUpdate &update, const NativeInstallationLock &lock) {
  if (QDir::cleanPath(root) != update.installationRoot || update.descriptorSha256.isEmpty() || update.historical)
    return {false, QStringLiteral("activation_admission_invalid"), {}};
  return publish(root, archive, update.offer, lock, false, update.descriptorSha256);
}
NativeActivationResult NativeActivationCore::rollback(const QString &, const NativeVerifiedUpdate &,
    const NativeInstallationLock &) {
  return {false, QStringLiteral("rollback_not_implemented"), {}};
}
NativeActivationResult NativeActivationCore::publish(const QString &root, const QString &archive,
    const QByteArray &offerBytes, const NativeInstallationLock &lock, bool developer, const QString &descriptorSha256) {
  QString id;
  try {
    if (!lock.locked() || lock.installationRoot() != QDir::cleanPath(root)) fail("activation_lock_required");
    if (!ordinary(root)) fail("activation_root_invalid");
    const auto offer = parse(offerBytes, 4 * 1024 * 1024);
    if (string(offer.at("schema")) != QLatin1String("gsw.update_offer/1") ||
        string(offer.at("platform")) != QLatin1String("windows-x64")) fail("activation_offer_invalid");
    const QString product = string(offer.at("product"));
    if (product != QLatin1String("Fubukisama.GaussianSceneWorkbench.Native") &&
        product != QLatin1String("Fubukisama.GaussianSceneWorkbench.UpdateFixture")) fail("activation_product_invalid");
    id = buildId(offer.at("buildId"));
    integer(offer.at("minimumBootstrapProtocol"), 2, 2);
    const auto bootstrap = parse(read(QDir(root).filePath(QStringLiteral("bootstrap.json")), 16384), 16384);
    if (integer(bootstrap.at("protocol"), 1, 2) != 2) fail("activation_bootstrap_migration_required");
    const qint64 archiveLength = integer(offer.at("archiveLength"), 1, maxArchive);
    const qint64 expanded = integer(offer.at("unpackedLength"), 1, maxExpanded);
    const qint64 count = integer(offer.at("fileCount"), 1, maxFiles);
    const QString archiveHash = string(offer.at("archiveSha256")); sha(archiveHash);
    const QString manifestHash = string(offer.at("payloadManifestSha256")); sha(manifestHash);
    const QString sourceHash = string(offer.at("correspondingSourceSha256")); sha(sourceHash);
    QFile held; heldRead(held, archive);
    if (held.size() != archiveLength || hash(held) != archiveHash) fail("activation_archive_digest_mismatch");
    QStorageInfo storage(root); storage.refresh();
    if (!storage.isValid() || storage.isReadOnly() || storage.bytesAvailable() < expanded + reserveSpace) fail("activation_insufficient_space");
    Zip zip; zip.z.m_pRead = zipRead; zip.z.m_pIO_opaque = &held;
    if (!mz_zip_reader_init(&zip.z, mz_uint64(held.size()), 0)) fail("activation_archive_invalid");
    zip.initialized = true;
    if (mz_zip_reader_get_num_files(&zip.z) != mz_uint(count + 1)) fail("activation_archive_file_set_differs");
    QMap<QString, Entry> entries;
    QMap<QString, QPair<QString, bool>> names;
    qint64 total = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.z); ++i) {
      mz_zip_archive_file_stat stat{};
      if (!mz_zip_reader_file_stat(&zip.z, i, &stat) || stat.m_is_directory || stat.m_is_encrypted ||
          !stat.m_is_supported || (stat.m_method != 0 && stat.m_method != 8)) fail("activation_archive_type_invalid");
      const auto type = (stat.m_external_attr >> 16) & 0170000;
      if ((type && type != 0100000) || (stat.m_external_attr & 0x10)) fail("activation_archive_type_invalid");
      const mz_uint length = mz_zip_reader_get_filename(&zip.z, i, nullptr, 0);
      if (!length || length > 1025 || stat.m_uncomp_size > mz_uint64(maxArchive)) fail("activation_archive_resource_limit");
      QByteArray name(qsizetype(length), Qt::Uninitialized);
      if (mz_zip_reader_get_filename(&zip.z, i, name.data(), length) != length || name.back() != '\0') fail("activation_archive_path_invalid");
      name.chop(1);
      if (name.contains('\0')) fail("activation_archive_path_invalid");
      const QString path = QString::fromUtf8(name);
      if (path.toUtf8() != name) fail("activation_archive_path_invalid");
      claim(names, path);
      total += qint64(stat.m_uncomp_size);
      if (total > expanded) fail("activation_archive_resource_limit");
      entries.insert(path, {i, path, {}, qint64(stat.m_uncomp_size)});
    }
    if (total != expanded || !entries.contains(QStringLiteral("release-files.json"))) fail("activation_archive_file_set_differs");
    const auto manifestEntry = entries.value(QStringLiteral("release-files.json"));
    const auto manifestBytes = zipBytes(zip, manifestEntry.index, manifestEntry.size);
    if (digest(manifestBytes) != manifestHash) fail("activation_manifest_digest_mismatch");
    const auto manifest = parse(manifestBytes);
    exactKeys(manifest, {"schema", "product", "platform", "buildId", "sourceCommit", "sourceTree", "files"});
    if (string(manifest.at("schema")) != QLatin1String("gsw.native_update_files/1") ||
        manifest.at("product") != offer.at("product") || manifest.at("platform") != offer.at("platform") ||
        manifest.at("buildId") != offer.at("buildId") || manifest.at("sourceCommit") != offer.at("sourceCommit") ||
        manifest.at("sourceTree") != offer.at("sourceTree") || !manifest.at("files").is_array() || manifest.at("files").size() != size_t(count)) fail("activation_manifest_identity_invalid");
    QSet<QString> recorded;
    for (const auto &row : manifest.at("files")) {
      exactKeys(row, {"path", "size", "sha256"});
      const auto path = safePath(string(row.at("path")));
      if (path == QLatin1String("release-files.json") || recorded.contains(path) || !entries.contains(path)) fail("activation_archive_file_set_differs");
      recorded.insert(path);
      auto &entry = entries[path];
      if (entry.size != integer(row.at("size"), 0, maxArchive)) fail("activation_archive_file_set_differs");
      entry.sha256 = string(row.at("sha256")); sha(entry.sha256);
    }
    const QString exe = QString::fromLatin1("bin/Gaussian Scene Workbench.exe");
    if (!entries.contains(exe) || entries[exe].sha256.isEmpty()) fail("activation_required_file_missing");
    const QString updates = QDir(root).filePath(QStringLiteral("updates"));
    const QString staging = QDir(updates).filePath(QStringLiteral("staging"));
    const QString receipts = QDir(updates).filePath(QStringLiteral("receipts"));
    const QString versions = QDir(root).filePath(QStringLiteral("versions"));
    ensureDirectory(staging); ensureDirectory(receipts); ensureDirectory(versions);
    const QString slot = QDir(versions).filePath(id);
    if (!QFileInfo::exists(slot)) {
      const QString stage = QDir(staging).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
      if (QFileInfo::exists(stage)) fail("activation_staging_collision");
      ensureDirectory(stage);
      for (const auto &entry : entries) {
        const QString path = QDir(stage).filePath(entry.name);
        ensureDirectory(QFileInfo(path).absolutePath());
        QFile output;
        secureOutputOpen(output, path);
        Writer writer(&output, entry.size);
        if (!mz_zip_reader_extract_to_callback(&zip.z, entry.index, zipWrite, &writer, 0) || writer.written != entry.size || !output.flush()) fail("activation_archive_corrupt");
        if (!entry.sha256.isEmpty() && QString::fromLatin1(writer.hash.result().toHex()) != entry.sha256) fail("activation_payload_digest_mismatch");
#ifdef Q_OS_WIN
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(output.handle())))) fail("activation_extract_write_failed");
#endif
        output.close();
      }
      verifySource(stage, entries, entries[exe].sha256, sourceHash);
      if (!ordinary(slot, true) || !QDir().rename(stage, slot)) fail("activation_slot_exists_or_occupied");
    }
    // Re-entry after a process failure never overwrites a slot. The exact
    // already-installed file set must independently match this admitted offer.
    const auto installed = verifyInstalledSlot(slot, offer);
    if (installed.executableSha256 != entries[exe].sha256) fail("activation_slot_digest_mismatch");
    Json receipt{{"schema", "gsw.native_activation_receipt/1"}, {"mode", developer ? "developer" : "online"},
      {"offer", offer}, {"manifestSha256", manifestHash.toStdString()}, {"executableSha256", entries[exe].sha256.toStdString()}};
    if (!developer) { sha(descriptorSha256); receipt["descriptorSha256"] = descriptorSha256.toStdString(); }
    durableWrite(QDir(receipts).filePath(id + QStringLiteral(".json")), QByteArray::fromStdString(receipt.dump()), true);
    pointToSlot(root, id, installed.executableSha256);
    return {true, QStringLiteral("ACTIVATED"), id};
  } catch (const Failure &e) { return {false, e.code, id}; }
    catch (const Json::exception &) { return {false, QStringLiteral("activation_json_invalid"), id}; }
    catch (...) { return {false, QStringLiteral("activation_failed"), id}; }
}
} // namespace gsw
