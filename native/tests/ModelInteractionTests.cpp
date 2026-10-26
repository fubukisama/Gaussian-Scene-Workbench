#include "ModelInteraction.h"
#include "PlyPointCloudLoader.h"
#include "TransformGizmo.h"

#include <QMatrix4x4>
#include <QTest>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace gsw;

class ModelInteractionTests final : public QObject {
  Q_OBJECT

private slots:
  void unprojectsViewportCentre();
  void intersectsTranslatedBoundsAndRejectsEmptySpace();
  void intersectsViewPlaneForFreeMovement();
  void rejectsParallelViewPlaneRay();
  void buildsPivotedTrsTransformAndInversePickRay();
  void computesStableAxisAndTrackballRotations();
  void rotatesEdgeOnAxesInBothProjections();
  void preservesFaceOnPlaneRotation();
  void laysOutAndHitsTransformGizmos();
  void keepsCompactCentreEasyToPick();
  void projectsFrontAndBackRingDepths();
  void projectsShadedSolidHandleFaces();
  void picksNearestOverlappingRing();
  void picksSolidHandleSilhouettes();
  void picksNearestOverlappingSolidHandles();
  void picksNearestOverlappingShaftsAndPlanes();
  void keepsTransformToolsVisible_data();
  void keepsTransformToolsVisible();
  void scalesTransformHandlesWithViewDistance();
  void projectsWorldAxesAndPlanes();
  void scalesOrthographicHandlesAndRejectsStaleHitAreas();
  void keepsClippedRotationRingInteractive();
  void picksNearHandlesForSmallResolvableDepthDifferences();
  void doesNotPickInvisibleLegacyHeadAreasWhenSolidFacesExist();
  void modelsTransformOrientationLocking();
  void mapsPreciseModelPickNeighborhood();
  void picksActualPointCentresWithoutAcceptingEmptyBounds();
  void keepsSquarePhysicalPixelToleranceAndCircularFootprints();
  void rejectsInvalidPointPickInputsAndClippedCentres();
  void picksNearestPointDepthAndDeletedSourceIndices();
  void picksTransformedThreeAndTwoDimensionalGaussianCentres();
  void visitsEveryPointWithoutSamplingOrMutatingInputs();
};

namespace {
PointCloudVertex pickVertex(float x, float y, float z, quint32 sourceIndex = 0) {
  PointCloudVertex vertex;
  vertex.x = x; vertex.y = y; vertex.z = z; vertex.sourceIndex = sourceIndex;
  return vertex;
}

PointCloudVertex pickWindowVertex(float x, float y, float depth = 0.0F) {
  // Power-of-two viewport dimensions make the edge cases exactly representable.
  return pickVertex(x / 64.0F - 1.0F, y / 64.0F - 1.0F, depth);
}
}

void ModelInteractionTests::picksActualPointCentresWithoutAcceptingEmptyBounds() {
  const auto mapping = modelPickViewport({64, 64}, {128, 128}, 1.0);
  QVERIFY(mapping);
  const std::array vertices{pickVertex(-0.8F, 0, 0, 0), pickVertex(0.8F, 0, 0, 1)};
  QVERIFY(!pointModelPickDepth(vertices, {}, {}, *mapping, 3.0));
  const std::array centre{pickVertex(0, 0, 0)};
  QCOMPARE(pointModelPickDepth(centre, {}, {}, *mapping, 3.0).value(), 0.5F);
  QVERIFY(!pointModelPickDepth({}, {}, {}, *mapping, 3.0));
}

void ModelInteractionTests::keepsSquarePhysicalPixelToleranceAndCircularFootprints() {
  const auto ordinary = modelPickViewport({64, 64}, {128, 128}, 1.0);
  const auto highDpi = modelPickViewport({32, 32}, {64, 64}, 2.0);
  QVERIFY(ordinary && highDpi);
  // A square pick mask covers its corners, even beyond a radial 6 px tolerance.
  const std::array corner{pickWindowVertex(70.5F, 69.5F)};
  QVERIFY(pointModelPickDepth(corner, {}, {}, *ordinary, 3.0));
  const std::array circleEdge{pickWindowVertex(72.0F, 69.5F)};
  QVERIFY(pointModelPickDepth(circleEdge, {}, {}, *ordinary, 3.0));
  const std::array beyondCircle{pickWindowVertex(72.125F, 69.5F)};
  QVERIFY(!pointModelPickDepth(beyondCircle, {}, {}, *ordinary, 3.0));
  const std::array diagonalOutside{pickWindowVertex(72.0F, 71.0F)};
  QVERIFY(!pointModelPickDepth(diagonalOutside, {}, {}, *ordinary, 3.0));
  const std::array dpiOnly{pickWindowVertex(76.0F, 63.0F)};
  QVERIFY(!pointModelPickDepth(dpiOnly, {}, {}, *ordinary, 3.0));
  QVERIFY(pointModelPickDepth(dpiOnly, {}, {}, *highDpi, 6.0));
  const std::array dpiEdge{pickWindowVertex(79.5F, 75.5F)};
  QVERIFY(pointModelPickDepth(dpiEdge, {}, {}, *highDpi, 6.0));
  const std::array dpiOutside{pickWindowVertex(79.625F, 75.5F)};
  QVERIFY(!pointModelPickDepth(dpiOutside, {}, {}, *highDpi, 6.0));
}

void ModelInteractionTests::rejectsInvalidPointPickInputsAndClippedCentres() {
  const auto mapping = modelPickViewport({64, 64}, {128, 128}, 1.0);
  QVERIFY(mapping);
  const std::array centre{pickVertex(0, 0, 0)};
  QVERIFY(!pointModelPickDepth(centre, {}, {}, {}, 3.0));
  for (const qreal diameter : {0.0, -1.0, std::numeric_limits<qreal>::infinity(),
                              std::numeric_limits<qreal>::quiet_NaN()})
    QVERIFY(!pointModelPickDepth(centre, {}, {}, *mapping, diameter));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const std::array invalid{pickVertex(nan, 0, 0), pickVertex(0, infinity, 0),
      pickVertex(0, 0, nan), pickVertex(0, 0, -1.001F), pickVertex(0, 0, 1.0F),
      pickVertex(0, 0, 1.001F), pickVertex(1.001F, 0, 0), pickVertex(0, -1.001F, 0)};
  // Even a huge footprint does not rescue a centre outside canonical clipping.
  QVERIFY(!pointModelPickDepth(invalid, {}, {}, *mapping, 1000.0));
  const std::array nearPlane{pickVertex(0, 0, -1.0F)};
  QCOMPARE(pointModelPickDepth(nearPlane, {}, {}, *mapping, 3.0).value(), 0.0F);
  QMatrix4x4 perspective;
  perspective.perspective(60, 1, 0.1F, 100.0F);
  const std::array behind{pickVertex(0, 0, 1), pickVertex(0, 0, 0), pickVertex(0, 0, -0.05F)};
  QVERIFY(!pointModelPickDepth(behind, {}, perspective, *mapping, 3.0));
  const std::array inFront{pickVertex(0, 0, -1)};
  QVERIFY(pointModelPickDepth(inFront, {}, perspective, *mapping, 3.0));
  QMatrix4x4 nonfinite;
  nonfinite(0, 0) = nan;
  QVERIFY(!pointModelPickDepth(centre, {}, nonfinite, *mapping, 3.0));
}

