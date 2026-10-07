#include "MeshRenderingSmokeTest.h"

#include "AppTheme.h"
#include "NativeViewport.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace gsw {
namespace {

void processFor(const int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

bool writeTiledSurface(const QString &path, const int side) {
  // A genuinely connected surface with distinct vertices and faces. Repeated
  // copies of one triangle would not exercise loss of surface coverage in LOD.
  const quint32 rowWidth = static_cast<quint32>(side + 1);
  const quint32 vertexCount = rowWidth * rowWidth;
  const quint32 triangleCount = static_cast<quint32>(2 * side * side);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray header = QByteArray("ply\nformat binary_little_endian 1.0\nelement vertex ") +
      QByteArray::number(vertexCount) +
      "\nproperty float x\nproperty float y\nproperty float z\n"
      "property float nx\nproperty float ny\nproperty float nz\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face " +
      QByteArray::number(triangleCount) +
      "\nproperty list uchar int vertex_indices\nend_header\n";
  if (file.write(header) != header.size()) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  for (int y = 0; y <= side; ++y) {
    for (int x = 0; x <= side; ++x) {
      stream << (20.0F + 2.0F * x / side) << (20.0F + 2.0F * y / side)
             << 20.0F << 0.0F << 0.0F << 1.0F
             << quint8(225) << quint8(45) << quint8(180);
    }
  }
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      const quint32 a = static_cast<quint32>(y) * rowWidth + x;
      const quint32 b = a + 1;
      const quint32 c = a + rowWidth;
      const quint32 d = c + 1;
      stream << quint8(3) << a << b << d;
      stream << quint8(3) << a << d << c;
    }
  }
  return stream.status() == QDataStream::Ok && file.flush();
}

struct Capture {
  QImage surface;
  QImage background;
  qint64 sourceVertices = 0;
  qint64 sourceFaces = 0;
  qsizetype previewTriangles = 0;
  qsizetype gpuTriangles = 0;
  qint64 parsedMilliseconds = 0;
  qint64 firstVisibleMilliseconds = -1;
  qint64 completeDisplayMilliseconds = 0;
  qint64 settlingMilliseconds = 0;
  QVector3D target;
  OrbitAngles angles;
  float distance = 0.0F;
};

enum class LoadingPolicy { Resident, Paged, Default };

const char *policyName(const LoadingPolicy policy) {
  switch (policy) {
  case LoadingPolicy::Resident: return "resident";
  case LoadingPolicy::Paged: return "paged";
  case LoadingPolicy::Default: return "default";
  }
  return "unknown";
}

bool foregroundAt(const QImage &surface, const QImage &background,
                  int x, int y);

