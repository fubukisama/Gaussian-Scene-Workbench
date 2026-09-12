#include "LanguageSmokeTest.h"
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
       static_cast<QDialog *>(&box), static_cast<QDialog *>(&exportDialog)}) dialog->ensurePolished();
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
  }
  if (testProcess) supervisor->shutdown();
  check(runWindowUiSmokeTest(), "unified window and file-dialog controls");
  qInfo().noquote() << "Language smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