void ModelInteractionTests::picksNearestPointDepthAndDeletedSourceIndices() {
  const auto mapping = modelPickViewport({64, 64}, {128, 128}, 1.0);
  QVERIFY(mapping);
  const std::array vertices{pickVertex(0, 0, 0.5F, 2), pickVertex(0, 0, -0.5F, 7)};
  QCOMPARE(pointModelPickDepth(vertices, {}, {}, *mapping, 3.0, true).value(), 0.25F);
  QCOMPARE(pointModelPickDepth(vertices, {}, {}, *mapping, 3.0, false).value(), 0.75F);
  QBitArray deleted(10);
  deleted.setBit(7);
  QCOMPARE(pointModelPickDepth(vertices, deleted, {}, *mapping, 3.0).value(), 0.75F);
  deleted.setBit(2);
  QVERIFY(!pointModelPickDepth(vertices, deleted, {}, *mapping, 3.0));
  QCOMPARE(deleted.count(true), 2);
}

void ModelInteractionTests::picksTransformedThreeAndTwoDimensionalGaussianCentres() {
  ModelTransform transform;
  transform.translation = {0.5F, 0, 0};
  transform.rotation = QQuaternion::fromAxisAndAngle({0, 0, 1}, 90);
  transform.scale = {2, 1, 0.5F};
  const auto mapping = modelPickViewport({96, 47}, {128, 128}, 1.0);
  const auto empty = modelPickViewport({64, 64}, {128, 128}, 1.0);
  QVERIFY(mapping && empty);
  auto gaussian = pickVertex(0.125F, 0, 0.2F);
  gaussian.scaleX = 5; gaussian.scaleY = 3; gaussian.scaleZ = 2;
  gaussian.rotationW = 0.5F; gaussian.rotationX = 0.5F;
  gaussian.rotationY = 0.5F; gaussian.rotationZ = 0.5F;
  gaussian.opacity = 0; // The existing mask picks centres, not visible alpha.
  for (const bool surfel : {false, true}) {
    gaussian.scaleZ = surfel ? 0.0F : 2.0F;
    const std::array vertices{gaussian};
    const auto depth = pointModelPickDepth(vertices, {}, transform.matrix({}), *mapping, 3.0);
    QVERIFY(depth);
    QVERIFY(std::abs(*depth - 0.55F) < 1e-6F);
    QVERIFY(!pointModelPickDepth(vertices, {}, transform.matrix({}), *empty, 3.0));
  }
}

void ModelInteractionTests::visitsEveryPointWithoutSamplingOrMutatingInputs() {
  const auto mapping = modelPickViewport({64, 64}, {128, 128}, 1.0);
  QVERIFY(mapping);
  std::vector<PointCloudVertex> vertices(512202);
  for (std::size_t i = 0; i < vertices.size(); ++i)
    vertices[i] = pickVertex(0.75F, 0.75F, 0, static_cast<quint32>(i));
  vertices.back().x = vertices.back().y = 0;
  const QBitArray deleted(static_cast<qsizetype>(vertices.size()));
  QVERIFY(pointModelPickDepth(vertices, deleted, {}, *mapping, 3.0));
  QCOMPARE(vertices.back().sourceIndex, quint32(512201));
  QCOMPARE(vertices.front().x, 0.75F);
  QCOMPARE(deleted.count(true), 0);
  auto hiddenLast = deleted;
  hiddenLast.setBit(512201);
  QVERIFY(!pointModelPickDepth(vertices, hiddenLast, {}, *mapping, 3.0));
}

void ModelInteractionTests::picksNearHandlesForSmallResolvableDepthDifferences() {
  TransformGizmoLayout layout;
  layout.valid = true;
  layout.center = QPointF(200, 200);
  layout.radius = 100;
  const QLineF crossing(100, 150, 300, 150);
  layout.axes[0].rotationSegments = {crossing};
  layout.axes[1].rotationSegments = {crossing};
  layout.axes[0].depthSegments = {{crossing, 0.2499999F, 0.2499999F}};
  layout.axes[1].depthSegments = {{crossing, 0.25F, 0.25F}};
  QCOMPARE(hitTestTransformGizmo(layout, QPointF(200, 150), TransformGizmoMode::Rotate),
           (TransformGizmoHandle{TransformGizmoHandleKind::RotateAxis, 0}));
  QPolygonF head;
  head << QPointF(200, 130) << QPointF(190, 155) << QPointF(210, 155);
  for (int axis = 0; axis < 2; ++axis) {
    layout.axes[axis].visible = true;
    layout.axes[axis].moveArrow = head;
    layout.axes[axis].moveEndpointDepth = axis == 0 ? 0.25F : 0.2499999F;
  }
  QCOMPARE(hitTestTransformGizmo(layout, QPointF(200, 145), TransformGizmoMode::Move),
           (TransformGizmoHandle{TransformGizmoHandleKind::MoveAxis, 1}));
}

void ModelInteractionTests::doesNotPickInvisibleLegacyHeadAreasWhenSolidFacesExist() {
  TransformGizmoLayout layout;
  layout.valid = true;
  layout.center = QPointF(400, 400);
  layout.radius = 100;
  auto &axis = layout.axes[0];
  axis.visible = true;
  axis.moveArrow << QPointF(40,80) << QPointF(140,80) << QPointF(90,160);
  axis.scaleHandle = QRectF(40,80,100,80);
  const TransformGizmoFace actual{QPolygonF(QRectF(80,90,20,20)), 0.1F, 1};
  axis.moveFaces = {actual};
  axis.scaleFaces = {actual};
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Scale}) {
    QVERIFY2(!hitTestTransformGizmo(layout, QPointF(45,85), mode).isValid(),
             "Invisible legacy heads must not remain clickable outside actual solid geometry.");
    QCOMPARE(hitTestTransformGizmo(layout, QPointF(90,100), mode),
             (TransformGizmoHandle{mode == TransformGizmoMode::Move ?
                  TransformGizmoHandleKind::MoveAxis : TransformGizmoHandleKind::ScaleAxis, 0}));
  }
}

