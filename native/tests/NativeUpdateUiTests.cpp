// SPDX-License-Identifier: GPL-3.0-or-later
// Remote real-window UI adapter acceptance. Helper bytes/progress are a
// protocol fixture, not signed-update or formal GSW startup evidence.
#include "AppLanguage.h"
#include "NativeUpdateUi.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using gsw::AppLanguage;
class NativeUpdateUiTests final : public QObject {
  Q_OBJECT
  QTemporaryDir temporary{QDir(qEnvironmentVariable("GSW_UPDATE_TEST_ROOT",
      QCoreApplication::applicationDirPath())).filePath("update-ui-tests-XXXXXX")};
  std::unique_ptr<QMainWindow> window;
  QString root, evidence;
  QByteArray originalPointer;

  static QByteArray read(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  }
  static bool write(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
  }
  QString path(const char *relative) const { return QDir(root).filePath(QString::fromLatin1(relative)); }
  static bool until(const std::function<bool()> &predicate, int timeout = 5000) {
    QElapsedTimer clock; clock.start();
    while (!predicate()) {
      if (clock.elapsed() >= timeout) return false;
      QTest::qWait(20);
    }
    return true;
  }
  QList<QJsonObject> journal(const QString &phase = {}) const {
    QList<QJsonObject> result;
    for (const auto &line : read(path("ui-fixture-events.jsonl")).split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (!row.isEmpty() && (phase.isEmpty() || row.value("phase").toString() == phase)) result.append(row);
    }
    return result;
  }
  qint64 latestPid() const {
    const auto started = journal(QString::fromLatin1("started"));
    return started.isEmpty() ? 0 : started.last().value("pid").toInteger();
  }
  void makeWindow() {
    window = std::make_unique<QMainWindow>();
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->setWindowTitle(QString::fromLatin1("Isolated native update UI acceptance"));
    window->resize(900, 680);
    auto *help = window->menuBar()->addMenu(QString::fromLatin1("Help"));
    gsw::installNativeUpdateUi(help, window.get());
    window->show();
  }
  QDialog *dialog() const { return window->findChild<QDialog *>(QString::fromLatin1("nativeUpdateDialog")); }
  QPushButton *button(const char *name) const { return dialog()->findChild<QPushButton *>(QString::fromLatin1(name)); }
  QLabel *status() const { return dialog()->findChild<QLabel *>(QString::fromLatin1("nativeUpdateStatus")); }
  QComboBox *channel() const { return dialog()->findChild<QComboBox *>(QString::fromLatin1("nativeUpdateChannel")); }
  QCheckBox *checkBox(const char *name) const { return dialog()->findChild<QCheckBox *>(QString::fromLatin1(name)); }
  void open() {
    auto *action = window->findChild<QAction *>(QString::fromLatin1("nativeUpdateAction"));
    QVERIFY(action);
    action->trigger();
    QVERIFY(dialog());
    QVERIFY(until([&] { return dialog()->isVisible(); }));
  }
  void check() {
    QTest::mouseClick(button("nativeUpdateCheck"), Qt::LeftButton);
    QVERIFY(until([&] { return button("nativeUpdateDownload")->isEnabled(); }));
    QVERIFY(!journal(QString::fromLatin1("started")).isEmpty());
  }
private slots:
  void initTestCase() {
    QVERIFY(temporary.isValid());
    QVERIFY(QFileInfo::exists(qEnvironmentVariable("GSW_UPDATE_UI_HELPER_FIXTURE")));
    QCoreApplication::setOrganizationName(QString::fromLatin1("GSWUpdateUiFixture"));
    QCoreApplication::setApplicationName(QString::fromLatin1("OwnedAcceptance"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.filePath(QString::fromLatin1("settings")));
    evidence = qEnvironmentVariable("GSW_UPDATE_UI_EVIDENCE",
        QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1("ui-evidence")));
    QVERIFY(QDir().mkpath(evidence));
    QVERIFY(AppLanguage::initialize(QString::fromLatin1("zh_CN")));
  }
  void init() {
    QSettings().clear();
    QVERIFY(AppLanguage::apply(QString::fromLatin1("zh_CN"), false));
    QString caseName = QString::fromLatin1(QTest::currentTestFunction());
    if (const auto *tag = QTest::currentDataTag(); tag && *tag)
      caseName += QLatin1Char('-') + QString::fromLatin1(tag);
    root = temporary.filePath(caseName);
    const auto application = path("versions/ui-fixture-a/bin/Gaussian Scene Workbench.exe");
    const QByteArray fake("owned canonical application identity; never executed");
    QVERIFY(write(application, fake));
    QVERIFY(write(path("bin/Gaussian Scene Workbench Launcher.exe"), "owned launcher identity; never executed"));
    const QJsonObject pointer{{"schema", "gsw.native_active_version/1"}, {"version", "ui-fixture-a"},
        {"executableSha256", QString::fromLatin1(QCryptographicHash::hash(fake, QCryptographicHash::Sha256).toHex())}};
    originalPointer = QJsonDocument(pointer).toJson(QJsonDocument::Compact);
    QVERIFY(write(path("current.json"), originalPointer));
    QVERIFY(write(path("user-project.gsw"), "unchanged project sentinel"));
    QVERIFY(QFile::copy(qEnvironmentVariable("GSW_UPDATE_UI_HELPER_FIXTURE"),
        path("bin/Gaussian Scene Workbench Update Helper.exe")));
    qputenv("GSW_UPDATE_UI_FIXTURE_APPLICATION", application.toUtf8());
    makeWindow();
    QTest::qWait(100); // Settle native Show events before foreground assertions.
    QVERIFY(!dialog()); // Installation never creates an unsolicited dialog.
  }
  void cleanup() {
    if (root.isEmpty()) return;
    write(path("ui-fixture-allow-complete"), "owned completion gate");
    window.reset();
    QVERIFY(until([&] { return journal(QString::fromLatin1("started")).size() ==
        journal(QString::fromLatin1("exited")).size(); }, 4000));
    QCOMPARE(read(path("current.json")), originalPointer);
    QCOMPARE(read(path("user-project.gsw")), QByteArray("unchanged project sentinel"));
    qunsetenv("GSW_UPDATE_UI_FIXTURE_APPLICATION");
  }
  void helperVersionAndBuildLabelsAlwaysUsePlainText() {
    open(); check();
    QCOMPARE(status()->textFormat(), Qt::PlainText);
    QVERIFY(status()->text().contains(QString::fromLatin1("UI fixture <b>2 & preview</b>")));
    auto *running = dialog()->findChild<QLabel *>(QString::fromLatin1("nativeUpdateRunningBuild"));
    QVERIFY(running);
    QCOMPARE(running->textFormat(), Qt::PlainText);
  }
  void actualHelperProgressRetranslatesWithoutRestartAndSupportsCancelRetryAndApply() {
    open(); check();
    QTest::mouseClick(button("nativeUpdateDownload"), Qt::LeftButton);
    auto *progress = dialog()->findChild<QProgressBar *>();
    QVERIFY(progress);
    QVERIFY(until([&] { return progress->maximum() == 1000 && progress->value() == 250; }));
    const auto pid = latestPid(); QVERIFY(pid > 0);
    const auto calls = journal(QString::fromLatin1("started")).size();
    const QMap<QString, QString> notes{{QString::fromLatin1("zh_CN"), QString::fromUtf8("中文 <b>原样</b> & <script>fixture</script>")},
        {QString::fromLatin1("en_US"), QString::fromLatin1("English <b>literal</b> & <script>fixture</script>")},
        {QString::fromLatin1("ja_JP"), QString::fromUtf8("日本語 <b>そのまま</b> & <script>fixture</script>")}};
    auto *releaseNotes = dialog()->findChild<QPlainTextEdit *>(QString::fromLatin1("nativeUpdateNotes"));
    QVERIFY(releaseNotes);
    for (const auto &language : AppLanguage::supported()) {
      QVERIFY(AppLanguage::apply(language, false));
      QCOMPARE(dialog()->windowTitle(), QCoreApplication::translate("Workbench", "软件更新"));
      QCOMPARE(button("nativeUpdateCancel")->text(), QCoreApplication::translate("Workbench", "取消下载"));
      QCOMPARE(releaseNotes->toPlainText(), notes.value(language));
      QVERIFY(status()->text().contains(QString::fromLatin1("4.0")));
      QCOMPARE(latestPid(), pid);
      QCOMPARE(journal(QString::fromLatin1("started")).size(), calls);
      QCOMPARE(progress->value(), 250);
      QTest::qWait(50);
      QVERIFY(dialog()->grab().save(QDir(evidence).filePath(QString::fromLatin1("update-ui-") + language + QString::fromLatin1(".png"))));
    }
    QTest::mouseClick(button("nativeUpdateCancel"), Qt::LeftButton);
    QVERIFY(until([&] { return button("nativeUpdateRetry")->isEnabled(); }));
    QVERIFY(status()->text().contains(QString::fromLatin1("download_cancelled")));
    QCOMPARE(read(path("current.json")), originalPointer);
    QTest::mouseClick(button("nativeUpdateRetry"), Qt::LeftButton);
    QVERIFY(until([&] { return latestPid() > 0 && latestPid() != pid && progress->value() == 250; }));
    QVERIFY(write(path("ui-fixture-allow-complete"), "complete this owned helper request"));
    QVERIFY(until([&] { return button("nativeUpdateApply")->isEnabled(); }));
    QVERIFY(QFileInfo::exists(path("updates/prepared.json")));
    QTest::mouseClick(button("nativeUpdateApply"), Qt::LeftButton);
    QVERIFY(until([&] { return status()->text() == QCoreApplication::translate("Workbench", "下次启动时生效；当前窗口和任务将继续运行。"); }));
    QVERIFY(!button("nativeUpdateApply")->isEnabled());
    QCOMPARE(read(path("current.json")), originalPointer); // UI intent does not authorize installation.
  }
  void persistsChannelSkipDeferAndPreferencesWhenTheControllerIsReopened() {
    open(); check();
    QTest::mouseClick(button("nativeUpdateSkip"), Qt::LeftButton);
    QVERIFY(QSettings().value(QString::fromLatin1("updates/skipped/stable")).toStringList().contains(QString::fromLatin1("ui-fixture-b")));
    auto *automatic = checkBox("nativeUpdateAutomaticCheck");
    auto *prepare = checkBox("nativeUpdateAutomaticPrepare");
    QVERIFY(automatic); QVERIFY(prepare);
    QVERIFY(automatic->isChecked()); QVERIFY(!prepare->isChecked());
    QTest::mouseClick(automatic, Qt::LeftButton, Qt::NoModifier, QPoint(10, automatic->height() / 2));
    QTest::mouseClick(prepare, Qt::LeftButton, Qt::NoModifier, QPoint(10, prepare->height() / 2));
    channel()->setFocus(); QTest::keyClick(channel(), Qt::Key_End);
    QCOMPARE(channel()->currentData().toString(), QString::fromLatin1("preview"));
    check();
    QTest::mouseClick(button("nativeUpdateSkip"), Qt::LeftButton);
    QVERIFY(QSettings().value(QString::fromLatin1("updates/skipped/preview")).toStringList().contains(QString::fromLatin1("ui-fixture-preview-b")));
    QTest::mouseClick(button("nativeUpdateLater"), Qt::LeftButton);
    QVERIFY(!dialog()->isVisible());
    QVERIFY(QSettings().value(QString::fromLatin1("updates/deferredUntil")).toDateTime() > QDateTime::currentDateTimeUtc());
    window.reset();
    makeWindow(); open();
    QCOMPARE(channel()->currentData().toString(), QString::fromLatin1("preview"));
    QVERIFY(!checkBox("nativeUpdateAutomaticCheck")->isChecked());
    QVERIFY(checkBox("nativeUpdateAutomaticPrepare")->isChecked());
    QVERIFY(QSettings().value(QString::fromLatin1("updates/skipped/stable")).toStringList().contains(QString::fromLatin1("ui-fixture-b")));
  }
  void retriesAnActualExternalHelperFailureWithoutTouchingCurrentOrProject() {
    open();
    QVERIFY(write(path("ui-fixture-fail-next"), "fail one owned request"));
    QTest::mouseClick(button("nativeUpdateCheck"), Qt::LeftButton);
    QVERIFY(until([&] { return button("nativeUpdateRetry")->isEnabled(); }));
    QVERIFY(status()->text().contains(QString::fromLatin1("FIXTURE_TRANSIENT")));
    const auto first = latestPid();
    QTest::mouseClick(button("nativeUpdateRetry"), Qt::LeftButton);
    QVERIFY(until([&] { return button("nativeUpdateDownload")->isEnabled(); }));
    QVERIFY(latestPid() != first);
    QCOMPARE(read(path("current.json")), originalPointer);
  }
  void insufficientDiskSpaceIsActionableInEachLanguageWithoutRestart_data() {
    QTest::addColumn<QByteArray>("code");
    QTest::newRow("download-preflight") << QByteArray("download_insufficient_space");
    QTest::newRow("activation-preflight") << QByteArray("activation_insufficient_space");
  }
  void insufficientDiskSpaceIsActionableInEachLanguageWithoutRestart() {
    QFETCH(QByteArray, code);
    open();
    QVERIFY(write(path("ui-fixture-space-next"), code));
    QTest::mouseClick(button("nativeUpdateCheck"), Qt::LeftButton);
    QVERIFY(until([&] { return button("nativeUpdateRetry")->isEnabled(); }));
    const auto process = latestPid();
    const auto started = journal(QString::fromLatin1("started")).size();
    const QMap<QString, QString> expected{
      {QStringLiteral("zh_CN"), QStringLiteral("磁盘可用空间不足，当前版本未更改。请释放磁盘空间后重试。")},
      {QStringLiteral("en_US"), QStringLiteral("Not enough free disk space. The current version is unchanged. Free disk space and try again.")},
      {QStringLiteral("ja_JP"), QStringLiteral("ディスクの空き容量が不足しています。現在のバージョンは変更していません。空き容量を確保して再試行してください。")}};
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
      QVERIFY(AppLanguage::apply(it.key(), false));
      QCOMPARE(status()->text(), it.value());
      QCOMPARE(status()->textFormat(), Qt::PlainText);
      QCOMPARE(latestPid(), process);
      QCOMPARE(journal(QString::fromLatin1("started")).size(), started);
    }
    QCOMPARE(read(path("current.json")), originalPointer);
  }
  void thirtySecondBackgroundCheckNeverCreatesADialogOrTakesTheForeground() {
    QVERIFY(!dialog());
    auto *active = QApplication::activeWindow();
#ifdef Q_OS_WIN
    const auto foreground = GetForegroundWindow();
#endif
    QVERIFY(until([&] { return !journal(QString::fromLatin1("exited")).isEmpty(); }, 35000));
    QCOMPARE(journal(QString::fromLatin1("started")).size(), qsizetype(1));
    QCOMPARE(journal(QString::fromLatin1("started")).first().value("operation").toString(), QString::fromLatin1("--check"));
    QVERIFY(!dialog());
    QCOMPARE(QApplication::activeWindow(), active);
#ifdef Q_OS_WIN
    QCOMPARE(GetForegroundWindow(), foreground);
#endif
    QVERIFY(QSettings().value(QString::fromLatin1("updates/lastAttempt")).toDateTime().isValid());
    QVERIFY(!QFileInfo::exists(path("updates/prepared.json")));
    QCOMPARE(read(path("current.json")), originalPointer);
    const QJsonObject record{{"schema", "gsw.update_ui_acceptance/1"}, {"testOnly", true},
        {"realExternalHelper", true}, {"realWindowsWidgets", true}, {"backgroundDialogCreated", false},
        {"formalGswAcceptance", false}, {"installationMutation", false}};
    QVERIFY(write(QDir(evidence).filePath(QString::fromLatin1("scope.json")), QJsonDocument(record).toJson()));
  }
};
QTEST_MAIN(NativeUpdateUiTests)
#include "NativeUpdateUiTests.moc"
