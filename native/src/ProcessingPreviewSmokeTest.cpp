#include "ProcessingPreviewSmokeTest.h"
#include "AppLanguage.h"
#include "NativeViewport.h"
#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>

namespace gsw {
bool runProcessingPreviewSmokeTest(NativeViewport &viewport) {
  QTemporaryDir directory;
  if (!directory.isValid()) return false;
  const auto fixture = [&](const QString &name, int count, bool gaussian) {
    const QString path = directory.filePath(name + QStringLiteral(".ply"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QString();
    QByteArray header = "ply\nformat ascii 1.0\nelement vertex " + QByteArray::number(count) +
        "\nproperty float x\nproperty float y\nproperty float z\n";
    if (gaussian) header += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
    file.write(header + "end_header\n");
    for (int i = 0; i < count; ++i) file.write(QByteArray::number(i % 3 - 1) + " " + QByteArray::number(i % 2) + " 0" +
        (gaussian ? " 0 0 0 1 -2 -2 -2 1 0 0 0\n" : "\n"));
    return path;
  };
  const auto wait = [](const std::function<bool()> &condition) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 10000) {
      QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
    return condition();
  };
  bool passed = true;
  const auto check = [&](bool ok, const char *message) {
    if (!ok) {
      passed = false;
      std::fprintf(stderr, "Processing preview FAIL: %s\n", message);
      std::fflush(stderr);
      qCritical() << "Processing preview FAIL:" << message;
    }
  };
  int loaded = 0, failures = 0;
  const auto connection = QObject::connect(&viewport, &NativeViewport::sceneLoaded, &viewport,
      [&] { ++loaded; });
  const auto errorConnection = QObject::connect(&viewport, &NativeViewport::sceneLoadFailed, &viewport,
      [&] { ++failures; });
  viewport.setScene(fixture(QStringLiteral("previous"), 6, false), 6);
  check(wait([&] { return loaded == 1; }), "initial model loads");
  viewport.grabFramebuffer();
  viewport.beginProcessingPreview();
  viewport.setProcessingStage(QStringLiteral("colmap"), -1, -1, 25);
  check(viewport.visibleModelAvailable(), "previous geometry stays visible before first reconstruction");
  viewport.setPreviewScene(fixture(QStringLiteral("sparse"), 5, false), 5);
  check(viewport.renderedPointCount() == 6, "old buffers are not cleared on a request");
  check(wait([&] { return loaded == 2; }), "sparse model loads");
  viewport.grabFramebuffer();
  viewport.setEditToolsLocked(true);
  const auto drag = [&](QEvent::Type type, QPointF position, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, viewport.mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &event);
  };
  const QPointF start(viewport.width() * .75, viewport.height() * .4);
  drag(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
  drag(QEvent::MouseMove, start + QPointF(35, 20), Qt::NoButton, Qt::LeftButton);
  drag(QEvent::MouseButtonRelease, start + QPointF(35, 20), Qt::LeftButton, Qt::NoButton);
  const auto target = viewport.viewTarget();
  const auto distance = viewport.viewDistance();
  const auto angles = viewport.viewOrbitAngles();
  const auto cameraUnchanged = [&] {
    const auto now = viewport.viewOrbitAngles();
    return (target - viewport.viewTarget()).length() < 1e-4F &&
        std::abs(distance - viewport.viewDistance()) < 1e-4F &&
        std::abs(angles.yawDegrees - now.yawDegrees) < 1e-4F &&
        std::abs(angles.pitchDegrees - now.pitchDegrees) < 1e-4F;
  };
  viewport.setProcessingStage(QStringLiteral("train"), 0, 1000, 0);
  viewport.setPreviewScene(fixture(QStringLiteral("initial"), 9, true), 9);
  check(viewport.renderedPointCount() == 5, "points remain until Gaussians are ready");
  check(wait([&] { return loaded == 3; }), "initial Gaussians load");
  viewport.grabFramebuffer();
  check(viewport.sourceHasGaussianAttributes(), "point to Gaussian attribute transition");
  check(cameraUnchanged(), "initialization does not reset navigation");
  check(!viewport.hasEditableScene(), "live frame cannot be destructively edited");
  viewport.setPreviewScene(fixture(QStringLiteral("live1"), 12, true), 12);
  viewport.setPreviewScene(fixture(QStringLiteral("live2"), 15, true), 15);
  viewport.setPreviewScene(fixture(QStringLiteral("live3"), 18, true), 18);
  check(wait([&] { viewport.grabFramebuffer(); return viewport.renderedPointCount() == 18; }), "latest queued update is delivered");
  check(loaded == 5, "slow loader coalesces intermediate updates");
  check(cameraUnchanged(), "live updates retain camera");
  viewport.setPreviewScene(directory.filePath(QStringLiteral("missing.ply")), 20);
  check(wait([&] { return failures == 1; }), "failed read is reported");
  check(viewport.renderedPointCount() == 18 && viewport.visibleModelAvailable(), "failed read retains valid frame");
  viewport.setProcessingStage(QStringLiteral("train"), 120, 1000, 12);
  QSet<QString> translated;
  for (const QString language : {QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")}) {
    AppLanguage::apply(language, false);
    translated.insert(viewport.processingPreviewLabel());
    check(viewport.processingPreviewLabel().contains(QStringLiteral("120")), "language switch keeps iteration");
    check(cameraUnchanged(), "language switch keeps camera");
    const QString screenshots = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) viewport.grabFramebuffer().save(screenshots + QStringLiteral("/processing-") + language + QStringLiteral(".png"));
  }
  check(translated.size() == 3, "pipeline is translated immediately");
  viewport.finishProcessingPreview(false, true);
  check(viewport.renderedPointCount() == 6, "cancel returns to the retained original model");
  viewport.setProcessingPreviewVisible(true);
  check(viewport.renderedPointCount() == 18 && !viewport.hasEditableScene(),
        "cancelled task keeps its last valid observation available as a separate read-only view");
  viewport.finishProcessingPreview(true, false);
  SceneObject finalObject;
  finalObject.id = QStringLiteral("final-result");
  finalObject.path = fixture(QStringLiteral("final"), 21, true);
  finalObject.vertexCount = 21;
  viewport.setSceneObjects({finalObject}, finalObject.id);
  check(viewport.renderedPointCount() == 18, "final association does not clear preview");
  check(!viewport.hasEditableScene() && !viewport.selectableModelAvailable(), "final read cannot expose old snapshot as editable source");
  check(viewport.processingPreviewDetail() == QCoreApplication::translate("Workbench", "正在载入最终模型 · 可继续观察"), "final read is explicitly labeled");
  check(wait([&] { viewport.grabFramebuffer(); return viewport.renderedPointCount() == 21; }), "final result replaces preview");
  check(viewport.hasEditableScene() && viewport.selectableModelAvailable(), "full final model restores interaction");
  check(cameraUnchanged(), "final handoff retains camera");
  bool modelReadyPublished = false;
  const auto readyConnection = QObject::connect(&viewport, &NativeViewport::modelInteractionStateChanged,
      &viewport, [&](bool ready) { modelReadyPublished = ready; });
  viewport.beginProcessingPreview();
  viewport.finishProcessingPreview(true, false);
  check(modelReadyPublished && viewport.hasEditableScene(),
        "finishing without a new snapshot does not downgrade the original completed model");
  viewport.setSceneObjects({finalObject}, finalObject.id);
  check(modelReadyPublished, "same-path final adoption republishes readiness without reload");
  QSet<QString> finalLabels;
  for (const QString &language : AppLanguage::supported()) {
    AppLanguage::apply(language, false);
    finalLabels.insert(viewport.processingPreviewDetail());
    check(viewport.processingPreviewDetail() == QCoreApplication::translate("Workbench", "最终模型已就绪 · 编辑工具已锁定"), "terminal label preserves explicit lock in every language");
    check(cameraUnchanged(), "terminal label language switch retains camera");
  }
  check(finalLabels.size() == 3, "terminal detail is translated immediately");
  viewport.beginProcessingPreview();
  viewport.setPreviewScene(fixture(QStringLiteral("late-a"), 23, true), 23);
  viewport.setPreviewScene(fixture(QStringLiteral("late-b"), 26, true), 26);
  viewport.finishProcessingPreview(true, false);
  finalObject.path = fixture(QStringLiteral("new-final"), 30, true);
  finalObject.vertexCount = 30;
  viewport.setSceneObjects({finalObject}, finalObject.id);
  check(!viewport.hasEditableScene(), "in-flight handoff stays read-only");
  check(wait([&] { viewport.grabFramebuffer(); return viewport.renderedPointCount() == 30; }), "full final source supersedes in-flight and queued observations");
  check(viewport.scenePath() == finalObject.path && modelReadyPublished, "late observations cannot replace associated final model");
  check(cameraUnchanged(), "in-flight final adoption preserves camera");
  SceneObject companion;
  companion.id = QStringLiteral("companion");
  companion.path = fixture(QStringLiteral("companion"), 3, true);
  companion.vertexCount = 3;
  viewport.setSceneObjects({finalObject, companion}, companion.id);
  check(wait([&] { return viewport.hasEditableScene(); }), "independent companion object loads");
  viewport.beginProcessingPreview();
  viewport.finishProcessingPreview(true, false);
  viewport.setSceneObjects({finalObject, companion}, finalObject.id);
  check(viewport.selectableModelAvailable() && viewport.hasEditableScene() && modelReadyPublished,
        "existing full object restores tools without sharing the retained original state");
  viewport.beginProcessingPreview();
  viewport.finishProcessingPreview(true, false);
  finalObject.path = directory.filePath(QStringLiteral("invalid-final.ply"));
  viewport.setSceneObjects({finalObject}, finalObject.id);
  check(wait([&] { return failures == 2; }), "invalid final read reports failure");
  check(viewport.visibleModelAvailable() && viewport.renderedPointCount() == 30, "invalid final retains last visible geometry");
  check(!viewport.hasEditableScene() && !viewport.selectableModelAvailable() && !modelReadyPublished, "invalid final never exposes stale geometry to edits");
  check(viewport.processingPreviewDetail() == QCoreApplication::translate("Workbench", "最终模型载入失败 · 保留只读预览"), "invalid final is accurately labeled");
  QObject::disconnect(readyConnection);
  QObject::disconnect(connection);
  QObject::disconnect(errorConnection);
  qInfo() << "Processing preview smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}
}
