// SPDX-License-Identifier: GPL-3.0-or-later
// Separate build-time tool. Never link this adapter into the public helper.
#include "NativeActivationCore.h"
#include "NativeActivationFiles.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

namespace {
int result(const gsw::NativeActivationResult &value) {
  QJsonObject message{{QStringLiteral("schema"), QStringLiteral("gsw.native_developer_publication/1")},
      {QStringLiteral("ok"), value.ok}, {QStringLiteral("code"), value.code},
      {QStringLiteral("admission"), QStringLiteral("local-developer-unsigned")},
      {QStringLiteral("publisherAuthenticated"), false}};
  if (!value.buildId.isEmpty()) message.insert(QStringLiteral("buildId"), value.buildId);
  const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
  std::fflush(stdout);
  return value.ok ? 0 : 2;
}

bool trustTrace(const QString &root) {
  // Refuse before even creating a lock file. Corrupt/inaccessible markers and
  // abandoned trust directories are not an unsigned-development installation.
  for (const auto &relative : {QStringLiteral(".update-trust-anchor"), QStringLiteral("updates/trust")}) {
    const auto path = QDir(root).filePath(relative);
#ifdef Q_OS_WIN
    const auto api = gsw::updatepaths::nativeApiPath(path);
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(api.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return true;
#else
    const QFileInfo info(path);
    if (info.exists() || info.isSymLink()) return true;
#endif
  }
  return false;
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto arguments = app.arguments().mid(1);
  // Naming the only operation makes unsigned local admission explicit. There
  // is no signing claim, network fetch, seed root, channel promotion or fallback.
  if (arguments.size() != 4 || arguments[0] != QLatin1String("--publish-unsigned-local"))
    return result({false, QStringLiteral("INVALID_REQUEST"), {}});
  const auto root = QDir::fromNativeSeparators(arguments[1]);
  const auto archive = QDir::fromNativeSeparators(arguments[2]);
  const auto offerPath = QDir::fromNativeSeparators(arguments[3]);
  if (!gsw::updatepaths::logicalLocalPath(root) || !gsw::updatepaths::logicalLocalPath(archive) ||
      !gsw::updatepaths::logicalLocalPath(offerPath) || !gsw::activation::ordinary(root) ||
      !QFileInfo(root).isDir())
    return result({false, QStringLiteral("developer_local_path_required"), {}});
  if (trustTrace(root))
    return result({false, QStringLiteral("developer_trusted_root_forbidden"), {}});
  try {
    // Hold the descriptor's opened bytes; activation opens and holds the ZIP
    // itself, then performs the common whole-payload/source/license validation.
    const auto offer = gsw::activation::read(offerPath, 4 * 1024 * 1024);
    gsw::NativeInstallationLock lock(root);
    if (!lock.locked()) return result({false, lock.error(), {}});
    return result(gsw::NativeDeveloperPublisher::publish(root, archive, offer, lock));
  } catch (const gsw::activation::Failure &error) {
    return result({false, error.code, {}});
  } catch (...) {
    return result({false, QStringLiteral("developer_publication_failed"), {}});
  }
}
