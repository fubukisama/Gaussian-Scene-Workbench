#include "NativeUpdateDownload.h"

#include <QFile>
#include <QDir>
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
};
QTEST_GUILESS_MAIN(NativeUpdateDownloadTests)
#include "NativeUpdateDownloadTests.moc"
