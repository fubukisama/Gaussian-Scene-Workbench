#include "ResourceBudgetSmokeTest.h"

#include "MainWindow.h"
#include "NativeViewport.h"
#include "AppLanguage.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QColor>
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
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <functional>

namespace gsw {
namespace {

bool waitUntil(const std::function<bool()> &predicate, const int milliseconds = 20000) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < milliseconds) {
    if (predicate()) return true;
    QEventLoop events;
    QTimer::singleShot(20, &events, &QEventLoop::quit);
    events.exec();
  }
  return predicate();
}

bool writeSurface(const QString &path, const int side,
                  const QColor color = QColor(225, 45, 180)) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const quint32 row = static_cast<quint32>(side + 1);
  const QByteArray header = QByteArray("ply\nformat binary_little_endian 1.0\nelement vertex ") +
      QByteArray::number(row * row) +
      "\nproperty float x\nproperty float y\nproperty float z\n"
      "property float nx\nproperty float ny\nproperty float nz\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face " +
      QByteArray::number(2 * side * side) +
      "\nproperty list uchar int vertex_indices\nend_header\n";
  if (file.write(header) != header.size()) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  for (int y = 0; y <= side; ++y) {
    for (int x = 0; x <= side; ++x) {
      stream << (20.0F + 2.0F * x / side) << (20.0F + 2.0F * y / side) << 20.0F
             << 0.0F << 0.0F << 1.0F << quint8(color.red()) << quint8(color.green()) << quint8(color.blue());
    }
  }
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      const quint32 a = quint32(y) * row + quint32(x);
      stream << quint8(3) << a << a + 1 << a + row + 1;
      stream << quint8(3) << a << a + row + 1 << a + row;
    }
  }
  return stream.status() == QDataStream::Ok && file.flush();
}

bool writeTexturedSurface(const QString &path, const QString &atlasName) {
  constexpr int side = 256;
  constexpr quint32 row = 257;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray header = QByteArray("ply\nformat binary_little_endian 1.0\ncomment TextureFile ") +
      atlasName.toUtf8() + "\nelement vertex 66049\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property float nx\nproperty float ny\nproperty float nz\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\n"
      "element face 131072\nproperty list uchar int vertex_indices\n"
      "property list uchar float texcoord\nend_header\n";
  if (file.write(header) != header.size()) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  for (int y = 0; y <= side; ++y) {
    for (int x = 0; x <= side; ++x) {
      const float z = 21.0F + 0.5F * std::sin(4.0F * 3.14159265358979323846F * x / side) *
          std::sin(4.0F * 3.14159265358979323846F * y / side);
      stream << (20.0F + 2.0F * x / side) << (20.0F + 2.0F * y / side) << z
             << 0.0F << 0.0F << 1.0F << quint8(225) << quint8(45) << quint8(180);
    }
  }
  quint32 face = 0;
  const auto triangle = [&](const quint32 a, const quint32 b, const quint32 c) {
    const float u = (static_cast<float>(face++) + 0.5F) / 131072.0F;
    // Deliberate UV seams preserve three distinct mesh corners per triangle.
    // The atlas is solid, so this does not change the surface's visible color.
    stream << quint8(3) << a << b << c << quint8(6)
           << u << 0.0F << u << 0.5F << u << 1.0F;
  };
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      const quint32 a = quint32(y) * row + quint32(x);
      triangle(a, a + 1, a + row + 1);
      triangle(a, a + row + 1, a + row);
    }
  }
  return stream.status() == QDataStream::Ok && file.flush();
}

bool foregroundAt(const QImage &surface, const QImage &background, const int x, const int y) {
  const QColor a = surface.pixelColor(x, y);
  const QColor b = background.pixelColor(x, y);
  return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                   std::abs(a.blue() - b.blue())}) >= 24;
}

qint64 coloredSurfacePixels(const QImage &frame) {
  qint64 count = 0;
  for (int y = frame.height() / 6; y < frame.height() * 5 / 6; ++y) {
    for (int x = frame.width() / 8; x < frame.width() * 7 / 8; ++x) {
      const QColor color = frame.pixelColor(x, y);
      count += color.red() > 70 && color.red() > color.green() * 2 &&
               color.blue() > color.green() * 2;
    }
  }
  return count;
}

qint64 modelColorPixels(const QImage &frame, const bool red) {
  qint64 count = 0;
  for (int y = frame.height() / 6; y < frame.height() * 5 / 6; ++y) {
    for (int x = frame.width() / 8; x < frame.width() * 7 / 8; ++x) {
      const QColor color = frame.pixelColor(x, y);
      const int primary = red ? color.red() : color.blue();
      const int secondary = red ? color.blue() : color.red();
      count += primary > 70 && primary > secondary * 2 && primary > color.green() * 2;
    }
  }
  return count;
}

QImage firstHudDigit(const QImage &frame, const qreal scale) {
  const int width = std::min(frame.width(), qRound(280 * scale));
  const int height = std::min(frame.height(), qRound(100 * scale));
  QVector<quint8> pixels(width * height, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto color = frame.pixelColor(x, y);
      pixels[y * width + x] = color.blue() > color.red() + 60 && color.blue() > color.green() + 25;
    }
  }
  QRect first;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (pixels[y * width + x] != 1) continue;
      QVector<QPoint> pending{QPoint(x, y)};
      pixels[y * width + x] = 2;
      QRect box(x, y, 1, 1);
      for (qsizetype i = 0; i < pending.size(); ++i) {
        const auto point = pending.at(i);
        box |= QRect(point, QSize(1, 1));
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const QPoint adjacent = point + QPoint(dx, dy);
            if (adjacent.x() < 0 || adjacent.y() < 0 || adjacent.x() >= width || adjacent.y() >= height) continue;
            auto &value = pixels[adjacent.y() * width + adjacent.x()];
            if (value != 1) continue;
            value = 2;
            pending.append(adjacent);
          }
        }
      }
      if (pending.size() < 12 * scale || box.height() < 8 * scale || box.left() > 40 * scale) continue;
      if (first.isNull() || box.left() < first.left()) first = box;
    }
  }
  if (first.isNull()) return {};
  QImage digit(first.size(), QImage::Format_Grayscale8);
  digit.fill(0);
  for (int y = 0; y < first.height(); ++y)
    for (int x = 0; x < first.width(); ++x)
      digit.setPixelColor(x, y, pixels[(first.y() + y) * width + first.x() + x] ? Qt::white : Qt::black);
  return digit;
}

