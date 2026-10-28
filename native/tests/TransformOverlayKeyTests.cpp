#include "TransformOverlayKey.h"

#include <QtTest>

#include <functional>
#include <limits>
#include <utility>

using namespace gsw;

namespace {
TransformGizmoLayout example() {
  TransformGizmoLayout value;
  value.valid = true;
  value.center = {50, 50}; value.radius = 25; value.pivotDepth = .1F;
  value.lineWidth = 1.5; value.rotationHitInnerRadius = 4;
  value.trackballInnerRadius = 3; value.trackballOuterRadius = 10;
  value.centerHandle = {48, 48, 4, 4}; value.centerHitRadius = 6;
  value.trackballBounds = {40, 40, 20, 20};
  value.viewRing = QPolygonF(QRectF(20, 20, 60, 60));
  for (int index = 0; index < 3; ++index) {
    auto &axis = value.axes[std::size_t(index)];
    axis.direction = {1.F, float(index), 0.F}; axis.visible = true;
    axis.moveLine = {20, 30, 80, 60}; axis.scaleLine = {25, 35, 75, 65};
    axis.moveEndpoint = {80, 60}; axis.scaleEndpoint = {75, 65};
    axis.moveArrow = QPolygonF(QRectF(70, 55, 10, 10));
    axis.scaleHandle = {70, 60, 10, 10};
    axis.rotationRing = QPolygonF(QRectF(25, 25, 50, 50));
    axis.rotationSegments = {{25, 25, 75, 25}};
    axis.depthSegments = {{axis.rotationSegments[0], -.2F, .3F}};
    axis.moveFaces = {{axis.moveArrow, .2F, .8F, {.1F, .2F, .3F, .4F, .1F}}};
    axis.scaleFaces = {{QPolygonF(axis.scaleHandle), .3F, .7F, {.1F, .2F, .3F, .4F, .1F}}};
    axis.moveEndpointDepth = .2F; axis.scaleEndpointDepth = .3F;
    axis.moveShaft = TransformGizmoDepthSegment{axis.moveLine, .1F, .2F};
    axis.scaleShaft = TransformGizmoDepthSegment{axis.scaleLine, .1F, .3F};
    value.planeHandles[std::size_t(index)] = QPolygonF(QRectF(40, 40, 10, 10));
    value.planeDepths[std::size_t(index)] = float(index) / 10;
  }
  return value;
}
QByteArray key(const TransformGizmoLayout &layout) {
  return transformOverlayKey(layout, TransformGizmoMode::Transform, TransformGizmoStyle{}, {}, false);
}
} // namespace

class TransformOverlayKeyTests final : public QObject {
  Q_OBJECT
private slots:
  void exactIdentityIncludesGeometryDepthAndInteraction();
  void boundedStreamingRefusesOversizeInsteadOfTruncating();
  void boundsIncludePaintNotHitTargetsAndClipToVisibleRect();
};

