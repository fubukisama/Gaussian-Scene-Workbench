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
#include <functional>

namespace gsw {
bool runProcessingCompletionSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    if (!ok) { passed = false; qCritical() << "Processing completion FAIL:" << message; }
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
      if (training) window.mPendingTraining = MainWindow::PendingTraining{task, project, {}, output, kind, 2};
      else {
        window.mPendingMesh = MainWindow::PendingMesh{task, project, output, {}};
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
      if (window.isVisible()) (void)window.mViewport->grabFramebuffer();
      check(!window.mViewport->hasEditableScene(), "live observation stays read-only");
      const auto target = window.mViewport->viewTarget();
      const auto distance = window.mViewport->viewDistance();
      const auto angles = window.mViewport->viewOrbitAngles();
      check(write(release, "done"), "release owned worker");
      if (!check(wait([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingTraining && !window.mPendingMesh; }), "worker finishes")) return false;
      if (!check(wait([&] { return window.mViewport->selectableModelAvailable() &&
          window.mViewport->scenePath() == window.mWorkspace.scenePath(); }), "completion hands final model to inspect tools")) return false;
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
  if (!realModel.isEmpty()) {
    QFile original(realModel);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == realBytes, "original real-result fixture remains unchanged");
  }
  window.mViewport->setEditToolsLocked(false);
  qInfo() << "Processing completion desktop smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}
}