void ModelInteractionTests::keepsCompactCentreEasyToPick() {
  QMatrix4x4 view;
  view.lookAt({0, 0, 10}, {}, {0, 1, 0});
  QMatrix4x4 projection;
  projection.ortho(-4, 4, -4, 4, 0.1F, 100.0F);
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Scale,
                          TransformGizmoMode::Transform}) {
    const auto layout = transformGizmoLayout({}, {}, projection * view,
                                            {800, 800}, mode, 1);
    QVERIFY(layout.valid);
    QVERIFY2(layout.centerHandle.width() <= 11.0,
             "The centre grip must be compact at a 100 px gizmo radius");
    const auto expected = mode == TransformGizmoMode::Scale
                              ? TransformGizmoHandleKind::ScaleUniform
                              : TransformGizmoHandleKind::MoveView;
    QCOMPARE(hitTestTransformGizmo(layout, layout.center, mode).kind, expected);
    // A compact visual must not make the pre-existing safe centre target
    // harder to reach; this point is outside the new visible grip.
    QCOMPARE(hitTestTransformGizmo(layout, layout.center + QPointF(6, 0), mode).kind,
             expected);
  }
}

void ModelInteractionTests::projectsFrontAndBackRingDepths() {
  QMatrix4x4 view;
  view.lookAt({0, 0, 10}, {}, {0, 1, 0});
  for (const bool orthographic : {false, true}) {
    QMatrix4x4 projection;
    if (orthographic) projection.ortho(-4, 4, -4, 4, 0.1F, 100.0F);
    else projection.perspective(50, 1, 0.1F, 100.0F);
    const auto layout = transformGizmoLayout({}, {}, projection * view,
                                            {800, 800}, TransformGizmoMode::Rotate, 1);
    QVERIFY(layout.valid);
    // These depths follow independently from the fixture's near/far planes
    // and 10-unit view distance, not another call to the projection code.
    const float expectedPivot = orthographic ? -0.8018018F : 0.981982F;
    QVERIFY(std::abs(layout.pivotDepth - expectedPivot) < 0.0001F);
    bool front = false, back = false;
    QVERIFY(!layout.axes[0].depthSegments.isEmpty());
    for (const auto &stroke : layout.axes[0].depthSegments) {
      QVERIFY(std::isfinite(stroke.startDepth) && std::isfinite(stroke.endDepth));
      QVERIFY(stroke.startDepth >= -1 && stroke.startDepth <= 1);
      QVERIFY(stroke.endDepth >= -1 && stroke.endDepth <= 1);
      const float middleDepth = (stroke.startDepth + stroke.endDepth) * 0.5F;
      front |= middleDepth < layout.pivotDepth - 0.00001F;
      back |= middleDepth > layout.pivotDepth + 0.00001F;
    }
    QVERIFY2(front && back, "A tilted ring must expose both spatial half-rings");
  }
}

void ModelInteractionTests::projectsShadedSolidHandleFaces() {
  QMatrix4x4 view;
  view.lookAt({5, 4, 8}, {}, {0, 1, 0});
  for (const bool orthographic : {false, true}) {
    QMatrix4x4 projection;
    if (orthographic) projection.ortho(-4, 4, -4, 4, 0.1F, 100.0F);
    else projection.perspective(50, 1, 0.1F, 100.0F);
    const auto layout = transformGizmoLayout({}, {}, projection * view,
                                            {800, 800}, TransformGizmoMode::Transform, 1);
    QVERIFY(layout.valid);
    float darkest = 1, brightest = 0;
    for (int axis = 0; axis < 3; ++axis) {
      const auto &grip = layout.axes[static_cast<std::size_t>(axis)];
      QVERIFY(!grip.moveFaces.isEmpty());
      QVERIFY(!grip.scaleFaces.isEmpty());
      for (const auto &faces : {grip.moveFaces, grip.scaleFaces}) {
        for (const auto &face : faces) {
          QVERIFY(face.polygon.size() >= 3);
          QVERIFY(std::isfinite(face.normalizedDepth));
          QVERIFY(face.normalizedDepth >= -1 && face.normalizedDepth <= 1);
          QVERIFY(face.light >= 0.45F && face.light <= 1);
          darkest = std::min(darkest, face.light);
          brightest = std::max(brightest, face.light);
          for (const auto &point : face.polygon) {
            QVERIFY(std::isfinite(point.x()) && std::isfinite(point.y()));
            QVERIFY(QRectF(0, 0, 800, 800).contains(point));
          }
        }
      }
    }
    QVERIFY2(brightest - darkest > 0.15F,
             "Different solid faces must convey an observable lighting contrast");
  }
}

void ModelInteractionTests::picksNearestOverlappingRing() {
  TransformGizmoLayout layout;
  layout.valid = true;
  layout.radius = 100;
  layout.center = {100, 100};
  const QLineF sharedStroke({150, 100}, {190, 100});
  for (const int nearAxis : {0, 1}) {
    for (int axis = 0; axis < 2; ++axis) {
      const float depth = axis == nearAxis ? -0.5F : 0.5F;
      auto &ring = layout.axes[static_cast<std::size_t>(axis)];
      ring.rotationSegments = {sharedStroke};
      ring.depthSegments = {{sharedStroke, depth, depth}};
    }
    QCOMPARE(hitTestTransformGizmo(layout, {170, 100}, TransformGizmoMode::Rotate),
             (TransformGizmoHandle{TransformGizmoHandleKind::RotateAxis, nearAxis}));
  }
}

