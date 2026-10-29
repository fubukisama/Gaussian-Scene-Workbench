// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-only signed-update integration fixture, never a shipping application.
#include "AppLanguage.h"
#include "NativeInstallation.h"
#include "NativeUpdateUi.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QtTest>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifndef GSW_UPDATE_SIGNED_AUTOMATIC_FIXTURE
#error This source is only the private signed automatic-update fixture.
#endif

namespace {
bool writeMarker(const QString &path, QJsonObject fields) {
  fields.insert("schema", "gsw.signed_automatic_ui_fixture/1");
  fields.insert("testOnly", true);
  fields.insert("formalGswAcceptance", false);
  fields.insert("pid", double(QCoreApplication::applicationPid()));
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  const auto bytes = QJsonDocument(fields).toJson(QJsonDocument::Compact);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QLatin1String("true") || argc != 1) return 64;
  const auto root = gsw::managedInstallationForApplication(app.applicationFilePath());
  if (root.isEmpty()) return 65;
  QDir slot(QFileInfo(app.applicationFilePath()).absolutePath());
  if (!slot.cdUp() || slot.dirName() != QLatin1String("fixture-a")) return 65;
  const auto evidence = QDir(root).filePath("fixture-evidence");
  const auto marker = QDir(evidence).filePath("automatic-A.json");
  const auto release = QDir(evidence).filePath("release-A");
  if (!QDir().mkpath(evidence) || QFileInfo::exists(marker) || QFileInfo::exists(release)) return 65;

  // A new private settings identity, not the user's application registry/settings.
  QCoreApplication::setOrganizationName("GSWSignedAutomaticFixture");
  QCoreApplication::setApplicationName("OwnedAutomaticFlow");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
      QDir(root).filePath("fixture-settings"));
  if (!QSettings().allKeys().isEmpty()) return 65;
  if (!gsw::AppLanguage::initialize("en_US")) return 65;

  QElapsedTimer clock;
  clock.start();
  QMainWindow window;
  window.setAttribute(Qt::WA_ShowWithoutActivating);
  window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
  window.setWindowTitle("Isolated signed automatic-update fixture A");
  window.resize(760, 520);
  auto *help = window.menuBar()->addMenu("Fixture Help");
  // This is the unmodified production UI library: no root/helper/feed locator.
  gsw::installNativeUpdateUi(help, &window);
  window.show();

  QPointer<QDialog> dialog;
  QPointer<QLabel> status;
  bool armed = false, activated = false, foregroundUnchanged = true;
  QString failure, lastText;
  quintptr foreground = 0;
  qint64 armedAt = -1, activatedAt = -1;
  auto state = [&](const char *phase) {
    return QJsonObject{{"phase", QLatin1String(phase)}, {"label", "A"},
      {"buildId", "fixture-a"}, {"executable", app.applicationFilePath()},
      {"installationRoot", root}, {"elapsedMs", double(clock.elapsed())},
      {"armedAtMs", double(armedAt)}, {"activatedAtMs", double(activatedAt)},
      {"statusText", lastText}, {"foregroundUnchanged", foregroundUnchanged},
      {"foregroundWindow", QString::number(quint64(foreground), 16)},
      {"automaticCheck", QSettings().value("updates/automaticCheck", true).toBool()},
      {"prepareForNextLaunch", QSettings().value("updates/prepareForNextLaunch", false).toBool()},
      {"failure", failure}};
  };
  auto fail = [&](const char *code) {
    if (!failure.isEmpty()) return;
    failure = QLatin1String(code);
    writeMarker(marker, state("failed"));
    app.exit(2);
  };

  QTimer::singleShot(200, &app, [&] {
    auto *action = window.findChild<QAction *>("nativeUpdateAction");
    if (!action) { fail("update_help_action_missing"); return; }
    action->trigger();
    dialog = window.findChild<QDialog *>("nativeUpdateDialog");
    if (!dialog || !dialog->isVisible()) { fail("update_dialog_missing"); return; }
    QCheckBox *automatic = nullptr, *prepare = nullptr;
    const auto automaticText = QCoreApplication::translate("Workbench", "后台自动检查（不弹出窗口）");
    const auto prepareText = QCoreApplication::translate("Workbench", "自动下载并在下次启动时使用，不中断当前任务");
    for (auto *checkbox : dialog->findChildren<QCheckBox *>()) {
      if (checkbox->text() == automaticText) automatic = checkbox;
      if (checkbox->text() == prepareText) prepare = checkbox;
    }
    status = dialog->findChild<QLabel *>("nativeUpdateStatus");
    if (!automatic || !prepare || !status || !automatic->isChecked() || prepare->isChecked()) {
      fail("fresh_preferences_incorrect"); return;
    }
    // A real widget click opts in; no private Controller call or fake event.
    QTest::mouseClick(prepare, Qt::LeftButton, Qt::NoModifier,
        QPoint(10, prepare->height() / 2));
    if (!prepare->isChecked() || !QSettings().value("updates/prepareForNextLaunch").toBool()) {
      fail("automatic_opt_in_not_persisted"); return;
    }
    dialog->close();
    if (dialog->isVisible()) { fail("configuration_dialog_did_not_close"); return; }
    QTimer::singleShot(100, &app, [&] {
#ifdef Q_OS_WIN
      foreground = quintptr(GetForegroundWindow());
#endif
      armed = true;
      armedAt = clock.elapsed();
      lastText = status->text();
      if (!writeMarker(marker, state("armed"))) app.exit(3);
    });
  });

  QTimer poll;
  poll.setInterval(50);
  QObject::connect(&poll, &QTimer::timeout, &app, [&] {
    if (!armed || !failure.isEmpty()) return;
#ifdef Q_OS_WIN
    foregroundUnchanged = foregroundUnchanged && quintptr(GetForegroundWindow()) == foreground;
#endif
    if (!foregroundUnchanged) { fail("background_flow_changed_foreground"); return; }
    if (!dialog || dialog->isVisible() || !status) { fail("background_flow_opened_dialog"); return; }
    lastText = status->text();
    const auto completed = QCoreApplication::translate("Workbench",
        "下次启动时生效；当前窗口和任务将继续运行。");
    if (!activated && lastText == completed) {
      // Only an observation marker. The Python driver must independently
      // launch signed B via the unchanged stable launcher before accepting.
      activated = true;
      activatedAt = clock.elapsed();
      if (!QSettings().value("updates/lastAttempt").toDateTime().isValid() ||
          !writeMarker(marker, state("activated"))) { fail("automatic_result_record_failed"); return; }
    }
    if (QFileInfo::exists(release)) {
      if (!activated) { fail("parent_released_before_activation"); return; }
      window.close();
      app.quit();
    }
  });
  poll.start();
  // A dedicated integration fixture lifetime; product 30 s/24 h scheduling
  // and the existing 100/150 ms A/B lifecycle fixtures are not changed.
  QTimer::singleShot(90000, &app, [&] { fail("automatic_flow_fixture_deadline"); });
  const int result = app.exec();
  if (result == 0 && activated && failure.isEmpty()) {
    if (!writeMarker(marker, state("normal_exit"))) return 3;
  }
  return result;
}
