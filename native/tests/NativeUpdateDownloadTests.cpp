#include "NativeUpdateDownload.h"

#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSslCertificate>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#ifdef Q_OS_WIN
#include "NativeUpdateLocalPath.h"
#include <windows.h>
#endif

using namespace gsw;
class NativeUpdateDownloadTests final : public QObject {
  Q_OBJECT
  QTemporaryDir temporary{QDir(qEnvironmentVariable("GSW_UPDATE_TEST_ROOT",
      QCoreApplication::applicationDirPath())).filePath("native-update-download-tests-XXXXXX")};
  QProcess server;
  QJsonObject fixture;

  NativeUpdateDownloadPolicy policy() const {
    NativeUpdateDownloadPolicy p;
    p.approvedHosts = {QStringLiteral("127.0.0.1")};
    p.inactivityTimeoutMs = 1500; p.totalTimeoutMs = 10000;
    p.maximumAttempts = 3; p.retryDelayMs = 20;
    QFile ca(fixture.value("caPath").toString());
    if (ca.open(QIODevice::ReadOnly))
      p.sslConfiguration.addCaCertificates(QSslCertificate::fromData(ca.readAll()));
    return p;
  }
  NativeUpdateDownloadRequest request(const QString &path, const QString &name) const {
    return {QUrl(fixture.value("baseUrl").toString() + path),
            temporary.filePath(name), QStringLiteral("test-only-owned-payload"),
            fixture.value("length").toInteger(), fixture.value("sha256").toString()};
  }
  static QByteArray read(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  }
  static NativeUpdateDownloadResult download(const NativeUpdateDownloadRequest &r,
                                              const NativeUpdateDownloadPolicy &p) {
    NativeUpdateDownload client;
    QSignalSpy done(&client, &NativeUpdateDownload::finished);
    if (!client.start(r, p)) return {false, false, QStringLiteral("test_start_rejected")};
    if (done.isEmpty() && !done.wait(12000)) return {false, false, QStringLiteral("test_deadline")};
    return qvariant_cast<NativeUpdateDownloadResult>(done.first().first());
  }
private slots:
  void initTestCase() {
    QVERIFY(temporary.isValid());
    const QString python = qEnvironmentVariable("GSW_UPDATE_TEST_PYTHON");
    QVERIFY2(!python.isEmpty(), "Remote update test Python must be explicitly selected.");
    const QString script = qEnvironmentVariable("GSW_UPDATE_NETWORK_FIXTURE",
        QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir().absoluteFilePath("../../scripts/native_update_network_fixture.py"));
    const QString ready = temporary.filePath("ready.json");
    server.setProgram(python);
    server.setArguments({QStringLiteral("-I"), QStringLiteral("-B"), script,
        QStringLiteral("--root"), temporary.path(), QStringLiteral("--ready"), ready});
    server.start();
    QVERIFY(server.waitForStarted(5000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready), 10000);
    fixture = QJsonDocument::fromJson(read(ready)).object();
    QVERIFY(fixture.value("testOnly").toBool());
    QCOMPARE(fixture.value("schema").toString(), QStringLiteral("gsw.update_network_fixture/1"));
  }
  void cleanupTestCase() {
    server.terminate();
    if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(3000); }
  }
  void downloadsRealHttpsBytesAndVerifiesExpectedLengthAndHash() {
    auto r = request(QStringLiteral("/payload"), QStringLiteral("verified.bin"));
    const auto result = download(r, policy());
    QVERIFY2(result.ok, qPrintable(result.code));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.receivedBytes, r.expectedLength);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
  }
  void followsAnApprovedRelativeHttpsRedirectWithoutCookiesOrUrlLogging() {
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    auto r = request(QStringLiteral("/redirect?test_only_token=never_log_this_fixture_value"),
        QStringLiteral("approved-redirect.bin"));
    const auto result = download(r, policy());
    QVERIFY2(result.ok, qPrintable(result.code));
    QCOMPARE(result.receivedBytes, r.expectedLength);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
    QList<QJsonObject> requests;
    const auto raw = read(logPath).mid(previousLogSize);
    QVERIFY(!raw.contains("never_log_this_fixture_value"));
    for (const auto &line : raw.split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (!row.isEmpty()) requests.append(row);
    }
    QCOMPARE(requests.size(), qsizetype(2));
    QCOMPARE(requests[0].value("path").toString(), QStringLiteral("/redirect"));
    QCOMPARE(requests[1].value("path").toString(), QStringLiteral("/payload"));
    for (const auto &row : requests) {
      QVERIFY(!row.value("cookie_present").toBool());
      QVERIFY(!row.value("authorization_present").toBool());
      QCOMPARE(row.value("accept_encoding").toString(), QStringLiteral("identity"));
    }
  }
  void retriesOneRealHttps503AndPublishesOnlyTheCompleteVerifiedBytes() {
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    auto r = request(QStringLiteral("/service-unavailable-once"), QStringLiteral("retry-503.bin"));
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write("previous verified destination"), qint64(29)); previous.close();
    const auto currentPath = temporary.filePath(QStringLiteral("retry-current.json"));
    QFile current(currentPath); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write("unchanged active version"), qint64(24)); current.close();
    auto p = policy(); p.maximumAttempts = 2;
    const auto result = download(r, p);
    QVERIFY2(result.ok, qPrintable(result.code));
    QCOMPARE(result.receivedBytes, r.expectedLength);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
    QCOMPARE(read(currentPath), QByteArray("unchanged active version"));
    QList<QJsonObject> requests;
    for (const auto &line : read(logPath).mid(previousLogSize).split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (!row.isEmpty()) requests.append(row);
    }
    QCOMPARE(requests.size(), qsizetype(p.maximumAttempts));
    for (const auto &row : requests) {
      QCOMPARE(row.value("path").toString(), QStringLiteral("/service-unavailable-once"));
      QVERIFY(row.value("range").toString().isEmpty());
      QVERIFY(!row.value("cookie_present").toBool());
      QVERIFY(!row.value("authorization_present").toBool());
    }
  }
  void retriesAnInterruptedHttpsBodyWithExactlyTheBoundStrongEtagPrefix() {
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    auto r = request(QStringLiteral("/drop-once"), QStringLiteral("retry-interrupted.bin"));
    const QByteArray original("existing verified package remains until final commit");
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write(original), qint64(original.size())); previous.close();
    const auto currentPath = temporary.filePath(QStringLiteral("interrupted-current.json"));
    const QByteArray active("active version must not change on a network retry");
    QFile current(currentPath); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write(active), qint64(active.size())); current.close();
    auto p = policy(); p.maximumAttempts = 2;
    NativeUpdateDownload client;
    QSignalSpy done(&client, &NativeUpdateDownload::finished);
    QSignalSpy progress(&client, &NativeUpdateDownload::progress);
    bool retainedExactPrefix = false, oldFilesUnchangedDuringRetry = false;
    QObject::connect(&client, &NativeUpdateDownload::stateChanged, &client,
        [&](NativeUpdateDownload::State state) {
      if (state != NativeUpdateDownload::State::RetryWait) return;
      const auto prefix = read(r.destinationPath + QStringLiteral(".part"));
      retainedExactPrefix = prefix.size() == 32768 &&
          prefix == read(fixture.value("payloadPath").toString()).left(32768);
      oldFilesUnchangedDuringRetry = read(r.destinationPath) == original && read(currentPath) == active;
    });
    QVERIFY(client.start(r, p));
    QVERIFY(done.wait(12000));
    const auto result = qvariant_cast<NativeUpdateDownloadResult>(done.first().first());
    QVERIFY2(result.ok, qPrintable(result.code));
    QVERIFY(result.resumed);
    QVERIFY(retainedExactPrefix);
    QVERIFY(oldFilesUnchangedDuringRetry);
    QCOMPARE(result.receivedBytes, r.expectedLength);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
    QCOMPARE(read(currentPath), active);
    qint64 previousReceived = 0;
    for (const auto &sample : progress) {
      const auto received = sample[0].toLongLong();
      const auto total = sample[1].toLongLong();
      QCOMPARE(total, r.expectedLength);
      QVERIFY(received >= previousReceived && received <= total);
      previousReceived = received;
    }
    QList<QJsonObject> requests;
    for (const auto &line : read(logPath).mid(previousLogSize).split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (!row.isEmpty()) requests.append(row);
    }
    QCOMPARE(requests.size(), qsizetype(p.maximumAttempts));
    QCOMPARE(requests[0].value("path").toString(), QStringLiteral("/drop-once"));
    QVERIFY(requests[0].value("range").toString().isEmpty());
    QCOMPARE(requests[1].value("path").toString(), QStringLiteral("/drop-once"));
    QCOMPARE(requests[1].value("range").toString(), QStringLiteral("bytes=32768-"));
    QCOMPARE(requests[1].value("if_range").toString(), QStringLiteral("\"owned-payload-a\""));
    QTest::qWait(100);
    QCOMPARE(done.size(), qsizetype(1));
  }
  void cancellationRetainsOwnedStrongEtagPrefixAndResumesActualHttpsRange() {
    auto r = request(QStringLiteral("/slow"), QStringLiteral("resume.bin"));
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write("previous destination"), qint64(20)); previous.close();
    const QString pointer = temporary.filePath(QStringLiteral("current.json"));
    QFile current(pointer); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write("unchanged active release"), qint64(24)); current.close();
    NativeUpdateDownload client;
    QSignalSpy cancelled(&client, &NativeUpdateDownload::finished);
    QObject::connect(&client, &NativeUpdateDownload::progress, &client,
        [&client](qint64 received, qint64) { if (received >= 32768) client.cancel(); });
    QVERIFY(client.start(r, policy()));
    QVERIFY(cancelled.wait(8000));
    const auto first = qvariant_cast<NativeUpdateDownloadResult>(cancelled.first().first());
    QVERIFY(first.cancelled); QVERIFY(!first.ok);
    QCOMPARE(read(r.destinationPath), QByteArray("previous destination"));
    QCOMPARE(read(pointer), QByteArray("unchanged active release"));
    const qint64 prefix = QFileInfo(r.destinationPath + QStringLiteral(".part")).size();
    QVERIFY(prefix >= 32768 && prefix < r.expectedLength);
    const auto result = download(r, policy());
    QVERIFY2(result.ok, qPrintable(result.code));
    QVERIFY(result.resumed);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
    QCOMPARE(read(pointer), QByteArray("unchanged active release"));
    bool sawBoundRange = false;
    for (const auto &line : read(fixture.value("requestLog").toString()).split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row.value("path").toString() == QLatin1String("/slow") &&
          row.value("range").toString() == QStringLiteral("bytes=%1-").arg(prefix) &&
          row.value("if_range").toString() == QLatin1String("\"owned-payload-a\"")) sawBoundRange = true;
    }
    QVERIFY(sawBoundRange);
  }
  void downloadsRealHttpsBytesToALongCjkOwnedCachePath() {
    QString directory = temporary.filePath(QString::fromLatin1("long-cache"));
    for (int i = 0; directory.size() < 350; ++i)
      directory += QLatin1Char('/') + QString::fromUtf8("缓存_日本語_") + QString::number(i) +
          QLatin1Char('_') + QString(42, QLatin1Char('x'));
    QVERIFY(QDir().mkpath(directory));
    auto r = request(QString::fromLatin1("/payload"), QString::fromLatin1("placeholder.bin"));
    r.destinationPath = QDir(directory).filePath(QString::fromUtf8("更新_アップデート.zip"));
    QVERIFY(r.destinationPath.size() > 350);
    const auto result = download(r, policy());
    QVERIFY2(result.ok, qPrintable(result.code));
    QCOMPARE(result.receivedBytes, r.expectedLength);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
  }
  void rejectsDeviceNamespaceAndAdsDestinationsWithoutPublishing() {
#ifdef Q_OS_WIN
    const auto local = temporary.filePath("namespace-test.bin");
    const QStringList destinations{gsw::updatepaths::nativeApiPath(local),
      local + QStringLiteral(":stream"), temporary.filePath("CON.bin"),
      temporary.filePath(QString::fromUtf8("LPT¹.bin")), temporary.filePath("trailing.")};
    for (const auto &path : destinations) {
      auto r = request(QStringLiteral("/payload"), QStringLiteral("unused-destination.bin"));
      r.destinationPath = path;
      const auto result = download(r, policy());
      QVERIFY(!result.ok); QCOMPARE(result.code, QStringLiteral("download_path_unsafe"));
    }
#endif
  }
  void refusesHardlinkedOwnedPartWithoutChangingTheOriginal() {
#ifdef Q_OS_WIN
    const auto source = temporary.filePath("retained-part-original.bin");
    const QByteArray before("unrelated owned fixture must remain unchanged");
    QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(before), qint64(before.size())); file.close();
    auto r = request(QStringLiteral("/payload"), QStringLiteral("hardlink-destination.bin"));
    const auto nativePart = gsw::updatepaths::nativeApiPath(r.destinationPath + QStringLiteral(".part"));
    const auto nativeSource = gsw::updatepaths::nativeApiPath(source);
    QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(nativePart.utf16()),
                           reinterpret_cast<LPCWSTR>(nativeSource.utf16()), nullptr));
    const auto result = download(r, policy());
    QVERIFY(!result.ok); QCOMPARE(result.code, QStringLiteral("download_path_unsafe"));
    QCOMPARE(read(source), before); QVERIFY(!QFileInfo::exists(r.destinationPath));