void ModelInteractionTests::picksSolidHandleSilhouettes() {
  QMatrix4x4 view;
  view.lookAt({5, 4, 8}, {}, {0, 1, 0});
  QMatrix4x4 projection;
  projection.ortho(-1.2F, 1.2F, -1.2F, 1.2F, 0.1F, 100.0F);
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Scale}) {
    const auto layout = transformGizmoLayout({}, {}, projection * view,
                                            {800, 800}, mode, 1);
    bool reachedExpandedSilhouette = false;
    for (int axis = 0; axis < 3; ++axis) {
      const auto &grip = layout.axes[static_cast<std::size_t>(axis)];
      const auto &faces = mode == TransformGizmoMode::Move ? grip.moveFaces : grip.scaleFaces;
      for (const auto &face : faces) {
        QPointF centre;
        for (const auto &point : face.polygon) centre += point;
        centre /= face.polygon.size();
        for (const auto &corner : face.polygon) {
          const QPointF point = corner * 0.85 + centre * 0.15;
          const bool legacyHead = mode == TransformGizmoMode::Move
              ? grip.moveArrow.containsPoint(point, Qt::OddEvenFill) ||
                    pointPolylineDistance(point, grip.moveArrow, true) <= 3
              : grip.scaleHandle.adjusted(-3, -3, 3, 3).contains(point);
          const QLineF line = mode == TransformGizmoMode::Move ? grip.moveLine : grip.scaleLine;
          if (legacyHead || pointPolylineDistance(point, {line.p1(), line.p2()}, false) <= 8.1)
            continue;
          reachedExpandedSilhouette = true;
          QCOMPARE(hitTestTransformGizmo(layout, point, mode),
                   (TransformGizmoHandle{mode == TransformGizmoMode::Move
                       ? TransformGizmoHandleKind::MoveAxis : TransformGizmoHandleKind::ScaleAxis,
                       axis}));
        }
      }
    }
    QVERIFY2(reachedExpandedSilhouette,
             "The oblique solid silhouette must extend beyond the old flat grip");
  }
}

void ModelInteractionTests::picksNearestOverlappingSolidHandles() {
  TransformGizmoLayout layout;
  layout.valid = true;
  layout.radius = 100;
  layout.center = {100, 100};
  const QPolygonF arrow{{160, 100}, {145, 95}, {145, 105}};
  for (const int nearAxis : {1, 0}) {
    for (int axis = 0; axis < 2; ++axis) {
      auto &grip = layout.axes[static_cast<std::size_t>(axis)];
      grip.visible = true;
      grip.moveArrow = arrow;
      grip.scaleHandle = {145, 95, 15, 10};
      grip.moveEndpointDepth = grip.scaleEndpointDepth = axis == nearAxis ? -0.5F : 0.5F;
    }
    QCOMPARE(hitTestTransformGizmo(layout, {150, 100}, TransformGizmoMode::Move),
             (TransformGizmoHandle{TransformGizmoHandleKind::MoveAxis, nearAxis}));
    QCOMPARE(hitTestTransformGizmo(layout, {150, 100}, TransformGizmoMode::Scale),
             (TransformGizmoHandle{TransformGizmoHandleKind::ScaleAxis, nearAxis}));
  }
}

void ModelInteractionTests::picksNearestOverlappingShaftsAndPlanes() {
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Scale}) {
    for (const int nearAxis : {0, 1}) {
      TransformGizmoLayout layout;
      layout.valid = true;
      layout.radius = 100;
      layout.center = {100, 100};
      for (int axis = 0; axis < 2; ++axis) {
        auto &grip = layout.axes[static_cast<std::size_t>(axis)];
        grip.visible = true;
        grip.moveLine = grip.scaleLine = {{150, 100}, {190, 100}};
        grip.moveEndpoint = grip.scaleEndpoint = {190, 100};
        grip.moveEndpointDepth = grip.scaleEndpointDepth = axis == nearAxis ? -0.5F : 0.5F;
      }
      QCOMPARE(hitTestTransformGizmo(layout, {170, 100}, mode),
               (TransformGizmoHandle{mode == TransformGizmoMode::Move
                   ? TransformGizmoHandleKind::MoveAxis : TransformGizmoHandleKind::ScaleAxis,
                   nearAxis}));
      for (int axis = 0; axis < 2; ++axis) {
        layout.axes[static_cast<std::size_t>(axis)].visible = false;
        layout.planeHandles[static_cast<std::size_t>(axis)] = QPolygonF(QRectF(150, 95, 40, 10));
        layout.planeDepths[static_cast<std::size_t>(axis)] = axis == nearAxis ? -0.5F : 0.5F;
      }
      QCOMPARE(hitTestTransformGizmo(layout, {170, 100}, mode),
               (TransformGizmoHandle{mode == TransformGizmoMode::Move
                   ? TransformGizmoHandleKind::MovePlane : TransformGizmoHandleKind::ScalePlane,
                   nearAxis}));
    }
  }
}

void ModelInteractionTests::scalesTransformHandlesWithViewDistance() {
  QMatrix4x4 projection;
  projection.perspective(50.0F, 1.0F, 0.1F, 1000.0F);
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Rotate,
                          TransformGizmoMode::Scale, TransformGizmoMode::Transform}) {
    qreal previousDiameter = 0.0;
    qreal previousAxisLength = 0.0;
    std::array<qreal, 5> previousParts{};
    for (const float distance : {32.0F, 16.0F, 8.0F, 4.0F, 2.0F}) {
      QMatrix4x4 view;
      view.lookAt(QVector3D(0, 0, distance), QVector3D(), QVector3D(0, 1, 0));
      const auto layout = transformGizmoLayout(
          QVector3D(), QQuaternion(), projection * view, QSizeF(800, 800), mode, 1.0);
      QVERIFY(layout.valid);
      const qreal diameter = layout.centerHandle.width();
      const qreal axisLength = QLineF(layout.center, layout.axes[0].moveEndpoint).length();
      const std::array<qreal, 5> parts{
          layout.axes[0].moveArrow.boundingRect().width(),
          layout.axes[0].scaleHandle.width(), layout.planeHandles[2].boundingRect().width(),
          layout.viewRing.boundingRect().width(), layout.trackballBounds.width()};
      if (previousDiameter > 0.0) {
        QVERIFY2(std::abs(diameter / previousDiameter - 2.0) < 0.001,
                 "Center handles must double when the camera distance halves, without pixel clamps");
        QVERIFY(std::abs(axisLength / previousAxisLength - 2.0) < 0.001);
        for (std::size_t part = 0; part < parts.size(); ++part) {
          QVERIFY(std::abs(parts[part] / previousParts[part] - 2.0) < 0.001);
        }
      }
      previousDiameter = diameter;
      previousAxisLength = axisLength;
      previousParts = parts;
    }
  }
}