void TransformOverlayKeyTests::exactIdentityIncludesGeometryDepthAndInteraction() {
  const auto original = example();
  const QByteArray baseline = key(original);
  QCOMPARE(baseline.size(), 32);
  QCOMPARE(key(original), baseline);
  using Mutation = std::pair<const char *, std::function<void(TransformGizmoLayout &)>>;
  const Mutation changes[]{
      {"valid", [](auto &v) { v.valid = false; }},
      {"center", [](auto &v) { v.center.rx() += 1e-9; }},
      {"radius", [](auto &v) { v.radius += 1e-9; }},
      {"pivot-depth", [](auto &v) { v.pivotDepth += .01F; }},
      {"line-width", [](auto &v) { v.lineWidth += 1e-9; }},
      {"ring-context", [](auto &v) { v.rotationHitInnerRadius += 1e-9; }},
      {"trackball-inner", [](auto &v) { v.trackballInnerRadius += 1e-9; }},
      {"trackball-outer", [](auto &v) { v.trackballOuterRadius += 1e-9; }},
      {"center-handle", [](auto &v) { v.centerHandle.translate(1e-9, 0); }},
      {"center-hit", [](auto &v) { v.centerHitRadius += 1e-9; }},
      {"trackball-bounds", [](auto &v) { v.trackballBounds.translate(1e-9, 0); }},
      {"view-ring", [](auto &v) { v.viewRing[0].rx() += 1e-9; }},
      {"axis-direction", [](auto &v) { v.axes[0].direction.setX(.9F); }},
      {"move-line", [](auto &v) { v.axes[0].moveLine.translate(1e-9, 0); }},
      {"scale-line", [](auto &v) { v.axes[0].scaleLine.translate(1e-9, 0); }},
      {"move-endpoint", [](auto &v) { v.axes[0].moveEndpoint.rx() += 1e-9; }},
      {"scale-endpoint", [](auto &v) { v.axes[0].scaleEndpoint.rx() += 1e-9; }},
      {"arrow", [](auto &v) { v.axes[0].moveArrow[0].rx() += 1e-9; }},
      {"scale-handle", [](auto &v) { v.axes[0].scaleHandle.translate(1e-9, 0); }},
      {"rotation-ring", [](auto &v) { v.axes[0].rotationRing[0].rx() += 1e-9; }},
      {"rotation-segment", [](auto &v) { v.axes[0].rotationSegments[0].translate(1e-9, 0); }},
      {"depth-segment-line", [](auto &v) { v.axes[0].depthSegments[0].line.translate(1e-9, 0); }},
      {"depth-segment-start", [](auto &v) { v.axes[0].depthSegments[0].startDepth += .01F; }},
      {"depth-segment-end", [](auto &v) { v.axes[0].depthSegments[0].endDepth += .01F; }},
      {"face-polygon", [](auto &v) { v.axes[0].moveFaces[0].polygon[0].rx() += 1e-9; }},
      {"face-depth", [](auto &v) { v.axes[0].moveFaces[0].normalizedDepth += .01F; }},
      {"face-light", [](auto &v) { v.axes[0].moveFaces[0].light += .01F; }},
      {"face-vertex-depth", [](auto &v) { v.axes[0].moveFaces[0].vertexDepths[0] += .01F; }},
      {"scale-face", [](auto &v) { v.axes[0].scaleFaces[0].light += .01F; }},
      {"move-end-depth", [](auto &v) { v.axes[0].moveEndpointDepth += .01F; }},
      {"scale-end-depth", [](auto &v) { v.axes[0].scaleEndpointDepth += .01F; }},
      {"move-shaft", [](auto &v) { v.axes[0].moveShaft->startDepth += .01F; }},
      {"scale-shaft", [](auto &v) { v.axes[0].scaleShaft->endDepth += .01F; }},
      {"optional-shaft", [](auto &v) { v.axes[0].moveShaft.reset(); }},
      {"visibility", [](auto &v) { v.axes[0].visible = false; }},
      {"plane", [](auto &v) { v.planeHandles[0][0].rx() += 1e-9; }},
      {"plane-depth", [](auto &v) { v.planeDepths[0] += .01F; }},
      {"other-axis", [](auto &v) { v.axes[2].moveFaces[0].light += .01F; }}};
  for (const auto &[name, change] : changes) {
    auto changed = original;
    change(changed);
    QVERIFY2(key(changed) != baseline, name);
  }
  TransformGizmoStyle style;
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Rotate, TransformGizmoMode::Scale})
    QVERIFY(transformOverlayKey(original, mode, style, {}, false) != baseline);
  for (int axis = 0; axis < 3; ++axis) {
    auto changed = style; changed.axes[std::size_t(axis)] = QColor(1, 2, 3);
    QVERIFY(transformOverlayKey(original, TransformGizmoMode::Transform, changed, {}, false) != baseline);
  }
  for (int field = 0; field < 4; ++field) {
    auto changed = style;
    if (field == 0) changed.highlight = QColor(1, 2, 3);
    if (field == 1) changed.neutral = QColor(1, 2, 3);
    if (field == 2) changed.centerEdge = QColor(1, 2, 3);
    if (field == 3) changed.light = !changed.light;
    QVERIFY(transformOverlayKey(original, TransformGizmoMode::Transform, changed, {}, false) != baseline);
  }
  QVERIFY(transformOverlayKey(original, TransformGizmoMode::Transform, style,
      {TransformGizmoHandleKind::RotateAxis, 1}, false) != baseline);
  QVERIFY(transformOverlayKey(original, TransformGizmoMode::Transform, style, {}, true) != baseline);
}

void TransformOverlayKeyTests::boundedStreamingRefusesOversizeInsteadOfTruncating() {
  auto layout = example();
  layout.viewRing.resize(transformOverlayContainerElementLimit + 1);
  QVERIFY(key(layout).isEmpty());
  layout = example();
  const QPolygonF large(transformOverlayContainerElementLimit);
  for (auto &axis : layout.axes) axis.rotationRing = large;
  QVERIFY2(key(layout).isEmpty(), "Individually allowed containers must still obey the total byte budget.");
  QVERIFY(transformOverlayBounds(layout, QRectF(0, 0, 100, 100)).isValid());
}

void TransformOverlayKeyTests::boundsIncludePaintNotHitTargetsAndClipToVisibleRect() {
  TransformGizmoLayout layout;
  layout.valid = true; layout.center = {50, 50}; layout.lineWidth = 1.5;
  layout.centerHandle = {48, 48, 4, 4};
  layout.centerHitRadius = 1000000; // A hit target must not allocate a giant image.
  layout.axes[0].visible = true;
  layout.axes[0].moveLine = {20, 30, 80, 60};
  QCOMPARE(transformOverlayBounds(layout, {0, 0, 100, 100}), QRectF(16, 26, 68, 38));
  QCOMPARE(transformOverlayBounds(layout, {0, 0, 50, 100}), QRectF(16, 26, 34, 38));
  layout.planeHandles[2] = QPolygonF(QRectF(85, 70, 5, 5));
  QVERIFY(transformOverlayBounds(layout, {0, 0, 100, 100}).contains(QRectF(85, 70, 5, 5)));
  layout.axes[1].depthSegments = {{{5, 80, 10, 90}, -.5F, .5F}};
  QVERIFY(transformOverlayBounds(layout, {0, 0, 100, 100}).contains(QPointF(5, 90)));
  layout.axes[0].moveFaces = {{QPolygonF(QRectF(200, 200, 10, 10)), 0, 1, {}}};
  QVERIFY(transformOverlayBounds(layout, {0, 0, 100, 100}).right() <= 100);
  layout.lineWidth = std::numeric_limits<qreal>::infinity();
  QVERIFY(transformOverlayBounds(layout, {0, 0, 100, 100}).isEmpty());
  layout.valid = false;
  QVERIFY(transformOverlayBounds(layout, {0, 0, 100, 100}).isEmpty());
}

QTEST_GUILESS_MAIN(TransformOverlayKeyTests)
#include "TransformOverlayKeyTests.moc"
