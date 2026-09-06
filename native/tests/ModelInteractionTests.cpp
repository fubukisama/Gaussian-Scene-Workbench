#include "ModelInteraction.h"
#include "TransformGizmo.h"

#include <QMatrix4x4>
#include <QTest>

#include <array>

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
  void modelsTransformOrientationLocking();
  void mapsPreciseModelPickNeighborhood();
};

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
      TransformGizmoMode::Move, 84.0);
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
      TransformGizmoMode::Scale, 84.0);
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
      TransformGizmoMode::Rotate, 84.0);
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
      TransformGizmoMode::Transform, 148.0);
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
