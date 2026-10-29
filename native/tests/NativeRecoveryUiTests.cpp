// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-only private QtTest product. The recovery module calls the ACTUAL
// fixed fixture Helper/TUF/complete-slot verifier; no verified bool or mock.
#include "NativeRecovery.h"
#include "AppLanguage.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifndef GSW_RECOVERY_QT_TEST
#error This source belongs only to the private remote fixture target.
#endif

namespace {
QByteArray bytes(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QMap<QString, QByteArray> hashes(const QString &root) {
  QMap<QString, QByteArray> result;
  QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const auto path = it.next();
    result.insert(QDir(root).relativeFilePath(path), QCryptographicHash::hash(bytes(path), QCryptographicHash::Sha256));
  }
  return result;
}
bool writeBytes(const QString &path, const QByteArray &data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}
QString activeVersion(const QString &root) {
  return QJsonDocument::fromJson(bytes(QDir(root).filePath("current.json"))).object().value("version").toString();
}
void driveConfirmation(QTimer &timer, gsw::NativeRecoveryWindow &window,
    QMessageBox::StandardButton choice, bool &seen) {
  auto deadline = std::make_shared<QElapsedTimer>(); deadline->start();
  timer.setInterval(20);
  QObject::connect(&timer, &QTimer::timeout, &window, [&timer, &window, choice, &seen, deadline] {
    if (deadline->elapsed() > 5000) {
      timer.stop();
      // Release only an unexpected modal owned by THIS test window so a bad
      // confirmation implementation fails an assertion instead of hanging.
      for (auto *owned : window.findChildren<QMessageBox *>())
        if (owned->isVisible()) owned->reject();
      return;
    }
    auto *box = window.findChild<QMessageBox *>(QStringLiteral("recoveryConfirm"));
    if (!box || !box->isVisible() || !box->button(choice)) return;
    seen = true; timer.stop();
    QTest::mouseClick(box->button(choice), Qt::LeftButton);
  });
  timer.start();
}
}

class NativeRecoveryUiTests final : public QObject {
  Q_OBJECT
private slots:
  void realTrustedChooserRechecksTamperingAndRestoresWithoutStartingApplication() {
    const auto root = qEnvironmentVariable("GSW_RECOVERY_TEST_INSTALLATION");
    const auto evidence = qEnvironmentVariable("GSW_RECOVERY_TEST_EVIDENCE");
    QVERIFY(QDir::isAbsolutePath(root));
    QVERIFY(QDir::isAbsolutePath(evidence));
    const auto pointerPath = QDir(root).filePath("current.json");
    const auto malformed = bytes(pointerPath);
    QCOMPARE(malformed, QByteArray("owned invalid current for trusted Recovery chooser"));
    const auto allBefore = hashes(root);
    const auto trustBefore = hashes(QDir(root).filePath("updates/trust"));
    const auto anchorBefore = bytes(QDir(root).filePath(".update-trust-anchor"));
    const auto versionsBefore = hashes(QDir(root).filePath("versions"));
    const auto bootstrapBefore = hashes(QDir(root).filePath("bin"));
    const auto dataBefore = hashes(QDir(root).filePath("user-data"));
    gsw::NativeRecoveryWindow window(root);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *list = window.findChild<QListWidget *>(QStringLiteral("recoveryCandidates"));
    auto *restore = window.findChild<QPushButton *>(QStringLiteral("recoveryRestore"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("recoveryStatus"));
    auto *language = window.findChild<QComboBox *>(QStringLiteral("recoveryLanguage"));
    QVERIFY2(list, "The independent window must expose a real trusted chooser, not an entry-only placeholder");
    QVERIFY(restore); QVERIFY(status); QVERIFY(language);
    QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 10000);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("fixture-a"));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
    QTRY_VERIFY(restore->isEnabled());

    // Inspect a second actual public Helper call: no GUI bool can substitute
    // for independently accepted/non-revoked history or grant rollback.
    QProcess catalog;
    catalog.setProgram(QDir(root).filePath(QString::fromLatin1("bin/GSW Update Fixture Helper.exe")));
    catalog.setArguments({"--list-recovery", "stable"});
#ifdef Q_OS_WIN
    catalog.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *parameters) {
      parameters->flags |= CREATE_NO_WINDOW;
    });
