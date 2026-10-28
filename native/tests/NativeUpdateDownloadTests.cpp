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

using namespace gsw;
class NativeUpdateDownloadTests final : public QObject {
  Q_OBJECT
  QTemporaryDir temporary;
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
};
QTEST_GUILESS_MAIN(NativeUpdateDownloadTests)
#include "NativeUpdateDownloadTests.moc"
