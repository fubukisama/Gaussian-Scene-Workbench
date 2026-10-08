#include "TrainingPreflightSmokeTest.h"
#include "TrainingDialog.h"
#include "AppLanguage.h"

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool waitFor(const std::function<bool()> &condition) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < 10000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return condition();
}
bool check(bool okay, const char *message) {
  std::fprintf(stderr, "Training preflight %s: %s\n", okay ? "PASS" : "FAIL", message);
  std::fflush(stderr);
  return okay;
}

bool writeJson(const QString &path, const QJsonObject &value) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(value).toJson()) > 0;
}

bool writeBinaryFixture(const QDir &dataset) {
  if (!dataset.mkpath(QStringLiteral("sparse/0")) ||
      !dataset.mkpath(QStringLiteral("images"))) return false;
  QImage photo(24, 16, QImage::Format_RGB32);
  photo.fill(Qt::gray);
  for (int index = 0; index < 6; ++index)
    if (!photo.save(dataset.filePath(QStringLiteral("images/frame%1.png").arg(index))))
      return false;
  const QDir sparse(dataset.filePath(QStringLiteral("sparse/0")));
  QFile cameras(sparse.filePath(QStringLiteral("cameras.bin")));
  if (!cameras.open(QIODevice::WriteOnly)) return false;
  QDataStream cameraStream(&cameras);
  cameraStream.setByteOrder(QDataStream::LittleEndian);
  cameraStream << quint64(1) << qint32(1) << qint32(1) << quint64(24) << quint64(16)
               << double(20) << double(20) << double(12) << double(8);
  cameras.close();
  QFile images(sparse.filePath(QStringLiteral("images.bin")));
  if (!images.open(QIODevice::WriteOnly)) return false;
  QDataStream imageStream(&images);
  imageStream.setByteOrder(QDataStream::LittleEndian);
  imageStream << quint64(3);
  for (int index = 0; index < 3; ++index) {
    imageStream << qint32(index + 1) << double(1) << double(0) << double(0) << double(0)
                << double(index * .1) << double(0) << double(0) << qint32(1);
    const QByteArray name = QStringLiteral("frame%1.png").arg(index).toUtf8() + '\0';
    imageStream.writeRawData(name.constData(), name.size());
    imageStream << quint64(0);
  }
  images.close();
  QFile points(sparse.filePath(QStringLiteral("points3D.bin")));
  if (!points.open(QIODevice::WriteOnly)) return false;
  QDataStream pointStream(&points);
  pointStream.setByteOrder(QDataStream::LittleEndian);
  pointStream << quint64(100);
  for (int index = 0; index < 100; ++index)
    pointStream << quint64(index + 1) << double(index * .01) << double(0) << double(1)
                << quint8(100) << quint8(120) << quint8(140) << double(.5) << quint64(2)
                << qint32(1) << qint32(index) << qint32(2) << qint32(index);
  return pointStream.status() == QDataStream::Ok;
}
}