double digitTemplateMatch(const QImage &observed, const QFont &publicFont,
                          const qreal scale, const QChar expected) {
  if (observed.isNull()) return 0;
  double best = 0;
  // These are independent literal digit templates, not text read from model
  // internals. A small font/phase range accommodates normal paint-device/DPI
  // rasterization without copying private overlay layout or font calculations.
  for (int pixelSize = 16; pixelSize <= 22; ++pixelSize) {
    for (const qreal phase : {0.0, 0.5}) {
      QImage canvas(qRound(64 * scale), qRound(64 * scale), QImage::Format_ARGB32_Premultiplied);
      canvas.setDevicePixelRatio(scale);
      canvas.fill(Qt::transparent);
      QFont font = publicFont;
      font.setPixelSize(pixelSize);
      QPainter painter(&canvas);
      painter.setRenderHint(QPainter::TextAntialiasing);
      painter.setFont(font);
      painter.setPen(Qt::white);
      painter.drawText(QPointF(4 + phase, 40 + phase), QString(expected));
      painter.end();
      for (const int alpha : {70, 100, 130}) {
        QRect box;
        qint64 templateInk = 0;
        for (int y = 0; y < canvas.height(); ++y) {
          for (int x = 0; x < canvas.width(); ++x) {
            if (qAlpha(canvas.pixel(x, y)) < alpha) continue;
            box |= QRect(x, y, 1, 1);
            ++templateInk;
          }
        }
        qint64 observedInk = 0;
        for (int y = 0; y < observed.height(); ++y)
          for (int x = 0; x < observed.width(); ++x)
            observedInk += qGray(observed.pixel(x, y)) > 0;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            qint64 intersection = 0;
            for (int y = 0; y < observed.height(); ++y) {
              for (int x = 0; x < observed.width(); ++x) {
                if (qGray(observed.pixel(x, y)) == 0) continue;
                const QPoint source = box.topLeft() + QPoint(x + dx, y + dy);
                if (canvas.rect().contains(source) && qAlpha(canvas.pixel(source)) >= alpha) ++intersection;
              }
            }
            const auto total = templateInk + observedInk - intersection;
            if (total > 0) best = std::max(best, double(intersection) / total);
          }
        }
      }
    }
  }
  return best;
}

} // namespace

bool runResourceBudgetHudSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_HUD FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *light = window.findChild<QAction *>(QStringLiteral("lightThemeAction"));
  if (!check(viewport && light && AppLanguage::apply(QStringLiteral("en_US"), false),
             "real viewport and English interface available")) return false;
  light->trigger();
  QFont font(QString::fromLatin1("Segoe UI"));
  font.setPixelSize(20);
  viewport->setFont(font);
  viewport->setProjectLabel(QStringLiteral("HUD"));
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  viewport->setEditToolsLocked(true);
  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-hud-XXXXXX")));
  if (!check(fixtures.isValid(), "bounded local fixture directory available")) return false;
  const auto path = QDir(fixtures.path()).filePath(QStringLiteral("hud-cube.ply"));
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray input(
      "ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face 12\n"
      "property list uchar int vertex_indices\nend_header\n"
      "-1 -1 -1 225 45 180\n1 -1 -1 225 45 180\n-1 1 -1 225 45 180\n1 1 -1 225 45 180\n"
      "-1 -1 1 225 45 180\n1 -1 1 225 45 180\n-1 1 1 225 45 180\n1 1 1 225 45 180\n"
      "3 0 2 1\n3 1 2 3\n3 4 5 6\n3 5 7 6\n3 0 1 4\n3 1 5 4\n"
      "3 2 6 3\n3 3 6 7\n3 0 4 2\n3 2 4 6\n3 1 3 5\n3 3 7 5\n");
  if (!check(file.write(input) == input.size() && file.flush(), "known eight-vertex twelve-face source saved")) return false;
  file.close();
  bool loaded = false;
  bool failed = false;
  qint64 vertices = 0;
  qint64 faces = 0;
  const auto loadedConnection = QObject::connect(viewport, &NativeViewport::sceneLoaded, viewport,
      [&](qint64 sourceVertices, qsizetype, qint64 sourceFaces, qsizetype) {
        loaded = true;
        vertices = sourceVertices;
        faces = sourceFaces;
      });
  const auto failedConnection = QObject::connect(viewport, &NativeViewport::sceneLoadFailed, viewport,
      [&](const QString &, const QString &) { failed = true; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  viewport->setScene(path, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed && vertices == 8 && faces == 12,
             "ordinary unknown-count import reports the independently known source counts")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  if (!check(viewport->focusModel(), "real mesh supports ordinary framing")) return false;
  viewport->clearSelection();
  QImage frame;
  if (!check(waitUntil([&]() {
        frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        return viewport->resourceBudgetStatus().residentMeshCount == 1 && coloredSurfacePixels(frame) >= 1000;
      }), "fully resident mesh is actually visible before reading its HUD")) return false;
  const auto digit = firstHudDigit(frame, viewport->devicePixelRatioF());
  const double expected = digitTemplateMatch(digit, font, viewport->devicePixelRatioF(), QChar('8'));
  const double zero = digitTemplateMatch(digit, font, viewport->devicePixelRatioF(), QChar('0'));
  qInfo() << "RESOURCE_BUDGET_HUD: real first numeric glyph" << digit.size()
          << "literal 8 / negative-control 0 template overlap" << expected << zero;
  const auto output = qEnvironmentVariable("GSW_RESOURCE_HUD_CAPTURE_PATH");
  if (!output.isEmpty()) {
    if (!check(QDir().mkpath(QFileInfo(output).absolutePath()) && frame.save(output), "real HUD capture saved")) return false;
    digit.save(output + QStringLiteral(".digit.png"));
  }
  if (!check(expected >= 0.80 && expected > zero + 0.05,
             zero >= 0.80 && zero > expected + 0.05
                 ? "real HUD shows zero vertices instead of the known eight imported vertices"
                 : "real HUD must present the known eight-vertex count, with a reliable independent glyph match")) return false;
  viewport->setScene({}, 0);
  qInfo() << "RESOURCE_BUDGET_HUD PASS: public unknown-count mesh import displays the correct vertex count";
  return true;
}

bool runResourceBudgetSmokeTest(MainWindow &window) {
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!action || !action->isEnabled()) {
    qCritical() << "RESOURCE_BUDGET FAIL: resource settings entry is unavailable";
    return false;
  }

  bool observed = false;
  bool controlsAvailable = false;
  QElapsedTimer deadline;
  deadline.start();
  QTimer inspect;
  QEventLoop wait;
  inspect.setInterval(20);
  QObject::connect(&inspect, &QTimer::timeout, &window, [&]() {
    auto *dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
    if (dialog && dialog->isVisible() &&
        dialog->objectName() == QStringLiteral("resourceBudgetDialog")) {
      observed = true;
      auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
      auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
      auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
      controlsAvailable = mode && ram && gpu && mode->findData(0) >= 0 && mode->findData(1) >= 0;
      if (controlsAvailable) {
        mode->setCurrentIndex(mode->findData(1));
        ram->setValue(0.5); // Known user input: 512 MiB, not derived from policy code.
        gpu->setValue(0.0625); // Known user input: 64 MiB.
        controlsAvailable = mode->currentData().toInt() == 1 && ram->isEnabled() && gpu->isEnabled() &&
            ram->value() == 0.5 && gpu->value() == 0.0625;
      }
      inspect.stop();
      dialog->reject();
      wait.quit();
    } else if (deadline.elapsed() > 2500) {
      inspect.stop();
      if (dialog) dialog->reject();
      wait.quit();
    }
  });
  inspect.start();
  action->trigger();
  if (!observed) wait.exec();
  inspect.stop();
  if (!observed) {
    qCritical() << "RESOURCE_BUDGET FAIL: resource settings did not present its real dialog";
    return false;
  }
  if (!controlsAvailable) {
    qCritical() << "RESOURCE_BUDGET FAIL: resource settings lacks usable automatic/manual RAM and GPU controls";
    return false;
  }
  qInfo() << "RESOURCE_BUDGET PASS: real settings accepts manual RAM 512 MiB and GPU 64 MiB without an Apply step";
  return true;
}

