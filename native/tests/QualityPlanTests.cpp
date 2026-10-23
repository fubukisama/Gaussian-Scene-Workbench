#include "AppLanguage.h"
#include "TrainingDialog.h"
#include "TrainingRequestStore.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QImage>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>
#include <limits>

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  application.setOrganizationName(QStringLiteral("GSW-owned-quality-plan-ui"));
  application.setApplicationName(QStringLiteral("GSW-owned-quality-plan-ui"));
  const QString root = qEnvironmentVariable("GSW_TEST_ROOT", QDir::tempPath());
  if (!QDir().mkpath(root)) return 2;
  QTemporaryDir fixture(QDir(root).filePath(QStringLiteral("quality-plan-ui-XXXXXX")));
  if (!fixture.isValid() || !QDir().mkpath(fixture.filePath(QStringLiteral("input")))) return 2;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
  QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::gray);
  if (!image.save(fixture.filePath(QStringLiteral("input/owned.png")))) return 2;
  gsw::AppLanguage::apply(QStringLiteral("en_US"), false);
  gsw::TrainingDialog dialog(fixture.path(), QStringLiteral("owned quality plan"), fixture.path(), false, true);
  bool passed = true;
  const auto check = [&](bool okay, const char *message) {
    passed &= okay;
    std::printf("Quality plan UI %s: %s\n", okay ? "PASS" : "FAIL", message);
  };
  const QString diagnostic = QStringLiteral("quality_mip_raw_requires_fusion: owned.ply");
  QSet<QString> localizedDiagnostics;
  for (const QString &locale : {QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")}) {
    check(gsw::AppLanguage::apply(locale, false), "diagnostic locale applies immediately");
    const QString text = gsw::AppLanguage::diagnosticText(diagnostic);
    check(text != diagnostic && text.endsWith(diagnostic), "localized diagnostic retains exact machine identifier and user filename");
    localizedDiagnostics.insert(text);
  }
  check(localizedDiagnostics.size() == 3, "quality diagnostic follows all three live languages without a worker restart");
  gsw::AppLanguage::apply(QStringLiteral("en_US"), false);
  const auto legacy = dialog.configurationDocument();
  check(!legacy.contains(QStringLiteral("qualityPlan")), "legacy defaults do not create a quality plan or new RNG policy");
  const QJsonObject plan{{QStringLiteral("version"), 1},
      {QStringLiteral("geometry_lr_multiplier"), .25},
      {QStringLiteral("densification"), QJsonObject{{QStringLiteral("strategy"), QStringLiteral("abs_gs")},
          {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("split_threshold"), .0012}}}}},
      {QStringLiteral("filter"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("backend_default")}}}};
  auto requested = legacy;
  requested.insert(QStringLiteral("qualityPlan"), plan);
  check(dialog.setConfigurationDocument(requested), "optional experimental quality plan is accepted through the public request interface");
  check(dialog.configurationDocument() == requested, "quality plan parameters survive request roundtrip without normalization or invented hash");
  auto invalid = requested;
  auto invalidPlan = plan;
  invalidPlan.insert(QStringLiteral("quality_accepted"), true);
  invalid.insert(QStringLiteral("qualityPlan"), invalidPlan);
  check(!dialog.setConfigurationDocument(invalid) && dialog.configurationDocument() == requested,
        "unknown quality fields are rejected atomically and cannot fabricate acceptance");
  auto *enabled = dialog.findChild<QCheckBox *>(QStringLiteral("trainingQualityPlanEnabled"));
  auto *multiplier = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("trainingGeometryLrMultiplier"));
  auto *strategy = dialog.findChild<QComboBox *>(QStringLiteral("trainingDensificationStrategy"));
  auto *filter = dialog.findChild<QComboBox *>(QStringLiteral("trainingSamplingFilter"));
  check(enabled && multiplier && strategy && filter, "experimental controls exist as actual editable Qt widgets");
  if (enabled && multiplier && strategy && filter) {
    check(enabled->isChecked() && multiplier->value() == .25 && strategy->currentData() == QStringLiteral("abs_gs"),
          "imported known values are reflected without discarding method parameters");
    multiplier->setValue(.5);
    check(dialog.configuration().qualityPlan.value(QStringLiteral("densification")) == plan.value(QStringLiteral("densification")),
          "changing only geometry multiplier preserves imported method parameters");
    const int mcmc = strategy->findData(QStringLiteral("mcmc"));
    check(mcmc >= 0 && !(strategy->model()->flags(strategy->model()->index(mcmc, 0)) & Qt::ItemIsEnabled),
          "MCMC is visible but not selectable before trainer/GPU acceptance");
    auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo"));
    auto *start = dialog.findChild<QPushButton *>(QStringLiteral("trainingStartButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("trainingQualityPlanStatus"));
    check(backend && start && status, "compatibility is presented through actual start/status controls");
    if (backend && start && status) {
      filter->setCurrentIndex(filter->findData(QStringLiteral("full_mip")));
      check(!start->isEnabled() && !dialog.qualityPlanValidationText().isEmpty() &&
            dialog.configuration().qualityPlan.value(QStringLiteral("densification")).toObject()
                .value(QStringLiteral("strategy")).toString() == QStringLiteral("abs_gs"),
            "unvalidated combination is retained and blocked rather than silently changed");
      filter->setCurrentIndex(filter->findData(QStringLiteral("backend_default")));
      const auto beforeBackend = dialog.configuration().qualityPlan;
      backend->setCurrentIndex(backend->findData(QStringLiteral("2dgs")));
      check(!start->isEnabled() && dialog.configuration().qualityPlan == beforeBackend &&
            !(strategy->model()->flags(strategy->model()->index(strategy->findData(QStringLiteral("abs_gs")), 0)) & Qt::ItemIsEnabled) &&
            !(filter->model()->flags(filter->model()->index(filter->findData(QStringLiteral("full_mip")), 0)) & Qt::ItemIsEnabled),
            "2DGS rejects 3D-only methods without fallback or JSON mutation");
      backend->setCurrentIndex(backend->findData(QStringLiteral("3dgs")));
      check(start->isEnabled() && dialog.qualityPlanValidationText().isEmpty(), "explicitly returning to a supported backend removes only the compatibility block");
      enabled->setChecked(false);
      check(dialog.configuration().qualityPlan.isEmpty() && !dialog.configurationDocument().contains(QStringLiteral("qualityPlan")),
            "explicitly disabling the feature omits the worker/saved plan without creating a replacement");
      enabled->setChecked(true);
      check(dialog.configuration().qualityPlan == beforeBackend, "re-enabling retains the user's experimental request");
      const auto invariant = dialog.configurationDocument();
      QSet<QString> captions, summaries, mcmcHints;
      for (const auto &language : gsw::AppLanguage::supported()) {
        check(gsw::AppLanguage::apply(language, false), "language switches immediately in the open dialog");
        check(dialog.configurationDocument() == invariant, "language changes do not rewrite the plan or legacy runtime/input policies");
        captions.insert(enabled->text()); summaries.insert(status->text());
        mcmcHints.insert(strategy->itemData(mcmc, Qt::ToolTipRole).toString());
      }
      check(captions.size() == 3 && summaries.size() == 3 && mcmcHints.size() == 3,
            "experimental captions, status and unavailable-method tooltips have three live translations");
    }
  }
  const QJsonObject mask{{QStringLiteral("manifest"), QStringLiteral("preserve-relative-mask.json")},
      {QStringLiteral("foreground_weight"), .8}, {QStringLiteral("background_alpha_weight"), .2},
      {QStringLiteral("unit_to_training_scale"), .001},
      {QStringLiteral("scale_evidence_identity"), QString(64, QLatin1Char('a'))}};
  const QJsonObject supervised{{QStringLiteral("version"), 1}, {QStringLiteral("geometry_lr_multiplier"), .75},
      {QStringLiteral("densification"), QJsonObject{{QStringLiteral("strategy"), QStringLiteral("standard")}, {QStringLiteral("parameters"), QJsonObject()}}},
      {QStringLiteral("filter"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("backend_default")}}},
      {QStringLiteral("supervision"), QJsonObject{{QStringLiteral("mask"), mask}}}};
  const QString planPath = fixture.filePath(QStringLiteral("owned-quality-plan.json"));
  QFile planFile(planPath);
  check(planFile.open(QIODevice::WriteOnly) && planFile.write(QJsonDocument(supervised).toJson()) > 0,
        "test owns an actual quality-plan JSON file");
  planFile.close();
  check(dialog.importQualityPlanFile(planPath) && dialog.configuration().qualityPlan == supervised,
        "actual JSON import preserves supervision parameters, paired registration scale/evidence and raw paths");
  if (multiplier) {
    multiplier->setValue(1.5);
    check(dialog.configuration().qualityPlan.value(QStringLiteral("supervision")) == supervised.value(QStringLiteral("supervision")),
          "editing geometry multiplier does not discard imported supervision or registration evidence");
  }
  const auto importedState = dialog.configurationDocument();
  auto badSupervision = supervised;
  auto unpaired = mask; unpaired.remove(QStringLiteral("scale_evidence_identity"));
  badSupervision.insert(QStringLiteral("supervision"), QJsonObject{{QStringLiteral("mask"), unpaired}});
  check(!dialog.setQualityPlanDocument(badSupervision) && dialog.configurationDocument() == importedState,
        "unpaired mask registration scale/evidence is rejected atomically");
  auto badMethod = supervised;
  badMethod.insert(QStringLiteral("densification"), QJsonObject{{QStringLiteral("strategy"), QStringLiteral("mcmc")}});
  check(!dialog.setQualityPlanDocument(badMethod) && dialog.configurationDocument() == importedState,
        "JSON import cannot bypass the unavailable MCMC method gate");
  badMethod = supervised; badMethod.insert(QStringLiteral("geometry_lr_multiplier"), 0.);
  check(!dialog.setQualityPlanDocument(badMethod) && dialog.configurationDocument() == importedState,
        "nonpositive multiplier is rejected before modifying controls");
  badMethod = supervised;
  badMethod.insert(QStringLiteral("geometry_lr_multiplier"), std::numeric_limits<double>::denorm_min());
  check(!dialog.setQualityPlanDocument(badMethod) && dialog.configurationDocument() == importedState,
        "a positive value that the native editor would round to zero is rejected rather than silently clamped");
  const gsw::TrainingRequestStore store(fixture.path(), fixture.path(), QStringLiteral("owned-quality-plan-identity"));
  QString error;
  check(store.save(importedState, &error) && store.load(&error) == importedState && error.isEmpty(),
        "real project store preserves the complete quality request");
  gsw::TrainingDialog reopened(fixture.path(), QStringLiteral("separate result"), fixture.path(), false, true);
  check(reopened.setConfigurationDocument(store.load(&error)) && reopened.configurationDocument() == importedState,
        "reopened dialog preserves the exact stored plan without probing its supervision paths");
  check(reopened.setConfigurationDocument(legacy) && reopened.configuration().qualityPlan.isEmpty() &&
        !reopened.configurationDocument().contains(QStringLiteral("qualityPlan")),
        "old nine-field configuration clears opt-in quality and remains compatible");
  dialog.resize(360, 280); dialog.show(); application.processEvents();
  const auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("dialogBodyScroll"));
  check(scroll && scroll->verticalScrollBar()->maximum() > 0,
        "new controls remain in the existing resizable scroll body without starting any worker");
  dialog.hide();
  return passed ? 0 : 1;
}
