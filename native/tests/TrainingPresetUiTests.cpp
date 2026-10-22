#include "AppLanguage.h"
#include "TrainingDialog.h"
#include "TrainingRequestStore.h"
#include "CameraDatasetInfo.h"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSpinBox>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::fprintf(stderr, "Preset UI bootstrap: QApplication\n");
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  std::fprintf(stderr, "Preset UI bootstrap: QApplication ready\n");
  application.setOrganizationName(QStringLiteral("GSW-owned-preset-ui"));
  application.setApplicationName(QStringLiteral("GSW-owned-preset-ui"));
  const QString root = qEnvironmentVariable("GSW_TEST_ROOT", QDir::tempPath());
  if (!QDir().mkpath(root)) return 2;
  QTemporaryDir fixture(QDir(root).filePath(QStringLiteral("preset-ui-XXXXXX")));
  if (!fixture.isValid() || !QDir().mkpath(fixture.filePath(QStringLiteral("input")))) return 2;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
  QImage image(20, 16, QImage::Format_RGB32);
  image.fill(Qt::gray);
  if (!image.save(fixture.filePath(QStringLiteral("input/view.png")))) return 2;
  const QString initialLocale = argc > 3 ? QString::fromUtf8(argv[3]) : QStringLiteral("en_US");
  gsw::AppLanguage::apply(initialLocale, false);
  gsw::TrainingDialog dialog(fixture.path(), QStringLiteral("owned preset fixture"),
                             fixture.path(), false, true);
  auto *iterations = dialog.findChild<QSpinBox *>(QStringLiteral("trainingIterationsSpinBox"));
  auto *quality = dialog.findChild<QComboBox *>(QStringLiteral("trainingQualityCombo"));
  auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("trainingApplyQualityPreset"));
  auto *resolution = dialog.findChild<QComboBox *>(QStringLiteral("trainingResolutionCombo"));
  auto *policy = dialog.findChild<QComboBox *>(QStringLiteral("trainingResolutionPolicyCombo"));
  auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo"));
  auto *summary = dialog.findChild<QLabel *>(QStringLiteral("trainingQualityPresetSummary"));
  auto *pose = dialog.findChild<QComboBox *>(QStringLiteral("trainingPosePolicyCombo"));
  auto *guard = dialog.findChild<QCheckBox *>(QStringLiteral("trainingResourceGuard"));
  auto *staging = dialog.findChild<QCheckBox *>(QStringLiteral("trainingImageStaging"));
  auto *recommended = dialog.findChild<QPushButton *>(QStringLiteral("trainingApplyRecommendedExecution"));
  if (!iterations || !quality || !apply || !resolution || !policy || !backend || !summary ||
      !pose || !guard || !staging || !recommended) return 2;
  bool passed = true;
  int checks = 0;
  const auto check = [&](bool okay, const char *message) {
    ++checks;
    passed &= okay;
    std::printf("Preset UI %s: %s\n", okay ? "PASS" : "FAIL", message);
  };
  iterations->setValue(17000);
  quality->setCurrentIndex(quality->findData(QStringLiteral("max_quality")));
  check(dialog.configuration().iterations == 17000,
        "choosing a preset does not discard manually entered iterations");
  check(dialog.configuration().quality == QStringLiteral("quick"),
        "pending selection cannot silently change worker densification/optimizer profile");
  apply->click();
  check(dialog.configuration().iterations == 30000 && dialog.configuration().resolution == 2 &&
        dialog.configuration().quality == QStringLiteral("max_quality"),
        "explicit apply resets advertised iterations/ratio/profile");
  backend->setCurrentIndex(backend->findData(QStringLiteral("2dgs")));
  iterations->setValue(19000);
  resolution->setCurrentIndex(resolution->findData(4));
  quality->setCurrentIndex(quality->findData(QStringLiteral("full")));
  const auto edited = dialog.configuration();
  QSet<QString> applyCaptions, summaries;
  for (const auto &locale : gsw::AppLanguage::supported()) {
    check(gsw::AppLanguage::apply(locale, false), "locale changes immediately");
    applyCaptions.insert(apply->text());
    summaries.insert(summary->text());
    const auto value = dialog.configuration();
    check(value.backend == QStringLiteral("2dgs") && value.iterations == 19000 &&
          value.resolution == 4 && value.quality == QStringLiteral("max_quality") &&
          value.runtimeOptions == edited.runtimeOptions,
          "language changes preserve edited and applied parameters, not pending selection");
  }
  check(applyCaptions.size() == 3 && summaries.size() == 3,
        "apply action and precise pending changes have three actual live translations");
  // The persisted public request, not widget internals, is the reopen boundary.
  const QJsonObject saved = dialog.configurationDocument();
  check(saved.value(QStringLiteral("schema")).toString() == QStringLiteral("gsw.training_dialog_configuration/1") &&
        saved.value(QStringLiteral("quality")).toString() == QStringLiteral("max_quality") &&
        saved.value(QStringLiteral("iterations")).toInt() == 19000 &&
        !saved.contains(QStringLiteral("datasetPath")) && !saved.contains(QStringLiteral("outputRoot")) &&
        !saved.contains(QStringLiteral("outputScene")) && !saved.contains(QStringLiteral("overwrite")),
        "saved request contains applied values but no input/output identity or overwrite authority");
  const gsw::TrainingRequestStore requestStore(fixture.path(), fixture.path(), QStringLiteral("owned-dataset-identity"));
  QString storeError;
  check(requestStore.save(saved, &storeError) && storeError.isEmpty(),
        "actual project request store accepts the dialog's complete public request");
  const auto loaded = requestStore.load(&storeError);
  check(storeError.isEmpty() && loaded == saved,
        "actual project request store reads the same dataset-scoped request");
  gsw::TrainingDialog reopened(fixture.path(), QStringLiteral("new output identity"), fixture.path(), false, true);
  check(reopened.setConfigurationDocument(loaded) && reopened.configurationDocument() == saved,
        "save/read/reopen roundtrip keeps exact manual and applied requests");
  const auto original = reopened.configurationDocument();
  auto invalid = saved;
  invalid.insert(QStringLiteral("iterations"), 999);
  check(!reopened.setConfigurationDocument(invalid) && reopened.configurationDocument() == original,
        "invalid saved numbers are rejected atomically rather than silently clamped");
  invalid = saved;
  invalid.insert(QStringLiteral("outputRoot"), QStringLiteral("forbidden external destination"));
  check(!reopened.setConfigurationDocument(invalid) && reopened.configurationDocument() == original,
        "external state cannot inject output destination or publication authority");
  reopened.setCameraDatasetReference(QStringLiteral("owned-metadata.json"), QStringLiteral("owned-identity"));
  const auto lockedBefore = reopened.configuration();
  check(!reopened.setConfigurationDocument(saved) && reopened.configuration().posePolicy == QStringLiteral("locked") &&
        !reopened.configuration().runColmap && reopened.configuration().resolution == lockedBefore.resolution &&
        reopened.configuration().resolutionPolicy == lockedBefore.resolutionPolicy,
        "solve request cannot downgrade a current locked-camera input or resize it");
  const auto lockedRequest = reopened.configurationDocument();
  auto contradictory = lockedRequest;
  contradictory.insert(QStringLiteral("resolution"), 4);
  contradictory.insert(QStringLiteral("iterations"), 18000);
  check(!reopened.setConfigurationDocument(contradictory) && reopened.configurationDocument() == lockedRequest,
        "contradictory preserve/resolution request is rejected before any manual value changes");
  reopened.findChild<QSpinBox *>(QStringLiteral("trainingIterationsSpinBox"))->setValue(13000);
  reopened.findChild<QCheckBox *>(QStringLiteral("trainingResourceGuard"))->setChecked(false);
  const auto currentInput = reopened.configuration();
  check(reopened.setConfigurationDocument(saved, true) &&
        reopened.configuration().iterations == saved.value(QStringLiteral("iterations")).toInt() &&
        reopened.configuration().quality == saved.value(QStringLiteral("quality")).toString() &&
        reopened.configuration().runtimeOptions == saved.value(QStringLiteral("runtimeOptions")).toObject() &&
        reopened.configuration().posePolicy == currentInput.posePolicy && !reopened.configuration().runColmap &&
        reopened.configuration().resolutionPolicy == currentInput.resolutionPolicy &&
        reopened.configuration().resolution == currentInput.resolution,
        "project reopen restores independent manual values without reusing a saved solve or pixel policy");
  for (const QString &backendId : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    backend->setCurrentIndex(backend->findData(backendId));
    for (const QString &poseId : {QStringLiteral("solve"), QStringLiteral("locked")}) {
      pose->setCurrentIndex(pose->findData(poseId));
      iterations->setValue(23000);
      const auto before = dialog.configuration();
      quality->setCurrentIndex(quality->findData(QStringLiteral("quality")));
      guard->setChecked(!guard->isChecked());
      staging->setChecked(false);
      recommended->click();
      const auto after = dialog.configuration();
      check(after.backend == backendId && after.posePolicy == poseId &&
            after.iterations == 23000 && after.resolution == before.resolution &&
            after.resolutionPolicy == before.resolutionPolicy && after.quality == before.quality,
            "backend/mode matrix retains manual values through pending preset and independent execution edits");
      apply->click();
      check(dialog.configuration().quality == QStringLiteral("quality") && dialog.configuration().iterations == 30000 &&
            (poseId != QStringLiteral("locked") ||
              (dialog.configuration().resolution == before.resolution && dialog.configuration().resolutionPolicy == before.resolutionPolicy)),
            "explicit apply uses the new profile while locked input keeps its exact pixel policy");
      policy->setCurrentIndex(policy->findData(QStringLiteral("preserve")));
      quality->setCurrentIndex(quality->findData(QStringLiteral("quick")));
      apply->click();
      check(dialog.configuration().resolutionPolicy == QStringLiteral("preserve") && dialog.configuration().resolution == 1 &&
            dialog.configuration().iterations == 10000 && dialog.configuration().quality == QStringLiteral("quick"),
            "preserve resolution survives explicit preset reapplication in both camera modes/backends");
      policy->setCurrentIndex(policy->findData(QStringLiteral("manual")));
      resolution->setCurrentIndex(resolution->findData(4));
      iterations->setValue(21000);
      const auto retained = dialog.configurationDocument();
      for (const auto &locale : gsw::AppLanguage::supported()) {
        gsw::AppLanguage::apply(locale, false);
        check(dialog.configurationDocument() == retained,
              "all locales preserve the complete request in the backend/camera-mode matrix");
      }
      gsw::TrainingDialog restored(fixture.path(), QStringLiteral("separate output"), fixture.path(), false, true);
      if (poseId == QStringLiteral("locked")) {
        restored.setCameraDatasetReference(QStringLiteral("owned-policy-reference.json"), QStringLiteral("owned-policy-identity"));
      }
      check(requestStore.save(retained, &storeError) && storeError.isEmpty() &&
            restored.setConfigurationDocument(requestStore.load(&storeError)) && storeError.isEmpty() &&
            restored.configurationDocument() == retained,
            "project-store reopen restores manual values in both backend/camera-mode contexts");
      std::printf("PRESET_CASE %s\n", QJsonDocument(restored.configurationDocument()).toJson(QJsonDocument::Compact).constData());
    }
  }
  pose->setCurrentIndex(pose->findData(QStringLiteral("solve")));
  const auto beforeBackend = dialog.configuration();
  backend->setCurrentIndex(backend->findData(QStringLiteral("3dgs")));
  check(dialog.configuration().iterations == beforeBackend.iterations &&
        dialog.configuration().resolution == beforeBackend.resolution &&
        dialog.configuration().resolutionPolicy == beforeBackend.resolutionPolicy &&
        dialog.configuration().quality == beforeBackend.quality,
        "changing backend after manual edits keeps iteration, pixel policy and applied profile");
  pose->setCurrentIndex(pose->findData(QStringLiteral("locked")));
  check(dialog.configuration().iterations == 21000 && dialog.configuration().quality == QStringLiteral("quick") &&
        dialog.configuration().resolution == 1 && dialog.configuration().resolutionPolicy == QStringLiteral("preserve") &&
        !dialog.configuration().runColmap,
        "entering locked mode changes only the explicitly documented camera/pixel constraints");
  pose->setCurrentIndex(pose->findData(QStringLiteral("solve")));
  check(dialog.configuration().iterations == 21000 && dialog.configuration().resolution == 4 &&
        dialog.configuration().resolutionPolicy == QStringLiteral("manual"),
        "returning to solve recovers the previous manual ratio without resetting iterations");
  apply->click();
  check(dialog.configuration().quality == QStringLiteral("quick") && dialog.configuration().iterations == 10000 &&
        dialog.configuration().resolution == 2,
        "explicitly reapplying the same preset intentionally resets its advertised manual overrides");
  const QString rawPathError = QStringLiteral("camera_solve_path_budget_exceeded: owned-example");
  QSet<QString> pathGuidance;
  for (const auto &language : gsw::AppLanguage::supported()) {
    gsw::AppLanguage::apply(language, false);
    const QString guidance = gsw::CameraDatasetInfo::importErrorText(rawPathError);
    check(guidance != rawPathError && guidance.contains(rawPathError),
          "unsupported path guidance is localized while the original diagnostic is retained");
    pathGuidance.insert(guidance);
  }
  check(pathGuidance.size() == 3, "path guidance is distinct in all three live languages");
  std::printf("PRESET_CONFIGURATION %s\n", QJsonDocument(saved).toJson(QJsonDocument::Compact).constData());
  std::printf("Preset UI completed: %d checks\n", checks);
  return passed ? 0 : 1;
}