bool runResourceBudgetMeshSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_MESH FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!check(viewport && action && action->isEnabled(), "real viewport and settings entry available"))
    return false;
  action->trigger();
  QDialog *dialog = nullptr;
  if (!check(waitUntil([&]() {
        dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
        return dialog && dialog->isVisible();
      }, 2500), "settings presents its real modeless dialog")) return false;
  const auto closeDialog = qScopeGuard([&]() { if (dialog) dialog->close(); });
  auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
  auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
  auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
  if (!check(mode && ram && gpu, "automatic/manual controls available")) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(1.0);
  gpu->setValue(0.5);
  const auto highPolicy = viewport->resourceBudgetPolicy();
  if (!check(highPolicy.mode == ResourceBudgetPolicy::Mode::Manual &&
             highPolicy.ramLimitMiB == 1024 && highPolicy.gpuLimitMiB == 512,
             "real settings immediately applies literal RAM 1024 MiB and GPU 512 MiB")) return false;

  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-mesh-XXXXXX")));
  if (!check(fixtures.isValid(), "D-drive bounded fixture directory available")) return false;
  const QString referencePath = QDir(fixtures.path()).filePath(QStringLiteral("reference-plane.ply"));
  const QString modelPath = QDir(fixtures.path()).filePath(QStringLiteral("connected-grid.ply"));
  if (!check(writeSurface(referencePath, 1) && writeSurface(modelPath, 1024),
             "independent two-triangle reference and connected grid generated")) return false;
  bool loaded = false;
  bool failed = false;
  const auto loadedConnection = QObject::connect(viewport, &NativeViewport::sceneLoaded, viewport,
      [&](qint64, qsizetype, qint64, qsizetype) { loaded = true; });
  const auto failedConnection = QObject::connect(viewport, &NativeViewport::sceneLoadFailed, viewport,
      [&](const QString &, const QString &message) { failed = true; qCritical() << message; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  const auto load = [&](const QString &path) {
    loaded = failed = false;
    viewport->setScene(path, 0);
    return waitUntil([&]() { return loaded || failed; }) && loaded && !failed &&
        viewport->meshRenderingAvailable();
  };
  if (!check(load(referencePath), "independent reference loads")) return false;
  viewport->setEditToolsLocked(true);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  viewport->setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport->setAxisView(NavigationAxis::PositiveZ);
  if (!check(viewport->focusModel(), "reference can be framed through the public viewport")) return false;
  viewport->clearSelection();
  QImage reference;
  if (!check(waitUntil([&]() {
        reference = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        return !reference.isNull() && coloredSurfacePixels(reference) >= 1000;
      }), "reference presents a continuous actual surface")) return false;
  const QVector3D referenceTranslation = viewport->modelTranslation();
  viewport->setModelTranslation(referenceTranslation + QVector3D(10000, 10000, 10000));
  const QImage background = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  viewport->setModelTranslation(referenceTranslation);

  gpu->setValue(0.0625);
  if (!check(viewport->resourceBudgetPolicy().gpuLimitMiB == 64,
             "GPU 64 MiB applies without Accept or reimport")) return false;
  if (!check(load(modelPath), "connected grid loads under controlled low GPU budget")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport->setAxisView(NavigationAxis::PositiveZ);
  if (!check(viewport->focusModel(), "paged model can be framed through the public viewport")) return false;
  viewport->clearSelection();
  QImage pagedFrame;
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        pagedFrame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        return status.pagedMeshCount == 1 && status.residentMeshCount == 0 &&
            !pagedFrame.isNull() && coloredSurfacePixels(pagedFrame) >= 100;
      }), "insufficient budget uses paging and presents nonempty real geometry")) return false;
  const auto source = viewport->scenePath();
  const auto target = viewport->viewTarget();
  const auto distance = viewport->viewDistance();
  const auto angles = viewport->viewOrbitAngles();
  const auto translation = viewport->modelTranslation();
  const auto rotation = viewport->modelRotation();
  const auto scale = viewport->modelScale();
  qInfo() << "RESOURCE_BUDGET_MESH: low-budget paging visible" << coloredSurfacePixels(pagedFrame);

  // Only the public, live setting changes. The test does not reimport/reload,
  // manipulate scene internals or reconstruct a budget formula as its oracle.
  gpu->setValue(0.5);
  if (!check(viewport->resourceBudgetPolicy().gpuLimitMiB == 512,
             "GPU 512 MiB applies immediately to the same viewport")) return false;
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        return status.residentMeshCount == 1 && status.pagedMeshCount == 0 &&
               status.loadingMeshCount == 0 && viewport->meshPagingSettled();
      }), "raising live budget promotes the existing model to complete residency")) return false;
  const float tolerance = std::max(1.0e-5F, distance * 1.0e-5F);
  if (!check(viewport->scenePath() == source &&
             (viewport->viewTarget() - target).length() <= tolerance &&
             std::abs(viewport->viewDistance() - distance) <= tolerance &&
             viewport->viewOrbitAngles() == angles && viewport->modelTranslation() == translation &&
             viewport->modelRotation() == rotation && viewport->modelScale() == scale,
             "budget promotion preserves source, camera and model transform")) return false;
  const QImage promoted = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
  if (!check(promoted.size() == reference.size() && background.size() == reference.size(),
             "reference and promoted frames share the same public viewport")) return false;
  qint64 referenceInterior = 0;
  qint64 promotedInterior = 0;
  for (int y = reference.height() / 6 + 2; y < reference.height() * 5 / 6 - 2; ++y) {
    for (int x = reference.width() / 8 + 2; x < reference.width() * 7 / 8 - 2; ++x) {
      bool interior = foregroundAt(reference, background, x, y);
      for (int dy = -2; dy <= 2 && interior; ++dy) {
        for (int dx = -2; dx <= 2 && interior; ++dx)
          interior = foregroundAt(reference, background, x + dx, y + dy);
      }
      if (interior) {
        ++referenceInterior;
        promotedInterior += foregroundAt(promoted, background, x, y);
      }
    }
  }
  const double coverage = referenceInterior > 0 ? double(promotedInterior) / referenceInterior : 0.0;
  qInfo() << "RESOURCE_BUDGET_MESH: promoted coverage versus independent two-triangle plane"
          << coverage << "reference interior" << referenceInterior;
  if (!check(referenceInterior >= 1000 && coverage >= 0.90,
             "promoted model preserves the independent continuous reference surface")) return false;
  if (!check(viewport->resourceBudgetStatus().managedGpuBytes > 64LL * 1024 * 1024,
             "the real resident buffers exceed the next manual cap")) return false;
  gpu->setValue(0.0625);
  QImage demoted;
  const bool demotedReady = waitUntil([&]() {
    demoted = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    const auto status = viewport->resourceBudgetStatus();
    return status.residentMeshCount == 0 && status.pagedMeshCount == 1 && status.loadingMeshCount == 0 &&
        status.managedGpuBytes + status.reservedGpuBytes <= 64LL * 1024 * 1024 &&
        status.managedRamBytes + status.reservedRamBytes <= 1024LL * 1024 * 1024 &&
        coloredSurfacePixels(demoted) >= 100;
  });
  const auto demotedStatus = viewport->resourceBudgetStatus();
  qInfo() << "RESOURCE_BUDGET_MESH: same-model live demotion"
          << "resident/paged/loading" << demotedStatus.residentMeshCount << demotedStatus.pagedMeshCount
          << demotedStatus.loadingMeshCount << "managed GPU bytes" << demotedStatus.managedGpuBytes
          << "reserved RAM/GPU bytes" << demotedStatus.reservedRamBytes << demotedStatus.reservedGpuBytes
          << "real surface pixels" << coloredSurfacePixels(demoted)
          << "source unchanged" << (viewport->scenePath() == source)
          << "target difference" << (viewport->viewTarget() - target).length()
          << "distance difference" << std::abs(viewport->viewDistance() - distance)
          << "angles unchanged" << (viewport->viewOrbitAngles() == angles)
          << "translation/rotation/scale unchanged" << (viewport->modelTranslation() == translation)
          << (viewport->modelRotation() == rotation) << (viewport->modelScale() == scale);
  if (!check(demotedReady && viewport->scenePath() == source &&
             (viewport->viewTarget() - target).length() <= tolerance &&
             std::abs(viewport->viewDistance() - distance) <= tolerance && viewport->viewOrbitAngles() == angles &&
             viewport->modelTranslation() == translation && viewport->modelRotation() == rotation &&
             viewport->modelScale() == scale,
             "lowering the live cap pages real geometry within budget and preserves source, camera and transform")) return false;
  viewport->setScene({}, 0);
  // Active page prefetch may hold a bounded RAM lease while the source is
  // displayed. A cleared scene, unlike a navigating paged scene, must return it.
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto cleared = viewport->resourceBudgetStatus();
        return cleared.residentMeshCount == 0 && cleared.pagedMeshCount == 0 && cleared.loadingMeshCount == 0 &&
            cleared.managedGpuBytes == 0 && cleared.reservedRamBytes == 0 && cleared.reservedGpuBytes == 0;
      }), "clearing the demoted source returns real pending page leases and GL storage")) return false;
  qInfo() << "RESOURCE_BUDGET_MESH PASS: live settings page, promote and demote real geometry without user reimport";
  return true;
}

bool runResourceBudgetSharedMeshSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_SHARED FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!check(viewport && action && action->isEnabled(), "real viewport and settings entry available"))
    return false;
  action->trigger();
  QDialog *dialog = nullptr;
  if (!check(waitUntil([&]() {
        dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
        return dialog && dialog->isVisible();
      }, 2500), "settings presents its real modeless dialog")) return false;
  const auto closeDialog = qScopeGuard([&]() { if (dialog) dialog->close(); });
  auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
  auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
  auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
  if (!check(mode && ram && gpu, "automatic/manual controls available")) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(1.0);
  gpu->setValue(0.0625);
  const auto policy = viewport->resourceBudgetPolicy();
  if (!check(policy.mode == ResourceBudgetPolicy::Mode::Manual && policy.ramLimitMiB == 1024 &&
             policy.gpuLimitMiB == 64, "the real shared setting is RAM 1024 MiB and GPU 64 MiB")) return false;
  viewport->setEditToolsLocked(true);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  viewport->setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);

  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-shared-XXXXXX")));
  if (!check(fixtures.isValid(), "D-drive bounded fixture directory available")) return false;
  const QString redPath = QDir(fixtures.path()).filePath(QStringLiteral("red-grid.ply"));
  const QString bluePath = QDir(fixtures.path()).filePath(QStringLiteral("blue-grid.ply"));
  if (!check(writeSurface(redPath, 768, QColor(240, 20, 20)) &&
             writeSurface(bluePath, 768, QColor(20, 20, 240)),
             "two distinct bounded connected surfaces generated")) return false;
  const SceneObject red{QStringLiteral("budget-red"), redPath, {-1.5F, 0, 0}, {}, {1, 1, 1}, 589825};
  const SceneObject blue{QStringLiteral("budget-blue"), bluePath, {1.5F, 0, 0}, {}, {1, 1, 1}, 589825};
  const QList<SceneObject> both{red, blue};

  // Establish the bounded workload through an actual complete rendering at a
  // generous known cap before expecting the same source to fit the lower cap.
  // This catches stale conservative estimates without copying their formula.
  gpu->setValue(0.5);
  viewport->setSceneObjects({red}, red.id);
  QImage calibration;
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        calibration = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        return status.residentMeshCount == 1 && status.pagedMeshCount == 0 &&
            status.loadingMeshCount == 0 && status.reservedGpuBytes == 0 &&
            !calibration.isNull() && modelColorPixels(calibration, true) >= 500;
      }), "high-budget calibration presents the complete bounded model")) return false;
  const auto measuredSingleGpuBytes = viewport->resourceBudgetStatus().managedGpuBytes;
  qInfo() << "RESOURCE_BUDGET_SHARED: high-budget independently observed single-model GPU bytes"
          << measuredSingleGpuBytes;
  if (!check(measuredSingleGpuBytes > 32LL * 1024LL * 1024LL &&
             measuredSingleGpuBytes <= 64LL * 1024LL * 1024LL,
             "the complete model is measured to fit 64 MiB alone but two equal buffers cannot")) return false;
  viewport->setSceneObjects({}, {});
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        return status.residentMeshCount == 0 && status.pagedMeshCount == 0 &&
            status.loadingMeshCount == 0 && status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
      }), "high-budget calibration releases its real loading lease")) return false;
  gpu->setValue(0.0625);

  const auto runCase = [&](const bool sameBatch) {
    viewport->setSceneObjects({}, {});
    if (!check(waitUntil([&]() {
          const auto status = viewport->resourceBudgetStatus();
          return status.residentMeshCount == 0 && status.pagedMeshCount == 0 &&
                 status.loadingMeshCount == 0 && status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
        }), "clearing the collection releases real pending reservations")) return false;
    if (!sameBatch) {
      viewport->setSceneObjects({red}, red.id);
      QImage single;
      const bool singleReady = waitUntil([&]() {
            const auto status = viewport->resourceBudgetStatus();
            single = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
            return status.residentMeshCount == 1 && status.pagedMeshCount == 0 &&
                status.loadingMeshCount == 0 && status.reservedGpuBytes == 0 &&
                !single.isNull() && modelColorPixels(single, true) >= 500;
          });
      if (!singleReady) {
        const auto actual = viewport->resourceBudgetStatus();
        qInfo() << "RESOURCE_BUDGET_SHARED: model measured to fit was not resident at 64 MiB"
                << "resident" << actual.residentMeshCount << "paged" << actual.pagedMeshCount
                << "managed GPU bytes" << actual.managedGpuBytes;
      }
      if (!check(singleReady, "one independently measured fitting model is completely resident at 64 MiB")) return false;
      const auto singleGpuBytes = viewport->resourceBudgetStatus().managedGpuBytes;
      qInfo() << "RESOURCE_BUDGET_SHARED: independently observed single-model GPU bytes" << singleGpuBytes;
      if (!check(singleGpuBytes > 32LL * 1024LL * 1024LL && singleGpuBytes <= 64LL * 1024LL * 1024LL,
                 "fixture really fits alone but two equal buffers cannot fit the entered 64 MiB budget")) return false;
    }
    viewport->setSceneObjects(both, red.id);
    if (!check(waitUntil([&]() {
          const auto status = viewport->resourceBudgetStatus();
          return status.residentMeshCount + status.pagedMeshCount == 2 && status.loadingMeshCount == 0;
        }), "both serial or same-batch models finish source loading")) return false;
    for (const auto &object : both) {
      if (!check(viewport->activateSceneObject(object.id) && viewport->meshRenderingAvailable(),
                 "each imported source is available through ordinary scene activation")) return false;
      viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
    }
    viewport->setAxisView(NavigationAxis::PositiveZ);
    waitUntil([]() { return false; }, 300); // Let ordinary navigation snap finish.
    viewport->selectAllModels();
    if (!check(viewport->focusModel(), "both models are framed through ordinary group navigation")) return false;
    viewport->clearSelection();
    QImage frame;
    if (!check(waitUntil([&]() {
          const auto status = viewport->resourceBudgetStatus();
          frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
          return status.loadingMeshCount == 0 && status.reservedRamBytes == 0 && status.reservedGpuBytes == 0 &&
              !frame.isNull() && modelColorPixels(frame, true) >= 500 && modelColorPixels(frame, false) >= 500;
        }), "both colored surfaces are visible together and the real loading leases are returned")) return false;
    const auto status = viewport->resourceBudgetStatus();
    qInfo() << "RESOURCE_BUDGET_SHARED:" << (sameBatch ? "same-batch" : "serial")
            << "resident" << status.residentMeshCount << "paged" << status.pagedMeshCount
            << "managed GPU bytes" << status.managedGpuBytes << "reserved GPU bytes" << status.reservedGpuBytes
            << "red/blue real surface pixels" << modelColorPixels(frame, true) << modelColorPixels(frame, false);
    // Both source models fit alone, but their complete buffers cannot fit the
    // known user-entered total of 64 MiB. This literal acceptance bound is not
    // recalculated using the controller's allocation formula.
    if (!check(status.residentMeshCount <= 1 && status.residentMeshCount + status.pagedMeshCount == 2 &&
               status.managedGpuBytes > 0 && status.managedGpuBytes <= 64LL * 1024LL * 1024LL,
               "models do not each reuse the same 64 MiB total budget")) return false;

    QString residentId;
    if (status.residentMeshCount == 1) {
      for (const auto &object : both) {
        if (!viewport->activateSceneObject(object.id)) return false;
        const auto activeFrame = viewport->grabFramebuffer();
        const bool visible = modelColorPixels(activeFrame, object.id == red.id) >= 500;
        if (visible && viewport->meshRenderingAvailable() && viewport->meshPagingSettled() &&
            viewport->residentMeshTriangleCount() == 0) residentId = object.id;
      }
    }
    if (!check(!residentId.isEmpty(), "a model that fits alone retains its complete visible residency")) return false;
    const SceneObject remaining = residentId == red.id ? blue : red;
    viewport->setSceneObjects({remaining}, remaining.id);
    QImage remainingFrame;
    const bool remainingReady = waitUntil([&]() {
          remainingFrame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
          const auto remainingStatus = viewport->resourceBudgetStatus();
          return remainingStatus.residentMeshCount == 1 && remainingStatus.pagedMeshCount == 0 &&
              remainingStatus.loadingMeshCount == 0 && remainingStatus.reservedRamBytes == 0 &&
              remainingStatus.reservedGpuBytes == 0 && viewport->scenePath() == remaining.path &&
              viewport->meshPagingSettled() && !remainingFrame.isNull() &&
              modelColorPixels(remainingFrame, remaining.id == red.id) >= 500;
        });
    if (!remainingReady) {
      const auto actual = viewport->resourceBudgetStatus();
      qInfo() << "RESOURCE_BUDGET_SHARED: after removal" << "resident" << actual.residentMeshCount
              << "paged" << actual.pagedMeshCount << "loading" << actual.loadingMeshCount
              << "managed GPU bytes" << actual.managedGpuBytes
              << "reserved RAM/GPU bytes" << actual.reservedRamBytes << actual.reservedGpuBytes
              << "source" << viewport->scenePath() << "expected" << remaining.path
              << "settled" << viewport->meshPagingSettled()
              << "remaining surface pixels" << modelColorPixels(remainingFrame, remaining.id == red.id);
    }
    if (!check(remainingReady, "removing the resident model frees the shared budget and promotes the remaining model without reimport"))
      return false;
    return true;
  };
  if (!runCase(false) || !runCase(true)) return false;
  viewport->setSceneObjects({}, {});
  qInfo() << "RESOURCE_BUDGET_SHARED PASS: serial and concurrent imports share one budget, render together and return their leases";
  return true;
}

bool runResourceBudgetCacheFirstSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_CACHE_FIRST FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!check(viewport && action && action->isEnabled(), "real viewport and resource settings available")) return false;
  action->trigger();
  auto *dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
  if (!check(dialog && dialog->isVisible(), "real modeless settings visible")) return false;
  const auto closeDialog = qScopeGuard([&]() { dialog->close(); });
  auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
  auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
  auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
  if (!check(mode && ram && gpu, "real manual controls available")) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(1.0);
  gpu->setValue(80.0 / 1024.0);
  viewport->setEditToolsLocked(true);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-cache-first-XXXXXX")));
  if (!check(fixtures.isValid(), "D-drive bounded fixture directory available")) return false;
  SceneObject red;
  red.id = QStringLiteral("cache-first-red");
  red.path = QDir(fixtures.path()).filePath(QStringLiteral("red.ply"));
  red.translation = QVector3D(-1.5F, 0, 0);
  SceneObject blue = red;
  blue.id = QStringLiteral("cache-first-blue");
  blue.path = QDir(fixtures.path()).filePath(QStringLiteral("blue.ply"));
  blue.translation = QVector3D(1.5F, 0, 0);
  if (!check(writeSurface(red.path, 768, QColor(225, 45, 45)) &&
             writeSurface(blue.path, 768, QColor(45, 60, 225)), "connected equal colored sources saved")) return false;
  viewport->setSceneObjects({red}, red.id);
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        return status.residentMeshCount == 1 && status.pagedMeshCount == 0 && status.loadingMeshCount == 0 &&
            viewport->meshRenderingAvailable();
      }), "first source completely loads under the initial 80 MiB cap")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport->setSceneObjects({red, blue}, red.id);
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        return status.residentMeshCount == 1 && status.pagedMeshCount == 1 && status.loadingMeshCount == 0;
      }), "second source uses paging under the shared 80 MiB cap")) return false;
  for (const auto &object : {red, blue}) {
    if (!check(viewport->activateSceneObject(object.id) && viewport->meshRenderingAvailable(),
               "both source objects expose the real mesh renderer")) return false;
    viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  }
  viewport->setAxisView(NavigationAxis::PositiveZ);
  waitUntil([]() { return false; }, 300);
  viewport->selectAllModels();
  if (!check(viewport->focusModel(), "normal navigation frames both objects")) return false;
  viewport->clearSelection();
  QImage frame;
  const bool pressureReady = waitUntil([&]() {
        frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        const auto status = viewport->resourceBudgetStatus();
        return status.managedGpuBytes > 64LL * 1024 * 1024 && status.managedGpuBytes <= 80LL * 1024 * 1024 &&
            modelColorPixels(frame, true) >= 500 && modelColorPixels(frame, false) >= 500;
      });
  const auto beforePressure = viewport->resourceBudgetStatus();
  qInfo() << "RESOURCE_BUDGET_CACHE_FIRST: initial real usage" << beforePressure.managedGpuBytes
          << "resident/paged/loading" << beforePressure.residentMeshCount << beforePressure.pagedMeshCount
          << beforePressure.loadingMeshCount << "reserved RAM/GPU" << beforePressure.reservedRamBytes
          << beforePressure.reservedGpuBytes << "red/blue pixels"
          << modelColorPixels(frame, true) << modelColorPixels(frame, false);
  if (!check(pressureReady, "real pages consume more than the next cap while both surfaces are visible")) return false;
  if (!check(viewport->activateSceneObject(red.id) && viewport->residentMeshTriangleCount() == 0 &&
             viewport->meshPagingSettled(), "first red source remains genuinely resident before reduction")) return false;
  const auto sourceBefore = viewport->scenePath();
  const auto targetBefore = viewport->viewTarget();
  const auto distanceBefore = viewport->viewDistance();
  int reloadSignals = 0;
  bool observedLoading = false;
  const auto loadedConnection = QObject::connect(viewport, &NativeViewport::sceneLoaded, viewport,
      [&](qint64, qsizetype, qint64, qsizetype) { ++reloadSignals; });
  const auto statusConnection = QObject::connect(viewport, &NativeViewport::resourceBudgetStatusChanged, viewport,
      [&]() { observedLoading |= viewport->resourceBudgetStatus().loadingMeshCount > 0; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(statusConnection);
  });
  gpu->setValue(0.0625);
  const bool trimmed = waitUntil([&]() {
    frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    const auto status = viewport->resourceBudgetStatus();
    observedLoading |= status.loadingMeshCount > 0;
    return status.managedGpuBytes + status.reservedGpuBytes <= 64LL * 1024 * 1024 &&
        status.managedRamBytes + status.reservedRamBytes <= 1024LL * 1024 * 1024 &&
        status.residentMeshCount == 1 && status.pagedMeshCount == 1 && status.loadingMeshCount == 0 &&
        modelColorPixels(frame, true) >= 500 && modelColorPixels(frame, false) >= 500;
  });
  const auto actual = viewport->resourceBudgetStatus();
  qInfo() << "RESOURCE_BUDGET_CACHE_FIRST: reduced real 80-to-64 MiB cap"
          << "resident/paged/loading" << actual.residentMeshCount << actual.pagedMeshCount << actual.loadingMeshCount
          << "managed GPU bytes" << actual.managedGpuBytes << "reload signals" << reloadSignals
          << "observed loading" << observedLoading << "reserved RAM/GPU bytes"
          << actual.reservedRamBytes << actual.reservedGpuBytes << "red/blue pixels"
          << modelColorPixels(frame, true) << modelColorPixels(frame, false)
          << "source unchanged" << (viewport->scenePath() == sourceBefore)
          << "active paged triangles" << viewport->residentMeshTriangleCount()
          << "active settled" << viewport->meshPagingSettled()
          << "target/distance unchanged" << (viewport->viewTarget() == targetBefore)
          << (viewport->viewDistance() == distanceBefore);
  if (!check(trimmed && !observedLoading && reloadSignals == 0 && viewport->scenePath() == sourceBefore &&
             viewport->residentMeshTriangleCount() == 0 && viewport->meshPagingSettled() &&
             viewport->viewTarget() == targetBefore && viewport->viewDistance() == distanceBefore,
             "discarding surplus pages meets the lower cap without needlessly reloading the fitting resident source")) return false;
  viewport->setSceneObjects({}, {});
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto cleared = viewport->resourceBudgetStatus();
        return cleared.residentMeshCount == 0 && cleared.pagedMeshCount == 0 && cleared.loadingMeshCount == 0 &&
            cleared.managedGpuBytes == 0 && cleared.reservedRamBytes == 0 && cleared.reservedGpuBytes == 0;
      }), "clearing both sources returns their real pending page reservations and GL storage")) return false;
  qInfo() << "RESOURCE_BUDGET_CACHE_FIRST PASS: disposable pages shrink first and both surfaces stay visible";
  return true;
}

bool runResourceBudgetMinimumPageSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_MINIMUM_PAGE FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!check(viewport && action && action->isEnabled(), "real viewport and settings entry available")) return false;
  const auto languageBefore = AppLanguage::current();
  if (!check(AppLanguage::apply(QStringLiteral("en_US"), false), "real interface starts in English")) return false;
  const auto restoreLanguage = qScopeGuard([&]() { (void)AppLanguage::apply(languageBefore, false); });
  action->trigger();
  auto *dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
  if (!check(dialog && dialog->isVisible(), "real modeless settings visible")) return false;
  const auto closeDialog = qScopeGuard([&]() { dialog->close(); });
  auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
  auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
  auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
  if (!check(mode && ram && gpu, "real manual budget controls available")) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(1.0);
  gpu->setValue(86.0 / 1024.0);
  if (!check(viewport->resourceBudgetPolicy().gpuLimitMiB == 86,
             "86 MiB manual cap applies through ordinary settings")) return false;
  viewport->setEditToolsLocked(true);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-minimum-page-XXXXXX")));
  if (!check(fixtures.isValid(), "D-drive fixture directory available")) return false;
  QImage atlas(4096, 4096, QImage::Format_RGBA8888);
  atlas.fill(QColor(225, 45, 180));
  if (!check(!atlas.isNull() && atlas.save(QDir(fixtures.path()).filePath(QStringLiteral("atlas.png"))),
             "bounded portable atlas saved")) return false;
  atlas = {};
  const QString sourcePath = QDir(fixtures.path()).filePath(QStringLiteral("textured-surface.ply"));
  if (!check(writeTexturedSurface(sourcePath, QStringLiteral("atlas.png")), "connected textured source saved")) return false;
  bool loaded = false;
  bool failed = false;
  bool started = false;
  QString failure;
  const auto startedConnection = QObject::connect(viewport, &NativeViewport::sceneLoadStarted, viewport,
      [&](const QString &path) { started |= path == sourcePath; });
  const auto loadedConnection = QObject::connect(viewport, &NativeViewport::sceneLoaded, viewport,
      [&](qint64, qsizetype, qint64, qsizetype) { loaded = true; });
  const auto failedConnection = QObject::connect(viewport, &NativeViewport::sceneLoadFailed, viewport,
      [&](const QString &, const QString &message) { failed = true; failure = message; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(startedConnection);
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  if (!check(!viewport->grabFramebuffer().isNull(), "ordinary viewport has presented a real GL frame")) return false;
  viewport->setScene(sourcePath, 0);
  if (!check(started && !loaded && !failed,
             "real source request starts before any queued worker result is delivered")) return false;
  if (!check(QThreadPool::globalInstance()->waitForDone(5000) && !loaded && !failed,
             "bounded real worker finishes in English before GUI delivery")) return false;
  if (!check(AppLanguage::apply(QStringLiteral("ja_JP"), false) &&
             AppLanguage::current() == QStringLiteral("ja_JP"),
             "public live-language selection changes to Japanese before result delivery")) return false;
  if (!check(waitUntil([&]() { return loaded || failed; }), "insufficient minimum display budget returns a public result")) return false;
  QImage rejectedFrame;
  if (loaded && !failed) {
    viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
    (void)viewport->focusModel();
    viewport->clearSelection();
    waitUntil([&]() {
      rejectedFrame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
      return coloredSurfacePixels(rejectedFrame) >= 100;
    }, 5000);
  }
  const auto rejectedStatus = viewport->resourceBudgetStatus();
  qInfo() << "RESOURCE_BUDGET_MINIMUM_PAGE: 86 MiB outcome loaded/failed" << loaded << failed
          << "message" << failure << "texture available" << viewport->meshTextureAvailable()
          << "managed/effective GPU bytes" << rejectedStatus.managedGpuBytes << rejectedStatus.effectiveGpuBudgetBytes
          << "reserved RAM/GPU bytes" << rejectedStatus.reservedRamBytes << rejectedStatus.reservedGpuBytes
          << "real surface pixels" << coloredSurfacePixels(rejectedFrame);
  // The source atlas fits this ceiling by itself, but its lowest useful mesh
  // page does not. Announcing a successfully loaded blank scene is not success.
  if (!check(failed && !loaded && !failure.isEmpty(),
             "atlas plus minimum visible page shortage is reported explicitly instead of silently loading blank")) return false;
  const QString deliveredFailure = failure;
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto status = viewport->resourceBudgetStatus();
        return status.loadingMeshCount == 0 && status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
      }), "rejected import releases its real reservation")) return false;
  loaded = failed = false;
  failure.clear();
  gpu->setValue(94.0 / 1024.0);
  const bool languageRecovery = waitUntil([&]() { return loaded || failed; }) && loaded && !failed;
  if (!languageRecovery) {
    const auto actual = viewport->resourceBudgetStatus();
    qInfo() << "RESOURCE_BUDGET_MINIMUM_PAGE: cross-language live-budget recovery"
            << "current language" << AppLanguage::current() << "loaded/failed" << loaded << failed
            << "resident/paged/loading" << actual.residentMeshCount << actual.pagedMeshCount << actual.loadingMeshCount
            << "manual GPU MiB" << viewport->resourceBudgetPolicy().gpuLimitMiB
            << "source" << viewport->scenePath();
  }
  if (!check(languageRecovery,
             "raising only the live budget after a language change recovers the same source without manual reimport")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  if (!check(viewport->focusModel(), "recovered model supports normal navigation")) return false;
  viewport->clearSelection();
  QImage frame;
  if (!check(waitUntil([&]() {
        frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        const auto status = viewport->resourceBudgetStatus();
        return status.pagedMeshCount == 1 && status.residentMeshCount == 0 && status.loadingMeshCount == 0 &&
            status.reservedRamBytes == 0 && status.reservedGpuBytes == 0 && viewport->meshTextureAvailable() &&
            status.managedGpuBytes <= 94LL * 1024 * 1024 && coloredSurfacePixels(frame) >= 100;
      }), "same source recovers to a real textured surface within the raised total cap")) return false;
  qInfo() << "RESOURCE_BUDGET_MINIMUM_PAGE: recovered real surface pixels" << coloredSurfacePixels(frame);
  if (!check(AppLanguage::current() == QStringLiteral("ja_JP") && deliveredFailure == QString::fromUtf8(
                 "メモリまたは GPU メモリの予算が不足しているため、メッシュプレビューとテクスチャを読み込めません。"),
             "queued admission failure uses the current Japanese interface and language changes do not alter retry behavior")) return false;
  if (!check(AppLanguage::apply(QStringLiteral("en_US"), false),
             "ordinary language selection returns to English for the independent atlas rejection check")) return false;
  viewport->setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto status = viewport->resourceBudgetStatus();
        return status.managedGpuBytes == 0 && status.loadingMeshCount == 0 &&
            status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
      }), "recovered source releases actual buffers before the texture-only shortage check")) return false;
  gpu->setValue(0.0625);
  loaded = failed = false;
  failure.clear();
  viewport->setScene(sourcePath, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && failed && !loaded &&
             failure == QString::fromLatin1("Insufficient memory for the mesh preview and texture."),
             "a 64 MiB cap explicitly rejects the original full-resolution atlas instead of silently downscaling")) return false;
  if (!check(waitUntil([&]() {
        const auto status = viewport->resourceBudgetStatus();
        return status.loadingMeshCount == 0 && status.reservedRamBytes == 0 && status.reservedGpuBytes == 0;
      }), "texture-budget rejection returns its reservation")) return false;
  loaded = failed = false;
  failure.clear();
  gpu->setValue(94.0 / 1024.0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed,
             "raising the live ceiling recovers an atlas-budget rejected source without manual reimport")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  if (!check(viewport->focusModel(), "atlas-budget recovered source retains ordinary navigation")) return false;
  viewport->clearSelection();
  if (!check(waitUntil([&]() {
        frame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        const auto status = viewport->resourceBudgetStatus();
        return viewport->meshTextureAvailable() && status.pagedMeshCount == 1 && status.loadingMeshCount == 0 &&
            status.reservedRamBytes == 0 && status.reservedGpuBytes == 0 &&
            status.managedGpuBytes <= 94LL * 1024 * 1024 && coloredSurfacePixels(frame) >= 100;
      }), "recovered original atlas and actual surface render inside the new ceiling")) return false;
  qInfo() << "RESOURCE_BUDGET_MINIMUM_PAGE: 64-to-94 MiB atlas recovery real surface pixels" << coloredSurfacePixels(frame);
  viewport->setScene({}, 0);
  qInfo() << "RESOURCE_BUDGET_MINIMUM_PAGE PASS: insufficient minimum display cost rejects clearly and live budget recovers";
  return true;
}

