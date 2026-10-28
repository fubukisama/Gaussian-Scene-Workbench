#include "ScaleConstraintsSmokeTest.h"
#include "ScaleConstraintsDialog.h"
#include "AppLanguage.h"
#include "BackendLocator.h"
#include "CameraDatasetInfo.h"
#include "ModelExportDialog.h"
#include "../tests/UiFormVisualProbe.h"

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
  const bool visualForms = !qEnvironmentVariable("GSW_UI_SURFACE_EVIDENCE").isEmpty();
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
  auto *distanceTable = dialog.findChild<QTableWidget *>(QStringLiteral("scaleDistanceTable"));
  auto *role = distanceTable && distanceTable->rowCount() > 0
      ? qobject_cast<QComboBox *>(distanceTable->cellWidget(0, 1)) : nullptr;
  auto *independence = distanceTable && distanceTable->rowCount() > 0
      ? qobject_cast<QComboBox *>(distanceTable->cellWidget(0, 9)) : nullptr;
  check(mode && mode->itemData(0).toString() == QStringLiteral("scale_only") &&
      mode->itemData(1).toString() == QStringLiteral("sim3") && mode->currentData().toString() == QStringLiteral("scale_only") &&
      role && role->itemData(0).toString() == QStringLiteral("fit") && role->itemData(1).toString() == QStringLiteral("check") &&
      role->currentData().toString() == QStringLiteral("fit") && independence && independence->itemData(0).isValid() &&
      independence->itemData(0).toString().isEmpty() && independence->itemData(1).toString() == QStringLiteral("false") &&
      independence->itemData(2).toString() == QStringLiteral("true"),
      "combo item userData retains machine modes, fit/check roles and explicit independence values instead of using the icon overload");
  check(dialog.constraintDocument() == request, "editable request round-trips float64 and original provenance without rounding");
  const QString language = AppLanguage::current();
  for (const QString &locale : AppLanguage::supported()) {
    AppLanguage::apply(locale, false);
    check(dialog.constraintDocument() == request && dialog.solutionReport().isEmpty(),
        "changing language retains exact constraint values and does not estimate");
    check(mode->currentData().toString() == QStringLiteral("scale_only") && role &&
        role->currentData().toString() == QStringLiteral("fit") && independence &&
        independence->currentData().toString().isEmpty() && mode->itemData(1).toString() == QStringLiteral("sim3") &&
        role->itemData(1).toString() == QStringLiteral("check") && independence->itemData(2).toString() == QStringLiteral("true"),
        "all three languages change combo captions without changing machine userData or selected policy values");
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
  actual.show(); // Ordinary smoke does not activate; the opt-in native matrix owns activation on CI.
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
  if (visualForms) {
    const QString resultManifest = QDir(actual.resultDatasetPath()).filePath(QStringLiteral("camera_dataset.json"));
    bool originalBytesReadable = !originals.isEmpty();
    for (auto it = originals.cbegin(); it != originals.cend(); ++it)
      originalBytesReadable = fileHash(it.key()).size() == 32 && originalBytesReadable;
    check(!actual.isRunning() && !actual.solutionReport().isEmpty() && originalBytesReadable &&
        fileHash(resultManifest).size() == 32,
        "native matrix begins with a real scale solution and readable source and adopted-manifest bytes");
    const auto snapshot = [&]() {
      QJsonObject inputHashes;
      for (auto it = originals.cbegin(); it != originals.cend(); ++it)
        inputHashes.insert(it.key(), QString::fromLatin1(fileHash(it.key()).toHex()));
      return QJsonObject{{"constraintDocument", actual.constraintDocument()},
          {"solutionReport", actual.solutionReport()}, {"sourceDescriptor", actual.sourceDescriptor()},
          {"resultDatasetPath", actual.resultDatasetPath()}, {"resultDatasetInfo", actual.resultDatasetInfo()},
          {"resultVersionPath", actual.resultVersionPath()}, {"isRunning", actual.isRunning()},
          {"inputSha256", inputHashes},
          {"resultManifestSha256", QString::fromLatin1(fileHash(resultManifest).toHex())}};
    };
    check(tests::captureUiFormMatrix(visualForms, actual, QStringLiteral("scale-constraints-adopted"),
        QSize(1100, 760), QSize(680, 620), snapshot),
        "native three-language light/dark wide/narrow evidence preserves exact constraints, real solution, adopted version and original bytes");
  }
  QProcess exportFixture;
#ifdef Q_OS_WIN
  exportFixture.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW; args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW; args->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  // This is the shared CPU preparation/export contract, not a claim that the
  // disposable Gaussian vertices were trained or that a fit measures accuracy.
  const QString exportFixtureScript = QString::fromUtf8(R"PY(import sys,json,hashlib,copy
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import numpy as np
from plyfile import PlyData,PlyElement
from native.worker.canonical_training import prepare_training_dataset,training_contract,transform_points
from native.backend.camera_scale_export import export_camera_scale_ply
prepared=prepare_training_dataset(sys.argv[2],resolution=1)
contract=training_contract(prepared)
assert contract['scale']['status']=='constraint_derived' and contract['scale']['meters_per_unit']==3.
source_xyz=np.asarray([[1.,2.,3.],[2.,3.,4.]])
canonical_xyz=transform_points(contract['transforms']['source_to_canonical'],source_xyz)
training_xyz=transform_points(contract['transforms']['canonical_to_training'],canonical_xyz)
paths=[]
for backend,dimensions in [('3dgs',3),('2dgs',2)]:
    path=Path(sys.argv[3])/(backend+'-export-training.ply')
    names=['x','y','z','opacity','f_dc_0','f_dc_1','f_dc_2']+['scale_'+str(i) for i in range(dimensions)]+['rot_'+str(i) for i in range(4)]
    vertices=np.zeros(2,dtype=[(name,'f8') for name in names])
    for i,name in enumerate(('x','y','z')): vertices[name]=training_xyz[:,i]
    for i in range(dimensions): vertices['scale_'+str(i)]=-2.
    vertices['rot_0']=1.
    PlyData([PlyElement.describe(vertices,'vertex')]).write(path)
    metadata=dict(copy.deepcopy(contract),backend=backend,ply_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    path.with_suffix('.camera-scale.json').write_text(json.dumps(metadata),encoding='utf-8')
    for mode,expected in [('source',source_xyz),('metric',canonical_xyz)]:
        output=path.with_name(backend+'-verified-'+mode+'.ply')
        export_camera_scale_ply(path,output,mode=mode)
        restored=PlyData.read(output,mmap=False)['vertex'].data
        np.testing.assert_allclose(np.column_stack([restored[name] for name in ('x','y','z')]),expected,rtol=0.,atol=1e-10)
    paths.append(str(path))
assert 'torch' not in sys.modules
print(json.dumps(paths))
)PY");
  exportFixture.setProgram(python);
  exportFixture.setArguments({QStringLiteral("-B"), QStringLiteral("-c"), exportFixtureScript,
      root, actual.resultDatasetPath(), workspace.path()});
  exportFixture.setProcessEnvironment(QProcessEnvironment::systemEnvironment()); exportFixture.setWorkingDirectory(root);
  exportFixture.start();
  const bool exportFixtureReady = exportFixture.waitForFinished(30000) && exportFixture.exitCode() == 0;
  check(exportFixtureReady, "shared CPU preparation and actual source/metric export validate both Gaussian contracts without Torch or training");
  if (!exportFixtureReady) std::fprintf(stderr, "%s\n", exportFixture.readAllStandardError().constData());
  const auto exportPaths = QJsonDocument::fromJson(exportFixture.readAllStandardOutput()).array();
  check(exportFixtureReady && exportPaths.size() == 2, "real helper publishes 3DGS and 2DGS scale-recovery fixtures");
  for (const auto &entry : exportPaths) {
    ModelExportOptions sourceOptions; sourceOptions.sourcePath = entry.toString();
    const auto capability = cameraScaleRecoveryCapability(sourceOptions.sourcePath);
    check(capability.source && capability.metric && capability.preserve, "actual scale sidecar enables source and metric recovery without fabricating a capability");
    const auto plyBefore = fileHash(sourceOptions.sourcePath), sidecarBefore = fileHash(cameraScaleMetadataPath(sourceOptions.sourcePath));
    ModelExportDialog exportDialog(sourceOptions, false, true, {});
    auto *coordinates = exportDialog.findChild<QComboBox *>(QStringLiteral("modelExportCoordinates"));
    for (const QString &recovery : {QStringLiteral("source"), QStringLiteral("metric")}) {
      const int index = coordinates ? coordinates->findData(recovery) : -1;
      check(index >= 2, "recovery choice carries its machine coordinate mode as userData");
      if (index < 2) continue;
      coordinates->setCurrentIndex(index);
      for (const QString &locale : AppLanguage::supported()) {
        AppLanguage::apply(locale, false);
        const auto options = exportDialog.options();
        check(coordinates->currentData().toString() == recovery && options.cameraCoordinateRecovery == recovery &&
            !options.applyTransform && options.format == ModelExportFormat::Ply,
            "live three-language captions preserve selected source/metric export policy through public options");
      }
    }
    check(fileHash(sourceOptions.sourcePath) == plyBefore && fileHash(cameraScaleMetadataPath(sourceOptions.sourcePath)) == sidecarBefore,
        "changing export policy or language leaves the verified source Gaussian and scale record unchanged");
  }
  AppLanguage::apply(language, false);
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