bool capturePolicy(NativeViewport &viewport, const QString &path,
                   const LoadingPolicy policy, const bool tiledFixture, Capture &capture) {
  if (policy == LoadingPolicy::Default) {
    qunsetenv("GSW_MESH_RESIDENT_VERTEX_LIMIT");
    qunsetenv("GSW_MESH_RESIDENT_FACE_LIMIT");
  } else {
    const QByteArray limit = policy == LoadingPolicy::Paged ? "4" : "2147483647";
    qputenv("GSW_MESH_RESIDENT_VERTEX_LIMIT", limit);
    qputenv("GSW_MESH_RESIDENT_FACE_LIMIT", limit);
  }
  viewport.setScene({}, 0); // Reload the same source under the other policy.
  processFor(30);
  bool loaded = false;
  bool failed = false;
  const auto loadedConnection = QObject::connect(
      &viewport, &NativeViewport::sceneLoaded, &viewport,
      [&](qint64 vertices, qsizetype, qint64 faces, qsizetype triangles) {
        capture.sourceVertices = vertices;
        capture.sourceFaces = faces;
        capture.previewTriangles = triangles;
        loaded = true;
      });
  const auto failedConnection = QObject::connect(
      &viewport, &NativeViewport::sceneLoadFailed, &viewport,
      [&](const QString &, const QString &message) {
        qCritical() << "Mesh surface comparison: source load failed:" << message;
        failed = true;
      });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  QElapsedTimer loading;
  loading.start();
  viewport.setScene(path, 0);
  while (!loaded && !failed && loading.elapsed() < 20000) processFor(20);
  if (!loaded || failed) {
    qCritical() << "Mesh surface comparison FAIL: source did not load within 20 seconds" << path;
    return false;
  }
  capture.parsedMilliseconds = loading.elapsed();
  if (!viewport.meshRenderingAvailable()) {
    qCritical() << "Mesh surface comparison FAIL: mesh rendering is unavailable" << path;
    return false;
  }

  viewport.setEditToolsLocked(true);
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  viewport.setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);
  viewport.setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport.resetCamera();
  if (tiledFixture) {
    viewport.setAxisView(NavigationAxis::PositiveZ);
    processFor(300);
  }
  if (!viewport.focusModel()) return false;
  viewport.clearSelection();
  if (viewport.renderMode() != NativeViewport::RenderMode::Mesh) return false;

  // A GPU-count plateau can be an ancestor fallback while the requested surface
  // is still arriving. Use the viewport's public readiness contract, including
  // pending texture uploads, instead of guessing completion from a triangle
  // count or retrying until the coverage assertion passes.
  QElapsedTimer settling;
  settling.start();
  struct TimedFrame { qint64 milliseconds; QImage thumbnail; };
  QVector<TimedFrame> frames;
  do {
    processFor(40);
    capture.surface = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    // The timestamp includes the actual GPU readback, rather than an estimated
    // render rate. Small thumbnails keep the timing trace bounded while the
    // matching empty background is captured through the public transform later.
    frames.append({loading.elapsed(), capture.surface.scaledToWidth(320)});
  } while (settling.elapsed() < 40000 &&
           !viewport.meshPagingSettled());
  capture.settlingMilliseconds = settling.elapsed();
  capture.completeDisplayMilliseconds = loading.elapsed();
  if (!viewport.meshPagingSettled()) {
    qCritical() << "Mesh surface comparison FAIL: viewport surface was not ready within 40 seconds"
                << policyName(policy) << path
                << "GPU triangles" << viewport.residentMeshTriangleCount()
                << "elapsed milliseconds" << capture.settlingMilliseconds;
    const QString directory = qEnvironmentVariable("GSW_MESH_BENCHMARK_DIR");
    if (!directory.isEmpty() && !capture.surface.isNull()) {
      capture.surface.save(QDir(directory).filePath(QString::fromLatin1(policyName(policy)) +
                                                   QString::fromLatin1("-unsettled.png")));
    }
    return false;
  }
  // Keep drawing after readiness so the last queued upload has also been
  // presented by the actual mesh shader before taking the comparison frame.
  QElapsedTimer finalFrames;
  finalFrames.start();
  do {
    processFor(40);
    capture.surface = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  } while (finalFrames.elapsed() < 1000);
  if (!viewport.meshPagingSettled()) {
    qCritical() << "Mesh surface comparison FAIL: viewport surface became unsettled during final frames"
                << policyName(policy) << path;
    return false;
  }
  if (capture.surface.isNull()) return false;
  capture.gpuTriangles = viewport.residentMeshTriangleCount();
  capture.target = viewport.viewTarget();
  capture.angles = viewport.viewOrbitAngles();
  capture.distance = viewport.viewDistance();

  // Moving the model offscreen through its public transform leaves the camera,
  // world-zero grid, and framebuffer unchanged. This supplies a matching empty
  // background without relying on a theme-specific color or renderer internals.
  const QVector3D originalTranslation = viewport.modelTranslation();
  const float offset = std::max(1000.0F, capture.distance * 1000.0F);
  viewport.setModelTranslation(originalTranslation + QVector3D(offset, offset, offset));
  capture.background = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  viewport.setModelTranslation(originalTranslation);
  if (capture.background.isNull() || capture.background.size() != capture.surface.size())
    return false;
  const QImage thumbnailBackground = capture.background.scaledToWidth(320);
  const QRect region(thumbnailBackground.width() / 8, thumbnailBackground.height() / 6,
                     thumbnailBackground.width() * 3 / 4,
                     thumbnailBackground.height() * 2 / 3);
  for (const TimedFrame &frame : frames) {
    qint64 foreground = 0;
    for (int y = region.top(); y <= region.bottom() && foreground < 20; ++y)
      for (int x = region.left(); x <= region.right() && foreground < 20; ++x)
        foreground += foregroundAt(frame.thumbnail, thumbnailBackground, x, y);
    if (foreground >= 20) {
      capture.firstVisibleMilliseconds = frame.milliseconds;
      break;
    }
  }
  qInfo() << "Mesh load timing:" << "policy" << policyName(policy)
          << "source" << path << "source parsed milliseconds" << capture.parsedMilliseconds
          << "first visible milliseconds" << capture.firstVisibleMilliseconds
          << "complete display milliseconds" << capture.completeDisplayMilliseconds
          << "first visible to complete milliseconds"
          << (capture.firstVisibleMilliseconds >= 0
                  ? capture.completeDisplayMilliseconds - capture.firstVisibleMilliseconds : -1)
          << "GPU triangles" << capture.gpuTriangles;
  if (capture.firstVisibleMilliseconds < 0) {
    qCritical() << "Mesh load timing FAIL: no visible surface was presented" << policyName(policy);
    return false;
  }
  return true;
}

