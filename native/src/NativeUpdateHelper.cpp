// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"
#include "NativeUpdateTrustBridge.h"
#include "NativeUpdateDownload.h"
#include "NativeActivationFiles.h"
#include "NativeInstallation.h"
#include "NativeStartupHealth.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
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

QString healthRecordPath(const QString &root) {
  return QDir(root).filePath(QStringLiteral("updates/startup-health.json"));
}
void saveHealth(const QString &root, const QJsonObject &status) noexcept {
  // Optional availability evidence, not update authority. Read-only roots,
  // damaged records or a full disk must never turn PROCESS_CREATED into failure.
  try {
    const auto path = healthRecordPath(root);
    gsw::activation::ensureDirectory(QFileInfo(path).absolutePath());
    gsw::activation::durableWrite(path, QJsonDocument(status).toJson(QJsonDocument::Compact), true);
  } catch (...) {}
}
void saveObservedHealthIfCurrent(const QString &root, const QJsonObject &status) noexcept {
  // Availability is optional. A later process can start while an older
  // observer is still waiting. Never let that older completion overwrite the
  // newer launch's record, and never wait on an ongoing update just to write
  // this advisory evidence. This lock admits no package or rollback.
  try {
    gsw::NativeInstallationLock lock(root, gsw::NativeInstallationLock::OpenMode::RequireExisting);
    if (!lock.locked()) return;
    const auto current = gsw::activation::parse(gsw::activation::read(healthRecordPath(root), 8192), 8192);
    const auto observed = gsw::activation::parse(QJsonDocument(status).toJson(QJsonDocument::Compact), 8192);
    if (current.at("schema") != "gsw.native_startup_status/1" ||
        observed.at("schema") != "gsw.native_startup_status/1") return;
    for (const auto *key : {"attemptId", "buildId", "executableSha256", "pid", "processCreationTime"})
      if (current.at("attempt").at(key) != observed.at("attempt").at(key)) return;
    saveHealth(root, status);
  } catch (...) {} // Corrupt, unwritable or contended evidence stays neutral.
}
int startupStatus(const QString &root) {
  QJsonObject fields;
  QString code = QStringLiteral("UNCONFIRMED");
  try {
    const auto value = gsw::activation::parse(gsw::activation::read(healthRecordPath(root), 8192), 8192);
    gsw::activation::exactKeys(value, {"schema", "code", "attempt"});
    if (gsw::activation::string(value.at("schema")) != QLatin1String("gsw.native_startup_status/1"))
      gsw::activation::fail("INVALID_REQUEST");
    const auto bytes = QByteArray::fromStdString(value.dump());
    const auto status = QJsonDocument::fromJson(bytes).object();
    const auto storedCode = status.value(QStringLiteral("code")).toString();
    const auto attempt = status.value(QStringLiteral("attempt")).toObject();
    const QSet<QString> baseKeys{"attemptId", "buildId", "executableSha256", "pid", "processCreationTime"};
    auto expectedKeys = baseKeys;
    if (storedCode == QLatin1String("HEALTHY")) expectedKeys.insert(QStringLiteral("healthScope"));
    else if (storedCode != QLatin1String("UNCONFIRMED")) gsw::activation::fail("INVALID_REQUEST");
    const auto names = attempt.keys();
    if (QSet<QString>(names.cbegin(), names.cend()) != expectedKeys) gsw::activation::fail("INVALID_REQUEST");
    static const QRegularExpression id(QStringLiteral("^[a-f0-9]{32}$"));
    static const QRegularExpression birth(QStringLiteral("^[a-f0-9]{16}$"));
    if (!id.match(attempt.value("attemptId").toString()).hasMatch() ||
        !birth.match(attempt.value("processCreationTime").toString()).hasMatch() ||
        attempt.value("processCreationTime").toString() == QLatin1String("0000000000000000") ||
        !attempt.value("pid").isDouble() || attempt.value("pid").toDouble() < 1 ||
        attempt.value("pid").toDouble() > 4294967295.0 ||
        attempt.value("pid").toDouble() != double(attempt.value("pid").toInteger())) gsw::activation::fail("INVALID_REQUEST");
    gsw::activation::buildId(gsw::activation::Json(attempt.value("buildId").toString().toStdString()));
    gsw::activation::sha(attempt.value("executableSha256").toString());
#ifdef GSW_UPDATER_FIXTURE_BUILD
    constexpr auto scope = "qt-fixture-window-surface";
#else
    constexpr auto scope = "main-window-base-viewport";
#endif
    if (storedCode == QLatin1String("HEALTHY") && attempt.value("healthScope") != QLatin1String(scope))
      gsw::activation::fail("INVALID_REQUEST");
    code = storedCode; fields.insert(QStringLiteral("attempt"), attempt);
  } catch (...) {} // Missing/malformed evidence is neutral; never rewrite it.
  event(QStringLiteral("startup_status"), code, fields);
  return 0;
}

