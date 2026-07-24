#include "ViewportCamera.h"

#include <QtTest>

using namespace gsw;

class ViewportCameraTests final : public QObject {
  Q_OBJECT

private slots:
  void mapsHorizontalAndVerticalLeftDragDirections();
  void allowsVerticalOrbitPastBothPoles();
  void wrapsAnglesAfterCompleteTurns();
  void usesZAsWorldUpAxis();
  void keepsCameraFrameContinuousAcrossPoles();
};

void ViewportCameraTests::mapsHorizontalAndVerticalLeftDragDirections() {
  const OrbitAngles horizontal =
      orbitAnglesAfterLeftDrag({42.0F, 24.0F}, QPoint(25, 0));
  QCOMPARE(horizontal.yawDegrees, 34.0F);
  QCOMPARE(horizontal.pitchDegrees, 24.0F);

  const OrbitAngles vertical =
      orbitAnglesAfterLeftDrag({42.0F, 24.0F}, QPoint(0, 10));
  QCOMPARE(vertical.yawDegrees, 42.0F);
  QVERIFY(qAbs(vertical.pitchDegrees - 26.8F) < 1.0e-5F);
}

void ViewportCameraTests::allowsVerticalOrbitPastBothPoles() {
  const OrbitAngles pastNorth =
      orbitAnglesAfterLeftDrag({0.0F, 85.0F}, QPoint(0, 30));
  QVERIFY(qAbs(pastNorth.pitchDegrees - 93.4F) < 1.0e-5F);

  const OrbitAngles pastSouth =
      orbitAnglesAfterLeftDrag({0.0F, -85.0F}, QPoint(0, -30));
  QVERIFY(qAbs(pastSouth.pitchDegrees + 93.4F) < 1.0e-5F);
}

void ViewportCameraTests::wrapsAnglesAfterCompleteTurns() {
  const OrbitAngles wrapped =
      orbitAnglesAfterLeftDrag({179.0F, 179.0F}, QPoint(-10, -10));
  QVERIFY(wrapped.yawDegrees >= -180.0F && wrapped.yawDegrees < 180.0F);
  QVERIFY(wrapped.pitchDegrees >= -180.0F && wrapped.pitchDegrees < 180.0F);

  const OrbitAngles fullPitchTurn =
      orbitAnglesAfterLeftDrag({0.0F, 0.0F}, QPoint(0, 1286));
  QVERIFY(qAbs(fullPitchTurn.pitchDegrees) < 0.1F);
}

void ViewportCameraTests::usesZAsWorldUpAxis() {
  const OrbitFrame frame = orbitFrame({0.0F, 0.0F});

  QCOMPARE(frame.cameraOffsetDirection, QVector3D(0.0F, 1.0F, 0.0F));
  QCOMPARE(frame.upDirection, QVector3D(0.0F, 0.0F, 1.0F));
}

void ViewportCameraTests::keepsCameraFrameContinuousAcrossPoles() {
  const OrbitFrame before = orbitFrame({37.0F, 89.9F});
  const OrbitFrame after = orbitFrame({37.0F, 90.1F});

  QVERIFY(QVector3D::dotProduct(before.cameraOffsetDirection,
                                after.cameraOffsetDirection) > 0.999F);
  QVERIFY(QVector3D::dotProduct(before.upDirection, after.upDirection) >
          0.999F);
  QVERIFY(qAbs(QVector3D::dotProduct(after.cameraOffsetDirection,
                                     after.upDirection)) < 1.0e-5F);
  QVERIFY(qAbs(after.cameraOffsetDirection.length() - 1.0F) < 1.0e-5F);
  QVERIFY(qAbs(after.upDirection.length() - 1.0F) < 1.0e-5F);
}

QTEST_GUILESS_MAIN(ViewportCameraTests)

#include "ViewportCameraTests.moc"