void ModelInteractionTests::projectsWorldAxesAndPlanes() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0, 0, 10), QVector3D(), QVector3D(0, 1, 0));
  QMatrix4x4 projection;
  projection.perspective(50, 1, 0.1F, 100);
  const QMatrix4x4 vp = projection * view;
  const auto project = [&vp](const QVector3D &point) {
    const QVector3D ndc = (vp * QVector4D(point, 1)).toVector3DAffine();
    return QPointF((ndc.x() + 1) * 400, (1 - ndc.y()) * 400);
  };
  const QQuaternion orientation = QQuaternion::fromAxisAndAngle(0, 1, 0, 45);
  const auto layout = transformGizmoLayout({}, orientation, vp, {800, 800},
                                           TransformGizmoMode::Move, 1.0);
  const QVector3D x = orientation.rotatedVector(QVector3D(1, 0, 0));
  const QVector3D y = orientation.rotatedVector(QVector3D(0, 1, 0));
  QVERIFY(QLineF(layout.axes[0].moveEndpoint, project(x * 0.86F)).length() < 0.001);
  QVERIFY(!layout.planeHandles[2].isEmpty());
  QVERIFY(QLineF(layout.planeHandles[2].constFirst(),
                 project((x + y) * (18.0F / 84.0F))).length() < 0.001);
}

void ModelInteractionTests::scalesOrthographicHandlesAndRejectsStaleHitAreas() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0, 0, 10), {}, QVector3D(0, 1, 0));
  for (const auto mode : {TransformGizmoMode::Move, TransformGizmoMode::Rotate,
                          TransformGizmoMode::Scale, TransformGizmoMode::Transform}) {
    qreal previousRadius = 0;
    for (const float extent : {16.0F, 8.0F, 4.0F, 2.0F, 1.0F}) {
      QMatrix4x4 projection;
      projection.ortho(-extent, extent, -extent, extent, 0.1F, 100.0F);
      const auto layout = transformGizmoLayout({}, {}, projection * view, {800, 800}, mode, 1);
      QVERIFY(layout.valid);
      QVERIFY(std::abs(layout.radius - 400.0 / extent) < 0.001);
      if (previousRadius > 0) {
        QVERIFY(std::abs(layout.radius / previousRadius - 2.0) < 0.001);
      }
      previousRadius = layout.radius;
      if (mode == TransformGizmoMode::Scale || mode == TransformGizmoMode::Transform) {
        QCOMPARE(hitTestTransformGizmo(layout, layout.axes[0].scaleHandle.center(), mode),
                 (TransformGizmoHandle{TransformGizmoHandleKind::ScaleAxis, 0}));
      } else if (mode == TransformGizmoMode::Move) {
        QCOMPARE(hitTestTransformGizmo(layout, layout.axes[0].moveArrow.boundingRect().center(), mode),
                 (TransformGizmoHandle{TransformGizmoHandleKind::MoveAxis, 0}));
      } else {
        QCOMPARE(hitTestTransformGizmo(layout, layout.viewRing.constFirst(), mode).kind,
                 TransformGizmoHandleKind::RotateView);
      }
    }
  }
  QMatrix4x4 projection;
  projection.ortho(-150, 150, -150, 150, 0.1F, 100);
  const auto tiny = transformGizmoLayout({}, {}, projection * view, {800, 800},
                                         TransformGizmoMode::Move, 1);
  QVERIFY(!hitTestTransformGizmo(tiny, tiny.center + QPointF(2, 2),
                                 TransformGizmoMode::Move).isValid());
  QCOMPARE(hitTestTransformGizmo(tiny, tiny.axes[0].moveEndpoint,
                                 TransformGizmoMode::Move).kind,
           TransformGizmoHandleKind::MoveAxis);
}

void ModelInteractionTests::keepsClippedRotationRingInteractive() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0, 0, 0.5F), {}, QVector3D(0, 1, 0));
  QMatrix4x4 projection;
  projection.perspective(90, 1, 0.1F, 100);
  const auto layout = transformGizmoLayout({}, {}, projection * view, {800, 800},
                                           TransformGizmoMode::Rotate, 1);
  QVERIFY(layout.valid);
  // The X ring crosses the near plane; its visible lower/upper arc must not
  // disappear just because some other samples lie behind the camera.
  QCOMPARE(hitTestTransformGizmo(layout, layout.center + QPointF(0, 100),
                                 TransformGizmoMode::Rotate),
           (TransformGizmoHandle{TransformGizmoHandleKind::RotateAxis, 0}));
}

void ModelInteractionTests::unprojectsViewportCentre() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0.0F, 0.0F, 10.0F), QVector3D(),
              QVector3D(0.0F, 1.0F, 0.0F));
  QMatrix4x4 projection;
  projection.perspective(60.0F, 1.0F, 0.1F, 100.0F);

  const std::optional<WorldRay> ray =
      screenRay(QPointF(400.0, 400.0), QSize(800, 800), projection * view);
  QVERIFY(ray.has_value());
  QVERIFY(std::abs(ray->direction.x()) < 1.0e-5F);
  QVERIFY(std::abs(ray->direction.y()) < 1.0e-5F);
  QVERIFY(ray->direction.z() < -0.999F);
}

void ModelInteractionTests::intersectsTranslatedBoundsAndRejectsEmptySpace() {
  const WorldRay hitRay{QVector3D(5.0F, 2.0F, 10.0F),
                        QVector3D(0.0F, 0.0F, -1.0F)};
  const auto hit = rayAabbDistance(hitRay, QVector3D(4.0F, 1.0F, -1.0F),
                                   QVector3D(6.0F, 3.0F, 1.0F));
  QVERIFY(hit.has_value());
  QVERIFY(std::abs(*hit - 9.0F) < 1.0e-5F);

  const WorldRay missRay{QVector3D(-5.0F, 2.0F, 10.0F),
                         QVector3D(0.0F, 0.0F, -1.0F)};
  QVERIFY(!rayAabbDistance(missRay, QVector3D(4.0F, 1.0F, -1.0F),
                           QVector3D(6.0F, 3.0F, 1.0F))
               .has_value());
}

void ModelInteractionTests::intersectsViewPlaneForFreeMovement() {
  const WorldRay ray{QVector3D(1.0F, 2.0F, 10.0F),
                     QVector3D(0.0F, 0.0F, -1.0F)};
  const auto point = rayPlaneIntersection(ray, QVector3D(0.0F, 0.0F, 3.0F),
                                          QVector3D(0.0F, 0.0F, 1.0F));
  QVERIFY(point.has_value());
  QCOMPARE(*point, QVector3D(1.0F, 2.0F, 3.0F));
}

