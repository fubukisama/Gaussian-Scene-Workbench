// SPDX-License-Identifier: GPL-3.0-or-later
// Private implementation utilities; no authorization decisions are made here.
#pragma once
#include "json.hpp"
#include "miniz.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QVector>
#include <limits>
#include <memory>
#include <set>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace gsw::activation {
using Json = nlohmann::json;
constexpr qint64 maxArchive = 4LL * 1024 * 1024 * 1024;
constexpr qint64 maxExpanded = 16LL * 1024 * 1024 * 1024;
constexpr qint64 maxManifest = 64LL * 1024 * 1024;
constexpr qint64 reserveSpace = 512LL * 1024 * 1024;
constexpr qint64 maxFiles = 100000;
struct Failure { QString code; };
[[noreturn]] inline void fail(const char *code) { throw Failure{QString::fromLatin1(code)}; }
inline QString string(const Json &j) {
  if (!j.is_string()) fail("activation_json_type_invalid");
  const auto &s = j.get_ref<const std::string &>();
  return QString::fromUtf8(s.data(), qsizetype(s.size()));
}
inline Json parse(const QByteArray &raw, qint64 limit = maxManifest) {
  if (raw.isEmpty() || raw.size() > limit) fail("activation_json_resource_limit");
  std::vector<std::set<std::string>> keys;
  return Json::parse(raw.constBegin(), raw.constEnd(), [&](int depth, Json::parse_event_t event, Json &value) {
    if (depth > 32) fail("activation_json_too_deep");
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    if (event == Json::parse_event_t::key &&
        (keys.empty() || !keys.back().insert(value.get<std::string>()).second)) fail("activation_json_duplicate_key");
    if (event == Json::parse_event_t::object_end) keys.pop_back();
    return true;
  });
}
inline void exactKeys(const Json &j, const std::set<std::string> &required) {
  if (!j.is_object() || j.size() != required.size()) fail("activation_schema_invalid");
  for (const auto &key : required) if (!j.contains(key)) fail("activation_schema_invalid");
}
inline qint64 integer(const Json &j, qint64 low, qint64 high) {
  if (!j.is_number_integer() || (j.is_number_unsigned() && j.get<quint64>() > quint64(high))) fail("activation_integer_invalid");
  const auto value = j.get<qint64>();
  if (value < low || value > high) fail("activation_integer_invalid");
  return value;
}
inline QString digest(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
inline QString hash(QIODevice &file) {
  if (!file.seek(0)) fail("activation_file_read_failed");
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&file)) fail("activation_file_read_failed");
  return QString::fromLatin1(h.result().toHex());
}
inline void sha(const QString &s) {
  static const QRegularExpression expression(QStringLiteral("^[a-f0-9]{64}$"));
  if (!expression.match(s).hasMatch()) fail("activation_digest_invalid");
}
inline bool ordinary(const QString &path, bool missing = false) {
  if (!QDir::isAbsolutePath(path) || path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("\\\\"))) return false;
  QString cursor = QDir::cleanPath(path);
  for (;;) {
#ifdef Q_OS_WIN
    const DWORD a = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(cursor).utf16()));
    if (a == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      if (!missing || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return false;
    } else if (a & FILE_ATTRIBUTE_REPARSE_POINT) return false;
#else
    if (QFileInfo(cursor).isSymLink() || (!missing && !QFileInfo::exists(cursor))) return false;
#endif
    const QString parent = QFileInfo(cursor).absolutePath();
    if (parent == cursor) return true;
    cursor = parent;
  }
}
inline QString safePath(const QString &value) {
  if (value.isEmpty() || value.toUtf8().size() > 1024 || value.startsWith('/') || value.contains('\\') ||
      value.contains(':') || value.normalized(QString::NormalizationForm_C) != value) fail("activation_archive_path_invalid");
  const auto parts = value.split('/');
  if (parts.size() > 32) fail("activation_archive_path_invalid");
  static const QRegularExpression device(QStringLiteral("^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|CLOCK\\$|COM[1-9]|LPT[1-9])(?:\\..*)?$"));
  for (const auto &part : parts) {
    if (part.isEmpty() || part.size() > 255 || part == QLatin1String(".") || part == QLatin1String("..") ||
        part.endsWith('.') || part.endsWith(' ') || device.match(part.normalized(QString::NormalizationForm_KC).toUpper()).hasMatch())
      fail("activation_archive_path_invalid");
    for (const auto ch : part) if (ch.unicode() < 32 || QStringLiteral("<>\"|?*").contains(ch)) fail("activation_archive_path_invalid");
  }
  return value;
}
inline void claim(QMap<QString, QPair<QString, bool>> &names, const QString &name) {
  safePath(name);
  const auto parts = name.split('/');
  QString prefix;
  for (qsizetype i = 0; i < parts.size(); ++i) {
    if (i) prefix += '/';
    prefix += parts[i];
    const bool file = i + 1 == parts.size();
    const QString alias = prefix.normalized(QString::NormalizationForm_KC).toCaseFolded();
    if (names.contains(alias)) {
      const auto previous = names.value(alias);
      if (file || previous.first != prefix || previous.second) fail("activation_archive_path_alias");
    } else names.insert(alias, {prefix, file});
  }
}
inline QString buildId(const Json &j) {
  const QString s = string(j);
  static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$"));
  if (!pattern.match(s).hasMatch() || s.contains('/') || safePath(s) != s) fail("activation_build_id_invalid");
  return s;
}
inline void ensureDirectory(const QString &path) {
  if (!ordinary(path, true) || !QDir().mkpath(path) || !ordinary(path) || !QFileInfo(path).isDir()) fail("activation_directory_unavailable");
}
inline void heldRead(QFile &file, const QString &path) {
  if (!ordinary(path) || !QFileInfo(path).isFile()) fail("activation_file_invalid");
  file.setFileName(path);
#ifdef Q_OS_WIN
  const auto native = QDir::toNativeSeparators(QDir::cleanPath(path));
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) fail("activation_file_occupied");
  BY_HANDLE_FILE_INFORMATION info{};
  wchar_t finalPath[32768]{};
  const DWORD count = GetFinalPathNameByHandleW(handle, finalPath, 32768, FILE_NAME_NORMALIZED);
  QString actual = count > 0 && count < 32768 ? QString::fromWCharArray(finalPath) : QString();
  if (actual.startsWith(QLatin1String("\\\\?\\"))) actual.remove(0, 4);
  if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || actual.compare(native, Qt::CaseInsensitive)) {
    CloseHandle(handle); fail("activation_file_invalid");
  }
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
  if (fd < 0) { CloseHandle(handle); fail("activation_file_read_failed"); }
  if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { _close(fd); fail("activation_file_read_failed"); }
