#include "QualityMipModelContract.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <cmath>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {
namespace {
bool regularUnlinkedFile(const QString &path) {
  QFileInfo current(path);
  if (!current.isFile()) return false;
  for (;;) {
    if (current.isSymbolicLink() || current.isJunction()) return false;
    const QString parent = current.absolutePath();
    if (current.absoluteFilePath() == parent) break;
    current.setFile(parent);
  }
  return true;
}
bool hashString(const QJsonValue &value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return value.isString() && pattern.match(value.toString()).hasMatch();
}
QString hashFile(const QString &path) {
  if (!regularUnlinkedFile(path)) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    const QByteArray block = file.read(4 * 1024 * 1024);
    if (block.isEmpty() && file.error() != QFileDevice::NoError) return {};
    hash.addData(block);
  }
  return QString::fromLatin1(hash.result().toHex());
}
QString sidecarPathFor(const QString &path) {
  const QFileInfo model(path);
  return model.absoluteDir().filePath(model.completeBaseName() + QStringLiteral(".quality.json"));
}
bool occupied(const QString &path) {
  const QFileInfo info(path);
  return info.exists() || info.isSymbolicLink() || info.isJunction();
}
bool safeOutputDirectory(const QString &path) {
  QFileInfo current(path);
  if (!current.isDir()) return false;
  for (;;) {
    if (current.isSymbolicLink() || current.isJunction()) return false;
    const QString parent = current.absolutePath();
    if (current.absoluteFilePath() == parent) return true;
    current.setFile(parent);
  }
}
struct OwnedFileIdentity {
  quint64 volume = 0;
  quint64 index = 0;
  bool valid = false;
};
#ifdef Q_OS_WIN
OwnedFileIdentity identityOfHandle(const HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information) ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
  return {information.dwVolumeSerialNumber,
      (quint64(information.nFileIndexHigh) << 32) | information.nFileIndexLow, true};
}
OwnedFileIdentity flushOwnedFile(const QString &path) {
  const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
      GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return {};
  auto identity = identityOfHandle(handle);
  if (!FlushFileBuffers(handle)) identity.valid = false;
  CloseHandle(handle);
  return identity;
}
bool removeOwnedFile(const QString &path, const OwnedFileIdentity &expected) {
  if (!expected.valid) return false;
  const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
      DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const auto observed = identityOfHandle(handle);
  FILE_DISPOSITION_INFO disposition{TRUE};
  const bool same = observed.valid && observed.volume == expected.volume && observed.index == expected.index;
  const bool removed = same && SetFileInformationByHandle(handle, FileDispositionInfo,
      &disposition, sizeof(disposition));
  CloseHandle(handle);
  return removed;
}
#else
OwnedFileIdentity flushOwnedFile(const QString &path) {
  return {0, 0, QFileInfo(path).isFile()};
}
bool removeOwnedFile(const QString &, const OwnedFileIdentity &) {
  // Do not delete by a computed path without a native identity-safe primitive.
  // A sidecar-only orphan is fail-closed and can be reported for manual cleanup.
  return false;
}
#endif
}

struct QualityMipExportTransaction::Impl {
  QualityMipModelContract source;
  QString destination;
  QString sidecar;
  std::unique_ptr<QTemporaryFile> ply;
  std::unique_ptr<QTemporaryFile> json;
  bool published = false;
};

QualityMipExportTransaction::QualityMipExportTransaction(const QualityMipModelContract &source, QString destination)
    : m(std::make_unique<Impl>()) {
  m->source = source;
  m->destination = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
  m->sidecar = sidecarPathFor(m->destination);
}
QualityMipExportTransaction::~QualityMipExportTransaction() = default;

bool QualityMipExportTransaction::open(QString &error) {
  error.clear();
  if (!m->source.enabled || !m->source.errorCode.isEmpty() || m->ply) {
    error = QStringLiteral("quality_mip_export_state_invalid"); return false;
  }
  if (occupied(m->destination) || occupied(m->sidecar)) {
    error = QStringLiteral("quality_mip_export_requires_fresh_destination"); return false;
  }
  const QDir parent = QFileInfo(m->destination).absoluteDir();
  if (!safeOutputDirectory(parent.absolutePath())) {
    error = QStringLiteral("quality_mip_export_parent_invalid"); return false;
  }
  m->ply = std::make_unique<QTemporaryFile>(parent.filePath(QStringLiteral(".gsw-mip-XXXXXX")));
  if (!m->ply->open()) { error = QStringLiteral("quality_mip_export_temporary_failed"); return false; }
  return true;
}

