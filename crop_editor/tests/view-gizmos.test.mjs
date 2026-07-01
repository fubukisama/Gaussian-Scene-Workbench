import test from "node:test";
import assert from "node:assert/strict";
import { axisGizmoSegments, centerGizmoMetrics, screenStableGizmoScale } from "../static/view-gizmos.mjs";

test("axis gizmo anchors to the lower right and projects camera-facing axes", () => {
  const segments = axisGizmoSegments({
    width: 1000,
    height: 600,
    length: 40,
    margin: 24,
    cameraRight: [1, 0, 0],
    cameraUp: [0, 0, 1],
  });

  assert.deepEqual(segments.map((segment) => segment.label), ["X", "Y", "Z"]);
  assert.deepEqual(segments[0].start, { x: 936, y: 536 });
  assert.deepEqual(segments[0].end, { x: 976, y: 536 });
  assert.deepEqual(segments[1].end, { x: 936, y: 536 });
  assert.deepEqual(segments[2].end, { x: 936, y: 496 });
});

test("center gizmo metrics use bounds center and a stable radius", () => {
  const metrics = centerGizmoMetrics({
    min: [-2, -1, 1],
    max: [2, 3, 5],
  });

  assert.deepEqual(metrics.center, [0, 1, 3]);
  assert.ok(metrics.radius > 0.4);
  assert.equal(centerGizmoMetrics(null).visible, false);
});

test("center gizmo world scale preserves a fixed screen radius", () => {
  const near = screenStableGizmoScale({
    distance: 5,
    fovDeg: 60,
    viewportHeight: 500,
    pixelRadius: 90,
  });
  const far = screenStableGizmoScale({
    distance: 10,
    fovDeg: 60,
    viewportHeight: 500,
    pixelRadius: 90,
  });

  assert.ok(near > 0);
  assert.equal(Number((far / near).toFixed(6)), 2);
  assert.equal(screenStableGizmoScale({ distance: 0, fallback: 3 }), 3);
});
