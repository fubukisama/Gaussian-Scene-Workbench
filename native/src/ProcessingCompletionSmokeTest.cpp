#include "ProcessingCompletionSmokeTest.h"
#include "AppLanguage.h"
#include "MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <cstdio>
#include <functional>

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
      if (training) {
        check(write(QDir(output).filePath("cameras.json"),
            "[{\"img_name\":\"fixture\",\"position\":[0,0,0],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]],\"width\":1920,\"height\":1080,\"fx\":1300,\"fy\":1300}]"), "write source camera fixture");
        check(write(QDir(output).filePath("reconstruction_quality.json"),
            "{\"registeredImages\":9,\"inputImages\":9}"), "write quality report fixture");
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
        check(QFileInfo::exists(directory.filePath("reconstruction_quality.json")), "protected result retains quality report");
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
  if (!realModel.isEmpty()) {
    QFile original(realModel);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == realBytes, "original real-result fixture remains unchanged");
  }
  window.mViewport->setEditToolsLocked(false);
  qInfo() << "Processing completion desktop smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}
}
