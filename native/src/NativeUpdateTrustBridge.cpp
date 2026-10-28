// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeUpdateTrustBridge.h"
#include "gsw_update_trust.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSslCertificate>

namespace gsw {
namespace {
int cancelled(void *context) {
  return context && static_cast<const std::atomic_bool *>(context)->load() ? 1 : 0;
}
}
NativeTrustResult NativeUpdateTrustBridge::request(const QString &operation, const QString &root,
    const QString &channel, const QString &expectedBuildId, const NativeInstallationLock &lock,
    const std::atomic_bool *cancellation) {
  NativeTrustResult result;
  result.code = QStringLiteral("INVALID_REQUEST");
  if (!lock.locked() || lock.installationRoot() != QDir::cleanPath(root)) {
    result.code = QStringLiteral("activation_lock_required"); return result;
  }
  if (!QStringList{QStringLiteral("resolve"), QStringLiteral("verify_cached"),
      QStringLiteral("inspect_accepted")}.contains(operation) ||
      !QStringList{QStringLiteral("stable"), QStringLiteral("preview")}.contains(channel)) return result;
  QJsonObject request{{"schema", 1}, {"operation", operation},
      {"installation_root", lock.installationRoot()}, {"channel", channel}};
  if (!expectedBuildId.isEmpty()) request.insert(QStringLiteral("expected_build_id"), expectedBuildId);
  const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
  auto buffer = gsw_update_trust_request(reinterpret_cast<const unsigned char *>(bytes.constData()),
      size_t(bytes.size()), cancelled, const_cast<std::atomic_bool *>(cancellation));
  if (!buffer.data || !buffer.length || buffer.length > 4 * 1024 * 1024) {
    gsw_update_trust_free(buffer); result.code = QStringLiteral("TRUST_RESPONSE_INVALID"); return result;
  }
  const QByteArray raw(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length));
  gsw_update_trust_free(buffer);
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(raw, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    result.code = QStringLiteral("TRUST_RESPONSE_INVALID"); return result;
  }
  const auto response = document.object();
  result.code = response.value(QStringLiteral("code")).toString(QStringLiteral("TRUST_RESPONSE_INVALID"));
  if (response.value(QStringLiteral("schema")).toInt() != 1 ||
      !response.value(QStringLiteral("ok")).isBool() || !response.value(QStringLiteral("ok")).toBool()) return result;
  if (operation == QLatin1String("inspect_accepted")) {
    if (!response.value(QStringLiteral("acceptedState")).isObject()) { result.code = QStringLiteral("TRUST_RESPONSE_INVALID"); return result; }
    result.acceptedState = response.value(QStringLiteral("acceptedState")).toObject();
    result.ok = true; return result;
  }
  result.offer = response.value(QStringLiteral("offer")).toObject();
#ifdef GSW_UPDATER_FIXTURE_BUILD
  const QString product = QStringLiteral("Fubukisama.GaussianSceneWorkbench.UpdateFixture");
#else
  const QString product = QStringLiteral("Fubukisama.GaussianSceneWorkbench.Native");
#endif
  if (result.offer.value(QStringLiteral("product")).toString() != product ||
      result.offer.value(QStringLiteral("platform")).toString() != QLatin1String("windows-x64") ||
      result.offer.value(QStringLiteral("channel")).toString() != channel ||
      (!expectedBuildId.isEmpty() && result.offer.value(QStringLiteral("buildId")).toString() != expectedBuildId)) {
    result.code = QStringLiteral("TRUST_IDENTITY_INVALID"); return result;
  }
  const auto policy = response.value(QStringLiteral("transportPolicy")).toObject();
  for (const auto &host : policy.value(QStringLiteral("approvedHosts")).toArray())
    if (host.isString()) result.downloadPolicy.approvedHosts.append(host.toString());
  if (result.downloadPolicy.approvedHosts.isEmpty()) { result.code = QStringLiteral("TRUST_POLICY_INVALID"); return result; }
#ifdef GSW_UPDATER_FIXTURE_BUILD
  const auto ca = policy.value(QStringLiteral("fixtureCaPem")).toString().toUtf8();
  if (!ca.isEmpty()) {
    const auto certificates = QSslCertificate::fromData(ca, QSsl::Pem);
    if (certificates.isEmpty()) { result.code = QStringLiteral("TRUST_POLICY_INVALID"); return result; }
    auto config = result.downloadPolicy.sslConfiguration;
    auto authorities = config.caCertificates(); authorities.append(certificates);
    config.setCaCertificates(authorities); result.downloadPolicy.sslConfiguration = config;
  }
#endif
  const auto descriptor = response.value(QStringLiteral("descriptorSha256")).toString();
  static const QRegularExpression sha(QStringLiteral("^[a-f0-9]{64}$"));
  if (!sha.match(descriptor).hasMatch()) { result.code = QStringLiteral("TRUST_RESPONSE_INVALID"); return result; }
  result.update.reset(new NativeVerifiedUpdate);
  result.update->offer = QJsonDocument(result.offer).toJson(QJsonDocument::Compact);
  result.update->descriptorSha256 = descriptor;
  result.update->installationRoot = lock.installationRoot();
  result.ok = true;
  return result;
}
NativeTrustResult NativeUpdateTrustBridge::rollbackAdmission(const QString &root,
    const QString &channel, const QString &buildId, const NativeInstallationLock &lock,
    const std::atomic_bool *cancellation) {
  auto result = request(QStringLiteral("inspect_accepted"), root, channel, {}, lock, cancellation);
  if (!result.ok) return result;
  result.ok = false;
  for (const auto &revoked : result.acceptedState.value(QStringLiteral("revokedBuildIds")).toArray())
    if (revoked.toString() == buildId) { result.code = QStringLiteral("BUILD_REVOKED"); return result; }
  const auto accepted = result.acceptedState.value(QStringLiteral("accepted")).toObject().value(buildId).toObject();
  const auto offer = accepted.value(QStringLiteral("offer")).toObject();
#ifdef GSW_UPDATER_FIXTURE_BUILD
  const QString product = QStringLiteral("Fubukisama.GaussianSceneWorkbench.UpdateFixture");
#else
  const QString product = QStringLiteral("Fubukisama.GaussianSceneWorkbench.Native");
#endif
  if (offer.value(QStringLiteral("buildId")).toString() != buildId ||
      offer.value(QStringLiteral("product")).toString() != product ||
      offer.value(QStringLiteral("platform")).toString() != QLatin1String("windows-x64")) {
    result.code = QStringLiteral("ROLLBACK_NOT_ACCEPTED"); return result;
  }
  result.update.reset(new NativeVerifiedUpdate);
  result.update->offer = QJsonDocument(offer).toJson(QJsonDocument::Compact);
  result.update->installationRoot = lock.installationRoot();
  result.update->historical = true;
  for (const auto &item : accepted.value(QStringLiteral("descriptorSha256s")).toArray())
    if (item.isString()) result.update->acceptedDescriptors.append(item.toString());
  if (result.update->acceptedDescriptors.isEmpty()) { result.code = QStringLiteral("ROLLBACK_NOT_ACCEPTED"); result.update.reset(); return result; }
  result.offer = offer; result.code = QStringLiteral("ACCEPTED_HISTORY"); result.ok = true;
  return result;
}
} // namespace gsw
