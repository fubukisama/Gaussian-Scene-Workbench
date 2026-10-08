#include "TrainingIsolationSmokeTest.h"
#include "AppLanguage.h"
#include "NativeViewport.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool waitFor(const std::function<bool()> &condition) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < 10000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return condition();
}
QString writePoints(const QTemporaryDir &directory, const QString &name, int count) {
  QFile file(directory.filePath(name + QStringLiteral(".ply")));
  if (!file.open(QIODevice::WriteOnly)) return {};
  file.write("ply\nformat ascii 1.0\nelement vertex " + QByteArray::number(count) +
             "\nproperty float x\nproperty float y\nproperty float z\n"
             "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
  for (int index = 0; index < count; ++index)
    file.write(QByteArray::number((index % 5) * .1) + " " +
               QByteArray::number((index / 5) * .1) + " 0 231 47 193\n");
  return file.fileName();
}
}

bool runTrainingIsolationSmokeTest(NativeViewport &viewport) {
  QTemporaryDir directory;
  if (!directory.isValid()) return false;
  const auto check = [](bool okay, const char *message) {
    std::fprintf(stderr, "Training isolation %s: %s\n", okay ? "PASS" : "FAIL", message);
    std::fflush(stderr);
    if (!okay) qCritical() << "Training isolation FAIL:" << message;
    return okay;
  };
  SceneObject first;
  first.id = QStringLiteral("reference-first");
  first.path = writePoints(directory, QStringLiteral("first"), 15);
  first.vertexCount = 15;
  first.translation = {1.25F, -2.5F, .75F};
  first.rotation = QQuaternion::fromAxisAndAngle(0, 0, 1, 30);
  first.scale = {1.5F, 2.F, .5F};
  SceneObject second;
  second.id = QStringLiteral("reference-second");
  second.path = writePoints(directory, QStringLiteral("second"), 20);
  second.vertexCount = 20;
  second.translation = {-3.F, 1.F, 2.F};
  second.rotation = QQuaternion::fromAxisAndAngle(1, 0, 0, -20);
  second.scale = {.75F, .5F, 1.25F};
  const QString preview = writePoints(directory, QStringLiteral("task-preview"), 30);
  if (first.path.isEmpty() || second.path.isEmpty() || preview.isEmpty()) return false;
  viewport.setSceneObjects({first, second}, first.id);
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == first.path && viewport.renderedPointCount() == 15;
      }), "first reference is loaded through the real viewport")) return false;
  if (!check(viewport.activateSceneObject(second.id) && waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == second.path && viewport.renderedPointCount() == 20;
      }), "second reference is independently loaded")) return false;
  viewport.activateSceneObject(first.id);
  if (!check(viewport.focusModel(), "the original reference is framed through the normal navigation interface")) return false;
  const QImage referenceFrame = viewport.grabFramebuffer();
  const auto referenceTarget = viewport.viewTarget();
  const auto referenceDistance = viewport.viewDistance();
  const auto referenceAngles = viewport.viewOrbitAngles();
  const auto referenceGpuBytes = viewport.resourceBudgetStatus().managedGpuBytes;
  if (!check(referenceGpuBytes > 0, "reference display allocations are accounted before training")) return false;
  QVector<QPoint> referencePixels;
  for (int y = referenceFrame.height() / 4; y < referenceFrame.height() * 3 / 4; ++y) {
    for (int x = referenceFrame.width() / 4; x < referenceFrame.width() * 3 / 4; ++x) {
      const QRgb pixel = referenceFrame.pixel(x, y);
      if (qRed(pixel) > 120 && qBlue(pixel) > 100 && qGreen(pixel) < 100 &&
          qRed(pixel) - qGreen(pixel) > 60 && qBlue(pixel) - qGreen(pixel) > 40)
        referencePixels.append({x, y});
    }
  }
  std::fprintf(stderr, "Reference display: %lld model-colored pixels, %lld managed GPU bytes\n",
               static_cast<long long>(referencePixels.size()), static_cast<long long>(referenceGpuBytes));
  std::fflush(stderr);
  if (!check(!referencePixels.isEmpty(), "the reference fixture has visible model-colored pixels in the real framebuffer")) return false;
  const QStringList intensiveStages{QStringLiteral("train"), QStringLiteral("mesh"), QStringLiteral("texture")};
  for (int stageIndex = 0; stageIndex < intensiveStages.size(); ++stageIndex) {
    const auto &stage = intensiveStages[stageIndex];
    viewport.beginProcessingPreview();
    viewport.setProcessingStage(stage, 0, 100, 0);
    std::fprintf(stderr, "Training priority first-frame stage: %s\n", stage.toUtf8().constData());
    std::fflush(stderr);
    if (!check(waitFor([&] {
          viewport.grabFramebuffer();
          return viewport.resourceBudgetStatus().managedGpuBytes < referenceGpuBytes;
        }), "a GPU-intensive stage yields reference display storage before the first task frame")) return false;
    const QImage waitingFrame = viewport.grabFramebuffer();
    int retainedModelPixels = 0;
    if (waitingFrame.size() == referenceFrame.size()) {
      for (const QPoint &point : referencePixels) {
        const QRgb before = referenceFrame.pixel(point);
        const QRgb waiting = waitingFrame.pixel(point);
        if (std::abs(qRed(before) - qRed(waiting)) <= 25 &&
            std::abs(qGreen(before) - qGreen(waiting)) <= 25 &&
            std::abs(qBlue(before) - qBlue(waiting)) <= 25) ++retainedModelPixels;
      }
    }
    if (!check(!viewport.hasProcessingPreview() && viewport.sceneObjectCount() == 2 &&
               viewport.activeSceneId() == first.id && retainedModelPixels * 2 >= referencePixels.size(),
               "waiting for the first task frame keeps a static visible reference image, not a blank viewport")) return false;
    viewport.setProcessingPreviewVisible(false);
    if (!check(waitFor([&] {
          viewport.grabFramebuffer();
          return viewport.resourceBudgetStatus().managedGpuBytes >= referenceGpuBytes &&
              viewport.renderedPointCount() == 15 && viewport.scenePath() == first.path;
        }), "explicitly viewing the reference restores its GPU storage before any task frame")) return false;
    if (!check(viewport.modelTranslation() == first.translation &&
               viewport.modelRotation() == first.rotation && viewport.modelScale() == first.scale &&
               viewport.viewTarget() == referenceTarget && viewport.viewDistance() == referenceDistance &&
               viewport.viewOrbitAngles() == referenceAngles,
               "first-frame waiting and reference restoration preserve object transforms and camera")) return false;
    viewport.finishProcessingPreview(stageIndex == 2, stageIndex == 0, stageIndex == 1);
    if (!check(viewport.hasEditableScene() && viewport.scenePath() == first.path &&
               viewport.sceneObjectCount() == 2 && viewport.modelTranslation() == first.translation &&
               viewport.modelRotation() == first.rotation && viewport.modelScale() == first.scale,
               "success, cancellation and pause without a task snapshot leave the original model usable")) return false;
  }
  viewport.beginProcessingPreview();
  viewport.setPreviewScene(preview, 30);
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == preview && viewport.renderedPointCount() == 30;
      }), "new task becomes visible without a trainer or external process")) return false;
  if (!check(viewport.hasProcessingPreview() && viewport.processingPreviewVisible() &&
             viewport.sceneObjectCount() == 2 && viewport.activeSceneId() == first.id,
             "the task display is separate from persisted reference identities")) return false;
  if (!check(viewport.resourceBudgetStatus().managedGpuBytes < referenceGpuBytes,
             "showing the task releases reference display GPU storage")) return false;
  const QPointF center = viewport.rect().center();
  QWheelEvent zoom(center, viewport.mapToGlobal(center.toPoint()), {}, {0, -120},
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&viewport, &zoom);
  const auto previewTarget = viewport.viewTarget();
  const auto previewDistance = viewport.viewDistance();
  const auto previewAngles = viewport.viewOrbitAngles();
  viewport.setProcessingPreviewVisible(false);
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == first.path && viewport.renderedPointCount() == 15 &&
            viewport.resourceBudgetStatus().managedGpuBytes >= referenceGpuBytes;
      }), "viewing references restores their real display buffers")) return false;
  if (!check(!viewport.processingPreviewVisible() && viewport.hasProcessingPreview() &&
             viewport.modelTranslation() == first.translation &&
             viewport.modelRotation() == first.rotation && viewport.modelScale() == first.scale &&
             viewport.viewTarget() == referenceTarget && viewport.viewDistance() == referenceDistance &&
             viewport.viewOrbitAngles() == referenceAngles,
             "reference view and transforms are not replaced by task navigation")) return false;
  viewport.setProcessingPreviewVisible(true);
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == preview && viewport.renderedPointCount() == 30;
      }), "the retained task preview can be shown again")) return false;
  if (!check(viewport.viewTarget() == previewTarget && viewport.viewDistance() == previewDistance &&
             viewport.viewOrbitAngles() == previewAngles,
             "task navigation is remembered independently from the reference view")) return false;
  auto *previewAction = viewport.window()->findChild<QAction *>(QStringLiteral("taskPreviewAction"));
  if (!check(previewAction && previewAction->isEnabled() && previewAction->isChecked(),
             "the real task-preview action reflects the visible task layer")) return false;
  const QString initialLanguage = AppLanguage::current();
  QSet<QString> actionLabels;
  QSet<QString> actionTooltips;
  for (const auto &language : AppLanguage::supported()) {
    AppLanguage::apply(language, false);
    actionLabels.insert(previewAction->text());
    actionTooltips.insert(previewAction->toolTip());
    if (!check(previewAction->text() == QCoreApplication::translate("Workbench", "任务预览") &&
               previewAction->isEnabled() && previewAction->isChecked() &&
               viewport.scenePath() == preview && viewport.activeSceneId() == first.id &&
               viewport.sceneObjectCount() == 2 && viewport.viewTarget() == previewTarget &&
               viewport.viewDistance() == previewDistance && viewport.viewOrbitAngles() == previewAngles,
               "language selection refreshes the real action without rebuilding the task or reference views")) return false;
  }
  if (!check(actionLabels.size() == 3 && actionTooltips.size() == 3,
             "the task-preview action and help are distinct in Chinese, English and Japanese")) return false;
  previewAction->trigger();
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return !previewAction->isChecked() && !viewport.processingPreviewVisible() &&
            viewport.scenePath() == first.path && viewport.renderedPointCount() == 15;
      }), "triggering the real action switches to the original scene")) return false;
  if (!check(viewport.modelTranslation() == first.translation &&
             viewport.modelRotation() == first.rotation && viewport.modelScale() == first.scale,
             "the user-facing switch keeps the original model transform")) return false;
  previewAction->trigger();
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return previewAction->isChecked() && viewport.processingPreviewVisible() &&
            viewport.scenePath() == preview && viewport.renderedPointCount() == 30;
      }), "triggering the real action again returns to the task preview")) return false;
  AppLanguage::apply(initialLanguage, false);
  viewport.setProcessingStage(QStringLiteral("train"), 1, 30000, 0);
  int presentedFrames = 0;
  const auto frameConnection = QObject::connect(&viewport, &NativeViewport::frameMetricsChanged,
      &viewport, [&] { ++presentedFrames; });
  QEventLoop frameWindow;
  QTimer::singleShot(1200, &frameWindow, &QEventLoop::quit);
  frameWindow.exec();
  QObject::disconnect(frameConnection);
  std::fprintf(stderr, "Training live frames: %d completed frames in 1200 ms\n", presentedFrames);
  std::fflush(stderr);
  if (!check(presentedFrames > 0, "the visible task continues presenting real completed frames")) return false;
  if (!check(presentedFrames <= 45,
             "training observation is capped near 30 FPS instead of competing at unlimited refresh")) return false;
  viewport.finishProcessingPreview(false, true);
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == first.path && viewport.renderedPointCount() == 15;
      }), "cancelling a task restores the original active model, not its last preview")) return false;
  if (!check(viewport.sceneObjectCount() == 2 && viewport.activeSceneId() == first.id &&
             viewport.modelTranslation() == first.translation &&
             viewport.modelRotation() == first.rotation && viewport.modelScale() == first.scale,
             "the original active identity and transform survive cancellation")) return false;
  previewAction->trigger();
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return previewAction->isChecked() && viewport.scenePath() == preview &&
            viewport.renderedPointCount() == 30 && !viewport.hasEditableScene();
      }), "a completed task observation remains available from the real action")) return false;
  const auto terminalPreviewTarget = viewport.viewTarget();
  const auto terminalPreviewDistance = viewport.viewDistance();
  const auto terminalPreviewAngles = viewport.viewOrbitAngles();
  viewport.setSceneObjects({first, second}, first.id);
  if (!check(viewport.scenePath() == preview && viewport.processingPreviewVisible() &&
             previewAction->isChecked() && viewport.sceneObjectCount() == 2 &&
             viewport.activeSceneId() == first.id && !viewport.hasEditableScene() &&
             viewport.viewTarget() == terminalPreviewTarget &&
             viewport.viewDistance() == terminalPreviewDistance &&
             viewport.viewOrbitAngles() == terminalPreviewAngles,
             "synchronizing the unchanged project cannot interrupt a manually selected terminal preview")) return false;
  viewport.clearProcessingPreview();
  if (!check(!viewport.hasProcessingPreview() && !viewport.processingPreviewVisible() &&
             !previewAction->isEnabled() && !previewAction->isChecked() &&
             viewport.processingPreviewLabel().isEmpty() && viewport.processingPreviewDetail().isEmpty(),
             "an explicit project-context reset clears only the transient task view and its user action")) return false;
  // Another project may link exactly the same models and retain their stable
  // IDs. Explicit context reset must still remove the old task observation.
  viewport.setSceneObjects({first, second}, first.id);
  if (!check(waitFor([&] {
        viewport.grabFramebuffer();
        return viewport.scenePath() == first.path && viewport.renderedPointCount() == 15 &&
            viewport.resourceBudgetStatus().managedGpuBytes >= referenceGpuBytes;
      }), "the new context shows the original references even when their IDs and paths are unchanged")) return false;
  if (!check(viewport.sceneObjectCount() == 2 && viewport.activeSceneId() == first.id &&
             viewport.hasEditableScene() && viewport.modelTranslation() == first.translation &&
             viewport.modelRotation() == first.rotation && viewport.modelScale() == first.scale &&
             viewport.viewTarget() == referenceTarget && viewport.viewDistance() == referenceDistance &&
             viewport.viewOrbitAngles() == referenceAngles && !viewport.hasProcessingPreview(),
             "clearing task context preserves editable reference identities, transforms and camera")) return false;
  if (!check(viewport.activateSceneObject(second.id) && viewport.scenePath() == second.path &&
             viewport.modelTranslation() == second.translation &&
             viewport.modelRotation() == second.rotation && viewport.modelScale() == second.scale,
             "the other reference model and transform also survive cancellation")) return false;
  viewport.setSceneObjects({}, {});
  std::fprintf(stderr, "TRAINING_ISOLATION PASS: separate task/reference views, GPU surrender and cancellation retention\n");
  std::fflush(stderr);
  qInfo() << "TRAINING_ISOLATION PASS: separate task/reference views, GPU surrender and cancellation retention";
  return true;
}
}