void ModelInteractionTests::rejectsParallelViewPlaneRay() {
  const WorldRay ray{QVector3D(), QVector3D(1.0F, 0.0F, 0.0F)};
  QVERIFY(!rayPlaneIntersection(ray, QVector3D(),
                                QVector3D(0.0F, 0.0F, 1.0F))
               .has_value());
}

void ModelInteractionTests::buildsPivotedTrsTransformAndInversePickRay() {
  const ModelTransform transform{
      QVector3D(3.0F, -2.0F, 1.0F),
      QQuaternion::fromAxisAndAngle(QVector3D(0.0F, 0.0F, 1.0F), 90.0F),
      QVector3D(2.0F, 3.0F, 4.0F)};
  const QVector3D pivot(5.0F, 0.0F, 0.0F);
  const QMatrix4x4 matrix = transform.matrix(pivot);
  const QVector3D transformedPivot = matrix.map(pivot);
  QVERIFY((transformedPivot - (pivot + transform.translation)).length() <
          1.0e-5F);
  const QVector3D transformedX =
      matrix.map(pivot + QVector3D(1.0F, 0.0F, 0.0F));
  QVERIFY((transformedX -
           (pivot + transform.translation + QVector3D(0.0F, 2.0F, 0.0F)))
              .length() < 1.0e-4F);

  const WorldRay localExpected{QVector3D(5.0F, 0.0F, 10.0F),
                               QVector3D(0.0F, 0.0F, -1.0F)};
  WorldRay worldRay{matrix.map(localExpected.origin),
                    matrix.mapVector(localExpected.direction)};
  worldRay.direction.normalize();
  const auto local = rayInModelSpace(worldRay, matrix);
  QVERIFY(local.has_value());
  QVERIFY((local->origin - localExpected.origin).length() < 1.0e-4F);
  QVERIFY((local->direction - localExpected.direction).length() < 1.0e-5F);
}

void ModelInteractionTests::rotatesEdgeOnAxesInBothProjections() {
  constexpr float radius = 84.0F;
  const float expected = 32.0F / radius * 180.0F / 3.14159265F;
  for (const bool orthographic : {false, true}) {
    for (int component = 0; component < 3; ++component) {
      for (const float tilt : {-0.01F, 0.0F, 0.01F, 0.5F}) {
        for (const bool local : {false, true}) {
          QVector3D axis;
          axis[component] = 1.0F;
          QVector3D camera;
          camera[(component + 1) % 3] = 10.0F;
          camera += axis * tilt;
          if (local) {
            const auto orientation = QQuaternion::fromEulerAngles(23, 37, 11);
            axis = orientation.rotatedVector(axis);
            camera = orientation.rotatedVector(camera);
          }
          QMatrix4x4 view;
          view.lookAt(camera, {}, axis);
          QMatrix4x4 projection;
          if (orthographic) {
            projection.ortho(-4, 4, -3, 3, 0.01F, 100.0F);
          } else {
            projection.perspective(46, 4.0F / 3.0F, 0.01F, 100.0F);
          }
          const QMatrix4x4 vp = projection * view;
          const QPointF start(440, 300);
          const auto drag = [&](const QPointF &delta) {
            return axisRotationDragDegrees(start, start + delta, {}, axis,
                                           vp, {800, 600}, radius);
          };
          QVERIFY(std::abs(drag({32, 0}) - expected) < 0.01F);
          QVERIFY(std::abs(drag({16, 0}) - expected * 0.5F) < 0.01F);
          QVERIFY(std::abs(drag({-32, 0}) + expected) < 0.01F);
          QVERIFY(std::abs(drag({0, 32})) < 0.01F);
          QVERIFY(std::abs(drag({0, 0})) < 0.01F);
          // The fallback remains continuous over several turns, not atan2's
          // +/-180-degree discontinuity or a frozen edge-on ray intersection.
          QVERIFY(std::abs(drag({640, 0}) - expected * 20.0F) < 0.01F);
        }
      }
    }
  }
}

void ModelInteractionTests::preservesFaceOnPlaneRotation() {
  for (const bool orthographic : {false, true}) {
    for (const float side : {-1.0F, 1.0F}) {
      QMatrix4x4 view;
      view.lookAt(QVector3D(0, 0, 10 * side), {}, QVector3D(0, 1, 0));
      QMatrix4x4 projection;
      if (orthographic) {
        projection.ortho(-4, 4, -3, 3, 0.01F, 100.0F);
      } else {
        projection.perspective(46, 4.0F / 3.0F, 0.01F, 100.0F);
      }
      const QMatrix4x4 vp = projection * view;
      const float angle = axisRotationDragDegrees(
          {460, 300}, {400, 240}, {}, {0, 0, 1}, vp, {800, 600}, 84);
      QVERIFY(std::abs(angle - side * 90.0F) < 0.01F);
      QCOMPARE(axisRotationDragDegrees({460, 300}, {400, 240}, {}, {},
                                       vp, {800, 600}, 84), 0.0F);
      QCOMPARE(axisRotationDragDegrees({460, 300}, {400, 240}, {}, {0, 0, 1},
                                       vp, {800, 600}, 0), 0.0F);
    }
  }
}

