#include "BlenderViewportVisualTestSupport.h"
#include "AppTheme.h"
#include "NativeViewport.h"
#include "NavigationGizmo.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <array>

namespace gsw::viewportqa {
namespace {

enum class FixtureKind { Points, Mesh, Gaussian3D, Surfel2D };

QByteArray modelFixture(const FixtureKind kind) {
  const bool gaussian = kind == FixtureKind::Gaussian3D ||
                        kind == FixtureKind::Surfel2D;
  const bool mesh = kind == FixtureKind::Mesh;
  const int side = gaussian ? 3 : 15;
  const int count = mesh ? 4 : side * side;
  QByteArray contents = "ply\nformat ascii 1.0\nelement vertex " +
                        QByteArray::number(count) +
                        "\nproperty float x\nproperty float y\nproperty float z\n";
  if (gaussian) {
    contents += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
                "property float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (kind == FixtureKind::Gaussian3D) contents += "property float scale_2\n";
    contents += "property float rot_0\nproperty float rot_1\n"
                "property float rot_2\nproperty float rot_3\n";
  } else {
    contents += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  }
  if (mesh) contents += "element face 2\nproperty list uchar int vertex_indices\n";
  contents += "end_header\n";
  if (mesh) {
    contents += "19 19 20 225 35 215\n21 19 20 225 35 215\n"
                "21 21 20 225 35 215\n19 21 20 225 35 215\n"
                "3 0 1 2\n3 0 2 3\n";
  } else {
    for (int y = 0; y < side; ++y) {
      for (int x = 0; x < side; ++x) {
        contents += QByteArray::number(19.0 + 2.0 * x / (side - 1), 'g', 9) + ' ' +
                    QByteArray::number(19.0 + 2.0 * y / (side - 1), 'g', 9) + " 20 ";
        if (gaussian) {
          contents += "1.2 -1.5 1.2 8 -1.7 -1.7 ";
          if (kind == FixtureKind::Gaussian3D) contents += "-1.7 ";
          contents += "1 0 0 0\n";
        } else {
          contents += "225 35 215\n";
        }
      }
    }
  }
  return contents;
}

int renderedMagentaPixels(const QImage &frame) {
  const QRect sample(qRound(frame.width() * 0.20),
                     qRound(frame.height() * 0.20),
                     qRound(frame.width() * 0.60),
                     qRound(frame.height() * 0.60));
  int count = 0;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      // None of the grid, world axes, cursor, navigation or neutral UI uses
      // this magenta signature. Positive evidence must come from the model.
      if (pixel.red() >= 80 && pixel.blue() >= 80 &&
          pixel.red() - pixel.green() >= 40 &&
          pixel.blue() - pixel.green() >= 40) ++count;
    }
  }
  return count;
}

} // namespace

