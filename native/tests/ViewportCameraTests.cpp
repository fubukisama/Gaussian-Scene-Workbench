#include "ViewportCamera.h"
#include "ReferenceAxisGeometry.h"

#include <QtTest>

#include <array>
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
  void framesModelBoundsAtCurrentOrbit();
  void rejectsInvalidFocusBounds();
  void selectsScreenParallelGridForAxisOrthographicViews();
  void keepsWorldGroundGridForObliqueOrthographicViews();
  void keepsWorldGroundGridForPerspectiveViews();
  void clampsZoomToSceneAwareFiniteLimits();
  void keepsGridStepsOnConcreteDecimalScales();
  void enforcesMinimumGridStepAtClosestZoom();
  void keepsColoredAxesInsideRenderedGridCoverage();
  void usesMetashapeStylePointPreviewDiameter();
  void formatsGridScaleWithReadableMetricUnits();
  void keepsReferenceMarkerReadableAcrossZoomAndProjection();
  void hidesReferenceMarkerOutsideView();
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

void ViewportCameraTests::framesModelBoundsAtCurrentOrbit() {
  const std::array<QVector3D, 8> points = {
      QVector3D(-6.0F, -2.0F, -1.0F), QVector3D(6.0F, -2.0F, -1.0F),
      QVector3D(-6.0F, 2.0F, -1.0F),  QVector3D(6.0F, 2.0F, -1.0F),
      QVector3D(-6.0F, -2.0F, 1.0F),  QVector3D(6.0F, -2.0F, 1.0F),
      QVector3D(-6.0F, 2.0F, 1.0F),   QVector3D(6.0F, 2.0F, 1.0F),
  };
  constexpr OrbitAngles angles{42.0F, 24.0F};
  constexpr float aspectRatio = 16.0F / 9.0F;
  constexpr float margin = 1.08F;
  const std::optional<ViewportCameraFrame> result =
      viewportFrameForPoints(points, angles, aspectRatio, false, margin);

  QVERIFY(result.has_value());
  QCOMPARE(result->target, QVector3D());
  QVERIFY(result->distance > 0.0F);
  QVERIFY(result->distance < result->radius * 2.8F);

  const OrbitFrame orbit = orbitFrame(angles);
  const QVector3D forward = -orbit.cameraOffsetDirection;
  const QVector3D right =
      QVector3D::crossProduct(forward, orbit.upDirection).normalized();
  constexpr float verticalTangent = 0.4244748F;
  const float horizontalTangent = verticalTangent * aspectRatio;
  for (const QVector3D &point : points) {
    const QVector3D offset = point - result->target;
    const float depth = result->distance +
                        QVector3D::dotProduct(offset, forward);
    QVERIFY(depth > 0.0F);
    const float normalizedX =
        std::abs(QVector3D::dotProduct(offset, right)) /
        (depth * horizontalTangent);
    const float normalizedY =
        std::abs(QVector3D::dotProduct(offset, orbit.upDirection)) /
        (depth * verticalTangent);
    QVERIFY(normalizedX <= 1.0F / margin + 1.0e-4F);
    QVERIFY(normalizedY <= 1.0F / margin + 1.0e-4F);
  }

  constexpr float portraitAspect = 0.5F;
  const std::optional<ViewportCameraFrame> orthographic =
      viewportFrameForPoints(points, angles, portraitAspect, true, margin);
  QVERIFY(orthographic.has_value());
  const float orthographicHalfWidth =
      orthographic->distance * verticalTangent * portraitAspect;
  const float orthographicHalfHeight =
      orthographic->distance * verticalTangent;
  for (const QVector3D &point : points) {
    const QVector3D offset = point - orthographic->target;
    QVERIFY(std::abs(QVector3D::dotProduct(offset, right)) <=
            orthographicHalfWidth / margin + 1.0e-4F);
    QVERIFY(std::abs(
                QVector3D::dotProduct(offset, orbit.upDirection)) <=
            orthographicHalfHeight / margin + 1.0e-4F);
  }
}

