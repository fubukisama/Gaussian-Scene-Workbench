export const AXIS_GIZMO_AXES = [
  { label: "X", color: "#ff4d4d", vector: [1, 0, 0] },
  { label: "Y", color: "#6ee27d", vector: [0, 1, 0] },
  { label: "Z", color: "#6d8cff", vector: [0, 0, 1] }
];
export const CENTER_GIZMO_SCREEN_RADIUS_PX = 92;

function dot(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function finiteVector(value, fallback) {
  if (!Array.isArray(value) || value.length !== 3 || value.some((item) => !Number.isFinite(item))) {
    return fallback;
  }
  return value;
}

export function axisGizmoSegments(options) {
  options = options || {};
  const width = options.width;
  const height = options.height;
  const length = options.length === undefined ? 42 : options.length;
  const margin = options.margin === undefined ? 24 : options.margin;
  const cameraRight = options.cameraRight || [1, 0, 0];
  const cameraUp = options.cameraUp || [0, 0, 1];
  const w = Number.isFinite(width) ? width : 0;
  const h = Number.isFinite(height) ? height : 0;
  const axisLength = Number.isFinite(length) ? length : 42;
  const inset = Number.isFinite(margin) ? margin : 24;
  const origin = {
    x: Math.round(w - inset - axisLength),
    y: Math.round(h - inset - axisLength)
  };
  const right = finiteVector(cameraRight, [1, 0, 0]);
  const up = finiteVector(cameraUp, [0, 0, 1]);
  return AXIS_GIZMO_AXES.map((axis) => {
    return {
      label: axis.label,
      color: axis.color,
      vector: axis.vector,
      start: origin,
      end: {
        x: Math.round(origin.x + dot(axis.vector, right) * axisLength),
        y: Math.round(origin.y - dot(axis.vector, up) * axisLength)
      }
    };
  });
}

export function centerGizmoMetrics(bounds) {
  if (!bounds || !Array.isArray(bounds.min) || !Array.isArray(bounds.max)) {
    return { visible: false, center: [0, 0, 0], radius: 1 };
  }
  const min = finiteVector(bounds.min, null);
  const max = finiteVector(bounds.max, null);
  if (!min || !max) {
    return { visible: false, center: [0, 0, 0], radius: 1 };
  }
  const size = [
    Math.max(0, max[0] - min[0]),
    Math.max(0, max[1] - min[1]),
    Math.max(0, max[2] - min[2])
  ];
  const diagonal = Math.hypot(size[0], size[1], size[2]);
  const longest = Math.max(size[0], size[1], size[2], 1);
  return {
    visible: true,
    center: [
      (min[0] + max[0]) * 0.5,
      (min[1] + max[1]) * 0.5,
      (min[2] + max[2]) * 0.5
    ],
    radius: Math.max(diagonal * 0.075, longest * 0.045, 0.05)
  };
}

export function screenStableGizmoScale(options = {}) {
  const distance = options.distance;
  const fovDeg = options.fovDeg;
  const viewportHeight = options.viewportHeight;
  const pixelRadius = options.pixelRadius === undefined ? CENTER_GIZMO_SCREEN_RADIUS_PX : options.pixelRadius;
  const fallback = options.fallback === undefined ? 1 : options.fallback;
  if (
    !Number.isFinite(distance) ||
    !Number.isFinite(fovDeg) ||
    !Number.isFinite(viewportHeight) ||
    !Number.isFinite(pixelRadius) ||
    distance <= 0 ||
    viewportHeight <= 0 ||
    pixelRadius <= 0
  ) {
    return fallback;
  }
  const visibleWorldHeight = 2 * Math.tan((fovDeg * Math.PI / 180) * 0.5) * distance;
  return Math.max((visibleWorldHeight * pixelRadius) / viewportHeight, 0.0001);
}