bool foregroundAt(const QImage &surface, const QImage &background,
                  const int x, const int y) {
  const QRgb a = reinterpret_cast<const QRgb *>(surface.constScanLine(y))[x];
  const QRgb b = reinterpret_cast<const QRgb *>(background.constScanLine(y))[x];
  return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                   std::abs(qBlue(a) - qBlue(b))}) >= 24;
}

bool saveCapture(const Capture &capture, const QString &directory,
                 const char *policy) {
  if (directory.isEmpty()) return true;
  const QString prefix = QDir(directory).filePath(QString::fromLatin1(policy));
  return capture.surface.save(prefix + QString::fromLatin1("-surface.png")) &&
         capture.background.save(prefix + QString::fromLatin1("-background.png"));
}

} // namespace

bool runMeshRenderingSmokeTest(NativeViewport &viewport, const QString &path) {
  const bool hadVertexLimit = qEnvironmentVariableIsSet("GSW_MESH_RESIDENT_VERTEX_LIMIT");
  const bool hadFaceLimit = qEnvironmentVariableIsSet("GSW_MESH_RESIDENT_FACE_LIMIT");
  const QByteArray vertexLimit = qgetenv("GSW_MESH_RESIDENT_VERTEX_LIMIT");
  const QByteArray faceLimit = qgetenv("GSW_MESH_RESIDENT_FACE_LIMIT");
  const auto restoreEnvironment = qScopeGuard([&]() {
    if (hadVertexLimit) qputenv("GSW_MESH_RESIDENT_VERTEX_LIMIT", vertexLimit);
    else qunsetenv("GSW_MESH_RESIDENT_VERTEX_LIMIT");
    if (hadFaceLimit) qputenv("GSW_MESH_RESIDENT_FACE_LIMIT", faceLimit);
    else qunsetenv("GSW_MESH_RESIDENT_FACE_LIMIT");
  });
  const QString outputDirectory = qEnvironmentVariable("GSW_MESH_BENCHMARK_DIR");
  if (!outputDirectory.isEmpty() && !QDir().mkpath(outputDirectory)) return false;
  QTemporaryDir temporary(QDir(outputDirectory.isEmpty()
      ? QCoreApplication::applicationDirPath() : outputDirectory)
      .filePath(QString::fromLatin1("mesh-surface-XXXXXX")));
  if (!temporary.isValid()) return false;
  const bool tiledFixture = path.isEmpty();
  QString sourcePath = path;
  int side = qEnvironmentVariableIntValue("GSW_MESH_TEST_GRID_SIDE");
  side = side > 0 ? std::clamp(side, 16, 1024) : 768;
  if (tiledFixture) {
    sourcePath = QDir(temporary.path()).filePath(QString::fromLatin1("tiled-surface.ply"));
    if (!writeTiledSurface(sourcePath, side)) return false;
  }

  Capture resident;
  Capture paged;
  const bool defaultPolicy = qEnvironmentVariableIntValue("GSW_MESH_TEST_DEFAULT_POLICY") != 0;
  const LoadingPolicy comparisonPolicy = defaultPolicy ? LoadingPolicy::Default : LoadingPolicy::Paged;
  if (!capturePolicy(viewport, sourcePath, LoadingPolicy::Resident, tiledFixture, resident) ||
      !capturePolicy(viewport, sourcePath, comparisonPolicy, tiledFixture, paged)) return false;
  if (!saveCapture(resident, outputDirectory, "resident") ||
      !saveCapture(paged, outputDirectory, policyName(comparisonPolicy))) return false;
  qInfo() << "Mesh surface comparison: resident source vertices/faces/preview triangles/frame"
          << resident.sourceVertices << resident.sourceFaces << resident.previewTriangles
          << resident.surface.size()
          << "paged source vertices/faces/preview triangles/frame"
          << paged.sourceVertices << paged.sourceFaces << paged.previewTriangles
          << paged.surface.size();
  if (resident.sourceVertices <= 0 || resident.sourceFaces <= 0 ||
      resident.previewTriangles <= 0 || paged.previewTriangles <= 0 ||
      resident.sourceVertices != paged.sourceVertices ||
      resident.sourceFaces != paged.sourceFaces ||
      resident.surface.size() != paged.surface.size()) {
    qCritical() << "Mesh surface comparison FAIL: source counts or frame sizes differ";
    return false;
  }
  if (tiledFixture &&
      (resident.sourceVertices != qint64(side + 1) * (side + 1) ||
       resident.sourceFaces != qint64(2) * side * side ||
       resident.previewTriangles != resident.sourceFaces)) {
    qCritical() << "Mesh surface comparison FAIL: connected fixture counts were not preserved";
    return false;
  }
  const float poseTolerance = std::max(1.0e-5F, resident.distance * 1.0e-5F);
  if ((resident.target - paged.target).length() > poseTolerance ||
      std::abs(resident.distance - paged.distance) > poseTolerance ||
      std::abs(resident.angles.yawDegrees - paged.angles.yawDegrees) > 0.001F ||
      std::abs(resident.angles.pitchDegrees - paged.angles.pitchDegrees) > 0.001F ||
      std::abs(resident.angles.rollDegrees - paged.angles.rollDegrees) > 0.001F) {
    qCritical() << "Mesh surface comparison FAIL: public camera poses differ";
    return false;
  }

  // Exclude viewport chrome. Erode the reference mask by two pixels so a valid
  // coarser silhouette and normal changes cannot fail on antialiased edge pixels.
  const QSize size = resident.surface.size();
  const QRect region(size.width() / 8, size.height() / 6,
                     size.width() * 3 / 4, size.height() * 2 / 3);
  qint64 residentInterior = 0;
  qint64 pagedInterior = 0;
  qint64 pagedForeground = 0;
  for (int y = region.top() + 2; y < region.bottom() - 2; ++y) {
    for (int x = region.left() + 2; x < region.right() - 2; ++x) {
      const bool candidate = foregroundAt(paged.surface, paged.background, x, y);
      pagedForeground += candidate;
      bool interior = foregroundAt(resident.surface, resident.background, x, y);
      if (interior) {
        for (int dy = -2; dy <= 2 && interior; ++dy)
          for (int dx = -2; dx <= 2 && interior; ++dx)
            interior = foregroundAt(resident.surface, resident.background, x + dx, y + dy);
      }
      if (interior) {
        ++residentInterior;
        pagedInterior += candidate;
      }
    }
  }
  const double preservedCoverage = residentInterior > 0
      ? double(pagedInterior) / residentInterior : 0.0;
  qInfo() << "Mesh surface comparison:" << "source" << sourcePath
          << "theme" << (AppTheme::currentTheme() == UiTheme::Light ? "light" : "dark")
          << "fixture side" << (tiledFixture ? side : 0)
          << "vertices" << resident.sourceVertices << "faces" << resident.sourceFaces
          << "resident preview triangles" << resident.previewTriangles
          << "paged preview triangles" << paged.previewTriangles << "frame" << size
          << "resident GPU triangles" << resident.gpuTriangles
          << "paged GPU triangles" << paged.gpuTriangles
          << "resident settling milliseconds" << resident.settlingMilliseconds
          << "paged settling milliseconds" << paged.settlingMilliseconds
          << "resident surface interior pixels" << residentInterior
          << "paged surface interior pixels" << pagedInterior
          << "paged foreground pixels" << pagedForeground
          << "preserved surface coverage" << preservedCoverage;
  // The independent visible-behavior requirement is that paging retains at
  // least 90% of a continuous surface. A point-like sparse subset is not a mesh.
  const int maximumCompleteMilliseconds = qEnvironmentVariableIntValue("GSW_MESH_TEST_MAX_COMPLETE_MS");
  const bool completedPromptly = maximumCompleteMilliseconds <= 0 ||
      paged.completeDisplayMilliseconds <= maximumCompleteMilliseconds;
  // Default-policy regression is opt-in for a bounded model that fits the test
  // device. Its ready-to-display mesh must preserve the resident reference's
  // valid triangles, rather than announce only a sampled preview as loaded.
  const bool completePreview = !defaultPolicy ||
      paged.previewTriangles == resident.previewTriangles;
  const bool preservedSurface = residentInterior >= 1000 && preservedCoverage >= 0.90;
  const bool passed = preservedSurface && completedPromptly && completePreview;
  if (!completedPromptly)
    qCritical() << "Mesh load timing FAIL:" << policyName(comparisonPolicy)
                << "complete display milliseconds" << paged.completeDisplayMilliseconds
                << "exceeds public performance gate" << maximumCompleteMilliseconds;
  if (!completePreview)
    qCritical() << "Mesh load timing FAIL: bounded default-policy model and resident reference"
                << "announced different valid triangle counts:" << paged.previewTriangles << "versus"
                << resident.previewTriangles;
  if (!preservedSurface)
    qCritical() << "Mesh surface comparison FAIL: paged mesh lost continuous surface coverage";
  else if (passed)
    qInfo() << "Mesh surface comparison PASS: paging preserves the visible surface";
  return passed;
}

} // namespace gsw
