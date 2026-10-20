#include "MeshGenerationDialog.h"
#include "CameraDatasetInfo.h"
#include "AppLanguage.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <cstdio>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// Presentation/dispatch contract only. The generated Gaussian fixture is not a
// trained model and this test does not establish GPU execution or accuracy.
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  if (app.arguments().size() != 3) return 2;
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    std::printf("Geometry dialog %s: %s\n", ok ? "PASS" : "FAIL", message); passed &= ok;
  };
  QTemporaryDir temporary(QDir(qEnvironmentVariable("GSW_INITIALIZATION_TEST_ROOT", QDir::tempPath()))
      .filePath(QStringLiteral("geometry-ui-XXXXXX")));
  if (!temporary.isValid()) return 2;
  // Use the existing CPU source-admission fixture: its full-K camera contract
  // comes from actual prepare_training_dataset, never an empty marker JSON.
  const QString script = QString::fromUtf8(R"PY(
import copy,json,shutil,sys
import numpy as np
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from native.backend.tests.test_gs_geometry import GsGeometrySourceTests
from native.backend import camera_dataset
from native.backend.gs_geometry import inspect_geometry_source
from native.worker.training_checkpoint import file_digest
from plyfile import PlyData,PlyElement
case=GsGeometrySourceTests();case.setUp()
try:
    root=Path(sys.argv[2])
    for backend in ('3dgs','2dgs'):
        out=root/backend
        shutil.copytree(case.root,out)
        model=out/'model';dataset=out/'dataset'
        contract=copy.deepcopy(case.contract);contract['backend']=backend
        ply=model/'point_cloud/iteration_7/point_cloud.ply'
        if backend=='2dgs':
            data=PlyData.read(str(ply),mmap=False)['vertex'].data
            names=[name for name in data.dtype.names if name!='scale_2']
            packed=np.empty(len(data),dtype=[(name,data.dtype[name]) for name in names])
            for name in names: packed[name]=data[name]
            PlyData([PlyElement.describe(packed,'vertex')]).write(str(ply))
        camera_dataset.write_camera_dataset(contract,model/'training-camera-contract.json')
        camera_dataset.write_camera_dataset(dict(contract,ply_sha256=file_digest(ply)),ply.with_suffix('.camera-scale.json'))
        camera_dataset.write_camera_dataset({'backend':backend},model/'training_backend.json')
        (model/'cfg_args').write_text("Namespace(sh_degree=0)\n",encoding='utf-8')
        observed=inspect_geometry_source(model,7,dataset)
        assert observed['backend']==backend
        old=model/'point_cloud/iteration_3';old.mkdir()
        shutil.copyfile(ply,old/'point_cloud.ply')
    print(json.dumps({'success':True,'cpuFixtureOnly':True}))
finally:
    case.doCleanups()
)PY");
  QProcess helper;
