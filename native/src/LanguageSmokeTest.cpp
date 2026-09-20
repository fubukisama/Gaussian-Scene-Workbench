#include "LanguageSmokeTest.h"
#include "MultiItemList.h"
#include "ExternalBackupStore.h"
#include "AppLanguage.h"
#include "MainWindow.h"
#include "TrainingDialog.h"
#include "DatasetImportDialog.h"
#include "ReconstructionDialog.h"
#include "TrainingMonitorWidget.h"
#include "WorkspaceDocument.h"
#include "ManagedName.h"
#include "ModelExportDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include "WindowUiSmokeTest.h"
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QPushButton>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTimer>

#include <functional>

namespace gsw {
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
  for (int i = 0; i < 3; ++i) {
    check(languageMenu->actions()[i]->data().toString() == AppLanguage::supported()[i], "stable locale identifiers");
    check(languageMenu->actions()[i]->text() == AppLanguage::displayName(AppLanguage::supported()[i]), "autonyms");
  }
  const QString name = QStringLiteral("古墳 / 日本語:*?\"<>|📷 ");
  TrainingDialog training(QCoreApplication::applicationDirPath(), name,
                          QCoreApplication::applicationDirPath(), true, true, &window);
  const auto configuration = training.configuration();
  check(configuration.backend == QStringLiteral("3dgs"), "training backend must not be translated");
  check(configuration.quality == QStringLiteral("quick"), "preset identifier must not be translated");
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
  ReconstructionDialog reconstruction(QCoreApplication::applicationDirPath(),
                                      QCoreApplication::applicationDirPath(), &window);
  const auto reconstructionConfig = reconstruction.configuration();
  // A custom iteration count catches accidental preset resets caused by
  // currentTextChanged signals when combo captions are translated.
  training.findChild<QSpinBox *>()->setValue(12345);
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
  // Window controls are installed lazily when a dialog is polished. Warm
  // existing dialogs before asserting that language changes add no actions.
  for (QDialog *dialog : {static_cast<QDialog *>(&training), static_cast<QDialog *>(&import),
       static_cast<QDialog *>(&reconstruction), static_cast<QDialog *>(&namedImport),
       static_cast<QDialog *>(&box), static_cast<QDialog *>(&exportDialog),
       static_cast<QDialog *>(&spzDialog)}) dialog->ensurePolished();
  QApplication::processEvents();
  const auto actionCount = window.findChildren<QAction *>().size();

  // The build-tree test owns a real child process. It must stay alive during
  // every switch. Installed-package QA runs the same UI/data assertions.
  const QString fixture = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("gsw_process_output_fixture.exe"));
  const bool testProcess = QFileInfo::exists(fixture);
  if (testProcess) check(supervisor->start(name, fixture, {QStringLiteral("tree-child")}), "start test worker");
  monitor->beginTraining(name, QStringLiteral("3dgs"), 7000);
  WorkerStatus status;
  status.state = QStringLiteral("running");
  status.stage = QStringLiteral("train");
  status.iteration = 3500;
  status.totalIterations = 7000;
  status.progressPercent = 50;
  status.loss = 0.125;
  status.psnr = 27.5;
  status.gaussianCount = 100000;
  monitor->updateStatus(status);
  emit supervisor->workerStatusReady(status);
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
  for (int step = 1; step <= 6; ++step) {
    const int next = (index + step) % 3;
    const QString language = AppLanguage::supported()[next];
    languageMenu->actions()[next]->trigger();
    QApplication::processEvents();
    check(AppLanguage::current() == language && AppLanguage::saved() == language, "immediate persisted language");
    check(saveAction->text() == saveTexts[next], "existing action updates immediately");
    check(exportAction->text() == QCoreApplication::translate("Workbench", "导出模型...") &&
          exportDialog.windowTitle() == QCoreApplication::translate("Workbench", "导出模型"), "export action and dialog translated live");
    check(exportDialog.options().format == ModelExportFormat::Glb && exportDialog.options().destinationPath == exportPath &&
          exportDialog.options().applyTransform, "export settings survive language changes");
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
    check(training.configuration().iterations == 12345 && training.configuration().quality == configuration.quality &&
          training.configuration().backend == configuration.backend, "training parameters retained");
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
  dismissUnexpectedNotice.stop();
  check(!AppLanguage::apply(QStringLiteral("invalid")) && AppLanguage::current() == locale &&
        AppLanguage::saved() == locale, "invalid locale leaves current UI unchanged");
  const QString screenshotDirectory = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  if (!screenshotDirectory.isEmpty()) {
    // Expand only the isolated QA layout to inspect translated monitor labels.
    if (tabs) {
      if (auto *dock = qobject_cast<QDockWidget *>(tabs->parentWidget()))
        window.resizeDocks({dock}, {260}, Qt::Vertical);
    }
    if (toolbar) toolbar->show();
    (void)viewport->focusModel();
    QApplication::processEvents();
    QDir().mkpath(screenshotDirectory);
    check(window.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral(".png"))), "UI screenshot");
    check(viewport->grabFramebuffer().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-viewport.png"))), "trackball screenshot");
    exportDialog.show(); exportDialog.adjustSize(); QApplication::processEvents();
    check(exportDialog.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-export.png"))), "export dialog screenshot");
    exportDialog.hide();
    spzDialog.show(); spzDialog.adjustSize(); QApplication::processEvents();
    check(spzDialog.grab().save(QDir(screenshotDirectory).filePath(locale + QStringLiteral("-spz.png"))), "SPZ dialog screenshot");
    spzDialog.hide();
  }
  check(runListInteractionSmokeTest(window), "all item-list interactions");
  if (testProcess) supervisor->shutdown();
  check(runWindowUiSmokeTest(window), "native window and dock controls");
  qInfo().noquote() << "Language smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