#else
  if (!file.open(QIODevice::ReadOnly)) fail("activation_file_read_failed");
#endif
}
#ifdef Q_OS_WIN
// Keep every ancestor stable while a new extraction file is being written.
// This QObject lives under the caller's QFile, not a temporary open helper.
class OutputDirectoryLease final : public QObject {
  QVector<HANDLE> handles;
public:
  explicit OutputDirectoryLease(QFile *owner) : QObject(owner) {}
  ~OutputDirectoryLease() override { for (auto handle : handles) CloseHandle(handle); }
  void acquire(const QString &path) {
    QStringList parents;
    QString cursor = QFileInfo(path).absolutePath();
    for (;;) {
      parents.append(cursor);
      const auto parent = QFileInfo(cursor).absolutePath();
      if (parent == cursor) break;
      cursor = parent;
    }
    // Anchor the drive first, then each descendant. A name cannot be swapped
    // into a junction between path inspection and CREATE_NEW below.
    for (auto it = parents.crbegin(); it != parents.crend(); ++it) {
      const auto native = QDir::toNativeSeparators(QDir::cleanPath(*it));
      HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) fail("activation_write_path_invalid");
      handles.append(handle);
      BY_HANDLE_FILE_INFORMATION info{};
      wchar_t finalPath[32768]{};
      const DWORD count = GetFinalPathNameByHandleW(handle, finalPath, 32768, FILE_NAME_NORMALIZED);
      QString actual = count > 0 && count < 32768 ? QString::fromWCharArray(finalPath) : QString();
      if (actual.startsWith(QLatin1String("\\\\?\\"))) actual.remove(0, 4);
      if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
          (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || actual.compare(native, Qt::CaseInsensitive))
        fail("activation_write_path_invalid");
    }
  }
};
#endif
inline void secureOutputOpen(QFile &file, const QString &path) {
  if (file.isOpen() || !ordinary(QFileInfo(path).absolutePath()) || !ordinary(path, true))
    fail("activation_write_path_invalid");
  file.setFileName(path);
#ifdef Q_OS_WIN
  auto ancestors = std::make_unique<OutputDirectoryLease>(&file);
  ancestors->acquire(path);
  const auto native = QDir::toNativeSeparators(QDir::cleanPath(path));
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_WRITE,
      0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) fail("activation_extract_write_failed");
  BY_HANDLE_FILE_INFORMATION info{};
  wchar_t finalPath[32768]{};
  const DWORD count = GetFinalPathNameByHandleW(handle, finalPath, 32768, FILE_NAME_NORMALIZED);
  QString actual = count > 0 && count < 32768 ? QString::fromWCharArray(finalPath) : QString();
  if (actual.startsWith(QLatin1String("\\\\?\\"))) actual.remove(0, 4);
  if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || actual.compare(native, Qt::CaseInsensitive)) {
    CloseHandle(handle); fail("activation_write_path_invalid");
  }
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
  if (fd < 0) { CloseHandle(handle); fail("activation_extract_write_failed"); }
  if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    _close(fd); fail("activation_extract_write_failed");
  }
  ancestors.release(); // QFile owns the guard until its object is destroyed.