int recoveryCandidates(const QString &root, const QString &channel, const gsw::NativeInstallationLock &lock) {
  auto history = gsw::NativeUpdateTrustBridge::request(QStringLiteral("inspect_accepted"), root, channel, {}, lock);
  if (!history.ok) return failure(history.code);
  const auto accepted = history.acceptedState.value(QStringLiteral("accepted")).toObject();
  if (accepted.size() > 1024) return failure(QStringLiteral("RECOVERY_CATALOG_LIMIT"));
  QSet<QString> revoked;
  for (const auto &item : history.acceptedState.value(QStringLiteral("revokedBuildIds")).toArray()) revoked.insert(item.toString());
  QJsonArray candidates;
  for (const auto &id : accepted.keys()) {
    if (revoked.contains(id)) continue;
    // Directory/receipt presence is never admission. Mint independent history
    // and verify the online receipt, immutable descriptor and COMPLETE slot.
    auto admission = gsw::NativeUpdateTrustBridge::rollbackAdmission(root, channel, id, lock);
    if (!admission.ok) continue;
    const auto verified = gsw::NativeActivationCore::verifyInstalled(root, *admission.update, lock);
    if (!verified.ok || verified.buildId != id) continue;
    // A public catalog never transports archive URLs or arbitrary signed text.
    // A non-version display label falls back to the safe immutable build ID;
    // the original admitted offer/descriptor is not changed.
    const auto display = admission.offer.value(QStringLiteral("displayVersion")).toString(id);
    static const QRegularExpression uriLike(QStringLiteral("(?:[A-Za-z][A-Za-z0-9+.-]*:|//|\\\\\\\\)"));
    bool safeDisplay = !display.isEmpty() && display.toUcs4().size() <= 128 && !uriLike.match(display).hasMatch();
    for (const auto character : display)
      if (character.category() == QChar::Other_Control) safeDisplay = false;
    candidates.append(QJsonObject{{"buildId", id},
      {"displayVersion", safeDisplay ? display : id}});
  }
  event(QStringLiteral("recovery_candidates"), QStringLiteral("ACCEPTED_HISTORY"), {{"candidates", candidates}});
  return 0;
}

QByteArray readLaunchInput() {
#ifdef Q_OS_WIN
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  if (input == INVALID_HANDLE_VALUE || GetFileType(input) != FILE_TYPE_PIPE)
    gsw::activation::fail("INVALID_REQUEST");
  QByteArray bytes;
  QElapsedTimer deadline; deadline.start();
  while (deadline.elapsed() < 5000) {
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE) return bytes;
      gsw::activation::fail("INVALID_REQUEST");
    }
    if (!available) { QThread::msleep(5); continue; }
    if (available > 64 * 1024 - DWORD(bytes.size())) gsw::activation::fail("INVALID_REQUEST");
    const qsizetype old = bytes.size();
    bytes.resize(old + available);
    DWORD received = 0;
    if (!ReadFile(input, bytes.data() + old, available, &received, nullptr))
      gsw::activation::fail("INVALID_REQUEST");
    bytes.resize(old + received);
  }
#endif
  gsw::activation::fail("INVALID_REQUEST");
}

