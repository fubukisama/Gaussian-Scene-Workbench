#include "LanguageSmokeTest.h"
#include "AppLanguage.h"
#include "MainWindow.h"
#include "TrainingDialog.h"
#include "DatasetImportDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>

namespace gsw {
bool runLanguageSmokeTest(MainWindow &window) {
  const QString locale = AppLanguage::current();
  const int index = AppLanguage::supported().indexOf(locale);
  bool passed = index >= 0 && window.isVisible();
  auto check = [&passed](bool condition, const char *description) {
    if (!condition) { qCritical() << "Language smoke:" << description; passed = false; }
  };
  if (!passed) return false;
  const QStringList saveTexts = {QStringLiteral("保存工程"), QStringLiteral("Save Project"), QStringLiteral("プロジェクトを保存")};
  const QStringList pointTexts = {QStringLiteral("点云"), QStringLiteral("Point Cloud"), QStringLiteral("点群")};
  const auto *saveAction = window.findChild<QAction *>(QStringLiteral("saveProjectAction"));
  check(saveAction && saveAction->text() == saveTexts[index], "save action translation");
  check(QCoreApplication::translate("Workbench", "点云") == pointTexts[index], "point cloud terminology");
  const QStringList untitled = {QStringLiteral("未命名工程"), QStringLiteral("Untitled Project"), QStringLiteral("無題のプロジェクト")};
  const auto *nameLabel = window.findChild<QLabel *>(QStringLiteral("projectNameValue"));
  check(nameLabel && nameLabel->text() == untitled[index], "dynamic project label");
  auto *languageMenu = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
  check(languageMenu && languageMenu->actions().size() == 3, "language menu");
  if (!languageMenu) return false;
  for (int i = 0; i < 3; ++i) {
    check(languageMenu->actions()[i]->data().toString() == AppLanguage::supported()[i], "stable locale identifiers");
    check(languageMenu->actions()[i]->text() == AppLanguage::displayName(AppLanguage::supported()[i]), "autonyms");
  }
  const QString name = QStringLiteral("Test_工程_日本語");
  TrainingDialog training(QCoreApplication::applicationDirPath(), name,
                          QCoreApplication::applicationDirPath(), true, true, &window);
  const auto configuration = training.configuration();
  check(configuration.backend == QStringLiteral("3dgs"), "training backend must not be translated");
  check(configuration.quality == QStringLiteral("quick"), "preset identifier must not be translated");
  DatasetImportDialog import({}, name, {}, QCoreApplication::applicationDirPath(), true, &window);
  check(import.request().sceneName == name, "user scene name must remain unchanged");
  // This source lives in the catalog under the original English diagnostic.
  const QString error = QCoreApplication::translate("Workbench", "Unable to open PLY file %1: %2")
                            .arg(name, QStringLiteral("test"));
  check(error.contains(name) && !error.contains(QStringLiteral("%1")), "error placeholders");
  QMessageBox box(QMessageBox::Question, QStringLiteral("test"), QStringLiteral("test"),
                 QMessageBox::Save | QMessageBox::Cancel, &window);
  const QString cancel = box.button(QMessageBox::Cancel)->text().remove(QLatin1Char('&'));
  check(!cancel.isEmpty() && (index == 1 || cancel != QStringLiteral("Cancel")), "Qt standard buttons");
  check(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs), "file dialog locale isolation");
  const QString other = AppLanguage::supported()[(index + 1) % 3];
  bool noticeSeen = false;
  QTimer::singleShot(0, &window, [&noticeSeen, &check]() {
    auto *notice = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    noticeSeen = notice != nullptr;
    if (notice) {
      check(!notice->text().isEmpty(), "restart notice");
      notice->accept();
    }
  });
  languageMenu->actions()[(index + 1) % 3]->trigger();
  check(noticeSeen && AppLanguage::saved() == other, "menu persists next-launch language");
  check(AppLanguage::current() == locale && saveAction->text() == saveTexts[index], "language change must not interrupt the current UI");
  check(!AppLanguage::save(QStringLiteral("invalid")) && AppLanguage::saved() == other, "invalid locale rejected");
  check(AppLanguage::save(locale), "restore isolated test preference");
  const QString screenshotDirectory = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  if (!screenshotDirectory.isEmpty()) {
    QDir().mkpath(screenshotDirectory);
    check(window.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral(".png"))), "UI screenshot");
  }
  qInfo().noquote() << "Language smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
