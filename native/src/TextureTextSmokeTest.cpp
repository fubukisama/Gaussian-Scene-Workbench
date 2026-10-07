#include "TextureTextSmokeTest.h"

#include "AppLanguage.h"
#include "AppTheme.h"
#include "NativeViewport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>

namespace gsw {
namespace {

void processFor(const int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

} // namespace

bool runTextureTextSmokeTest(NativeViewport &viewport, const QString &scenePath) {
  const QString outputPath = qEnvironmentVariable("GSW_TEXTURE_TEXT_CAPTURE_PATH");
  const bool expectTexture = qEnvironmentVariableIntValue("GSW_TEXTURE_TEXT_EXPECT_TEXTURE") != 0;
  if (scenePath.isEmpty() || outputPath.isEmpty() ||
      !QDir().mkpath(QFileInfo(outputPath).absolutePath())) {
    qCritical() << "Texture text capture FAIL: source and output paths are required";
    return false;
  }

  // Pin the public font to keep independent processes comparable at every DPI.
  QFont captureFont(QString::fromLatin1("Segoe UI"));
  captureFont.setPixelSize(16);
  viewport.setFont(captureFont);
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  viewport.setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);

  bool loaded = false;
  bool failed = false;
  qint64 sourceVertices = 0;
  qint64 sourceFaces = 0;
  const auto loadedConnection = QObject::connect(
      &viewport, &NativeViewport::sceneLoaded, &viewport,
      [&](qint64 vertices, qsizetype, qint64 faces, qsizetype) {
        sourceVertices = vertices;
        sourceFaces = faces;
        loaded = true;
      });
  const auto failedConnection = QObject::connect(
      &viewport, &NativeViewport::sceneLoadFailed, &viewport,
      [&](const QString &, const QString &message) {
        qCritical() << "Texture text capture source load failed:" << message;
        failed = true;
      });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  viewport.setScene(scenePath, 0);
  QElapsedTimer loading;
  loading.start();
  while (!loaded && !failed && loading.elapsed() < 15000) processFor(20);
  if (!loaded || failed || sourceVertices <= 0 || sourceFaces <= 0 ||
      !viewport.meshRenderingAvailable()) {
    qCritical() << "Texture text capture FAIL: mesh source did not load correctly"
                << sourceVertices << sourceFaces;
    return false;
  }

  viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport.resetCamera();
  if (!viewport.focusModel()) return false;
  viewport.clearSelection();

  QElapsedTimer rendering;
  rendering.start();
  QImage frame;
  do {
    processFor(20);
    frame = viewport.grabFramebuffer();
  } while (rendering.elapsed() < 15000 &&
           (rendering.elapsed() < 200 || !viewport.meshPagingSettled() ||
            (expectTexture && !viewport.meshTextureAvailable())));
  if (frame.isNull() || !viewport.meshPagingSettled() ||
      viewport.meshTextureAvailable() != expectTexture) {
    qCritical() << "Texture text capture FAIL: actual renderer did not present expected texture state"
                << "expected" << expectTexture << "available" << viewport.meshTextureAvailable();
    return false;
  }

  // These are project-data glyphs, not UI strings. Introduce them only after the
  // mesh shader has actually run, so a previously populated glyph atlas cannot
  // hide corruption affecting the first text upload after a texture draw.
  // Keep the label short: translated statistics may change the card width, but
  // must not make this fixed title elide differently between the two controls.
  viewport.setProjectLabel(QString::fromUtf8(
      "Glyph QXZ 37 \xE7\x94\xB2\xE4\xB9\x99\xE4\xB8\x99 "
      "\xE3\x82\xAB\xE3\x83\x8C \xE6\xBC\xA2\xE5\xAD\x97"));
  viewport.setEditToolsLocked(true);
  QElapsedTimer finalFrames;
  finalFrames.start();
  do {
    processFor(20);
    frame = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  } while (finalFrames.elapsed() < 700);
  if (frame.isNull() || !frame.save(outputPath)) return false;

  // Public widget/framebuffer dimensions describe the screenshot without
  // exporting private overlay geometry or reproducing the renderer's layout.
  const QJsonObject metadata{
      {QString::fromLatin1("logical_width"), viewport.width()},
      {QString::fromLatin1("logical_height"), viewport.height()},
      {QString::fromLatin1("frame_width"), frame.width()},
      {QString::fromLatin1("frame_height"), frame.height()},
      {QString::fromLatin1("device_pixel_ratio"), viewport.devicePixelRatioF()},
      {QString::fromLatin1("source_vertices"), sourceVertices},
      {QString::fromLatin1("source_faces"), sourceFaces},
      {QString::fromLatin1("language"), AppLanguage::current()},
      {QString::fromLatin1("theme"), AppTheme::currentTheme() == UiTheme::Light
          ? QString::fromLatin1("light") : QString::fromLatin1("dark")},
      {QString::fromLatin1("texture_available"), viewport.meshTextureAvailable()}};
  QFile metadataFile(outputPath + QString::fromLatin1(".json"));
  if (!metadataFile.open(QIODevice::WriteOnly) ||
      metadataFile.write(QJsonDocument(metadata).toJson()) < 0) return false;
  qInfo() << "Texture text capture PASS:" << outputPath << frame.size()
          << "texture" << expectTexture << "language" << AppLanguage::current();
  return true;
}

} // namespace gsw
