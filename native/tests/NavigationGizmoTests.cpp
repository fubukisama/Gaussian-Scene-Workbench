#include "NavigationGizmo.h"

#include <QLineF>
#include <QMatrix4x4>
#include <QtTest>

#include <cmath>

using namespace gsw;

class NavigationGizmoTests final : public QObject {
  Q_OBJECT

private slots:
  void projectsAxesAndHidesAlignedBackHandle();
  void selectsNearestAxisInsideRotationCircle();
  void exposesSeparateBlenderProjectionControl();
  void preservesBlenderProjectedAxisLengths();
  void alignedCenterSelectsOppositeAxis();
  void obliqueCenterSelectsNearestAxisWithoutProjection();
  void selectsVisibleCircularHandleRims();
  void selectsNearestAxisInUnpaintedCircleArea();
  void usesRawProjectedAxesForSelection();
  void keepsBlenderStableTieOrder();
  void circularButtonCornersDoNotActivate();
  void excludesPointsOutsideRotationCircle();
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
  QVERIFY(positiveY.aligned);
  QCOMPARE(positiveY.center, layout.center);
  QCOMPARE(hitTestNavigationGizmo(layout, layout.center),
           (NavigationGizmoHit{NavigationGizmoPart::Rotate,
                               NavigationAxis::NegativeY}));
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

void NavigationGizmoTests::exposesSeparateBlenderProjectionControl() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(800.0, 600.0), 18.0);

  QCOMPARE(hitTestNavigationGizmo(layout, layout.projectionButton.center()).part,
           NavigationGizmoPart::Projection);
  QVERIFY(layout.projectionButton.top() > layout.cameraButton.bottom());
  QCOMPARE(layout.projectionButton.size(), layout.cameraButton.size());
  QCOMPARE(layout.projectionButton.center().x(), layout.cameraButton.center().x());
  QVERIFY(!layout.rotateBounds.intersects(layout.projectionButton));
  QCOMPARE(hitTestNavigationGizmo(layout, layout.projectionLabel.center()).part,
           NavigationGizmoPart::None);
  QVERIFY(hitTestNavigationGizmo(layout, layout.center).part !=
          NavigationGizmoPart::Projection);
  QCOMPARE(hitTestNavigationGizmo(layout,
                                  layout.center + QPointF(layout.radius * 0.62,
                                                          layout.radius * 0.18))
               .part,
           NavigationGizmoPart::Rotate);
}

void NavigationGizmoTests::preservesBlenderProjectedAxisLengths() {
  // A nearly end-on axis must approach the center continuously. Reserving a
  // minimum radius for a center button causes the old artificial outward jump.
  qreal previousExtent = 0.0;
  for (const float eyeX : {0.02F, 0.4F, 2.0F, 5.0F}) {
    QMatrix4x4 view;
    view.lookAt(QVector3D(eyeX, 10.0F, 0.0F), QVector3D(),
                QVector3D(0.0F, 0.0F, 1.0F));
    const auto layout = navigationGizmoLayout(view, QSizeF(800.0, 600.0), 18.0);
    const auto &nearY = layout.handles[3];
    const auto &farY = layout.handles[2];
    QVERIFY(!nearY.hidden);
    QVERIFY(!farY.hidden);
    const qreal extent = QLineF(layout.center, nearY.center).length();
    const qreal expected = layout.radius * 0.8 * eyeX / std::hypot(eyeX, 10.0F);
    QVERIFY(std::abs(extent - expected) < 0.001);
    QVERIFY(extent > previousExtent);
    previousExtent = extent;
    QVERIFY(nearY.radius > farY.radius);
    QVERIFY(std::abs(QLineF(layout.center, farY.center).length() - extent) < 0.001);
    QCOMPARE(hitTestNavigationGizmo(layout, nearY.center),
             (NavigationGizmoHit{NavigationGizmoPart::Rotate, nearY.axis}));
  }
}

void NavigationGizmoTests::alignedCenterSelectsOppositeAxis() {
  for (int index = 0; index < 3; ++index) {
    for (const float sign : {-1.0F, 1.0F}) {
      QVector3D eye;
      eye[index] = 10.0F * sign;
      QMatrix4x4 view;
      view.lookAt(eye, QVector3D(), index == 2 ? QVector3D(0, 1, 0)
                                               : QVector3D(0, 0, 1));
      const auto layout = navigationGizmoLayout(view, QSizeF(800.0, 600.0), 18.0);
      const auto &near = layout.handles[static_cast<std::size_t>(index * 2 + (sign > 0 ? 1 : 0))];
      const auto &far = layout.handles[static_cast<std::size_t>(index * 2 + (sign > 0 ? 0 : 1))];
      QVERIFY(near.aligned);
      QVERIFY(!near.hidden);
      QVERIFY(far.hidden);
      QCOMPARE(near.center, layout.center);
      QCOMPARE(hitTestNavigationGizmo(layout, layout.center),
               (NavigationGizmoHit{NavigationGizmoPart::Rotate, far.axis}));
    }
  }
}

void NavigationGizmoTests::obliqueCenterSelectsNearestAxisWithoutProjection() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(6, 8, 5), QVector3D(), QVector3D(0, 0, 1));
  const auto layout = navigationGizmoLayout(view, QSizeF(800.0, 600.0), 18.0);
  for (const auto &handle : layout.handles) QVERIFY(!handle.hidden);
  QCOMPARE(hitTestNavigationGizmo(layout, layout.center),
           (NavigationGizmoHit{NavigationGizmoPart::Rotate, NavigationAxis::NegativeY}));
}

