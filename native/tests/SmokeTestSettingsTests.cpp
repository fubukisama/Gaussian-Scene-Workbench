#include "SmokeTestSettings.h"
#include "UiSurfaceEvidence.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QtTest>

class SmokeTestSettingsTests : public QObject {
  Q_OBJECT
private slots:
  void evidenceRootsRemainFreshWhenProcessIdsRepeat() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString prefix = QStringLiteral("process-%1")
        .arg(QCoreApplication::applicationPid());
    const QString legacyRoot = QDir(root.path()).filePath(prefix);
    QVERIFY(QDir().mkdir(legacyRoot));
    QStringList retainedRoots;
    for (int invocation = 0; invocation < 8; ++invocation) {
      const QString candidate = gsw::newUiSurfaceEvidenceRoot(root.path());
      QVERIFY(QDir::isAbsolutePath(candidate));
      QVERIFY(QFileInfo(candidate).fileName().startsWith(prefix + QLatin1Char('-')));
      QVERIFY(!QFileInfo::exists(candidate));
      QVERIFY(!retainedRoots.contains(candidate));
      QVERIFY(QDir().mkdir(candidate));
      retainedRoots.append(candidate);
    }
    // Allocation neither removes earlier evidence nor reuses the PID-only root.
    QVERIFY(QFileInfo(legacyRoot).isDir());
    for (const QString &retained : retainedRoots) QVERIFY(QFileInfo(retained).isDir());
  }

  void ignoresStalePidPreferencesAndCleansEachRun() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("SmokeSettingsFixture"));
    QCoreApplication::setApplicationName(QStringLiteral("Fixture"));
    const QString legacyRoot = QDir(root.path()).filePath(QString::number(QCoreApplication::applicationPid()));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, legacyRoot);
    QString legacyFile;
    {
      QSettings stale;
      stale.setValue(QStringLiteral("view/editToolsLocked"), true);
      stale.sync();
      legacyFile = stale.fileName();
    }
    QString previousPath;
    for (int run = 0; run < 2; ++run) {
      auto isolated = gsw::isolateSmokeTestSettings(root.path());
      QVERIFY(isolated);
      const QString path = isolated->path();
      QVERIFY(path != previousPath && path != legacyRoot);
      {
        QSettings current;
        QVERIFY(!current.value(QStringLiteral("view/editToolsLocked"), false).toBool());
        current.setValue(QStringLiteral("view/editToolsLocked"), true);
        current.sync();
      }
      isolated.reset();
      QVERIFY(!QFileInfo::exists(path));
      previousPath = path;
    }
    QSettings legacy(legacyFile, QSettings::IniFormat);
    QVERIFY(legacy.value(QStringLiteral("view/editToolsLocked")).toBool());
  }
};

QTEST_GUILESS_MAIN(SmokeTestSettingsTests)
#include "SmokeTestSettingsTests.moc"
