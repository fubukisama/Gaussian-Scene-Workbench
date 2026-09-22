#include "SmokeTestSettings.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QtTest>

class SmokeTestSettingsTests : public QObject {
  Q_OBJECT
private slots:
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
