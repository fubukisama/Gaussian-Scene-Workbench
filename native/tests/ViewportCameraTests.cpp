#include "ViewportCamera.h"

#include <QtTest>

#include <limits>

using namespace gsw;

class ViewportCameraTests final : public QObject {
  Q_OBJECT

private slots:
  void mapsHorizontalAndVerticalLeftDragDirections();
  void recognizesTemporaryTrimOrbitShortcut();
  void allowsVerticalOrbitPastBothPoles();
  void wrapsAnglesAfterCompleteTurns();
  void usesZAsWorldUpAxis();
  void keepsCameraFrameContinuousAcrossPoles();
  void selectsScreenParallelGridForAxisOrthographicViews();
  void keepsWorldGroundGridForPerspectiveViews();
  void clampsZoomToSceneAwareFiniteLimits();
  void keepsGridStepsOnConcreteDecimalScales();
  void enforcesMinimumGridStepAtClosestZoom();
  void usesMetashapeStylePointPreviewDiameter();
  void formatsGridScaleWithReadableMetricUnits();
};

void ViewportCameraTests::mapsHorizontalAndVerticalLeftDragDirections() {
  const OrbitAngles horizontal =
      orbitAnglesAfterLeftDrag({42.0F, 24.0F}, QPoint(25, 0));
  QCOMPARE(horizontal.yawDegrees, 50.0F);
  QCOMPARE(horizontal.pitchDegrees, 24.0F);

  const OrbitAngles vertical =
      orbitAnglesAfterLeftDrag({42.0F, 24.0F}, QPoint(0, 10));
  QCOMPARE(vertical.yawDegrees, 42.0F);
  QVERIFY(qAbs(vertical.pitchDegrees - 26.8F) < 1.0e-5F);
}

void ViewportCameraTests::recognizesTemporaryTrimOrbitShortcut() {
  QVERIFY(isTemporaryOrbitShortcut(Qt::LeftButton, Qt::ControlModifier));
  QVERIFY(isTemporaryOrbitShortcut(
      Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier));
  QVERIFY(!isTemporaryOrbitShortcut(Qt::LeftButton, Qt::NoModifier));
  QVERIFY(!isTemporaryOrbitShortcut(Qt::RightButton, Qt::ControlModifier));
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

void ViewportCameraTests::selectsScreenParallelGridForAxisOrthographicViews() {
  QCOMPARE(referenceGridPlane({90.0F, 0.0F}, true),
           ReferenceGridPlane::YZ);
  QCOMPARE(referenceGridPlane({-90.0F, 0.0F}, true),
           ReferenceGridPlane::YZ);
  QCOMPARE(referenceGridPlane({0.0F, 0.0F}, true),
           ReferenceGridPlane::XZ);
  QCOMPARE(referenceGridPlane({180.0F, 0.0F}, true),
           ReferenceGridPlane::XZ);
  QCOMPARE(referenceGridPlane({0.0F, 90.0F}, true),
           ReferenceGridPlane::XY);
  QCOMPARE(referenceGridPlane({0.0F, -90.0F}, true),
           ReferenceGridPlane::XY);
}

void ViewportCameraTests::keepsWorldGroundGridForPerspectiveViews() {
  QCOMPARE(referenceGridPlane({90.0F, 0.0F}, false),
           ReferenceGridPlane::XY);
  QCOMPARE(referenceGridPlane({0.0F, 0.0F}, false),
           ReferenceGridPlane::XY);
}

void ViewportCameraTests::clampsZoomToSceneAwareFiniteLimits() {
  const ViewportZoomLimits limits = viewportZoomLimits(4.0F);

  QVERIFY(limits.minimumDistance > 0.0F);
  QVERIFY(limits.maximumDistance > limits.minimumDistance);
  QCOMPARE(clampViewportDistance(0.0F, 4.0F), limits.minimumDistance);
  QCOMPARE(clampViewportDistance(1.0e9F, 4.0F), limits.maximumDistance);
  QCOMPARE(clampViewportDistance(std::numeric_limits<float>::infinity(), 4.0F),
           limits.maximumDistance);
  QCOMPARE(clampViewportDistance(12.0F, 4.0F), 12.0F);
}

void ViewportCameraTests::keepsGridStepsOnConcreteDecimalScales() {
  const ReferenceGridScale scale = referenceGridScale(12.0F, 800);

  QCOMPARE(scale.minimumStep, 0.001F);
  QCOMPARE(scale.lowerMinorStep, 0.2F);
  QCOMPARE(scale.upperMinorStep, 0.5F);
  QVERIFY(scale.levelBlend >= 0.0F && scale.levelBlend <= 1.0F);
  QVERIFY(scale.visibleDistance >= 12.0F * 8.0F);
  QCOMPARE(scale.displayMajorStep, 5.0F);
}

void ViewportCameraTests::enforcesMinimumGridStepAtClosestZoom() {
  const ReferenceGridScale scale = referenceGridScale(1.0e-9F, 2160);

  QCOMPARE(scale.lowerMinorStep, 0.001F);
  QCOMPARE(scale.upperMinorStep, 0.001F);
  QCOMPARE(scale.levelBlend, 0.0F);
  QCOMPARE(scale.displayMajorStep, 0.01F);
  QVERIFY(scale.visibleDistance > 0.0F);
}

void ViewportCameraTests::usesMetashapeStylePointPreviewDiameter() {
  QCOMPARE(pointPreviewDiameterPixels(1.0F), 1.0F);
  QCOMPARE(pointPreviewDiameterPixels(1.5F), 1.5F);
  QCOMPARE(pointPreviewDiameterPixels(2.0F), 2.0F);
  QCOMPARE(pointPreviewDiameterPixels(0.0F), 1.0F);
  QCOMPARE(pointPreviewDiameterPixels(
               std::numeric_limits<float>::quiet_NaN()),
           1.0F);
}

void ViewportCameraTests::formatsGridScaleWithReadableMetricUnits() {
  QCOMPARE(formatMetricDistance(0.001F), QStringLiteral("1 mm"));
  QCOMPARE(formatMetricDistance(0.009F), QStringLiteral("9 mm"));
  QCOMPARE(formatMetricDistance(0.01F), QStringLiteral("1 cm"));
  QCOMPARE(formatMetricDistance(0.5F), QStringLiteral("50 cm"));
  QCOMPARE(formatMetricDistance(1.0F), QStringLiteral("1 m"));
  QCOMPARE(formatMetricDistance(250.0F), QStringLiteral("250 m"));
}

QTEST_GUILESS_MAIN(ViewportCameraTests)

#include "ViewportCameraTests.moc"