#ifdef Q_OS_WIN
  helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW; args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    args->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("GSW_GEOMETRY_TEST_ROOT"), temporary.path());
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  helper.setProcessEnvironment(environment);
  helper.start(app.arguments()[2], {QStringLiteral("-B"), QStringLiteral("-c"), script, app.arguments()[1], temporary.path()});
  const bool fixtureReady = helper.waitForFinished(30000) && helper.exitStatus() == QProcess::NormalExit &&
      helper.exitCode() == 0 && QJsonDocument::fromJson(helper.readAllStandardOutput()).object().value("success").toBool();
  check(fixtureReady, "actual CPU preparation and geometry-source admission generate both supported backend fixtures");
  if (!fixtureReady) { std::fprintf(stderr, "%s", helper.readAllStandardError().constData()); return 1; }
  const auto fileHash = [](const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) : QByteArray{};
  };
  for (const QString backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    const QDir root(temporary.filePath(backend));
    const QString source = root.filePath(QStringLiteral("model"));
    const QString dataset = root.filePath(QStringLiteral("dataset/camera_dataset.json"));
    const QString contractPath = QDir(source).filePath(QStringLiteral("training-camera-contract.json"));
    const QByteArray contractBefore = fileHash(contractPath), datasetBefore = fileHash(dataset);
    gsw::MeshGenerationDialog dialog(source);
    auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("meshMethodCombo"));
    auto *data = dialog.findChild<QLineEdit *>(QStringLiteral("geometryCameraDataset"));
    auto *coordinate = dialog.findChild<QComboBox *>(QStringLiteral("geometryCoordinateMode"));
    auto *alpha = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("geometryAlphaThreshold"));
    auto *stride = dialog.findChild<QSpinBox *>(QStringLiteral("geometryPixelStride"));
    auto *maximum = dialog.findChild<QSpinBox *>(QStringLiteral("geometryMaxPoints"));
    auto *memory = dialog.findChild<QSpinBox *>(QStringLiteral("geometryMemoryBudget"));
    const auto textures = dialog.findChildren<QCheckBox *>();
    check(mode && data && coordinate && alpha && stride && maximum && memory && textures.size() == 1,
        "native editable depth controls and texture control are reachable without a visible window");
    if (!mode || !data || !coordinate || !alpha || !stride || !maximum || !memory || textures.size() != 1) continue;
    const auto initial = dialog.configuration();
    check(mode->currentData() == QStringLiteral("gs_depth") && initial.value("iteration") == 7 &&
        data->text().isEmpty(), "a marked camera-contract source defaults to depth extraction at the selected latest iteration without guessing a dataset");
    check(initial.value("meshOptions").toObject().value("coordinate_mode") == QStringLiteral("training"),
        "the native comparison default uses the source Gaussian training frame instead of implying automatic canonical-to-training registration");
    dialog.setCameraDatasetPath(dataset);
    check(QDir::cleanPath(dialog.configuration().value("cameraDatasetPath").toString()) == QDir::cleanPath(dataset),
        "the caller-selected canonical dataset is kept explicitly in the worker configuration");
    alpha->setValue(.625); stride->setValue(3); maximum->setValue(123456); memory->setValue(96);
    coordinate->setCurrentIndex(coordinate->findData(QStringLiteral("source")));
    textures.first()->setChecked(true);
    const auto configuration = dialog.configuration();
    const auto options = configuration.value("meshOptions").toObject();
    check(options.value("alpha_threshold") == .625 && options.value("pixel_stride") == 3 &&
        options.value("max_points") == 123456 && options.value("memory_budget_mb") == 96 &&
        options.value("coordinate_mode") == QStringLiteral("source"),
        "depth alpha, sampling stride, point budget, memory and coordinate mode reach the public configuration unchanged");
    check(!textures.first()->isEnabled() && configuration.value("bakeTexture") == false &&
        !configuration.contains("textureResolution") && !options.contains("mesh_res") &&
        !options.contains("gs2mesh_downsample"), "disabled photo-texture and legacy meshing settings are not sent into depth extraction");
    for (const auto &locale : gsw::AppLanguage::supported()) {
      check(gsw::AppLanguage::apply(locale, false), "native language switches immediately");
      check(dialog.configuration() == configuration &&
          dialog.windowTitle() == QCoreApplication::translate("Workbench", "生成几何"),
          "each locale refreshes the dialog without resetting any depth setting or file identity");
    }
    for (const QString &frame : {QStringLiteral("canonical"), QStringLiteral("training"), QStringLiteral("source")}) {
      coordinate->setCurrentIndex(coordinate->findData(frame));
      check(dialog.configuration().value("meshOptions").toObject().value("coordinate_mode") == frame,
          "all three explicit coordinate frames preserve their machine-facing values");
    }
    for (const QString &legacy : {QStringLiteral("bounded"), QStringLiteral("unbounded"), QStringLiteral("sugar"), QStringLiteral("gs2mesh")}) {
      mode->setCurrentIndex(mode->findData(legacy));
      check(gsw::CameraDatasetInfo::meshGenerationBlocked(dialog.configuration()) &&
          !data->isEnabled() && !alpha->isEnabled() && !coordinate->isEnabled(),
          "choosing a legacy mode does not remove its known-camera compatibility gate or mix depth settings into it");
    }
    mode->setCurrentIndex(mode->findData(QStringLiteral("gs_depth")));
    check(!gsw::CameraDatasetInfo::meshGenerationBlocked(dialog.configuration()),
        "only the explicit depth mode reaches its own worker admission, not a legacy fallback");
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QPushButton *start = nullptr;
    if (buttons) for (auto *button : buttons->buttons()) {
      if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) start = qobject_cast<QPushButton *>(button);
    }
    if (start) start->click();
    check(start && dialog.result() == QDialog::Accepted && dialog.findChildren<QProcess *>().isEmpty(),
        "the actual explicit action accepts only a job configuration and never starts a training or geometry process in the dialog");
    check(contractBefore == fileHash(contractPath) && datasetBefore == fileHash(dataset) && !dialog.isVisible(),
        "settings, locale changes and acceptance preserve the owned source bytes without showing the dialog");
  }
  check(QApplication::platformName() == QStringLiteral("offscreen"), "no GSW application, visible window, OpenGL context or GPU job is used");
  return passed ? 0 : 1;
}
