#include "TrainingBatchLifecycleSmokeTest.h"
#include "MainWindow.h"
#include "ScaleConstraintsDialog.h"
#include "AppLanguage.h"
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool waitLifecycle(const std::function<bool()> &ready) {
  QElapsedTimer elapsed; elapsed.start();
  while (!ready() && elapsed.elapsed() < 15000) {
    QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
  }
  return ready();
}
bool writeLifecycle(const QString &path, const QByteArray &bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readLifecycle(const QString &path) {
  QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeLifecycleJson(const QString &path, const QJsonObject &object) {
  return writeLifecycle(path, QJsonDocument(object).toJson());
}
}

bool runTrainingBatchLifecycleSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    std::fprintf(stderr, "Batch lifecycle %s: %s\n", ok ? "PASS" : "FAIL", message);
    std::fflush(stderr); passed = passed && ok; return ok;
  };
  if (!check(!window.mProcessSupervisor.isRunning(), "owned acceptance window starts without a real user task")) return false;
  QTemporaryDir fixture;
  if (!check(fixture.isValid(), "isolated lifecycle fixture is available")) return false;
  QString python = qEnvironmentVariable("GSW_BATCH_TEST_PYTHON");
  if (python.isEmpty()) python = qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");
  if (!check(QFileInfo(python).isFile(), "CPU fixture Python is explicitly provided")) return false;
  const bool hadPythonPrefix = qEnvironmentVariableIsSet("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  const QByteArray oldPythonPrefix = qgetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  const auto restorePythonPrefix = qScopeGuard([&] {
    if (hadPythonPrefix) qputenv("GAUSSIAN_SPLATTING_CONDA_PREFIX", oldPythonPrefix);
    else qunsetenv("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  });
  // Process-local CPU helper selection only. No runtime installation, registry
  // edit, model training or modification of the user's configured environment.
  qputenv("GAUSSIAN_SPLATTING_CONDA_PREFIX", QFileInfo(python).absolutePath().toUtf8());
  const auto originalLanguage = AppLanguage::current();
  const QByteArray points("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n1 0 0\n0 1 0\n0 0 1\n");
  const QString worker = fixture.filePath(QStringLiteral("owned_lifecycle_worker.py"));
  // This external-process fixture exercises the native supervision boundary.
  // It is not a trainer and never claims to save optimizer state: pause exits
  // 76 at a between-run boundary, not checkpoint-bearing exit 75.
  const QByteArray workerBytes(
      "import json,sys,time\nfrom pathlib import Path\n"
      "state=Path(sys.argv[1]);expected=sys.argv[2];mode=sys.argv[3]\n"
      "def publish(status):\n"
      " state.write_text(json.dumps({'schema':'gsw.training_batch_state/1','plan_sha256':('0'*64 if mode.startswith('wrong-') else expected),'status':status,'runs':[]}),encoding='utf-8')\n"
      "publish('running')\n"
      "print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'running','stage':'batch_run','progressPercent':0}),flush=True)\n"
      "if mode.endswith('pause'):\n"
      " command=sys.stdin.readline().strip()\n"
      " if command!='pause': sys.exit(2)\n"
      " publish('interrupted');print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'interrupted','stage':'batch_run'}),flush=True);sys.exit(76)\n"
      "time.sleep(.2);publish('completed');print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'done','stage':'batch_run','progressPercent':100}),flush=True)\n");
  if (!check(writeLifecycle(worker, workerBytes), "write test-owned CPU process-boundary fixture")) return false;
  // A batch can deserialize an optimizer checkpoint. Declining the default-No
  // trust prompt must leave the supervisor idle and the persisted plan intact.
  const QString trustProject = fixture.filePath(QStringLiteral("trust-project"));
  QString trustError;
  check(QDir().mkpath(trustProject) && window.mWorkspace.create(trustProject, &trustError), "create trust-boundary fixture");
  const QString trustPlan = QDir(trustProject).filePath(QStringLiteral(".gsw/training-batches/trust-owned/plan.json"));
  check(writeLifecycleJson(trustPlan, QJsonObject{{"schema","gsw.training_batch/1"},
      {"plan_sha256",QString(64,QLatin1Char('b'))},{"runs",QJsonArray{}}}), "write owned trust-boundary plan");
  const auto trustBytes = readLifecycle(trustPlan);
  bool sawTrust = false, defaultNo = false;
  QTimer declineTrust;
  QObject::connect(&declineTrust, &QTimer::timeout, &window, [&] {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      sawTrust = true;
      defaultNo = box->defaultButton() && box->standardButton(box->defaultButton()) == QMessageBox::No;
      box->done(QMessageBox::No);
    }
  });
  declineTrust.start(10);
  window.startTrainingBatch(trustPlan, true);
  declineTrust.stop();
  check(sawTrust && defaultNo && !window.mProcessSupervisor.isRunning() && !window.mPendingTrainingBatch &&
      readLifecycle(trustPlan) == trustBytes && window.mWorkspace.workflowArtifacts().isEmpty(),
      "resume requires explicit trust with default No; declining never starts or registers a batch");
  QString lastProject;
  for (const bool locked : {false, true}) {
    for (const QString &mode : {QStringLiteral("complete"), QStringLiteral("pause"),
        QStringLiteral("wrong-complete"), QStringLiteral("wrong-pause")}) {
      const bool pause = mode.endsWith(QStringLiteral("pause"));
      const bool mismatch = mode.startsWith(QStringLiteral("wrong-"));
      const QString projectFile = fixture.filePath(mode +
          (locked ? QStringLiteral("-locked.gsw") : QStringLiteral("-unlocked.gsw")));
      // Use the same managed data root as a fully saved project. Saving an
      // arbitrary working root here would schedule a Save-As migration at the
      // next task boundary and legitimately remap every reference path.
      const QString project = WorkspaceDocument::projectDataRootForFile(projectFile);
      QString error;
      if (!check(QDir().mkpath(project) && window.mWorkspace.create(project, &error), "create independent lifecycle project")) return false;
      if (!check(window.mWorkspace.saveManifest(projectFile, &error) &&
          !window.mWorkspace.hasPendingDataMigration() && window.mWorkspace.rootPath() == project,
          "save lifecycle project with a stable managed data root before starting a process")) return false;
      const QString first = QDir(project).filePath(QStringLiteral("first.ply"));
      const QString second = QDir(project).filePath(QStringLiteral("second.ply"));
      if (!check(writeLifecycle(first, points) && writeLifecycle(second, points) && window.mWorkspace.setScenePath(first, &error) &&
          window.mWorkspace.setSceneTransform({1.25F,-2.5F,.75F}, QQuaternion::fromAxisAndAngle(0,0,1,25), {1.5F,2.F,.5F}, &error) &&
          window.mWorkspace.addScenePath(second, &error, false), "retain two reference objects and a nonidentity transform")) return false;
      window.mViewport->setSceneObjects(window.mWorkspace.sceneObjects(), window.mWorkspace.activeSceneId());
      if (!check(waitLifecycle([&] { return window.mViewport->hasEditableScene(); }), "real reference viewport data is ready")) return false;
      window.mViewport->setEditToolsLocked(locked);
      const auto collection = window.mWorkspace.sceneCollectionJson();
      const QString originalScene = window.mWorkspace.scenePath();
      const QString store = QDir(project).filePath(QStringLiteral(".gsw/jobs/training"));
      const QString session = QStringLiteral("abcdef0123456789abcdef0123456789");
      const QString jobId = QStringLiteral("0123456789abcdef0123456789abcdef");
      const QString historicalOutput = QDir(project).filePath(QStringLiteral("output/historical-owned"));
      const QString historicalConfig = QDir(project).filePath(QStringLiteral(".gsw/jobs/historical-owned.json"));
      const QString historicalJob = QDir(store).filePath(jobId + QStringLiteral(".json"));
      const QString pauseRequest = QDir(store).filePath(QStringLiteral("pause-%1.json").arg(session));
      QDir().mkpath(historicalOutput); QDir().mkpath(store); QDir().mkpath(QDir(project).filePath(QStringLiteral("dataset")));
      const QJsonObject configuration{{"projectRoot",project},{"backend","3dgs"},{"nativeCheckpoint",true},
          {"outputRoot",QDir(project).filePath(QStringLiteral("output"))},{"outputScene","historical-owned"},
          {"datasetPath",QDir(project).filePath(QStringLiteral("dataset"))},{"jobStore",store}};
      const QJsonObject oldJob{{"id",jobId},{"status","running"},{"stage","train"},{"backend","3dgs"},
          {"output_scene","historical-owned"},{"dataset_path",QDir(project).filePath(QStringLiteral("dataset"))},
          {"native_control",QJsonObject{{"output",historicalOutput},{"request",pauseRequest},{"session",session},{"backend","3dgs"}}}};
      if (!check(writeLifecycleJson(historicalConfig, configuration) && writeLifecycleJson(historicalJob, oldJob) &&
          saveActiveTrainingJob(project, {historicalConfig,historicalOutput}, &error), "create a fully addressable historical pause-control sentinel, not a checkpoint")) return false;
      const QString historicalMarker = QDir(project).filePath(QStringLiteral(".gsw/jobs/active-training.json"));
      const auto markerBefore = readLifecycle(historicalMarker), configBefore = readLifecycle(historicalConfig), jobBefore = readLifecycle(historicalJob);
      const QString planDirectory = QDir(project).filePath(QStringLiteral(".gsw/training-batches/owned-lifecycle"));
      const QString plan = QDir(planDirectory).filePath(QStringLiteral("plan.json"));
      const QString state = QDir(planDirectory).filePath(QStringLiteral("state.json"));
      const QString planSha(64, QLatin1Char('a'));
      check(writeLifecycleJson(plan, QJsonObject{{"schema","gsw.training_batch/1"},{"plan_sha256",planSha},{"runs",QJsonArray{}}}), "write explicit native boundary plan identity");
      const QString task = QStringLiteral("owned-batch-") + mode;
      window.mPendingTrainingBatch = MainWindow::PendingTrainingBatch{task,project,plan,planSha};
      window.mPauseRequested = false;
      if (!check(window.mProcessSupervisor.start(task, python, {QStringLiteral("-B"),worker,state,planSha,mode}, project,
          QProcessEnvironment::systemEnvironment(), true), "start only the owned CPU stub through the real supervisor")) return false;
      if (!check(waitLifecycle([&] { return window.mProcessSupervisor.isRunning() && window.mPauseTrainingAction->isEnabled(); }), "real pause action is enabled for an active batch")) return false;
      if (pause) {
        for (const auto &language : AppLanguage::supported()) AppLanguage::apply(language, false);
        window.mPauseTrainingAction->trigger();
      }
      if (!check(waitLifecycle([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingTrainingBatch; }), "owned process reaches its terminal native lifecycle")) return false;
      const int row = window.mTaskTable->rowCount() - 1;
      const QString taskState = row >= 0 ? window.mTaskTable->item(row,0)->data(Qt::UserRole+31).toString() : QString();
      check(taskState == (mismatch ? QStringLiteral("failed") : pause ? QStringLiteral("paused") : QStringLiteral("done")),
          "terminal state is bound to the launched plan identity, not just an exit code and a status word");
      check(readLifecycle(historicalMarker)==markerBefore && readLifecycle(historicalConfig)==configBefore &&
          readLifecycle(historicalJob)==jobBefore && !QFileInfo::exists(pauseRequest), "batch pause never touches the historical active-training marker, job, configuration or pause path");
      check(window.mWorkspace.sceneCollectionJson()==collection,
          "completion, interruption and invalid state retain every reference object and transform");
      check(window.mWorkspace.scenePath()==originalScene,
          "completion, interruption and invalid state retain the active workspace source path");
      check(window.mViewport->scenePath()==originalScene,
          "completion, interruption and invalid state restore the reference viewport source path");
      check(window.mViewport->editToolsLocked()==locked,
          "completion, interruption and invalid state retain the editing lock");
      check(!window.mPauseTrainingAction->isEnabled(), "pause is no longer enabled after the owned batch stops");
      lastProject = project;
    }
  }
  // The callback is exercised as a UI integration seam; this directory is a
  // durable observation record, not a forged solver result or training model.
  QTimer expectedDialogs;
  int guards = 0;
  QObject::connect(&expectedDialogs,&QTimer::timeout,&window,[&] {
    if(auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { ++guards; box->accept(); }
  });
  expectedDialogs.start(10);
  window.showScaleConstraints(true);
  if (!check(bool(window.mScaleConstraintsDialog), "explicit model-scale action creates its real callback owner")) return false;
  const auto beforeGuard = window.mWorkspace.sceneCollectionJson();
  const QString busyVersion = QDir(lastProject).filePath(QStringLiteral(".gsw/scale/owned-busy-observation"));
  check(writeLifecycleJson(QDir(busyVersion).filePath(QStringLiteral("observation.json")),QJsonObject{{"test_scope","native_busy_guard_durability_only"}}), "write an independent busy-guard observation record");
  const QString guardPlan = QDir(lastProject).filePath(QStringLiteral(".gsw/training-batches/owned-lifecycle/plan.json"));
  const QString guardState = QDir(QFileInfo(guardPlan).absolutePath()).filePath(QStringLiteral("state.json"));
  const QString guardSha(64, QLatin1Char('a'));
  window.mPendingTrainingBatch = MainWindow::PendingTrainingBatch{QStringLiteral("owned-busy-guard"),lastProject,guardPlan,guardSha};
  check(window.mProcessSupervisor.start(QStringLiteral("owned-busy-guard"),python,
      {QStringLiteral("-B"),worker,guardState,guardSha,QStringLiteral("pause")},lastProject,QProcessEnvironment::systemEnvironment(),true),
      "start an owned CPU boundary worker for the busy-adoption guard");
  check(waitLifecycle([&]{return window.mProcessSupervisor.isRunning()&&window.mPauseTrainingAction->isEnabled();}),"busy guard observes an actual supervised process");
  window.mScaleConstraintsDialog->versionCreated(QStringLiteral("model"),busyVersion,
      QJsonObject{{"representation","transform_manifest"},{"modelPath",""}});
  WorkspaceDocument busyReopened;QString busyError;
  check(guards==1&&busyReopened.load(window.mWorkspace.projectFilePath(),&busyError)&&!busyReopened.workflowArtifacts().isEmpty()&&
      window.mWorkspace.sceneCollectionJson()==beforeGuard,"a busy-adoption refusal persists the version while leaving all scene objects unchanged");
  window.mPauseTrainingAction->trigger();
  check(waitLifecycle([&]{return !window.mProcessSupervisor.isRunning()&&!window.mPendingTrainingBatch;}),"owned busy-guard worker stops via the real pause action");
  const QString guardedVersion = QDir(lastProject).filePath(QStringLiteral(".gsw/scale/owned-durable-observation"));
  check(writeLifecycleJson(QDir(guardedVersion).filePath(QStringLiteral("observation.json")),QJsonObject{{"test_scope","native_callback_durability_only"}}), "write owned artifact-registration observation record");
  const auto objects = window.mWorkspace.sceneObjects();
  if (objects.size()>1) window.mWorkspace.activateSceneObject(objects.last().id);
  window.mScaleConstraintsDialog->versionCreated(QStringLiteral("model"),guardedVersion,
      QJsonObject{{"representation","transform_manifest"},{"modelPath",""}});
  WorkspaceDocument reopened;QString reopenError;
  bool guardedReference = false;
  const bool loaded = reopened.load(window.mWorkspace.projectFilePath(),&reopenError);
  if (loaded) for(const auto &value:reopened.workflowArtifacts()) {
    const auto record = value.toObject();
    if(record.value(QStringLiteral("kind"))==QStringLiteral("scale_version") &&
        record.value(QStringLiteral("path")).toString()==QDir(lastProject).relativeFilePath(guardedVersion)) guardedReference=true;
  }
  check(guards==2 && loaded && guardedReference,
      "a guarded source change persists its version reference before refusing adoption");
  check(window.mWorkspace.sceneObjects().size()==beforeGuard.value(QStringLiteral("objects")).toArray().size(), "guarded transform-only record never adds a pretend baked scene model");
  window.mScaleConstraintsDialog->close();
  check(waitLifecycle([&]{return !window.mScaleConstraintsDialog;}), "closing the owned scale dialog cleans up only its own CPU helper");
  // Preserve the same crop/export guard used by ordinary append-import.
  // A new scaled object must not hide transient edits on its source object.
  window.mViewport->setEditToolsLocked(false);
  window.mViewport->clearSelection();
  window.mViewport->invertSelection();
  window.mViewport->deleteSelection();
  check(window.mViewport->hasUnsavedSceneEdits(), "create a transient crop using public selection/edit operations");
  const QString editedId = window.mWorkspace.activeSceneId();
  const QString editedPath = window.mWorkspace.scenePath();
  window.showScaleConstraints(true);
  if (check(bool(window.mScaleConstraintsDialog), "open explicit source-scale workflow while source crop is unsaved")) {
    const QString cropVersion = QDir(lastProject).filePath(QStringLiteral(".gsw/scale/owned-crop-guard"));
    const QString cropModel = QDir(cropVersion).filePath(QStringLiteral("scaled.ply"));
    check(writeLifecycle(cropModel, points), "write an owned model-adoption boundary fixture, not scale accuracy evidence");
    const auto count = window.mWorkspace.sceneObjects().size();
    window.mScaleConstraintsDialog->versionCreated(QStringLiteral("model"), cropVersion,
        QJsonObject{{"representation","baked_model"},{"modelPath",cropModel}});
    check(window.mWorkspace.sceneObjects().size() == count + 1 &&
        window.mWorkspace.activeSceneId() == editedId && window.mWorkspace.scenePath() == editedPath &&
        window.mViewport->hasUnsavedSceneEdits(),
        "adopting a scaled model retains the edited source as active so export/discard/exit guards remain reachable");
    window.mScaleConstraintsDialog->close();
    check(waitLifecycle([&]{return !window.mScaleConstraintsDialog;}), "close only the test-owned scale helper");
  }
  window.mViewport->undoEdit(); // Test cleanup, not an operation on a user project.
  expectedDialogs.stop();AppLanguage::apply(originalLanguage,false);
  std::fprintf(stderr,"Training batch lifecycle smoke: %s\n",passed?"PASS":"FAIL");
  return passed;
}
}