void ModelInteractionTests::laysOutAndHitsTransformGizmos() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0.0F, 0.0F, 10.0F), QVector3D(),
              QVector3D(0.0F, 1.0F, 0.0F));
  QMatrix4x4 projection;
  projection.perspective(50.0F, 1.0F, 0.1F, 100.0F);
  const TransformGizmoLayout moveLayout = transformGizmoLayout(
      QVector3D(), QQuaternion(), projection * view, QSizeF(800.0, 800.0),
      TransformGizmoMode::Move, 0.979246F);
  QVERIFY(moveLayout.valid);
  QVERIFY(moveLayout.axes[0].visible);
  QVERIFY(moveLayout.axes[1].visible);
  const QPointF movePoint = moveLayout.axes[0].moveLine.center();
  const TransformGizmoHandle moveAxis = hitTestTransformGizmo(
      moveLayout, movePoint, TransformGizmoMode::Move);
  QCOMPARE(moveAxis.kind, TransformGizmoHandleKind::MoveAxis);
  QCOMPARE(moveAxis.axis, 0);

  const TransformGizmoLayout scaleLayout = transformGizmoLayout(
      QVector3D(), QQuaternion(), projection * view, QSizeF(800.0, 800.0),
      TransformGizmoMode::Scale, 0.979246F);
  const TransformGizmoHandle scaleAxis = hitTestTransformGizmo(
      scaleLayout, scaleLayout.axes[0].scaleHandle.center(),
      TransformGizmoMode::Scale);
  QCOMPARE(scaleAxis.kind, TransformGizmoHandleKind::ScaleAxis);
  QCOMPARE(scaleAxis.axis, 0);
  const TransformGizmoHandle uniform = hitTestTransformGizmo(
      scaleLayout, scaleLayout.center, TransformGizmoMode::Scale);
  QCOMPARE(uniform.kind, TransformGizmoHandleKind::ScaleUniform);
  QCOMPARE(uniform.axis, -1);

  const TransformGizmoLayout rotateLayout = transformGizmoLayout(
      QVector3D(), QQuaternion(), projection * view, QSizeF(800.0, 800.0),
      TransformGizmoMode::Rotate, 0.979246F);
  const TransformGizmoHandle viewRotation = hitTestTransformGizmo(
      rotateLayout, rotateLayout.viewRing.constFirst(),
      TransformGizmoMode::Rotate);
  QCOMPARE(viewRotation.kind, TransformGizmoHandleKind::RotateView);
  const TransformGizmoHandle trackball = hitTestTransformGizmo(
      rotateLayout, rotateLayout.center + QPointF(18.0, 21.0),
      TransformGizmoMode::Rotate);
  QCOMPARE(trackball.kind, TransformGizmoHandleKind::RotateTrackball);

  const TransformGizmoLayout combined = transformGizmoLayout(
      QVector3D(), QQuaternion(), projection * view, QSizeF(800.0, 800.0),
      TransformGizmoMode::Transform, 1.725338F);
  QVERIFY(combined.valid);
  const TransformGizmoAxisLayout &combinedX = combined.axes[0];
  const qreal moveDistance =
      QLineF(combined.center, combinedX.moveEndpoint).length();
  const qreal scaleDistance =
      QLineF(combined.center, combinedX.scaleEndpoint).length();
  QVERIFY2(scaleDistance - moveDistance >= 28.0,
           "Combined move and scale handles must have separate screen zones");
  QVERIFY2(combined.rotationHitInnerRadius - scaleDistance >= 6.0,
           "Rotation hits must stay outside the scale-handle zone");
  QCOMPARE(hitTestTransformGizmo(combined,
                                 combinedX.moveArrow.boundingRect().center(),
                                 TransformGizmoMode::Transform),
           (TransformGizmoHandle{TransformGizmoHandleKind::MoveAxis, 0}));
  QCOMPARE(hitTestTransformGizmo(combined, combinedX.scaleHandle.center(),
                                 TransformGizmoMode::Transform),
           (TransformGizmoHandle{TransformGizmoHandleKind::ScaleAxis, 0}));
  QCOMPARE(hitTestTransformGizmo(combined, combinedX.scaleLine.center(),
                                 TransformGizmoMode::Transform),
           (TransformGizmoHandle{TransformGizmoHandleKind::ScaleAxis, 0}));
  QCOMPARE(hitTestTransformGizmo(combined, combined.center,
                                 TransformGizmoMode::Transform),
           (TransformGizmoHandle{TransformGizmoHandleKind::MoveView, -1}));

  const qreal trackballDistance =
      (combined.trackballInnerRadius + combined.trackballOuterRadius) * 0.5;
  const QPointF trackballPoint =
      combined.center + QPointF(-trackballDistance * 0.707,
                                trackballDistance * 0.707);
  QCOMPARE(hitTestTransformGizmo(combined, trackballPoint,
                                 TransformGizmoMode::Transform),
           (TransformGizmoHandle{TransformGizmoHandleKind::RotateTrackball,
                                 -1}));

  QPointF outerAxisRingPoint;
  qreal outerAxisRingDistance = 0.0;
  for (const QPointF &point : combinedX.rotationRing) {
    const qreal distance = QLineF(combined.center, point).length();
    if (distance > outerAxisRingDistance) {
      outerAxisRingDistance = distance;
      outerAxisRingPoint = point;
    }
  }
  QVERIFY(outerAxisRingDistance > scaleDistance + 18.0);
  QCOMPARE(hitTestTransformGizmo(combined, outerAxisRingPoint,
                                 TransformGizmoMode::Transform)
               .kind,
           TransformGizmoHandleKind::RotateAxis);
  QCOMPARE(hitTestTransformGizmo(combined, combined.viewRing.constFirst(),
                                 TransformGizmoMode::Transform)
               .kind,
           TransformGizmoHandleKind::RotateView);

  const QRectF hint = transformGizmoHintRect(
      combined, QSizeF(330.0, 30.0), QSizeF(800.0, 800.0));
  QVERIFY(!hint.isEmpty());
  QVERIFY(QRectF(QPointF(), QSizeF(800.0, 800.0)).contains(hint));
  QVERIFY(!hint.intersects(combined.viewRing.boundingRect().adjusted(
      -12.0, -12.0, 12.0, 12.0)));

  const TransformToolStripLayout tools =
      transformToolStripLayout(QSizeF(1280.0, 720.0), 16.0);
  QCOMPARE(hitTestTransformToolStrip(tools, tools.buttons[3].center()), 3);
  QCOMPARE(hitTestTransformToolStrip(tools, tools.orientationButton.center()),
           4);
}

void ModelInteractionTests::keepsTransformToolsVisible_data() {
  QTest::addColumn<QSizeF>("viewportSize");
  QTest::addColumn<qreal>("fontHeight");
  QTest::addColumn<bool>("horizontal");
  QTest::addColumn<bool>("visible");
  QTest::newRow("normal-vertical") << QSizeF(1280, 720) << qreal(16) << false << true;
  QTest::newRow("short-wide") << QSizeF(800, 180) << qreal(24) << true << true;
  QTest::newRow("short-large-font") << QSizeF(500, 180) << qreal(32) << true << true;
  QTest::newRow("short-wrapped") << QSizeF(180, 180) << qreal(24) << true << true;
  QTest::newRow("narrow-vertical") << QSizeF(90, 400) << qreal(32) << false << true;
  QTest::newRow("compact-wrapped") << QSizeF(100, 100) << qreal(32) << true << true;
  QTest::newRow("minimum-horizontal") << QSizeF(160, 32) << qreal(16) << true << true;
  QTest::newRow("cannot-fit-minimum-hit-width") << QSizeF(31, 200) << qreal(24) << false << false;
  QTest::newRow("empty-viewport") << QSizeF() << qreal(16) << false << false;
}