#else
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("activation_extract_write_failed");
#endif
}
inline QByteArray read(const QString &path, qint64 limit = maxManifest) {
  QFile file; heldRead(file, path);
  if (file.size() > limit) fail("activation_file_resource_limit");
  return file.readAll();
}
inline void durableWrite(const QString &path, const QByteArray &bytes, bool replace) {
  if (!ordinary(QFileInfo(path).absolutePath()) || !ordinary(path, true)) fail("activation_write_path_invalid");
  const QString temp = path + QStringLiteral(".new-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  QFile file(temp);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size() || !file.flush()) fail("activation_write_failed");
#ifdef Q_OS_WIN
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
  if (handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(handle)) fail("activation_write_failed");
  file.close();
  if (!MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(temp).utf16()),
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) fail("activation_atomic_write_failed");
#else
  file.close();
  if (replace && QFileInfo::exists(path)) fail("activation_platform_unsupported");
  if (!QFile::rename(temp, path)) fail("activation_atomic_write_failed");
#endif
}
struct Entry { mz_uint index; QString name, sha256; qint64 size; };
struct Zip final { mz_zip_archive z{}; bool initialized = false; ~Zip() { if (initialized) mz_zip_reader_end(&z); } };
inline size_t zipRead(void *opaque, mz_uint64 offset, void *buffer, size_t count) {
  auto *f = static_cast<QFile *>(opaque);
  if (offset > quint64(std::numeric_limits<qint64>::max()) || count > size_t(std::numeric_limits<qint64>::max()) || !f->seek(qint64(offset))) return 0;
  const auto received = f->read(static_cast<char *>(buffer), qint64(count));
  return received < 0 ? 0 : size_t(received);
}
struct Writer {
  QFile *file;
  QCryptographicHash hash{QCryptographicHash::Sha256};
  qint64 written = 0, limit;
  Writer(QFile *f, qint64 size) : file(f), limit(size) {}
};
inline size_t zipWrite(void *opaque, mz_uint64 offset, const void *buffer, size_t count) {
  auto *w = static_cast<Writer *>(opaque);
  if (offset != quint64(w->written) || count > quint64(w->limit - w->written)) return 0;
  const auto bytes = QByteArrayView(static_cast<const char *>(buffer), qsizetype(count));
  if (w->file && w->file->write(bytes.data(), bytes.size()) != bytes.size()) return 0;
  w->hash.addData(bytes); w->written += bytes.size();
  return count;
}
inline QByteArray zipBytes(Zip &zip, mz_uint index, qint64 size) {
  if (size < 0 || size > maxManifest) fail("activation_manifest_resource_limit");
  QByteArray bytes(qsizetype(size), Qt::Uninitialized);
  if (!mz_zip_reader_extract_to_mem(&zip.z, index, bytes.data(), size_t(size), 0)) fail("activation_archive_corrupt");
  return bytes;
}
inline void verifySource(const QString &slot, const QMap<QString, Entry> &entries, const QString &exeHash, const QString &sourceHash) {
  const QStringList required{QStringLiteral("COPYING"), QStringLiteral("LICENSE"), QStringLiteral("THIRD_PARTY_LICENSES.md"),
    QStringLiteral("native/LICENSE.md"), QStringLiteral("source/native/CMakeLists.txt"), QStringLiteral("source-manifest.json"),
    QStringLiteral("build_manifest.json"), QStringLiteral("backend_manifest.json")};
  for (const auto &name : required) if (!entries.contains(name)) fail("activation_required_file_missing");
  const auto raw = read(QDir(slot).filePath(QStringLiteral("source-manifest.json")));
  if (digest(raw) != sourceHash) fail("activation_source_manifest_mismatch");
  const auto source = parse(raw);
  exactKeys(source, {"schema", "executableSha256", "license", "sourceFileCount", "files"});
  if (string(source.at("schema")) != QLatin1String("gsw-native-corresponding-source-v1") ||
      string(source.at("license")) != QLatin1String("GPL-3.0-or-later") || string(source.at("executableSha256")) != exeHash ||
      !source.at("files").is_array() || integer(source.at("sourceFileCount"), 1, maxFiles) != qint64(source.at("files").size())) fail("activation_source_identity_invalid");
  QSet<QString> recorded, actual;
  QMap<QString, QPair<QString, bool>> names;
  for (const auto &row : source.at("files")) {
    exactKeys(row, {"path", "size", "sha256"});
    const QString name = safePath(string(row.at("path"))); claim(names, name);
    const auto found = entries.constFind(QStringLiteral("source/") + name);
    if (found == entries.cend() || found->size != integer(row.at("size"), 0, maxArchive) ||
        found->sha256 != string(row.at("sha256"))) fail("activation_source_files_differ");
    recorded.insert(name);
  }
  for (auto it = entries.cbegin(); it != entries.cend(); ++it) if (it.key().startsWith(QLatin1String("source/"))) actual.insert(it.key().mid(7));
  if (recorded != actual) fail("activation_source_files_differ");
  const auto build = parse(read(QDir(slot).filePath(QStringLiteral("build_manifest.json"))));
  if (string(build.at("product")) != QLatin1String("Gaussian Scene Workbench") || build.at("legacyWebRuntimeIncluded") != false) fail("activation_product_invalid");
}
struct InstalledSlot final {
  QString executableSha256;
  QMap<QString, Entry> entries;
};
// The expected offer must come from the online trust bridge or the accepted
// rollback ledger. Reading a local receipt/manifest alone grants no authority.
inline InstalledSlot verifyInstalledSlot(const QString &slot, const Json &expectedOffer) {
  if (!ordinary(slot) || !QFileInfo(slot).isDir()) fail("activation_slot_invalid");
  if (string(expectedOffer.at("schema")) != QLatin1String("gsw.update_offer/1") ||
      string(expectedOffer.at("platform")) != QLatin1String("windows-x64")) fail("activation_offer_invalid");
  const auto product = string(expectedOffer.at("product"));
  if (product != QLatin1String("Fubukisama.GaussianSceneWorkbench.Native") &&
      product != QLatin1String("Fubukisama.GaussianSceneWorkbench.UpdateFixture")) fail("activation_product_invalid");
  buildId(expectedOffer.at("buildId"));
  static const QRegularExpression gitIdentity(QStringLiteral("^[a-f0-9]{40}$"));
  if (!gitIdentity.match(string(expectedOffer.at("sourceCommit"))).hasMatch() ||
      !gitIdentity.match(string(expectedOffer.at("sourceTree"))).hasMatch()) fail("activation_source_identity_invalid");
  const auto fileCount = integer(expectedOffer.at("fileCount"), 1, maxFiles);
  const auto expanded = integer(expectedOffer.at("unpackedLength"), 1, maxExpanded);
  const auto manifestHash = string(expectedOffer.at("payloadManifestSha256")); sha(manifestHash);
  const auto sourceHash = string(expectedOffer.at("correspondingSourceSha256")); sha(sourceHash);
  const QString manifestName = QStringLiteral("release-files.json");
  const auto raw = read(QDir(slot).filePath(manifestName));
  if (digest(raw) != manifestHash) fail("activation_manifest_digest_mismatch");
  const auto manifest = parse(raw);
  exactKeys(manifest, {"schema", "product", "platform", "buildId", "sourceCommit", "sourceTree", "files"});
  if (string(manifest.at("schema")) != QLatin1String("gsw.native_update_files/1") ||
      manifest.at("product") != expectedOffer.at("product") || manifest.at("platform") != expectedOffer.at("platform") ||
      manifest.at("buildId") != expectedOffer.at("buildId") || manifest.at("sourceCommit") != expectedOffer.at("sourceCommit") ||
      manifest.at("sourceTree") != expectedOffer.at("sourceTree") || !manifest.at("files").is_array() ||
      manifest.at("files").size() != size_t(fileCount)) fail("activation_manifest_identity_invalid");
  InstalledSlot verified;
  verified.entries.insert(manifestName, {0, manifestName, manifestHash, raw.size()});
  QMap<QString, QPair<QString, bool>> names;
  claim(names, manifestName);
  QSet<QString> expectedFiles{manifestName}, expectedDirectories;
  qint64 total = raw.size();
  for (const auto &row : manifest.at("files")) {
    exactKeys(row, {"path", "size", "sha256"});
    const auto path = safePath(string(row.at("path"))); claim(names, path);
    const auto size = integer(row.at("size"), 0, maxArchive);
    const auto checksum = string(row.at("sha256")); sha(checksum);
    if (total > expanded - size) fail("activation_slot_resource_limit");
    total += size;
    verified.entries.insert(path, {0, path, checksum, size});
    expectedFiles.insert(path);
    QString parent = path;
    for (auto slash = parent.lastIndexOf('/'); slash >= 0; slash = parent.lastIndexOf('/')) {
      parent.truncate(slash); expectedDirectories.insert(parent);
    }
  }
  if (total != expanded) fail("activation_slot_resource_limit");
  const QString exe = QString::fromLatin1("bin/Gaussian Scene Workbench.exe");
  if (!verified.entries.contains(exe)) fail("activation_required_file_missing");
  QSet<QString> actualFiles, actualDirectories;
  QDirIterator iterator(slot, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
      QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    iterator.next();
    const auto info = iterator.fileInfo();
    const auto path = info.absoluteFilePath();
    const auto relative = safePath(QDir(slot).relativeFilePath(path));
    if (!ordinary(path)) fail("activation_slot_path_invalid");
    if (info.isDir()) {
      if (!expectedDirectories.contains(relative)) fail("activation_slot_file_set_differs");
      actualDirectories.insert(relative);
    } else if (info.isFile()) {
      if (!expectedFiles.contains(relative) || actualFiles.contains(relative)) fail("activation_slot_file_set_differs");
      const auto &entry = verified.entries[relative];
      QFile held; heldRead(held, path);
      if (held.size() != entry.size || hash(held) != entry.sha256) fail("activation_slot_digest_mismatch");
      actualFiles.insert(relative);
    } else fail("activation_slot_path_invalid");
  }
  if (actualFiles != expectedFiles || actualDirectories != expectedDirectories) fail("activation_slot_file_set_differs");
  verified.executableSha256 = verified.entries[exe].sha256;
  verifySource(slot, verified.entries, verified.executableSha256, sourceHash);
  return verified;
}
} // namespace gsw::activation
