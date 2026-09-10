#include "ViewportCamera.h"
#include "ReferenceAxisGeometry.h"

#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
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
  void preservesTrackballCameraRollAndPoleContinuity();
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
  void keepsReferenceAxesAtExactWorldLength();
  void clipsReferenceAxesWithoutResizing();
  void keepsOrthographicAxisLengthIndependentOfCameraDistance();
  void keepsReferenceMarkerConsistentAcrossSceneUnits();
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

void ViewportCameraTests::preservesTrackballCameraRollAndPoleContinuity() {
  for (const OrbitAngles original : {OrbitAngles{42, 24, 35}, OrbitAngles{0, 90, -70},
                                    OrbitAngles{12, -90, 125}, OrbitAngles{37, 150, -42}}) {
    const auto before = orbitFrame(original);
    for (const QVector3D axis : {QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(0, 0, 1)}) {
      const auto rotation = QQuaternion::fromAxisAndAngle(axis, 113.0F);
      const auto actual = orbitFrame(orbitAnglesAfterRotation(original, rotation));
      QVERIFY((actual.cameraOffsetDirection - rotation.rotatedVector(before.cameraOffsetDirection)).length() < 1.0e-5F);
      QVERIFY((actual.upDirection - rotation.rotatedVector(before.upDirection)).length() < 1.0e-5F);
      QVERIFY(std::abs(QVector3D::dotProduct(actual.cameraOffsetDirection, actual.upDirection)) < 1.0e-5F);
    }
  }
  OrbitAngles state{42, 24, 18};
  const auto original = orbitFrame(state);
  for (int step = 0; step < 360; ++step)
    state = orbitAnglesAfterRotation(state, QQuaternion::fromAxisAndAngle(1, 0, 0, 1));
  QVERIFY((orbitFrame(state).upDirection - original.upDirection).length() < 1.0e-3F);
  QVERIFY((orbitFrame(state).cameraOffsetDirection - original.cameraOffsetDirection).length() < 1.0e-3F);
  QCOMPARE(orbitAnglesAfterLeftDrag({42, 24, 55}, {20, 30}).rollDegrees, 55.0F);
  QCOMPARE(orbitAnglesAfterRotation(state, QQuaternion(0, 0, 0, 0)), state);
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

void ViewportCameraTests::keepsReferenceAxesAtExactWorldLength() {
  const QSizeF viewport(800.0, 600.0);
  for (const bool orthographic : {false, true}) {
    for (const float density : {0.85F, 1.0F, 1.4F}) {
      float previousLength = 0.0F;
      for (const float distance : {12.0F, 6.0F, 3.0F}) {
        QMatrix4x4 view;
        view.lookAt(QVector3D(0, 0, distance), {}, QVector3D(0, 1, 0));
        QMatrix4x4 projection;
        if (orthographic) {
          projection.ortho(-distance * 0.56F, distance * 0.56F,
                           -distance * 0.42F, distance * 0.42F, 0.01F, 100.0F);
        } else {
          projection.perspective(46.0F, 4.0F / 3.0F, 0.01F, 100.0F);
        }
        const QMatrix4x4 vp = projection * view;
        const auto vertices = referenceAxisGeometry(vp, {}, viewport, density, 1.2F);
        QVERIFY(!vertices.isEmpty());
        for (const QVector3D endpoint : {QVector3D(1.2F, 0, 0), QVector3D(0, 1.2F, 0)}) {
          const QVector3D expected = (vp * QVector4D(endpoint, 1)).toVector3DAffine();
          // Verify actual emitted arrow-tip vertices, not merely a monotonic
          // size trend (which incorrectly accepted the old saturation curve).
          QVERIFY(std::any_of(vertices.cbegin(), vertices.cend(), [&](const auto &v) {
            return (QVector3D(v.x, v.y, v.z) - expected).length() < 0.00001F;
          }));
        }
        const float pixelLength = (vp * QVector4D(1.2F, 0, 0, 1)).toVector3DAffine().x() * 400.0F;
        if (previousLength > 0.0F) {
          QVERIFY(std::abs(pixelLength / previousLength - 2.0F) < 0.0001F);
        }
        previousLength = pixelLength;
      }
      QVERIFY(previousLength > 250.0F); // Beyond both former pixel-size caps.
    }
  }
}

void ViewportCameraTests::clipsReferenceAxesWithoutResizing() {
  for (const bool orthographic : {false, true}) {
    for (const float distance : {0.02F, 12.0F, 1200.0F, 1.0e6F}) {
      const OrbitFrame frame = orbitFrame({42.0F, 24.0F});
      QMatrix4x4 view;
      view.lookAt(frame.cameraOffsetDirection * distance, {}, frame.upDirection);
      QMatrix4x4 projection;
      if (orthographic) {
        projection.ortho(-distance * 0.56F, distance * 0.56F,
                         -distance * 0.42F, distance * 0.42F,
                         distance * 0.0001F, distance * 10.0F);
      } else {
        projection.perspective(46.0F, 4.0F / 3.0F, distance * 0.0001F,
                               distance * 10.0F);
      }
      const auto vertices = referenceAxisGeometry(projection * view, {}, {800, 600});
      QVERIFY(!vertices.isEmpty());
      float extent = 0.0F;
      for (const auto &v : vertices) {
        QVERIFY(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
        QVERIFY(v.z >= -1.0F && v.z <= 1.0F);
        QVERIFY(std::abs(v.x) < 1.1F && std::abs(v.y) < 1.1F);
        extent = std::max({extent, std::abs(v.x), std::abs(v.y)});
      }
      if (distance == 0.02F) {
        QVERIFY2(extent > 0.99F, "Close-up shafts must extend to the viewport edge");
      } else if (distance >= 1200.0F) {
        QVERIFY2(extent < 0.004F, "Distant axes must not be expanded to a minimum pixel length");
      }
    }
  }
  QMatrix4x4 view;
  view.lookAt(QVector3D(0, 0, 10), {}, QVector3D(0, 1, 0));
  QMatrix4x4 projection;
  projection.ortho(-2, 2, -2, 2, 0.01F, 100.0F);
  // An offscreen origin does not discard an axis crossing the visible scene.
  const auto crossing = referenceAxisGeometry(projection * view, {-5, 0, 0}, {800, 600}, 1, 10);
  QVERIFY(!crossing.isEmpty());
  float minX = 1.0F;
  float maxX = -1.0F;
  for (const auto &v : crossing) {
    minX = std::min(minX, v.x);
    maxX = std::max(maxX, v.x);
    QVERIFY(std::abs(v.y) < 0.01F); // Shaft only, no fake arrow on the clip edge.
  }
  QVERIFY(minX < -0.99F && maxX > 0.99F);
}

void ViewportCameraTests::keepsOrthographicAxisLengthIndependentOfCameraDistance() {
  QMatrix4x4 projection;
  projection.ortho(-4, 4, -3, 3, 0.01F, 100.0F);
  QVector<ReferenceAxisVertex> baseline;
  for (const float distance : {3.0F, 6.0F, 12.0F}) {
    QMatrix4x4 view;
    view.lookAt(QVector3D(0, 0, distance), {}, QVector3D(0, 1, 0));
    const auto vertices = referenceAxisGeometry(projection * view, {}, {800, 600});
    if (baseline.isEmpty()) {
      baseline = vertices;
      QVERIFY(!baseline.isEmpty());
    } else {
      QCOMPARE(vertices.size(), baseline.size());
      for (qsizetype i = 0; i < vertices.size(); ++i) {
        QCOMPARE(vertices[i].x, baseline[i].x);
        QCOMPARE(vertices[i].y, baseline[i].y);
      }
    }
  }
}

void ViewportCameraTests::keepsReferenceMarkerConsistentAcrossSceneUnits() {
  for (const bool orthographic : {false, true}) {
    QVector<ReferenceAxisVertex> baseline;
    for (const float unitScale : {0.001F, 1.0F, 1000.0F}) {
      const float distance = 12.0F * unitScale;
      const OrbitFrame frame = orbitFrame({42.0F, 24.0F});
      QMatrix4x4 view;
      view.lookAt(frame.cameraOffsetDirection * distance, {}, frame.upDirection);
      QMatrix4x4 projection;
      if (orthographic) {
        projection.ortho(-distance * 0.56F, distance * 0.56F,
                         -distance * 0.42F, distance * 0.42F,
                         distance * 0.0001F, distance * 10.0F);
      } else {
        projection.perspective(46.0F, 4.0F / 3.0F, distance * 0.0001F,
                               distance * 10.0F);
      }
      const auto vertices = referenceAxisGeometry(
          projection * view, {}, {800, 600}, 1.0F, 1.2F * unitScale);
      QVERIFY(!vertices.isEmpty());
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