void ModelInteractionTests::keepsTransformToolsVisible() {
  QFETCH(QSizeF, viewportSize);
  QFETCH(qreal, fontHeight);
  QFETCH(bool, horizontal);
  QFETCH(bool, visible);
  const auto layout = transformToolStripLayout(viewportSize, fontHeight);
  QCOMPARE(layout.horizontal, horizontal);
  const QRectF viewport(QPointF(), viewportSize);
  if (!visible) {
    QVERIFY(layout.background.isEmpty());
    QCOMPARE(hitTestTransformToolStrip(layout, QPointF()), -1);
    return;
  }
  QVERIFY(viewport.contains(layout.background));
  const std::array<QRectF, 5> buttons{
      layout.buttons[0], layout.buttons[1], layout.buttons[2],
      layout.buttons[3], layout.orientationButton};
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const QRectF button = buttons[index];
    QVERIFY2(button.width() >= 32 && button.height() >= 32,
             "Fitting must preserve at least 32 logical pixels for every target");
    QVERIFY(viewport.contains(button));
    QVERIFY(layout.background.contains(button));
    const std::array<QPointF, 5> probes{
        button.center(), button.topLeft() + QPointF(0.25, 0.25),
        button.topRight() + QPointF(-0.25, 0.25),
        button.bottomLeft() + QPointF(0.25, -0.25),
        button.bottomRight() - QPointF(0.25, 0.25)};
    for (const QPointF &probe : probes)
      QCOMPARE(hitTestTransformToolStrip(layout, probe), static_cast<int>(index));
    for (std::size_t other = index + 1; other < buttons.size(); ++other) {
      QVERIFY2(!button.intersects(buttons[other]), "Tool hit regions must not overlap");
      const QRectF next = buttons[other];
      if (qFuzzyCompare(button.top(), next.top()) && next.left() > button.right() &&
          next.left() - button.right() < 32)
        QCOMPARE(hitTestTransformToolStrip(layout,
            QPointF((button.right() + next.left()) * 0.5, button.center().y())), -1);
      if (qFuzzyCompare(button.left(), next.left()) && next.top() > button.bottom() &&
          next.top() - button.bottom() < 32)
        QCOMPARE(hitTestTransformToolStrip(layout,
            QPointF(button.center().x(), (button.bottom() + next.top()) * 0.5)), -1);
    }
  }
  QCOMPARE(hitTestTransformToolStrip(layout,
      QPointF(layout.background.left() - 1, layout.background.center().y())), -1);
  QCOMPARE(hitTestTransformToolStrip(layout,
      QPointF(layout.background.right() + 1, layout.background.center().y())), -1);
}

void ModelInteractionTests::mapsPreciseModelPickNeighborhood() {
  const auto mapping = modelPickViewport(QPointF(100.0, 50.0),
                                         QSize(800, 600), 2.0, 6.0);
  QVERIFY(mapping.has_value());
  QCOMPARE(mapping->framebufferSize, QSize(25, 25));
  QCOMPARE(mapping->sourceViewportSize, QSize(1600, 1200));
  QCOMPARE(mapping->sampleCenter, QPoint(12, 12));
  QCOMPARE(mapping->viewportOrigin, QPoint(-188, -1087));

  std::array<quint8, 25> empty{};
  QVERIFY(!modelPickBufferHasCoverage(empty));
  empty.at(12) = 255;
  QVERIFY(modelPickBufferHasCoverage(empty));
  QVERIFY(!modelPickViewport(QPointF(), QSize(), 1.0).has_value());
}

void ModelInteractionTests::modelsTransformOrientationLocking() {
  QVERIFY(!transformOrientationLocked(TransformGizmoMode::Move));
  QVERIFY(!transformOrientationLocked(TransformGizmoMode::Rotate));
  QVERIFY(transformOrientationLocked(TransformGizmoMode::Scale));
  QVERIFY(transformOrientationLocked(TransformGizmoMode::Transform));

  QVERIFY(!transformUsesLocalOrientation(TransformGizmoMode::Move, false));
  QVERIFY(transformUsesLocalOrientation(TransformGizmoMode::Move, true));
  QVERIFY(transformUsesLocalOrientation(TransformGizmoMode::Scale, false));
  QVERIFY(
      transformUsesLocalOrientation(TransformGizmoMode::Transform, false));

  const TransformToolStripLayout tools =
      transformToolStripLayout(QSizeF(1280.0, 720.0), 16.0);
  QVERIFY(tools.orientationButton.height() >= 28.0);
  QVERIFY(tools.background.contains(tools.orientationButton));
}

void ModelInteractionTests::computesStableAxisAndTrackballRotations() {
  const WorldRay ray{QVector3D(2.0F, 5.0F, 3.0F),
                     QVector3D(0.0F, -1.0F, 0.0F)};
  const auto parameter = rayAxisParameter(
      ray, QVector3D(), QVector3D(1.0F, 0.0F, 0.0F));
  QVERIFY(parameter.has_value());
  QVERIFY(std::abs(*parameter - 2.0F) < 1.0e-5F);
  QVERIFY(std::abs(signedAngleDegrees(QVector3D(1.0F, 0.0F, 0.0F),
                                      QVector3D(0.0F, 1.0F, 0.0F),
                                      QVector3D(0.0F, 0.0F, 1.0F)) -
                   90.0F) < 1.0e-4F);

  QMatrix4x4 view;
  view.lookAt(QVector3D(0.0F, 0.0F, 10.0F), QVector3D(),
              QVector3D(0.0F, 1.0F, 0.0F));
  const QQuaternion delta = trackballRotationDelta(
      QPointF(400.0, 400.0), QPointF(520.0, 400.0),
      QPointF(400.0, 400.0), 240.0F, view);
  QVERIFY(!rotationsEquivalent(delta, QQuaternion()));
  QVERIFY(std::abs(delta.lengthSquared() - 1.0F) < 1.0e-5F);
  const QQuaternion identity = trackballRotationDelta(
      QPointF(400.0, 400.0), QPointF(400.0, 400.0),
      QPointF(400.0, 400.0), 240.0F, view);
  QVERIFY(rotationsEquivalent(identity, QQuaternion()));
}

QTEST_GUILESS_MAIN(ModelInteractionTests)

#include "ModelInteractionTests.moc"
