#include "TransformGizmoRenderer.h"

#include <QImage>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace gsw;

class TransformGizmoVisualTests final : public QObject {
  Q_OBJECT

private slots:
  void centerHasReadableOutlineWithoutOpaqueDisc();
  void ringsUseDepthAndDistinguishTheirFrontAndBack();
  void projectedScaleBoxesHaveVisibleLitFaces();
  void rendersEveryModeInBothThemesAndProjections();
};

void TransformGizmoVisualTests::centerHasReadableOutlineWithoutOpaqueDisc() {
  for (const bool light : {true, false}) {
    const QColor background = light ? QColor(244, 242, 236) : QColor(27, 30, 34);
    QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    TransformGizmoLayout layout;
    layout.valid = true;
    layout.center = QPointF(200, 200);
    layout.radius = 100;
    layout.centerHandle = QRectF(195, 195, 10, 10);
    TransformGizmoStyle style;
    style.light = light;
    style.centerEdge = light ? QColor(38, 43, 50) : QColor(228, 232, 234);
    QPainter painter(&image);
    paintTransformGizmo(painter, layout, TransformGizmoMode::Move, style);
    painter.end();
    int outlinePixels = 0;
    for (int y = 193; y <= 207; ++y) {
      for (int x = 193; x <= 207; ++x) {
        if (image.pixelColor(x, y) != background) ++outlinePixels;
      }
    }
    QVERIFY2(outlinePixels >= 12, "The compact move center must remain visibly outlined.");
    // A known model/background colour must remain visible through the handle.
    // This catches reintroduction of the former opaque white/gray center disc.
    for (int y = 199; y <= 201; ++y)
      for (int x = 199; x <= 201; ++x)
        QCOMPARE(image.pixelColor(x, y), background);
  }
}

void TransformGizmoVisualTests::ringsUseDepthAndDistinguishTheirFrontAndBack() {
  for (const bool light : {true, false}) {
    const QColor background = light ? QColor(244, 242, 236) : QColor(16, 18, 22);
    TransformGizmoStyle style;
    style.light = light;
    TransformGizmoLayout layout;
    layout.valid = true;
    layout.center = QPointF(200, 200);
    layout.radius = 100;
    layout.pivotDepth = 0;
    layout.lineWidth = 2.5;
    const QLineF horizontal(100, 150, 300, 150);
    const QLineF vertical(200, 50, 200, 250);
    layout.axes[0].rotationSegments = {horizontal};
    layout.axes[2].rotationSegments = {vertical};
    layout.axes[0].depthSegments = {{horizontal, -0.5F, -0.5F}};
    layout.axes[2].depthSegments = {{vertical, 0.5F, 0.5F}};
    const auto render = [&]() {
      QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
      image.fill(background);
      QPainter painter(&image);
      paintTransformGizmo(painter, layout, TransformGizmoMode::Rotate, style);
      painter.end();
      return image;
    };
    const auto contrast = [&background](const QColor &color) {
      return std::abs(color.red() - background.red()) +
             std::abs(color.green() - background.green()) +
             std::abs(color.blue() - background.blue());
    };
    const QImage first = render();
    const QColor front = first.pixelColor(150, 150);
    const QColor rear = first.pixelColor(200, 90);
    QVERIFY2(contrast(front) > 120, "Front rings must remain clearly readable.");
    QVERIFY2(contrast(rear) > 8, "The rear arc remains visible and selectable.");
    QVERIFY2(contrast(front) > contrast(rear) * 1.7,
             "Front/rear hierarchy cannot be the same flat coloured stroke.");
    const QColor crossing = first.pixelColor(200, 150);
    QVERIFY2(crossing.red() > crossing.green() + 60 && crossing.red() > crossing.blue() + 60,
             "The nearer X ring must cover the later-numbered Z ring at their crossing.");
    layout.axes[0].depthSegments = {{horizontal, 0.5F, 0.5F}};
    layout.axes[2].depthSegments = {{vertical, -0.5F, -0.5F}};
    const QColor reversed = render().pixelColor(200, 150);
    QVERIFY2(reversed.green() > reversed.red() + 60 && reversed.green() > reversed.blue() + 40,
             "Reversing actual depths must reverse visible intersection order.");
  }
}

void TransformGizmoVisualTests::projectedScaleBoxesHaveVisibleLitFaces() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(4, 6, 5), {}, QVector3D(0, 0, 1));
  QMatrix4x4 projection;
  projection.perspective(46, 4.0F / 3.0F, 0.01F, 100);
  const auto layout = transformGizmoLayout({}, {}, projection * view, {800, 600},
                                           TransformGizmoMode::Scale, 2);
  QVERIFY(layout.valid);
  for (const bool light : {true, false}) {
    const QColor background = light ? QColor(244, 242, 236) : QColor(16, 18, 22);
    QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    TransformGizmoStyle style;
    style.light = light;
    QPainter painter(&image);
    paintTransformGizmo(painter, layout, TransformGizmoMode::Scale, style);
    painter.end();
    for (const auto &axis : layout.axes) {
      QVERIFY(axis.scaleFaces.size() >= 2);
      int darkest = 765;
      int brightest = 0;
      for (const auto &face : axis.scaleFaces) {
        QPointF center;
        for (const auto &point : face.polygon) center += point;
        center /= face.polygon.size();
        const QColor pixel = image.pixelColor(qRound(center.x()), qRound(center.y()));
        QVERIFY2(pixel != background, "The projected scale box must actually be painted.");
        const int brightness = pixel.red() + pixel.green() + pixel.blue();
        darkest = std::min(darkest, brightness);
        brightest = std::max(brightest, brightness);
      }
      QVERIFY2(brightest - darkest >= 25,
               "Visible cube faces need meaningful light/shade, not a flat square.");
    }
  }
}

