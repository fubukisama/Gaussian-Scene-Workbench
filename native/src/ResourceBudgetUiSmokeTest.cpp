#include "ResourceBudgetUiSmokeTest.h"

#include "AppLanguage.h"
#include "AppTheme.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>

namespace gsw {
namespace {
void settle() {
  QEventLoop loop;
  QTimer::singleShot(80, &loop, &QEventLoop::quit);
  loop.exec();
}

QDialog *openBudgetDialog(MainWindow &window) {
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!action || !action->isEnabled()) return nullptr;
  action->trigger();
  settle();
  auto *dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
  return dialog && dialog->isVisible() ? dialog : nullptr;
}

bool setManualInputs(QDialog *dialog) {
  auto *mode = dialog ? dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode")) : nullptr;
  auto *ram = dialog ? dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB")) : nullptr;
  auto *gpu = dialog ? dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB")) : nullptr;
  if (!mode || !ram || !gpu || mode->findData(1) < 0) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(0.5);
  gpu->setValue(0.0625);
  settle();
  return mode->currentData().toInt() == 1 && ram->isEnabled() && gpu->isEnabled() &&
         ram->value() == 0.5 && gpu->value() == 0.0625;
}

QString expectedTitle(const QString &locale) {
  if (locale == QStringLiteral("zh_CN")) return QStringLiteral("资源预算");
  if (locale == QStringLiteral("ja_JP")) return QStringLiteral("リソース予算");
  return QStringLiteral("Resource Budget");
}

QString expectedUnknownManualDescription(const QString &locale) {
  if (locale == QStringLiteral("zh_CN"))
    return QStringLiteral("未检测到显存余量。手动上限不是测量值，无法保证保留安全余量。");
  if (locale == QStringLiteral("ja_JP"))
    return QStringLiteral("GPU メモリの余量を検出できません。手動上限は測定値ではなく、安全余量を保証できません。");
  return QStringLiteral("GPU memory headroom could not be detected. The manual limit is not a measurement and cannot guarantee a safety reserve.");
}

QString expectedUnknownManualValue(const QString &locale) {
  if (locale == QStringLiteral("zh_CN")) return QStringLiteral("无法检测（手动上限不是可用量）");
  if (locale == QStringLiteral("ja_JP")) return QStringLiteral("検出不可（手動上限は空き容量ではありません）");
  return QStringLiteral("Unavailable (manual limit is not available memory)");
}

QAction *uiScaleAction(MainWindow &window, const int percentage) {
  auto *display = window.findChild<QMenu *>(QStringLiteral("displaySettingsMenu"));
  if (!display) return nullptr;
  for (auto *action : display->findChildren<QAction *>()) {
    if (action->isCheckable() && action->data().typeId() == QMetaType::Int &&
        action->data().toInt() == percentage)
      return action;
  }
  return nullptr;
}
} // namespace

