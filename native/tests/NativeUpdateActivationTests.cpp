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
#ifdef Q_OS_WIN
#include <windows.h>
#endif

class NativeUpdateActivationTests final : public QObject {
  Q_OBJECT
private slots:
  void readOnlyStartupAdmissionDoesNotRequireWriteAccessToExistingLock() {
#ifdef Q_OS_WIN
    QTemporaryDir root(QDir(QCoreApplication::applicationDirPath()).filePath("readonly-startup-lock-XXXXXX"));
    QVERIFY(root.isValid());
    const QString path = QDir::toNativeSeparators(root.filePath(QStringLiteral(".publish.lock")));
    {
      gsw::NativeInstallationLock initialPublisher(root.path());
      QVERIFY(initialPublisher.locked());
    }
    const DWORD previous = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    QVERIFY(previous != INVALID_FILE_ATTRIBUTES);
    struct RestoreAttributes final {
      const QString &path;
      DWORD attributes;
      ~RestoreAttributes() { SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), attributes); }
    } restore{path, previous};
    const DWORD readOnly = (previous & ~DWORD(FILE_ATTRIBUTE_NORMAL)) | FILE_ATTRIBUTE_READONLY;
    QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), readOnly));
    QVERIFY(GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16())) & FILE_ATTRIBUTE_READONLY);
    {
      gsw::NativeInstallationLock admission(root.path(), gsw::NativeInstallationLock::OpenMode::RequireExisting);
      QVERIFY2(admission.locked(), qPrintable(admission.error()));
      QFile competingRead(path);
      QVERIFY(!competingRead.open(QIODevice::ReadOnly));
      gsw::NativeInstallationLock competingPublisher(root.path());
      QVERIFY(!competingPublisher.locked());
    }
    QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), previous));
    gsw::NativeInstallationLock publisherAfter(root.path());
    QVERIFY(publisherAfter.locked());
#else
    QSKIP("Windows file-attribute contract");
#endif
  }
  void readOnlyAdmissionLockRequiresExistingStateAndSharesPublisherExclusion() {
    QTemporaryDir root(QDir(QCoreApplication::applicationDirPath()).filePath("startup-lock-XXXXXX"));
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral(".publish.lock"));
    gsw::NativeInstallationLock missing(root.path(), gsw::NativeInstallationLock::OpenMode::RequireExisting);
    QVERIFY(!missing.locked());
    QVERIFY(!QFileInfo::exists(path));
    {
      gsw::NativeInstallationLock publisher(root.path());
      QVERIFY(publisher.locked());
      QVERIFY(QFileInfo::exists(path));
      gsw::NativeInstallationLock contended(root.path(), gsw::NativeInstallationLock::OpenMode::RequireExisting);
      QVERIFY(!contended.locked());
    }
    gsw::NativeInstallationLock readOnly(root.path(), gsw::NativeInstallationLock::OpenMode::RequireExisting);
    QVERIFY(readOnly.locked());
    QFile lockFile(path);
    // The exclusive guard is released by the owner's destructor, not by a
    // validation return value or a JSON receipt produced by another process.
    QVERIFY(!lockFile.open(QIODevice::ReadOnly));
  }
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
    const auto offerBytes = offer.readAll();
    const QString root = temporary.filePath("installation");
    gsw::NativeInstallationLock lock(root);
    const auto result = gsw::NativeDeveloperPublisher::publish(root,
        temporary.filePath("B.zip"), offerBytes, lock);
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
    // A completed slot and a retry after pointer failure must not become a
    // permanent collision. Re-entry verifies bytes instead of overwriting B.
    const auto repeated = gsw::NativeDeveloperPublisher::publish(root,
        temporary.filePath("B.zip"), offerBytes, lock);
    QVERIFY2(repeated.ok, qPrintable(repeated.code));
  }
};
QTEST_GUILESS_MAIN(NativeUpdateActivationTests)
#include "NativeUpdateActivationTests.moc"
