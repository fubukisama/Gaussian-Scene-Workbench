#include "TrainingPerformanceSmokeTest.h"
#include "AppLanguage.h"
#include "TrainingDialog.h"
#include "TrainingMonitorWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QProgressBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>

#include <array>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {

constexpr double Mebibyte = 1048576.0;

bool waitFor(const std::function<bool()> &condition) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < 5000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return condition();
}

QJsonObject wallObservation(const QString &backend) {
  return {{"version", 1}, {"backend", backend}, {"iteration", 20},
      {"wall_step_ms", 200.0},
      {"renderer_engine", backend == QStringLiteral("3dgs") ? "fastergs" : "stock"},
      {"loss_engine", "fused_l1_ssim"},
      {"stages_ms", QJsonObject{{"setup", 1.0}, {"transfer", 15.0},
          {"render", 90.0}, {"loss", 10.0}, {"backward", 40.0},
          {"densification", 5.0}, {"optimizer", 15.0}, {"preview", 20.0},
          {"checkpoint", 2.0}, {"report", 2.0}}},
      {"gpu_stages_ms", QJsonObject{{"render", 80.0}, {"loss", 9.0}, {"backward", 35.0}}},
      {"torch_allocated_bytes", 4.0 * Mebibyte},
      {"torch_reserved_bytes", 8.0 * Mebibyte},
      {"torch_peak_allocated_bytes", 6.0 * Mebibyte},
      {"device_free_bytes", 10240.0 * Mebibyte},
      {"device_total_bytes", 12288.0 * Mebibyte}};
}

QJsonObject growthObservation(const QString &backend, const int iteration,
                             const bool admitted) {
  return {{"version", 1}, {"backend", backend}, {"iteration", iteration},
      {"state", admitted ? "admitted" : "deferred"},
      {"reason", admitted ? "admitted" : "vram_pressure"},
      {"allowed", admitted}, {"observation_valid", true},
      {"candidate_counts_known", true}, {"point_count", 10},
      {"transient_point_count", 18}, {"final_point_count", 15},
      {"parameter_bytes", 2360}, {"optimizer_state_bytes", 4720},
      {"additional_peak_bytes", 32.0 * Mebibyte},
      {"free_bytes", (admitted ? 4096.0 : 1024.0) * Mebibyte},
      {"total_bytes", 12288.0 * Mebibyte}, {"spare_bytes", 1229.0 * Mebibyte},
      {"available_bytes", (admitted ? 2867.0 : 0.0) * Mebibyte},
      {"cache_credit_bytes", 0}, {"clone_count", 2}, {"split_count", 3}};
}

QString fullStepToken(const QString &language) {
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("全步：");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("ステップ全体：");
  return QString::fromUtf8("Full step:");
}

QString qualityWarningToken(const QString &language) {
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("持续受限可能减少细节");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("制約が続くと細部が減る可能性");
  return QString::fromUtf8("Persistent pressure may reduce detail");
}

QString cudaToken(const QString &language) {
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("CUDA 阶段（非整步）");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("CUDA 区間（ステップ全体ではありません）");
  return QString::fromUtf8("CUDA stages (not a full step)");
}

QString resumeBlockedToken(const QString &language) {
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("检查点使用的训练渲染后端不可用或已改变");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("チェックポイントで使用した学習描画バックエンドが利用できないか、変更されています");
  return QString::fromUtf8("The checkpoint's training renderer is unavailable or has changed");
}

QString independentQualityToken(const QString &language) {
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("未提供独立验证结果");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("独立した検証結果は未提供");
  return QString::fromUtf8("No independent validation results");
}

class LanguageRestore final {
public:
  LanguageRestore() : mLanguage(AppLanguage::current()), mLocale(QLocale()) {}
  ~LanguageRestore() {
    AppLanguage::apply(mLanguage, false);
    QLocale::setDefault(mLocale);
  }
private:
  QString mLanguage;
  QLocale mLocale;
};

} // namespace

