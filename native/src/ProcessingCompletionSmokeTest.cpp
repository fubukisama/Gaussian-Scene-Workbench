#include "ProcessingCompletionSmokeTest.h"
#include "AppLanguage.h"
#include "MainWindow.h"
#include "CameraTrajectory.h"
#include "SceneCoordinates.h"
#include "BackendLocator.h"
#include "ExistingTrainingDialog.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
bool runProcessingCompletionSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    if (!ok) {
      passed = false;
      std::fprintf(stderr, "Processing completion FAIL: %s\n", message);
      std::fflush(stderr);
      qCritical() << "Processing completion FAIL:" << message;
    }
    return ok;
  };
  const auto wait = [](const std::function<bool()> &predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
      QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
    return predicate();
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const QDir root(temporary.path());
  const QString helper = qEnvironmentVariable("GSW_PROCESS_OUTPUT_FIXTURE",
      QDir(QCoreApplication::applicationDirPath()).filePath("gsw_process_output_fixture.exe"));
  const QString realModel = qEnvironmentVariable("GSW_COMPLETION_MODEL");
  QByteArray realBytes;
  if (!realModel.isEmpty()) {
    QFile file(realModel);
    const auto metadata = WorkspaceDocument::inspectPly(realModel);
    if (!check(metadata.valid && metadata.looksLikeGaussianSplat() &&
        file.open(QIODevice::ReadOnly), "read optional real-result fixture without changing it")) return false;
    realBytes = file.readAll();
  }
  const auto write = [](const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  QTimer dialogs;
  QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      check(false, "unexpected result-validation dialog"); box->reject();
    }
  });
  dialogs.start(20);
  const QByteArray points("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n1 0 0\n0 1 0\n0 0 1\n");
  const QByteArray mesh("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nelement face 2\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n0 0 1\n3 0 1 2\n3 0 2 3\n");
  QByteArray gaussian("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nend_header\n0 0 0 0 0 0 1 -3 -3 -3 1 0 0 0\n1 0 0 0 0 0 1 -3 -3 -3 1 0 0 0\n0 1 0 0 0 0 1 -3 -3 -3 1 0 0 0\n0 0 1 0 0 0 1 -3 -3 -3 1 0 0 0\n");
  const QByteArray sourceCameraBytes("[{\"img_name\":\"fixture\",\"position\":[0,0,0],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]],\"width\":1920,\"height\":1080,\"fx\":1300,\"fy\":1300}]");
  const QString fixtureViewId = QStringLiteral("completion-fixture-view");
  const QJsonArray fixtureK{QJsonArray{1300, 7, 901}, QJsonArray{0, 1310, 503}, QJsonArray{0, 0, 1}};
  const QJsonArray fixtureCanonicalPose{QJsonArray{1, 0, 0, .25}, QJsonArray{0, 1, 0, -.125},
      QJsonArray{0, 0, 1, 1.5}, QJsonArray{0, 0, 0, 1}};
  // Independently worked normalization: 20 * (.25, -.125, 1.5) + (0, 0, -.5).
  const QJsonArray expectedTrainingPose{QJsonArray{1, 0, 0, 5}, QJsonArray{0, 1, 0, -2.5},
      QJsonArray{0, 0, 1, 29.5}, QJsonArray{0, 0, 0, 1}};
  const auto matchesFixtureCamera = [&](const CameraTrajectory &trajectory) {
    if (!trajectory.metadataVerified() || !trajectory.error().isEmpty() || trajectory.cameras().size() != 1)
      return false;
    const auto &camera = trajectory.cameras().constFirst();
    return trajectory.coordinateFrame() == QStringLiteral("training") &&
        trajectory.coordinateUnit() == QStringLiteral("training") &&
        camera.viewId == fixtureViewId && camera.split == QStringLiteral("train") &&
        camera.width == 1920 && camera.height == 1080 && camera.intrinsics == fixtureK &&
        camera.cameraToWorld == expectedTrainingPose;
  };
  for (const QString kind : {QStringLiteral("3dgs"), QStringLiteral("2dgs"),
      QStringLiteral("bounded_tsdf"), QStringLiteral("unbounded_tsdf"),
      QStringLiteral("sugar"), QStringLiteral("gs2mesh")}) {
    for (const bool locked : {false, true}) {
      const bool training = kind == "3dgs" || kind == "2dgs";
      const QString name = kind + (locked ? "-locked" : "-unlocked");
      const QString project = root.filePath(name + ".files");
      QString error;
      if (!check(QDir().mkpath(project) && window.mWorkspace.create(project, &error), "create isolated project")) return false;
      if (!check(window.mWorkspace.saveManifest(root.filePath(name + ".gsw"), &error) &&
          !window.mWorkspace.hasPendingDataMigration(), "save isolated project without migration")) return false;
      if (training) {
        // Reproduce the actual COLMAP -> training workflow, not just a worker
        // started in an empty workspace. Its cached sparse preview must never
        // take priority over the final Gaussian model after training finishes.
        const QString dataset = QDir(project).filePath("dataset");
        QImage photo(2, 2, QImage::Format_RGB32); photo.fill(Qt::white);
        if (!check(QDir().mkpath(QDir(dataset).filePath("images")) &&
            photo.save(QDir(dataset).filePath("images/fixture.png")) &&
            window.mWorkspace.setDatasetPath(dataset, &error), "associate reconstruction dataset")) return false;
        const QString sparse = QDir(dataset).filePath(".gsw-previews/sparse.ply");
        const QString release = QDir(dataset).filePath("release");
        check(write(sparse, points), "write independent sparse snapshot");
        window.mPendingReconstruction = MainWindow::PendingReconstruction{"reconstruction-completion", project, dataset};
        if (!check(window.mProcessSupervisor.start("reconstruction-completion", helper,
            {"completion-worker", sparse, release, "colmap_sparse"}, {}, {}, true), "start owned reconstruction worker")) return false;
        if (!check(wait([&] { return window.mViewport->scenePath() == sparse; }), "sparse snapshot loads")) return false;
        check(write(release, "done"), "release reconstruction worker");
        if (!check(wait([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingReconstruction; }), "reconstruction finishes")) return false;
        check(window.mViewport->visibleModelAvailable(), "completed sparse snapshot remains visible");
        check(window.mInspectAction->isEnabled() && !window.mSelectionToolbar->isHidden(), "completed sparse snapshot permits observation");
        check(!window.mRectangleAction->isEnabled(), "bounded sparse snapshot is not promoted to editable source");
      }
      const QString original = QDir(project).filePath("original.ply");
      if (!training || locked) {
        if (!check(write(original, points) && window.mWorkspace.setScenePath(original, &error), "associate original model")) return false;
        if (!check(wait([&] { return window.mViewport->hasEditableScene(); }), "original loads")) return false;
      }
      window.mViewport->setEditToolsLocked(locked);
      const QString output = QDir(project).filePath("output/result");
      const QString final = QDir(output).filePath(training ? "point_cloud/iteration_2/point_cloud.ply" : "mesh.ply");
      const QString preview = !training && !locked ? final : QDir(output).filePath("live.ply");
      QByteArray bytes = training ? gaussian : mesh;
      if (kind == "2dgs") { bytes.replace("property float scale_2\n", ""); bytes.replace("-3 -3 -3", "-3 -3"); }
      if (kind == "2dgs" && !realBytes.isEmpty()) bytes = realBytes;
      if (!check(write(final, bytes) && write(preview, bytes), "write independent preview/final fixtures")) return false;
      QByteArray trainingScaleBytes;
      if (training) {
        check(write(QDir(output).filePath("cameras.json"), sourceCameraBytes), "write source camera fixture");
        check(write(QDir(output).filePath("reconstruction_quality.json"),
            "{\"registeredImages\":9,\"inputImages\":9}"), "write quality report fixture");
        QJsonObject cameraContract{
            {QStringLiteral("schema"), QStringLiteral("gsw.training_camera_contract/1")},
            {QStringLiteral("backend"), kind},
            {QStringLiteral("input_data_identity"), QString(64, QLatin1Char('a'))},
            {QStringLiteral("pose_policy"), QStringLiteral("locked")},
            {QStringLiteral("views"), QJsonArray{QJsonObject{
                {QStringLiteral("view_id"), fixtureViewId}, {QStringLiteral("split"), QStringLiteral("train")},
                {QStringLiteral("image_path"), QStringLiteral("images/fixture.png")},
                {QStringLiteral("projection_model"), QStringLiteral("PINHOLE")},
                {QStringLiteral("pixel_coordinates"), QStringLiteral("pixel_border")},
                {QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080},
                {QStringLiteral("K"), fixtureK}, {QStringLiteral("c2w_opencv"), fixtureCanonicalPose}}}},
            {QStringLiteral("scale"), QJsonObject{
                {QStringLiteral("status"), QStringLiteral("source_declared")},
                {QStringLiteral("source_unit"), QStringLiteral("cm")},
                {QStringLiteral("meters_per_unit"), .01},
                {QStringLiteral("canonical_unit"), QStringLiteral("m")},
                {QStringLiteral("evidence"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixture_declaration")}}}}}},
            {QStringLiteral("transforms"), QJsonObject{
                {QStringLiteral("source_to_canonical"), QJsonObject{{QStringLiteral("s"), .01},
                    {QStringLiteral("R"), QJsonArray{QJsonArray{1, 0, 0}, QJsonArray{0, 1, 0}, QJsonArray{0, 0, 1}}},
                    {QStringLiteral("t"), QJsonArray{0, 0, 0}}, {QStringLiteral("applied"), true}}},
                {QStringLiteral("canonical_to_training"), QJsonObject{{QStringLiteral("s"), 20},
                    {QStringLiteral("R"), QJsonArray{QJsonArray{1, 0, 0}, QJsonArray{0, 1, 0}, QJsonArray{0, 0, 1}}},
                    {QStringLiteral("t"), QJsonArray{0, 0, -.5}}, {QStringLiteral("applied"), true}}}}}};
        check(write(QDir(output).filePath(QStringLiteral("training-camera-contract.json")),
                    QJsonDocument(cameraContract).toJson()), "write independent scale-contract fixture");
        cameraContract.insert(QStringLiteral("ply_sha256"),
            QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        trainingScaleBytes = QJsonDocument(cameraContract).toJson();
        const QFileInfo finalInfo(final);
        check(write(finalInfo.dir().filePath(finalInfo.completeBaseName() + QStringLiteral(".camera-scale.json")), trainingScaleBytes),
              "pair each generated Gaussian fixture with its own checksum-bound scale record");
        const auto admittedCoordinates = parsePlyCoordinateMetadata({}, final, false);
        if (!check(admittedCoordinates.validationError.isEmpty() &&
                   admittedCoordinates.dataIdentity == QString(64, QLatin1Char('a')) &&
                   admittedCoordinates.metersPerCoordinate.has_value() &&
                   std::abs(*admittedCoordinates.metersPerCoordinate - .05) < 1e-12,
                   "final fixture passes the actual scale and data-identity reader before publication")) return false;
        if (!check(matchesFixtureCamera(CameraTrajectory::loadForScene(final)),
                   "final fixture passes verified complete intrinsics and normalized camera pose before publication")) return false;
      }
      const QString task = kind + "-completion";
      GenerationExperiment experiment;
      experiment.id = QStringLiteral("history-") + name;
      experiment.pipeline = training ? QStringLiteral("training") : QStringLiteral("mesh");
      experiment.backend = kind;
      experiment.displayName = name;
      experiment.datasetPath = window.mWorkspace.datasetPath();
      experiment.configurationPath = QDir(project).filePath(QStringLiteral(".gsw/jobs/completion.json"));
      experiment.outputRoot = output;
      experiment.resultSceneId = training ? experiment.id : QString();
      experiment.status = QStringLiteral("queued");
      experiment.parameters = {{QStringLiteral("backend"), kind},
          {QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 2}}}};
      if (!check(write(experiment.configurationPath, QJsonDocument(experiment.parameters).toJson()) &&
          GenerationHistoryStore(project).upsert(experiment, &error), "retain independent generation parameters before launch")) return false;
      if (training) {
        window.mPendingTraining = MainWindow::PendingTraining{task, project, window.mWorkspace.datasetPath(), output, kind, 2, experiment.resultSceneId};
        window.mPendingTraining->historyId = experiment.id;
      }
      else {
        window.mPendingMesh = MainWindow::PendingMesh{task, project, output, {}};
        window.mPendingMesh->historyId = experiment.id;
        check(write(QDir(output).filePath("result.json"), QJsonDocument(QJsonObject{
          {"version", 1}, {"task", "mesh"}, {"state", "done"}, {"meshPath", final},
          {"completedStages", QJsonArray{QStringLiteral("mesh")}}}).toJson()), "write valid mesh journal");
      }
      // A real child process exercises status parsing, terminal events and the
      // controller's processing guard, including QProcess running-state timing.
      const QString release = QDir(output).filePath("release");
      if (!check(window.mProcessSupervisor.start(task, helper,
          {"completion-worker", preview, release, training ? "gaussian" : "mesh"}, {}, {}, true), "start owned worker")) return false;
      if (!check(wait([&] { return window.mViewport->scenePath() == preview; }), "live observation loads")) return false;
      check(GenerationHistoryStore(project).record(experiment.id).status == QStringLiteral("running"),
            "the real worker-start signal records this experiment as running");
      if (window.isVisible()) (void)window.mViewport->grabFramebuffer();
      check(!window.mViewport->hasEditableScene(), "live observation stays read-only");
      const auto target = window.mViewport->viewTarget();
      const auto distance = window.mViewport->viewDistance();
      const auto angles = window.mViewport->viewOrbitAngles();
      check(write(release, "done"), "release owned worker");
      if (!check(wait([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingTraining && !window.mPendingMesh; }), "worker finishes")) return false;
      if (!check(wait([&] { return window.mViewport->selectableModelAvailable() &&
          window.mViewport->scenePath() == window.mWorkspace.scenePath(); }), "completion hands final model to inspect tools")) return false;
      const auto archived = GenerationHistoryStore(project).record(experiment.id);
      check(archived.status == QStringLiteral("completed") &&
          archived.resultPath == window.mWorkspace.scenePath() &&
          archived.resultSceneId == window.mWorkspace.activeSceneId(),
          "validated completion retains the experiment's own result association and terminal state");
      check(archived.parameters.value(QStringLiteral("trainOptions")).toObject()
          .value(QStringLiteral("iterations")).toInt() == 2,
          "generation completion does not overwrite recorded settings");
      check(training ? window.mWorkspace.scenePath().contains("/.gsw/checkpoints/training/") :
          window.mWorkspace.scenePath() == final, "validated final is associated, not the previous model");
      check(!window.mSelectionToolbar->isHidden() && window.mInspectAction->isEnabled(), "inspect toolbar is restored");
      if (training) {
        check(wait([&] { return window.mViewport->cameraCount() == 1; }), "protected training result retains source cameras");
        const auto directory = QFileInfo(window.mViewport->scenePath()).absoluteDir();
        check(matchesFixtureCamera(CameraTrajectory::loadForScene(window.mWorkspace.scenePath())),
              "protected training result retains verified camera identity, complete intrinsics and the once-normalized pose");
        QFile protectedSourceCameras(directory.filePath(QStringLiteral("cameras.json")));
        check(protectedSourceCameras.open(QIODevice::ReadOnly) && protectedSourceCameras.readAll() == sourceCameraBytes,
              "protected raw training result preserves the exact legacy source cameras without using them in the bound frame");
        check(QFileInfo::exists(directory.filePath("reconstruction_quality.json")), "protected result retains quality report");
        const QFileInfo protectedInfo(window.mWorkspace.scenePath());
        QFile protectedScale(directory.filePath(protectedInfo.completeBaseName() + QStringLiteral(".camera-scale.json")));
        check(protectedScale.open(QIODevice::ReadOnly) && protectedScale.readAll() == trainingScaleBytes,
              "both training backends publish the exact matching scale contract beside the durable PLY");
      }
      check(window.mViewport->editToolsLocked() == locked, "explicit editing-lock preference survives completion");
      check(window.mRectangleAction->isEnabled() == (training && !locked), "trim availability follows final geometry and lock");
      check(window.mMoveModelAction->isEnabled() == !locked, "model transforms follow explicit lock");
      check((window.mViewport->viewTarget() - target).length() < 1e-4F &&
          std::abs(window.mViewport->viewDistance() - distance) < 1e-4F &&
          window.mViewport->viewOrbitAngles() == angles, "final handoff preserves the camera");
      if (training && !locked) {
        window.mRectangleAction->trigger();
        check(window.mRectangleAction->isChecked(), "can enter trim mode");
        window.mInvertSelectionAction->trigger();
        check(window.mSelectedPointCount == window.mWorkspace.sceneMetadata().vertexCount, "can select final model points");
        window.mDeleteSelectionAction->trigger();
        check(window.mDeletedPointCount == window.mWorkspace.sceneMetadata().vertexCount, "can trim final model");
        window.mUndoEditAction->trigger();
        check(window.mDeletedPointCount == 0, "can undo final-model trim");
      }
      window.mInspectAction->trigger();
      check(window.mInspectAction->isChecked(), "can return to inspect mode");
      int languageLoads = 0;
      const auto connection = QObject::connect(window.mViewport, &NativeViewport::sceneLoadStarted, &window,
          [&](const QString &) { ++languageLoads; });
      for (const auto &language : AppLanguage::supported()) {
        AppLanguage::apply(language, false);
        check(window.mInspectAction->text() == QCoreApplication::translate("Workbench", "查看"), "inspect label switches live");
        check(!window.mSelectionToolbar->isHidden() && window.mInspectAction->isEnabled(), "language switch preserves restored toolbar");
        const QString screenshots = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
        if (!screenshots.isEmpty() && kind == "2dgs" && !locked) {
          window.grab().save(QDir(screenshots).filePath("completion-" + language + ".png"));
        }
      }
      QObject::disconnect(connection);
      check(languageLoads == 0, "language switch does not reload final model");
      QFile source(final);
      check(source.open(QIODevice::ReadOnly) && source.readAll() == bytes, "generated source file is unchanged by trimming");
      if (training) {
        QFile sourceCameras(QDir(output).filePath(QStringLiteral("cameras.json")));
        check(sourceCameras.open(QIODevice::ReadOnly) && sourceCameras.readAll() == sourceCameraBytes,
              "generated source camera file is unchanged by publication and trimming");
        const QFileInfo finalInfo(final);
        QFile sourceScale(finalInfo.dir().filePath(finalInfo.completeBaseName() + QStringLiteral(".camera-scale.json")));
        check(sourceScale.open(QIODevice::ReadOnly) && sourceScale.readAll() == trainingScaleBytes,
              "generated source scale contract is unchanged by publication and trimming");
      }
      qInfo() << "Processing completion:" << kind << "locked" << locked << (passed ? "PASS" : "FAIL");
    }
  }
  // Distinct output parents may legitimately contain the same storage name.
  // Complete both through the real controller: a filename-only protected
  // checkpoint group would silently overwrite the first experiment's model.
  for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    const QString project = root.filePath(QStringLiteral("same-name-") + backend + QStringLiteral(".files"));
    QString error;
    if (!check(QDir().mkpath(project) && window.mWorkspace.create(project, &error) &&
        window.mWorkspace.saveManifest(root.filePath(QStringLiteral("same-name-") + backend + QStringLiteral(".gsw")), &error),
        "create same-basename experiment project")) return false;
    const QString dataset = QDir(project).filePath(QStringLiteral("dataset"));
    QImage photo(2, 2, QImage::Format_RGB32); photo.fill(Qt::white);
    if (!check(QDir().mkpath(QDir(dataset).filePath(QStringLiteral("images"))) &&
        photo.save(QDir(dataset).filePath(QStringLiteral("images/fixture.png"))) &&
        window.mWorkspace.setDatasetPath(dataset, &error), "retain the shared controlled dataset")) return false;
    const QString original = QDir(project).filePath(QStringLiteral("reference.ply"));
    if (!check(write(original, points) && window.mWorkspace.setScenePath(original, &error),
        "retain a reference object before independent experiment completion")) return false;
    const QString originalId = window.mWorkspace.activeSceneId();
    QByteArray firstBytes = gaussian;
    if (backend == QStringLiteral("2dgs")) {
      firstBytes.replace("property float scale_2\n", "");
      firstBytes.replace("-3 -3 -3", "-3 -3");
    }
    QByteArray secondBytes = firstBytes;
    secondBytes.replace("0 0 0 0 0 0 1", "0 0 0 0.5 0.25 0.75 1");
    const QByteArray firstCameras("[{\"img_name\":\"first-experiment\",\"position\":[0,0,0],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]],\"width\":1920,\"height\":1080,\"fx\":1300,\"fy\":1300}]");
    QByteArray secondCameras = firstCameras;
    secondCameras.replace("first-experiment", "second-experiment");
    QString firstPath;
    QString firstId;
    QString firstFinal;
    const QVector3D firstTranslation(1.0F, 2.0F, 3.0F);
    for (int number = 0; number < 2; ++number) {
      const QString output = QDir(project).filePath(
          QStringLiteral("outputs/%1/same-storage-name").arg(number == 0 ? QStringLiteral("first") : QStringLiteral("second")));
      const QString final = QDir(output).filePath(QStringLiteral("point_cloud/iteration_2/point_cloud.ply"));
      const QString preview = QDir(output).filePath(QStringLiteral("live.ply"));
      const QByteArray &bytes = number == 0 ? firstBytes : secondBytes;
      const QByteArray &cameras = number == 0 ? firstCameras : secondCameras;
      if (!check(write(final, bytes) && write(preview, bytes) &&
          write(QDir(output).filePath(QStringLiteral("cameras.json")), cameras),
          "write distinct same-basename experiment result sources")) return false;
      GenerationExperiment experiment;
      experiment.id = QStringLiteral("same-name-%1-%2").arg(backend).arg(number);
      experiment.pipeline = QStringLiteral("training");
      experiment.backend = backend;
      experiment.displayName = QStringLiteral("same-storage-name");
      experiment.datasetPath = dataset;
      experiment.configurationPath = QDir(project).filePath(
          QStringLiteral(".gsw/jobs/%1.json").arg(experiment.id));
      experiment.outputRoot = output;
      experiment.resultSceneId = experiment.id;
      experiment.status = QStringLiteral("queued");
      experiment.parameters = {{QStringLiteral("backend"), backend},
          {QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 2}}}};
      if (!check(write(experiment.configurationPath, QJsonDocument(experiment.parameters).toJson()) &&
          GenerationHistoryStore(project).upsert(experiment, &error),
          "archive independent experiments with identical storage basenames")) return false;
      const QString task = experiment.id + QStringLiteral("-completion");
      window.mPendingTraining = MainWindow::PendingTraining{
          task, project, dataset, output, backend, 2, experiment.resultSceneId};
      window.mPendingTraining->historyId = experiment.id;
      const QString release = QDir(output).filePath(QStringLiteral("release"));
      if (!check(window.mProcessSupervisor.start(task, helper,
          {"completion-worker", preview, release, "gaussian"}, {}, {}, true),
          "start same-basename owned completion worker")) return false;
      if (!check(wait([&] { return window.mViewport->scenePath() == preview; }),
          "same-basename completion preview becomes visible")) return false;
      check(write(release, "done"), "release same-basename completion worker");
      if (!check(wait([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingTraining; }),
          "same-basename completion reaches its terminal controller event")) return false;
      if (!check(wait([&] { return window.mViewport->scenePath() == window.mWorkspace.scenePath() &&
          window.mViewport->selectableModelAvailable(); }),
          "same-basename completion loads its protected result")) return false;
      const auto archived = GenerationHistoryStore(project).record(experiment.id);
      check(archived.status == QStringLiteral("completed") &&
          archived.resultSceneId == experiment.id && archived.resultPath == window.mWorkspace.scenePath(),
          "same-basename experiments retain separate completed archive identities");
      if (number == 0) {
        firstId = experiment.id;
        firstPath = archived.resultPath;
        firstFinal = final;
        check(window.mWorkspace.setSceneTranslation(firstTranslation, &error),
            "set first experiment object transform before the second completes");
      } else {
        check(archived.resultPath != firstPath,
            "different-parent same-basename experiments have independent protected result paths");
        const auto retained = GenerationHistoryStore(project).record(firstId);
        check(retained.resultSceneId == firstId && retained.resultPath == firstPath &&
            retained.parameters.value(QStringLiteral("trainOptions")).toObject()
                .value(QStringLiteral("iterations")).toInt() == 2,
            "second completion preserves the first archive's result identity, path and parameters");
        QFile firstProtected(firstPath);
        check(firstProtected.open(QIODevice::ReadOnly) && firstProtected.readAll() == firstBytes,
            "second completion does not overwrite the first protected Gaussian model bytes");
        QFile firstProtectedCameras(QFileInfo(firstPath).absoluteDir().filePath(QStringLiteral("cameras.json")));
        check(firstProtectedCameras.open(QIODevice::ReadOnly) && firstProtectedCameras.readAll() == firstCameras,
            "second completion does not overwrite the first protected camera source bytes");
        bool originalRetained = false;
        bool firstRetained = false;
        for (const auto &object : window.mWorkspace.sceneObjects()) {
          if (object.id == originalId) originalRetained = object.path == original;
          if (object.id == firstId) firstRetained = object.path == firstPath && object.translation == firstTranslation;
        }
        check(originalRetained && firstRetained && window.mWorkspace.sceneObjects().size() == 3,
            "second completion keeps reference and first-experiment object IDs, paths and transforms");
        QFile firstSource(firstFinal);
        QFile secondSource(final);
        check(firstSource.open(QIODevice::ReadOnly) && firstSource.readAll() == firstBytes &&
            secondSource.open(QIODevice::ReadOnly) && secondSource.readAll() == secondBytes,
            "both original experiment output sources remain byte-identical after completion");
      }
    }
  }
  // A valid PLY header is not sufficient for a camera-aware result. Exercise
  // the public terminal controller path, including the former legacy fallback
  // to the original output when protected publication fails.
  for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    for (const bool missingRecord : {false, true}) {
      const QString name = backend + (missingRecord ? QStringLiteral("-missing-scale") : QStringLiteral("-wrong-scale-hash"));
      const QString project = root.filePath(name + QStringLiteral(".files"));
      const QString original = QDir(project).filePath(QStringLiteral("reference.ply"));
      QString error;
      if (!check(QDir().mkpath(project) && window.mWorkspace.create(project, &error) &&
          window.mWorkspace.saveManifest(root.filePath(name + QStringLiteral(".gsw")), &error) &&
          write(original, points) && window.mWorkspace.setScenePath(original, &error),
          "retain an independent reference for rejected camera-scale output")) return false;
      const QString referenceId = window.mWorkspace.activeSceneId();
      const QString output = QDir(project).filePath(QStringLiteral("output/rejected"));
      const QString final = QDir(output).filePath(QStringLiteral("point_cloud/iteration_2/point_cloud.ply"));
      const QString preview = QDir(output).filePath(QStringLiteral("live.ply"));
      QByteArray bytes = gaussian;
      if (backend == QStringLiteral("2dgs")) {
        bytes.replace("property float scale_2\n", "");
        bytes.replace("-3 -3 -3", "-3 -3");
      }
      if (!check(write(final, bytes) && write(preview, bytes) &&
          write(QDir(output).filePath(QStringLiteral("training-camera-contract.json")), "{}"),
          "write a camera-aware training result with a deliberately invalid scale artifact")) return false;
      if (!missingRecord) {
        const QFileInfo info(final);
        check(write(info.dir().filePath(info.completeBaseName() + QStringLiteral(".camera-scale.json")),
            QJsonDocument(QJsonObject{{QStringLiteral("schema"), QStringLiteral("gsw.training_camera_contract/1")},
                {QStringLiteral("ply_sha256"), QString(64, QLatin1Char('0'))}}).toJson()),
            "write a mismatched scale checksum without changing PLY bytes");
      }
      GenerationExperiment experiment;
      experiment.id = name;
      experiment.pipeline = QStringLiteral("training");
      experiment.backend = backend;
      experiment.displayName = name;
      experiment.configurationPath = QDir(project).filePath(QStringLiteral(".gsw/jobs/rejected.json"));
      experiment.outputRoot = output;
      experiment.resultSceneId = name;
      experiment.status = QStringLiteral("queued");
      experiment.parameters = {{QStringLiteral("backend"), backend},
          {QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 2}}}};
      if (!check(write(experiment.configurationPath, QJsonDocument(experiment.parameters).toJson()) &&
          GenerationHistoryStore(project).upsert(experiment, &error), "retain the rejected experiment's own archive")) return false;
      const QString task = name + QStringLiteral("-completion");
      window.mPendingTraining = MainWindow::PendingTraining{task, project, {}, output, backend, 2, name};
      window.mPendingTraining->historyId = name;
      const QString release = QDir(output).filePath(QStringLiteral("release"));
      if (!check(window.mProcessSupervisor.start(task, helper,
          {"completion-worker", preview, release, "gaussian"}, {}, {}, true),
          "start a test-owned completion process for invalid scale metadata")) return false;
      if (!check(wait([&] { return window.mViewport->scenePath() == preview; }),
          "a preview can look valid while the final scale artifact remains invalid")) return false;
      check(write(release, "done"), "release the invalid-scale completion process");
      if (!check(wait([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingTraining; }),
          "invalid-scale completion reaches its public terminal event")) return false;
      const auto archived = GenerationHistoryStore(project).record(name);
      check(archived.status == QStringLiteral("failed") && archived.resultPath.isEmpty() &&
          window.mWorkspace.activeSceneId() == referenceId && window.mWorkspace.scenePath() == original &&
          window.mWorkspace.sceneObjects().size() == 1,
          "invalid or missing scale metadata cannot use original-file fallback or replace a reference object");
      QFile source(final);
      check(source.open(QIODevice::ReadOnly) && source.readAll() == bytes,
          "rejected camera-scale output remains available unchanged for investigation");
    }
  }
  if (!realModel.isEmpty()) {
    QFile original(realModel);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == realBytes, "original real-result fixture remains unchanged");
  }
