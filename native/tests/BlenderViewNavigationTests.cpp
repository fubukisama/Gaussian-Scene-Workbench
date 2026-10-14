#include "BlenderViewNavigation.h"

#include <QtTest>

#include <cmath>
#include <limits>

using namespace gsw;

namespace {
QMatrix4x4 cameraView(const QVector3D &target, const float distance,
                     const QVector3D &offset = QVector3D(0, 0, 1),
                     const QVector3D &up = QVector3D(0, 1, 0)) {
  QMatrix4x4 view;
  view.lookAt(target + offset * distance, target, up);
  return view;
}

QPointF projectedPixel(const QVector3D &point, const QMatrix4x4 &view,
                       const QMatrix4x4 &projection, const QSizeF &viewport) {
  const QVector4D clip = projection * view * QVector4D(point, 1);
  return {(clip.x() / clip.w() + 1) * viewport.width() * 0.5,
          (1 - clip.y() / clip.w()) * viewport.height() * 0.5};
}
} // namespace

class BlenderViewNavigationTests final : public QObject {
  Q_OBJECT
private slots:
  void zoomKeepsPointUnderCursor();
  void panMakesSceneFollowPointer();
  void navigationFollowsActualCamera_data();
  void navigationFollowsActualCamera();
  void navigationWorksInObliqueCorrectedCamera();
  void zoomUsesActualClampedProjection();
  void zoomUsesActualClampedDistance();
  void rejectsInvalidCameraInputs();
};

void BlenderViewNavigationTests::zoomKeepsPointUnderCursor() {
  const QSizeF viewport(800, 400);
  QMatrix4x4 projection;
  projection.perspective(90, 2, 0.1F, 100);
  const QVector3D target;
  const QVector3D detail(5, 2.5F, 0);
  const auto before = cameraView(target, 10);
  const auto after = cameraView(target, 5);
  const QPointF cursor(500, 150);
  QVERIFY(QLineF(projectedPixel(detail, before, projection, viewport), cursor).length() < 0.001);

  const auto result = blenderViewZoomTarget(target, before, projection, after,
                                           projection, viewport, cursor);
  QVERIFY(result.has_value());
  QVERIFY((*result - QVector3D(2.5F, 1.25F, 0)).length() < 0.0001F);
  const auto changedView = cameraView(*result, 5);
  QVERIFY(QLineF(projectedPixel(detail, changedView, projection, viewport), cursor).length() < 0.001);
  QCOMPARE(target, QVector3D());
  QCOMPARE(before, cameraView({}, 10));
  QCOMPARE(after, cameraView({}, 5));
}

void BlenderViewNavigationTests::panMakesSceneFollowPointer() {
  const QSizeF viewport(800, 400);
  QMatrix4x4 projection;
  projection.perspective(90, 2, 0.1F, 100);
  const QVector3D target;
  const auto view = cameraView(target, 10);
  const QPointF drag(40, -20);
  const auto result = blenderViewPanTarget(target, view, projection, viewport, drag);
  QVERIFY(result.has_value());
  QVERIFY((*result - QVector3D(-2, -1, 0)).length() < 0.0001F);
  const auto changed = cameraView(*result, 10);
  const auto pixel = projectedPixel({}, changed, projection, viewport);
  QVERIFY(QLineF(pixel, QPointF(440, 180)).length() < 0.001);
  QCOMPARE(target, QVector3D());
  QCOMPARE(view, cameraView({}, 10));
}

void BlenderViewNavigationTests::navigationFollowsActualCamera_data() {
  QTest::addColumn<QSizeF>("viewport");
  QTest::addColumn<bool>("orthographic");
  QTest::addColumn<float>("fieldOfView");
  QTest::addColumn<float>("roll");
  QTest::addColumn<bool>("oppositeView");
  for (const QSizeF size : {QSizeF(800, 400), QSizeF(240, 960), QSizeF(1400, 180)}) {
    for (const bool ortho : {false, true}) {
      for (const float angle : {0.0F, 57.0F, 180.0F}) {
        for (const bool opposite : {false, true}) {
          const QByteArray row = QStringLiteral("%1x%2-%3-roll%4-%5")
              .arg(size.width()).arg(size.height()).arg(ortho ? "ortho" : "persp")
              .arg(angle).arg(opposite ? "back" : "front").toLatin1();
          QTest::newRow(row.constData()) << size << ortho
              << (size.height() > size.width() ? 35.0F : 90.0F) << angle << opposite;
        }
      }
    }
  }
}

