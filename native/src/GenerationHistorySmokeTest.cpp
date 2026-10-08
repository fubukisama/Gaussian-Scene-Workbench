#include "GenerationHistorySmokeTest.h"
#include "AppLanguage.h"
#include "GenerationHistoryDialog.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool writeBytes(const QString &path, const QByteArray &bytes) {
  return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  }();
}
bool writeJson(const QString &path, const QJsonObject &object) {
  return writeBytes(path, QJsonDocument(object).toJson());
}
bool waitFor(const std::function<bool()> &predicate) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < 10000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return predicate();
}
bool selectByDisplayName(QTreeWidget *list, const QString &name) {
  if (!list) return false;
  for (int index = 0; index < list->topLevelItemCount(); ++index) {
    auto *item = list->topLevelItem(index);
    if (item->text(0) != name) continue;
    list->setCurrentItem(item);
    list->scrollToItem(item);
    return true;
  }
  return false;
}
} // namespace

bool runGenerationHistorySmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](bool okay, const char *message) {
    std::fprintf(stderr, "Generation history %s: %s\n", okay ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    passed = passed && okay;
    return okay;
  };
  QTemporaryDir temporary;
  if (!temporary.isValid()) return false;
  const QString projectFile = temporary.filePath(QStringLiteral("experiments.gsw.json"));
  const QString projectRoot = WorkspaceDocument::projectDataRootForFile(projectFile);
  QDir root(projectRoot);
  if (!QDir().mkpath(projectRoot)) return false;
  const QByteArray points("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n0 0 0 231 47 193\n1 0 0 231 47 193\n0 1 0 231 47 193\n0 0 1 231 47 193\n");
  const QByteArray mesh("ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nelement face 2\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n0 0 1\n3 0 1 2\n3 0 2 3\n");
  const QString originalPath = root.filePath(QStringLiteral("original.ply"));
  if (!writeBytes(originalPath, mesh)) return false;
  WorkspaceDocument fixture;
  QString error;
  if (!fixture.create(projectRoot, &error) || !fixture.setScenePath(originalPath, &error)) return false;
  const QString originalId = fixture.activeSceneId();
  GenerationHistoryStore store(projectRoot);
  QList<GenerationExperiment> expected;
  const QStringList pipelines{QStringLiteral("3dgs"), QStringLiteral("2dgs"),
      QStringLiteral("bounded"), QStringLiteral("unbounded"), QStringLiteral("sugar"), QStringLiteral("gs2mesh")};
  for (int index = 0; index < pipelines.size(); ++index) {
    const auto backend = pipelines.at(index);
    const bool training = index < 2;
    GenerationExperiment record;
    record.id = QStringLiteral("history-") + backend;
    record.pipeline = training ? QStringLiteral("training") : QStringLiteral("mesh");
    record.backend = backend;
    record.displayName = QString::fromUtf8("研究实验 / 撮影 · ") + backend;
    record.datasetPath = root.filePath(QStringLiteral("datasets/") + backend);
    record.configurationPath = root.filePath(QStringLiteral(".gsw/jobs/") + record.id + QStringLiteral(".json"));
    record.outputRoot = root.filePath(QStringLiteral("output/") + record.id);
    record.resultPath = QDir(record.outputRoot).filePath(QStringLiteral("result.ply"));
    record.resultSceneId = record.id;
    record.status = training ? QStringLiteral("paused") : QStringLiteral("completed");
    record.parameters = {{QStringLiteral("projectRoot"), projectRoot},
        {QStringLiteral("datasetPath"), record.datasetPath}, {QStringLiteral("backend"), backend},
        {QStringLiteral("nativeCheckpoint"), training},
        {QStringLiteral("outputRoot"), root.filePath(QStringLiteral("output"))},
        {QStringLiteral("outputScene"), record.id}, {QStringLiteral("outputDisplayName"), record.displayName},
        {QStringLiteral("experimentMarker"), 100 + index},
        {QStringLiteral("trainOptions"), QJsonObject{{QStringLiteral("iterations"), 30000}}}};
    if (!QDir().mkpath(record.datasetPath) || !writeBytes(record.resultPath, training ? points : mesh) ||
        !writeJson(record.configurationPath, record.parameters)) return false;
    if (training) {
      const QString checkpointDirectory = QDir(record.outputRoot).filePath(QStringLiteral(".gsw-resume"));
      const QString checkpointName = QStringLiteral("state-0123456789abcdef0123456789abcdef.pth");
      if (!writeBytes(QDir(checkpointDirectory).filePath(checkpointName), "opaque-ui-metadata-fixture") ||
          !writeJson(QDir(checkpointDirectory).filePath(QStringLiteral("ready.json")),
              {{QStringLiteral("version"), 1}, {QStringLiteral("backend"), backend},
               {QStringLiteral("file"), checkpointName}, {QStringLiteral("iteration"), 4407},
               {QStringLiteral("total"), 30000}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
               {QStringLiteral("identity"), QString(64, QLatin1Char('b'))}}) ||
          !fixture.publishGeneratedScene(record.resultPath, record.resultSceneId, &error)) return false;
    }
    if (!store.upsert(record, &error)) return false;
    expected.append(record);
  }
  auto objects = fixture.sceneObjects();
  for (int index = 0; index < objects.size(); ++index) {
    objects[index].translation = {float(index) + 1.25F, -2.5F, .75F};
    objects[index].rotation = QQuaternion::fromAxisAndAngle(0, 0, 1, 10.F + index * 15.F);
    objects[index].scale = {1.5F, .75F, 2.F};
  }
  if (!fixture.setSceneObjectTransforms(objects, &error) || !fixture.activateSceneObject(originalId) ||
      !fixture.setDatasetPath(expected.at(0).datasetPath, &error) ||
      !fixture.saveManifest(projectFile, &error) ||
      !saveActiveTrainingJob(projectRoot, {expected.at(0).configurationPath,
          expected.at(0).outputRoot, true, expected.at(0).resultSceneId}, &error)) return false;
  enum class Mode { Inspect, ViewFirst, CancelResumeSecond, InspectMissing };
  Mode mode = Mode::Inspect;
  int handledDialogs = 0;
  int cancelledTrust = 0;
  bool inDialogCallback = false;
  QTimer scripted;
  QObject::connect(&scripted, &QTimer::timeout, &window, [&] {
    if (inDialogCallback) return;
    if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      if (mode == Mode::CancelResumeSecond && question->standardButtons().testFlag(QMessageBox::No) &&
          question->text().contains(QStringLiteral("4407"))) {
        ++cancelledTrust;
        question->button(QMessageBox::No)->click();
      } else {
        check(false, "unexpected modal dialog in a test-owned project");
        question->reject();
      }
      return;
    }
    auto *dialog = qobject_cast<GenerationHistoryDialog *>(QApplication::activeModalWidget());
    if (!dialog) return;
    inDialogCallback = true;
    ++handledDialogs;
    auto *list = dialog->findChild<QTreeWidget *>(QStringLiteral("generationHistoryList"));
    auto *parameters = dialog->findChild<QPlainTextEdit *>(QStringLiteral("generationHistoryParameters"));
    auto *resume = dialog->findChild<QPushButton *>(QStringLiteral("generationHistoryResume"));
    auto *view = dialog->findChild<QPushButton *>(QStringLiteral("generationHistoryViewResult"));
    if (!check(list && parameters && resume && view && list->topLevelItemCount() == 6,
               "actual workflow action opens six independent persisted experiments")) {
      dialog->reject(); inDialogCallback = false; return;
    }
    if (mode == Mode::Inspect) {
      for (int index = 0; index < expected.size(); ++index) {
        const auto &record = expected.at(index);
        if (!check(selectByDisplayName(list, record.displayName), "each original experiment name is selectable")) continue;
        check(dialog->selectedRecord().id == record.id &&
              dialog->selectedRecord().parameters.value(QStringLiteral("experimentMarker")).toInt() == 100 + index,
              "selected experiment retains its own parameters rather than the latest task configuration");
        check(view->isEnabled() && resume->isEnabled() == (index < 2),
              "results are independently viewable; optimizer resume is limited to native 3DGS and 2DGS");
        auto cursor = parameters->textCursor();
        cursor.select(QTextCursor::Document);
        parameters->setTextCursor(cursor);
        const auto selectedText = cursor.selectedText();
        for (const auto &language : AppLanguage::supported()) {
          check(AppLanguage::apply(language, false), "live translation is available in the packaged application");
          check(dialog->selectedRecord().id == record.id && list->currentItem()->text(0) == record.displayName &&
                parameters->textCursor().selectedText() == selectedText,
                "language switching preserves selected experiment, user name and parameter selection");
        }
      }
      dialog->reject();
    } else if (mode == Mode::ViewFirst) {
      check(selectByDisplayName(list, expected.at(0).displayName), "select first experiment result");
      view->click();
    } else {
      check(selectByDisplayName(list, expected.at(1).displayName), "select second experiment independently");
      if (mode == Mode::InspectMissing) {
        check(!resume->isEnabled() && view->isEnabled() && !parameters->toPlainText().isEmpty(),
              "missing configuration disables resume while retaining saved parameters and result access");
        dialog->reject();
      } else {
        check(resume->isEnabled(), "the second native experiment exposes its own full-state metadata entry");
        resume->click();
      }
    }
    inDialogCallback = false;
  });
  scripted.start(10);
  if (!check(window.openProjectFile(projectFile), "open fixture through the public project interface")) return false;
  auto *historyAction = window.findChild<QAction *>(QStringLiteral("generationHistoryAction"));
  if (!check(historyAction && historyAction->isEnabled(), "generation history is a public workflow action")) return false;
  historyAction->trigger();
  check(handledDialogs == 1 && !window.mProcessSupervisor.isRunning(), "browsing the archive starts no worker");
  mode = Mode::ViewFirst;
  historyAction->trigger();
  check(waitFor([&] { window.mViewport->grabFramebuffer(); return window.mViewport->scenePath() == expected.at(0).resultPath; }),
        "viewing a retained result updates the actual viewport");
  check(window.mWorkspace.activeSceneId() == expected.at(0).resultSceneId &&
        window.mWorkspace.sceneObjects().size() == objects.size(),
        "view result activates its stable object instead of appending a duplicate or replacing a reference");
  const auto retained = window.mWorkspace.sceneObjects();
  for (const auto &before : objects) {
    bool matches = false;
    for (const auto &after : retained)
      if (before.id == after.id && before.path == after.path && before.translation == after.translation &&
          before.rotation == after.rotation && before.scale == after.scale) matches = true;
    check(matches, "view result retains every original object source and transform");
  }
  const QString datasetBefore = window.mWorkspace.datasetPath();
  const auto activeBefore = loadActiveTrainingJob(projectRoot);
  mode = Mode::CancelResumeSecond;
  historyAction->trigger();
  const auto activeAfter = loadActiveTrainingJob(projectRoot);
  check(cancelledTrust == 1 && !window.mProcessSupervisor.isRunning() &&
        datasetBefore == window.mWorkspace.datasetPath() &&
        activeBefore.configurationPath == activeAfter.configurationPath &&
        activeBefore.outputSceneRoot == activeAfter.outputSceneRoot &&
        activeBefore.resultSceneId == activeAfter.resultSceneId,
        "declining pickle trust starts no worker and changes neither dataset nor current resume selection");
  if (!window.mWorkspace.saveManifest(projectFile, &error)) return false;
  mode = Mode::Inspect;
  if (!check(window.openProjectFile(projectFile), "close and reopen the saved project through its public interface")) return false;
  historyAction->trigger();
  check(handledDialogs == 4 && store.records().size() == 6, "all six experimental archives survive reopening");
  const QString replacementPath = root.filePath(QStringLiteral("models/user-replacement.ply"));
  if (!writeBytes(replacementPath, points) || !window.mWorkspace.publishGeneratedScene(
          replacementPath, expected.at(0).resultSceneId, &error)) return false;
  const auto beforeViewingReplacedSlot = window.mWorkspace.sceneObjects();
  mode = Mode::ViewFirst;
  historyAction->trigger();
  check(window.mWorkspace.sceneObjects().size() == beforeViewingReplacedSlot.size() + 1 &&
        window.mWorkspace.activeSceneId() != expected.at(0).resultSceneId &&
        window.mWorkspace.scenePath() == expected.at(0).resultPath,
        "viewing an old experiment appends its original result when its former model slot has been explicitly replaced");
  for (const auto &before : beforeViewingReplacedSlot) {
    bool retainedReplacement = false;
    for (const auto &after : window.mWorkspace.sceneObjects())
      if (before.id == after.id && before.path == after.path && before.translation == after.translation &&
          before.rotation == after.rotation && before.scale == after.scale) retainedReplacement = true;
    check(retainedReplacement, "archive result navigation never overwrites a user replacement or another reference");
  }
  check(store.record(expected.at(0).id).resultSceneId == window.mWorkspace.activeSceneId(),
        "the experiment retains its stable archive identity and rebinds only its new result object");
  if (!QFile::remove(expected.at(1).configurationPath)) return false;
  mode = Mode::InspectMissing;
  historyAction->trigger();
  scripted.stop();
  AppLanguage::apply(QStringLiteral("zh_CN"), false);
  window.mWorkspace.saveManifest(projectFile, &error);
  std::fprintf(stderr, "Generation history smoke test %s\n", passed ? "passed" : "failed");
  std::fflush(stderr);
  return passed;
}
} // namespace gsw