void NavigationGizmoTests::selectsVisibleCircularHandleRims() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(800.0, 600.0), 18.0);
  for (const auto &handle : layout.handles) {
    if (handle.hidden || handle.aligned) continue;
    const QPointF direction = (handle.center - layout.center) /
        QLineF(layout.center, handle.center).length();
    const QPointF side(-direction.y(), direction.x());
    for (const QPointF offset : {direction, -direction, side, -side}) {
      const QPointF point = handle.center + offset * (handle.radius * 0.85);
      QCOMPARE(hitTestNavigationGizmo(layout, point),
               (NavigationGizmoHit{NavigationGizmoPart::Rotate, handle.axis}));
    }
  }
}

void NavigationGizmoTests::selectsNearestAxisInUnpaintedCircleArea() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(800.0, 600.0), 18.0);
  for (const auto &handle : layout.handles) {
    if (handle.hidden || handle.aligned) continue;
    const QPointF direction = (handle.center - layout.center) /
        QLineF(layout.center, handle.center).length();
    const QPointF empty = handle.center - direction * (handle.radius + 4.0);
    QCOMPARE(hitTestNavigationGizmo(layout, empty),
             (NavigationGizmoHit{NavigationGizmoPart::Rotate, handle.axis}));
  }
}

void NavigationGizmoTests::keepsBlenderStableTieOrder() {
  NavigationGizmoLayout layout;
  layout.center = {200, 200};
  layout.radius = 100;
  layout.handles[0] = {NavigationAxis::NegativeX, {240, 200}, 12, -0.5F, 0, false, false};
  layout.handles[1] = {NavigationAxis::PositiveX, {240, 200}, 12, 0.5F, 0, true, false};
  QCOMPARE(hitTestNavigationGizmo(layout, {240, 200}),
           (NavigationGizmoHit{NavigationGizmoPart::Rotate, NavigationAxis::NegativeX}));
  std::swap(layout.handles[0].depth, layout.handles[1].depth);
  QCOMPARE(hitTestNavigationGizmo(layout, {240, 200}),
           (NavigationGizmoHit{NavigationGizmoPart::Rotate, NavigationAxis::NegativeX}));
}

void NavigationGizmoTests::usesRawProjectedAxesForSelection() {
  NavigationGizmoLayout layout;
  layout.center = {200, 200};
  layout.radius = 100;
  // Projected unit endpoints are at x=.6 and x=.8. Their painted positions
  // are inset to x=.48 and x=.64. Blender picks the former at x=.6 even though
  // that location is closer to the latter painted disc.
  layout.handles[0] = {NavigationAxis::NegativeX, {248, 200}, 20, 0.8F, 0, false, false};
  layout.handles[1] = {NavigationAxis::PositiveY, {264, 200}, 20, -0.6F, 1, true, false};
  QCOMPARE(hitTestNavigationGizmo(layout, {260, 200}),
           (NavigationGizmoHit{NavigationGizmoPart::Rotate, NavigationAxis::NegativeX}));
}

void NavigationGizmoTests::circularButtonCornersDoNotActivate() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(800.0, 600.0), 18.0);
  for (const QRectF &button : {layout.zoomButton, layout.panButton,
                             layout.cameraButton, layout.projectionButton}) {
    QCOMPARE(hitTestNavigationGizmo(layout, button.topLeft() + QPointF(1, 1)).part,
             NavigationGizmoPart::None);
    QVERIFY(hitTestNavigationGizmo(layout, button.center()).part != NavigationGizmoPart::None);
  }
}

void NavigationGizmoTests::excludesPointsOutsideRotationCircle() {
  const NavigationGizmoLayout layout =
      navigationGizmoLayout(QMatrix4x4(), QSizeF(800.0, 600.0), 18.0);
  for (const QPointF direction : {QPointF(1, 0), QPointF(-1, 0),
                                  QPointF(0, -1), QPointF(0.8, -0.6)}) {
    const QPointF outside = layout.center + direction * (layout.radius + 0.1);
    QCOMPARE(hitTestNavigationGizmo(layout, outside), NavigationGizmoHit{});
    const QPointF inside = layout.center + direction * (layout.radius - 0.1);
    const auto hit = hitTestNavigationGizmo(layout, inside);
    QCOMPARE(hit.part, NavigationGizmoPart::Rotate);
    QVERIFY(hit.axis != NavigationAxis::None);
  }
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
  for (const QSizeF viewportSize : {QSizeF(420, 280), QSizeF(240, 180),
                                    QSizeF(800, 600), QSizeF(1920, 1080)}) {
    for (const qreal fontHeight : {12.0, 18.0, 24.0, 36.0}) {
      const auto layout = navigationGizmoLayout(QMatrix4x4(), viewportSize, fontHeight);
      const QRectF viewport(QPointF(), viewportSize);
      for (const QRectF bounds : {layout.rotateBounds, layout.zoomButton,
                                  layout.panButton, layout.cameraButton,
                                  layout.projectionButton, layout.projectionLabel}) {
        QVERIFY2(viewport.adjusted(-0.01, -0.01, 0.01, 0.01).contains(bounds),
                 "The navigation control must fit both compact and high-DPI viewports");
      }
      QVERIFY(layout.zoomButton.top() > layout.projectionLabel.bottom());
      QVERIFY(layout.panButton.top() > layout.zoomButton.bottom());
      QVERIFY(layout.cameraButton.top() > layout.panButton.bottom());
      QVERIFY(layout.projectionButton.top() > layout.cameraButton.bottom());
      QCOMPARE(hitTestNavigationGizmo(layout, layout.projectionButton.center()).part,
               NavigationGizmoPart::Projection);
      QCOMPARE(hitTestNavigationGizmo(layout, layout.projectionLabel.center()).part,
               NavigationGizmoPart::None);
    }
  }
}

QTEST_GUILESS_MAIN(NavigationGizmoTests)

#include "NavigationGizmoTests.moc"