QIODevice &QualityMipExportTransaction::device() {
  if (!m->ply || !m->ply->isOpen()) throw std::logic_error("quality_mip_export_not_open");
  return *m->ply;
}

bool QualityMipExportTransaction::commit(QString &error) {
  if (!m->ply || !m->ply->isOpen() || m->published) {
    error = QStringLiteral("quality_mip_export_state_invalid"); return false;
  }
  if (!m->ply->flush()) { error = QStringLiteral("quality_mip_export_write_failed"); return false; }
  m->ply->close();
  const auto plyIdentity = flushOwnedFile(m->ply->fileName());
  if (!plyIdentity.valid) { error = QStringLiteral("quality_mip_export_write_failed"); return false; }
  QFile header(m->ply->fileName());
  if (!header.open(QIODevice::ReadOnly)) { error = QStringLiteral("quality_mip_export_write_failed"); return false; }
  bool marker = false, ended = false;
  const auto lines = header.read(1024 * 1024).split('\n');
  for (const auto &raw : lines) {
    const QByteArray line = raw.trimmed();
    marker |= line == "comment GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1";
    if (line == "end_header") { ended = true; break; }
  }
  header.close();
  if (!marker || !ended) { error = QStringLiteral("quality_mip_export_marker_required"); return false; }
  const QString exportedHash = hashFile(m->ply->fileName());
  if (exportedHash.isEmpty()) { error = QStringLiteral("quality_mip_export_write_failed"); return false; }
  QJsonObject metadata = m->source.metadata;
  metadata[QStringLiteral("fused_sha256")] = exportedHash;
  metadata[QStringLiteral("fused_file")] = QFileInfo(m->destination).fileName();
  metadata[QStringLiteral("derived_from_fused_sha256")] = m->source.modelSha256;
  if (metadata.contains(QStringLiteral("filter_widths"))) {
    metadata[QStringLiteral("source_filter_widths_sha256")] = metadata.value(QStringLiteral("filter_widths")).toObject().value(QStringLiteral("sha256"));
    metadata.remove(QStringLiteral("filter_widths"));
  }
  const QByteArray bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
  const QDir parent = QFileInfo(m->destination).absoluteDir();
  m->json = std::make_unique<QTemporaryFile>(parent.filePath(QStringLiteral(".gsw-quality-XXXXXX")));
  if (!m->json->open() || m->json->write(bytes) != bytes.size() || !m->json->flush()) {
    error = QStringLiteral("quality_mip_export_write_failed"); return false;
  }
  m->json->close();
  const auto jsonIdentity = flushOwnedFile(m->json->fileName());
  if (!jsonIdentity.valid) { error = QStringLiteral("quality_mip_export_write_failed"); return false; }
  if (occupied(m->destination) || occupied(m->sidecar)) {
    error = QStringLiteral("quality_mip_export_requires_fresh_destination"); return false;
  }
  // Metadata is published first: a missing PLY cannot render, and the PLY's
  // mandatory marker prevents any interrupted pair from becoming legacy EWA.
  if (!m->json->rename(m->sidecar)) { error = QStringLiteral("quality_mip_export_publish_failed"); return false; }
  m->json->setAutoRemove(false);
  if (!m->ply->rename(m->destination)) {
    const bool cleaned = removeOwnedFile(m->sidecar, jsonIdentity);
    error = cleaned ? QStringLiteral("quality_mip_export_publish_failed")
                    : QStringLiteral("quality_mip_export_cleanup_required");
    return false;
  }
  m->ply->setAutoRemove(false);
  m->published = true;
  error.clear();
  return true;
}

bool QualityMipModelContract::appliesToRasterFrame(const bool ipcFrame, const bool surfel) const {
  return enabled && errorCode.isEmpty() && !ipcFrame && !surfel;
}