bool verifyModelRendering(NativeViewport &viewport, QApplication &application) {
  const QString root = QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("test-settings"));
  if (!QDir().mkpath(root)) return false;
  QTemporaryDir fixtures(QDir(root).filePath(QStringLiteral("viewport-models-XXXXXX")));
  if (!fixtures.isValid()) return false;
  struct ModelCase final { FixtureKind kind; const char *name; };
  constexpr std::array<ModelCase, 4> cases{{
      {FixtureKind::Points, "points"}, {FixtureKind::Mesh, "mesh"},
      {FixtureKind::Gaussian3D, "3dgs"}, {FixtureKind::Surfel2D, "2dgs"},
  }};
  for (const auto &model : cases) {
    const QString path = QDir(fixtures.path()).filePath(
        QString::fromLatin1(model.name) + QStringLiteral(".ply"));
    const QByteArray original = modelFixture(model.kind);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(original) != original.size())
      return false;
    file.close();
    bool loaded = false;
    bool loadFailed = false;
    const auto loadedConnection = QObject::connect(
        &viewport, &NativeViewport::sceneLoaded, &viewport,
        [&](qint64 vertices, qsizetype displayed, qint64, qsizetype) {
          loaded = vertices > 0 && displayed > 0;
        });
    const auto failedConnection = QObject::connect(
        &viewport, &NativeViewport::sceneLoadFailed, &viewport,
        [&](const QString &, const QString &message) {
          qCritical() << "Viewport model fixture failed to load:" << model.name << message;
          loadFailed = true;
        });
    const auto disconnect = qScopeGuard([&]() {
      QObject::disconnect(loadedConnection);
      QObject::disconnect(failedConnection);
    });
    viewport.setScene(path, 0);
    QElapsedTimer deadline;
    deadline.start();
    while (!loaded && !loadFailed && deadline.elapsed() < 5000) processFrames(20);
    if (!loaded || loadFailed) return false;
    const bool gaussian = model.kind == FixtureKind::Gaussian3D ||
                          model.kind == FixtureKind::Surfel2D;
    const bool surfel = model.kind == FixtureKind::Surfel2D;
    if (viewport.sourceHasGaussianAttributes() != gaussian ||
        viewport.sourceHasSurfelAttributes() != surfel ||
        (gaussian && !viewport.gaussianRenderingAvailable()) ||
        (model.kind == FixtureKind::Mesh && !viewport.meshRenderingAvailable())) {
      qCritical() << "Required real model renderer unavailable:" << model.name
                  << "Gaussian/surfel attributes" << viewport.sourceHasGaussianAttributes()
                  << viewport.sourceHasSurfelAttributes()
                  << "Gaussian/mesh renderer" << viewport.gaussianRenderingAvailable()
                  << viewport.meshRenderingAvailable();
      return false;
    }
    const auto mode = gaussian ? NativeViewport::RenderMode::Gaussians
        : model.kind == FixtureKind::Mesh ? NativeViewport::RenderMode::Mesh
                                         : NativeViewport::RenderMode::Points;
    viewport.setRenderMode(mode);
    viewport.setEditToolsLocked(true);
    viewport.setShowCameras(false);
    viewport.setShowObservationTrackball(false);
    const QVector3D translation(0.25F, -0.125F, 0.375F);
    const QQuaternion rotation = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 17.0F);
    const QVector3D scale(1.1F, 0.9F, 1.2F);
    viewport.setModelTransform(translation, rotation, scale);
    const QVector3D storedTranslation = viewport.modelTranslation();
    const QQuaternion storedRotation = viewport.modelRotation();
    const QVector3D storedScale = viewport.modelScale();
    if (storedTranslation != translation || storedScale != scale ||
        storedRotation.isIdentity()) return false;
    viewport.resetCamera();
    viewport.setAxisView(NavigationAxis::PositiveZ);
    processFrames(300);
    if (!viewport.focusModel()) return false;
    // Four actual image captures cover both themes and both projections.
    for (int configuration = 0; configuration < 4; ++configuration) {
      const bool light = configuration == 1 || configuration == 2;
      AppTheme::applyTheme(application, light ? UiTheme::Light : UiTheme::Dark, false);
      if (viewport.orthographicProjection() != (configuration >= 2)) {
        const auto navigation = navigationGizmoLayout(
            QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
        clickViewport(viewport, navigation.projectionButton.center());
      }
      processFrames(30);
      const QImage frame = viewport.grabFramebuffer();
      if (frame.isNull()) return false;
      const int modelPixels = renderedMagentaPixels(frame);
      const bool matchingState = viewport.renderMode() == mode &&
          viewport.modelTranslation() == storedTranslation &&
          viewport.modelRotation() == storedRotation && viewport.modelScale() == storedScale &&
          viewport.orthographicProjection() == (configuration >= 2) &&
          (!gaussian || viewport.gaussianRenderingAvailable());
      qInfo() << "Blender viewport real model rendering:" << model.name
              << "configuration" << configuration << "magenta-pixels" << modelPixels
              << "transform/render-path-preserved" << matchingState;
      if (!saveEvidence(frame, QStringLiteral("blender-model-") +
                               QString::fromLatin1(model.name) + QLatin1Char('-') +
                               QString::number(configuration))) return false;
      if (modelPixels < 20 || !matchingState) return false;
    }
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != original) return false;
    file.close();
  }
  viewport.setScene({}, 0);
  processFrames(30);
  return true;
}

} // namespace gsw::viewportqa