bool runResourceBudgetTexturePageSmokeTest(MainWindow &window) {
  const auto check = [](const bool condition, const char *message) {
    if (!condition) qCritical() << "RESOURCE_BUDGET_TEXTURE_PAGE FAIL:" << message;
    return condition;
  };
  auto *viewport = window.findChild<NativeViewport *>();
  auto *action = window.findChild<QAction *>(QStringLiteral("resourceBudgetAction"));
  if (!check(viewport && action && action->isEnabled(), "real viewport and settings entry available"))
    return false;
  action->trigger();
  auto *dialog = window.findChild<QDialog *>(QStringLiteral("resourceBudgetDialog"));
  if (!check(dialog && dialog->isVisible(), "real modeless settings visible")) return false;
  const auto closeDialog = qScopeGuard([&]() { dialog->close(); });
  auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("resourceBudgetMode"));
  auto *ram = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceRamLimitGiB"));
  auto *gpu = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("resourceGpuLimitGiB"));
  if (!check(mode && ram && gpu, "automatic/manual controls available")) return false;
  mode->setCurrentIndex(mode->findData(1));
  ram->setValue(1.0);
  gpu->setValue(0.5);
  viewport->setEditToolsLocked(true);
  viewport->setShowCameras(false);
  viewport->setShowObservationTrackball(false);
  viewport->setReferencePlaneMode(NativeViewport::ReferencePlaneMode::WorldZero);

  QTemporaryDir fixtures(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("resource-texture-page-XXXXXX")));
  if (!check(fixtures.isValid(), "D-drive bounded fixture directory available")) return false;
  const QString atlasPath = QDir(fixtures.path()).filePath(QStringLiteral("atlas-4096.png"));
  QImage atlas(4096, 4096, QImage::Format_RGBA8888);
  atlas.fill(QColor(225, 45, 180));
  if (!check(!atlas.isNull() && atlas.save(atlasPath), "portable 4096-pixel texture generated")) return false;
  atlas = {};
  const QString sourcePath = QDir(fixtures.path()).filePath(QStringLiteral("textured-surface.ply"));
  if (!check(writeTexturedSurface(sourcePath, QStringLiteral("atlas-4096.png")),
             "portable connected UV surface generated")) return false;
  bool loaded = false;
  bool failed = false;
  qint64 faces = 0;
  const auto loadedConnection = QObject::connect(viewport, &NativeViewport::sceneLoaded, viewport,
      [&](qint64, qsizetype, const qint64 sourceFaces, qsizetype) { faces = sourceFaces; loaded = true; });
  const auto failedConnection = QObject::connect(viewport, &NativeViewport::sceneLoadFailed, viewport,
      [&](const QString &, const QString &message) { failed = true; qCritical() << message; });
  const auto disconnect = qScopeGuard([&]() {
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
  });
  viewport->setScene(sourcePath, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed && faces == 131072,
             "complete source metadata loads for calibration")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  if (!check(viewport->focusModel(), "calibration surface is framed through ordinary navigation")) return false;
  viewport->clearSelection();
  QImage calibration;
  if (!check(waitUntil([&]() {
        calibration = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        const auto status = viewport->resourceBudgetStatus();
        return status.residentMeshCount == 1 && status.pagedMeshCount == 0 &&
            status.reservedRamBytes == 0 && status.reservedGpuBytes == 0 && viewport->meshTextureAvailable() &&
            !calibration.isNull() && coloredSurfacePixels(calibration) >= 1000;
      }), "generous budget presents the full textured surface")) return false;
  const auto measuredGpuBytes = viewport->resourceBudgetStatus().managedGpuBytes;
  qInfo() << "RESOURCE_BUDGET_TEXTURE_PAGE: independently measured complete GPU bytes" << measuredGpuBytes;
  if (!check(measuredGpuBytes > 94LL * 1024LL * 1024LL && measuredGpuBytes <= 128LL * 1024LL * 1024LL,
             "bounded complete texture and geometry really require paging at 94 MiB")) return false;
  viewport->setScene({}, 0);
  if (!check(waitUntil([&]() {
        viewport->grabFramebuffer();
        const auto cleared = viewport->resourceBudgetStatus();
        return cleared.managedGpuBytes == 0 && cleared.loadingMeshCount == 0 &&
            cleared.reservedRamBytes == 0 && cleared.reservedGpuBytes == 0;
      }), "calibration clears its actual GL buffers before the lower-budget import")) return false;
  gpu->setValue(94.0 / 1024.0);
  if (!check(viewport->resourceBudgetPolicy().gpuLimitMiB == 94,
             "real user setting applies the independent 94 MiB cap")) return false;
  loaded = failed = false;
  viewport->setScene(sourcePath, 0);
  if (!check(waitUntil([&]() { return loaded || failed; }) && loaded && !failed && faces == 131072,
             "texture fits the lower shared budget and source parsing succeeds")) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  if (!check(viewport->focusModel(), "paged texture source retains ordinary public navigation")) return false;
  viewport->clearSelection();
  QImage pagedFrame;
  const bool visible = waitUntil([&]() {
    pagedFrame = viewport->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    const auto status = viewport->resourceBudgetStatus();
    return status.pagedMeshCount == 1 && status.residentMeshCount == 0 && viewport->meshTextureAvailable() &&
        status.managedGpuBytes <= 94LL * 1024LL * 1024LL && !pagedFrame.isNull() &&
        coloredSurfacePixels(pagedFrame) >= 100;
  }, 15000);
  const auto status = viewport->resourceBudgetStatus();
  qInfo() << "RESOURCE_BUDGET_TEXTURE_PAGE: lower budget" << "resident/paged"
          << status.residentMeshCount << status.pagedMeshCount << "texture available" << viewport->meshTextureAvailable()
          << "managed GPU bytes" << status.managedGpuBytes << "effective GPU bytes" << status.effectiveGpuBudgetBytes
          << "reserved RAM/GPU bytes" << status.reservedRamBytes << status.reservedGpuBytes
          << "real surface pixels" << coloredSurfacePixels(pagedFrame);
  if (!check(visible, "a texture-compatible budget can present a fitting mesh page instead of a blank viewport")) return false;
  viewport->setScene({}, 0);
  qInfo() << "RESOURCE_BUDGET_TEXTURE_PAGE PASS: bounded texture paging presents real geometry without exhausting hardware";
  return true;
}

} // namespace gsw