void TransformGizmoVisualTests::rendersEveryModeInBothThemesAndProjections() {
  const QString output = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
  if (!output.isEmpty()) QVERIFY(QDir().mkpath(output));
  const std::array<TransformGizmoMode, 4> modes{TransformGizmoMode::Move,
      TransformGizmoMode::Rotate, TransformGizmoMode::Scale, TransformGizmoMode::Transform};
  const std::array<QString, 4> names{QStringLiteral("move"), QStringLiteral("rotate"),
      QStringLiteral("scale"), QStringLiteral("transform")}; // Machine artifact identifiers.
  qint64 totalPaintNanoseconds = 0;
  for (const bool light : {true, false}) {
    for (const bool orthographic : {false, true}) {
      QMatrix4x4 view;
      view.lookAt(QVector3D(4, 6, 5), {}, QVector3D(0, 0, 1));
      QMatrix4x4 projection;
      if (orthographic) projection.ortho(-4, 4, -3, 3, 0.01F, 100);
      else projection.perspective(46, 4.0F / 3.0F, 0.01F, 100);
      const auto vp = projection * view;
      const auto project = [&vp](const QVector3D &world) {
        const auto ndc = (vp * QVector4D(world, 1)).toVector3DAffine();
        return QPointF((ndc.x() + 1) * 400, (1 - ndc.y()) * 300);
      };
      for (std::size_t mode = 0; mode < modes.size(); ++mode) {
        QImage background(800, 600, QImage::Format_ARGB32_Premultiplied);
        background.fill(light ? QColor(245, 246, 248) : QColor(21, 24, 28));
        QPainter backdrop(&background);
        backdrop.setRenderHint(QPainter::Antialiasing);
        backdrop.setPen(QPen(light ? QColor(220, 224, 231) : QColor(39, 45, 54), 0.6));
        for (int line = -12; line <= 12; ++line) {
          backdrop.drawLine(project({static_cast<float>(line), -12, -1}),
                              project({static_cast<float>(line), 12, -1}));
          backdrop.drawLine(project({-12, static_cast<float>(line), -1}),
                              project({12, static_cast<float>(line), -1}));
        }
        // A controlled shaded model pedestal, not the user's research model.
        const std::array<QPolygonF, 3> pedestal{
            QPolygonF{project({-1.3F,-1,-0.8F}), project({1.3F,-1,-0.8F}),
                      project({1.3F,-1,0.6F}), project({-1.3F,-1,0.6F})},
            QPolygonF{project({1.3F,-1,-0.8F}), project({1.3F,1,-0.8F}),
                      project({1.3F,1,0.6F}), project({1.3F,-1,0.6F})},
            QPolygonF{project({-1.3F,-1,0.6F}), project({1.3F,-1,0.6F}),
                      project({1.3F,1,0.6F}), project({-1.3F,1,0.6F})}};
        const std::array<QColor, 3> faceColors{QColor(103, 113, 131),
            QColor(80, 91, 110), QColor(145, 154, 171)};
        backdrop.setPen(QPen(QColor(72, 83, 101), 0.8));
        for (std::size_t face = 0; face < pedestal.size(); ++face) {
          backdrop.setBrush(faceColors[face]);
          backdrop.drawPolygon(pedestal[face]);
        }
        backdrop.end();
        QImage frame = background.copy();
        const auto layout = transformGizmoLayout({}, {}, vp, {800, 600}, modes[mode],
            modes[mode] == TransformGizmoMode::Transform ? 2.0 : 1.2);
        QVERIFY(layout.valid);
        TransformGizmoStyle style;
        style.light = light;
        if (!light) {
          style.axes = {QColor(226,67,67), QColor(62,116,232), QColor(104,185,57)};
          style.highlight = QColor(255,224,118);
          style.neutral = QColor(225,229,232);
          style.centerEdge = QColor(228,232,234);
        }
        QPainter painter(&frame);
        QElapsedTimer paintClock;
        paintClock.start();
        paintTransformGizmo(painter, layout, modes[mode], style);
        totalPaintNanoseconds += paintClock.nsecsElapsed();
        painter.end();
        int changed = 0;
        for (int y = 0; y < frame.height(); ++y) {
          for (int x = 0; x < frame.width(); ++x) {
            QVERIFY(frame.pixelColor(x,y).alpha() == 255);
            if (frame.pixel(x,y) != background.pixel(x,y)) ++changed;
          }
        }
        QVERIFY2(changed >= 100, "Every tool must have visible operative geometry on the model.");
        if (!output.isEmpty()) {
          const auto path = QDir(output).filePath(QStringLiteral("gizmo-%1-%2-%3.png").arg(
              names[mode], orthographic ? QStringLiteral("ortho") : QStringLiteral("persp"),
              light ? QStringLiteral("light") : QStringLiteral("dark")));
          QVERIFY(frame.save(path));
        }
      }
    }
  }
  qInfo() << "Offscreen gizmo paint CPU nanoseconds, 16 frames:" << totalPaintNanoseconds;
}

QTEST_GUILESS_MAIN(TransformGizmoVisualTests)
#include "TransformGizmoVisualTests.moc"