bool runTrainingPreflightSmokeTest(const QString &python, const QString &backendRoot) {
  QTemporaryDir directory;
  if (!directory.isValid() || !writeBinaryFixture(QDir(directory.path()))) return false;
  TrainingDialog dialog(directory.path(), QStringLiteral("preflight-fixture"),
                        directory.path(), true, true);
  dialog.setReconstructionProbe(python, backendRoot, QProcessEnvironment::systemEnvironment());
  dialog.show();
  if (!check(waitFor([&] { return dialog.reconstructionPreflightReady(); }),
             "the real dialog finishes its read-only source probe")) return false;
  const auto *source = dialog.findChild<QLabel *>(QStringLiteral("trainingReconstructionSummaryLabel"));
  const bool okay = source && source->text().contains(QStringLiteral("images.bin")) &&
                    source->text().contains(QStringLiteral("cameras.bin"));
  if (!check(okay, "actual reconstruction files appear in the real dialog")) return false;
  const QRegularExpression statistics(QStringLiteral("3 / [^\\n]*6[^\\n]*1[^\\n]*100 / 100"));
  if (!check(statistics.match(source->text()).hasMatch(),
             "three registered photos and one calibration are not confused with six directory photos")) return false;
  if (!check(!dialog.effectiveRunColmap() &&
             dialog.reconstructionSummary().report.value(QStringLiteral("decision")).toString() == QStringLiteral("reuse"),
             "the dialog explains actual reuse rather than implicit reconstruction")) return false;
  auto *runColmap = dialog.findChild<QCheckBox *>(QStringLiteral("trainingRunColmapCheckBox"));
  auto *backend = dialog.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo"));
  auto *iterations = dialog.findChild<QSpinBox *>(QStringLiteral("trainingIterationsSpinBox"));
  auto *start = dialog.findChild<QPushButton *>(QStringLiteral("trainingStartButton"));
  if (!runColmap || !backend || !iterations || !start) return false;
  iterations->setValue(12000);
  runColmap->click();
  if (!check(!start->isEnabled() && waitFor([&] { return dialog.reconstructionPreflightReady(); }) &&
             dialog.effectiveRunColmap() &&
             dialog.reconstructionSummary().report.value(QStringLiteral("decision")).toString() == QStringLiteral("user_rerun"),
             "the user's rerun choice changes the source decision without starting training")) return false;
  runColmap->click();
  if (!check(waitFor([&] { return dialog.reconstructionPreflightReady(); }) && !dialog.effectiveRunColmap(),
             "the user can return to reuse without reconstructing or changing source files")) return false;
  const QString oldLanguage = AppLanguage::current();
  for (const QString &backendId : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    backend->setCurrentIndex(backend->findData(backendId));
    for (const QString &language : AppLanguage::supported()) {
      AppLanguage::apply(language, false);
      QApplication::processEvents();
      const QString decision = dialog.reconstructionDecisionText();
      const QString expected = language == QStringLiteral("zh_CN")
          ? QString::fromUtf8("复用当前重建") : language == QStringLiteral("en_US")
          ? QString::fromUtf8("Reuse the current reconstruction") : QString::fromUtf8("現在の再構築を再利用");
      if (!check(decision.startsWith(expected) && statistics.match(source->text()).hasMatch() &&
                 dialog.configuration().backend == backendId && dialog.configuration().iterations == 12000 &&
                 !dialog.configuration().runColmap && !dialog.effectiveRunColmap(),
                 "live language and backend edits preserve the actual decision and user parameters")) return false;
    }
  }
  AppLanguage::apply(oldLanguage, false);
  dialog.close();

  QTemporaryDir cacheDirectory;
  if (!cacheDirectory.isValid() || !writeBinaryFixture(QDir(cacheDirectory.path()))) return false;
  QDir cacheRoot(cacheDirectory.path());
  if (!cacheRoot.mkpath(QStringLiteral(".alignment_cache")) ||
      !cacheRoot.rename(QStringLiteral("sparse"), QStringLiteral(".alignment_cache/sparse"))) return false;
  TrainingDialog cached(cacheRoot.path(), QStringLiteral("cache-fixture"), cacheRoot.path(), false, true);
  cached.setReconstructionProbe(python, backendRoot, QProcessEnvironment::systemEnvironment());
  cached.show();
  if (!check(waitFor([&] { return cached.reconstructionPreflightReady(); }) &&
             cached.reconstructionSummary().report.value(QStringLiteral("decision")).toString() == QStringLiteral("reuse_cache") &&
             !cached.configuration().runColmap && !cached.effectiveRunColmap() &&
             !cacheRoot.exists(QStringLiteral("sparse")),
             "a validated cache defaults to reuse while preflight leaves the dataset untouched")) return false;
  cached.close();

  QTemporaryDir rawDirectory;
  if (!rawDirectory.isValid() || !writeBinaryFixture(QDir(rawDirectory.path()))) return false;
  QDir rawRoot(rawDirectory.path());
  if (!rawRoot.rename(QStringLiteral("images"), QStringLiteral("input"))) return false;
  TrainingDialog raw(rawRoot.path(), QStringLiteral("raw-fixture"), rawRoot.path(), true, true);
  raw.setReconstructionProbe(python, backendRoot, QProcessEnvironment::systemEnvironment());
  raw.show();
  if (!check(waitFor([&] { return raw.reconstructionPreflightReady(); }) &&
             raw.reconstructionSummary().report.value(QStringLiteral("decision")).toString() == QStringLiteral("undistortion_required") &&
             !raw.configuration().runColmap && raw.effectiveRunColmap(),
             "input-only conversion is explained even when the optional rerun checkbox is clear")) return false;
  raw.close();

  QTemporaryDir invalidDirectory;
  if (!invalidDirectory.isValid() || !writeBinaryFixture(QDir(invalidDirectory.path()))) return false;
  QFile invalidCameras(QDir(invalidDirectory.path()).filePath(QStringLiteral("sparse/0/cameras.bin")));
  if (!invalidCameras.open(QIODevice::WriteOnly) || invalidCameras.write("invalid") != 7) return false;
  invalidCameras.close();
  TrainingDialog invalid(invalidDirectory.path(), QStringLiteral("invalid-fixture"), invalidDirectory.path(), true, true);
  invalid.setReconstructionProbe(python, backendRoot, QProcessEnvironment::systemEnvironment());
  invalid.show();
  if (!check(waitFor([&] { return invalid.reconstructionPreflightReady(); }) &&
             invalid.reconstructionSummary().report.value(QStringLiteral("blocked")).toBool() &&
             !invalid.findChild<QPushButton *>(QStringLiteral("trainingStartButton"))->isEnabled(),
             "invalid calibration is never advertised as reusable or accepted for start")) return false;
  invalid.findChild<QCheckBox *>(QStringLiteral("trainingRunColmapCheckBox"))->click();
  if (!check(waitFor([&] { return invalid.reconstructionPreflightReady(); }) &&
             invalid.effectiveRunColmap() && invalid.findChild<QPushButton *>(QStringLiteral("trainingStartButton"))->isEnabled(),
             "explicit rerun can recover an unusable camera source without changing it in preflight")) return false;
  invalid.close();

  QTemporaryDir transformsDirectory;
  if (!transformsDirectory.isValid() || !writeBinaryFixture(QDir(transformsDirectory.path()))) return false;
  QDir transformsRoot(transformsDirectory.path());
  if (!transformsRoot.rename(QStringLiteral("sparse"), QStringLiteral("unused-sparse-fixture"))) return false;
  QJsonArray frames;
  for (int index = 0; index < 3; ++index)
    frames.append(QJsonObject{{QStringLiteral("file_path"), QStringLiteral("images/frame%1").arg(index)},
        {QStringLiteral("transform_matrix"), QJsonArray{
          QJsonArray{1, 0, 0, index * .1}, QJsonArray{0, 1, 0, 0},
          QJsonArray{0, 0, 1, 0}, QJsonArray{0, 0, 0, 1}}}});
  if (!writeJson(transformsRoot.filePath(QStringLiteral("transforms_train.json")),
                 {{QStringLiteral("camera_angle_x"), .8}, {QStringLiteral("frames"), frames}}) ||
      !writeJson(transformsRoot.filePath(QStringLiteral("transforms_test.json")),
                 {{QStringLiteral("camera_angle_x"), .8}, {QStringLiteral("frames"), QJsonArray{}}})) return false;
  TrainingDialog transforms(transformsRoot.path(), QStringLiteral("transforms-fixture"), transformsRoot.path(), true, true);
  transforms.setReconstructionProbe(python, backendRoot, QProcessEnvironment::systemEnvironment());
  transforms.show();
  if (!check(waitFor([&] { return transforms.reconstructionPreflightReady(); }) &&
             transforms.reconstructionSummary().report.value(QStringLiteral("decision")).toString() == QStringLiteral("reuse_transforms") &&
             transforms.reconstructionSummary().report.value(QStringLiteral("registeredImages")).isNull() &&
             transforms.findChild<QLabel *>(QStringLiteral("trainingReconstructionSummaryLabel"))->text().contains(QStringLiteral("transforms_train.json")) &&
             !transforms.effectiveRunColmap(),
             "transforms frames are identified explicitly rather than fabricated COLMAP registration")) return false;
  return true;
}
}
