#include "WorkspaceDropSmokeTest.h"

#include "AppLanguage.h"
#include "DatasetImportDialog.h"
#include "MainWindow.h"
#include "ModelExport.h"
#include "NativeViewport.h"
#include "ProcessSupervisor.h"
#include "WorkspaceDocument.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMessageBox>
#include <QPushButton>
#include <QPixmap>
#include <QScopeGuard>
#include <QSet>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>

#include <cstdio>
#include <functional>

namespace gsw {
namespace {

bool waitFor(const std::function<bool()> &predicate, const int timeout = 3000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return predicate();
}

bool writeBytes(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

struct DropResult {
  bool entered = false;
  bool dropped = false;
  Qt::DropAction action = Qt::IgnoreAction;
};

DropResult dropUrls(QWidget &target, const QList<QUrl> &urls,
                    const Qt::DropActions actions = Qt::CopyAction) {
  QMimeData mime;
  mime.setUrls(urls);
  const QPoint position = target.rect().center();
  QDragEnterEvent enter(position, actions, &mime, Qt::LeftButton,
                        Qt::NoModifier);
  QApplication::sendEvent(&target, &enter);
  QDropEvent drop(QPointF(position), actions, &mime, Qt::LeftButton,
                  Qt::NoModifier);
  QApplication::sendEvent(&target, &drop);
  if (!enter.isAccepted()) {
    // Qt does not dispatch a drop to a surface that rejected drag-enter.
    // End that rejected gesture exactly as the window system does.
    QDragLeaveEvent leave;
    QApplication::sendEvent(&target, &leave);
  }
  return {enter.isAccepted(), drop.isAccepted(), drop.dropAction()};
}

} // namespace

bool runWorkspaceDropSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](const bool okay, const char *message) {
    std::fprintf(stderr, "Workspace drop %s: %s\n", okay ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    passed = passed && okay;
    return okay;
  };
  const QString fixtureRoot = QDir(QStringLiteral("D:/")).exists()
      ? QStringLiteral("D:/Codex/qa-workspace-drop") : QDir::tempPath();
  if (!check(QDir().mkpath(fixtureRoot), "prepare a test-owned fixture directory")) return false;
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("workspace-drop-XXXXXX")));
  if (!check(temporary.isValid(), "create an isolated fixture without touching user projects")) return false;
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *viewport = window.findChild<NativeViewport *>(QStringLiteral("nativeViewport"));
  if (!check(document && viewport, "observe the workspace and viewport through public QObject interfaces")) return false;
  const QString model = temporary.filePath(QString::fromUtf8("dropped model.ply"));
  const QByteArray points("ply\nformat ascii 1.0\nelement vertex 4\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n"
      "0 0 0 231 47 193\n1 0 0 231 47 193\n0 1 0 231 47 193\n0 0 1 231 47 193\n");
  if (!check(writeBytes(model, points), "write a real PLY fixture")) return false;
  const auto initialCount = document->sceneObjects().size();
  const auto result = dropUrls(*viewport, {QUrl::fromLocalFile(model)});
  check(result.entered, "the real viewport accepts a local PLY drag-enter event");
  check(result.dropped, "the real viewport accepts the corresponding drop event");
  check(waitFor([&] {
    return document->sceneObjects().size() == initialCount + 1 &&
        QFileInfo(document->scenePath()).canonicalFilePath() == QFileInfo(model).canonicalFilePath() &&
        viewport->sceneObjectCount() == initialCount + 1;
  }), "dropping a PLY appends a real workspace object and synchronizes the viewport");
  if (!passed) return false;

  const QString originalId = document->activeSceneId();
  auto transformed = document->sceneObjects();
  for (auto &object : transformed) {
    if (object.id != originalId) continue;
    object.translation = {2.5F, -1.25F, 4.0F};
    object.rotation = QQuaternion::fromAxisAndAngle(0, 0, 1, 35.0F);
    object.scale = {1.5F, 0.75F, 2.0F};
  }
  QString error;
  if (!check(document->setSceneObjectTransforms(transformed, &error),
             "prepare a real original object with nonidentity translation, rotation and scale")) return false;

  const bool requireRenderedEdits = QApplication::platformName() != QStringLiteral("offscreen");
  if (requireRenderedEdits) {
    if (!check(waitFor([&] { return viewport->hasEditableScene(); }, 10000),
               "the real-window lane loads the original point cloud for crop preservation")) return false;
    viewport->setEditToolsLocked(false);
    viewport->clearSelection();
    viewport->invertSelection();
    viewport->deleteSelection();
    if (!check(viewport->hasUnsavedSceneEdits(),
               "create an actual unexported crop through public editing operations")) return false;
  } else {
    std::fprintf(stderr, "Workspace drop SKIP: crop rendering requires the remote real-window lane; offscreen checks object identity and transforms only\n");
    std::fflush(stderr);
  }

  const QString second = temporary.filePath(QString::fromUtf8("second model.ply"));
  const QString third = temporary.filePath(QString::fromUtf8("third model.ply"));
  if (!check(writeBytes(second, points) && writeBytes(third, points),
             "write two additional real model files")) return false;
  int unexpectedDialogs = 0;
  QTimer rejectUnexpected;
  QObject::connect(&rejectUnexpected, &QTimer::timeout, &window, [&] {
    if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      ++unexpectedDialogs;
      question->reject();
    }
  });
  rejectUnexpected.start(10);
  const auto originalViewTarget = viewport->viewTarget();
  const auto originalViewDistance = viewport->viewDistance();
  const auto originalViewOrbit = viewport->viewOrbitAngles();
  const auto multiple = dropUrls(*viewport, {QUrl::fromLocalFile(second), QUrl::fromLocalFile(third)});
  check(multiple.entered && multiple.dropped,
        "a real multi-model drop is accepted without per-model replacement prompts");
  check(waitFor([&] {
    return document->sceneObjects().size() == initialCount + 3 &&
        viewport->sceneObjectCount() == initialCount + 3;
  }), "one multi-model drop appends both models and retains all earlier objects");
  rejectUnexpected.stop();
  check(unexpectedDialogs == 0,
        "appending models does not ask to replace objects or discard an existing crop");
  bool preservedOriginal = false;
  for (const auto &object : document->sceneObjects()) {
    if (object.id != originalId) continue;
    preservedOriginal = object.path == model && object.translation == QVector3D(2.5F, -1.25F, 4.0F) &&
        object.rotation == QQuaternion::fromAxisAndAngle(0, 0, 1, 35.0F) &&
        object.scale == QVector3D(1.5F, 0.75F, 2.0F);
  }
  check(preservedOriginal, "multi-model append preserves the original stable ID, source and complete transform");
  if (requireRenderedEdits) {
    check(viewport->activeSceneId() == originalId && document->activeSceneId() == originalId &&
          viewport->hasUnsavedSceneEdits(),
          "a dropped append keeps a dirty original active so later project changes cannot silently lose its crop");
    check(viewport->viewTarget() == originalViewTarget && viewport->viewDistance() == originalViewDistance &&
          viewport->viewOrbitAngles() == originalViewOrbit,
          "appending while an original crop is dirty preserves the actual view target, distance and orbit");
    check(viewport->activateSceneObject(originalId) && viewport->hasUnsavedSceneEdits(),
          "reactivating the original model retains its unexported crop after multi-model append");
  }
  QFile originalFile(model);
  check(originalFile.open(QIODevice::ReadOnly) && originalFile.readAll() == points,
        "dragging and appending never rewrites the original PLY file");
  if (!passed) return false;

  auto *supervisor = window.findChild<ProcessSupervisor *>();
  if (!check(supervisor && !supervisor->isRunning(),
             "media cancellation starts with no test worker running")) return false;
  int mediaWorkerLaunches = 0;
  QObject mediaObserver;
  QObject::connect(supervisor, &ProcessSupervisor::taskLaunchRequested, &mediaObserver,
      [&](const QString &, const QString &) { ++mediaWorkerLaunches; });
  const QString photograph = temporary.filePath(QString::fromUtf8("photo 01.jpg"));
  const QString video = temporary.filePath(QString::fromUtf8("video 01.mp4"));
  const QString directory = temporary.filePath(QStringLiteral("media-directory"));
  const QString nested = QDir(directory).filePath(QStringLiteral("nested"));
  QImage photo(8, 8, QImage::Format_RGB32);
  photo.fill(QColor(120, 90, 60));
  if (!check(photo.save(photograph, "JPG") && writeBytes(video, "test-owned video source; cancel before decode") &&
             QDir().mkpath(nested) && photo.save(QDir(nested).filePath(QStringLiteral("nested.jpg")), "JPG") &&
             writeBytes(QDir(nested).filePath(QStringLiteral("nested.mov")), "cancelled video fixture"),
             "prepare real photo sources and test-owned undecoded video paths")) return false;
  const QByteArray originalPhotographBytes = readBytes(photograph);
  const QByteArray originalVideoBytes = readBytes(video);

  const auto cancelMediaDrop = [&](const QStringList &paths, const char *description) {
    const auto beforeObjects = document->sceneCollectionJson();
    const auto beforeDataset = document->datasetPath();
    const auto beforeRoot = document->rootPath();
    const auto beforeProject = document->projectFilePath();
    const auto beforeModified = document->isModified();
    int dialogs = 0;
    int unexpected = 0;
    bool handling = false;
    QTimer cancelDialog;
    QObject::connect(&cancelDialog, &QTimer::timeout, &window, [&] {
      if (handling) return;
      if (auto *dialog = qobject_cast<DatasetImportDialog *>(QApplication::activeModalWidget())) {
        handling = true;
        ++dialogs;
        auto *sources = dialog->findChild<QListWidget *>(QStringLiteral("datasetImportSourceList"));
        auto *fps = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("datasetImportFpsSpin"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("datasetImportCancelButton"));
        check(sources && sources->count() == paths.size() && dialog->request().sourcePaths == paths,
              "all dropped photos, videos or directories enter one existing media-import dialog");
        check(fps && fps->isEnabled(), "video extraction FPS remains an explicit editable import option");
        if (fps) {
          fps->setValue(2.5);
          check(dialog->request().framesPerSecond == 2.5,
                "the real media dialog retains the user-chosen extraction rate");
        }
        if (cancel) cancel->click();
        else { check(false, "the existing media dialog exposes its cancel button"); dialog->reject(); }
        handling = false;
      } else if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        ++unexpected;
        question->reject();
      }
    });
    cancelDialog.start(10);
    QList<QUrl> urls;
    for (const QString &path : paths) urls.append(QUrl::fromLocalFile(path));
    const auto media = dropUrls(*viewport, urls);
    check(media.entered && media.dropped, description);
    check(waitFor([&] { return dialogs > 0; }, 2000),
          "a real drop opens the media-import confirmation instead of silently starting work");
    cancelDialog.stop();
    check(dialogs == 1 && unexpected == 0,
          "one media drop produces one media dialog and no unrelated replacement prompt");
    check(document->sceneCollectionJson() == beforeObjects && document->datasetPath() == beforeDataset &&
          document->rootPath() == beforeRoot && document->projectFilePath() == beforeProject &&
          document->isModified() == beforeModified,
          "cancelling dragged media leaves existing models, dataset, project and modified state unchanged");
    check(mediaWorkerLaunches == 0 && !supervisor->isRunning(),
          "cancelled media drops launch no import, reconstruction or training process");
  };
  cancelMediaDrop({photograph, video}, "photo and video files are accepted together as one real media drop");
  cancelMediaDrop({directory}, "a media-directory drop uses the existing recursive-import confirmation");
  if (!passed) return false;

  const QString projectRoot = temporary.filePath(QStringLiteral("other-project"));
  const QString project = temporary.filePath(QStringLiteral("other.gsw.json"));
  const QString alternateProject = temporary.filePath(QStringLiteral("other.gswjson"));
  WorkspaceDocument otherProject;
  if (!check(QDir().mkpath(projectRoot) && otherProject.create(projectRoot, &error) &&
             otherProject.setScenePath(second, &error) && otherProject.save(project, &error) &&
             QFile::copy(project, alternateProject),
             "write valid independent GSW projects in both supported filename forms")) return false;
  const QByteArray originalProjectBytes = readBytes(project);
  const auto workspaceState = [&]() {
    return QJsonObject{{QStringLiteral("objects"), document->sceneCollectionJson()},
        {QStringLiteral("dataset"), document->datasetPath()},
        {QStringLiteral("project"), document->projectFilePath()},
        {QStringLiteral("root"), document->rootPath()},
        {QStringLiteral("modified"), document->isModified()}};
  };
  for (const auto &path : {project, alternateProject}) {
    const auto before = workspaceState();
    int savePrompts = 0;
    int otherDialogs = 0;
    QTimer cancelProject;
    QObject::connect(&cancelProject, &QTimer::timeout, &window, [&] {
      if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        if (question->standardButtons().testFlag(QMessageBox::Save) &&
            question->standardButtons().testFlag(QMessageBox::Discard) &&
            question->standardButtons().testFlag(QMessageBox::Cancel)) {
          ++savePrompts;
          question->button(QMessageBox::Cancel)->click();
        } else {
          ++otherDialogs;
          question->reject();
        }
      } else if (auto *dialog = qobject_cast<DatasetImportDialog *>(QApplication::activeModalWidget())) {
        ++otherDialogs;
        dialog->reject();
      }
    });
    cancelProject.start(10);
    const auto opening = dropUrls(*viewport, {QUrl::fromLocalFile(path)});
    check(opening.entered && opening.dropped, "a single valid project is accepted by the real drop route");
    check(waitFor([&] { return savePrompts > 0; }, 2000),
          "dropping a project asks before replacing an unsaved current workspace");
    cancelProject.stop();
    check(savePrompts == 1 && otherDialogs == 0 && workspaceState() == before &&
          (!requireRenderedEdits || viewport->hasUnsavedSceneEdits()),
          "cancelling the real save confirmation preserves existing project objects, transforms and crop");
  }

  const QString invalidProject = temporary.filePath(QStringLiteral("invalid.gsw.json"));
  const QString invalidAlternateProject = temporary.filePath(QStringLiteral("invalid.gswjson"));
  const QString text = temporary.filePath(QStringLiteral("notes.txt"));
  if (!check(writeBytes(invalidProject, "this is not project JSON") &&
             writeBytes(invalidAlternateProject, "{\"schemaVersion\":99}") && writeBytes(text, "not importable"),
             "prepare invalid project content and unsupported files")) return false;
  const QList<QList<QUrl>> rejectedBatches{
      {QUrl::fromLocalFile(model), QUrl::fromLocalFile(photograph)},
      {QUrl::fromLocalFile(project), QUrl::fromLocalFile(model)},
      {QUrl::fromLocalFile(project), QUrl::fromLocalFile(alternateProject)},
      {QUrl::fromLocalFile(model), QUrl(QStringLiteral("https://example.invalid/model.ply"))},
      {QUrl::fromLocalFile(temporary.filePath(QStringLiteral("missing.ply")))},
      {QUrl::fromLocalFile(text)},
      {QUrl::fromLocalFile(invalidProject)},
      {QUrl::fromLocalFile(invalidAlternateProject)}};
  for (const auto &urls : rejectedBatches) {
    const auto before = workspaceState();
    int savePrompts = 0;
    int unrelatedDialogs = 0;
    QTimer rejectInvalid;
    QObject::connect(&rejectInvalid, &QTimer::timeout, &window, [&] {
      if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        if (question->standardButtons().testFlag(QMessageBox::Save)) ++savePrompts;
        question->reject();
      } else if (auto *dialog = qobject_cast<DatasetImportDialog *>(QApplication::activeModalWidget())) {
        ++unrelatedDialogs;
        dialog->reject();
      }
    });
    rejectInvalid.start(10);
    const auto rejected = dropUrls(*viewport, urls);
    waitFor([] { return false; }, 80);
    rejectInvalid.stop();
    check(!rejected.dropped, "invalid, mixed, remote and multi-project drops are rejected as a whole");
    check(savePrompts == 0 && unrelatedDialogs == 0,
          "invalid input is checked before asking to discard edits or opening a media-import dialog");
    check(workspaceState() == before && (!requireRenderedEdits || viewport->hasUnsavedSceneEdits()) &&
          mediaWorkerLaunches == 0 && !supervisor->isRunning(),
          "a rejected batch changes no object, project, crop or task and does not partially import valid items");
  }
  if (!passed) return false;

  auto *card = window.findChild<QFrame *>(QStringLiteral("workspaceDropCard"));
  auto *dropTitle = window.findChild<QLabel *>(QStringLiteral("workspaceDropTitle"));
  auto *dropDetail = window.findChild<QLabel *>(QStringLiteral("workspaceDropDetail"));
  auto *projectTree = window.findChild<QTreeWidget *>(QStringLiteral("projectTree"));
  if (!check(card && dropTitle && dropDetail && projectTree,
             "the actual workspace exposes a nonmodal drag preview and the project-tree drop surface")) return false;
  check(!card->isVisible(), "drag guidance is hidden when no file drag is active");
  const QString originalLanguage = AppLanguage::current();
  const auto restoreLanguage = qScopeGuard([&] { AppLanguage::apply(originalLanguage, false); });
  int previewIndex = 0;
  const auto inspectPreview = [&](const QList<QUrl> &urls, const bool accepted,
                                  const char *titleSource, const char *detailSource, const int count) {
    ++previewIndex;
    const auto before = workspaceState();
    QMimeData mime;
    mime.setUrls(urls);
    const QPoint position = viewport->rect().center();
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &enter);
    QDragMoveEvent move(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    check(enter.isAccepted() == accepted && move.isAccepted() == accepted && card->isVisible(),
          "real drag-enter and drag-move display whether this batch can be dropped");
    QSet<QString> visibleTitles;
    for (const auto &language : {QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")}) {
      check(AppLanguage::apply(language, false), "switch the language while the same real drag preview is visible");
      QString expectedTitle = QCoreApplication::translate("Workbench", titleSource);
      if (count > 0) expectedTitle = expectedTitle.arg(count);
      check(card->isVisible() && dropTitle->text() == expectedTitle && !dropDetail->text().isEmpty(),
            "drag guidance immediately displays its translated action and a nonempty explanation");
      if (detailSource) {
        check(dropDetail->text() == QCoreApplication::translate("Workbench", detailSource),
              "drag guidance explains preservation, media confirmation or the exact rejection reason");
      } else if (!accepted) {
        check(dropDetail->text().contains(QCoreApplication::translate("Workbench", "工程文件必须单独拖入")) &&
              dropDetail->text().contains(QCoreApplication::translate("Workbench", "模型与媒体请分批拖入。")),
              "mixed-input guidance explains project isolation and separate model/media batches");
      }
      visibleTitles.insert(dropTitle->text());
      const QString evidenceDirectory = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
      if (!evidenceDirectory.isEmpty()) {
        check(QDir().mkpath(evidenceDirectory) && card->grab().save(QDir(evidenceDirectory).filePath(
            QStringLiteral("workspace-drop-%1-%2.png").arg(previewIndex).arg(language))),
            "save only the test-owned drag card as an optional visual artifact, never capture the desktop");
      }
      check(workspaceState() == before && window.findChild<QFrame *>(QStringLiteral("workspaceDropCard")) == card,
            "live language switching preserves the existing workspace and reuses the same preview widget");
    }
    check(visibleTitles.size() == 3, "Chinese, English and Japanese drag titles are independently localized");
    QDragLeaveEvent leave;
    QApplication::sendEvent(viewport, &leave);
    check(!card->isVisible(), "leaving the actual drop surface hides its drag preview");
  };
  inspectPreview({QUrl::fromLocalFile(model)}, true, "松开以追加 %1 个模型",
                 "保留现有对象、源坐标和裁剪；不自动配准。", 1);
  inspectPreview({QUrl::fromLocalFile(photograph), QUrl::fromLocalFile(video)}, true,
                 "松开以添加 %1 个媒体来源",
                 "支持照片、视频和目录；确认抽帧设置后再导入，不自动训练。", 2);
  inspectPreview({QUrl::fromLocalFile(project)}, true, "松开以打开工程", nullptr, 0);
  inspectPreview({QUrl(QStringLiteral("https://example.invalid/model.ply"))}, false,
                 "无法拖入这些文件", "仅支持本地文件和目录；不接受网页链接。", 0);
  inspectPreview({QUrl::fromLocalFile(model), QUrl::fromLocalFile(photograph)}, false,
                 "无法拖入这些文件", nullptr, 0);

  const auto beforeMoveOnly = workspaceState();
  const auto moveOnly = dropUrls(*viewport, {QUrl::fromLocalFile(second)}, Qt::MoveAction);
  check(!moveOnly.entered && !moveOnly.dropped && workspaceState() == beforeMoveOnly && !card->isVisible(),
        "a move-only file drag is rejected without moving sources or changing the project");
  const auto countBeforeDuplicate = document->sceneObjects().size();
  const auto duplicate = dropUrls(window,
      {QUrl::fromLocalFile(second), QUrl::fromLocalFile(second)}, Qt::CopyAction | Qt::MoveAction);
  check(duplicate.entered && duplicate.dropped && duplicate.action == Qt::CopyAction,
        "the main-window drop surface chooses safe copy semantics when move is also offered");
  check(document->sceneObjects().size() == countBeforeDuplicate + 1 && !card->isVisible(),
        "duplicate URLs in one batch append only one object and hide drag guidance after dropping");
  const auto countBeforeTree = document->sceneObjects().size();
  const auto treeDrop = dropUrls(*projectTree->viewport(), {QUrl::fromLocalFile(third)});
  check(treeDrop.entered && treeDrop.dropped && document->sceneObjects().size() == countBeforeTree + 1 &&
        !card->isVisible(), "the actual project-tree viewport shares the same append route and drop cleanup");
  check(QFileInfo::exists(second) && QFileInfo::exists(third),
        "copy-style drops retain all source files at their original locations");

  const QString helper = qEnvironmentVariable("GSW_PROCESS_OUTPUT_FIXTURE",
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gsw_process_output_fixture.exe")));
  const QString ready = temporary.filePath(QStringLiteral("busy-ready"));
  const QString release = temporary.filePath(QStringLiteral("busy-release"));
  const QString taskName = QString::fromUtf8("workspace-drop-owned-worker");
  if (!check(QFileInfo(helper).isFile() && supervisor->start(taskName, helper, {ready, release}, temporary.path()),
             "start an actual bounded test-owned worker through the public supervisor interface")) return false;
  const auto releaseWorker = qScopeGuard([&] {
    writeBytes(release, "release");
    if (!waitFor([&] { return !supervisor->isRunning(); }, 3000) && supervisor->activeTask() == taskName) {
      supervisor->stop();
      waitFor([&] { return !supervisor->isRunning(); }, 3000);
    }
  });
  if (!check(waitFor([&] { return QFileInfo::exists(ready) && supervisor->isRunning(); }),
             "the test-owned worker is alive before busy-state drops")) return false;
  inspectPreview({QUrl::fromLocalFile(model)}, false, "无法拖入这些文件",
                 "请先等待当前任务或导入操作结束，再拖入文件。", 0);
  const int ownLaunchCount = mediaWorkerLaunches;
  const QList<QList<QUrl>> busyBatches{{QUrl::fromLocalFile(model)},
      {QUrl::fromLocalFile(photograph), QUrl::fromLocalFile(video)}, {QUrl::fromLocalFile(project)}};
  for (const auto &urls : busyBatches) {
    const auto before = workspaceState();
    const auto rejected = dropUrls(*viewport, urls);
    check(!rejected.entered && !rejected.dropped && !card->isVisible() && workspaceState() == before,
          "running-task protection rejects model, media and project drops without workspace mutation");
    check(supervisor->isRunning() && supervisor->activeTask() == taskName &&
          !supervisor->wasStopRequested() && mediaWorkerLaunches == ownLaunchCount,
          "a busy drop neither stops the current worker nor launches a second process");
  }
  check(writeBytes(release, "release") && waitFor([&] { return !supervisor->isRunning(); }, 3000),
        "release and finish only the test-owned worker after the busy-state checks");
  if (!passed) return false;

  // Switch only after all preservation checks. Both the old project and its
  // unexported crop are test-owned, and this script explicitly discards them
  // through the same confirmations a human must complete.
  int projectDiscardPrompts = 0;
  int cropDiscardPrompts = 0;
  int unexpectedOpeningDialogs = 0;
  QTimer confirmProjectSwitch;
  QObject::connect(&confirmProjectSwitch, &QTimer::timeout, &window, [&] {
    if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      if (question->standardButtons().testFlag(QMessageBox::Discard) &&
          question->standardButtons().testFlag(QMessageBox::Cancel)) {
        if (question->windowTitle() == QCoreApplication::translate("Workbench", "工程尚未保存"))
          ++projectDiscardPrompts;
        else if (question->windowTitle() == QCoreApplication::translate("Workbench", "裁剪尚未导出"))
          ++cropDiscardPrompts;
        else ++unexpectedOpeningDialogs;
        question->button(QMessageBox::Discard)->click();
      } else {
        ++unexpectedOpeningDialogs;
        question->reject();
      }
    } else if (auto *dialog = qobject_cast<DatasetImportDialog *>(QApplication::activeModalWidget())) {
      ++unexpectedOpeningDialogs;
      dialog->reject();
    }
  });
  confirmProjectSwitch.start(10);
  const int launchesBeforeProject = mediaWorkerLaunches;
  const auto openingProject = dropUrls(window, {QUrl::fromLocalFile(project)});
  check(openingProject.entered && openingProject.dropped && waitFor([&] {
    return QFileInfo(document->projectFilePath()).canonicalFilePath() == QFileInfo(project).canonicalFilePath() &&
        document->activeSceneId() == otherProject.activeSceneId() && document->sceneObjects().size() == 1 &&
        viewport->sceneObjectCount() == 1;
  }), "a real main-window project drop opens the chosen project after explicit discard confirmations");
  check(projectDiscardPrompts == 1 && cropDiscardPrompts == (requireRenderedEdits ? 1 : 0) &&
        unexpectedOpeningDialogs == 0,
        "successful project switching requires explicit consent for the project and any remaining crop");
  const auto openingAlternate = dropUrls(*projectTree->viewport(), {QUrl::fromLocalFile(alternateProject)});
  check(openingAlternate.entered && openingAlternate.dropped && waitFor([&] {
    return QFileInfo(document->projectFilePath()).canonicalFilePath() == QFileInfo(alternateProject).canonicalFilePath() &&
        document->activeSceneId() == otherProject.activeSceneId() && document->sceneObjects().size() == 1 &&
        viewport->sceneObjectCount() == 1;
  }), "the actual project-tree surface also opens the supported .gswjson project form");
  confirmProjectSwitch.stop();
  check(document->scenePath() == second && document->sceneTranslation().isNull() &&
        document->sceneScale() == QVector3D(1, 1, 1) && !document->isModified() &&
        mediaWorkerLaunches == launchesBeforeProject && !supervisor->isRunning() && !card->isVisible(),
        "project drops restore the selected project's objects without importing media, starting training or retaining old transforms");

  const QString brokenModel = temporary.filePath(QStringLiteral("broken.ply"));
  if (!check(writeBytes(brokenModel, "not a PLY header"), "prepare a malformed model for a partial-batch regression")) return false;
  const auto retainedObjects = document->sceneObjects();
  const QString retainedDataset = document->datasetPath();
  int modelErrors = 0;
  int unexpectedModelDialogs = 0;
  QTimer dismissModelError;
  QObject::connect(&dismissModelError, &QTimer::timeout, &window, [&] {
    if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      if (question->icon() == QMessageBox::Critical &&
          question->windowTitle() == QCoreApplication::translate("Workbench", "无法导入场景")) {
        ++modelErrors;
        question->accept();
      } else {
        ++unexpectedModelDialogs;
        question->reject();
      }
    }
  });
  dismissModelError.start(10);
  const auto partial = dropUrls(*viewport,
      {QUrl::fromLocalFile(model), QUrl::fromLocalFile(brokenModel), QUrl::fromLocalFile(third)});
  check(partial.entered && partial.dropped && modelErrors == 1 && unexpectedModelDialogs == 0,
        "a malformed model interrupts only its accepted model batch with the expected import error");
  check(document->sceneObjects().size() == retainedObjects.size() + 1 &&
        document->scenePath() == model && document->datasetPath() == retainedDataset,
        "a middle failure retains the first successful append and never processes later models");
  for (const auto &before : retainedObjects) {
    bool retained = false;
    for (const auto &after : document->sceneObjects()) {
      if (before.id == after.id && before.path == after.path && before.translation == after.translation &&
          before.rotation == after.rotation && before.scale == after.scale) retained = true;
    }
    check(retained, "a partially failed batch preserves every original object and complete transform");
  }
  check(window.statusBar()->currentMessage().contains(
        QCoreApplication::translate("Workbench", "已追加 %1 / %2 个模型").arg(1).arg(3)),
        "the actual status bar accurately reports one of three models appended after a middle failure");

  const QString gaussian = temporary.filePath(QStringLiteral("tiny-gaussian.ply"));
  const QString compressed = temporary.filePath(QStringLiteral("tiny-gaussian.spz"));
  QByteArray gaussianBytes("ply\nformat ascii 1.0\nelement vertex 4\n");
  for (const char *name : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                           "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
    gaussianBytes += QByteArray("property float ") + name + '\n';
  gaussianBytes += "end_header\n0 0 0 1 -0.5 -0.5 3 -3 -3 -3 1 0 0 0\n"
      "1 0 0 1 -0.5 -0.5 3 -3 -3 -3 1 0 0 0\n"
      "0 1 0 1 -0.5 -0.5 3 -3 -3 -3 1 0 0 0\n"
      "0 0 1 1 -0.5 -0.5 3 -3 -3 -3 1 0 0 0\n";
  ModelExportOptions compression;
  compression.sourcePath = gaussian;
  compression.destinationPath = compressed;
  compression.format = ModelExportFormat::Spz;
  if (!check(writeBytes(gaussian, gaussianBytes) && exportModelFile(compression).success,
             "generate a tiny real SPZ through the existing public export API")) return false;
  const QByteArray originalCompressedBytes = readBytes(compressed);
  const auto countBeforeSpz = document->sceneObjects().size();
  const auto spzDrop = dropUrls(*viewport, {QUrl::fromLocalFile(compressed)});
  check(spzDrop.entered && spzDrop.dropped && document->sceneObjects().size() == countBeforeSpz + 1 &&
        document->sceneMetadata().looksLikeGaussianSplat() && document->sceneMetadata().vertexCount == 4 &&
        document->scenePath() != compressed && QFileInfo::exists(document->scenePath()),
        "a real SPZ drop appends its decoded Gaussian working copy without treating compressed bytes as editable PLY");
  const QString brokenSpz = temporary.filePath(QStringLiteral("broken.spz"));
  if (!check(writeBytes(brokenSpz, "not an SPZ"), "prepare a malformed SPZ delivery file")) return false;
  const auto beforeBadSpz = workspaceState();
  const int errorsBeforeSpz = modelErrors;
  const auto badSpzDrop = dropUrls(*viewport, {QUrl::fromLocalFile(brokenSpz)});
  dismissModelError.stop();
  check(badSpzDrop.entered && badSpzDrop.dropped && modelErrors == errorsBeforeSpz + 1 &&
        unexpectedModelDialogs == 0 && workspaceState() == beforeBadSpz,
        "a failed SPZ decode reports its import error and adds or replaces no object");
  struct SourceSnapshot {
    QString path;
    QByteArray bytes;
  };
  const QList<SourceSnapshot> selectedSources{
      {model, points}, {second, points}, {third, points}, {brokenModel, "not a PLY header"},
      {photograph, originalPhotographBytes}, {video, originalVideoBytes},
      {project, originalProjectBytes}, {alternateProject, originalProjectBytes},
      {gaussian, gaussianBytes}, {compressed, originalCompressedBytes}, {brokenSpz, "not an SPZ"}};
  bool sourcesUnchanged = true;
  for (const auto &source : selectedSources) {
    const QByteArray after = readBytes(source.path);
    const bool identical = after == source.bytes;
    sourcesUnchanged = sourcesUnchanged && identical;
    if (!identical) {
      const QFileInfo info(source.path);
      const QByteArray displayPath = QDir::toNativeSeparators(source.path).toUtf8();
      std::fprintf(stderr,
          "Workspace drop SOURCE MISMATCH: path=%s before_bytes=%lld after_file_bytes=%lld after_read_bytes=%lld exists=%d\n",
          displayPath.constData(), static_cast<long long>(source.bytes.size()),
          static_cast<long long>(info.size()), static_cast<long long>(after.size()), info.exists() ? 1 : 0);
      std::fflush(stderr);
    }
  }
  check(sourcesUnchanged,
        "successful, cancelled, rejected and partially failed drops leave every selected source byte-for-byte unchanged");
  check(!supervisor->isRunning() && mediaWorkerLaunches == launchesBeforeProject && !card->isVisible(),
        "all model and SPZ drop regressions finish without launching training or leaving drag guidance visible");
  return passed;
}

} // namespace gsw
