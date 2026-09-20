#include "SpzSmokeTest.h"
#include "MainWindow.h"
#include "ModelExport.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>

namespace gsw {
bool runSpzSmokeTest(MainWindow &window, const QString &source) {
  QTemporaryDir temporary;
  auto *viewport = qobject_cast<NativeViewport *>(window.centralWidget());
  auto *document = window.findChild<WorkspaceDocument *>();
  if (!temporary.isValid() || !viewport || !document) return false;
  const auto check = [](bool value, const char *what) { if (!value) qCritical() << "SPZ_SMOKE FAIL:" << what; return value; };
  const auto wait = [&](const std::function<bool()> &ready) {
    QElapsedTimer elapsed; elapsed.start();
    while (!ready() && elapsed.elapsed() < 30000) {
      QEventLoop loop; QTimer::singleShot(30, &loop, &QEventLoop::quit); loop.exec();
    }
    return ready();
  };
  QString ply = source;
  if (ply.isEmpty()) {
    ply = temporary.filePath(QStringLiteral("source.ply"));
    QFile file(ply);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray bytes = "ply\nformat ascii 1.0\nelement vertex 400\n";
    for (const char *name : {"x","y","z","f_dc_0","f_dc_1","f_dc_2","opacity","scale_0","scale_1","scale_2","rot_0","rot_1","rot_2","rot_3"})
      bytes += QByteArray("property float ") + name + '\n';
    bytes += "end_header\n";
    for (int i = 0; i < 400; ++i) bytes += QByteArray::number(i%20 * .08) + ' ' + QByteArray::number(i/20 * .08) + " 0 1 -0.5 -0.5 3 -3 -3 -3 1 0 0 0\n";
    if (file.write(bytes) != bytes.size()) return false;
  }
  const auto original = WorkspaceDocument::inspectPly(ply);
  if (!original.valid || !original.looksLikeGaussianSplat()) return false;
  const auto root = temporary.filePath(QStringLiteral("working"));
  if (!QDir().mkpath(root) || !document->createUntitled(root)) return false;
  // Dismiss only test-owned dialogs. Treat any unanticipated warning as failure.
  bool unexpected = false;
  bool expectDecodeError = false;
  bool decodeErrorSeen = false;
  QTimer dismiss;
  QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      if (box->objectName() == QStringLiteral("sceneImportChoiceDialog")) {
        if (auto *add = box->findChild<QAbstractButton *>(QStringLiteral("appendSceneButton"))) add->click();
      } else if (expectDecodeError && box->icon() == QMessageBox::Critical) {
        decodeErrorSeen = true; box->accept();
      } else { unexpected = true; qCritical() << box->text(); box->reject(); }
    }
  });
  dismiss.start(30);
  if (!check(window.importSceneFile(ply), "source import") || !wait([&] { return viewport->sourceHasGaussianAttributes(); })) return false;
  const QString spz = temporary.filePath(QStringLiteral("export.spz"));
  bool seen = false;
  QTimer configure;
  QObject::connect(&configure, &QTimer::timeout, &window, [&] {
    auto *dialog = window.findChild<QDialog *>(QStringLiteral("modelExportDialog"));
    if (!dialog || !dialog->isVisible()) return;
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("modelExportFormat"));
    auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("modelExportPath"));
    auto *button = dialog->findChild<QAbstractButton *>(QStringLiteral("confirmModelExport"));
    if (!format || !path || !button) { dialog->reject(); return; }
    configure.stop(); seen = true;
    format->setCurrentIndex(format->findData(static_cast<int>(ModelExportFormat::Spz)));
    path->setText(spz); button->click();
  });
  configure.start(30);
  auto *action = window.findChild<QAction *>(QStringLiteral("exportModelAction"));
  if (!check(action && action->isEnabled(), "export action")) return false;
  action->trigger(); configure.stop();
  if (!check(seen && QFileInfo(spz).size() > 32 && !unexpected, "UI export SPZ")) return false;
  const QString id = document->activeSceneId();
  if (!check(window.importSceneFile(spz), "UI SPZ import")) return false;
  if (!check(wait([&] { return viewport->selectableModelAvailable() && viewport->sourceHasGaussianAttributes() && viewport->scenePath() == document->scenePath(); }), "decoded model displayed")) return false;
  if (!check(document->sceneObjects().size() == 2 && document->activeSceneId() != id &&
      document->sceneMetadata().vertexCount == original.vertexCount && QFileInfo::exists(ply) && QFileInfo::exists(spz), "append preserves source objects and count")) return false;
  const QString project = temporary.filePath(QStringLiteral("saved.gsw"));
  QString error;
  if (!check(document->save(project, &error), "project save")) return false;
  WorkspaceDocument reloaded;
  if (!check(reloaded.load(project, &error) && reloaded.sceneObjects().size() == 2 &&
      reloaded.sceneMetadata().vertexCount == original.vertexCount && QFileInfo::exists(reloaded.scenePath()), "project reload retains managed SPZ work copy")) return false;
  if (!check(wait([&] { return viewport->hasEditableScene() && viewport->sourceHasGaussianAttributes() &&
      viewport->scenePath() == document->scenePath(); }), "managed model reload after save-as")) return false;
  (void)viewport->focusModel();
  const auto image = viewport->grabFramebuffer();
  if (!check(!image.isNull(), "rendered framebuffer")) return false;
  const QString screenshot = qEnvironmentVariable("GSW_LANGUAGE_SCREENSHOT_DIR");
  if (!screenshot.isEmpty()) { QDir().mkpath(screenshot); image.save(QDir(screenshot).filePath(QStringLiteral("spz-round-trip.png"))); }
  // Malformed delivery files must fail before prompting to discard current edits.
  viewport->setEditToolsLocked(false);
  viewport->clearSelection(); viewport->invertSelection(); viewport->deleteSelection();
  if (!check(viewport->hasUnsavedSceneEdits(), "create unsaved crop for failure guard")) return false;
  const auto retainedEdits = viewport->modelExportOptions().deletedVertices;
  const auto retainedScene = document->scenePath();
  const QString invalid = temporary.filePath(QStringLiteral("invalid.spz"));
  QFile bad(invalid);
  if (!bad.open(QIODevice::WriteOnly) || bad.write("not an SPZ") != 10) return false;
  bad.close(); expectDecodeError = true;
  if (!check(!window.importSceneFile(invalid) && decodeErrorSeen && !unexpected &&
      document->scenePath() == retainedScene && document->sceneObjects().size() == 2 &&
      viewport->hasUnsavedSceneEdits() && viewport->modelExportOptions().deletedVertices == retainedEdits,
      "failed SPZ import preserves current object and unsaved edits")) return false;
  viewport->discardSceneEdits();
  qInfo() << "SPZ_SMOKE PASS:" << original.vertexCount << "gaussians; UI export/import, append, managed save/reload; SPZ bytes" << QFileInfo(spz).size();
  return !unexpected;
}
} // namespace gsw
