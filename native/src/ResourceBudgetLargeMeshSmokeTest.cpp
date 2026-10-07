#include "ResourceBudgetLargeMeshSmokeTest.h"

#include "MainWindow.h"
#include "NativeViewport.h"

#include <QAction>
#include <QBuffer>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
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

bool waitUntil(const std::function<bool()> &predicate, const int milliseconds = 60000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < milliseconds) {
    if (predicate()) return true;
    QEventLoop events;
    QTimer::singleShot(20, &events, &QEventLoop::quit);
    events.exec();
  }
  return predicate();
}

bool writeLargeConnectedSurface(const QString &path) {
  constexpr int side = 1600;
  constexpr quint32 rowVertices = 1601;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  // The literal header and assertions below are a known input specification,
  // not values reconstructed from the loader's budget or size estimates.
  const QByteArray header(
      "ply\nformat binary_little_endian 1.0\n"
      "element vertex 2563201\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property float nx\nproperty float ny\nproperty float nz\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\n"
      "element face 5120000\nproperty list uchar int vertex_indices\nend_header\n");
  if (file.write(header) != header.size()) return false;

  QByteArray rowBytes;
  rowBytes.reserve(1601 * 27);
  for (int y = 0; y <= side; ++y) {
    rowBytes.clear();
    QBuffer buffer(&rowBytes);
    if (!buffer.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    for (int x = 0; x <= side; ++x) {
      stream << (20.0F + 2.0F * x / side) << (20.0F + 2.0F * y / side) << 20.0F
             << 0.0F << 0.0F << 1.0F << quint8(225) << quint8(45) << quint8(180);
    }
    if (stream.status() != QDataStream::Ok || file.write(rowBytes) != rowBytes.size()) return false;
    if ((y % 32) == 0) QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
  for (int y = 0; y < side; ++y) {
    rowBytes.clear();
    QBuffer buffer(&rowBytes);
    if (!buffer.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    for (int x = 0; x < side; ++x) {
      const quint32 a = quint32(y) * rowVertices + quint32(x);
      stream << quint8(3) << a << a + 1 << a + rowVertices + 1;
      stream << quint8(3) << a << a + rowVertices + 1 << a + rowVertices;
    }
    if (stream.status() != QDataStream::Ok || file.write(rowBytes) != rowBytes.size()) return false;
    if ((y % 32) == 0) QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
  return file.flush();
}

qint64 coloredSurfacePixels(const QImage &frame) {
  qint64 pixels = 0;
  for (int y = frame.height() / 6; y < frame.height() * 5 / 6; ++y) {
    for (int x = frame.width() / 8; x < frame.width() * 7 / 8; ++x) {
      const QColor color = frame.pixelColor(x, y);
      pixels += color.red() > 70 && color.red() > color.green() * 2 &&
                color.blue() > color.green() * 2;
    }
  }
  return pixels;
}

} // namespace

bool runResourceBudgetLargeMeshSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_LARGE_MESH FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!check(viewport && action && action->isEnabled(), "real viewport and resource settings available")) return false;
  action->trigger();
  QDialog *dialog = nullptr;
  if (!check(waitUntil([&]() {
        dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
        return dialog && dialog->isVisible();
      }, 2500), "settings presents its real dialog")) return false;
  const auto closeDialog = qScopeGuard([&]() { dialog->close(); });
  auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
  auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
  auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
  if (!check(mode && ram && gpu && mode->findData(1) >= 0, "real manual budget controls available")) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(4.0);
  gpu->setValue(1.0);
  const auto policy = viewport->resourceBudgetPolicy();
  if (!check(policy.mode == ResourceBudgetPolicy::Mode::Manual && policy.ramLimitMiB == 4096 &&
             policy.gpuLimitMiB == 1024,
             "literal RAM 4 GiB and GPU 1 GiB ceilings apply without an Apply step")) return false;
  viewport->setEditToolsLocked(true);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  viewport->setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);
  viewport->setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto status = viewport->resourceBudgetStatus();
        return status.loadingMeshCount == 0 && status.managedGpuBytes == 0 &&
            status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
      }, 10000), "preceding scene allocations have ended")) return false;

  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-large-mesh-XXXXXX")));
  if (!check(fixtures.isValid() && QFileInfo(fixtures.path()).absolutePath() ==
             QDir(QCoreApplication::applicationDirPath()).absolutePath(),
             "bounded fixture stays inside the application directory")) return false;
  const auto cleanupScene = qScopeGuard([&]() {
    viewport->setScene({}, 0);
    if (!waitUntil([&]() {
          viewport->grabFramebuffer();
          const auto status = viewport->resourceBudgetStatus();
          return status.loadingMeshCount == 0 && status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
        }, 5000)) {
      // A cancelled UI request does not imply its worker stopped. Preserve its
      // input instead of deleting files that may still be in use on failure.
      fixtures.setAutoRemove(false);
      qWarning() << "RESOURCE_BUDGET_LARGE_MESH: retained active-worker fixture" << fixtures.path();
    }
  });
  const QString modelPath = QDir(fixtures.path()).filePath(QStringLiteral("connected-grid-1600.ply"));
  QElapsedTimer fixtureClock;
  fixtureClock.start();
  if (!check(writeLargeConnectedSurface(modelPath), "bounded 1600-cell connected surface generated")) return false;
  qInfo() << "RESOURCE_BUDGET_LARGE_MESH: generated literal 2563201 vertices / 5120000 triangles"
          << "source bytes" << QFileInfo(modelPath).size() << "fixture milliseconds" << fixtureClock.elapsed();

  bool loaded = false;
  bool failed = false;
  qint64 sourceVertices = 0;
  qsizetype previewVertices = 0;
  qint64 sourceFaces = 0;
  qsizetype previewTriangles = 0;
  const auto loadedConnection = QObject::connect(viewport, &NativeViewport::sceneLoaded, viewport,
      [&](const qint64 vertices, const qsizetype verticesPresented, const qint64 faces,
          const qsizetype trianglesPresented) {
        sourceVertices = vertices;
        previewVertices = verticesPresented;
        sourceFaces = faces;
        previewTriangles = trianglesPresented;
        loaded = true;
      });
  const auto failedConnection = QObject::connect(viewport, &NativeViewport::sceneLoadFailed, viewport,
      [&](const QString &, const QString &message) { failed = true; qCritical() << message; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  QElapsedTimer loadClock;
  loadClock.start();
  viewport->setScene(modelPath, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed,
             "more than five million source faces load successfully")) return false;
  if (!check(sourceVertices == 2563201 && sourceFaces == 5120000 && previewTriangles == 5120000,
             "public load result retains every literal source vertex, face and triangle")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport->setAxisView(NavigationAxis::PositiveZ);
  if (!check(viewport->focusModel(), "large complete model supports ordinary axis navigation and framing")) return false;
  viewport->clearSelection();
  QImage frame;
  const bool presented = waitUntil([&]() {
    frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    const auto status = viewport->resourceBudgetStatus();
    return status.residentMeshCount == 1 && status.pagedMeshCount == 0 && status.loadingMeshCount == 0 &&
        status.reservedRamBytes == 0 && status.reservedGpuBytes == 0 &&
        viewport->meshRenderingAvailable() && viewport->meshPagingSettled() &&
        viewport->renderMode() == NativeViewport::RenderMode::Mesh &&
        !frame.isNull() && coloredSurfacePixels(frame) >= 5000;
  });
  const auto status = viewport->resourceBudgetStatus();
  qInfo() << "RESOURCE_BUDGET_LARGE_MESH: source vertices/faces and presented vertices/triangles"
          << sourceVertices << sourceFaces << previewVertices << previewTriangles
          << "resident/paged/loading" << status.residentMeshCount << status.pagedMeshCount << status.loadingMeshCount
          << "managed RAM/GPU bytes" << status.managedRamBytes << status.managedGpuBytes
          << "reserved RAM/GPU bytes" << status.reservedRamBytes << status.reservedGpuBytes
          << "real colored surface pixels" << coloredSurfacePixels(frame)
          << "complete display milliseconds" << loadClock.elapsed();
  if (!check(presented, "real mesh surface displays completely without fixed face-count paging")) return false;
  if (!check(status.managedGpuBytes > 0 && status.managedGpuBytes <= 1073741824LL &&
             status.managedRamBytes > 0 && status.managedRamBytes <= 4294967296LL,
             "actual managed storage stays within the literal entered ceilings")) return false;

  const QString capturePath = qEnvironmentVariable("GSW_RESOURCE_LARGE_MESH_CAPTURE_PATH");
  if (!capturePath.isEmpty()) {
    if (!check(QFileInfo(capturePath).isAbsolute() &&
               QDir().mkpath(QFileInfo(capturePath).absolutePath()) && frame.save(capturePath),
               "requested actual framebuffer evidence saves at the explicit absolute path")) return false;
  }
  viewport->setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto cleared = viewport->resourceBudgetStatus();
        return cleared.residentMeshCount == 0 && cleared.pagedMeshCount == 0 && cleared.loadingMeshCount == 0 &&
            cleared.managedGpuBytes == 0 && cleared.reservedRamBytes == 0 && cleared.reservedGpuBytes == 0;
      }, 10000), "clearing the model releases real buffers and all loading leases")) return false;
  qInfo() << "RESOURCE_BUDGET_LARGE_MESH PASS: 5120000 triangles fully resident and visible within RAM 4 GiB / GPU 1 GiB";
  return true;
}

} // namespace gsw
