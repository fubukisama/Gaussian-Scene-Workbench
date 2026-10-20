#include "ScaleConstraintsSmokeTest.h"
#include "ScaleConstraintsDialog.h"
#include "AppLanguage.h"
#include "BackendLocator.h"
#include "CameraDatasetInfo.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QProcess>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <array>
#include <functional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
bool waitForScale(const std::function<bool()> &condition) {
  QElapsedTimer elapsed; elapsed.start();
  while (!condition() && elapsed.elapsed() < 30000) {
    QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
  }
  return condition();
}
QByteArray fileHash(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) : QByteArray();
}
}
bool runScaleConstraintsSmokeTest() {
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    std::fprintf(stderr, "Scale constraints %s: %s\n", ok ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    passed = passed && ok;
  };
  QTemporaryDir workspace;
  ScaleConstraintsDialog dialog({}, {}, workspace.path(), {}, QString(64, QLatin1Char('a')),
      QProcessEnvironment::systemEnvironment());
  auto *limit = dialog.findChild<QLineEdit *>(QStringLiteral("scaleFitTolerance"));
  auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("scaleConstraintMode"));
  auto *create = dialog.findChild<QPushButton *>(QStringLiteral("scaleCreateVersionButton"));
  check(limit && limit->text().isEmpty() && mode && create && !create->isEnabled(),
      "fit limit starts blank and no version can be created before a valid estimate");
  QJsonObject request{{"schema", "gsw.scale_constraints/1"},
      {"input_data_identity", QString(64, QLatin1Char('a'))}, {"input_frame", "source"},
      {"mode", "scale_only"}, {"fit_tolerance_m", 0.001},
      {"constraints", QJsonArray{QJsonObject{{"id", "distance-alpha"}, {"kind", "distance"},
          {"role", "fit"}, {"reference_id", "survey-1"}, {"reference_unit", "mm"},
          {"source_a", QJsonArray{1000000.125, -2.25, 3.125}}, {"source_b", QJsonArray{1000001.125, -2.25, 3.125}},
          {"reference_distance", 13.123456789012345}, {"weight", 1.25}, {"reference_sigma", 0.125},
          {"provenance", QJsonObject{{"source", "independent survey"}, {"measurement_id", "obs-1"},
              {"uncertainty_notes", "declared, not inferred"}}}, {"extra_source_record", "preserve me"}}}}};
  dialog.setConstraintDocument(request);
  check(dialog.constraintDocument() == request, "editable request round-trips float64 and original provenance without rounding");
  const QString language = AppLanguage::current();
  for (const QString &locale : AppLanguage::supported()) {
    AppLanguage::apply(locale, false);
    check(dialog.constraintDocument() == request && dialog.solutionReport().isEmpty(),
        "changing language retains exact constraint values and does not estimate");
    const QString expectedTitle = locale == QStringLiteral("en_US") ? QString::fromUtf8("Scale and Align")
        : locale == QStringLiteral("ja_JP") ? QString::fromUtf8("スケール設定と位置合わせ") : QString::fromUtf8("定尺度与对齐");
    check(dialog.windowTitle() == expectedTitle && mode->currentData() == QStringLiteral("scale_only"),
        "actual dialog title translates immediately while the raw mode is unchanged");
    const QString rawError = QString::fromUtf8("trajectory_unmatched_image: imageA.png");
    const QString guidance = CameraDatasetInfo::importErrorText(rawError);
    const QString expectedGuidance = locale == QStringLiteral("en_US") ? QString::fromUtf8("Images and poses")
        : locale == QStringLiteral("ja_JP") ? QString::fromUtf8("画像と姿勢") : QString::fromUtf8("图像与位姿");
    check(guidance.contains(expectedGuidance) && guidance.endsWith(rawError),
        "trajectory failures have localized guidance while raw machine diagnostics remain byte-for-byte unchanged");
  }
  AppLanguage::apply(language, false);
  const QString saved = workspace.filePath(QStringLiteral("constraints.json"));
  check(dialog.saveConstraints(saved), "constraints can be saved independently before solving");
  QFile input(saved);
  check(input.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(input.readAll()).object() == request,
      "saved request retains the complete original machine document");
  input.close();
  check(!dialog.saveConstraints(saved), "saving refuses to overwrite an existing original request");
  limit->setText(QStringLiteral("0.002"));
  check(dialog.constraintDocument().value(QStringLiteral("fit_tolerance_m")).toDouble() == .002 &&
      dialog.solutionReport().isEmpty() && !create->isEnabled(), "editing a tolerance invalidates prior evidence");
  for (const auto *name : {"scaleDistanceTable", "scaleBaselineTable", "scaleControlTable"}) {
    auto *table = dialog.findChild<QTableWidget *>(QString::fromLatin1(name));
    check(table && table->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff &&
        table->editTriggers() != QAbstractItemView::NoEditTriggers,
        "distance, baseline and control tables are editable and scrollable");
  }
  dialog.startEstimate();
  check(!dialog.isRunning() && dialog.solutionReport().isEmpty(),
      "missing helper fails closed without claiming calibration or training success");

  const QString root = BackendLocator::findRepositoryRoot(QCoreApplication::applicationDirPath(), qEnvironmentVariable("GSW_BACKEND_ROOT"));
  QString python = qEnvironmentVariable("GSW_SCALE_TEST_PYTHON");
  if (python.isEmpty()) python = qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");
  if (python.isEmpty()) python = BackendLocator::findGaussianPython(root);
  const QString source = workspace.filePath(QStringLiteral("source"));
  check(!root.isEmpty() && !python.isEmpty() && QDir().mkpath(source), "real source and CPU Python are available for helper acceptance");
  QJsonArray views;
  QStringList imagePaths;
  for (int index = 0; index < 4; ++index) {
    QImage image(20, 14, QImage::Format_RGB32); image.fill(QColor(30 + index, 60, 90));
    const QString name = QStringLiteral("view-%1.png").arg(index);
    const QString path = QDir(source).filePath(name); imagePaths.append(path);
    check(image.save(path, "PNG"), "write test-owned image without touching source photographs");
    views.append(QJsonObject{{"view_id", QStringLiteral("view-%1").arg(index)}, {"image_path", name},
        {"split", index < 2 ? "train" : index == 2 ? "val" : "test"}, {"pixel_coordinates", "pixel_border"},
        {"K", QJsonArray{QJsonArray{24., 0., 10.}, QJsonArray{0., 25., 7.}, QJsonArray{0., 0., 1.}}},
        {"c2w_opencv", QJsonArray{QJsonArray{1., 0., 0., double(index)}, QJsonArray{0., 1., 0., 0.},
            QJsonArray{0., 0., 1., -4.}, QJsonArray{0., 0., 0., 1.}}}});
  }
  const QString manifest = QDir(source).filePath(QStringLiteral("camera_dataset.json"));
  QFile raw(manifest);
  const QJsonObject rawDataset{{"schema", "gsw.camera_dataset/1"}, {"pose_policy", "locked"},
      {"source", QJsonObject{{"kind", "manual"}}}, {"scale", QJsonObject{{"status", "arbitrary"}}}, {"views", views},
      {"initialization", QJsonObject{{"kind", "random"}, {"seed", 3}, {"count", 16},
          {"bounds", QJsonArray{QJsonArray{-.5, -.5, -.5}, QJsonArray{.5, .5, .5}}}, {"frame", "source"}}}};
  check(raw.open(QIODevice::WriteOnly) && raw.write(QJsonDocument(rawDataset).toJson()) > 0, "write unscaled canonical fixture"); raw.close();
  QProcess normalize;