bool runTrainingPerformanceSmokeTest() {
  const LanguageRestore restore;
  bool passed = true;
  QString caseName;
  const auto check = [&](const bool condition, const char *message) {
    std::fprintf(stderr, "Training performance [%s] %s: %s\n",
        caseName.toUtf8().constData(), condition ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    if (!condition) passed = false;
    return condition;
  };

  QMainWindow ownedWindow;
  ownedWindow.resize(900, 650);
  auto *monitor = new TrainingMonitorWidget(&ownedWindow);
  ownedWindow.setCentralWidget(monitor);
  ownedWindow.show(); // No raise(), activation request or real worker.
  caseName = QStringLiteral("setup");
  if (!check(waitFor([&] { return ownedWindow.windowHandle() &&
      ownedWindow.windowHandle()->isExposed() && monitor->isVisible(); }),
      "the actual test-owned monitor window is exposed")) return false;

  auto *profile = monitor->findChild<QLabel *>(QStringLiteral("trainingPerformanceDetails"));
  auto *qualityEvidence = monitor->findChild<QLabel *>(QStringLiteral("trainingQualityEvidence"));
  auto *warning = monitor->findChild<QLabel *>(QStringLiteral("trainingGrowthWarning"));
  auto *speed = monitor->findChild<QLabel *>(QStringLiteral("trainingMetricValue4"));
  auto *remaining = monitor->findChild<QLabel *>(QStringLiteral("trainingMetricValue6"));
  auto *title = monitor->findChild<QLabel *>(QStringLiteral("trainingTaskTitle"));
  auto *progress = monitor->findChild<QProgressBar *>();
  if (!check(profile && qualityEvidence && warning && speed && remaining && title && progress,
      "public monitor labels and the seven-metric interface are present")) return false;
  check(profile->wordWrap() && warning->wordWrap() &&
      profile->textFormat() == Qt::PlainText && warning->textFormat() == Qt::PlainText,
      "performance and quality warnings use wrapping literal text");

  {
    caseName = QStringLiteral("public-training-configuration");
    QTemporaryDir fixture;
    if (!check(fixture.isValid(), "a disposable training-dialog location is available")) return false;
    TrainingDialog dialog(fixture.path(), QStringLiteral("configuration-fixture"),
                          fixture.path(), true, true, &ownedWindow);
    auto *optimization = dialog.findChild<QCheckBox *>(QStringLiteral("trainingPerformanceOptimization"));
    auto *prefetch = dialog.findChild<QCheckBox *>(QStringLiteral("trainingCameraPrefetch"));
    auto *capacity = dialog.findChild<QCheckBox *>(QStringLiteral("trainingCapacityStorage"));
    auto *auxiliary = dialog.findChild<QCheckBox *>(QStringLiteral("trainingSurfelAuxiliary"));
    auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo"));
    auto *preset = dialog.findChild<QComboBox *>(QStringLiteral("trainingQualityCombo"));
    auto *iterations = dialog.findChild<QSpinBox *>(QStringLiteral("trainingIterationsSpinBox"));
    auto *resolution = dialog.findChild<QComboBox *>(QStringLiteral("trainingResolutionCombo"));
    if (!check(optimization && prefetch && capacity && auxiliary && backend && preset && iterations && resolution,
        "three independent performance controls are present on the public training form")) return false;
    const TrainingConfiguration defaults = dialog.configuration();
    check(defaults.cameraPrefetch && defaults.capacityStorage && defaults.surfelAuxiliary &&
        prefetch->isEnabled() && capacity->isEnabled() && !auxiliary->isEnabled(),
        "new controls default on, with only the surfel option disabled for 3DGS");
    optimization->setChecked(false);
    check(!prefetch->isEnabled() && !capacity->isEnabled() && !auxiliary->isEnabled() &&
        prefetch->isChecked() && capacity->isChecked() && auxiliary->isChecked(),
        "disabling overall optimization disables options without erasing their values");
    optimization->setChecked(true);
    prefetch->setChecked(false);
    capacity->setChecked(true);
    backend->setCurrentIndex(backend->findData(QStringLiteral("2dgs")));
    auxiliary->setChecked(false);
    check(auxiliary->isEnabled(), "the surfel option is editable for an optimized 2DGS job");
    const std::array<int, 5> presetIterations{10000, 30000, 30000, 30000, 30000};
    const std::array<int, 5> presetResolutions{2, 8, 4, 2, 1};
    if (!check(preset->count() == int(presetIterations.size()),
        "the existing five quality presets remain available")) return false;
    for (int index = 0; index < preset->count(); ++index) {
      preset->setCurrentIndex(index);
      const TrainingConfiguration config = dialog.configuration();
      check(config.iterations == presetIterations[index] && config.resolution == presetResolutions[index] &&
          !config.cameraPrefetch && config.capacityStorage && !config.surfelAuxiliary,
          "existing preset iteration/resolution rules do not reset independent performance choices");
    }
    iterations->setValue(17000);
    resolution->setCurrentIndex(resolution->findData(4));
    const QString chosenPreset = dialog.configuration().quality;
    for (const QString &language : AppLanguage::supported()) {
      AppLanguage::apply(language, false);
      const TrainingConfiguration config = dialog.configuration();
      check(config.iterations == 17000 && config.resolution == 4 && config.quality == chosenPreset &&
          config.backend == QStringLiteral("2dgs") && !config.cameraPrefetch &&
          config.capacityStorage && !config.surfelAuxiliary,
          "live language switching preserves edited options, backend and preset");
      check(!prefetch->text().isEmpty() && !capacity->text().isEmpty() && !auxiliary->text().isEmpty() &&
          !prefetch->toolTip().isEmpty() && !capacity->toolTip().isEmpty() && !auxiliary->toolTip().isEmpty(),
          "all independent controls expose localized descriptions");
    }
    backend->setCurrentIndex(backend->findData(QStringLiteral("3dgs")));
    check(!auxiliary->isEnabled() && !dialog.configuration().surfelAuxiliary,
        "returning to 3DGS disables but retains the user's surfel choice");
  }

  for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    for (const QString &language : AppLanguage::supported()) {
      caseName = backend + QStringLiteral("/") + language;
      check(AppLanguage::apply(language, false), "apply the requested language without persisting it");
      monitor->beginTraining(QStringLiteral("performance-fixture"), backend, 100);
      check(qualityEvidence->isVisible() && qualityEvidence->wordWrap() &&
          qualityEvidence->textFormat() == Qt::PlainText &&
          qualityEvidence->text().contains(independentQualityToken(language)),
          "training begins with explicit unavailable independent-quality evidence");
      WorkerStatus legacy;
      legacy.state = QStringLiteral("running");
      legacy.stage = QStringLiteral("train");
      legacy.iteration = 20;
      legacy.totalIterations = 100;
      legacy.loss = .1;
      legacy.iterationMilliseconds = 40.0;
      legacy.elapsedSeconds = 4.0;
      monitor->updateStatus(legacy);
      check(speed->text() == QStringLiteral("-") && remaining->text() == QStringLiteral("-"),
          "a legacy 40ms CUDA event never fabricates full-step speed or ETA");
      check(profile->isVisible() && profile->text().contains(cudaToken(language)) &&
          profile->text().contains(QStringLiteral("40.0 ms")),
          "legacy CUDA time is visible and explicitly labelled as not a full step");
      check(warning->isHidden(), "starting a new controlled task clears a prior resource warning");

      WorkerStatus wall = legacy;
      wall.trainingPerformance = wallObservation(backend);
      wall.trainingGrowth = growthObservation(backend, 20, false);
      monitor->updateStatus(wall);
      const QLocale locale;
      check(speed->text() == QStringLiteral("%1 it/s").arg(locale.toString(5.0, 'f', 1)) &&
          remaining->text() == QStringLiteral("00:16"),
          "200ms full wall step yields five iterations per second and sixteen seconds remaining");
      check(profile->text().contains(fullStepToken(language)) &&
          profile->text().contains(QStringLiteral("200.0 ms")) &&
          profile->text().contains(cudaToken(language)),
          "the real label distinguishes wall time from CUDA stage intervals");
      const QString engineText = QCoreApplication::translate("Workbench", "实际渲染后端：%1 · 损失计算：%2")
          .arg(backend == QStringLiteral("3dgs") ? QCoreApplication::translate("Workbench", "Fast-GS")
                   : QCoreApplication::translate("Workbench", "标准"),
               QCoreApplication::translate("Workbench", "融合 L1 + SSIM"));
      check(profile->text().contains(engineText) &&
          monitor->telemetry().performance().value(QStringLiteral("renderer_engine")).toString() ==
              (backend == QStringLiteral("3dgs") ? QStringLiteral("fastergs") : QStringLiteral("stock")),
          "actual renderer and loss engines are exposed, including a 2DGS stock-renderer fallback");
      const QString memoryText = QCoreApplication::translate("Workbench", "GPU 内存：Torch %1 MiB · 保留 %2 MiB · 峰值 %3 MiB · 设备空闲 %4 / %5 MiB")
          .arg(locale.toString(4), locale.toString(8), locale.toString(6),
               locale.toString(10240), locale.toString(12288));
      check(profile->text().contains(memoryText),
          "actual byte observations become exact allocated/reserved/peak/free/total MiB labels");
      const QStringList expectedStages{
          QCoreApplication::translate("Workbench", "准备") + QStringLiteral(" 1.0 ms"),
          QCoreApplication::translate("Workbench", "图像传输") + QStringLiteral(" 15.0 ms"),
          QCoreApplication::translate("Workbench", "渲染") + QStringLiteral(" 90.0 ms"),
          QCoreApplication::translate("Workbench", "损失计算") + QStringLiteral(" 10.0 ms"),
          QCoreApplication::translate("Workbench", "反向传播") + QStringLiteral(" 40.0 ms"),
          QCoreApplication::translate("Workbench", "增密") + QStringLiteral(" 5.0 ms"),
          QCoreApplication::translate("Workbench", "优化器更新") + QStringLiteral(" 15.0 ms"),
          QCoreApplication::translate("Workbench", "预览") + QStringLiteral(" 20.0 ms"),
          QCoreApplication::translate("Workbench", "检查点") + QStringLiteral(" 2.0 ms"),
          QCoreApplication::translate("Workbench", "上报") + QStringLiteral(" 2.0 ms"),
          QCoreApplication::translate("Workbench", "渲染") + QStringLiteral(" 80.0 ms"),
          QCoreApplication::translate("Workbench", "损失计算") + QStringLiteral(" 9.0 ms"),
          QCoreApplication::translate("Workbench", "反向传播") + QStringLiteral(" 35.0 ms")};
      for (const QString &value : expectedStages)
        check(profile->text().contains(value), "all named host and CUDA stage measurements remain visible");
      check(warning->isVisible() && warning->text().contains(qualityWarningToken(language)) &&
          warning->text().contains(QStringLiteral("%1 MiB").arg(locale.toString(1024))) &&
          warning->text().contains(QStringLiteral("32 MiB")) &&
          warning->text().contains(QStringLiteral("0 MiB")),
          "deferred growth displays observed free/extra-peak/admission capacity and the detail risk");

      const auto samples = monitor->telemetry().samples();
      if (!check(!samples.isEmpty(), "the running public monitor retains a real metric sample"))
        return false;
      const QJsonObject performance = monitor->telemetry().performance();
      const QJsonObject growth = monitor->telemetry().growth();
      const QString taskTitle = title->text();
      const int progressValue = progress->value();
      for (const QString &otherLanguage : AppLanguage::supported()) {
        check(AppLanguage::apply(otherLanguage, false), "switch a running monitor to another language");
        check(monitor->telemetry().samples().size() == samples.size() &&
            monitor->telemetry().samples().back().iteration == samples.back().iteration &&
            monitor->telemetry().samples().back().loss == samples.back().loss &&
            monitor->telemetry().performance() == performance && monitor->telemetry().growth() == growth &&
            monitor->telemetry().iteration().value() == 20 &&
            title->text() == taskTitle && progress->value() == progressValue,
            "language changes preserve samples, observers, task identity and progress");
        check(profile->text().contains(fullStepToken(otherLanguage)) &&
            warning->text().contains(qualityWarningToken(otherLanguage)) &&
            qualityEvidence->isVisible() &&
            qualityEvidence->text().contains(independentQualityToken(otherLanguage)),
            "performance and quality-warning language change immediately without restarting the task");
      }
      AppLanguage::apply(language, false);
      WorkerStatus fallback;
      fallback.state = QStringLiteral("running");
      fallback.stage = QStringLiteral("train");
      fallback.trainingPerformance = wallObservation(backend);
      fallback.trainingPerformance["iteration"] = 21;
      fallback.trainingPerformance["renderer_engine"] = "stock";
      fallback.trainingPerformance["loss_engine"] = "stock";
      monitor->updateStatus(fallback);
      const QString fallbackText = QCoreApplication::translate("Workbench", "实际渲染后端：%1 · 损失计算：%2")
          .arg(QCoreApplication::translate("Workbench", "标准"),
               QCoreApplication::translate("Workbench", "标准"));
      check(profile->text().contains(fallbackText) &&
          monitor->telemetry().performance().value(QStringLiteral("renderer_engine")).toString() ==
              QStringLiteral("stock") &&
          monitor->telemetry().performance().value(QStringLiteral("loss_engine")).toString() ==
              QStringLiteral("stock"),
          "a newer actual stock fallback replaces experimental renderer and loss captions");
      monitor->finishTraining(true, false);
      check(qualityEvidence->isVisible() &&
          qualityEvidence->text().contains(independentQualityToken(language)),
          "successful completion cannot fabricate an independent-quality pass");
      const QJsonObject finishedPerformance = monitor->telemetry().performance();
      for (const QString &finishedLanguage : AppLanguage::supported()) {
        AppLanguage::apply(finishedLanguage, false);
        check(qualityEvidence->isVisible() &&
            qualityEvidence->text().contains(independentQualityToken(finishedLanguage)) &&
            monitor->telemetry().performance() == finishedPerformance && progress->value() == 100,
            "completed-task language changes retain unavailable quality evidence and the measured result");
      }
      AppLanguage::apply(language, false);
      check(warning->isVisible() && warning->text().contains(qualityWarningToken(language)) &&
          remaining->text() == QStringLiteral("-"),
          "completion removes ETA but retains the last deferred-growth quality warning");
      WorkerStatus admitted;
      admitted.state = QStringLiteral("running");
      admitted.stage = QStringLiteral("train");
      admitted.trainingGrowth = growthObservation(backend, 21, true);
      monitor->updateStatus(admitted);
      check(warning->isHidden() && monitor->telemetry().growth().value(QStringLiteral("state")) ==
          QStringLiteral("admitted"), "a newer actual admission clears the old deferred warning");
      monitor->updateStatus(wall);
      check(warning->isHidden() && monitor->telemetry().growth().value(QStringLiteral("iteration")).toInt() == 21,
          "an older deferred snapshot cannot resurrect a cleared resource warning");
      WorkerStatus blocked;
      blocked.state = QStringLiteral("failed");
      blocked.stage = QStringLiteral("train");
      blocked.generationIssue = QStringLiteral("training_renderer_resume");
      const QJsonObject rendererBeforeFailure = monitor->telemetry().performance();
      const int samplesBeforeFailure = monitor->telemetry().samples().size();
      monitor->updateStatus(blocked);
      monitor->finishTraining(false, false);
      auto *failure = monitor->findChild<QLabel *>(QStringLiteral("reconstructionQualityLabel"));
      if (!check(failure != nullptr, "the public generation-failure explanation is available")) return false;
      for (const QString &failureLanguage : AppLanguage::supported()) {
        AppLanguage::apply(failureLanguage, false);
        check(failure->isVisible() && failure->text().contains(resumeBlockedToken(failureLanguage)) &&
            !failure->text().contains(QStringLiteral("training_renderer_resume")) &&
            monitor->telemetry().performance() == rendererBeforeFailure &&
            monitor->telemetry().samples().size() == samplesBeforeFailure,
            "renderer-resume blocking is localized without fabricating fallback or resetting recorded observations");
      }
      const QString preservedFailure = failure->text();
      blocked.generationIssue = QStringLiteral("unknown_resume_issue");
      monitor->updateStatus(blocked);
      check(failure->text() == preservedFailure,
          "an unknown issue identifier cannot replace a valid renderer-resume explanation");
      AppLanguage::apply(language, false);
      QCoreApplication::processEvents();
    }
  }
  ownedWindow.close();
  std::fprintf(stderr, "Training performance UI smoke: %s\n", passed ? "PASS" : "FAIL");
  std::fflush(stderr);
  return passed;
}

} // namespace gsw