int installedLaunch(const QString &operation, const QString &root,
    const gsw::activation::Json &intent) {
  using namespace gsw::activation;
  exactKeys(intent, {"schema", "buildId", "executableSha256", "arguments", "diagnostics"});
  if (string(intent.at("schema")) != QLatin1String("gsw.native_launch_intent/1") ||
      !intent.at("arguments").is_array() || intent.at("arguments").size() > 128 ||
      !intent.at("diagnostics").is_boolean()) fail("INVALID_REQUEST");
  const auto expected = buildId(intent.at("buildId"));
  const auto expectedHash = string(intent.at("executableSha256")); sha(expectedHash);
  QStringList arguments;
  for (const auto &value : intent.at("arguments")) {
    const auto argument = string(value);
    if (argument.contains(QChar(u'\0')) || argument.toUtf8().size() > 4096) fail("INVALID_REQUEST");
    arguments.append(argument);
  }
  const bool create = operation == QLatin1String("--launch-current");
  const bool diagnostics = intent.at("diagnostics").get<bool>();
  if (!create && (!arguments.isEmpty() || diagnostics)) fail("INVALID_REQUEST");
  auto lock = std::make_unique<gsw::NativeInstallationLock>(root,
      gsw::NativeInstallationLock::OpenMode::RequireExisting);
  if (!lock->locked()) return failure(lock->error());
  const auto pointer = parse(read(QDir(root).filePath(QStringLiteral("current.json")), 16384), 16384);
  exactKeys(pointer, {"schema", "version", "executableSha256"});
  if (string(pointer.at("schema")) != QLatin1String("gsw.native_active_version/1") ||
      buildId(pointer.at("version")) != expected || string(pointer.at("executableSha256")) != expectedHash)
    fail("LAUNCH_IDENTITY_CHANGED");
  auto accepted = gsw::NativeUpdateTrustBridge::rollbackAdmission(root, QStringLiteral("stable"), expected, *lock);
  if (!accepted.ok) return failure(accepted.code);
  const auto verified = gsw::NativeActivationCore::verifyInstalled(root, *accepted.update, *lock);
  if (!verified.ok) return failure(verified.code);
  if (verified.buildId != expected || verified.executableSha256 != expectedHash) fail("LAUNCH_IDENTITY_CHANGED");
  const auto fields = QJsonObject{{"buildId", expected}, {"executableSha256", expectedHash}};
  if (!create) { event(QStringLiteral("launch_verified"), QStringLiteral("VERIFIED"), fields); return 0; }
  // All authority, receipt, complete-slot and pointer comparisons happen under
  // this same lock. Never release it and return an executable for another
  // process to start after admission may have changed.
  auto plan = gsw::resolveNativeInstallation(root, diagnostics, arguments, QProcessEnvironment::systemEnvironment());
  if (!plan.valid() || plan.version != expected || plan.executableSha256 != expectedHash)
    fail("LAUNCH_IDENTITY_CHANGED");
  // Re-read the exact pointer under the held lock before process creation.
  if (parse(read(QDir(root).filePath(QStringLiteral("current.json")), 16384), 16384) != pointer)
    fail("LAUNCH_IDENTITY_CHANGED");
  QString startError;
  auto attempt = gsw::NativeStartupAttempt::start(plan, &startError);
  if (!attempt) return failure(startError);
  event(QStringLiteral("launched"), QStringLiteral("PROCESS_CREATED"), fields);
  try { saveHealth(root, attempt->initialStatus()); } catch (...) {}
  // The retained process handle now binds the optional observer to this exact
  // birth/image. Authority and CreateProcess were under one uninterrupted
  // installation lock; observing a live application must not hold that lock
  // for another15s and reject otherwise-valid updates or launches.
  lock.reset();
  // Exactly one canonical stdout result. Optional bounded observation must not
  // retract successful process creation, kill it, repoint current or restart it.
  try {
    saveObservedHealthIfCurrent(root, attempt->observe());
  } catch (...) {}
  return 0;
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  auto args = app.arguments().mid(1);
  // Deliberately no root, key, feed, verified, unsigned, dev or TLS-bypass CLI.
  if (args.isEmpty() || args.size() > 3) return failure(QStringLiteral("INVALID_REQUEST"));
  const QString operation = args.takeFirst();
  const bool launchOperation = operation == QLatin1String("--verify-installed") ||
      operation == QLatin1String("--launch-current");
  const bool healthOperation = operation == QLatin1String("--startup-status");
  const bool listOperation = operation == QLatin1String("--list-recovery");
  if ((launchOperation || healthOperation) && !args.isEmpty()) return failure(QStringLiteral("INVALID_REQUEST"));
  const QString channel = args.isEmpty() ? QStringLiteral("stable") : args.takeFirst();
  const QString expected = args.isEmpty() ? QString() : args.takeFirst();
  if (!QStringList{QStringLiteral("--check"), QStringLiteral("--prepare"),
      QStringLiteral("--install"), QStringLiteral("--apply-prepared"), QStringLiteral("--rollback"),
      QStringLiteral("--verify-installed"), QStringLiteral("--launch-current"), QStringLiteral("--startup-status"),
      QStringLiteral("--list-recovery")}.contains(operation) ||
      !QStringList{QStringLiteral("stable"), QStringLiteral("preview")}.contains(channel) ||
      (!expected.isEmpty() && operation != QLatin1String("--apply-prepared") && operation != QLatin1String("--rollback")) ||
      (operation == QLatin1String("--rollback") && expected.isEmpty())) return failure(QStringLiteral("INVALID_REQUEST"));
  QDir root(QCoreApplication::applicationDirPath());
  if (root.dirName() != QLatin1String("bin") || !root.cdUp()) return failure(QStringLiteral("activation_root_invalid"));
  gsw::activation::Json launchIntent;
  QFile heldLaunchHelper;
  if (launchOperation) {
    // Parse before acquiring the installation lock and before the cancellation
    // reader exists. That reader must never consume a launch intent.
    try {
#ifdef GSW_UPDATER_FIXTURE_BUILD
      const auto helperName = QString::fromLatin1("GSW Update Fixture Helper.exe");
#else
      const auto helperName = QString::fromLatin1("Gaussian Scene Workbench Update Helper.exe");
#endif
      if (QFileInfo(QCoreApplication::applicationFilePath()).fileName() != helperName)
        gsw::activation::fail("INVALID_REQUEST");
      gsw::activation::heldRead(heldLaunchHelper, QCoreApplication::applicationFilePath());
      launchIntent = gsw::activation::parse(readLaunchInput(), 64 * 1024);
    }
    catch (...) { return failure(QStringLiteral("INVALID_REQUEST")); }
  }
  // Advisory read-only query: it must also work while an updater holds the
  // activation lock, or current.json is malformed. It is never admission.
  if (healthOperation) return startupStatus(root.absolutePath());
  if (launchOperation) {
    try { return installedLaunch(operation, root.absolutePath(), launchIntent); }
    catch (const gsw::activation::Failure &error) { return failure(error.code); }
    catch (...) { return failure(QStringLiteral("INVALID_REQUEST")); }
  }
  gsw::NativeInstallationLock lock(root.absolutePath(), listOperation
      ? gsw::NativeInstallationLock::OpenMode::RequireExisting
      : gsw::NativeInstallationLock::OpenMode::CreateIfMissing);
  if (!lock.locked()) return failure(lock.error());
  if (listOperation) {
    // Pure read-only catalog, including a corrupt/missing current pointer.
    // Return before the stdin cancellation reader or any network/state path.
    try { return recoveryCandidates(root.absolutePath(), channel, lock); }
    catch (const gsw::activation::Failure &error) { return failure(error.code); }
    catch (...) { return failure(QStringLiteral("INVALID_REQUEST")); }
  }
  auto cancellation = std::make_shared<std::atomic_bool>(false);
  // The sole detached input reader owns only its shared atomic. It never uses
  // QObjects or library state during process shutdown. EOF is not cancellation.
  std::thread([cancellation] {
    char command[16]{};
    while (std::fgets(command, int(sizeof(command)), stdin))
      if (QByteArray(command).trimmed() == "cancel") cancellation->store(true);
  }).detach();
  try {
    if (operation == QLatin1String("--rollback")) {
      event(QStringLiteral("verifying"));
      auto accepted = gsw::NativeUpdateTrustBridge::rollbackAdmission(root.absolutePath(), channel, expected, lock, cancellation.get());
      if (!accepted.ok) return failure(accepted.code);
      const auto reverted = gsw::NativeActivationCore::rollback(root.absolutePath(), *accepted.update, lock);
      if (!reverted.ok) return failure(reverted.code);
      event(QStringLiteral("rolled_back"), QStringLiteral("NEXT_LAUNCH"), {{"buildId", reverted.buildId}}); return 0;
    }
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
      QElapsedTimer progressWindow;
      progressWindow.start();
      bool progressReported = false;
      timer.setInterval(100);
      QObject::connect(&timer, &QTimer::timeout, &loop, [&] { if (cancellation->load()) downloader.cancel(); });
      QObject::connect(&downloader, &gsw::NativeUpdateDownload::progress, &loop, [&](qint64 bytes, qint64 total) {
        const bool finalBytes = total > 0 && bytes == total;
        if (progressReported && !finalBytes && progressWindow.elapsed() < 100) return;
        event(QStringLiteral("downloading"), {}, {{"receivedBytes", double(bytes)}, {"totalBytes", double(total)}, {"buildId", build}});
        progressReported = true;
        progressWindow.restart();
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