void ViewportCameraTests::rejectsInvalidFocusBounds() {
  QVERIFY(!viewportFrameForPoints({}, {}, 1.0F, false).has_value());
  const std::array<QVector3D, 1> invalid = {QVector3D(
      std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F)};
  QVERIFY(!viewportFrameForPoints(invalid, {}, 1.0F, false).has_value());
  const std::array<QVector3D, 1> valid = {QVector3D()};
  QVERIFY(!viewportFrameForPoints(valid, {}, 0.0F, false).has_value());
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

void ViewportCameraTests::keepsWorldGroundGridForObliqueOrthographicViews() {
  QCOMPARE(referenceGridPlane({42.0F, 24.0F}, true),
           ReferenceGridPlane::XY);
  QCOMPARE(referenceGridPlane({-137.0F, -31.0F}, true),
           ReferenceGridPlane::XY);
  QCOMPARE(referenceGridPlane({0.0F, 1.0F}, true),
           ReferenceGridPlane::XZ);
  QCOMPARE(referenceGridPlane({0.0F, 4.0F}, true),
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

void ViewportCameraTests::keepsColoredAxesInsideRenderedGridCoverage() {
  const ReferenceGridScale scale = referenceGridScale(12.14211F, 1077);
  const ReferenceGridDrawSpans spans =
      referenceGridDrawSpans(scale);

  QVERIFY(spans.fineHalfSpan < spans.majorHalfSpan);
  QCOMPARE(spans.axisHalfSpan, spans.majorHalfSpan);
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

void ViewportCameraTests::keepsReferenceMarkerReadableAcrossZoomAndProjection() {
  const QSizeF viewport(800.0, 600.0);
  for (const bool orthographic : {false, true}) {
    QVector<ReferenceAxisVertex> baseline;
    for (const float distance : {0.02F, 12.0F, 1200.0F, 1.0e6F}) {
      const OrbitFrame frame = orbitFrame({42.0F, 24.0F});
      QMatrix4x4 view;
      view.lookAt(frame.cameraOffsetDirection * distance, QVector3D(),
                   frame.upDirection);
      QMatrix4x4 projection;
      if (orthographic) {
        projection.ortho(-distance * 0.56F, distance * 0.56F,
                          -distance * 0.42F, distance * 0.42F,
                          distance * 0.0001F, distance * 10.0F);
      } else {
        projection.perspective(46.0F, 4.0F / 3.0F, distance * 0.0001F,
                                distance * 10.0F);
      }
      const auto vertices = referenceAxisGeometry(projection * view, {}, viewport);
      QVERIFY(!vertices.isEmpty());
      float minY = 1.0F;
      float maxY = -1.0F;
      for (const auto &vertex : vertices) {
        QVERIFY(std::isfinite(vertex.x) && std::isfinite(vertex.y));
        QVERIFY(vertex.z > -1.0F && vertex.z < 1.0F);
        minY = std::min(minY, vertex.y);
        maxY = std::max(maxY, vertex.y);
      }
      const float pixelHeight = (maxY - minY) * 300.0F;
      QVERIFY(pixelHeight >= 75.0F && pixelHeight <= 180.0F);
      if (baseline.isEmpty()) {
        baseline = vertices;
      } else {
        QCOMPARE(vertices.size(), baseline.size());
        for (qsizetype i = 0; i < vertices.size(); ++i) {
          QVERIFY(std::abs(vertices[i].x - baseline[i].x) < 0.0001F);
          QVERIFY(std::abs(vertices[i].y - baseline[i].y) < 0.0001F);
        }
      }
    }
  }
}

void ViewportCameraTests::hidesReferenceMarkerOutsideView() {
  QMatrix4x4 view;
  view.lookAt(QVector3D(0, 0, 10), {}, QVector3D(0, 1, 0));
  QMatrix4x4 projection;
  projection.perspective(46.0F, 4.0F / 3.0F, 0.01F, 100.0F);
  const QMatrix4x4 vp = projection * view;
  QVERIFY(referenceAxisGeometry(vp, QVector3D(0, 0, 20), {800, 600}).isEmpty());
  QVERIFY(referenceAxisGeometry(vp, QVector3D(1000, 0, 0), {800, 600}).isEmpty());
  QVERIFY(referenceAxisGeometry(vp, {}, {0, 0}).isEmpty());
  // Axis-aligned orthographic/perspective views hide the foreshortened axis
  // without discarding the readable pair and origin collar.
  const auto aligned = referenceAxisGeometry(vp, {}, {800, 600});
  QVERIFY(!aligned.isEmpty());
  for (const auto &vertex : aligned) {
    QVERIFY(std::isfinite(vertex.x) && std::isfinite(vertex.y));
  }
}

QTEST_GUILESS_MAIN(ViewportCameraTests)

#include "ViewportCameraTests.moc"