QualityMipModelContract QualityMipModelContract::load(const QString &modelPath,
    const bool gaussian, const bool surfel, const bool rawFilterAttributes, const bool requiresSidecar) {
  QualityMipModelContract result;
  const QFileInfo model(modelPath);
  const QString sidecarPath = model.absoluteDir().filePath(model.completeBaseName() + QStringLiteral(".quality.json"));
  const QFileInfo sidecar(sidecarPath);
  if (!sidecar.exists()) {
    if (rawFilterAttributes) result.errorCode = QStringLiteral("quality_mip_raw_requires_fusion");
    else if (requiresSidecar) result.errorCode = QStringLiteral("quality_mip_sidecar_required");
    return result;
  }
  const auto fail = [&result](const char *code) {
    result.enabled = false;
    result.errorCode = QString::fromLatin1(code);
    return result;
  };
  if (!regularUnlinkedFile(sidecarPath) || sidecar.size() > 1024 * 1024)
    return fail("quality_mip_sidecar_invalid");
  QFile file(sidecarPath);
  if (!file.open(QIODevice::ReadOnly)) return fail("quality_mip_sidecar_invalid");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return fail("quality_mip_sidecar_invalid");
  const QJsonObject object = document.object();
  const QJsonObject filter2d = object.value(QStringLiteral("filter_2d")).toObject();
  if (object.value(QStringLiteral("schema")) != QStringLiteral("gsw.quality_model/1") ||
      object.value(QStringLiteral("filter")) != QStringLiteral("full_mip") ||
      !hashString(object.value(QStringLiteral("raw_source_sha256"))) ||
      !hashString(object.value(QStringLiteral("fused_sha256"))) || !hashString(object.value(QStringLiteral("plan_hash"))) ||
      !filter2d.value(QStringLiteral("variance")).isDouble() ||
      std::abs(filter2d.value(QStringLiteral("variance")).toDouble() - .3) > 1e-12 ||
      !filter2d.value(QStringLiteral("opacity_compensation")).isBool() ||
      !filter2d.value(QStringLiteral("opacity_compensation")).toBool())
    return fail("quality_mip_sidecar_invalid");
  result.modelSha256 = hashFile(modelPath);
  const QString representation = object.value(QStringLiteral("representation")).toString();
  const QString expectedHash = object.value(representation == QStringLiteral("raw")
      ? QStringLiteral("raw_source_sha256") : QStringLiteral("fused_sha256")).toString();
  if (result.modelSha256.isEmpty() || result.modelSha256 != expectedHash)
    return fail("quality_mip_model_hash_mismatch");
  if (representation == QStringLiteral("raw")) return fail("quality_mip_raw_requires_fusion");
  if (representation != QStringLiteral("fused") || !object.value(QStringLiteral("filter_3d_applied")).isBool() ||
      !object.value(QStringLiteral("filter_3d_applied")).toBool()) return fail("quality_mip_filter_state_invalid");
  if (rawFilterAttributes) return fail("quality_mip_double_application_risk");
  if (!gaussian || surfel) return fail("quality_mip_representation_unsupported");
  if (object.contains(QStringLiteral("fused_file")) &&
      object.value(QStringLiteral("fused_file")).toString() != model.fileName())
    return fail("quality_mip_model_binding_invalid");
  if (object.contains(QStringLiteral("kernel_build_sha256")) && !hashString(object.value(QStringLiteral("kernel_build_sha256"))))
    return fail("quality_mip_sidecar_invalid");
  if (object.contains(QStringLiteral("source_filter_widths_sha256")) && !hashString(object.value(QStringLiteral("source_filter_widths_sha256"))))
    return fail("quality_mip_filter_state_invalid");
  if (object.contains(QStringLiteral("filter_widths"))) {
    const auto widths = object.value(QStringLiteral("filter_widths")).toObject();
    const QString relative = widths.value(QStringLiteral("path")).toString();
    const bool validFrame = widths.contains(QStringLiteral("frame"))
        ? widths.size() == 3 && widths.value(QStringLiteral("frame")) == QStringLiteral("training")
        : widths.size() == 2;
    if (!validFrame || relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(':') ||
        QDir::fromNativeSeparators(relative).split('/').contains(QStringLiteral("..")) ||
        !hashString(widths.value(QStringLiteral("sha256"))) ||
        hashFile(model.absoluteDir().filePath(relative)) != widths.value(QStringLiteral("sha256")).toString())
      return fail("quality_mip_filter_widths_invalid");
  }
  result.enabled = true;
  result.pixelVariance = .3F;
  result.opacityCompensation = true;
  result.planHash = object.value(QStringLiteral("plan_hash")).toString();
  result.metadata = object;
  return result;
}

QByteArray qualityMipProjectionGlsl() {
  return QByteArray(R"GLSL(
vec3 gswApplyGaussian2dFilter(vec3 rawCovariance, inout float opacity, bool fullMip) {
  float variance = fullMip ? 0.3 : 0.09;
  vec3 covariance = rawCovariance + vec3(variance, 0.0, variance);
  if (fullMip) {
    float rawDeterminant = rawCovariance.x * rawCovariance.z - rawCovariance.y * rawCovariance.y;
    float filteredDeterminant = covariance.x * covariance.z - covariance.y * covariance.y;
    // Matches the quality CUDA kernel's determinant floor, not a second 3D filter.
    opacity *= sqrt(max(0.000025, rawDeterminant / filteredDeterminant));
  }
  return covariance;
}
)GLSL");
}
} // namespace gsw