void BlenderViewNavigationTests::navigationFollowsActualCamera() {
  QFETCH(QSizeF, viewport);
  QFETCH(bool, orthographic);
  QFETCH(float, fieldOfView);
  QFETCH(float, roll);
  QFETCH(bool, oppositeView);
  const QVector3D target(12, -7, 3);
  const QVector3D offset(0, 0, oppositeView ? -1.0F : 1.0F);
  const QVector3D up = QQuaternion::fromAxisAndAngle(offset, roll)
                           .rotatedVector(QVector3D(0, 1, 0));
  QMatrix4x4 beforeProjection;
  QMatrix4x4 afterProjection;
  const float aspect = static_cast<float>(viewport.width() / viewport.height());
  if (orthographic) {
    beforeProjection.ortho(-2 * aspect, 2 * aspect, -2, 2, 0.1F, 100);
    afterProjection.ortho(-aspect, aspect, -1, 1, 0.1F, 100);
  } else {
    beforeProjection.perspective(fieldOfView, aspect, 0.1F, 100);
    afterProjection = beforeProjection;
  }
  const auto before = cameraView(target, 8, offset, up);
  const auto after = cameraView(target, 4, offset, up);
  const QVector3D detail = target + QVector3D(0.2F, 0.1F, 0);
  const QPointF cursor = projectedPixel(detail, before, beforeProjection, viewport);
  const auto zoomTarget = blenderViewZoomTarget(target, before, beforeProjection,
      after, afterProjection, viewport, cursor);
  QVERIFY(zoomTarget.has_value());
  const auto changedView = cameraView(*zoomTarget, 4, offset, up);
  QVERIFY(QLineF(projectedPixel(detail, changedView, afterProjection, viewport), cursor).length() < 0.02);

  const QPointF drag(27, -19);
  const auto panTarget = blenderViewPanTarget(target, before, beforeProjection, viewport, drag);
  QVERIFY(panTarget.has_value());
  const auto panView = cameraView(*panTarget, 8, offset, up);
  QVERIFY(QLineF(projectedPixel(detail, panView, beforeProjection, viewport), cursor + drag).length() < 0.02);
  QCOMPARE(target, QVector3D(12, -7, 3));
  QCOMPARE(detail, QVector3D(12.2F, -6.9F, 3));
  QCOMPARE(before, cameraView(target, 8, offset, up));
  QCOMPARE(after, cameraView(target, 4, offset, up));
}

void BlenderViewNavigationTests::zoomUsesActualClampedProjection() {
  const QSizeF viewport(800, 400);
  QMatrix4x4 beforeProjection;
  QMatrix4x4 afterProjection;
  // A requested .5 distance ratio would yield .04, but the actual extent is
  // limited to .05 by the viewport's minimum orthographic half-height.
  beforeProjection.ortho(-0.16F, 0.16F, -0.08F, 0.08F, 0.001F, 100);
  afterProjection.ortho(-0.10F, 0.10F, -0.05F, 0.05F, 0.001F, 100);
  const QPointF cursor(500, 150);
  const QVector3D detail(0.04F, 0.02F, 0);
  const auto before = cameraView({}, 0.08F);
  const auto after = cameraView({}, 0.04F);
  const auto result = blenderViewZoomTarget({}, before, beforeProjection, after,
                                           afterProjection, viewport, cursor);
  QVERIFY(result.has_value());
  QVERIFY((*result - QVector3D(0.015F, 0.0075F, 0)).length() < 0.00001F);
  QVERIFY(QLineF(projectedPixel(detail, cameraView(*result, 0.04F),
      afterProjection, viewport), cursor).length() < 0.001);

  // Once both actual extents are at the same floor, changing distance alone
  // must not introduce a phantom lateral drift.
  const auto floorResult = blenderViewZoomTarget({}, after, afterProjection,
      cameraView({}, 0.02F), afterProjection, viewport, cursor);
  QVERIFY(floorResult.has_value());
  QVERIFY(floorResult->length() < 0.000001F);
}