bool runResourceBudgetUiSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "RESOURCE_BUDGET_UI FAIL:" << message;
      passed = false;
    }
  };
  const QString originalLocale = AppLanguage::current();
  const auto originalThemeMode = AppTheme::currentThemeMode();
  const int originalScale = qApp->property("gswUiScalePercent").toInt();
  const QString shots = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  if (!shots.isEmpty()) check(QDir().mkpath(shots), "screenshot output directory is writable");
  auto *viewport = window.findChild<NativeViewport *>();
  check(viewport != nullptr, "real window contains its public viewport");
  if (!viewport) return false;
  const ResourceBudgetPolicy originalPolicy = viewport->resourceBudgetPolicy();

  // This is a real pre-show application boundary, not a fabricated GPU
  // snapshot: the cold QOpenGLWidget has not created a graphics context.
  {
    MainWindow coldWindow;
    auto *coldViewport = coldWindow.findChild<NativeViewport *>();
    QPointer<QDialog> coldDialog = openBudgetDialog(coldWindow);
    check(coldViewport && !coldViewport->context(), "pre-show window naturally has no graphics context");
    check(coldDialog && !coldDialog->isModal(), "pre-show resource action opens a real modeless dialog");
    if (coldViewport && coldDialog) {
      check(setManualInputs(coldDialog), "unknown-probe manual limits use actual controls");
      check(coldViewport->resourceBudgetStatus().snapshot.gpuSource == GpuProbeSource::Unavailable,
            "unknown GPU measurement is observed at the natural pre-show boundary");
      auto *description = coldDialog->findChild<QLabel *>(QStringLiteral("resourceProbeDescription"));
      auto *available = coldDialog->findChild<QLabel *>(QStringLiteral("resourceGpuAvailable"));
      check(description && available, "real unknown-probe labels are visible through dialog widgets");
      for (const QString &locale : AppLanguage::supported()) {
        AppLanguage::apply(locale, false);
        settle();
        check(description && description->text() == expectedUnknownManualDescription(locale),
              "manual unknown-GPU explanation distinguishes an unmeasured ceiling from a safe fallback");
        check(available && available->text() == expectedUnknownManualValue(locale),
              "manual unknown-GPU value is not presented as measured available memory or a safe fallback");
        if (!shots.isEmpty())
          check(coldDialog->grab().save(QDir(shots).filePath(locale + QStringLiteral("-unknown-manual.png"))),
                "unknown-manual real-dialog screenshot is saved");
      }
      coldDialog->close();
      settle();
    }
  }

  QPointer<QDialog> dialog = openBudgetDialog(window);
  check(dialog && !dialog->isModal(), "real main-window action opens a modeless resource dialog");
  if (dialog) {
    check(setManualInputs(dialog), "real resource dialog uses 512 MiB RAM and 64 MiB GPU inputs");
    const ResourceBudgetPolicy edited = viewport->resourceBudgetPolicy();
    check(edited.mode == ResourceBudgetPolicy::Mode::Manual && edited.ramLimitMiB == 512 && edited.gpuLimitMiB == 64,
          "real controls apply independently specified limits immediately");
    const QSize originalSize = dialog->size();
    for (const QString &locale : AppLanguage::supported()) {
      AppLanguage::apply(locale, false);
      for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        auto *themeAction = window.findChild<QAction *>(theme + QStringLiteral("ThemeAction"));
        check(themeAction != nullptr, "real theme action is available");
        if (themeAction) themeAction->trigger();
        auto *normalScale = uiScaleAction(window, 100);
        check(normalScale != nullptr, "real 100-percent UI action is available");
        if (normalScale) normalScale->trigger();
        settle();
        check(dialog->windowTitle() == expectedTitle(locale), "resource title follows the live language");
        check(viewport->resourceBudgetPolicy() == edited, "language and appearance changes preserve the edited resource policy");
        auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
        auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
        auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
        check(mode && ram && gpu && mode->currentData().toInt() == 1 &&
                  ram->value() == 0.5 && gpu->value() == 0.0625,
              "live language and theme preserve user inputs in the same dialog");
        dialog->resize(originalSize);
        settle();
        if (!shots.isEmpty())
          check(dialog->grab().save(QDir(shots).filePath(locale + QLatin1Char('-') + theme + QStringLiteral("-resource-default.png"))),
                "default resource-dialog screenshot is saved");
        // Existing public scale actions expose their percentages as QAction
        // data. No private MainWindow or production sizing helper is used.
        auto *scale = uiScaleAction(window, 150);
        check(scale != nullptr, "real 150-percent UI action is available");
        if (scale) scale->trigger();
        dialog->resize(360, 300);
        settle();
        auto *scroll = dialog->findChild<QScrollArea *>(QStringLiteral("dialogBodyScroll"));
        check(scroll && scroll->horizontalScrollBar()->maximum() == 0,
              "150-percent narrow resource dialog wraps long captions without horizontal scrolling");
        auto *close = dialog->findChild<QWidget *>(QStringLiteral("resourceBudgetClose"));
        check(close && close->isVisible() && dialog->rect().contains(close->mapTo(dialog, QPoint(0, 0))),
              "Close remains visible outside the scrolling form");
        if (!shots.isEmpty())
          check(dialog->grab().save(QDir(shots).filePath(locale + QLatin1Char('-') + theme + QStringLiteral("-resource-narrow-150.png"))),
                "150-percent narrow resource-dialog screenshot is saved");
      }
    }
    dialog->close();
    settle();
  }
  viewport->setResourceBudgetPolicy(originalPolicy);
  AppLanguage::apply(originalLocale, false);
  AppTheme::applyThemeMode(*qApp, originalThemeMode, false);
  // Restore through the same public UI actions where a manual percentage is
  // present. Smoke-test settings are isolated from the user's profile.
  if (auto *action = uiScaleAction(window, originalScale)) action->trigger();
  settle();
  qInfo() << "RESOURCE_BUDGET_UI" << (passed ? "PASS" : "FAIL")
          << "three languages, two themes, live controls, narrow 150-percent layout, and naturally unknown GPU";
  return passed;
}
} // namespace gsw
