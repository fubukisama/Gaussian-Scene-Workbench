// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"
#include "NativeUpdateTrustBridge.h"
#include "NativeUpdateDownload.h"
#include "NativeActivationFiles.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

namespace {
void event(const QString &phase, const QString &code = {}, const QJsonObject &fields = {}) {
  auto message = fields;
  message.insert(QStringLiteral("schema"), QStringLiteral("gsw.update_event/1"));
  message.insert(QStringLiteral("phase"), phase);
  if (!code.isEmpty()) message.insert(QStringLiteral("code"), code);
  QTextStream stream(stdout);
  stream << QJsonDocument(message).toJson(QJsonDocument::Compact) << '\n'; stream.flush();
}
int failure(const QString &code) { event(QStringLiteral("failed"), code); return 2; }
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  auto args = app.arguments().mid(1);
  // Deliberately no root, key, feed, verified, unsigned, dev or TLS-bypass CLI.
  if (args.isEmpty() || args.size() > 3) return failure(QStringLiteral("INVALID_REQUEST"));
  const QString operation = args.takeFirst();
  const QString channel = args.isEmpty() ? QStringLiteral("stable") : args.takeFirst();
  const QString expected = args.isEmpty() ? QString() : args.takeFirst();
  if (!QStringList{QStringLiteral("--check"), QStringLiteral("--prepare"),
      QStringLiteral("--install"), QStringLiteral("--apply-prepared")}.contains(operation) ||
      !QStringList{QStringLiteral("stable"), QStringLiteral("preview")}.contains(channel) ||
      (!expected.isEmpty() && operation != QLatin1String("--apply-prepared"))) return failure(QStringLiteral("INVALID_REQUEST"));
  QDir root(QCoreApplication::applicationDirPath());
  if (root.dirName() != QLatin1String("bin") || !root.cdUp()) return failure(QStringLiteral("activation_root_invalid"));
  gsw::NativeInstallationLock lock(root.absolutePath());
  if (!lock.locked()) return failure(lock.error());
  auto cancellation = std::make_shared<std::atomic_bool>(false);
  // The sole detached input reader owns only its shared atomic. It never uses
  // QObjects or library state during process shutdown. EOF is not cancellation.
  std::thread([cancellation] {
    char command[16]{};
    while (std::fgets(command, int(sizeof(command)), stdin))
      if (QByteArray(command).trimmed() == "cancel") cancellation->store(true);
  }).detach();
  try {
    QString build = expected;
    const QString preparedPath = root.filePath(QStringLiteral("updates/prepared.json"));
    if (operation == QLatin1String("--apply-prepared")) {
      const auto prepared = gsw::activation::parse(gsw::activation::read(preparedPath, 16384), 16384);
      gsw::activation::exactKeys(prepared, {"schema", "buildId", "channel", "archiveSha256", "descriptorSha256"});
      if (gsw::activation::string(prepared.at("schema")) != QLatin1String("gsw.prepared_update/1") ||
          gsw::activation::string(prepared.at("channel")) != channel) return failure(QStringLiteral("PREPARED_IDENTITY_INVALID"));
      const auto preparedBuild = gsw::activation::buildId(prepared.at("buildId"));
      if (!build.isEmpty() && build != preparedBuild) return failure(QStringLiteral("PREPARED_IDENTITY_INVALID"));
      build = preparedBuild;
    }
    event(QStringLiteral("checking"));
    auto trust = gsw::NativeUpdateTrustBridge::request(
        operation == QLatin1String("--apply-prepared") ? QStringLiteral("verify_cached") : QStringLiteral("resolve"),
        root.absolutePath(), channel, build, lock, cancellation.get());
    if (!trust.ok) return failure(trust.code);
    build = trust.offer.value(QStringLiteral("buildId")).toString();
    // Never expose download URLs, signed query credentials or transport data
    // in stdout/UI logs. Release notes remain plain text at the UI boundary.
    QJsonObject display;
    for (const auto &key : {"buildId", "displayVersion", "channel", "archiveLength", "releaseNotes"})
      if (trust.offer.contains(QLatin1String(key))) display.insert(QLatin1String(key), trust.offer.value(QLatin1String(key)));
    event(QStringLiteral("available"), QStringLiteral("VERIFIED"), {{"offer", display}});
    if (operation == QLatin1String("--check")) return 0;
    const QString archiveHash = trust.offer.value(QStringLiteral("archiveSha256")).toString();
    gsw::activation::sha(archiveHash);
    gsw::activation::buildId(gsw::activation::Json(build.toStdString()));
    const QString downloads = root.filePath(QStringLiteral("updates/downloads"));
    gsw::activation::ensureDirectory(downloads);
    const QString archive = QDir(downloads).filePath(build + QLatin1Char('-') + archiveHash + QStringLiteral(".zip"));
    if (operation != QLatin1String("--apply-prepared")) {
      gsw::NativeUpdateDownload downloader;
      gsw::NativeUpdateDownloadRequest request;
      request.url = QUrl(trust.offer.value(QStringLiteral("archiveUrl")).toString());
      request.destinationPath = archive;
      request.identity = build + QLatin1Char(':') + archiveHash;
      request.expectedLength = qint64(trust.offer.value(QStringLiteral("archiveLength")).toDouble());
      request.sha256 = archiveHash;
      QEventLoop loop;
      QTimer timer;
      timer.setInterval(100);
      QObject::connect(&timer, &QTimer::timeout, &loop, [&] { if (cancellation->load()) downloader.cancel(); });
      QObject::connect(&downloader, &gsw::NativeUpdateDownload::progress, &loop, [&](qint64 bytes, qint64 total) {
        event(QStringLiteral("downloading"), {}, {{"receivedBytes", double(bytes)}, {"totalBytes", double(total)}, {"buildId", build}});
      });
      gsw::NativeUpdateDownloadResult downloaded;
      QObject::connect(&downloader, &gsw::NativeUpdateDownload::finished, &loop,
          [&](const auto &result) { downloaded = result; loop.quit(); });
      timer.start();
      downloader.start(request, trust.downloadPolicy); loop.exec(); timer.stop();
      if (!downloaded.ok) return failure(downloaded.code);
    }
    if (cancellation->load()) return failure(QStringLiteral("CANCELLED"));
    event(QStringLiteral("verifying"));
    // Revalidate cached original signed role bytes after the possibly long
    // download. A GUI assertion, stale offer object or cache marker is no grant.
    auto finalTrust = gsw::NativeUpdateTrustBridge::request(QStringLiteral("verify_cached"),
        root.absolutePath(), channel, build, lock, cancellation.get());
    if (!finalTrust.ok) return failure(finalTrust.code);
    if (finalTrust.offer != trust.offer) return failure(QStringLiteral("UPDATE_IDENTITY_CHANGED"));
    if (operation == QLatin1String("--prepare")) {
      const auto prepared = gsw::activation::Json{{"schema", "gsw.prepared_update/1"},
        {"buildId", build.toStdString()}, {"channel", channel.toStdString()},
        {"archiveSha256", archiveHash.toStdString()}, {"descriptorSha256", finalTrust.update->descriptorDigest().toStdString()}};
      // This record is intent only: apply always independently verifies trust
      // and all archive bytes again before activation.
      gsw::activation::durableWrite(preparedPath, QByteArray::fromStdString(prepared.dump()), true);
      event(QStringLiteral("prepared"), QStringLiteral("READY_FOR_NEXT_LAUNCH"), {{"buildId", build}}); return 0;
    }
    event(QStringLiteral("staging"), {}, {{"buildId", build}});
    const auto activated = gsw::NativeActivationCore::install(root.absolutePath(), archive, *finalTrust.update, lock);
    if (!activated.ok) return failure(activated.code);
    event(QStringLiteral("activated"), QStringLiteral("NEXT_LAUNCH"), {{"buildId", activated.buildId}});
    return 0;
  } catch (const gsw::activation::Failure &error) { return failure(error.code); }
    catch (...) { return failure(QStringLiteral("UPDATE_FAILED")); }
}