void BlenderViewNavigationTests::navigationWorksInObliqueCorrectedCamera() {
  const QSizeF viewport(300, 900);
  const QVector3D target(12, -7, 3);
  const QVector3D offset = QVector3D(1, 2, 3).normalized();
  const QVector3D initialRight = QVector3D::crossProduct(-offset, QVector3D(0, 0, 1)).normalized();
  const QVector3D initialUp = QVector3D::crossProduct(initialRight, -offset).normalized();
  const auto rotation = QQuaternion::fromAxisAndAngle(offset, 73);
  const QVector3D up = rotation.rotatedVector(initialUp);
  const QVector3D right = rotation.rotatedVector(initialRight);
  QMatrix4x4 projection;
  projection.perspective(83, 1.0F / 3.0F, 0.01F, 300);
  projection(0, 0) *= 1.7F; // Source-camera focal aspect correction.
  const QVector3D detail = target + right * 0.2F + up * 0.1F;
  const auto before = cameraView(target, 11, offset, up);
  const auto after = cameraView(target, 7, offset, up);
  const QPointF cursor = projectedPixel(detail, before, projection, viewport);
  const auto zoomTarget = blenderViewZoomTarget(target, before, projection,
      after, projection, viewport, cursor);
  QVERIFY(zoomTarget.has_value());
  const QPointF zoomPixel = projectedPixel(detail,
      cameraView(*zoomTarget, 7, offset, up), projection, viewport);
  const qreal zoomError = QLineF(zoomPixel, cursor).length();
  QVERIFY2(zoomError < 0.02, qPrintable(QStringLiteral("anchor error %1 px: before %2,%3 after %4,%5")
      .arg(zoomError).arg(cursor.x()).arg(cursor.y()).arg(zoomPixel.x()).arg(zoomPixel.y())));
  const QPointF drag(-31, 24);
  const auto panTarget = blenderViewPanTarget(target, before, projection, viewport, drag);
  QVERIFY(panTarget.has_value());
  QVERIFY(QLineF(projectedPixel(detail, cameraView(*panTarget, 11, offset, up),
      projection, viewport), cursor + drag).length() < 0.02);
  QCOMPARE(target, QVector3D(12, -7, 3));
}

void BlenderViewNavigationTests::zoomUsesActualClampedDistance() {
  const QSizeF viewport(800, 400);
  QMatrix4x4 projection;
  projection.perspective(90, 2, 0.1F, 100);
  const QPointF cursor(500, 150);
  const QVector3D detail(5, 2.5F, 0);
  // The caller already clamped the requested distance5 to its actual limit9.
  const auto result = blenderViewZoomTarget({}, cameraView({}, 10), projection,
      cameraView({}, 9), projection, viewport, cursor);
  QVERIFY(result.has_value());
  QVERIFY((*result - QVector3D(0.5F, 0.25F, 0)).length() < 0.0001F);
  QVERIFY(QLineF(projectedPixel(detail, cameraView(*result, 9), projection,
      viewport), cursor).length() < 0.002);
}

void BlenderViewNavigationTests::rejectsInvalidCameraInputs() {
  const QSizeF viewport(800, 400);
  QMatrix4x4 projection;
  projection.perspective(90, 2, 0.1F, 100);
  const auto before = cameraView({}, 10);
  const auto after = cameraView({}, 5);
  const QPointF cursor(500, 150);
  const QPointF drag(20, 10);
  const qreal infinity = std::numeric_limits<qreal>::infinity();
  for (const QSizeF size : {QSizeF(), QSizeF(0, 400), QSizeF(800, -1),
                            QSizeF(infinity, 400)}) {
    QVERIFY(!blenderViewZoomTarget({}, before, projection, after, projection, size, cursor));
    QVERIFY(!blenderViewPanTarget({}, before, projection, size, drag));
  }
  QMatrix4x4 singular;
  singular.fill(0);
  QMatrix4x4 invalid;
  invalid(0, 0) = std::numeric_limits<float>::quiet_NaN();
  for (const QMatrix4x4 matrix : {singular, invalid}) {
    QVERIFY(!blenderViewZoomTarget({}, matrix, projection, after, projection, viewport, cursor));
    QVERIFY(!blenderViewZoomTarget({}, before, matrix, after, projection, viewport, cursor));
    QVERIFY(!blenderViewZoomTarget({}, before, projection, matrix, projection, viewport, cursor));
    QVERIFY(!blenderViewZoomTarget({}, before, projection, after, matrix, viewport, cursor));
    QVERIFY(!blenderViewPanTarget({}, matrix, projection, viewport, drag));
    QVERIFY(!blenderViewPanTarget({}, before, matrix, viewport, drag));
  }
  const QVector3D invalidTarget(std::numeric_limits<float>::infinity(), 0, 0);
  QVERIFY(!blenderViewZoomTarget(invalidTarget, before, projection, after, projection, viewport, cursor));
  QVERIFY(!blenderViewPanTarget(invalidTarget, before, projection, viewport, drag));
  QVERIFY(!blenderViewZoomTarget({}, before, projection, after, projection, viewport, QPointF(infinity, 0)));
  QVERIFY(!blenderViewPanTarget({}, before, projection, viewport, QPointF(0, infinity)));
}

QTEST_GUILESS_MAIN(BlenderViewNavigationTests)
#include "BlenderViewNavigationTests.moc"
