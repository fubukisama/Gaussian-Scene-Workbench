#include "NavigationGizmo.h"

#include <QMatrix4x4>
#include <QtTest>

using namespace gsw;

class NavigationGizmoTests final : public QObject {
  Q_OBJECT

private slots:
  void projectsAxesAndHidesAlignedBackHandle();
  void selectsNearestAxisInsideRotationCircle();
  void exposesUnityStyleProjectionControl();
  void mapsSixAxisViews();
  void keepsNavigationButtonsInsideViewport();
};

void NavigationGizmoTests::projectsAxesAndHidesAlignedBackHandle() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0.0F, 10.0F, 0.0F), QVector3D(),
              QVector3D(0.0F, 0.0F, 1.0F));
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(view, QSizeF(800.0, 600.0), 18.0);

  const NavigationAxisHandle &negativeX = layout.handles[0];
  const NavigationAxisHandle &positiveX = layout.handles[1];
  const NavigationAxisHandle &negativeY = layout.handles[2];
  const NavigationAxisHandle &positiveY = layout.handles[3];
  const NavigationAxisHandle &positiveZ = layout.handles[5];

  QVERIFY(negativeX.center.x() > layout.center.x());
  QVERIFY(positiveX.center.x() < layout.center.x());
  QVERIFY(positiveZ.center.y() < layout.center.y());
  QVERIFY(negativeY.hidden);
  QVERIFY(!positiveY.hidden);
  QCOMPARE(positiveY.center, layout.center);
}

void NavigationGizmoTests::selectsNearestAxisInsideRotationCircle() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0.0F, 10.0F, 0.0F), QVector3D(),
              QVector3D(0.0F, 0.0F, 1.0F));
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(view, QSizeF(800.0, 600.0), 18.0);

  QCOMPARE(hitTestNavigationGizmo(layout, layout.handles[1].center),
           (NavigationGizmoHit{NavigationGizmoPart::Rotate,
                               NavigationAxis::PositiveX}));
  QCOMPARE(hitTestNavigationGizmo(layout, layout.zoomButton.center()).part,
           NavigationGizmoPart::Zoom);
  QCOMPARE(hitTestNavigationGizmo(layout, layout.cameraButton.center()).part,
           NavigationGizmoPart::Camera);
  QCOMPARE(hitTestNavigationGizmo(layout, QPointF(10.0, 10.0)).part,
           NavigationGizmoPart::None);
}

void NavigationGizmoTests::exposesUnityStyleProjectionControl() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(800.0, 600.0), 18.0);

  QCOMPARE(hitTestNavigationGizmo(layout, layout.projectionCube.center()).part,
           NavigationGizmoPart::Projection);
  QCOMPARE(hitTestNavigationGizmo(layout, layout.projectionLabel.center()).part,
           NavigationGizmoPart::Projection);
  QCOMPARE(hitTestNavigationGizmo(
               layout,
               layout.center + QPointF(layout.radius * 0.62,
                                       layout.radius * 0.18))
               .part,
           NavigationGizmoPart::Rotate);
}

void NavigationGizmoTests::mapsSixAxisViews() {
  QCOMPARE(navigationAxisViewAngles(NavigationAxis::PositiveX),
           std::optional<OrbitAngles>({90.0F, 0.0F}));
  QCOMPARE(navigationAxisViewAngles(NavigationAxis::NegativeX),
           std::optional<OrbitAngles>({-90.0F, 0.0F}));
  QCOMPARE(navigationAxisViewAngles(NavigationAxis::PositiveY),
           std::optional<OrbitAngles>({0.0F, 0.0F}));
  QCOMPARE(navigationAxisViewAngles(NavigationAxis::NegativeY),
           std::optional<OrbitAngles>({180.0F, 0.0F}));
  QCOMPARE(navigationAxisViewAngles(NavigationAxis::PositiveZ),
           std::optional<OrbitAngles>({0.0F, 90.0F}));
  QCOMPARE(navigationAxisViewAngles(NavigationAxis::NegativeZ),
           std::optional<OrbitAngles>({0.0F, -90.0F}));
}

void NavigationGizmoTests::keepsNavigationButtonsInsideViewport() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(420.0, 280.0), 24.0);
  const QRectF viewport(0.0, 0.0, 420.0, 280.0);
  QVERIFY(viewport.contains(layout.rotateBounds));
  QVERIFY(viewport.contains(layout.zoomButton));
  QVERIFY(viewport.contains(layout.panButton));
  QVERIFY(viewport.contains(layout.cameraButton));
  QVERIFY(viewport.contains(layout.projectionCube));
  QVERIFY(viewport.contains(layout.projectionLabel));
}

QTEST_GUILESS_MAIN(NavigationGizmoTests)

#include "NavigationGizmoTests.moc"