#ifdef Q_OS_WIN
  normalize.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW; args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW; args->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  normalize.setProgram(python);
  normalize.setArguments({QStringLiteral("-B"), QStringLiteral("-c"), QString::fromUtf8(
      "import sys;sys.path.insert(0,sys.argv[1]);from native.backend.camera_dataset import load_camera_dataset,write_camera_dataset;"
      "from pathlib import Path;d=load_camera_dataset(sys.argv[2]);write_camera_dataset(d,Path(sys.argv[2])/'camera_dataset.json');print(d['data_identity'])"), root, source});
  normalize.setProcessEnvironment(QProcessEnvironment::systemEnvironment()); normalize.setWorkingDirectory(root);
  normalize.start();
  check(normalize.waitForFinished(30000) && normalize.exitCode() == 0, "real adapter admits generated camera fixture");
  const QString identity = QString::fromUtf8(normalize.readAllStandardOutput()).trimmed();
  if (identity.size() != 64) { check(false, "canonical fixture has a stable identity"); return false; }
  QMap<QString, QByteArray> originals;
  for (const QString &path : imagePaths + QStringList{manifest}) originals[path] = fileHash(path);
  ScaleConstraintsDialog actual(python, root, workspace.path(), source, identity, QProcessEnvironment::systemEnvironment());
  actual.show(); // Only the remote test-owned smoke path exposes this window; never raises or activates it.
  auto fitRow = QJsonObject{{"id", "fit"}, {"kind", "distance"}, {"role", "fit"}, {"reference_id", "ruler-A"},
      {"reference_unit", "m"}, {"source_a", QJsonArray{0., 0., 0.}}, {"source_b", QJsonArray{2., 0., 0.}}, {"reference_distance", 6.},
      {"provenance", QJsonObject{{"source", "generated-fixture"}, {"measurement_id", "ruler-A"}}}};
  auto checkRow = QJsonObject{{"id", "check"}, {"kind", "distance"}, {"role", "check"}, {"reference_id", "ruler-B"},
      {"reference_unit", "m"}, {"source_a", QJsonArray{0., 0., 0.}}, {"source_b", QJsonArray{0., 1., 0.}}, {"reference_distance", 3.},
      {"independent_of_fit", true}, {"tolerance_m", 1e-8},
      {"provenance", QJsonObject{{"source", "separate-check-survey"}, {"measurement_id", "ruler-B"}}}};
  QJsonObject actualRequest{{"schema", "gsw.scale_constraints/1"}, {"input_data_identity", identity},
      {"input_frame", "source"}, {"mode", "scale_only"}, {"fit_tolerance_m", 1e-8}, {"constraints", QJsonArray{fitRow, checkRow}}};
  actual.setConstraintDocument(actualRequest); actual.startEstimate();
  const auto retainedRequest = actual.constraintDocument();
  for (const QString &locale : AppLanguage::supported()) AppLanguage::apply(locale, false);
  check(actual.constraintDocument() == retainedRequest, "language changes during the owned helper retain edited constraints");
  check(waitForScale([&] { return !actual.isRunning(); }) && actual.solutionReport().value("source_to_metric").toObject().value("s") == 3. &&
      actual.solutionReport().value("independent_checks").toObject().value("status") == "pass" &&
      actual.solutionReport().value("model_geometry_accuracy") == "not_measured", "real helper estimates scale and independently checks references without claiming model accuracy");
  const auto passingTransform = actual.solutionReport().value("source_to_metric");
  checkRow["reference_distance"] = 4.; actualRequest["constraints"] = QJsonArray{fitRow, checkRow}; actual.setConstraintDocument(actualRequest);
  actual.startEstimate();
  check(waitForScale([&] { return !actual.isRunning(); }) && actual.solutionReport().value("independent_checks").toObject().value("status") == "fail" &&
      actual.solutionReport().value("source_to_metric") == passingTransform &&
      !actual.findChild<QPushButton *>(QStringLiteral("scaleCreateVersionButton"))->isEnabled(),
      "failed independent check does not refit and blocks version creation");
  actual.createDatasetVersion(); check(!actual.isRunning() && actual.resultDatasetPath().isEmpty(), "failed-check version creation remains refused through the public method");
  actualRequest["constraints"] = QJsonArray{fitRow}; actual.setConstraintDocument(actualRequest); actual.startEstimate();
  check(waitForScale([&] { return !actual.isRunning(); }) && actual.solutionReport().value("independent_checks").toObject().value("status") == "not_provided",
      "no independent check remains explicitly unverified, not a model accuracy pass");
  actual.createDatasetVersion();
  check(waitForScale([&] { return !actual.isRunning(); }) && !actual.resultDatasetPath().isEmpty() &&
      actual.resultDatasetInfo().value("posePolicy") == "locked" && actual.resultDatasetInfo().value("scale").toObject().value("status") == "constraint_derived",
      "explicit adoption publishes a fresh locked dataset version with constraint-derived status");
  for (auto it = originals.begin(); it != originals.end(); ++it) check(fileHash(it.key()) == it.value(), "version creation leaves original images and manifest unchanged");
  actual.setConstraintDocument(actualRequest); actual.startEstimate(); actual.cancelOperation();
  check(waitForScale([&] { return !actual.isRunning(); }) && actual.solutionReport().isEmpty(), "cancel clears evidence and stops only this dialog-owned CPU helper");
  AppLanguage::apply(language, false); actual.close();

  const QString pointModel = workspace.filePath(QStringLiteral("points.ply"));
  const QByteArray pointBytes = "ply\nformat ascii 1.0\nelement vertex 4\nproperty double x\nproperty double y\nproperty double z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n0 0 0 255 0 0\n2 0 0 0 255 0\n0 1 0 0 0 255\n0 0 1 255 255 255\n";
  QFile points(pointModel);
  check(points.open(QIODevice::WriteOnly) && points.write(pointBytes) == pointBytes.size(), "write independent model-only point fixture"); points.close();
  ScaleConstraintsDialog model(python, root, workspace.path(), QJsonObject{{"kind", "model"}, {"path", pointModel}, {"frame", "source"}},
      QProcessEnvironment::systemEnvironment());
  check(!model.findChild<QPushButton *>(QStringLiteral("scaleEstimateButton"))->isEnabled(), "model estimates are disabled before asynchronous source inspection");
  QCoreApplication::processEvents();
  check(waitForScale([&] { return !model.isRunning(); }) && model.sourceDescriptor().value("inspected").toBool() &&
      model.sourceDescriptor().value("identity").toString().size() == 64 && model.solutionReport().isEmpty(),
      "actual hidden inspection obtains a semantic source identity without masquerading as a scale solution");
  auto modelRequest = actualRequest;
  modelRequest["input_data_identity"] = model.sourceDescriptor().value("identity");
  modelRequest["constraints"] = QJsonArray{fitRow}; model.setConstraintDocument(modelRequest);
  auto baseline = QJsonObject{{"id", "not-available"}, {"kind", "baseline"}, {"role", "fit"}, {"reference_id", "baseline-no-camera"},
      {"reference_unit", "m"}, {"view_a", "a"}, {"view_b", "b"}, {"reference_distance", 1.}};
  auto rejectedRequest = modelRequest; rejectedRequest["constraints"] = QJsonArray{fitRow, baseline}; model.setConstraintDocument(rejectedRequest);
  check(model.constraintDocument() == modelRequest, "model-only baseline import is refused without silently dropping rows or changing the current request");
  int modelCallbacks = 0;
  QObject::connect(&model, &ScaleConstraintsDialog::versionCreated, &model, [&](const QString &kind, const QString &path, const QJsonObject &info) {
    if (kind == QStringLiteral("model") && !path.isEmpty() && info.value("representation") == "baked_model" && !info.value("modelPath").toString().isEmpty()) ++modelCallbacks;
  });
  model.setConstraintDocument(modelRequest); model.startEstimate();
  check(waitForScale([&] { return !model.isRunning(); }) && model.solutionReport().value("source_to_metric").toObject().value("s") == 3.,
      "model-only distance constraint uses the real shared solver without images or camera solving");
  model.createDatasetVersion();
  check(waitForScale([&] { return !model.isRunning(); }) && modelCallbacks == 1 && model.resultDatasetPath().isEmpty() &&
      !model.resultVersionPath().isEmpty() && model.resultDatasetInfo().value("representation") == "baked_model" && fileHash(pointModel) ==
          QCryptographicHash::hash(pointBytes, QCryptographicHash::Sha256), "model-only scaling emits a typed baked-version result and preserves the original PLY");
  model.close();

  const QString shModel = workspace.filePath(QStringLiteral("directional-gaussians.ply"));
  QByteArray shBytes = "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nproperty float opacity\n";
  for (const char *name : {"scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3", "f_dc_0", "f_dc_1", "f_dc_2",
      "f_rest_0", "f_rest_1", "f_rest_2", "f_rest_3", "f_rest_4", "f_rest_5", "f_rest_6", "f_rest_7", "f_rest_8"})
    shBytes += "property float " + QByteArray(name) + '\n';
  shBytes += "end_header\n";
  for (const QByteArray &xyz : {QByteArray("0 0 0"), QByteArray("1 0 0"), QByteArray("0 1 0"), QByteArray("0 0 1")})
    shBytes += xyz + " 0 -2 -2 -2 1 0 0 0 0 0 0 .1 .2 .3 .4 .5 .6 .7 .8 .9\n";
  QFile directional(shModel); check(directional.open(QIODevice::WriteOnly) && directional.write(shBytes) == shBytes.size(), "write disposable directional-SH Gaussian fixture"); directional.close();
  ScaleConstraintsDialog sh(python, root, workspace.path(), QJsonObject{{"kind", "model"}, {"path", shModel}, {"frame", "source"}},
      QProcessEnvironment::systemEnvironment());
  QCoreApplication::processEvents(); check(waitForScale([&] { return !sh.isRunning(); }) && sh.sourceDescriptor().value("inspected").toBool(), "actual SH source is inspected before a full alignment");
  QJsonArray controls;
  const std::array<QJsonArray, 4> sourceControls{{QJsonArray{0.,0.,0.}, QJsonArray{1.,0.,0.}, QJsonArray{0.,1.,0.}, QJsonArray{0.,0.,1.}}};
  const std::array<QJsonArray, 4> referenceControls{{QJsonArray{10.,20.,30.}, QJsonArray{10.,22.,30.}, QJsonArray{8.,20.,30.}, QJsonArray{10.,20.,32.}}};
  for (int index = 0; index < 4; ++index) controls.append(QJsonObject{{"id", QStringLiteral("cp-%1").arg(index)}, {"kind", "control"},
      {"role", "fit"}, {"reference_id", QStringLiteral("survey-%1").arg(index)}, {"reference_unit", "m"},
      {"source_xyz", sourceControls[index]}, {"reference_xyz", referenceControls[index]}});
  QJsonObject shRequest{{"schema", "gsw.scale_constraints/1"}, {"input_data_identity", sh.sourceDescriptor().value("identity")},
      {"input_frame", "source"}, {"mode", "sim3"}, {"fit_tolerance_m", 1e-8}, {"constraints", controls}};
  sh.setConstraintDocument(shRequest); sh.startEstimate();
  check(waitForScale([&] { return !sh.isRunning(); }) && !sh.solutionReport().isEmpty(), "real full-Sim3 control-point fitting succeeds for model coordinates");
  int manifestCallbacks = 0;
  QObject::connect(&sh, &ScaleConstraintsDialog::versionCreated, &sh, [&](const QString &kind, const QString &path, const QJsonObject &info) {
    if (kind == QStringLiteral("model") && !path.isEmpty() && info.value("representation") == "transform_manifest" && info.value("modelPath").toString().isEmpty()) ++manifestCallbacks;
  });
  sh.createDatasetVersion();
  check(waitForScale([&] { return !sh.isRunning(); }) && manifestCallbacks == 1 && sh.resultDatasetInfo().value("representation") == "transform_manifest" &&
      sh.resultDatasetInfo().value("modelPath").toString().isEmpty() && fileHash(shModel) == QCryptographicHash::hash(shBytes, QCryptographicHash::Sha256),
      "unsupported directional-SH rotation creates only an explicit transform record, never a falsely baked model or source overwrite");
  sh.close();
  std::fprintf(stderr, "Scale constraint form smoke: %s\n", passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
