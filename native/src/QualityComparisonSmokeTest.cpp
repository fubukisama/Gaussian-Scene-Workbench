#include "QualityComparisonSmokeTest.h"
#include "QualityComparisonDialog.h"
#include "AppLanguage.h"
#include "BackendLocator.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool waitFor(const std::function<bool()> &condition) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < 15000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return condition();
}
bool writeJson(const QString &path, const QJsonObject &report) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
}
QByteArray digest(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
QJsonObject measured(const double psnr, const QString &split = QStringLiteral("held_out")) {
  return {{"version", 1}, {"identity", QJsonObject{
      {"dataset_id", "synthetic-dataset"}, {"viewset_id", "held-out-views"},
      {"resolution_id", "3804x2142"}, {"mask_id", "none"},
      {"metric_protocol_id", "synthetic-protocol-no-physical-unit-claim"}}},
      {"split", split}, {"metrics", QJsonObject{{"psnr", psnr}, {"ssim", .92}, {"lpips", .12}}},
      {"settings", QJsonObject{{"iterations", psnr == 30. ? 30000 : 15000}}}};
}
QString untranslatedStatusToken(const QString &language) {
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("未验证");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("未検証");
  return QStringLiteral("Unverified");
}
} // namespace

bool runQualityComparisonSmokeTest() {
  bool passed = true;
  const QString originalLanguage = AppLanguage::current();
  const QLocale originalLocale;
  struct RestoreLanguage {
    QString language;
    QLocale locale;
    ~RestoreLanguage() { AppLanguage::apply(language, false); QLocale::setDefault(locale); }
  } restore{originalLanguage, originalLocale};
  const auto check = [&](const bool condition, const char *message) {
    std::fprintf(stderr, "Quality comparison %s: %s\n", condition ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    passed = passed && condition;
    return condition;
  };
  const QString root = BackendLocator::findRepositoryRoot(QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  QString python = qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");
  if (python.isEmpty()) python = BackendLocator::findGaussianPython(root);
  QTemporaryDir fixture;
  if (!check(!root.isEmpty() && !python.isEmpty() && fixture.isValid(),
      "real source, Python and disposable workspace are available")) return false;
  const QString baseline = QDir(fixture.path()).filePath(QStringLiteral("baseline.json"));
  const QString candidate = QDir(fixture.path()).filePath(QStringLiteral("candidate.json"));
  QJsonObject reference = measured(30.);
  QJsonObject proposed = measured(29.9);
  if (!check(writeJson(baseline, reference) && writeJson(candidate, proposed),
      "test-owned baseline and candidate reports are written")) return false;
  QualityComparisonDialog dialog(python, root, fixture.path(), QProcessEnvironment::systemEnvironment());
  dialog.setReportPaths(baseline, candidate);
  dialog.show(); // Remote acceptance only; never raise or activate the window.
  if (!check(waitFor([&] { return dialog.windowHandle() && dialog.windowHandle()->isExposed(); }),
      "the actual test-owned quality dialog is exposed")) return false;
  auto *compare = dialog.findChild<QPushButton *>(QStringLiteral("qualityCompareButton"));
  auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("qualityCancelButton"));
  auto *save = dialog.findChild<QPushButton *>(QStringLiteral("qualitySaveButton"));
  auto *summary = dialog.findChild<QLabel *>(QStringLiteral("qualityComparisonSummary"));
  auto *psnrLimit = dialog.findChild<QLineEdit *>(QStringLiteral("quality_psnr_max_regression"));
  if (!check(compare && cancel && save && summary && psnrLimit,
      "comparison, cancellation, quality summary and editable tolerance are public controls")) return false;
  check(!save->isEnabled(), "a comparison report cannot be saved before any valid result exists");
  const auto run = [&]() {
    const QByteArray baselineHash = digest(baseline), candidateHash = digest(candidate);
    compare->click();
    const bool finished = waitFor([&] { return !dialog.isRunning(); });
    return check(finished && digest(baseline) == baselineHash && digest(candidate) == candidateHash,
        "the real CLI finishes without modifying either supplied measurement report");
  };
  run();
  check(dialog.comparisonReport().value(QStringLiteral("status")) == QStringLiteral("unverified"),
      "no user thresholds means unverified, never an implicit quality pass");
  QJsonObject contract{{"version", 1}, {"required_domains", QJsonArray{"image"}},
      {"metrics", QJsonObject{{"psnr", QJsonObject{{"max_regression", .2}}}}}};
  dialog.setContract(contract);
  run();
  check(dialog.comparisonReport().value(QStringLiteral("status")) == QStringLiteral("pass") &&
      !dialog.comparisonReport().value(QStringLiteral("settings_changes")).toArray().isEmpty(),
      "explicit tolerances can pass while the actual settings changes remain visible");
  const QString exportPath = QDir(fixture.path()).filePath(QStringLiteral("comparison.json"));
  const QByteArray originalBaseline = digest(baseline), originalCandidate = digest(candidate);
  check(save->isEnabled() && dialog.saveComparisonReport(exportPath),
      "the actual report can be saved to an explicitly selected fresh file");
  QFile exported(exportPath);
  QJsonObject saved;
  if (exported.open(QIODevice::ReadOnly)) {
    saved = QJsonDocument::fromJson(exported.readAll()).object();
    exported.close();
  }
  check(saved.value(QStringLiteral("version")) == 1 &&
      saved.value(QStringLiteral("comparison")).toObject() == dialog.comparisonReport() &&
      saved.value(QStringLiteral("contract")).toObject() == dialog.contract(),
      "saved JSON retains the exact verdict, settings changes and selected thresholds");
  const QByteArray exportedHash = digest(exportPath);
  check(!dialog.saveComparisonReport(exportPath) && !dialog.saveComparisonReport(baseline) &&
      digest(exportPath) == exportedHash && digest(baseline) == originalBaseline && digest(candidate) == originalCandidate,
      "saving refuses an existing file or input report without changing any original bytes");
  psnrLimit->setText(QStringLiteral("0.05"));
  check(dialog.comparisonReport().isEmpty(),
      "editing a threshold invalidates the previous verdict before recomparison");
  check(!save->isEnabled(), "changing a threshold disables export until the next completed comparison");
  run();
  check(dialog.comparisonReport().value(QStringLiteral("status")) == QStringLiteral("regressed"),
      "editing the actual tolerance control changes the machine verdict");
  QJsonObject geometryContract = contract;
  geometryContract["required_domains"] = QJsonArray{"image", "geometry"};
  dialog.setContract(geometryContract);
  run();
  check(dialog.comparisonReport().value(QStringLiteral("status")) == QStringLiteral("unverified") &&
      dialog.comparisonReport().value(QStringLiteral("domains")).toObject()
          .value(QStringLiteral("geometry")).toObject().value(QStringLiteral("status")) == QStringLiteral("unverified"),
      "requiring unmeasured geometry does not invent a geometry-quality pass");
  geometryContract["metrics"] = QJsonObject{{"psnr", QJsonObject{{"max_regression", .2}}},
      {"depth_rmse", QJsonObject{{"absolute_max", .02}}}};
  dialog.setContract(geometryContract);
  run();
  check(dialog.comparisonReport().value(QStringLiteral("metrics")).toObject()
      .value(QStringLiteral("depth_rmse")).toObject().value(QStringLiteral("status")) == QStringLiteral("unverified"),
      "an explicitly required but missing geometry measurement remains visibly unverified");
  reference = measured(30., QStringLiteral("training_fit"));
  proposed = measured(29.9, QStringLiteral("training_fit"));
  writeJson(baseline, reference);
  writeJson(candidate, proposed);
  dialog.setContract(contract);
  run();
  check(dialog.comparisonReport().value(QStringLiteral("status")) == QStringLiteral("unverified") &&
      !dialog.comparisonReport().value(QStringLiteral("independent_validation")).toBool(),
      "training-fit results cannot become independent validation");
  const QJsonObject retainedReport = dialog.comparisonReport();
  const QJsonObject retainedContract = dialog.contract();
  for (const QString &language : AppLanguage::supported()) {
    AppLanguage::apply(language, false);
    check(dialog.comparisonReport() == retainedReport && dialog.contract() == retainedContract &&
        summary->text().contains(untranslatedStatusToken(language)),
        "language changes translate the verdict and preserve input thresholds and machine report");
  }
  reference = measured(30.);
  proposed = measured(29.9);
  auto identity = proposed.value(QStringLiteral("identity")).toObject();
  identity["mask_id"] = QStringLiteral("different-mask");
  proposed["identity"] = identity;
  writeJson(baseline, reference);
  writeJson(candidate, proposed);
  run();
  check(dialog.comparisonReport().value(QStringLiteral("status")) == QStringLiteral("not_comparable"),
      "mismatched measurement inputs are not presented as a regression or pass");
  proposed = measured(29.9);
  writeJson(candidate, proposed);
  compare->click();
  check(dialog.isRunning(), "the real comparer starts asynchronously");
  cancel->click();
  check(waitFor([&] { return !dialog.isRunning(); }) && dialog.comparisonReport().isEmpty(),
      "cancel stops only the owned comparison helper and does not retain a stale pass");
  QFile invalid(candidate);
  if (invalid.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    invalid.write("{\"version\":1,\"version\":1}");
    invalid.close();
  }
  run();
  check(dialog.comparisonReport().isEmpty(), "invalid duplicate-key JSON is not a quality result");
  const QString oversized = QDir(fixture.path()).filePath(QStringLiteral("oversized.json"));
  QFile oversizedFile(oversized);
  if (oversizedFile.open(QIODevice::WriteOnly)) {
    oversizedFile.resize(16 * 1024 * 1024 + 1);
    oversizedFile.close();
  }
  dialog.setReportPaths(baseline, oversized);
  compare->click();
  check(!dialog.isRunning() && dialog.comparisonReport().isEmpty(),
      "oversized input is rejected before a comparison process starts");
  dialog.setReportPaths(baseline, candidate);
  writeJson(candidate, proposed);
  compare->click();
  dialog.close();
  check(waitFor([&] { return !dialog.isRunning() && !dialog.isVisible(); }),
      "closing during comparison cancels its owned process before closing the dialog");
  AppLanguage::apply(originalLanguage, false);
  std::fprintf(stderr, "Quality comparison UI smoke: %s\n", passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
