export const MAX_PICKING_ID = 0xffffff - 1;

export function encodePickingId(index) {
  if (!Number.isInteger(index) || index < 0 || index > MAX_PICKING_ID) {
    throw new Error(`Invalid picking index: ${index}`);
  }
  const value = index + 1;
  return [
    value & 0xff,
    (value >> 8) & 0xff,
    (value >> 16) & 0xff,
  ];
}

export function decodePickingId(r, g, b) {
  const value = (r | (g << 8) | (b << 16)) >>> 0;
  return value === 0 ? -1 : value - 1;
}

export function pickingBoundsForRect(a, b, viewport) {
  const minX = Math.min(a.x, b.x);
  const maxX = Math.max(a.x, b.x);
  const minY = Math.min(a.y, b.y);
  const maxY = Math.max(a.y, b.y);
  return clampPickingBounds({ x: minX, y: minY, width: maxX - minX, height: maxY - minY }, viewport);
}

export function pickingBoundsForBrush(center, radius, viewport) {
  return clampPickingBounds({
    x: center.x - radius,
    y: center.y - radius,
    width: radius * 2,
    height: radius * 2,
  }, viewport);
}

export function pickingBoundsForPolygon(poly, viewport) {
  if (!Array.isArray(poly) || poly.length < 3) return null;
  let minX = Infinity;
  let maxX = -Infinity;
  let minY = Infinity;
  let maxY = -Infinity;
  for (const p of poly) {
    minX = Math.min(minX, p.x);
    maxX = Math.max(maxX, p.x);
    minY = Math.min(minY, p.y);
    maxY = Math.max(maxY, p.y);
  }
  return clampPickingBounds({ x: minX, y: minY, width: maxX - minX, height: maxY - minY }, viewport);
}

export function deviceReadRect(cssBounds, pixelRatio, targetHeight) {
  const ratio = Math.max(1, pixelRatio || 1);
  const x = Math.max(0, Math.floor(cssBounds.x * ratio));
  const top = Math.max(0, Math.floor(cssBounds.y * ratio));
  const width = Math.max(1, Math.ceil(cssBounds.width * ratio));
  const height = Math.max(1, Math.ceil(cssBounds.height * ratio));
  return {
    x,
    y: Math.max(0, targetHeight - top - height),
    width,
    height,
    pixelRatio: ratio,
  };
}

export function collectPickingIds(buffer, readRect, cssBounds, shape = null) {
  const ids = new Set();
  const ratio = Math.max(1, readRect.pixelRatio || 1);
  for (let row = 0; row < readRect.height; row++) {
    const cssY = cssBounds.y + cssBounds.height - ((row + 0.5) / ratio);
    for (let col = 0; col < readRect.width; col++) {
      const cssX = cssBounds.x + ((col + 0.5) / ratio);
      if (shape && !shape(cssX, cssY)) continue;
      const offset = (row * readRect.width + col) * 4;
      const id = decodePickingId(buffer[offset], buffer[offset + 1], buffer[offset + 2]);
      if (id >= 0) ids.add(id);
    }
  }
  return ids;
}

export function pointInPickingPolygon(x, y, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const xi = poly[i].x;
    const yi = poly[i].y;
    const xj = poly[j].x;
    const yj = poly[j].y;
    const intersect = ((yi > y) !== (yj > y)) &&
      (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

export function shouldSampleBrushPicking({ last, point, radius, subtract = false, force = false }) {
  if (force || !last) return true;
  if (last.subtract !== subtract) return true;
  const threshold = Math.max(1, radius * 0.33);
  const dx = point.x - last.x;
  const dy = point.y - last.y;
  return dx * dx + dy * dy >= threshold * threshold;
}

export function pickingRenderKey({
  editRevision,
  pointCount,
  width,
  height,
  pixelRatio,
  viewMatrix,
  projectionMatrix,
}) {
  return [
    editRevision,
    pointCount,
    width,
    height,
    Number(pixelRatio || 1).toFixed(3),
    matrixKey(viewMatrix),
    matrixKey(projectionMatrix),
  ].join("|");
}

function clampPickingBounds(bounds, viewport) {
  const width = Math.max(1, viewport?.width || 1);
  const height = Math.max(1, viewport?.height || 1);
  const x0 = Math.max(0, Math.floor(bounds.x));
  const y0 = Math.max(0, Math.floor(bounds.y));
  const x1 = Math.min(width, Math.ceil(bounds.x + bounds.width));
  const y1 = Math.min(height, Math.ceil(bounds.y + bounds.height));
  if (x1 <= x0 || y1 <= y0) return null;
  return { x: x0, y: y0, width: x1 - x0, height: y1 - y0 };
}

function matrixKey(values) {
  return Array.from(values || []).map((value) => Number(value || 0).toFixed(5)).join(",");
}
