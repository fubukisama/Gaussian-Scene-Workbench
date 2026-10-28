// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class NativeUpdateActivationTests final : public QObject {
  Q_OBJECT
private slots:
  void publishesCompleteImmutableSlotAndSwitchesOnlyThePointer() {
    QTemporaryDir temporary(QDir(QCoreApplication::applicationDirPath()).filePath("activation-XXXXXX"));
    QVERIFY(temporary.isValid());
    QProcess fixture;
    fixture.start(qEnvironmentVariable("GSW_UPDATE_TEST_PYTHON"),
        {QStringLiteral("-I"), QStringLiteral("-B"),
         qEnvironmentVariable("GSW_UPDATE_ACTIVATION_FIXTURE"), temporary.path()});
    QVERIFY(fixture.waitForFinished(30000));
    QCOMPARE(fixture.exitCode(), 0);
    QFile offer(temporary.filePath("offer.json"));
    QVERIFY(offer.open(QIODevice::ReadOnly));
    const QString root = temporary.filePath("installation");
    gsw::NativeInstallationLock lock(root);
    const auto result = gsw::NativeDeveloperPublisher::publish(root,
        temporary.filePath("B.zip"), offer.readAll(), lock);
    QVERIFY2(result.ok, qPrintable(result.code));
    QFile pointer(QDir(root).filePath("current.json"));
    QVERIFY(pointer.open(QIODevice::ReadOnly));
    const auto active = QJsonDocument::fromJson(pointer.readAll()).object();
    QCOMPARE(active.value("version").toString(), QStringLiteral("fixture-B"));
    QVERIFY(QFileInfo::exists(QDir(root).filePath("versions/fixture-A/keep.txt")));
    QVERIFY(QFileInfo::exists(QDir(root).filePath("versions/fixture-B/source/.github/workflows/ci.yml")));
    QFile unchanged(QDir(root).filePath("bin/launcher-sentinel"));
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("unchanged-bootstrap"));
  }
};
QTEST_GUILESS_MAIN(NativeUpdateActivationTests)
#include "NativeUpdateActivationTests.moc"
