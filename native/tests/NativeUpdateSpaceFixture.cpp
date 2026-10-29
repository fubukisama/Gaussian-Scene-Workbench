// SPDX-License-Identifier: GPL-3.0-or-later
// Distinct remote-only CLI; neither an online admission nor a shipping helper.
#include "NativeUpdateDownload.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTextStream>

#ifndef GSW_REMOTE_SPACE_FIXTURE
#error This product must never be compiled as a shipping updater.
#endif

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QLatin1String("true") ||
      qEnvironmentVariable("RUNNER_OS") != QLatin1String("Windows") || argc != 3) return 64;
  QFile input(application.arguments().at(1));
  if (!input.open(QIODevice::ReadOnly) || input.size() > 16384) return 65;
  const auto metadata = QJsonDocument::fromJson(input.readAll()).object();
  if (metadata.value(QStringLiteral("schema")).toString() != QLatin1String("gsw.update_network_fixture/1") ||
      !metadata.value(QStringLiteral("testOnly")).toBool()) return 65;
  gsw::NativeUpdateDownloadPolicy policy;
  policy.approvedHosts = {QStringLiteral("127.0.0.1")};
  policy.maximumLength = 128LL * 1024 * 1024;
  policy.inactivityTimeoutMs = 10000; policy.totalTimeoutMs = 30000; policy.maximumAttempts = 1;
  policy.sslConfiguration = QSslConfiguration::defaultConfiguration();
  const auto certificates = QSslCertificate::fromPath(metadata.value(QStringLiteral("caPath")).toString());
  if (certificates.size() != 1) return 65;
  policy.sslConfiguration.addCaCertificates(certificates); // This fixture product only.
  gsw::NativeUpdateDownloadRequest request;
  request.url = QUrl(metadata.value(QStringLiteral("baseUrl")).toString() + QStringLiteral("/payload"));
  request.destinationPath = application.arguments().at(2);
  request.identity = QStringLiteral("owned-space-fixture-caller-identity");
  request.expectedLength = qint64(metadata.value(QStringLiteral("length")).toDouble());
  request.sha256 = metadata.value(QStringLiteral("sha256")).toString();
  gsw::NativeUpdateDownload download;
  int outcome = 2;
  QObject::connect(&download, &gsw::NativeUpdateDownload::finished, &application,
      [&](const gsw::NativeUpdateDownloadResult &result) {
    const QJsonObject record{{"schema", "gsw.update_space_download_fixture/1"}, {"testOnly", true},
      {"ok", result.ok}, {"code", result.code}, {"receivedBytes", double(result.receivedBytes)},
      {"httpStatus", result.httpStatus}, {"applicationStarted", false},
      {"publisherAuthenticated", false}, {"production", false}};
    QTextStream output(stdout); output << QJsonDocument(record).toJson(QJsonDocument::Compact) << '\n'; output.flush();
    outcome = result.ok ? 0 : 2;
    application.quit();
  });
  if (!download.start(request, policy)) return 65;
  application.exec();
  return outcome;
}
