#include "ModelInteraction.h"

#include <QMatrix4x4>
#include <QTest>

using namespace gsw;

class ModelInteractionTests final : public QObject {
  Q_OBJECT

private slots:
  void unprojectsViewportCentre();
  void intersectsTranslatedBoundsAndRejectsEmptySpace();
  void intersectsViewPlaneForFreeMovement();
  void rejectsParallelViewPlaneRay();
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

QTEST_GUILESS_MAIN(ModelInteractionTests)

#include "ModelInteractionTests.moc"