#endif
    catalog.start(); catalog.closeWriteChannel(); QVERIFY(catalog.waitForFinished(15000));
    QCOMPARE(catalog.exitStatus(), QProcess::NormalExit); QCOMPARE(catalog.exitCode(), 0);
    const auto catalogBytes = catalog.readAllStandardOutput();
    QVERIFY(!catalogBytes.contains("://"));
    const auto catalogRecord = QJsonDocument::fromJson(catalogBytes).object();
    QCOMPARE(catalogRecord.value("phase").toString(), QStringLiteral("recovery_candidates"));
    QCOMPARE(catalogRecord.value("code").toString(), QStringLiteral("ACCEPTED_HISTORY"));
    const auto candidates = catalogRecord.value("candidates").toArray();
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.at(0).toObject().value("buildId").toString(), QStringLiteral("fixture-a"));

    const QStringList ids{"zh_CN", "en_US", "ja_JP"};
    const QStringList titles{QString::fromUtf8("版本恢复"), "Version recovery", QString::fromUtf8("バージョンの復元")};
    const QStringList buttons{QString::fromUtf8("恢复选中版本"), "Restore selected version", QString::fromUtf8("選択したバージョンを復元")};
    for (qsizetype i = 0; i < ids.size(); ++i) {
      language->setCurrentIndex(language->findData(ids.at(i)));
      QCOMPARE(gsw::AppLanguage::current(), ids.at(i));
      QCOMPARE(window.windowTitle(), titles.at(i));
      QCOMPARE(restore->text(), buttons.at(i));
      QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("fixture-a"));
      QVERIFY(window.grab().save(QDir(evidence).filePath("recovery-chooser-" + ids.at(i) + ".png")));
    }
    language->setCurrentIndex(language->findData("en_US"));
    bool cancelled = false;
    QTimer cancelTimer; driveConfirmation(cancelTimer, window, QMessageBox::Cancel, cancelled);
    QTest::mouseClick(restore, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(cancelled, 5000);
    QTRY_VERIFY(restore->isEnabled());
    QCOMPARE(hashes(root), allBefore);

    const auto sourcePath = QDir(root).filePath("versions/fixture-a/source/native/tests/NativeUpdateApplicationFixture.cpp");
    const auto originalSource = bytes(sourcePath);
    QVERIFY(!originalSource.isEmpty());
    QVERIFY(writeBytes(sourcePath, originalSource + "\n// owned post-listing tamper probe\n"));
    bool confirmedBad = false;
    QTimer badTimer; driveConfirmation(badTimer, window, QMessageBox::Yes, confirmedBad);
    QTest::mouseClick(restore, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(confirmedBad, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(restore->isEnabled() && status->text().startsWith("Recovery was not completed"), 15000);
    QCOMPARE(bytes(pointerPath), malformed);
    QCOMPARE(hashes(QDir(root).filePath("updates/trust")), trustBefore);
    QCOMPARE(bytes(QDir(root).filePath(".update-trust-anchor")), anchorBefore);
    QVERIFY(writeBytes(sourcePath, originalSource));

    bool confirmedGood = false;
    QTimer goodTimer; driveConfirmation(goodTimer, window, QMessageBox::Yes, confirmedGood);
    QTest::mouseClick(restore, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(confirmedGood, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(activeVersion(root), QStringLiteral("fixture-a"), 15000);
    QTRY_VERIFY(status->text().contains("next launch"));
    const auto current = QJsonDocument::fromJson(bytes(pointerPath)).object();
    const auto exe = QDir(root).filePath(QString::fromLatin1("versions/fixture-a/bin/Gaussian Scene Workbench.exe"));
    QCOMPARE(current.value("executableSha256").toString(), QString::fromLatin1(
        QCryptographicHash::hash(bytes(exe), QCryptographicHash::Sha256).toHex()));
    QCOMPARE(hashes(QDir(root).filePath("updates/trust")), trustBefore);
    QCOMPARE(bytes(QDir(root).filePath(".update-trust-anchor")), anchorBefore);
    QCOMPARE(hashes(QDir(root).filePath("versions")), versionsBefore);
    QCOMPARE(hashes(QDir(root).filePath("bin")), bootstrapBefore);
    QCOMPARE(hashes(QDir(root).filePath("user-data")), dataBefore);
    QVERIFY(!QFile::exists(qEnvironmentVariable("GSW_UPDATE_FIXTURE_MARKER")));
    QJsonObject result{{"schema", "gsw.recovery_chooser_ui_evidence/1"}, {"testOnly", true},
      {"formalGswAcceptance", false}, {"passed", true}, {"actualHelper", true},
      {"onlyAcceptedNonRevokedListed", true}, {"immediateThreeLanguages", true},
      {"postListingTamperRejected", true}, {"explicitRestoredBuild", "fixture-a"},
      {"automaticApplicationLaunch", false}, {"trustAndOtherFilesUnchanged", true}};
    QVERIFY(writeBytes(QDir(evidence).filePath("recovery-chooser-result.json"), QJsonDocument(result).toJson()));
    window.close();
  }
};

int main(int argc, char **argv) {
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QLatin1String("true") ||
      qEnvironmentVariable("GSW_RECOVERY_QT_FIXTURE") != QLatin1String("1") ||
      !QDir::isAbsolutePath(qEnvironmentVariable("GSW_RECOVERY_TEST_INSTALLATION")) ||
      !QDir::isAbsolutePath(qEnvironmentVariable("GSW_RECOVERY_TEST_EVIDENCE"))) return 77;
  QApplication application(argc, argv);
  application.setQuitOnLastWindowClosed(false);
  QCoreApplication::setOrganizationName(QStringLiteral("GSWUpdateFixture"));
  QCoreApplication::setApplicationName(QString::fromLatin1("GSW Recovery Chooser UI Fixture"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
      QDir(qEnvironmentVariable("GSW_RECOVERY_TEST_EVIDENCE")).filePath("owned-preferences"));
  if (!gsw::AppLanguage::initialize(QStringLiteral("zh_CN"))) return 3;
  NativeRecoveryUiTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "NativeRecoveryUiTests.moc"
