#include "LanguageSmokeTest.h"
#include "MultiItemList.h"
#include "ExternalBackupStore.h"
#include "AppLanguage.h"
#include "AppTheme.h"
#include "MainWindow.h"
#include "TrainingDialog.h"
#include "MeshGenerationDialog.h"
#include "DatasetImportDialog.h"
#include "ReconstructionDialog.h"
#include "TrainingMonitorWidget.h"
#include "TrainingOutputLocator.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "WorkspaceDocument.h"
#include "ManagedName.h"
#include "ModelExportDialog.h"
#include "WrappingCheckBox.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QComboBox>
#include <QDockWidget>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QImage>
#include <QItemSelectionModel>
#include "WindowUiSmokeTest.h"
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QPushButton>
#include <QMenu>
#include <QMessageBox>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QToolBar>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeView>
#include <QTreeWidgetItemIterator>
#include <QTimer>

#include <functional>
#include <cmath>
#include <utility>

namespace gsw {
bool runMeshGenerationSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { qCritical() << "Mesh generation:" << message; passed = false; }
  };
  const auto waitUntil = [](const std::function<bool()> &predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
      QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
    }
    return predicate();
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const QDir root(temporary.path());
  const auto write = [](const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  QString error;
  const QString project = root.filePath(QStringLiteral("mesh-project.files"));
  check(QDir().mkpath(project) && window.mWorkspace.create(project, &error), "create owned project");
  check(window.mWorkspace.saveManifest(root.filePath(QStringLiteral("mesh-project.gsw.json")), &error), "save owned project");
  const QString original = QDir(window.mWorkspace.rootPath()).filePath(QStringLiteral("source.ply"));
  const QByteArray points("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n1 0 0\n0 1 0\n");
  check(write(original, points) && window.mWorkspace.addScenePath(original, &error), "load original model");
  check(window.mWorkspace.setSceneTranslation(QVector3D(1, 2, 3)), "transform original model");
  const QString originalId = window.mWorkspace.activeSceneId();
  const QString helper = qEnvironmentVariable("GSW_PROCESS_OUTPUT_FIXTURE",
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gsw_process_output_fixture.exe")));
  if (!QFileInfo(helper).isFile()) {
    qCritical() << "Mesh generation: missing owned worker fixture:" << helper;
    return false;
  }
  const QByteArray mesh("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n");
  QString texturedPath;
  QString texturedId;
  for (const int scenario : {0, 1, 2, 3}) {
    const bool failed = scenario == 1 || scenario == 3;
    const bool textured = scenario == 2;
    const bool invalidMaterial = scenario == 3;
    const QString state = scenario == 1 ? QStringLiteral("failed") : QStringLiteral("done");
    const QString expectedState = failed ? QStringLiteral("failed") : QStringLiteral("done");
    const QString taskName = invalidMaterial ? QStringLiteral("invalid-material") :
        textured ? QStringLiteral("textured") : state;
    const QString directory = QDir(window.mWorkspace.rootPath()).filePath(QStringLiteral("output/meshes/") + taskName);
    const QString path = QDir(directory).filePath(QStringLiteral("model/mesh.ply"));
    check(write(path, mesh), "write complete mesh fixture");
    QJsonObject result{
        {"version", 1}, {"task", "mesh"}, {"state", state}, {"meshPath", path},
        {"completedStages", QJsonArray{QStringLiteral("mesh")}}};
    QString expectedPath = path;
    const int expectedFaces = textured ? 2 : 1;
    if (textured || invalidMaterial) {
      const QString materialPath = QDir(directory).filePath(QStringLiteral("model/material-preview/textured.ply"));
      const QByteArray texturedMesh("ply\nformat ascii 1.0\ncomment TextureFile atlas.png\nelement vertex 4\nproperty double x\nproperty double y\nproperty double z\nelement face 2\nproperty list uchar int vertex_indices\nproperty list uchar float texcoord\nend_header\n0 0 0\n1 0 0\n1 1 0\n0 1 0\n3 0 1 2 6 0 0 1 0 1 1\n3 0 2 3 6 0.25 0.25 1 1 0 1\n");
      check(write(materialPath, texturedMesh), "write textured mesh with face-corner UV seam");
      QImage atlas(2, 2, QImage::Format_RGBA8888);
      atlas.setPixelColor(0, 0, Qt::red); atlas.setPixelColor(1, 0, Qt::green);
      atlas.setPixelColor(0, 1, Qt::blue); atlas.setPixelColor(1, 1, Qt::yellow);
      const QString atlasPath = QFileInfo(materialPath).absoluteDir().filePath(QStringLiteral("atlas.png"));
      if (!invalidMaterial)
        check(atlas.save(atlasPath), "write native-decoded PNG material fixture");
      result.insert(QStringLiteral("materialPreview"), QJsonObject{{"ply", materialPath}, {"atlas", atlasPath}});
      result.insert(QStringLiteral("completedStages"), QJsonArray{
          QStringLiteral("mesh"), QStringLiteral("texture"), QStringLiteral("texture_preview")});
      if (textured) {
        texturedPath = materialPath;
        expectedPath = materialPath;
      }
    }
    check(write(QDir(directory).filePath(QStringLiteral("result.json")), QJsonDocument(result).toJson()), "write stage journal");
    const auto before = window.mWorkspace.sceneObjects().size();
    window.mPendingMesh = MainWindow::PendingMesh{taskName, window.mWorkspace.rootPath(), directory, {}};
    check(window.mProcessSupervisor.start(taskName, helper,
        {QStringLiteral("mesh-worker"), path, state}, {}, {}, true), "start owned mesh worker");
    check(waitUntil([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingMesh; }), "mesh worker finishes");
    check(window.mWorkspace.sceneObjects().size() == before + 1 && window.mWorkspace.scenePath() == expectedPath,
          "append final or validated partial mesh without replacing originals");
    check(waitUntil([&] { return window.mViewport->meshRenderingAvailable() &&
        window.mViewport->scenePath() == expectedPath && window.mViewport->renderMode() == NativeViewport::RenderMode::Mesh &&
        (!textured || window.mViewport->meshTextureAvailable()); }),
        "generated geometry appears in mesh mode");
    if (textured) check(window.mViewport->meshTextureAvailable(), "completed material preview is uploaded to the GPU");
    const auto *taskState = window.mTaskTable->item(window.mTaskTable->rowCount() - 1, 0);
    check(taskState && taskState->data(Qt::UserRole + 31).toString() == expectedState,
          "partial mesh cannot mark failed texturing successful");
    if (invalidMaterial)
      check(!window.mViewport->meshTextureAvailable() && window.mWorkspace.scenePath() == path,
            "missing atlas rejects material success but retains valid base mesh");
    const auto selectedId = window.mWorkspace.activeSceneId();
    if (textured) texturedId = selectedId;
    const auto viewTarget = window.mViewport->viewTarget();
    const auto viewDistance = window.mViewport->viewDistance();
    const auto viewAngles = window.mViewport->viewOrbitAngles();
    const auto viewOrthographic = window.mViewport->orthographicProjection();
    int languageSceneLoads = 0;
    const auto loadConnection = QObject::connect(window.mViewport, &NativeViewport::sceneLoadStarted,
        &window, [&](const QString &) { ++languageSceneLoads; });
    for (const QString &language : AppLanguage::supported()) {
      AppLanguage::apply(language, false);
      check(window.mGenerateMeshAction->text() == QCoreApplication::translate("Workbench", "生成网格..."), "mesh action translates live");
      check(window.mTaskTable->item(window.mTaskTable->rowCount() - 1, 3)->text() == (failed
          ? QCoreApplication::translate("Workbench", "保留已完成网格；后续阶段未完成")
          : QCoreApplication::translate("Workbench", textured ? "贴图网格已载入 · %1 个面" : "网格已载入 · %1 个面").arg(expectedFaces)), "mesh history translates live");
      check(window.mWorkspace.activeSceneId() == selectedId, "language change preserves generated model");
      check(window.mViewport->scenePath() == expectedPath && (!textured || window.mViewport->meshTextureAvailable()),
            "language change preserves loaded geometry and material");
      check(window.mViewport->viewTarget() == viewTarget && window.mViewport->viewDistance() == viewDistance &&
          window.mViewport->viewOrbitAngles() == viewAngles && window.mViewport->orthographicProjection() == viewOrthographic,
          "language change preserves camera state");
    }
    QObject::disconnect(loadConnection);
    check(languageSceneLoads == 0, "language switch does not rebuild model or texture state");
  }
  const QString selectedResult = texturedId;
  check(!selectedResult.isEmpty(), "textured result has independent persistent identity");
  check(window.mViewport->activateSceneObject(originalId), "select preserved original");
  check(waitUntil([&] { return window.mViewport->scenePath() == original &&
      window.mViewport->renderedPointCount() == 3; }), "original has independent viewport data");
  check(window.mViewport->activateSceneObject(selectedResult), "return to generated mesh");
  const bool returnedMaterialReady = waitUntil([&] {
    // Hidden installed-QA windows may not receive a presentation paint after
    // switching objects. Render the owned framebuffer to service pending GPU
    // uploads, rather than treating window exposure as material readiness.
    (void)window.mViewport->grabFramebuffer();
    return window.mViewport->scenePath() == texturedPath && window.mViewport->meshTextureAvailable();
  });
  if (!returnedMaterialReady)
    qCritical() << "[material-return]" << "actual path" << window.mViewport->scenePath()
        << "expected path" << texturedPath << "mesh available" << window.mViewport->meshRenderingAvailable()
        << "texture available" << window.mViewport->meshTextureAvailable()
        << "render mode" << static_cast<int>(window.mViewport->renderMode())
        << "rendered points" << window.mViewport->renderedPointCount();
  check(returnedMaterialReady, "material remains available after changing active model");
  QFile source(original);
  check(source.open(QIODevice::ReadOnly) && source.readAll() == points, "original file unchanged");
  check(window.mWorkspace.saveManifest({}, &error), "save selected textured result before reopen");
  WorkspaceDocument reopened;
  check(reopened.load(window.mWorkspace.projectFilePath(), &error), "reopen staged project");
  check(reopened.sceneObjects().size() == 5 && reopened.activeSceneId() == window.mWorkspace.activeSceneId(),
        "mesh objects and selected result persist");
  check(reopened.scenePath() == texturedPath, "reopened active model links to material preview");
  const auto reopenedMaterial = PlyPointCloudLoader::load(reopened.scenePath());
  check(reopenedMaterial.isValid() && reopenedMaterial.hasMesh() && reopenedMaterial.meshHasTextureCoordinates &&
      reopenedMaterial.sourceFaceCount == 2 && reopenedMaterial.meshVertices.size() == 5 &&
      reopenedMaterial.meshTextureImage.size() == QSize(2, 2) && reopenedMaterial.meshTextureError.isEmpty(),
      "saved project preserves texture asset linkage and UV seam");
  if (!reopenedMaterial.meshTextureImage.isNull())
    check(reopenedMaterial.meshTextureImage.pixelColor(0, 0) == QColor(Qt::red) &&
        reopenedMaterial.meshTextureImage.pixelColor(0, 1) == QColor(Qt::blue),
        "reopened PNG retains material color and orientation");
  const auto objects = reopened.sceneObjects();
  bool originalPreserved = false;
  for (const auto &object : objects)
    if (object.id == originalId) originalPreserved = object.translation == QVector3D(1, 2, 3);
  check(originalPreserved, "source transform preserved");
  qInfo() << "Mesh generation desktop smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runTrainingResumeSmokeTest(MainWindow &window) {
  const QString backend = qEnvironmentVariable("GSW_RESUME_SMOKE_BACKEND", "3dgs");
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { qCritical() << "Training resume:" << message; passed = false; }
  };
  QTimer unexpectedDialog;
  QObject::connect(&unexpectedDialog, &QTimer::timeout, &window, [&]() {
    if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      qCritical() << "Training resume: unexpected dialog" << dialog->text();
      passed = false;
      dialog->reject();
    }
  });
  unexpectedDialog.start(50);
  const auto waitUntil = [](const std::function<bool()> &predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
      QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
    }
    return predicate();
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const QDir root(temporary.path());
  const auto write = [&](const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  const QString project = root.filePath(QStringLiteral("project"));
  QString createError;
  if (!QDir().mkpath(project) || !window.mWorkspace.create(project, &createError)) {
    qCritical() << "Training resume: create isolated project:" << createError;
    return false;
  }
  if (!window.mWorkspace.saveManifest(root.filePath(QStringLiteral("paused.gsw")), &createError)) {
    qCritical() << "Training resume: save isolated project:" << createError;
    return false;
  }
  const QString output = QDir(project).filePath(QStringLiteral("output/test"));
  const QString config = QDir(project).filePath(QStringLiteral(".gsw/jobs/test.json"));
  check(write(config, QJsonDocument(QJsonObject{{"nativeCheckpoint", true}, {"backend", backend}}).toJson()), "write task config");
  check(write(QDir(output).filePath(QStringLiteral(".gsw-resume/state-0123456789abcdef0123456789abcdef.pth")), "fixture"), "write checkpoint fixture");
  check(write(QDir(output).filePath(QStringLiteral(".gsw-resume/ready.json")),
      QJsonDocument(QJsonObject{{"version", 1}, {"iteration", 3}, {"total", 10}, {"backend", backend},
          {"file", "state-0123456789abcdef0123456789abcdef.pth"}}).toJson()), "write checkpoint manifest");
  QByteArray preview(
      "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
      "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\n"
      "property float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\n"
      "property float rot_1\nproperty float rot_2\nproperty float rot_3\nend_header\n"
      "0 0 0 0 0 0 1 -3 -3 -3 1 0 0 0\n1 0 0 0 0 0 1 -3 -3 -3 1 0 0 0\n"
      "0 1 0 0 0 0 1 -3 -3 -3 1 0 0 0\n0 0 1 0 0 0 1 -3 -3 -3 1 0 0 0\n");
  if (backend == QStringLiteral("2dgs")) {
    preview.replace("property float scale_2\n", "");
    preview.replace("-3 -3 -3", "-3 -3");
  }
  check(write(QDir(output).filePath(QStringLiteral("point_cloud/iteration_3/point_cloud.ply")), preview), "write complete preview");
  check(saveActiveTrainingJob(project, {config, output}), "save active task");
  const QString helper = qEnvironmentVariable("GSW_PROCESS_OUTPUT_FIXTURE",
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gsw_process_output_fixture.exe")));
  window.mPendingTraining = MainWindow::PendingTraining{QStringLiteral("fixture"), project, {}, output, backend, 10};
  check(window.mProcessSupervisor.start(QStringLiteral("fixture"), helper, {QStringLiteral("pause-worker")}, {}, {}, true), "start owned worker");
  WorkerStatus status; status.state = QStringLiteral("running"); status.stage = QStringLiteral("colmap");
  emit window.mProcessSupervisor.workerStatusReady(status);
  check(!window.mPauseTrainingAction->isEnabled(), "cannot pause COLMAP as Gaussian training");
  status.stage = QStringLiteral("train"); status.iteration = 3; status.totalIterations = 10;
  emit window.mProcessSupervisor.workerStatusReady(status);
  check(window.mPauseTrainingAction->isEnabled(), "pause enabled during Gaussian training");
  window.mPauseTrainingAction->trigger();
  check(!window.mPauseTrainingAction->isEnabled() && window.mPauseRequested, "duplicate pause blocked while saving");
  check(waitUntil([&] { return !window.mProcessSupervisor.isRunning() && !window.mPendingTraining; }), "pause finishes");
  check(!window.mProcessSupervisor.wasStopRequested(), "pause never cancels");
  check(window.mResumeTrainingAction->isEnabled(), "resume available after pause");
  check(loadActiveTrainingJob(project).isValid(), "durable resume pointer retained");
  check(!window.mWorkspace.scenePath().isEmpty(), "paused preview associated with project");
  check(waitUntil([&] { return window.mViewport->selectableModelAvailable() && window.mViewport->hasEditableScene(); }),
        "paused full checkpoint restores observation and trim eligibility");
  check(window.mInspectAction->isEnabled() && !window.mSelectionToolbar->isHidden(), "paused checkpoint restores inspect toolbar");
  check(window.mTaskTable->item(0, 0)->data(Qt::UserRole + 31) == QStringLiteral("paused"), "history shows paused, not failed");
  const auto samples = window.mTrainingMonitor->telemetry().samples().size();
  for (const QString &language : AppLanguage::supported()) {
    AppLanguage::apply(language);
    check(window.mPauseTrainingAction->text() == QCoreApplication::translate("Workbench", "暂停训练"), "pause label language");
    check(window.mResumeTrainingAction->text() == QCoreApplication::translate("Workbench", "继续训练"), "resume label language");
    check(window.mTaskTable->item(0, 0)->text() == QCoreApplication::translate("Workbench", "已暂停"), "paused history translates live");
    check(window.mTrainingMonitor->telemetry().samples().size() == samples, "translation preserves telemetry");
  }
  // A paused job is not an unexpected crash. Reopening must not replace a
  // different model that the user imported/selected after the safe pause.
  const QString otherModel = QDir(window.mWorkspace.rootPath()).filePath(QStringLiteral("other.ply"));
  check(QFile::copy(window.mWorkspace.scenePath(), otherModel), "copy independent model fixture");
  check(window.mWorkspace.addScenePath(otherModel), "append another model after pause");
  check(window.mWorkspace.setSceneTranslation(QVector3D(1, 2, 3)), "transform other model");
  check(window.mWorkspace.saveManifest(), "save multi-model paused project");
  const QString selectedId = window.mWorkspace.activeSceneId();
  MainWindow reopened;
  check(reopened.openProjectFile(window.mWorkspace.projectFilePath()), "reopen paused project");
  check(reopened.mResumeTrainingAction->isEnabled() && loadActiveTrainingJob(project).isValid(), "resume survives project reopen");
  check(reopened.mWorkspace.scenePath() == otherModel && reopened.mWorkspace.activeSceneId() == selectedId &&
      reopened.mWorkspace.sceneTranslation() == QVector3D(1, 2, 3) && reopened.mWorkspace.sceneObjects().size() == 2,
      "reopen preserves subsequent model selection and transform");
  qInfo() << "Training resume desktop smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runListInteractionSmokeTest(MainWindow &window) {
  bool passed = true;
  auto check = [&](bool condition, const char *description) {
    if (!condition) { qCritical() << "List smoke:" << description; passed = false; }
  };
  const auto key = [](QWidget *target, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, code, modifiers);
    QApplication::sendEvent(target, &overrideEvent);
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(target, &event);
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  QStringList mediaPaths;
  for (int i = 0; i < 3; ++i) {
    QFile file(QDir(temporary.path()).filePath(QString("source-%1.jpg").arg(i)));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write("fixture"); file.close(); mediaPaths.append(file.fileName());
  }
  DatasetImportDialog media({}, "Batch", mediaPaths, temporary.path(), true, &window);
  auto *sources = media.findChild<QListWidget *>();
  check(sources && sources->selectionMode() == QAbstractItemView::ExtendedSelection, "media extended selection");
  if (sources) {
    key(sources, Qt::Key_A, Qt::ControlModifier);
    check(sources->selectedItems().size() == 3, "media select all");
    key(sources, Qt::Key_Delete);
    check(sources->count() == 0, "media batch removal");
  }
  for (const auto &path : mediaPaths) check(QFileInfo::exists(path), "media source preserved");

  auto *tasks = window.mTaskTable;
  const bool running = window.mProcessSupervisor.isRunning();
  tasks->insertRow(0);
  for (int col = 0; col < 4; ++col) tasks->setItem(0, col, new QTableWidgetItem("history-fixture"));
  if (window.mActiveTaskRow >= 0) ++window.mActiveTaskRow;
  const QPersistentModelIndex active = running
      ? QPersistentModelIndex(tasks->model()->index(window.mActiveTaskRow, 0)) : QPersistentModelIndex();
  QTimer answer;
  QObject::connect(&answer, &QTimer::timeout, &window, []() {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->done(QMessageBox::Yes);
  });
  answer.start(10);
  key(tasks, Qt::Key_A, Qt::ControlModifier);
  key(tasks, Qt::Key_Delete);
  check(tasks->rowCount() == (running ? 1 : 0), "task batch history removal");
  if (running) {
    check(active.isValid() && active.row() == window.mActiveTaskRow && window.mActiveTaskRow == 0,
          "active task row remapped and protected");
    key(tasks, Qt::Key_Delete);
    check(tasks->rowCount() == 1, "running-only selection cannot be removed");
    window.mProcessSupervisor.shutdown();
  }
  answer.stop();

  // Never operate on the user's recovery catalog: replace it with an owned fixture.
  auto originalStore = std::move(window.mRecoveryStore);
  window.mRecoveryStore = std::make_unique<RecoveryStore>(QDir(temporary.path()).filePath("recovery"));
  for (int i = 0; i < 3; ++i)
    check(window.mRecoveryStore->beginWorkspace(QString("Recovery %1").arg(i)).has_value(), "create recovery fixture");
  const QString locale = AppLanguage::current();
  QTimer drive;
  QElapsedTimer elapsed;
  elapsed.start();
  int stage = 0;
  QTimer recoveryAnswer;
  QObject::connect(&recoveryAnswer, &QTimer::timeout, &window, [&]() {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      box->done(stage == 1 ? QMessageBox::Cancel : QMessageBox::Yes);
  });
  recoveryAnswer.start(10);
  QObject::connect(&drive, &QTimer::timeout, &window, [&]() {
    auto *modal = QApplication::activeModalWidget();
    if (!modal) return;
    if (elapsed.elapsed() > 12000) { passed = false; modal->close(); return; }
    if (auto *box = qobject_cast<QMessageBox *>(modal)) {
      box->done(stage == 1 ? QMessageBox::Cancel : QMessageBox::Yes);
      return;
    }
    auto *table = modal->findChild<QTableWidget *>("recoveryRecords");
    if (!table || !table->isEnabled()) return;
    if (stage == 0) {
      table->selectAll();
      check(selectedListRows(table).size() == 3, "recovery batch selected");
      for (auto *button : modal->findChildren<QPushButton *>())
        if (button->property("gswTranslation_text").toByteArray() == "恢复所选工程")
          check(!button->isEnabled(), "restore disabled for multiple records");
      const auto selected = selectedListRows(table);
      for (const auto &language : AppLanguage::supported()) {
        check(AppLanguage::apply(language, false), "list live language switch");
        check(selectedListRows(table) == selected, "list selection survives translation");
        check(table->findChild<QAction *>("listSelectAll")->text() ==
              QCoreApplication::translate("Workbench", "全选"), "list action translates live");
        const QString output = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
        if (!output.isEmpty()) {
          QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
          modal->grab().save(QDir(output).filePath(language + "-recovery-list.png"));
        }
      }
      AppLanguage::apply(locale, false);
      stage = 1;
      table->findChild<QAction *>("listRemoveSelected")->trigger();
      check(table->rowCount() == 3, "cancel keeps recovery records");
      stage = 2;
    } else if (stage == 2) {
      stage = 3;
      table->findChild<QAction *>("listRemoveSelected")->trigger();
      check(table->rowCount() == 0, "all selected recovery records removed");
      modal->close();
    }
  });
  drive.start(15);
  window.showRecoveryCenter(false);
  drive.stop();
  recoveryAnswer.stop();
  check(stage == 3 && window.mRecoveryStore->recoverableWorkspaces().isEmpty(), "recovery batch completed in place");
  window.mRecoveryStore = std::move(originalStore);
  AppLanguage::apply(locale, false);
  const auto inspectRecordDialog = [&](const QString &objectName, const std::function<void()> &open) {
    QTimer inspect;
    bool visited = false;
    QObject::connect(&inspect, &QTimer::timeout, &window, [&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      auto *table = dialog->findChild<QTableWidget *>(objectName);
      if (!table) { passed = false; dialog->reject(); return; }
      visited = true;
      table->selectAll();
      const auto selection = selectedListRows(table);
      check(selection.size() >= 2, "snapshot/backup multiple records");
      for (const auto &language : AppLanguage::supported()) {
        AppLanguage::apply(language, false);
        check(selectedListRows(table) == selection, "snapshot/backup selection survives live translation");
        check(table->findChild<QAction *>("listRemoveSelected")->text() ==
              QCoreApplication::translate("Workbench", "删除所选记录"), "snapshot/backup removal translated");
      }
      AppLanguage::apply(locale, false);
      dialog->reject();
    });
    inspect.start(15);
    open();
    inspect.stop();
    check(visited, "snapshot/backup dialog exercised");
  };
  QString storeError;
  const QString projectFile = QDir(temporary.path()).filePath("list-project.gsw.json");
  check(window.mWorkspace.saveManifest(projectFile, &storeError), "save snapshot fixture project");
  for (int i = 0; i < 2; ++i)
    check(window.mRecoveryStore->createProjectSnapshot(QString("{\"revision\":%1}").arg(i).toUtf8(),
        projectFile, window.mWorkspace.rootPath(), 20, &storeError).has_value(), "create snapshot list fixture");
  inspectRecordDialog("snapshotRecords", [&]() { window.showSnapshotHistory(); });
  const QString backupRoot = QDir(temporary.path()).filePath("backups");
  const QString backupData = QDir(temporary.path()).filePath("backup-data");
  QDir().mkpath(backupData);
  ExternalBackupStore backupStore(backupRoot);
  const QString backupProject = QDir(temporary.path()).filePath("backup-source.gsw.json");
  for (int i = 0; i < 2; ++i) {
    QFile file(backupProject);
    check(file.open(QIODevice::WriteOnly), "create backup fixture");
    file.write(QString("{\"revision\":%1}").arg(i).toUtf8()); file.close();
    check(backupStore.backupProject(backupProject, backupData, &storeError).has_value(), "create backup list fixture");
  }
  const auto previousBackupRoot = QSettings().value("recovery/externalBackupRoot");
  QSettings().setValue("recovery/externalBackupRoot", backupRoot);
  inspectRecordDialog("backupRecords", [&]() { window.showExternalBackups(); });
  if (previousBackupRoot.isValid()) QSettings().setValue("recovery/externalBackupRoot", previousBackupRoot);
  else QSettings().remove("recovery/externalBackupRoot");
  // Model references are removed as a batch; test-owned original PLYs survive.
  QFile extraModel(QDir(temporary.path()).filePath("second-model.ply"));
  check(extraModel.open(QIODevice::WriteOnly), "create second model fixture");
  extraModel.write("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n1 0 0\n0 1 0\n");
  extraModel.close();
  check(window.mWorkspace.addScenePath(extraModel.fileName(), &storeError), "import second model fixture");
  auto *tree = window.mProjectTree;
  const auto objects = window.mWorkspace.sceneObjects();
  check(objects.size() >= 2, "multiple model fixtures available");
  key(tree, Qt::Key_A, Qt::ControlModifier);
  check(window.mViewport->selectedSceneIds().size() == objects.size(), "project tree select all models");
  answer.start(10);
  key(tree, Qt::Key_Delete);
  answer.stop();
  check(window.mWorkspace.sceneObjects().isEmpty(), "project tree unloads all selected models");
  for (const auto &object : objects) check(QFileInfo::exists(object.path), "model source preserved");
  qInfo() << "List interaction smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runLanguageSmokeTest(MainWindow &window) {
  const QString locale = AppLanguage::current();
  const QString screenshotDirectory = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  const int index = AppLanguage::supported().indexOf(locale);
  bool passed = index >= 0 && window.isVisible();
  auto check = [&passed](bool condition, const char *description) {
    if (!condition) { qCritical() << "Language smoke:" << description; passed = false; }
  };
  if (!passed) return false;
  const QStringList saveTexts = {QStringLiteral("保存工程"), QStringLiteral("Save Project"), QStringLiteral("プロジェクトを保存")};
  const QStringList pointTexts = {QStringLiteral("点云"), QStringLiteral("Point Cloud"), QStringLiteral("点群")};
  const auto *saveAction = window.findChild<QAction *>(QStringLiteral("saveProjectAction"));
  check(saveAction && saveAction->text() == saveTexts[index], "save action translation");
  check(QCoreApplication::translate("Workbench", "点云") == pointTexts[index], "point cloud terminology");
  const QStringList untitled = {QStringLiteral("未命名工程"), QStringLiteral("Untitled Project"), QStringLiteral("無題のプロジェクト")};
  const auto *nameLabel = window.findChild<QLabel *>(QStringLiteral("projectNameValue"));
  check(nameLabel && nameLabel->text() == untitled[index], "dynamic project label");
  auto *languageMenu = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
  check(languageMenu && languageMenu->actions().size() == 3, "language menu");
  if (!languageMenu) return false;
  auto *appearanceMenu = window.findChild<QMenu *>(QStringLiteral("appearanceMenu"));
  auto *automaticTheme = window.findChild<QAction *>(QStringLiteral("automaticThemeAction"));
  auto *lightTheme = window.findChild<QAction *>(QStringLiteral("lightThemeAction"));
  auto *darkTheme = window.findChild<QAction *>(QStringLiteral("darkThemeAction"));
  auto *automaticThemeTimer = window.findChild<QTimer *>(QStringLiteral("automaticThemeTimer"));
  check(appearanceMenu && automaticTheme && lightTheme && darkTheme &&
        appearanceMenu->actions().size() == 3 && automaticThemeTimer,
        "automatic/day/night appearance menu and schedule are available");
  if (!appearanceMenu || !automaticTheme || !lightTheme || !darkTheme || !automaticThemeTimer) return false;
  check(automaticThemeTimer->isSingleShot() && automaticThemeTimer->timerType() == Qt::PreciseTimer,
        "automatic appearance uses one precise single-shot timer");
  const UiThemeMode originalThemeMode = AppTheme::currentThemeMode();
  const auto restoreThemeMode = [&] {
    (originalThemeMode == UiThemeMode::Automatic ? automaticTheme :
        originalThemeMode == UiThemeMode::Light ? lightTheme : darkTheme)->trigger();
  };
  for (int i = 0; i < 3; ++i) {
    check(languageMenu->actions()[i]->data().toString() == AppLanguage::supported()[i], "stable locale identifiers");
    check(languageMenu->actions()[i]->text() == AppLanguage::displayName(AppLanguage::supported()[i]), "autonyms");
  }
  const QString name = QStringLiteral("古墳 / 日本語:*?\"<>|📷 ");
  QTemporaryDir trainingImages;
  if (!trainingImages.isValid()) return false;
  check(QDir().mkpath(QDir(trainingImages.path()).filePath(QStringLiteral("images"))), "create training image header fixture");
  QImage trainingPhoto(1928, 1084, QImage::Format_RGB32);
  trainingPhoto.fill(Qt::darkGray);
  check(trainingPhoto.save(QDir(trainingImages.path()).filePath(QStringLiteral("images/古墳.png"))), "save full-size training fixture");
  TrainingDialog training(trainingImages.path(), name,
                          QCoreApplication::applicationDirPath(), true, true, &window);
  const auto configuration = training.configuration();
  MeshGenerationDialog meshing({}, &window);
  auto *meshMethod = meshing.findChild<QComboBox *>(QStringLiteral("meshMethodCombo"));
  check(meshMethod && meshMethod->count() == 4, "all native mesh methods exposed");
  check(meshing.windowTitle() == QCoreApplication::translate("Workbench", "生成网格"), "mesh dialog translation");
  if (meshMethod) meshMethod->setCurrentIndex(3);
  check(configuration.backend == QStringLiteral("3dgs"), "training backend must not be translated");
  check(configuration.quality == QStringLiteral("quick"), "preset identifier must not be translated");
  check(configuration.iterations == 10000 && configuration.resolution == 2,
        "3DGS quick preset uses the shared preview fidelity target");
  auto *backendCombo = training.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo"));
  auto *pipelineHint = training.findChild<QLabel *>(QStringLiteral("trainingPipelineHint"));
  check(backendCombo && backendCombo->count() == 2 && pipelineHint, "both training backends expose capabilities");
  if (backendCombo && pipelineHint) {
    backendCombo->setCurrentIndex(1);
    check(training.configuration().backend == QStringLiteral("2dgs"), "2DGS backend identifier remains stable");
    check(training.configuration().iterations == 10000 && training.configuration().resolution == 2,
          "2DGS quick preset retains parity");
    check(pipelineHint->text() == QCoreApplication::translate("Workbench", "2DGS 支持暂停、完整状态续训与连续快照预览；视口使用透视校正曲面显示，混合结果与 CUDA 渲染可能不同。"), "2DGS capabilities translated");
    backendCombo->setCurrentIndex(0);
  }
  auto *qualityCombo = training.findChild<QComboBox *>(QStringLiteral("trainingQualityCombo"));
  auto *resolutionCombo = training.findChild<QComboBox *>(QStringLiteral("trainingResolutionCombo"));
  auto *iterationsSpin = training.findChild<QSpinBox *>(QStringLiteral("trainingIterationsSpinBox"));
  auto *resolutionHint = training.findChild<QLabel *>(QStringLiteral("trainingResolutionHint"));
  check(qualityCombo && qualityCombo->count() == 5 && resolutionCombo && iterationsSpin && resolutionHint,
        "all five presets and original-resolution controls available");
  if (backendCombo && qualityCombo && resolutionCombo && iterationsSpin) {
    for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
      backendCombo->setCurrentIndex(backendCombo->findData(backend));
      for (const QString &quality : {QStringLiteral("full"), QStringLiteral("quality"),
                                     QStringLiteral("max_quality"), QStringLiteral("original_quality"), QStringLiteral("quick")}) {
        qualityCombo->setCurrentIndex(qualityCombo->findData(quality));
        const int expectedIterations = quality == QStringLiteral("quick") ? 10000 : 30000;
        const int expectedResolution = quality == QStringLiteral("full") ? 8 :
            quality == QStringLiteral("quality") ? 4 :
            quality == QStringLiteral("original_quality") ? 1 : 2;
        check(training.configuration().iterations == expectedIterations &&
              training.configuration().resolution == expectedResolution,
              "UI/backend preset resolution and iteration parity");
        if (resolutionHint) check(resolutionHint->isHidden() == (expectedResolution != 1),
                                  "original-resolution resource warning follows actual resolution");
      }
    }
    backendCombo->setCurrentIndex(0);
    qualityCombo->setCurrentIndex(qualityCombo->findData(QStringLiteral("original_quality")));
    resolutionCombo->setCurrentIndex(resolutionCombo->findData(1));
  }
  DatasetImportDialog import({}, name, {}, QCoreApplication::applicationDirPath(), true, &window);
  check(import.request().sceneName == name, "user scene name must remain unchanged");
  // This source lives in the catalog under the original English diagnostic.
  const QString error = QCoreApplication::translate("Workbench", "Unable to open PLY file %1: %2")
                            .arg(name, QStringLiteral("test"));
  check(error.contains(name) && !error.contains(QStringLiteral("%1")), "error placeholders");
  QMessageBox box(QMessageBox::Question, QStringLiteral("test"), QStringLiteral("test"),
                 QMessageBox::Save | QMessageBox::Cancel, &window);
  const QString cancel = box.button(QMessageBox::Cancel)->text().remove(QLatin1Char('&'));
  check(!cancel.isEmpty() && (index == 1 || cancel != QStringLiteral("Cancel")), "Qt standard buttons");
  check(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs), "file dialog locale isolation");
  const auto waitUntil = [](const std::function<bool()> &predicate, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
      QEventLoop loop;
      QTimer::singleShot(20, &loop, &QEventLoop::quit);
      loop.exec();
    }
    return predicate();
  };
  auto *inputSummary = training.findChild<QLabel *>(QStringLiteral("trainingInputSummaryLabel"));
  const QString fullSize = QCoreApplication::translate("Workbench", "%1 × %2 → %3 × %4 px（%5 张）")
      .arg(QLocale().toString(1928), QLocale().toString(1084), QLocale().toString(1928),
           QLocale().toString(1084), QLocale().toString(1));
  check(inputSummary && waitUntil([&] { return inputSummary->text().contains(fullSize); }),
        "asynchronous image headers show actual paired source size and original-resolution estimate");
  if (resolutionCombo && inputSummary) {
    resolutionCombo->setCurrentIndex(resolutionCombo->findData(2));
    check(inputSummary->text().contains(QCoreApplication::translate("Workbench", "%1 × %2 → %3 × %4 px（%5 张）")
        .arg(QLocale().toString(1928), QLocale().toString(1084), QLocale().toString(964),
             QLocale().toString(542), QLocale().toString(1))), "ratio change formats cached image dimensions");
    resolutionCombo->setCurrentIndex(resolutionCombo->findData(1));
  }
  ReconstructionDialog reconstruction(QCoreApplication::applicationDirPath(),
                                      QCoreApplication::applicationDirPath(), &window);
  const auto reconstructionConfig = reconstruction.configuration();
  // A custom iteration count catches accidental preset resets caused by
  // currentTextChanged signals when combo captions are translated.
  if (iterationsSpin) iterationsSpin->setValue(12345);
  if (backendCombo && iterationsSpin && resolutionCombo) {
    const int selectedResolution = training.configuration().resolution;
    backendCombo->setCurrentIndex(1);
    check(training.configuration().iterations == 12345 && training.configuration().resolution == selectedResolution,
          "backend switch preserves manually edited shared parameters");
    backendCombo->setCurrentIndex(0);
    check(training.configuration().iterations == 12345 && training.configuration().resolution == selectedResolution,
          "returning to 3DGS preserves manually edited shared parameters");
  }
  training.findChild<QLineEdit *>(QStringLiteral("trainingOutputNameEdit"))->setText(name);
  const auto editedConfiguration = training.configuration();
  auto *viewport = qobject_cast<NativeViewport *>(window.centralWidget());
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *supervisor = window.findChild<ProcessSupervisor *>();
  auto *monitor = window.findChild<TrainingMonitorWidget *>();
  auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("projectTree"));
  if (!viewport || !document || !supervisor || !monitor || !tree) return false;
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const auto media = QDir(temporary.path()).filePath(QStringLiteral("images/frame.jpg"));
  check(QDir().mkpath(QFileInfo(media).absolutePath()), "create media fixture directory");
  QImage photo(8, 8, QImage::Format_RGB32);
  photo.fill(Qt::red);
  check(photo.save(media), "create photo fixture");
  DatasetImportDialog namedImport(temporary.path(), name, {media}, temporary.path(), true, &window);
  QMetaObject::invokeMethod(&namedImport, "accept", Qt::DirectConnection);
  check(namedImport.result() == QDialog::Accepted && namedImport.validatedPlan().has_value(), "arbitrary name accepted by import dialog");
  if (namedImport.validatedPlan()) {
    check(namedImport.validatedPlan()->sceneName() == name, "import display name preserved exactly");
    check(QFileInfo(namedImport.validatedPlan()->managedDatasetPath(temporary.path())).fileName() ==
          managedStorageName(name), "import path encoded safely");
  }
  check(training.configuration().outputScene == name && training.configuration().outputStorageName == managedStorageName(name),
        "training display and storage names separated");
  QFile ply(QDir(temporary.path()).filePath(QStringLiteral("点云_日本語.ply")));
  if (!ply.open(QIODevice::WriteOnly)) return false;
  ply.write("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nend_header\n-1 -1 0\n1 -1 0\n0 1 0\n0 0 1\n");
  ply.close();
  check(window.importSceneFile(ply.fileName()), "import test model");
  check(waitUntil([&] { return viewport->selectableModelAvailable(); }), "model is loaded");
  viewport->setModelTransform({1, 2, 3}, QQuaternion::fromEulerAngles(12, 23, 34), {2, 3, 4});
  viewport->selectModelForRotate();
  viewport->setAxisView(NavigationAxis::PositiveZ);
  waitUntil([] { return false; }, 400);
  auto *lockTools = window.findChild<QAction *>(QStringLiteral("lockEditToolsAction"));
  auto *trackball = window.findChild<QAction *>(QStringLiteral("observationTrackballAction"));
  if (!trackball) return false;
  if (!lockTools) return false;
  lockTools->setChecked(true);
  auto *exportAction = window.findChild<QAction *>(QStringLiteral("exportModelAction"));
  check(exportAction && exportAction->isEnabled() && exportAction->shortcut() == QKeySequence(QStringLiteral("Ctrl+E")),
        "model export available without crop edits and while editing is locked");
  // Exercise the actual menu action, options dialog and asynchronous writer.
  const QString uiExportPath = QDir(temporary.path()).filePath(QStringLiteral("ui-export.glb"));
  bool exportDialogSeen = false;
  QTimer exportClick;
  QObject::connect(&exportClick, &QTimer::timeout, &window, [&] {
    auto *activeDialog = window.findChild<QDialog *>(QStringLiteral("modelExportDialog"));
    if (!activeDialog || !activeDialog->isVisible()) return;
    auto *format = activeDialog->findChild<QComboBox *>(QStringLiteral("modelExportFormat"));
    auto *path = activeDialog->findChild<QLineEdit *>(QStringLiteral("modelExportPath"));
    auto *confirm = activeDialog->findChild<QAbstractButton *>(QStringLiteral("confirmModelExport"));
    if (!format || !path || !confirm) { activeDialog->reject(); return; }
    exportClick.stop(); exportDialogSeen = true;
    format->setCurrentIndex(format->findData(static_cast<int>(ModelExportFormat::Glb)));
    path->setText(uiExportPath); confirm->click();
  });
  exportClick.start(20);
  if (exportAction) exportAction->trigger();
  exportClick.stop();
  check(exportDialogSeen && QFileInfo(uiExportPath).size() > 28 && viewport->editToolsLocked(),
        "model export action writes a standalone GLB while locked");
  ModelExportDialog exportDialog(viewport->modelExportOptions(), true, false, {ply.fileName()}, &window);
  auto spzOptions = viewport->modelExportOptions();
  spzOptions.spzVersion = 3; spzOptions.spzQuality = 2; spzOptions.spzMaximumShDegree = 2;
  ModelExportDialog spzDialog(spzOptions, false, true, {ply.fileName()}, &window);
  auto *spzFormat = spzDialog.findChild<QComboBox *>(QStringLiteral("modelExportFormat"));
  auto *spzQuality = spzDialog.findChild<QComboBox *>(QStringLiteral("spzQuality"));
  auto *spzVersion = spzDialog.findChild<QComboBox *>(QStringLiteral("spzVersion"));
  auto *spzDegree = spzDialog.findChild<QComboBox *>(QStringLiteral("spzShDegree"));
  spzFormat->setCurrentIndex(spzFormat->findData(static_cast<int>(ModelExportFormat::Spz)));
  check(spzQuality->currentIndex() == 2 && spzVersion->currentData().toInt() == 3 &&
        spzDegree->currentData().toInt() == 2, "SPZ controls preserve supplied options");
  check(spzDialog.options().format == ModelExportFormat::Spz && !spzDialog.options().applyTransform,
        "SPZ uses original coordinates and true Gaussian format");
  auto *exportFormat = exportDialog.findChild<QComboBox *>(QStringLiteral("modelExportFormat"));
  check(exportFormat && exportFormat->count() == 6, "all model export formats");
  exportFormat->setCurrentIndex(exportFormat->findData(static_cast<int>(ModelExportFormat::Glb)));
  const QString exportPath = exportDialog.options().destinationPath;
  check(exportPath.endsWith(QStringLiteral(".glb")), "format switch updates extension");
  check(viewport->editToolsLocked() && !viewport->modelTransformActive(), "lock cancels modal edit");
  const QStringList lockTexts = {QStringLiteral("锁定编辑工具"), QStringLiteral("Lock Editing Tools"), QStringLiteral("編集ツールをロック")};
  const QStringList trackballTexts = {QStringLiteral("观察轨迹球"), QStringLiteral("View Trackball"), QStringLiteral("ビュートラックボール")};
  auto *root = tree->topLevelItem(0);
  if (root && root->childCount() > 0) root->child(0)->setExpanded(false);
  const auto treeState = [&] {
    QStringList state;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
      state << QStringLiteral("%1:%2:%3").arg((*it)->isExpanded()).arg((*it)->isSelected())
          .arg(tree->currentItem() == *it);
    return state;
  };
  const auto selection = viewport->selectedSceneIds();
  const auto originalTree = treeState();
  const auto target = viewport->viewTarget();
  const auto distance = viewport->viewDistance();
  const auto rotation = viewport->modelRotation();
  const auto translation = viewport->modelTranslation();
  const auto scale = viewport->modelScale();
  const auto angles = viewport->viewOrbitAngles();
  const auto orthographic = viewport->orthographicProjection();
  const auto projectName = document->projectName();
  const auto activeId = viewport->activeSceneId();
  const auto modelCount = viewport->sceneObjectCount();
  // Keep a real filesystem row selected while the appearance schedule runs.
  // This is a test-owned dialog/fixture; no user file or system clock is changed.
  QFileDialog appearanceFiles(&window);
  appearanceFiles.setOption(QFileDialog::DontUseNativeDialog);
  appearanceFiles.setWindowModality(Qt::NonModal);
  appearanceFiles.setFileMode(QFileDialog::ExistingFile);
  appearanceFiles.setViewMode(QFileDialog::Detail);
  appearanceFiles.setNameFilter(QStringLiteral("PLY (*.ply)"));
  appearanceFiles.setDirectory(temporary.path());
  appearanceFiles.selectFile(ply.fileName());
  appearanceFiles.resize(680, 460);
  appearanceFiles.show();
  auto *appearanceFileTree = appearanceFiles.findChild<QTreeView *>(QStringLiteral("treeView"));
  auto *appearanceFileName = appearanceFiles.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
  QModelIndex appearanceFileIndex;
  check(appearanceFileTree && appearanceFileName && waitUntil([&] {
    for (int row = 0; row < appearanceFileTree->model()->rowCount(appearanceFileTree->rootIndex()); ++row) {
      const auto candidate = appearanceFileTree->model()->index(row, 0, appearanceFileTree->rootIndex());
      if (candidate.data().toString() == QFileInfo(ply.fileName()).fileName()) {
        appearanceFileIndex = candidate;
        return true;
      }
    }
    return false;
  }), "automatic appearance starts with a loaded Unicode file selection fixture");
  if (appearanceFileTree && appearanceFileIndex.isValid())
    appearanceFileTree->selectionModel()->setCurrentIndex(appearanceFileIndex,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  QApplication::processEvents();
  const auto appearanceSelectedRows = [&] {
    QStringList result;
    if (appearanceFileTree && appearanceFileTree->selectionModel())
      for (const auto &row : appearanceFileTree->selectionModel()->selectedRows(0))
        result.append(row.data().toString());
    result.sort();
    return result;
  };
  const QStringList appearanceSelection = appearanceSelectedRows();
  const QStringList appearanceSelectedFiles = appearanceFiles.selectedFiles();
  const QString appearanceEnteredName = appearanceFileName ? appearanceFileName->text() : QString();
  const QString appearanceDirectory = appearanceFiles.directory().absolutePath();
  const QString appearanceFilter = appearanceFiles.selectedNameFilter();
  check(!appearanceSelection.isEmpty() && appearanceSelectedFiles.contains(ply.fileName()),
        "appearance preservation checks own a real selected source file");
  appearanceFiles.hide();
  // Window controls are installed lazily when a dialog is polished. Warm
  // existing dialogs before asserting that language changes add no actions.
  for (QDialog *dialog : {static_cast<QDialog *>(&training), static_cast<QDialog *>(&import),
       static_cast<QDialog *>(&meshing),
       static_cast<QDialog *>(&reconstruction), static_cast<QDialog *>(&namedImport),
       static_cast<QDialog *>(&box), static_cast<QDialog *>(&exportDialog),
       static_cast<QDialog *>(&spzDialog)}) dialog->ensurePolished();
  QApplication::processEvents();
  const auto actionCount = window.findChildren<QAction *>().size();

  // The test owns a real child process. Installed-package QA may explicitly
  // supply the same fixture without shipping test helpers in the package.
  const QString fixture = qEnvironmentVariable("GSW_PROCESS_OUTPUT_FIXTURE",
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("gsw_process_output_fixture.exe")));
  const bool testProcess = QFileInfo::exists(fixture);
  if (testProcess) check(supervisor->start(name, fixture, {QStringLiteral("tree-child")}), "start test worker");
  monitor->beginTraining(name, QStringLiteral("3dgs"), 30000);
  WorkerStatus status;
  status.state = QStringLiteral("running");
  status.stage = QStringLiteral("train");
  status.iteration = 15000;
  status.totalIterations = 30000;
  status.progressPercent = 50;
  status.loss = 0.125;
  status.psnr = 27.5;
  status.gaussianCount = 100000;
  status.densityGuardIteration = 3100;
  status.densityGuardDeferred = 3300;
  status.reconstructionQuality = QStringLiteral("accepted");
  status.reconstructionViews = 9;
  status.reconstructionInputs = 9;
  status.reconstructionPoints = 2399;
  status.trainingSummary = {{"version", 1}, {"phase", "loaded"}, {"backend", "3dgs"},
      {"quality", "original_quality"}, {"iterations", 30000}, {"resolution", 1}, {"optimizer", "default"},
      {"densifyUntil", 22000}, {"densificationInterval", 80}, {"densifyGradient", .00012},
      {"antialiasing", true}, {"exposureCompensation", true}, {"trainImageCount", 9},
      {"trainDimensionKinds", 1}, {"trainDimensions", QJsonArray{QJsonValue(QJsonArray{1928, 1084, 9})}},
      {"trainPixels", qint64(1928) * 1084 * 9}};
  monitor->updateStatus(status);
  emit supervisor->workerStatusReady(status);
  TrainingMonitorWidget surfelMonitor;
  surfelMonitor.beginTraining(name, QStringLiteral("2dgs"), 30000);
  WorkerStatus surfelStatus = status;
  surfelStatus.trainingSummary["backend"] = QStringLiteral("2dgs");
  surfelStatus.trainingSummary["optimizer"] = QStringLiteral("adam");
  surfelStatus.trainingSummary["resolution"] = 16;
  surfelStatus.trainingSummary["depthRatio"] = 0.0;
  surfelStatus.trainingSummary["trainDimensions"] = QJsonArray{QJsonValue(QJsonArray{16, 8, 9})};
  surfelStatus.trainingSummary["trainPixels"] = 16 * 8 * 9;
  surfelMonitor.updateStatus(surfelStatus);
  auto configuredStatus = surfelStatus;
  configuredStatus.trainingSummary["phase"] = QStringLiteral("configured");
  surfelMonitor.updateStatus(configuredStatus);
  // Partial/old-worker progress must not erase the most recent loaded report.
  WorkerStatus legacyStatus; legacyStatus.stage = QStringLiteral("train");
  surfelMonitor.updateStatus(legacyStatus);
  auto *toolbar = window.findChild<QToolBar *>();
  if (toolbar) toolbar->hide();
  auto *progress = monitor->findChild<QProgressBar *>();
  const auto sampleCount = monitor->telemetry().samples().size();
  auto *tabs = qobject_cast<QTabWidget *>(monitor->parentWidget()->parentWidget());
  if (tabs) tabs->setCurrentWidget(monitor);
  // Fail safely if a future regression restores the blocking restart notice.
  QTimer dismissUnexpectedNotice;
  QObject::connect(&dismissUnexpectedNotice, &QTimer::timeout, &window, [&] {
    if (auto *notice = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      check(false, "language selection must not open a restart dialog");
      notice->reject();
    }
  });
  dismissUnexpectedNotice.start(10);
  auto *shMenu = window.findChild<QMenu *>(QStringLiteral("shDisplayMenu"));
  auto *shAction = window.findChild<QAction *>(QStringLiteral("shDegree2Action"));
  if (!shMenu || !shAction) return false;
  const int savedShDegree = viewport->maximumShDegree();
  shAction->trigger();
  int presentationSceneLoads = 0;
  const auto presentationLoadConnection = QObject::connect(viewport, &NativeViewport::sceneLoadStarted,
      &window, [&](const QString &) { ++presentationSceneLoads; });
  const auto checkThemeStatePreserved = [&] {
    check(viewport->scenePath() == ply.fileName() && viewport->activeSceneId() == activeId &&
          viewport->sceneObjectCount() == modelCount && viewport->selectedSceneIds() == selection &&
          viewport->modelTranslation() == translation && viewport->modelRotation() == rotation &&
          viewport->modelScale() == scale && viewport->viewTarget() == target &&
          viewport->viewDistance() == distance && viewport->viewOrbitAngles() == angles &&
          viewport->orthographicProjection() == orthographic && treeState() == originalTree &&
          presentationSceneLoads == 0,
          "manual/automatic appearance preserves models, transforms, camera, selection and scene residency");
    check(monitor->telemetry().samples().size() == sampleCount && progress->value() == 50 &&
          surfelMonitor.telemetry().iteration() == surfelStatus.iteration &&
          training.configuration().iterations == 12345 &&
          meshing.configuration().value(QStringLiteral("mode")).toString() == QStringLiteral("gs2mesh"),
          "manual/automatic appearance preserves both trainers, mesh settings and recorded telemetry");
    if (testProcess) check(supervisor->isRunning() && supervisor->activeTask() == name,
                          "manual/automatic appearance does not interrupt processing");
    check(appearanceFileName && appearanceFileName->text() == appearanceEnteredName &&
          appearanceFiles.selectedFiles() == appearanceSelectedFiles &&
          appearanceFiles.directory().absolutePath() == appearanceDirectory &&
          appearanceFiles.selectedNameFilter() == appearanceFilter &&
          appearanceSelectedRows() == appearanceSelection,
          "manual/automatic appearance preserves selected files, filename, format and directory");
  };
  const auto checkViewportTheme = [&](const bool light) {
    const QImage frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_ARGB32);
    bool opaque = !frame.isNull();
    for (int y = 0; y < frame.height() && opaque; ++y) {
      const auto *pixels = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
      for (int x = 0; x < frame.width(); ++x)
        if (qAlpha(pixels[x]) != 255) { opaque = false; break; }
    }
    check(opaque, "manual/automatic viewport stays opaque through overlay alpha blending");
    viewport->makeCurrent();
    GLfloat clearColor[4] = {};
    if (viewport->context())
      viewport->context()->functions()->glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    viewport->doneCurrent();
    const float expectedClear[4] = {light ? 1.0F : .047F,
        light ? 1.0F : .051F, light ? 1.0F : .055F, 1.0F};
    for (int channel = 0; channel < 4; ++channel)
      check(std::abs(clearColor[channel] - expectedClear[channel]) < .002F,
            "manual/automatic appearance reaches the actual OpenGL viewport background");
  };
  const std::pair<QTime, UiTheme> themeBoundarySamples[] = {
      {QTime(5, 59, 59, 999), UiTheme::Dark},
      {QTime(6, 0), UiTheme::Light},
      {QTime(17, 59, 59, 999), UiTheme::Light},
      {QTime(18, 0), UiTheme::Dark},
      {QTime(23, 59, 59, 999), UiTheme::Dark},
      {QTime(0, 0), UiTheme::Dark}};
  lightTheme->trigger();
  for (int step = 1; step <= 6; ++step) {
    const int next = (index + step) % 3;
    const QString language = AppLanguage::supported()[next];
    languageMenu->actions()[next]->trigger();
    QApplication::processEvents();
    check(AppLanguage::current() == language && AppLanguage::saved() == language, "immediate persisted language");
    check(AppTheme::currentTheme() == UiTheme::Light && AppTheme::currentThemeMode() == UiThemeMode::Light &&
          lightTheme->isChecked() && !darkTheme->isChecked() && !automaticTheme->isChecked() &&
          !automaticThemeTimer->isActive(), "live language switching preserves the selected manual theme");
    check(appearanceMenu->title() == QCoreApplication::translate("Workbench", "外观") &&
          automaticTheme->text() == QCoreApplication::translate("Workbench", "自动（按时间）") &&
          lightTheme->text() == QCoreApplication::translate("Workbench", "浅色（白天）") &&
          darkTheme->text() == QCoreApplication::translate("Workbench", "深色（黑夜）"),
          "appearance menu switches language immediately");
    check(automaticTheme->toolTip() == QCoreApplication::translate("Workbench",
          "按本机时间自动切换：06:00 至 18:00 前为浅色，其余时间为深色（午夜保持深色）；运行中自动更新并记住选择"),
          "automatic appearance schedule description switches language immediately");
    check(lightTheme->toolTip() == QCoreApplication::translate("Workbench",
          "使用白色工作区、灰色工具栏、深色文字与统一浅灰面板及弹窗边界，适合白天观察；立即生效并记住选择"),
          "high-contrast light appearance description switches language immediately");
    const auto themeFont = qApp->font();
    const auto themeScale = qApp->property("gswUiScalePercent");
    const QString effectiveBeforeTheme = monitor->findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"))->text();
    for (QAction *action : {darkTheme, lightTheme}) {
      action->trigger();
      QApplication::processEvents();
      const bool light = action == lightTheme;
      check(AppTheme::currentTheme() == (light ? UiTheme::Light : UiTheme::Dark) &&
            AppTheme::currentThemeMode() == (light ? UiThemeMode::Light : UiThemeMode::Dark) &&
            AppTheme::loadTheme() == AppTheme::currentTheme() &&
            action->isChecked() && !automaticTheme->isChecked() && !automaticThemeTimer->isActive() &&
            QSettings().value(QStringLiteral("ui/theme")).toString() ==
                action->data().toString(), "theme changes and persists immediately through the real menu");
      check((qApp->palette().color(QPalette::Window).lightness() > 200) == light &&
            qApp->font() == themeFont && qApp->property("gswUiScalePercent") == themeScale,
            "theme updates palette without changing language fonts or scale");
      for (const auto &[time, scheduledTheme] : themeBoundarySamples) {
        Q_UNUSED(scheduledTheme);
        check(!AppTheme::refreshAutomaticTheme(*qApp, time) &&
              AppTheme::currentTheme() == (light ? UiTheme::Light : UiTheme::Dark) &&
              AppTheme::loadTheme(time) == AppTheme::currentTheme() &&
              AppTheme::currentThemeMode() == (light ? UiThemeMode::Light : UiThemeMode::Dark) &&
              action->isChecked() && !automaticTheme->isChecked() && !automaticThemeTimer->isActive(),
              "manual light/dark overrides ignore dawn, dusk and midnight clock samples");
      }
      if (step == 1) {
        checkViewportTheme(light);
      }
      checkThemeStatePreserved();
      check(monitor->findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"))->text() == effectiveBeforeTheme,
            "manual appearance preserves displayed effective training parameters");
    }
    automaticTheme->trigger();
    QApplication::processEvents();
    check(AppTheme::currentThemeMode() == UiThemeMode::Automatic &&
          AppTheme::loadThemeMode() == UiThemeMode::Automatic && automaticTheme->isChecked() &&
          !lightTheme->isChecked() && !darkTheme->isChecked() && automaticThemeTimer->isActive() &&
          automaticThemeTimer->interval() >= 1 && automaticThemeTimer->interval() <= 30000 &&
          QSettings().value(QStringLiteral("ui/theme")).toString() == QStringLiteral("auto"),
          "automatic appearance persists its mode and activates the bounded schedule through the real menu");
    languageMenu->actions()[(next + 1) % 3]->trigger();
    QApplication::processEvents();
    check(AppTheme::currentThemeMode() == UiThemeMode::Automatic && automaticThemeTimer->isActive() &&
          automaticTheme->isChecked() && !lightTheme->isChecked() && !darkTheme->isChecked() &&
          automaticTheme->text() == QCoreApplication::translate("Workbench", "自动（按时间）") &&
          automaticTheme->toolTip() == QCoreApplication::translate("Workbench",
              "按本机时间自动切换：06:00 至 18:00 前为浅色，其余时间为深色（午夜保持深色）；运行中自动更新并记住选择") &&
          QSettings().value(QStringLiteral("ui/theme")).toString() == QStringLiteral("auto"),
          "live language changes preserve automatic selection, timer and translated schedule text");
    languageMenu->actions()[next]->trigger();
    QApplication::processEvents();
    checkThemeStatePreserved();
    // Inject wall-clock samples through the public theme seam, without altering
    // the computer's clock or waiting until evening. The same live state gates
    // as the manual switches cover both trainers and every mesh mode.
    // Keep a real 06:00/18:00 tick from racing these deterministic samples;
    // the actual timeout callback below must rearm this same timer afterwards.
    appearanceFiles.show();
    QApplication::processEvents();
    automaticThemeTimer->stop();
    AppTheme::applyThemeMode(*qApp, UiThemeMode::Automatic, false, QTime(5, 59, 59, 998));
    check(AppTheme::currentTheme() == UiTheme::Dark, "automatic appearance before 06:00 starts dark");
    for (const auto &[time, expected] : themeBoundarySamples) {
      check(AppTheme::themeForTime(time) == expected,
            "independent dawn/dusk/midnight theme schedule expectations");
      const bool changes = AppTheme::currentTheme() != expected;
      check(AppTheme::refreshAutomaticTheme(*qApp, time) == changes,
            "automatic appearance changes only when crossing a day/night boundary");
      QApplication::processEvents();
      const bool light = expected == UiTheme::Light;
      check(AppTheme::currentTheme() == expected && AppTheme::currentThemeMode() == UiThemeMode::Automatic &&
            automaticTheme->isChecked() && !lightTheme->isChecked() && !darkTheme->isChecked() &&
            QSettings().value(QStringLiteral("ui/theme")).toString() == QStringLiteral("auto") &&
            (qApp->palette().color(QPalette::Window).lightness() > 200) == light &&
            qApp->font() == themeFont && qApp->property("gswUiScalePercent") == themeScale,
            "06:00/18:00 switches and unchanged midnight retain automatic policy, fonts, scale and menu selection");
      checkViewportTheme(light);
      checkThemeStatePreserved();
      check(monitor->findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"))->text() == effectiveBeforeTheme,
            "scheduled appearance preserves displayed effective training parameters");
    }
    appearanceFiles.hide();
    QApplication::processEvents();
    // Force the real schedule callback to correct an intentionally stale
    // palette to today's clock, exercising MainWindow's presentation path.
    const UiTheme realTheme = AppTheme::themeForTime(QTime::currentTime());
    AppTheme::refreshAutomaticTheme(*qApp, realTheme == UiTheme::Light ? QTime(18, 0) : QTime(6, 0));
    check(QMetaObject::invokeMethod(automaticThemeTimer, "timeout", Qt::DirectConnection),
          "invoke the real automatic appearance timer callback");
    QApplication::processEvents();
    const QTime actualLocalTime = QTime::currentTime();
    const bool actualDaytime = actualLocalTime.hour() >= 6 && actualLocalTime.hour() < 18;
    const UiTheme refreshedTheme = actualDaytime ? UiTheme::Light : UiTheme::Dark;
    check(AppTheme::currentTheme() == refreshedTheme && automaticThemeTimer->isActive() &&
          AppTheme::currentThemeMode() == UiThemeMode::Automatic && automaticTheme->isChecked() &&
          QSettings().value(QStringLiteral("ui/theme")).toString() == QStringLiteral("auto"),
          "real timer corrects stale palettes and rearms without overwriting automatic preference");
    check((qApp->palette().color(QPalette::Window).lightness() > 200) == actualDaytime,
          "real timer palette follows independent local 06:00-inclusive/18:00-exclusive expectations");
    checkViewportTheme(refreshedTheme == UiTheme::Light);
    checkThemeStatePreserved();
    if (step == 3) {
      qInfo().noquote() << QStringLiteral("Automatic theme smoke: localTime=%1 mode=auto resolved=%2")
          .arg(actualLocalTime.toString(QStringLiteral("HH:mm:ss")),
               AppTheme::currentTheme() == UiTheme::Light ? QStringLiteral("light") : QStringLiteral("dark"));
      if (!screenshotDirectory.isEmpty()) {
        check(QDir().mkpath(screenshotDirectory), "prepare real-clock automatic appearance screenshot directory");
        const QString prefix = locale + QStringLiteral("-theme-auto-real");
        check(window.grab().save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral(".png"))),
              "real-clock automatic appearance screenshot before manual cleanup");
        check(viewport->grabFramebuffer().save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral("-viewport.png"))),
              "real-clock automatic viewport screenshot before manual cleanup");
      }
    }
    lightTheme->trigger();
    check(!automaticThemeTimer->isActive(), "returning to a manual theme stops scheduled checks");
    const auto *densityWarning = monitor->findChild<QLabel *>(QStringLiteral("densityGuardWarning"));
    check(densityWarning && !densityWarning->isHidden() && densityWarning->text() ==
          QCoreApplication::translate("Workbench", "过度裁剪保护：第 %1 次迭代暂缓删除 %2 个高斯。请检查拍摄覆盖与重建尺度；数量不代表几何质量。")
              .arg(QLocale().toString(3100), QLocale().toString(3300)),
          "density protection warning translates live without resetting training state");
    const auto *effective = monitor->findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"));
    const QString expectedEffective = QCoreApplication::translate("Workbench", "生效参数：%1 · %2 次迭代 · %3 · 优化器 %4。")
            .arg(QStringLiteral("3DGS"), QLocale().toString(30000),
                 QCoreApplication::translate("Workbench", "1/%1 分辨率").arg(1), QStringLiteral("Adam")) + QLatin1Char('\n') +
        QCoreApplication::translate("Workbench", "实际训练图像：%1 张 · %2 · 总计 %3 MP。")
            .arg(QLocale().toString(9), QCoreApplication::translate("Workbench", "%1 × %2 px（%3 张）")
                .arg(QLocale().toString(1928), QLocale().toString(1084), QLocale().toString(9)),
                 QLocale().toString(1928.0 * 1084 * 9 / 1000000, 'f', 2));
    if (effective && effective->text() != expectedEffective)
      qWarning() << "Effective parameters actual/expected:" << effective->text() << expectedEffective;
    check(effective && !effective->isHidden() && effective->text() == expectedEffective,
        "actual training dimensions and effective Adam fallback translate without resetting samples");
    const auto *surfelEffective = surfelMonitor.findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"));
    check(surfelEffective && !surfelEffective->isHidden() &&
        surfelEffective->text().contains(QCoreApplication::translate("Workbench", "目标宽度 %1 px").arg(QLocale().toString(16))) &&
        surfelEffective->text().contains(QCoreApplication::translate("Workbench", "%1 × %2 px（%3 张）")
            .arg(QLocale().toString(16), QLocale().toString(8), QLocale().toString(9))) &&
        surfelEffective->toolTip().contains(QCoreApplication::translate("Workbench", "深度混合比例：%1").arg(QLocale().toString(0.0, 'g', 6))),
        "2DGS target-width semantics and loaded metadata survive language/partial/configured updates");
    check(saveAction->text() == saveTexts[next], "existing action updates immediately");
    const auto *pause = window.findChild<QAction *>(QStringLiteral("pauseTrainingAction"));
    const auto *resume = window.findChild<QAction *>(QStringLiteral("resumeTrainingAction"));
    check(pause && resume && pause->text() == QCoreApplication::translate("Workbench", "暂停训练") &&
          resume->text() == QCoreApplication::translate("Workbench", "继续训练"), "training controls translate live");
    check(shMenu->title() == QCoreApplication::translate("Workbench", "球谐显示") &&
          shAction->text() == QCoreApplication::translate("Workbench", "SH 2 阶") &&
          shAction->isChecked() && viewport->maximumShDegree() == 2,
          "SH display translated live without resetting quality preference");
    check(exportAction->text() == QCoreApplication::translate("Workbench", "导出模型...") &&
          exportDialog.windowTitle() == QCoreApplication::translate("Workbench", "导出模型"), "export action and dialog translated live");
    check(exportDialog.options().format == ModelExportFormat::Glb && exportDialog.options().destinationPath == exportPath &&
          exportDialog.options().applyTransform, "export settings survive language changes");
    check(meshing.windowTitle() == QCoreApplication::translate("Workbench", "生成网格") &&
          meshing.configuration().value(QStringLiteral("mode")).toString() == QStringLiteral("gs2mesh"),
          "mesh dialog translates live without changing backend");
    check(spzQuality->currentText() == QCoreApplication::translate("Workbench", "高精度（较大文件）") &&
          spzDialog.options().spzQuality == 2 && spzDialog.options().spzVersion == 3 &&
          spzDialog.options().spzMaximumShDegree == 2 && !spzDialog.options().applyTransform,
          "SPZ options translate immediately without changing export parameters");
    check(trackball->text() == trackballTexts[next] && trackball->isChecked() && viewport->observationTrackballVisible(),
          "observation trackball translated and state retained");
    check(lockTools->text() == lockTexts[next] && lockTools->isChecked() && viewport->editToolsLocked() &&
          QSettings().value(QStringLiteral("view/editToolsLocked")).toBool(),
          "lock label switches language without unlocking tools or changing preference");
    check(QCoreApplication::translate("Workbench", "点云") == pointTexts[next], "updated terminology");
    check(languageMenu->actions()[next]->isChecked(), "language action checked");
    check(window.findChildren<QAction *>().size() == actionCount, "no duplicate actions");
    if (toolbar) check(toolbar->isHidden(), "user-hidden toolbar retained");
    for (auto *object : window.findChildren<QObject *>()) {
      // Check the source-key bindings, not reverse matching displayed text.
      for (const auto &property : object->dynamicPropertyNames()) {
        if (!property.startsWith("gswTranslation_")) continue;
        const QByteArray targetProperty = property.mid(sizeof("gswTranslation_") - 1);
        // Dynamic status/title labels intentionally override their idle text.
        const QByteArray source = object->property(property.constData()).toByteArray();
        if (object == &window || (targetProperty == "text" &&
            (source == "尚未开始训练" || source == "空闲" || source == "未打开工程" ||
             source == "选择 0 | 删除 0" || source == "点预览 | 未载入场景 | FPS — (— ms)" ||
             source == "正在读取 PLY 场景"))) continue;
        check(object->property(targetProperty.constData()).toString() ==
              QCoreApplication::translate("Workbench", source.constData()), "live property binding");
      }
    }
    check(import.request().sceneName == name && training.configuration().outputScene == editedConfiguration.outputScene,
          "user names are not translated");
    check(training.configuration().iterations == 12345 && training.configuration().quality == editedConfiguration.quality &&
          training.configuration().backend == configuration.backend &&
          training.configuration().resolution == editedConfiguration.resolution, "training parameters retained");
    if (resolutionCombo) check(resolutionCombo->currentText() ==
        QCoreApplication::translate("Workbench", "原始分辨率（1:1）"), "original-resolution caption switches live");
    if (qualityCombo) check(qualityCombo->currentText() ==
        QCoreApplication::translate("Workbench", "最高精度（原始分辨率）"), "highest-fidelity preset switches live");
    check(inputSummary && inputSummary->text().contains(
        QCoreApplication::translate("Workbench", "%1 × %2 → %3 × %4 px（%5 张）")
            .arg(QLocale().toString(1928), QLocale().toString(1084), QLocale().toString(1928),
                 QLocale().toString(1084), QLocale().toString(1))) &&
        inputSummary->text().contains(QDir::toNativeSeparators(QDir(trainingImages.path()).filePath(QStringLiteral("images")))),
        "cached input header summary translates live without changing dimensions or user paths");
    check(reconstruction.configuration().cameraModel == reconstructionConfig.cameraModel &&
          reconstruction.configuration().featureMaxNumFeatures == reconstructionConfig.featureMaxNumFeatures,
          "reconstruction parameters retained");
    check(import.windowTitle() == QCoreApplication::translate("Workbench", "添加照片与视频") &&
          training.windowTitle() == QCoreApplication::translate("Workbench", "训练设置"), "open dialogs translated");
    check(import.findChild<QLabel *>(QStringLiteral("datasetImportSummaryLabel"))->text() ==
          QCoreApplication::translate("Workbench", "尚未添加媒体来源。"), "dynamic dialog summary");
    check(viewport == window.centralWidget() && viewport->scenePath() == ply.fileName() &&
          viewport->activeSceneId() == activeId && viewport->sceneObjectCount() == modelCount,
          "model and viewport not reloaded");
    check(viewport->selectedSceneIds() == selection && treeState() == originalTree, "selection and tree state retained");
    check(viewport->modelTranslation() == translation && viewport->modelRotation() == rotation &&
          viewport->modelScale() == scale, "model transforms retained");
    check(viewport->viewTarget() == target && viewport->viewDistance() == distance &&
          viewport->viewOrbitAngles() == angles &&
          viewport->orthographicProjection() == orthographic, "camera retained");
    check(document->projectName() == projectName && nameLabel->text() == projectName, "project name retained");
    check(monitor->telemetry().samples().size() == sampleCount && monitor->telemetry().iteration() == status.iteration &&
          monitor->telemetry().loss() == status.loss && progress->value() == 50, "telemetry and progress retained");
    check(monitor->findChild<QLabel *>(QStringLiteral("reconstructionQualityLabel"))->text() ==
          QCoreApplication::translate("Workbench", "重建检查：相机 %1/%2 · 有效稀疏点 %3 · %4。高斯数量和训练 PSNR 不代表新视角质量。")
              .arg(QLocale().toString(9), QLocale().toString(9), QLocale().toString(2399),
                   QCoreApplication::translate("Workbench", "最低质量检查通过")), "reconstruction quality translated live without modifying samples");
    check(monitor->findChild<QLabel *>(QStringLiteral("statusWarn"))->text() ==
          QCoreApplication::translate("Workbench", "训练中"), "live training state caption");
    check(box.button(QMessageBox::Cancel)->text() ==
          QMessageBox(QMessageBox::Question, {}, {}, QMessageBox::Cancel).button(QMessageBox::Cancel)->text(),
          "existing Qt standard button retranslates");
    if (testProcess) {
      auto *table = window.findChild<QTableWidget *>();
      check(table && table->item(table->rowCount() - 1, 3)->text() ==
            QCoreApplication::translate("Workbench", "训练") + QStringLiteral(" · 50%"), "live worker stage caption");
    }
    if (tabs) check(tabs->currentWidget() == monitor, "active tab retained");
    if (testProcess) check(supervisor->isRunning() && supervisor->activeTask() == name, "worker not interrupted");
  }
  QObject::disconnect(presentationLoadConnection);
  check(presentationSceneLoads == 0, "theme and language changes do not reload scene data");
  dismissUnexpectedNotice.stop();
  restoreThemeMode();
  surfelMonitor.finishTraining(true, false);
  check(!surfelMonitor.findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"))->isHidden(),
        "finished training retains its loaded report");
  surfelMonitor.beginTraining(name, QStringLiteral("2dgs"), 10000);
  check(surfelMonitor.findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"))->isHidden(),
        "new/legacy training clears stale effective metadata");
  if (auto *restoreSh = window.findChild<QAction *>(QStringLiteral("shDegree%1Action").arg(savedShDegree)))
    restoreSh->trigger();
  check(!AppLanguage::apply(QStringLiteral("invalid")) && AppLanguage::current() == locale &&
        AppLanguage::saved() == locale, "invalid locale leaves current UI unchanged");
  // Resize a separate, owned monitor while exercising the production scale
  // actions on the isolated smoke-test window. Never re-ingest samples while
  // changing presentation. All three language CTests run both trainers.
  const auto settleLayout = [] {
    for (int pass = 0; pass < 4; ++pass) QApplication::processEvents();
  };
  auto *displaySettings = window.findChild<QMenu *>(QStringLiteral("displaySettingsMenu"));
  auto *automaticScale = window.findChild<QAction *>(QStringLiteral("autoUiScaleAction"));
  const auto scaleAction = [displaySettings](const int percent) -> QAction * {
    if (!displaySettings) return nullptr;
    for (auto *menu : displaySettings->findChildren<QMenu *>()) {
      for (auto *action : menu->actions()) {
        if (action->isCheckable() && action->data().toInt() == percent)
          return action;
      }
    }
    return nullptr;
  };
  const int retainedUiScale = qApp->property("gswUiScalePercent").toInt();
  const bool retainedAutomaticScale = automaticScale && automaticScale->isChecked();
  check(displaySettings && automaticScale && scaleAction(90) && scaleAction(150),
        "production application-scale actions are available");
  for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
    TrainingMonitorWidget responsive;
    const QString longName = name + QStringLiteral("\n<b>model</b>\t") + QChar(0x2028) + QString(240, QLatin1Char('W'));
    const QString fullTitle = backend.toUpper() + QStringLiteral(" · ") + longName;
    responsive.beginTraining(longName, backend, 30000);
    WorkerStatus responsiveStatus = status;
    responsiveStatus.trainingSummary["backend"] = backend;
    if (backend == QStringLiteral("2dgs")) {
      responsiveStatus.trainingSummary["optimizer"] = QStringLiteral("adam");
      responsiveStatus.trainingSummary["depthRatio"] = 0.0;
    }
    responsive.updateStatus(responsiveStatus);
    responsiveStatus.iteration = 15010;
    responsiveStatus.loss = 0.12;
    responsiveStatus.psnr = 27.6;
    responsive.updateStatus(responsiveStatus);
    const auto retainedSamples = responsive.telemetry().samples().size();
    const auto *effective = responsive.findChild<QLabel *>(QStringLiteral("trainingEffectiveParameters"));
    const QString retainedReport = effective->text();
    auto *title = responsive.findChild<QLabel *>(QStringLiteral("trainingTaskTitle"));
    auto *scroll = responsive.findChild<QScrollArea *>(QStringLiteral("trainingMonitorScroll"));
    auto *curves = responsive.findChild<QWidget *>(QStringLiteral("trainingCurves"));
    auto *responsiveProgress = responsive.findChild<QProgressBar *>();
    check(title && scroll && curves && responsiveProgress, "responsive monitor test objects");
    if (!title || !scroll || !curves || !responsiveProgress) continue;
    responsive.show();
    for (const int fontPercent : {90, 150, 90}) {
      // Use the production application-font/QSS path, not a local font
      // override that can be overridden by the stylesheet's font resolution.
      auto *action = scaleAction(fontPercent);
      if (!action) continue;
      action->trigger();
      responsive.resize(1400, 260);
      settleLayout();
      check(qAbs(curves->font().pointSizeF() - fontPercent / 10.0) < .01,
            "monitor matrix uses the actual application font scale");
      const int wideColumns = responsive.property("metricColumns").toInt();
      check(wideColumns >= 4, "wide monitor retains a compact metric row");
      for (const int width : {320, 520, 900}) {
        responsive.resize(width, 180);
        settleLayout();
        check(responsive.size() == QSize(width, 180), "monitor does not force the dock to grow");
        check(responsive.property("metricColumns").toInt() <= wideColumns &&
            (width != 320 || responsive.property("metricColumns").toInt() < wideColumns),
            "metric columns adapt to available width and font scale");
        check(scroll->horizontalScrollBar()->maximum() == 0,
            "normal narrow monitors do not require horizontal scrolling");
        check(curves->height() >= curves->fontMetrics().height() * 6 + 32,
            "curves preserve a readable font-scaled height");
        check(title->textFormat() == Qt::PlainText && title->toolTip() == Qt::convertFromPlainText(fullTitle) &&
            !title->text().contains(QLatin1Char('\n')) && !title->text().contains(QLatin1Char('\t')) &&
            title->fontMetrics().horizontalAdvance(title->text()) <= title->width(),
            "long user titles are plain text and elided with complete tooltips");
        for (int metric = 0; metric < 7; ++metric) {
          auto *value = responsive.findChild<QLabel *>(QStringLiteral("trainingMetricValue%1").arg(metric));
          check(value && !value->isHidden(), "all seven metrics survive reflow");
          if (!value) continue;
          check(qAbs(value->font().pointSizeF() - fontPercent / 10.0) < .01,
              "metric values follow the actual live font scale");
          check(value->width() >= value->fontMetrics().horizontalAdvance(value->text()),
              "metric values are not clipped");
          scroll->ensureWidgetVisible(value, 0, 0);
          settleLayout();
          const QRect visible(value->mapTo(scroll->viewport(), QPoint()), value->size());
          check(scroll->viewport()->rect().contains(visible.center()),
              "each metric is reachable through scrolling");
        }
        check(scroll->verticalScrollBar()->maximum() > 0,
            "short monitors scroll rather than crush report and curves");
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        settleLayout();
        const int bottom = curves->mapTo(scroll->viewport(), QPoint(0, curves->height())).y();
        check(bottom <= scroll->viewport()->height(), "curve bottom remains reachable");
        const int scrollPosition = scroll->verticalScrollBar()->value();
        WorkerStatus partial; partial.stage = QStringLiteral("train");
        responsive.updateStatus(partial);
        settleLayout();
        check(scroll->verticalScrollBar()->value() == scrollPosition,
            "ordinary telemetry refresh does not reset the scroll position");
        check(responsive.telemetry().samples().size() == retainedSamples &&
            responsive.telemetry().iteration() == responsiveStatus.iteration &&
            responsiveProgress->value() == 50 && effective->text() == retainedReport,
            "resize and font changes preserve telemetry and loaded parameters");
      }
      responsive.resize(1400, 260);
      settleLayout();
      check(responsive.property("metricColumns").toInt() == wideColumns &&
          scroll->horizontalScrollBar()->maximum() == 0,
          "wide layout recovers after narrowing and font-scale round trips");
    }
    if (!screenshotDirectory.isEmpty()) {
      QDir().mkpath(screenshotDirectory);
      responsive.resize(360, 260);
      scroll->verticalScrollBar()->setValue(0);
      settleLayout();
      check(responsive.grab().save(QDir(screenshotDirectory).filePath(
          locale + QStringLiteral("-monitor-narrow-") + backend + QStringLiteral(".png"))),
          "narrow monitor screenshot");
      scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
      settleLayout();
      check(responsive.grab().save(QDir(screenshotDirectory).filePath(
          locale + QStringLiteral("-monitor-curves-") + backend + QStringLiteral(".png"))),
          "scrolled monitor screenshot");
      responsive.resize(1400, 380);
      settleLayout();
      check(responsive.grab().save(QDir(screenshotDirectory).filePath(
          locale + QStringLiteral("-monitor-wide-") + backend + QStringLiteral(".png"))),
          "wide monitor screenshot");
    }
    responsive.hide();
  }
  // Forms share one scroll/reflow contract across both Gaussian trainers,
  // reconstruction, all mesh methods, media import and model export. Resize
  // existing edited dialogs; do not rebuild their controls or worker state.
  const QJsonObject retainedMeshForm = meshing.configuration();
  const QString retainedExportDestination = exportDialog.options().destinationPath;
  const int retainedTrainingIterations = training.configuration().iterations;
  const QString retainedTrainingBackend = training.configuration().backend;
  TrainingDialog twoDSettings(trainingImages.path(), name,
      QCoreApplication::applicationDirPath(), true, true, &window);
  if (auto *backend = twoDSettings.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo")))
    backend->setCurrentIndex(backend->findData(QStringLiteral("2dgs")));
  const int retainedTwoDIterations = twoDSettings.configuration().iterations;
  check(twoDSettings.configuration().backend == QStringLiteral("2dgs"),
        "responsive settings matrix covers the real 2DGS form too");
  for (const int fontPercent : {90, 150, 90}) {
    auto *action = scaleAction(fontPercent);
    if (!action) continue;
    action->trigger();
    settleLayout();
    for (QDialog *dialog : {static_cast<QDialog *>(&training),
                           static_cast<QDialog *>(&twoDSettings),
                           static_cast<QDialog *>(&reconstruction),
                           static_cast<QDialog *>(&meshing),
                           static_cast<QDialog *>(&import),
                           static_cast<QDialog *>(&exportDialog),
                           static_cast<QDialog *>(&spzDialog)}) {
      const bool visible = dialog->isVisible();
      const QSize retainedSize = dialog->size();
      dialog->showNormal();
      dialog->resize(680, 460);
      settleLayout();
      auto *scroll = dialog->findChild<QScrollArea *>(QStringLiteral("dialogBodyScroll"));
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      check(scroll && buttons, "settings dialog has scrolling body and independent action buttons");
      if (scroll && buttons) {
        check(!scroll->isAncestorOf(buttons) &&
              dialog->rect().contains(QRect(buttons->mapTo(dialog, QPoint()), buttons->size())),
              "small high-font settings dialog keeps all action buttons visible");
        if (scroll->horizontalScrollBar()->maximum() != 0)
          qCritical() << "Horizontal settings overflow:" << dialog->objectName()
                      << dialog->windowTitle() << fontPercent
                      << scroll->horizontalScrollBar()->maximum();
        check(scroll->horizontalScrollBar()->maximum() == 0,
              "settings dialog wraps long form rows instead of horizontal clipping");
        for (auto *form : scroll->findChildren<QFormLayout *>())
          check(form->rowWrapPolicy() == QFormLayout::WrapLongRows &&
                form->formAlignment().testFlag(Qt::AlignTop),
                "settings form wraps and remains top aligned on enlargement");
        for (auto *checkBox : scroll->findChildren<QCheckBox *>()) {
          if (!checkBox->isVisible()) continue;
          check(checkBox->height() >= checkBox->heightForWidth(checkBox->width()),
                "localized checkbox captions reserve enough height for every wrapped line");
        }
        for (auto *field : scroll->findChildren<QLineEdit *>()) {
          if (!field->isVisible()) continue;
          scroll->ensureWidgetVisible(field);
          settleLayout();
          check(scroll->viewport()->rect().contains(
              QRect(field->mapTo(scroll->viewport(), QPoint()), field->size()).center()),
              "every settings input remains reachable through scrolling");
        }
        scroll->verticalScrollBar()->setValue(0);
      }
      if (dialog == &import && scroll) {
        auto *sourceActions = dialog->findChild<QWidget *>(QStringLiteral("datasetImportSourceActions"));
        auto *sourceList = dialog->findChild<QListWidget *>(QStringLiteral("datasetImportSourceList"));
        check(sourceActions && sourceList && !scroll->isAncestorOf(sourceList),
              "media source list keeps its independent scrolling view");
        if (sourceActions) {
          for (const int width : {460, 1100, 460}) {
            dialog->resize(width, 460);
            settleLayout();
            const auto actions = sourceActions->findChildren<QPushButton *>();
            check(actions.size() == 4, "media source reflow preserves all four action buttons");
            check(sourceActions->height() >= sourceActions->minimumSizeHint().height(),
                  "media source actions reserve the complete reflowed row height");
            for (qsizetype i = 0; i < actions.size(); ++i) {
              const auto *button = actions.at(i);
              const QRect geometry(button->mapTo(sourceActions, QPoint()), button->size());
              check(sourceActions->rect().contains(geometry) &&
                    button->height() >= button->sizeHint().height(),
                    "each media source button remains fully visible after narrow-wide-narrow resizing");
              for (qsizetype j = i + 1; j < actions.size(); ++j)
                check(!geometry.intersects(QRect(actions.at(j)->mapTo(sourceActions, QPoint()),
                                                actions.at(j)->size())),
                      "reflowed media source buttons do not overlap");
            }
          }
          dialog->resize(680, 460);
          settleLayout();
        }
      }
      if (fontPercent == 150 && dialog == &training && !screenshotDirectory.isEmpty()) {
        QDir().mkpath(screenshotDirectory);
        check(dialog->grab().save(QDir(screenshotDirectory).filePath(
            locale + QStringLiteral("-settings-150-small.png"))), "high-font settings screenshot");
      }
      if (fontPercent == 150 && dialog == &twoDSettings && !screenshotDirectory.isEmpty()) {
        QDir().mkpath(screenshotDirectory);
        check(dialog->grab().save(QDir(screenshotDirectory).filePath(
            locale + QStringLiteral("-settings-2dgs-150-small.png"))), "high-font 2DGS settings screenshot");
      }
      dialog->showMaximized();
      settleLayout();
      if (buttons)
        check(dialog->rect().contains(QRect(buttons->mapTo(dialog, QPoint()), buttons->size())),
              "maximized settings dialog keeps action buttons reachable");
      dialog->showNormal();
      dialog->resize(retainedSize);
      if (!visible) dialog->hide();
      settleLayout();
    }
    check(training.configuration().iterations == retainedTrainingIterations &&
          training.configuration().backend == retainedTrainingBackend &&
          twoDSettings.configuration().backend == QStringLiteral("2dgs") &&
          twoDSettings.configuration().iterations == retainedTwoDIterations &&
          reconstruction.configuration().cameraModel == reconstructionConfig.cameraModel &&
          meshing.configuration() == retainedMeshForm &&
          exportDialog.options().destinationPath == retainedExportDestination,
          "dialog resizing and font changes retain edited generation and export settings");
  }
  if (retainedAutomaticScale && automaticScale) {
    automaticScale->trigger();
  } else {
    auto *action = scaleAction(retainedUiScale);
    check(action != nullptr, "original manual scale can be restored");
    if (action) action->trigger();
  }
  settleLayout();
  check(monitor->telemetry().samples().size() == sampleCount &&
        monitor->telemetry().iteration() == status.iteration &&
        viewport->scenePath() == ply.fileName() && viewport->activeSceneId() == activeId,
        "application-scale changes preserve the live monitor and loaded model");
  if (testProcess) check(supervisor->isRunning() && supervisor->activeTask() == name,
                        "application-scale changes do not interrupt the worker");
  if (!screenshotDirectory.isEmpty()) {
    // Expand only the isolated QA layout to inspect translated monitor labels.
    if (tabs) {
      if (auto *dock = qobject_cast<QDockWidget *>(tabs->parentWidget()))
        window.resizeDocks({dock}, {360}, Qt::Vertical);
    }
    if (toolbar) toolbar->show();
    (void)viewport->focusModel();
    QApplication::processEvents();
    QDir().mkpath(screenshotDirectory);
    check(window.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral(".png"))), "UI screenshot");
    check(viewport->grabFramebuffer().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-viewport.png"))), "trackball screenshot");
    for (QAction *action : {lightTheme, darkTheme}) {
      action->trigger();
      settleLayout();
      const QString prefix = locale + QStringLiteral("-theme-") + action->data().toString();
      check(window.grab().save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral(".png"))),
            "installed day/night UI screenshot");
      const QImage frame = viewport->grabFramebuffer();
      check(!frame.isNull() && frame.save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral("-viewport.png"))),
            "installed day/night viewport screenshot");
      exportDialog.show(); exportDialog.adjustSize(); settleLayout();
      check(exportDialog.grab().save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral("-export.png"))),
            "installed day/night export-dialog screenshot");
      exportDialog.hide();
    }
    restoreThemeMode();
    if (backendCombo) {
      backendCombo->setCurrentIndex(1);
      backendCombo->setCurrentIndex(0);
    }
    if (qualityCombo) qualityCombo->setCurrentIndex(qualityCombo->findData(QStringLiteral("quick")));
    training.show(); training.adjustSize(); QApplication::processEvents();
    check(training.configuration().iterations == 10000 && training.configuration().resolution == 2,
          "3DGS displayed preview values match backend defaults");
    check(training.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-training-3dgs.png"))), "3DGS training values screenshot");
    if (backendCombo) backendCombo->setCurrentIndex(1);
    training.show(); training.adjustSize(); QApplication::processEvents();
    check(training.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-training.png"))), "training capabilities screenshot");
    if (backendCombo && qualityCombo) {
      qualityCombo->setCurrentIndex(qualityCombo->findData(QStringLiteral("original_quality")));
      for (const QString &backend : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) {
        backendCombo->setCurrentIndex(backendCombo->findData(backend));
        training.adjustSize(); QApplication::processEvents();
        check(training.configuration().iterations == 30000 && training.configuration().resolution == 1,
              "original-resolution displayed values match backend defaults");
        check(training.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-training-original-") + backend + QStringLiteral(".png"))),
              "original-resolution training screenshot");
      }
    }
    training.hide();
    meshing.show(); meshing.adjustSize(); QApplication::processEvents();
    check(meshing.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-meshing.png"))), "mesh dialog screenshot");
    meshing.hide();
    exportDialog.show(); exportDialog.adjustSize(); QApplication::processEvents();
    check(exportDialog.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-export.png"))), "export dialog screenshot");
    exportDialog.hide();
    spzDialog.show(); spzDialog.adjustSize(); QApplication::processEvents();
    check(spzDialog.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-spz.png"))), "SPZ dialog screenshot");
    spzDialog.hide();
    // Inspect the same shallow perspective grid as a normal daytime session,
    // after the live-state assertions above. Only the isolated QA view changes.
    viewport->resetCamera();
    for (QAction *action : {lightTheme, darkTheme}) {
      action->trigger();
      settleLayout();
      const QString prefix = locale + QStringLiteral("-perspective-") + action->data().toString();
      check(window.grab().save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral(".png"))),
            "installed perspective theme UI screenshot");
      check(viewport->grabFramebuffer().save(QDir(screenshotDirectory).filePath(prefix + QStringLiteral("-viewport.png"))),
            "installed perspective theme grid screenshot");
    }
    restoreThemeMode();
  }
  check(runListInteractionSmokeTest(window), "all item-list interactions");
  if (testProcess) supervisor->shutdown();
  check(runWindowUiSmokeTest(window), "native window and dock controls");
  qInfo().noquote() << "Language smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
