#include "QualityMeasurementSmokeTest.h"
#include "QualityMeasurementDialog.h"
#include "RecoveryStore.h"
#include "QualityComparisonDialog.h"
#include "AppLanguage.h"
#include "BackendLocator.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <cmath>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool waitFor(const std::function<bool()> &condition) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < 20000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return condition();
}
QByteArray digest(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
bool writeJson(const QString &path, const QJsonObject &object) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
      file.write(QJsonDocument(object).toJson()) > 0;
}
} // namespace

bool runQualityMeasurementSmokeTest() {
  bool passed = true;
  struct RestoreLanguage {
    QString language = AppLanguage::current();
    QLocale locale;
    ~RestoreLanguage() { AppLanguage::apply(language, false); QLocale::setDefault(locale); }
  } restore;
  const auto check = [&](const bool value, const char *message) {
    std::fprintf(stderr, "Quality measurement %s: %s\n", value ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    passed = passed && value;
    return value;
  };
  const QString root = BackendLocator::findRepositoryRoot(QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  QString python = qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");
  if (python.isEmpty()) python = BackendLocator::findGaussianPython(root);
  QTemporaryDir fixture;
  if (!check(!root.isEmpty() && !python.isEmpty() && fixture.isValid(),
      "real Python, producer source and an isolated workspace are available")) return false;
  const QString reference = QDir(fixture.path()).filePath(QStringLiteral("reference.png"));
  const QString render = QDir(fixture.path()).filePath(QStringLiteral("render.png"));
  const QString manifest = QDir(fixture.path()).filePath(QStringLiteral("pairs.json"));
  QImage image(16, 16, QImage::Format_RGB888);
  image.fill(QColor(127, 127, 127));
  const bool referenceSaved = image.save(reference, "PNG");
  image.fill(QColor(128, 128, 128));
  if (!check(referenceSaved && image.save(render, "PNG") && writeJson(manifest,
      {{"version", 1}, {"views", QJsonArray{QJsonObject{{"id", "fixture-view"},
          {"reference", "reference.png"}, {"render", "render.png"}}}}}),
      "real RGB PNG pairs and an explicit relative-path manifest are written")) return false;
  const QByteArray referenceHash = digest(reference), renderHash = digest(render), manifestHash = digest(manifest);
  QualityMeasurementDialog dialog(python, root, fixture.path(), QProcessEnvironment::systemEnvironment());
  dialog.setManifestPath(manifest);
  dialog.show(); // Remote acceptance only. Never raise or activate a local user window.
  if (!check(waitFor([&] { return dialog.windowHandle() && dialog.windowHandle()->isExposed(); }),
      "the actual test-owned measurement dialog is exposed")) return false;
  auto *start = dialog.findChild<QPushButton *>(QStringLiteral("qualityMeasureStart"));
  auto *baseline = dialog.findChild<QPushButton *>(QStringLiteral("qualityUseAsBaseline"));
  auto *candidate = dialog.findChild<QPushButton *>(QStringLiteral("qualityUseAsCandidate"));
  auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("qualityMeasuredViews"));
  if (!check(start && baseline && candidate && table,
      "measurement and explicit baseline/candidate selection are public controls")) return false;
  int selected = 0;
  QString selectedPath;
  bool selectedBaseline = true;
  QObject::connect(&dialog, &QualityMeasurementDialog::reportChosen, &dialog,
      [&](const QString &path, const bool asBaseline) { ++selected; selectedPath = path; selectedBaseline = asBaseline; });
  check(!baseline->isEnabled() && !candidate->isEnabled(), "no report can be selected before measurement");
  start->click();
  check(dialog.isRunning(), "the real CPU producer starts asynchronously");
  if (!check(waitFor([&] { return !dialog.isRunning(); }), "the actual image measurement helper completes")) return false;
  const auto report = dialog.measurementReport();
  const auto metrics = report.value(QStringLiteral("metrics")).toObject();
  check(std::abs(metrics.value(QStringLiteral("psnr")).toDouble() - 48.1308036087) < 0.000001 &&
      metrics.value(QStringLiteral("ssim")).toDouble() > .99 && table->rowCount() == 1,
      "one-byte RGB difference produces the independently known PSNR and a visible measured SSIM row");
  check(report.value(QStringLiteral("split")) == QStringLiteral("unverified") &&
      !report.value(QStringLiteral("evidence")).toObject().value(QStringLiteral("held_out_verified")).toBool(),
      "unproven view provenance never becomes independent validation");
  check(selected == 0 && QFileInfo(dialog.reportPath()).isFile(),
      "the fresh workspace report persists without automatically filling a comparison input");
  candidate->click();
  check(selected == 1 && !selectedBaseline && selectedPath == dialog.reportPath(),
      "only an explicit user candidate selection publishes the measured report path");
  baseline->click();
  check(selected == 2 && selectedBaseline, "the user can explicitly choose the same measurement as a baseline");
  const QString changedReport = dialog.reportPath();
  const QByteArray trustedReportHash = digest(changedReport);
  QFile changed(changedReport);
  if (!check(changed.open(QIODevice::WriteOnly | QIODevice::Truncate) && changed.write("external-edit") == 13,
      "the test can externally alter only its own adopted report")) return false;
  changed.close();
  const QByteArray changedHash = digest(changedReport);
  candidate->click();
  check(selected == 3 && !selectedBaseline && selectedPath != changedReport &&
      QFileInfo(selectedPath).isFile() && digest(selectedPath) == trustedReportHash && digest(changedReport) == changedHash,
      "repeated adoption republishes a fresh trusted report instead of using or overwriting externally modified bytes");
  const QString deletedReport = dialog.reportPath();
  if (!check(QFile::remove(deletedReport), "the test can externally delete only its own adopted report")) return false;
  baseline->click();
  check(selected == 4 && selectedBaseline && selectedPath != deletedReport &&
      QFileInfo(selectedPath).isFile() && digest(selectedPath) == trustedReportHash,
      "repeated adoption never emits a deleted cached path and republishes the retained measurement freshly");
  const QJsonObject retainedReport = dialog.measurementReport();
  const int retainedSelectionCount = selected;
  const QString retainedPath = dialog.reportPath();
  const QByteArray retainedHash = digest(retainedPath);
  auto *summary = dialog.findChild<QLabel *>(QStringLiteral("qualityMeasurementSummary"));
  for (const QString &language : AppLanguage::supported()) {
    AppLanguage::apply(language, false);
    const QString title = language == QStringLiteral("zh_CN") ? QString::fromUtf8("图像质量实测")
        : language == QStringLiteral("ja_JP") ? QString::fromUtf8("画像品質の測定") : QString::fromUtf8("Image Quality Measurement");
    const QString noProof = language == QStringLiteral("zh_CN") ? QString::fromUtf8("独立验证：未提供")
        : language == QStringLiteral("ja_JP") ? QString::fromUtf8("独立検証：未提供") : QString::fromUtf8("Independent validation: not provided");
    check(dialog.windowTitle() == title && summary && summary->text().contains(noProof) &&
        dialog.measurementReport() == retainedReport && dialog.manifestPath() == manifest &&
        dialog.reportPath() == retainedPath && selected == retainedSelectionCount,
        "all three live languages retain measurements, the manifest and explicit selection without claiming validation");
  }
  check(digest(reference) == referenceHash && digest(render) == renderHash && digest(manifest) == manifestHash,
      "measurement leaves all original image and manifest bytes unchanged");
  dialog.close();
  check(QFileInfo(retainedPath).isFile() && digest(retainedPath) == retainedHash &&
      QDir::fromNativeSeparators(retainedPath).contains(QStringLiteral("/.gsw/quality/measurements/")),
      "an explicitly selected immutable workspace report survives closing its measurement window");
  RecoveryStore recovery(fixture.path());
  QString recoveryError;
  const auto disposable = recovery.beginWorkspace(QStringLiteral("quality-measurement-storage"), &recoveryError);
  check(disposable.has_value() && recovery.discardWorkspace(*disposable, &recoveryError) &&
      QFileInfo(retainedPath).isFile() && digest(retainedPath) == retainedHash,
      "discarding a disposable project leaves adopted reports in the application storage base intact");

  QualityComparisonDialog comparison(python, root, fixture.path(), QProcessEnvironment::systemEnvironment());
  comparison.setReportPaths(QStringLiteral("original-baseline.json"), QStringLiteral("original-candidate.json"));
  comparison.show();
  auto *measureImages = comparison.findChild<QPushButton *>(QStringLiteral("qualityMeasureImagesButton"));
  auto *comparisonBaseline = comparison.findChild<QLineEdit *>(QStringLiteral("qualityBaselinePath"));
  auto *comparisonCandidate = comparison.findChild<QLineEdit *>(QStringLiteral("qualityCandidatePath"));
  if (!check(measureImages && comparisonBaseline && comparisonCandidate,
      "the comparison window exposes an explicit image-measurement action")) return false;
  measureImages->click();
  QPointer<QualityMeasurementDialog> child = comparison.findChild<QualityMeasurementDialog *>();
  if (!check(child && waitFor([&] { return child->isVisible(); }),
      "only invoking the public comparison action opens its measurement window")) return false;
  child->setManifestPath(manifest);
  child->findChild<QPushButton *>(QStringLiteral("qualityMeasureStart"))->click();
  if (!check(waitFor([&] { return !child->isRunning(); }) && !child->measurementReport().isEmpty(),
      "the comparison workflow executes the same real CPU producer")) return false;
  check(comparisonBaseline->text() == QStringLiteral("original-baseline.json") &&
      comparisonCandidate->text() == QStringLiteral("original-candidate.json"),
      "finishing measurement never silently replaces either comparison input");
  child->findChild<QPushButton *>(QStringLiteral("qualityUseAsCandidate"))->click();
  const QString publishedCandidate = child->reportPath();
  child->close();
  check(waitFor([&] { return child.isNull(); }) && QFileInfo(publishedCandidate).isFile() &&
      comparisonBaseline->text() == QStringLiteral("original-baseline.json") &&
      QDir::fromNativeSeparators(comparisonCandidate->text()) == QDir::fromNativeSeparators(publishedCandidate),
      "explicit candidate adoption fills only the chosen input with a durable path after child-window deletion");

  measureImages->click();
  child = comparison.findChild<QualityMeasurementDialog *>();
  if (!check(!child.isNull(), "a new explicit measurement can be started without reopening the parent")) return false;
  child->setManifestPath(manifest);
  child->findChild<QPushButton *>(QStringLiteral("qualityMeasureStart"))->click();
  check(child->isRunning(), "the actual helper can be cancelled while starting asynchronously");
  child->findChild<QPushButton *>(QStringLiteral("qualityMeasureCancel"))->click();
  check(waitFor([&] { return !child->isRunning(); }) && child->measurementReport().isEmpty() &&
      comparisonCandidate->text() == QDir::toNativeSeparators(publishedCandidate) && digest(retainedPath) == retainedHash,
      "cancelling discards only the new temporary measurement and preserves prior adopted reports and inputs");
  child->findChild<QPushButton *>(QStringLiteral("qualityMeasureStart"))->click();
  comparison.close();
  check(waitFor([&] { return !comparison.isVisible() && (!child || !child->isRunning()); }),
      "closing the parent while measuring cancels its owned CPU helper before closing");
  check(digest(reference) == referenceHash && digest(render) == renderHash && digest(manifest) == manifestHash,
      "cancellation, language changes and parent close preserve every original fixture byte");

  const QString referenceFolder = QDir(fixture.path()).filePath(QStringLiteral("references"));
  const QString renderFolder = QDir(fixture.path()).filePath(QStringLiteral("renders"));
  QDir().mkpath(QDir(referenceFolder).filePath(QStringLiteral("nested")));
  QDir().mkpath(renderFolder);
  image.fill(QColor(127, 127, 127));
  image.save(QDir(referenceFolder).filePath(QStringLiteral("view.png")), "PNG");
  image.save(QDir(referenceFolder).filePath(QStringLiteral("nested/not-a-top-level-pair.png")), "PNG");
  image.fill(QColor(128, 128, 128));
  image.save(QDir(renderFolder).filePath(QStringLiteral("view.png")), "PNG");
  QualityMeasurementDialog folders(python, root, fixture.path(), QProcessEnvironment::systemEnvironment());
  folders.setPairedFolders(referenceFolder, renderFolder);
  folders.show();
  auto *pairs = folders.findChild<QTableWidget *>(QStringLiteral("qualityImagePairs"));
  auto *folderStart = folders.findChild<QPushButton *>(QStringLiteral("qualityMeasureStart"));
  auto *refreshPairs = folders.findChild<QPushButton *>(QStringLiteral("qualityRefreshPairs"));
  if (!check(pairs && folderStart && refreshPairs && folderStart->isEnabled() && pairs->rowCount() == 1,
      "folder mode exposes the actual exact-name pair list without recursively including nested images")) return false;
  pairs->selectRow(0);
  folderStart->click();
  check(waitFor([&] { return !folders.isRunning(); }) &&
      folders.measurementReport().value(QStringLiteral("evidence")).toObject()
          .value(QStringLiteral("views")).toArray().size() == 1,
      "folder selection generates an explicit manifest for the same real CPU measurement path");
  check(pairs->selectionModel()->isSelected(pairs->model()->index(0, 0)),
      "the selected image pair survives real helper progress and measurement completion");
  const auto folderReport = folders.measurementReport();
  auto *referenceField = folders.findChild<QLineEdit *>(QStringLiteral("qualityReferenceFolder"));
  for (const QString &language : AppLanguage::supported()) {
    AppLanguage::apply(language, false);
    check(referenceField && referenceField->text() == referenceFolder && pairs->rowCount() == 1 &&
        folders.measurementReport() == folderReport && pairs->selectionModel()->isSelected(pairs->model()->index(0, 0)),
        "live language changes preserve folder choices, the exact pairing list and measured values");
  }
  const QString maskFolder = QDir(fixture.path()).filePath(QStringLiteral("masks"));
  QDir().mkpath(maskFolder);
  folders.setPairedFolders(referenceFolder, renderFolder, maskFolder);
  check(!folderStart->isEnabled(), "choosing a mask folder requires a matching mask for every reference");
  QImage mask(16, 16, QImage::Format_Grayscale8);
  mask.fill(255);
  mask.save(QDir(maskFolder).filePath(QStringLiteral("view.png")), "PNG");
  folders.setPairedFolders(referenceFolder, renderFolder, maskFolder);
  folderStart->click();
  const bool maskCompleted = waitFor([&] { return !folders.isRunning(); });
  const auto maskViews = folders.measurementReport().value(QStringLiteral("evidence")).toObject()
      .value(QStringLiteral("views")).toArray();
  check(maskCompleted && maskViews.size() == 1 && maskViews.first().toObject()
          .value(QStringLiteral("valid_pixels")).toInt() == 256,
      "an explicitly selected exact-name grayscale mask reaches the real pixel measurement path");
  const QString missing = QDir(referenceFolder).filePath(QStringLiteral("missing.png"));
  image.save(missing, "PNG");
  folders.setPairedFolders(referenceFolder, renderFolder);
  check(!folderStart->isEnabled() && folders.measurementReport().isEmpty(),
      "an unmatched reference disables measurement rather than silently using a partial subset");
  folders.startMeasurement();
  check(!folders.isRunning() && folders.measurementReport().isEmpty(),
      "the public start seam also refuses incomplete folder pairing");
  QFile::remove(missing); // This test owns only this fresh fixture file.
  refreshPairs->click();
  check(folderStart->isEnabled() && pairs->rowCount() == 1,
      "explicit refresh recovers complete pairing after files are repaired without changing folder text");
  folders.close();
  std::fprintf(stderr, "Quality measurement UI smoke: %s\n", passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
