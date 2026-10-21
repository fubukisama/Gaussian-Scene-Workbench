#include "PointStreamingSmokeTest.h"

#include "NativeViewport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>

namespace gsw {
namespace {

constexpr qint64 kSourcePoints = 110592; // Independent literal: 48 * 48 * 48.

bool waitUntil(const std::function<bool()> &predicate, const int timeoutMs = 30000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < timeoutMs) {
    if (predicate()) return true;
    QEventLoop events;
    QTimer::singleShot(20, &events, &QEventLoop::quit);
    events.exec();
  }
  return predicate();
}

bool writePointCube(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray header(
      "ply\nformat binary_little_endian 1.0\nelement vertex 110592\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
  if (file.write(header) != header.size()) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  for (int z = 0; z < 48; ++z) {
    for (int y = 0; y < 48; ++y) {
      for (int x = 0; x < 48; ++x) {
        stream << (-1.0F + 2.0F * x / 47.0F)
               << (-1.0F + 2.0F * y / 47.0F)
               << (-1.0F + 2.0F * z / 47.0F)
               << quint8(220) << quint8(35) << quint8(180);
      }
    }
  }
  return stream.status() == QDataStream::Ok && file.flush();
}

QByteArray sourceHash(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result();
}

qint64 coloredPointPixels(const QImage &frame) {
  qint64 result = 0;
  for (int y = frame.height() / 6; y < frame.height() * 5 / 6; ++y) {
    for (int x = frame.width() / 8; x < frame.width() * 7 / 8; ++x) {
      const QColor color = frame.pixelColor(x, y);
      result += color.red() > 100 && color.red() > color.green() * 2 &&
                color.blue() > 90 && color.blue() > color.green() * 2;
    }
  }
  return result;
}

} // namespace

bool runPointStreamingSmokeTest(NativeViewport &viewport) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "POINT_STREAMING FAIL:" << message;
    return condition;
  };
  // This acceptance path runs in a dedicated smoke process, never against an
  // existing project or training session. It neither opens nor raises windows.
  if (!check(viewport.scenePath().isEmpty() && viewport.sceneObjectCount() <= 1 &&
             !viewport.hasProcessingPreview() &&
             !viewport.trainingGpuPreviewActive(),
             "dedicated empty viewport has no user model or training preview")) return false;

  const auto oldPolicy = viewport.resourceBudgetPolicy();
  const bool oldLocked = viewport.editToolsLocked();
  const bool oldTrackball = viewport.observationTrackballVisible();
  const auto oldReferencePlane = viewport.referencePlaneMode();
  const QSize oldMinimum = viewport.minimumSize();
  const QSize oldMaximum = viewport.maximumSize();
  const auto restorePolicy = qScopeGuard([&]() {
    viewport.setResourceBudgetPolicy(oldPolicy);
    viewport.setEditToolsLocked(oldLocked);
    viewport.setShowObservationTrackball(oldTrackball);
    viewport.setReferencePlaneMode(oldReferencePlane);
    viewport.setMinimumSize(oldMinimum);
    viewport.setMaximumSize(oldMaximum);
  });
  // Normalize the acceptance framebuffer's pixel work, as the navigation
  // smoke does. This does not change the production loader or paging policy.
  viewport.setFixedSize(QSize(640, 400));
  ResourceBudgetPolicy bounded;
  bounded.mode = ResourceBudgetPolicy::Mode::Manual;
  bounded.ramLimitMiB = 128;
  bounded.gpuLimitMiB = 64;
  viewport.setResourceBudgetPolicy(bounded);
  viewport.setEditToolsLocked(true);
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  viewport.setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);
  viewport.grabFramebuffer();
  const auto capacity = viewport.resourceBudgetStatus();
  if (!check(capacity.effectiveRamBudgetBytes >= 32LL * 1024 * 1024 &&
             capacity.effectiveGpuBudgetBytes >= 16LL * 1024 * 1024,
             "bounded fixture has independently observed safe RAM/GPU headroom")) return false;

  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("point-streaming-XXXXXX")));
  if (!check(fixtures.isValid() && QFileInfo(fixtures.path()).absolutePath() ==
             QDir(QCoreApplication::applicationDirPath()).absolutePath(),
             "fixture stays inside the smoke application's directory")) return false;
  const auto clearFixtureScene = qScopeGuard([&]() {
    viewport.setScene({}, 0);
    if (!waitUntil([&]() {
          viewport.grabFramebuffer();
          const auto status = viewport.resourceBudgetStatus();
          return status.loadingMeshCount == 0 && status.reservedRamBytes == 0 &&
                 status.reservedGpuBytes == 0 && status.managedGpuBytes == 0;
        }, 5000)) {
      fixtures.setAutoRemove(false);
      qWarning() << "POINT_STREAMING: retained fixture for active work" << fixtures.path();
    }
  });
  const auto path = fixtures.filePath(QStringLiteral("colored-cube-48.ply"));
  if (!check(writePointCube(path), "literal 110592 colored source points generated")) return false;
  const auto originalHash = sourceHash(path);
  if (!check(originalHash.size() == 32, "source SHA-256 is available")) return false;

  const bool flagWasSet = qEnvironmentVariableIsSet("GSW_FORCE_POINT_PAGING");
  const QByteArray oldFlag = qgetenv("GSW_FORCE_POINT_PAGING");
  const auto restoreFlag = [&]() {
    if (flagWasSet) qputenv("GSW_FORCE_POINT_PAGING", oldFlag);
    else qunsetenv("GSW_FORCE_POINT_PAGING");
  };
  const auto restoreEnvironment = qScopeGuard(restoreFlag);
  qputenv("GSW_FORCE_POINT_PAGING", "1");

  bool loaded = false;
  bool failed = false;
  qint64 sourceCount = 0;
  quint64 presentedFrames = 0;
  const auto loadedConnection = QObject::connect(&viewport, &NativeViewport::sceneLoaded, &viewport,
      [&](const qint64 sourceVertices, const qsizetype, const qint64 sourceFaces,
          const qsizetype) {
        sourceCount = sourceVertices;
        loaded = sourceFaces == 0;
      });
  const auto failedConnection = QObject::connect(&viewport, &NativeViewport::sceneLoadFailed, &viewport,
      [&](const QString &, const QString &message) { failed = true; qCritical() << message; });
  const auto frameConnection = QObject::connect(&viewport, &QOpenGLWidget::frameSwapped, &viewport,
      [&]() { ++presentedFrames; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
    QObject::disconnect(frameConnection);
  });
  QElapsedTimer clock;
  clock.start();
  viewport.setScene(path, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed &&
             sourceCount == kSourcePoints,
             "real viewport imports the complete independent point count")) return false;
  // Keep the diagnostic override through asynchronous load completion only.
  restoreFlag();
  if (!check(!viewport.hasEditableScene() && !viewport.meshRenderingAvailable() &&
             !viewport.sourceHasGaussianAttributes(),
             "forced paging is a read-only ordinary point-cloud display")) return false;

  const auto cache = PointCloudCache::loadForSource(path);
  qint64 leafCount = 0;
  qint64 leafPoints = 0;
  for (const auto &node : cache.nodes) {
    if (node.isLeaf() && node.sourcePointCount > 0) {
      ++leafCount;
      leafPoints += node.sourcePointCount;
    }
  }
  if (!check(cache.isValid() && cache.fullPointCount == kSourcePoints &&
             leafCount == 512 && leafPoints == kSourcePoints,
             "real on-disk octree preserves every point across 512 populated leaves")) return false;

  viewport.setRenderMode(NativeViewport::RenderMode::Points);
  viewport.setAxisView(NavigationAxis::PositiveZ);
  if (!check(viewport.focusModel(), "ordinary public camera framing succeeds")) return false;
  viewport.clearSelection();
  const auto translation = viewport.modelTranslation();
  const auto rotation = viewport.modelRotation();
  const auto scale = viewport.modelScale();
  QImage frame;
  if (!check(waitUntil([&]() {
        frame = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        return viewport.residentPointCount() > 0 && coloredPointPixels(frame) >= 64;
      }), "actual framebuffer presents colored point pages")) return false;
  const qint64 firstVisibleMs = clock.elapsed();
  const quint64 firstVisibleFrames = presentedFrames;
  qWarning() << "POINT_STREAMING_VISIBLE:" << "elapsed milliseconds" << firstVisibleMs
             << "frameSwapped" << firstVisibleFrames
             << "resident/source points" << viewport.residentPointCount() << kSourcePoints
             << "viewport/framebuffer" << viewport.size() << frame.size();
  // Observe the actual refresh/event-loop path while it streams. Qt's
  // grabFramebuffer renders synchronously, so calling it on every poll adds
  // a second paint and can delay GUI-thread delivery of read completions.
  const bool ready = waitUntil([&]() {
        const auto status = viewport.resourceBudgetStatus();
        return viewport.pointPagingSettled() && viewport.residentPointCount() >= kSourcePoints &&
               status.loadingMeshCount == 0 && status.reservedRamBytes == 0 &&
               status.reservedGpuBytes == 0;
      });
  frame = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  if (!ready) {
    const auto stalled = viewport.resourceBudgetStatus();
    qCritical() << "POINT_STREAMING_STATE:" << "elapsed milliseconds" << clock.elapsed()
                << "first-visible milliseconds/frameSwapped" << firstVisibleMs << firstVisibleFrames
                << "current frameSwapped" << presentedFrames
                << "resident/source points" << viewport.residentPointCount() << kSourcePoints
                << "paging settled/loading" << viewport.pointPagingSettled() << stalled.loadingMeshCount
                << "reserved RAM/GPU bytes" << stalled.reservedRamBytes << stalled.reservedGpuBytes
                << "managed RAM/GPU bytes" << stalled.managedRamBytes << stalled.managedGpuBytes
                << "effective RAM/GPU budgets" << stalled.effectiveRamBudgetBytes << stalled.effectiveGpuBudgetBytes
                << "colored framebuffer pixels" << coloredPointPixels(frame)
                << "viewport/framebuffer" << viewport.size() << frame.size()
                << "view distance/orthographic/angles" << viewport.viewDistance()
                << viewport.orthographicProjection() << viewport.viewOrbitAngles().yawDegrees
                << viewport.viewOrbitAngles().pitchDegrees << viewport.viewOrbitAngles().rollDegrees
                << "scene/source unchanged" << (viewport.scenePath() == path)
                << (sourceHash(path) == originalHash);
    const QString output = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
    if (!output.isEmpty() && QFileInfo(output).isAbsolute() && QDir().mkpath(output)) {
      const QString capture = QDir(output).filePath(QStringLiteral("point-streaming-failed.png"));
      qCritical() << "POINT_STREAMING_FAILURE_CAPTURE:" << capture << frame.save(capture);
    }
  }
  if (!check(ready, "all requested point pages settle and release their loading leases")) return false;
  if (!check(coloredPointPixels(frame) >= 64,
             "completed real framebuffer still presents colored point pages")) return false;
  const qint64 readyMs = clock.elapsed();
  const quint64 readyFrames = presentedFrames;
  const auto status = viewport.resourceBudgetStatus();
  const auto residentPoints = viewport.residentPointCount();
  if (!check(status.managedGpuBytes > 0 && status.managedGpuBytes <= 64LL * 1024 * 1024 &&
             status.managedRamBytes <= 128LL * 1024 * 1024 &&
             viewport.scenePath() == path && viewport.modelTranslation() == translation &&
             viewport.modelRotation() == rotation && viewport.modelScale() == scale &&
             sourceHash(path) == originalHash && !viewport.hasUnsavedSceneEdits(),
             "bounded streaming does not modify the source, model transform or scene edits")) return false;

  const QString output = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
  if (!output.isEmpty() &&
      !check(QFileInfo(output).isAbsolute() && QDir().mkpath(output) &&
             frame.save(QDir(output).filePath(QStringLiteral("point-streaming.png"))),
             "actual framebuffer evidence saves at the explicit output directory")) return false;
  viewport.setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport.grabFramebuffer();
        const auto cleared = viewport.resourceBudgetStatus();
        return cleared.loadingMeshCount == 0 && cleared.reservedRamBytes == 0 &&
               cleared.reservedGpuBytes == 0 && cleared.managedGpuBytes == 0 &&
               viewport.residentPointCount() == 0;
      }, 10000), "clearing the fixture releases point buffers and loading leases")) return false;

  // Exercise only public lifecycle operations while page jobs still own their
  // buffers. A cache/scene replacement must not apply stale results, release a
  // worker lane early, strand reservations, or prevent a later explicit retry.
  loaded = false;
  failed = false;
  qputenv("GSW_FORCE_POINT_PAGING", "1");
  viewport.setScene(path, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed,
             "the same unchanged source can be explicitly reopened for lifecycle acceptance")) return false;
  restoreFlag();
  if (!check(waitUntil([&]() {
        viewport.grabFramebuffer();
        return viewport.residentPointCount() > 0 && !viewport.pointPagingSettled();
      }), "public viewport exposes a partially resident stream before clearing")) return false;
  viewport.setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport.grabFramebuffer();
        const auto cleared = viewport.resourceBudgetStatus();
        return cleared.loadingMeshCount == 0 && cleared.reservedRamBytes == 0 &&
               cleared.reservedGpuBytes == 0 && cleared.managedGpuBytes == 0 &&
               viewport.residentPointCount() == 0;
      }, 10000), "clearing in-flight point pages drains their actual leases without stale residency")) return false;

  loaded = false;
  failed = false;
  qputenv("GSW_FORCE_POINT_PAGING", "1");
  viewport.setScene(path, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed,
             "explicit retry imports the same source after an in-flight clear")) return false;
  restoreFlag();
  viewport.setRenderMode(NativeViewport::RenderMode::Points);
  viewport.setAxisView(NavigationAxis::PositiveZ);
  if (!check(viewport.focusModel(), "retry uses the same public camera framing")) return false;
  if (!check(waitUntil([&]() {
        const auto retried = viewport.resourceBudgetStatus();
        return viewport.pointPagingSettled() && viewport.residentPointCount() >= kSourcePoints &&
               retried.loadingMeshCount == 0 && retried.reservedRamBytes == 0 &&
               retried.reservedGpuBytes == 0;
      }), "retry settles under the unchanged thirty-second gate after cancelling page work")) return false;
  const auto retriedFrame = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  if (!check(coloredPointPixels(retriedFrame) >= 64 && sourceHash(path) == originalHash &&
             viewport.scenePath() == path && !viewport.hasUnsavedSceneEdits(),
             "retry renders actual colored pages and preserves source bytes and edits")) return false;
  viewport.setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport.grabFramebuffer();
        const auto cleared = viewport.resourceBudgetStatus();
        return cleared.loadingMeshCount == 0 && cleared.reservedRamBytes == 0 &&
               cleared.reservedGpuBytes == 0 && cleared.managedGpuBytes == 0 &&
               viewport.residentPointCount() == 0;
      }, 10000), "retry cleanup releases all point storage and loading leases")) return false;
  qInfo() << "POINT_STREAMING PASS:" << "source points/leaves" << kSourcePoints << leafCount
          << "source bytes" << QFileInfo(path).size()
          << "first-visible/ready milliseconds" << firstVisibleMs << readyMs
          << "frameSwapped first-visible/ready" << firstVisibleFrames << readyFrames
          << "resident points" << residentPoints
          << "colored framebuffer pixels" << coloredPointPixels(frame)
          << "managed RAM/GPU bytes" << status.managedRamBytes << status.managedGpuBytes
          << "source SHA256" << originalHash.toHex();
  return true;
}

} // namespace gsw
