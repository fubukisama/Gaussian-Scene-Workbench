// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-only long-path regressions; does not execute the GSW application.
#include "NativeActivationCore.h"
#include "NativeActivationFiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class NativeUpdatePathTests final : public QObject {
  Q_OBJECT
  QTemporaryDir temporary{QDir(qEnvironmentVariable("GSW_UPDATE_TEST_ROOT",
      QCoreApplication::applicationDirPath())).filePath("update-long-path-XXXXXX")};

  QString longDirectory(const QString &label, qsizetype minimum) const {
    QString result = temporary.filePath(label);
    for (int i = 0; result.size() < minimum; ++i)
      result += QLatin1Char('/') + QString::fromUtf8("中文_日本語_") + QString::number(i) +
          QLatin1Char('_') + QString(44, QLatin1Char('x'));
    return result;
  }
private slots:
  void initTestCase() { QVERIFY(temporary.isValid()); }
  void locksARealLongCjkInstallationRootWithoutChangingItsIdentity() {
    const auto root = longDirectory(QString::fromLatin1("installation"), 350);
    QVERIFY(root.size() > 320);
    QVERIFY(QDir().mkpath(root));
    gsw::NativeInstallationLock lock(root);
    QVERIFY2(lock.locked(), qPrintable(lock.error()));
    QCOMPARE(lock.installationRoot(), QDir::cleanPath(root));
    QVERIFY(!lock.installationRoot().startsWith(QLatin1String("\\\\?\\")));
    gsw::NativeInstallationLock blocked(root);
    QVERIFY(!blocked.locked());
  }
  void securelyWritesAndReopensADeepCorrespondingSourcePath() {
    const auto directory = longDirectory(QString::fromLatin1("source/native/vendor"), 720);
    QVERIFY(directory.size() > 700);
    QVERIFY(QDir().mkpath(directory));
    const auto path = QDir(directory).filePath(QString::fromUtf8("源码_ソース.cpp"));
    const QByteArray bytes("complete owned corresponding-source bytes\n");
    try {
      {
        QFile output;
        gsw::activation::secureOutputOpen(output, path);
        QCOMPARE(output.write(bytes), qint64(bytes.size()));
        QVERIFY(output.flush());
      }
      QFile input;
      gsw::activation::heldRead(input, path);
      QCOMPARE(input.size(), qint64(bytes.size()));
      QCOMPARE(gsw::activation::hash(input), gsw::activation::digest(bytes));
    } catch (const gsw::activation::Failure &error) {
      QFAIL(qPrintable(error.code));
    }
  }
  void installsAndReverifiesACompleteArchiveInALongCjkRoot() {
    const auto base = longDirectory(QString::fromLatin1("archive"), 350);
    QVERIFY(QDir().mkpath(base));
    QProcess fixture;
    fixture.start(qEnvironmentVariable("GSW_UPDATE_TEST_PYTHON"),
        {QString::fromLatin1("-I"), QString::fromLatin1("-B"),
         qEnvironmentVariable("GSW_UPDATE_ACTIVATION_FIXTURE"), base});
    QVERIFY(fixture.waitForFinished(30000));
    QCOMPARE(fixture.exitCode(), 0);
    const auto offerPath = QDir(base).filePath(QString::fromLatin1("offer.json"));
    QFile offer(offerPath); QVERIFY(offer.open(QIODevice::ReadOnly));
    const auto raw = offer.readAll(); offer.close();
    const auto root = QDir(base).filePath(QString::fromLatin1("installation"));
    gsw::NativeInstallationLock lock(root);
    QVERIFY2(lock.locked(), qPrintable(lock.error()));
    const auto result = gsw::NativeDeveloperPublisher::publish(root,
        QDir(base).filePath(QString::fromLatin1("B.zip")), raw, lock);
    QVERIFY2(result.ok, qPrintable(result.code));
    try {
      const auto verified = gsw::activation::verifyInstalledSlot(
          QDir(root).filePath(QString::fromLatin1("versions/fixture-B")), gsw::activation::parse(raw));
      QVERIFY(!verified.executableSha256.isEmpty());
      QVERIFY(verified.entries.contains(QString::fromLatin1("source/.github/workflows/ci.yml")));
    } catch (const gsw::activation::Failure &error) {
      QFAIL(qPrintable(error.code));
    }
  }
};
QTEST_GUILESS_MAIN(NativeUpdatePathTests)
#include "NativeUpdatePathTests.moc"
