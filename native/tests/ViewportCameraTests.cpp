#include "ViewportCamera.h"

#include <QtTest>

using namespace gsw;

class ViewportCameraTests final : public QObject {
  Q_OBJECT

private slots:
  void reversesHorizontalAndVerticalLeftDragRotation();
  void clampsPitchAfterReversedVerticalDrag();
};

void ViewportCameraTests::reversesHorizontalAndVerticalLeftDragRotation() {
  const OrbitAngles horizontal =
      orbitAnglesAfterLeftDrag({42.0F, 24.0F}, QPoint(25, 0));
  QCOMPARE(horizontal.yawDegrees, 34.0F);
  QCOMPARE(horizontal.pitchDegrees, 24.0F);

  const OrbitAngles vertical =
      orbitAnglesAfterLeftDrag({42.0F, 24.0F}, QPoint(0, 10));
  QCOMPARE(vertical.yawDegrees, 42.0F);
  QVERIFY(qAbs(vertical.pitchDegrees - 21.2F) < 1.0e-5F);
}

void ViewportCameraTests::clampsPitchAfterReversedVerticalDrag() {
  QCOMPARE(orbitAnglesAfterLeftDrag({0.0F, 0.0F}, QPoint(0, 1000))
               .pitchDegrees,
           -86.0F);
  QCOMPARE(orbitAnglesAfterLeftDrag({0.0F, 0.0F}, QPoint(0, -1000))
               .pitchDegrees,
           86.0F);
}

QTEST_GUILESS_MAIN(ViewportCameraTests)

#include "ViewportCameraTests.moc"