#endif
  }
  void safelyRestartsAnUnusableResumeResponse_data() {
    QTest::addColumn<QString>("endpoint");
    QTest::addColumn<int>("expectedRequests");
    QTest::newRow("changed-strong-etag") << QStringLiteral("/changed-etag") << 2;
    QTest::newRow("invalid-content-range") << QStringLiteral("/bad-range") << 2;
    QTest::newRow("range-ignored-full-200") << QStringLiteral("/range-ignored") << 1;
  }
  void safelyRestartsAnUnusableResumeResponse() {
    QFETCH(QString, endpoint);
    QFETCH(int, expectedRequests);
    auto r = request(QStringLiteral("/slow"), QStringLiteral("resume-restart-") + endpoint.mid(1) + QStringLiteral(".bin"));
    const QByteArray original("original destination is not a download work file");
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write(original), qint64(original.size())); previous.close();
    const auto currentPath = r.destinationPath + QStringLiteral(".current.json");
    const QByteArray active("active release remains untouched");
    QFile current(currentPath); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write(active), qint64(active.size())); current.close();
    {
      NativeUpdateDownload first;
      QSignalSpy done(&first, &NativeUpdateDownload::finished);
      QObject::connect(&first, &NativeUpdateDownload::progress, &first,
          [&first](qint64 bytes, qint64) { if (bytes >= 32768) first.cancel(); });
      QVERIFY(first.start(r, policy()));
      QVERIFY(done.wait(8000));
      QVERIFY(qvariant_cast<NativeUpdateDownloadResult>(done.first().first()).cancelled);
    }
    const auto prefix = QFileInfo(r.destinationPath + QStringLiteral(".part")).size();
    QVERIFY(prefix >= 32768 && prefix < r.expectedLength);
    QCOMPARE(read(r.destinationPath), original);
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    r.url = request(endpoint, QStringLiteral("unused.bin")).url;
    auto p = policy(); p.maximumAttempts = 2;
    const auto result = download(r, p);
    QVERIFY2(result.ok, qPrintable(result.code));
    QVERIFY(!result.resumed); // No bytes from the rejected 206 may be spliced.
    QCOMPARE(result.receivedBytes, r.expectedLength);
    QCOMPARE(read(r.destinationPath), read(fixture.value("payloadPath").toString()));
    QCOMPARE(read(currentPath), active);
    QList<QJsonObject> requests;
    for (const auto &line : read(logPath).mid(previousLogSize).split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (!row.isEmpty()) requests.append(row);
    }
    QCOMPARE(requests.size(), qsizetype(expectedRequests));
    QCOMPARE(requests[0].value("path").toString(), endpoint);
    QCOMPARE(requests[0].value("range").toString(), QStringLiteral("bytes=%1-").arg(prefix));
    QCOMPARE(requests[0].value("if_range").toString(), QStringLiteral("\"owned-payload-a\""));
    if (expectedRequests == 2) {
      QCOMPARE(requests[1].value("path").toString(), endpoint);
      QVERIFY(requests[1].value("range").toString().isEmpty());
      QVERIFY(requests[1].value("if_range").toString().isEmpty());
    }
    QVERIFY(!QFileInfo::exists(r.destinationPath + QStringLiteral(".part")));
    QVERIFY(!QFileInfo::exists(r.destinationPath + QStringLiteral(".resume.json")));
  }
  void doesNotWaitAnUntrustedDayLongRetryAfterOrChangeExistingFiles() {
    auto r = request(QStringLiteral("/error429"), QStringLiteral("rate-limit.bin"));
    const QByteArray original("existing signed package before rate limiting");
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write(original), qint64(original.size())); previous.close();
    const auto currentPath = r.destinationPath + QStringLiteral(".current.json");
    QFile current(currentPath); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write(original), qint64(original.size())); current.close();
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    auto p = policy(); p.maximumAttempts = 2; p.totalTimeoutMs = 1000;
    QElapsedTimer clock; clock.start();
    const auto result = download(r, p);
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QStringLiteral("download_http_error"));
    QVERIFY(clock.elapsed() < 3000);
    QCOMPARE(read(r.destinationPath), original); QCOMPARE(read(currentPath), original);
    qsizetype requests = 0;
    for (const auto &line : read(logPath).mid(previousLogSize).split('\n'))
      if (!QJsonDocument::fromJson(line).object().isEmpty()) ++requests;
    QVERIFY(requests >= 1 && requests <= p.maximumAttempts);
  }
  void rejectsAnUntrustedHttpsCertificateWithoutRetryOrPublication() {
    auto r = request(QStringLiteral("/payload?test_only_token=never_log_tls_secret"), QStringLiteral("untrusted-tls.bin"));
    const QByteArray original("existing bytes survive a certificate failure");
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write(original), qint64(original.size())); previous.close();
    const auto currentPath = r.destinationPath + QStringLiteral(".current.json");
    QFile current(currentPath); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write(original), qint64(original.size())); current.close();
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    auto p = policy(); p.sslConfiguration = QSslConfiguration::defaultConfiguration();
    NativeUpdateDownload client;
    QSignalSpy done(&client, &NativeUpdateDownload::finished);
    bool retried = false;
    QObject::connect(&client, &NativeUpdateDownload::stateChanged, &client,
        [&](NativeUpdateDownload::State state) { if (state == NativeUpdateDownload::State::RetryWait) retried = true; });
    QVERIFY(client.start(r, p)); QVERIFY(done.wait(12000));
    const auto result = qvariant_cast<NativeUpdateDownloadResult>(done.first().first());
    QVERIFY(!result.ok); QCOMPARE(result.code, QStringLiteral("download_tls_failed"));
    QVERIFY(!retried); QVERIFY(!result.code.contains(QStringLiteral("never_log_tls_secret")));
    QCOMPARE(read(r.destinationPath), original); QCOMPARE(read(currentPath), original);
    QVERIFY(read(logPath).mid(previousLogSize).trimmed().isEmpty()); // TLS must fail before HTTP bytes.
    QVERIFY(!QFileInfo::exists(r.destinationPath + QStringLiteral(".part")));
  }
  void retryBackoffRemainsBoundedAndCancellable_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("attempt-exhaustion") << QStringLiteral("exhaustion");
    QTest::newRow("original-total-deadline") << QStringLiteral("deadline");
    QTest::newRow("cancel-during-backoff") << QStringLiteral("cancel");
  }
  void retryBackoffRemainsBoundedAndCancellable() {
    QFETCH(QString, mode);
    auto r = request(QStringLiteral("/service-unavailable-always"), QStringLiteral("bounded-") + mode + QStringLiteral(".bin"));
    const QByteArray original("original release is not a retry scratch file");
    QFile previous(r.destinationPath); QVERIFY(previous.open(QIODevice::WriteOnly));
    QCOMPARE(previous.write(original), qint64(original.size())); previous.close();
    const auto currentPath = r.destinationPath + QStringLiteral(".current.json");
    QFile current(currentPath); QVERIFY(current.open(QIODevice::WriteOnly));
    QCOMPARE(current.write(original), qint64(original.size())); current.close();
    const auto logPath = fixture.value("requestLog").toString();
    const auto previousLogSize = read(logPath).size();
    auto p = policy(); p.maximumAttempts = 2;
    p.retryDelayMs = mode == QLatin1String("deadline") ? 1500 : mode == QLatin1String("cancel") ? 150 : 10;
    if (mode == QLatin1String("deadline")) p.totalTimeoutMs = 700;
    NativeUpdateDownload client;
    QSignalSpy done(&client, &NativeUpdateDownload::finished);
    bool observedBackoff = false;
    QObject::connect(&client, &NativeUpdateDownload::stateChanged, &client,
        [&](NativeUpdateDownload::State state) {
      if (state != NativeUpdateDownload::State::RetryWait) return;
      observedBackoff = true;
      QCOMPARE(read(r.destinationPath), original); QCOMPARE(read(currentPath), original);
      if (mode == QLatin1String("cancel")) client.cancel();
    });
    QElapsedTimer clock; clock.start();
    QVERIFY(client.start(r, p)); QVERIFY(done.wait(8000));
    const auto result = qvariant_cast<NativeUpdateDownloadResult>(done.first().first());
    QVERIFY(!result.ok); QVERIFY(observedBackoff);
    const auto expected = mode == QLatin1String("cancel") ? QStringLiteral("download_cancelled") :
        mode == QLatin1String("deadline") ? QStringLiteral("download_total_timeout") : QStringLiteral("download_http_error");
    QCOMPARE(result.code, expected);
    QCOMPARE(result.cancelled, mode == QLatin1String("cancel"));
    QVERIFY(clock.elapsed() < 5000); // Never adopts the untrusted day-long Retry-After.
    QTest::qWait(p.retryDelayMs + 100); // Old queued retry must not issue after terminal state.
    QCOMPARE(done.size(), qsizetype(1));
    QCOMPARE(read(r.destinationPath), original); QCOMPARE(read(currentPath), original);
    qsizetype requests = 0;
    for (const auto &line : read(logPath).mid(previousLogSize).split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row.isEmpty()) continue;
      QCOMPARE(row.value("path").toString(), QStringLiteral("/service-unavailable-always"));
      QVERIFY(row.value("range").toString().isEmpty());
      ++requests;
    }
    QCOMPARE(requests, qsizetype(mode == QLatin1String("exhaustion") ? 2 : 1));
    QVERIFY(!QFileInfo::exists(r.destinationPath + QStringLiteral(".part")));
    QVERIFY(!QFileInfo::exists(r.destinationPath + QStringLiteral(".resume.json")));
  }
};
QTEST_GUILESS_MAIN(NativeUpdateDownloadTests)
#include "NativeUpdateDownloadTests.moc"