#ifdef Q_OS_WIN
  // This block runs only under the existing real-window remote smoke mode.
  // Its detached child is a bounded CPU lease fixture, never a GS trainer.
  // No fake optimizer checkpoint is needed to verify ownership and migration.
  dialogs.stop();
  const QString repository = BackendLocator::findRepositoryRoot(
      QCoreApplication::applicationDirPath(), qEnvironmentVariable("GSW_BACKEND_ROOT"));
  const QString python = qEnvironmentVariable("GSW_RUNTIME_UI_TEST_PYTHON",
      qEnvironmentVariable("GSW_PREFLIGHT_TEST_PYTHON", QStandardPaths::findExecutable("python")));
  const QString cpuFixture = QDir(repository).filePath(QStringLiteral("native/tests/helpers/OwnedTrainingConnectionFixture.py"));
  if (!check(!repository.isEmpty() && QFileInfo(python).isFile() && QFileInfo(cpuFixture).isFile(),
      "resolve real CPU lifecycle fixture without a GPU runtime or hard-coded machine path")) return false;
  const bool previousPrefixPresent = qEnvironmentVariableIsSet("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  const QByteArray previousPrefix = qgetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  qputenv("GAUSSIAN_SPLATTING_CONDA_PREFIX", QFileInfo(python).absolutePath().toUtf8());
  const auto restorePrefix = qScopeGuard([&] {
    if (previousPrefixPresent) qputenv("GAUSSIAN_SPLATTING_CONDA_PREFIX", previousPrefix);
    else qunsetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  });
  const auto readBytes = [](const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
  };
  const auto readObject = [&](const QString &path) { return QJsonDocument::fromJson(readBytes(path)).object(); };
  for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    const QString lifecycle = root.filePath(QStringLiteral("persistent-") + backend);
    const QString recoveryBase = QDir(lifecycle).filePath(QStringLiteral("recovery"));
    const QString phase = QDir(lifecycle).filePath(QStringLiteral("phase"));
    if (!check(QDir().mkpath(recoveryBase), "create isolated persistent lifecycle recovery catalog")) return false;
    auto owner = std::make_unique<MainWindow>();
    std::unique_ptr<MainWindow> reopened;
    HANDLE child = nullptr;
    const auto cleanupOwned = qScopeGuard([&] {
      write(phase + QStringLiteral("-abort"), "abort");
      write(phase + QStringLiteral("-exit"), "exit");
      if (child) {
        if (WaitForSingleObject(child, 5000) != WAIT_OBJECT_0) {
          check(false, "owned CPU fixture failed its bounded graceful cleanup; no unrelated PID is targeted");
          TerminateProcess(child, 197);
          WaitForSingleObject(child, 2000);
        }
        CloseHandle(child);
      }
      reopened.reset();
      owner.reset();
    });
    QString error;
    if (!check(owner->discardCurrentRecovery(&error), "remove only the new smoke window's unused initial recovery")) return false;
    owner->mRecoveryStore = std::make_unique<RecoveryStore>(recoveryBase);
    if (!check(owner->beginUntitledProject(QStringLiteral("owned-persistent-") + backend, &error),
        "create owned untitled project for persistent training")) return false;
    const QString oldRoot = owner->mWorkspace.rootPath();
    const QString dataset = QDir(oldRoot).filePath(QStringLiteral("datasets/owned"));
    const QString photo = QDir(dataset).filePath(QStringLiteral("images/owned.png"));
    QImage fixturePhoto(3, 2, QImage::Format_RGB32); fixturePhoto.fill(Qt::white);
    const QString reference = QDir(oldRoot).filePath(QStringLiteral("reference.ply"));
    if (!check(QDir().mkpath(QFileInfo(photo).absolutePath()) && fixturePhoto.save(photo) &&
        owner->mWorkspace.setDatasetPath(dataset, &error) && write(reference, points) &&
        owner->mWorkspace.setScenePath(reference, &error) &&
        owner->mWorkspace.setSceneTranslation(QVector3D(1, 2, 3), &error),
        "retain independent photo, reference object and transform for close/reopen acceptance")) return false;
    const QString referenceId = owner->mWorkspace.activeSceneId();
    const QString output = QDir(oldRoot).filePath(QStringLiteral("output/owned"));
    const QString jobs = QDir(oldRoot).filePath(QStringLiteral(".gsw/jobs"));
    const QString store = QDir(jobs).filePath(QStringLiteral("training"));
    const QString configPath = QDir(jobs).filePath(QStringLiteral("owned-configuration.json"));
    const QString jobId = QUuid::createUuid().toString(QUuid::Id128);
    const QString jobPath = QDir(store).filePath(jobId + QStringLiteral(".json"));
    const QString manifestPath = QDir(lifecycle).filePath(QStringLiteral("saved.gsw.json"));
    const QJsonObject configuration{{"projectRoot", oldRoot}, {"backend", backend}, {"nativeCheckpoint", true},
        {"datasetPath", dataset}, {"outputRoot", QDir(oldRoot).filePath(QStringLiteral("output"))},
        {"outputScene", "owned"}, {"outputDisplayName", "owned-cpu-lifecycle"}, {"jobStore", store},
        {"trainOptions", QJsonObject{{"iterations", 10}}}};
    GenerationExperiment experiment;
    experiment.id = QStringLiteral("owned-persistent-") + backend;
    experiment.pipeline = QStringLiteral("training"); experiment.backend = backend;
    experiment.displayName = QStringLiteral("owned-cpu-lifecycle"); experiment.configurationPath = configPath;
    experiment.outputRoot = output; experiment.datasetPath = dataset; experiment.status = QStringLiteral("queued");
    experiment.resultSceneId = experiment.id; experiment.parameters = configuration;
    if (!check(write(configPath, QJsonDocument(configuration).toJson()) && QDir().mkpath(store) &&
        GenerationHistoryStore(oldRoot).upsert(experiment, &error) &&
        saveActiveTrainingJob(oldRoot, {configPath, output, false, experiment.id}, &error) &&
        owner->mWorkspace.saveManifest(manifestPath, &error) && owner->mWorkspace.hasPendingDataMigration(),
        "save a pending-migration project without moving its owned training inputs")) return false;
    const QByteArray originalConfig = readBytes(configPath), originalPhoto = readBytes(photo);
    const QString archivePath = QDir(oldRoot).filePath(QStringLiteral(".gsw/experiments/") + experiment.id + QStringLiteral(".json"));
    QByteArray originalArchive;
    // Inspect recovery through its public semantic identity, not a guessed
    // marker filename or a fabricated full-state checkpoint.
    const ActiveTrainingJob originalActive = loadActiveTrainingJob(oldRoot);
    const QString task = QStringLiteral("owned-persistent-") + backend;
    owner->mPendingTraining = MainWindow::PendingTraining{task, oldRoot, dataset, output, backend, 10, experiment.id};
    owner->mPendingTraining->historyId = experiment.id;
    QProcessEnvironment cpuEnvironment = QProcessEnvironment::systemEnvironment();
    cpuEnvironment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    cpuEnvironment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    if (!check(owner->mProcessSupervisor.startPersistentTraining(task, python,
        {QStringLiteral("-B"), cpuFixture, QStringLiteral("--mode"), QStringLiteral("mainwindow"),
         QStringLiteral("--store"), store, QStringLiteral("--job-id"), jobId,
         QStringLiteral("--backend"), backend, QStringLiteral("--output"), output,
         QStringLiteral("--configuration"), configPath, QStringLiteral("--phase-prefix"), phase},
        repository, cpuEnvironment, QDir(jobs).filePath(QStringLiteral("owned-cpu.log")), store) &&
        wait([&] { return QFileInfo::exists(phase + QStringLiteral("-ready.json")); }),
        "launch an actual bounded detached CPU owner before job registration")) return false;
    originalArchive = readBytes(archivePath); // taskStarted legitimately marks the archive running.
    const QJsonObject identity = readObject(phase + QStringLiteral("-ready.json")).value(QStringLiteral("workerIdentity")).toObject();
    child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
        static_cast<DWORD>(identity.value(QStringLiteral("pid")).toInteger()));
    FILETIME creation{}, exited{}, kernel{}, user{};
    const bool validIdentity = child && GetProcessTimes(child, &creation, &exited, &kernel, &user) &&
        identity.value(QStringLiteral("startToken")).toString() == QStringLiteral("win-filetime:") +
            QString::number((quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
    if (!validIdentity && child) { CloseHandle(child); child = nullptr; }
    if (!check(validIdentity && WaitForSingleObject(child, 0) == WAIT_TIMEOUT,
        "retain a query/synchronize handle for this fixture's verified OS creation identity")) return false;
    check(!QFileInfo::exists(jobPath) && ProcessSupervisor::hasUnfinishedTraining(store),
        "persistent ownership protects the workspace before any job JSON or lease exists");
    check(owner->saveProject(false) && owner->mWorkspace.rootPath() == oldRoot &&
        owner->mWorkspace.hasPendingDataMigration(), "save during startup records state without migrating live inputs");
    enum class ExpectedDialog { Exit, Reopen, History, None };
    ExpectedDialog expected = ExpectedDialog::Exit;
    bool exitSeen = false, pendingWarningSeen = false, observationSeen = false;
    QTimer lifecycleDialogs;
    QObject::connect(&lifecycleDialogs, &QTimer::timeout, &window, [&] {
      auto *modal = QApplication::activeModalWidget();
      if (!modal) return;
      if (expected == ExpectedDialog::History) {
        if (auto *observer = qobject_cast<ExistingTrainingDialog *>(modal)) {
          observationSeen = true; observer->close(); return;
        }
      }
      if (auto *box = qobject_cast<QMessageBox *>(modal)) {
        if (expected == ExpectedDialog::Exit && box->parentWidget() == owner.get()) {
          if (box->objectName() == QStringLiteral("exitConfirmationDialog")) {
            exitSeen = true;
            for (auto *button : box->buttons()) if (box->buttonRole(button) == QMessageBox::AcceptRole) { button->click(); return; }
          } else if (box->button(QMessageBox::Save)) { box->button(QMessageBox::Save)->click(); return; }
        }
        if (expected == ExpectedDialog::Reopen && box->windowTitle() ==
            QCoreApplication::translate("Workbench", "工程数据迁移待恢复")) {
          pendingWarningSeen = true; box->accept(); return;
        }
        check(false, "persistent lifecycle never silently accepts an unrelated modal error"); box->reject();
      }
    });
    lifecycleDialogs.start(10);
    owner->show();
    QPointer<MainWindow> destroyedOwner(owner.get());
    if (!check(owner->close() && exitSeen && !owner->isVisible(),
        "real MainWindow close accepts explicit exit without sending worker stop")) return false;
    const auto retainedRecovery = owner->mCurrentRecovery;
    owner.reset();
    check(destroyedOwner.isNull() && WaitForSingleObject(child, 0) == WAIT_TIMEOUT &&
        ProcessSupervisor::hasUnfinishedTraining(store) && QFileInfo(oldRoot).isDir(),
        "actual MainWindow destruction preserves the still-live CPU owner and recovery workspace");
    reopened = std::make_unique<MainWindow>();
    if (!check(reopened->discardCurrentRecovery(&error), "discard only the reopen window's unused fresh workspace")) return false;
    reopened->mRecoveryStore = std::make_unique<RecoveryStore>(recoveryBase);
    reopened->mCurrentRecovery = retainedRecovery;
    expected = ExpectedDialog::Reopen;
    check(reopened->openProjectFile(manifestPath) && pendingWarningSeen &&
        reopened->mWorkspace.rootPath() == oldRoot && reopened->mWorkspace.hasPendingDataMigration(),
        "reopening a live pending Save As preserves old inputs and refuses automatic migration");
    check(reopened->recoverInterruptedTraining(&error) &&
        loadActiveTrainingJob(oldRoot).configurationPath == originalActive.configurationPath &&
        loadActiveTrainingJob(oldRoot).outputSceneRoot == originalActive.outputSceneRoot &&
        readBytes(configPath) == originalConfig && readBytes(photo) == originalPhoto &&
        readBytes(archivePath) == originalArchive && reopened->mWorkspace.activeSceneId() == referenceId &&
        reopened->mWorkspace.sceneTranslation() == QVector3D(1, 2, 3),
        "reopen does not label live training interrupted, clear recovery or alter model/input identity");
    expected = ExpectedDialog::History;
    reopened->showGenerationHistory();
    check(observationSeen && readBytes(archivePath) == originalArchive,
        "history routes live ownership to observation without rewriting in-flight experiments");
    expected = ExpectedDialog::None;
    check(write(phase + QStringLiteral("-register"), "register") &&
        wait([&] { return QFileInfo::exists(phase + QStringLiteral("-registered.json")); }),
        "release registration only after real UI destruction and project reopen");
    QProcess stop;
    stop.setWorkingDirectory(repository); stop.setProcessEnvironment(cpuEnvironment);
    stop.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_NO_WINDOW; });
    stop.start(python, {QStringLiteral("-B"), QStringLiteral("-m"), QStringLiteral("native.worker.training_job_connection"),
        QStringLiteral("--job-store"), store, QStringLiteral("--job-id"), jobId,
        QStringLiteral("--command"), QStringLiteral("stop")});
    if (!check(stop.waitForStarted(3000) && stop.waitForFinished(3000) && stop.exitCode() == 0 &&
        wait([&] { return readObject(phase + QStringLiteral("-ack.json")).value(QStringLiteral("command")).toString() == QStringLiteral("stop"); }),
        "explicit session-bound stop is actually acknowledged by the owned CPU worker")) return false;
    check(WaitForSingleObject(child, 0) == WAIT_TIMEOUT && ProcessSupervisor::hasUnfinishedTraining(store) &&
        !reopened->finalizePendingProjectSave(&error) && reopened->mWorkspace.rootPath() == oldRoot,
        "terminal job metadata cannot permit migration while its owner process is still alive");
    check(write(phase + QStringLiteral("-exit"), "exit") && wait([&] { return WaitForSingleObject(child, 0) == WAIT_OBJECT_0; }),
        "actual owned process exit is verified rather than inferred from a command receipt or job label");
    DWORD exitCode = 1;
    check(GetExitCodeProcess(child, &exitCode) && exitCode == 0 && !ProcessSupervisor::hasUnfinishedTraining(store),
        "only proved owner exit releases conservative migration protection");
    check(reopened->finalizePendingProjectSave(&error) && !reopened->mWorkspace.hasPendingDataMigration() &&
        reopened->mWorkspace.rootPath() != oldRoot && !QFileInfo::exists(oldRoot),
        "pending project migration and recovery completion succeed after actual owner exit");
    const QString migratedRoot = reopened->mWorkspace.rootPath();
    check(readBytes(QDir(migratedRoot).filePath(QStringLiteral(".gsw/jobs/owned-configuration.json"))) == originalConfig &&
        readBytes(QDir(migratedRoot).filePath(QStringLiteral("datasets/owned/images/owned.png"))) == originalPhoto &&
        reopened->mWorkspace.activeSceneId() == referenceId && reopened->mWorkspace.sceneTranslation() == QVector3D(1, 2, 3),
        "finished migration preserves input bytes, model identity and transform for both native backends");
    lifecycleDialogs.stop();
  }
  dialogs.start(20);
  for(const auto &backend:{QStringLiteral("3dgs"),QStringLiteral("2dgs")}) {
    const QString name=QStringLiteral("nonzero-delivery-")+backend;
    const QString project=root.filePath(name+QStringLiteral(".files"));
    const QString output=QDir(project).filePath(QStringLiteral("output/owned"));
    const QString final=QDir(output).filePath(QStringLiteral("point_cloud/iteration_2/point_cloud.ply"));
    const QString configurationPath=QDir(project).filePath(QStringLiteral(".gsw/jobs/owned.json"));
    QString error;QByteArray bytes=gaussian;
    if(backend==QStringLiteral("2dgs")){bytes.replace("property float scale_2\n","");bytes.replace("-3 -3 -3","-3 -3");}
    if(!check(QDir().mkpath(project)&&window.mWorkspace.create(project,&error)&&
        window.mWorkspace.saveManifest(root.filePath(name+QStringLiteral(".gsw.json")),&error)&&write(final,bytes),
        "prepare independent format-valid output for nonzero export-exit semantics, not an optimizer checkpoint"))return false;
    GenerationExperiment experiment;experiment.id=name;experiment.pipeline=QStringLiteral("training");
    experiment.backend=backend;experiment.displayName=name;experiment.configurationPath=configurationPath;
    experiment.outputRoot=output;experiment.resultSceneId=name;experiment.status=QStringLiteral("queued");
    experiment.parameters={{"projectRoot",project},{"backend",backend},{"outputRoot",QDir(project).filePath(QStringLiteral("output"))},
        {"outputScene","owned"},{"trainOptions",QJsonObject{{"iterations",2}}}};
    if(!check(write(configurationPath,QJsonDocument(experiment.parameters).toJson())&&
        GenerationHistoryStore(project).upsert(experiment,&error)&&saveActiveTrainingJob(project,{configurationPath,output,false,name},&error),
        "record the owned optimizer/export state fixture without manufacturing serialized resume data"))return false;
    window.mPendingTraining=MainWindow::PendingTraining{name,project,{},output,backend,2,name};
    window.mPendingTraining->historyId=name;
    // Machine-facing CPU fixture code, not a translatable UI sentence.
    const QString script=QString::fromLatin1("import json,sys;print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'done','stage':'done','iteration':2,'totalIterations':2,'trainingDelivery':{'training_status':'done','export_status':'failed','reason':'owned_runtime_error','internal_model_path':sys.argv[1],'iteration':2}}),flush=True);sys.exit(1)");
    if(!check(window.mProcessSupervisor.start(name,python,{QStringLiteral("-B"),QStringLiteral("-c"),script,final},repository,{},false)&&
        wait([&]{return !window.mProcessSupervisor.isRunning()&&!window.mPendingTraining;}),
        "a real owned CPU process emits optimizer-done/export-failed and actually exits nonzero"))return false;
    const auto record=GenerationHistoryStore(project).record(name);
    const auto outcome=record.parameters.value(QStringLiteral("resultDeliveryOutcome")).toObject();
    check(record.status==QStringLiteral("completed")&&outcome.value(QStringLiteral("training_status"))==QStringLiteral("done")&&
        outcome.value(QStringLiteral("export_status"))==QStringLiteral("failed")&&
        outcome.value(QStringLiteral("reason"))==QStringLiteral("owned_runtime_error"),
        "nonzero export exit retains completed optimization and the independent failed-delivery history");
    check(!record.resultPath.isEmpty()&&readBytes(final)==bytes&&QFileInfo::exists(record.resultPath)&&
        !QFileInfo::exists(QDir(output).filePath(QStringLiteral(".gsw-resume/ready.json"))),
        "the original model and retained result survive without advertising a fabricated optimizer resume");
  }
  dialogs.stop();
#endif
  window.mViewport->setEditToolsLocked(false);
  qInfo() << "Processing completion desktop smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}
}
