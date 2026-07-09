import test from "node:test";
import assert from "node:assert/strict";

import {
  collectPickingIds,
  decodePickingId,
  deviceReadRect,
  encodePickingId,
  pickingRenderKey,
  pickingBoundsForBrush,
  pickingBoundsForPolygon,
  pickingBoundsForRect,
  pointInPickingPolygon,
  shouldSampleBrushPicking,
} from "../static/selection-picking.mjs";

test("picking IDs round trip through RGB bytes", () => {
  for (const id of [0, 1, 255, 256, 65535, 65536, 1234567]) {
    const [r, g, b] = encodePickingId(id);
    assert.equal(decodePickingId(r, g, b), id);
  }
  assert.equal(decodePickingId(0, 0, 0), -1);
});

test("selection bounds are clamped to the viewport", () => {
  assert.deepEqual(pickingBoundsForRect({ x: 8, y: 9 }, { x: 2, y: 3 }, { width: 10, height: 10 }), {
    x: 2,
    y: 3,
    width: 6,
    height: 6,
  });
  assert.deepEqual(pickingBoundsForBrush({ x: 3, y: 3 }, 5, { width: 10, height: 10 }), {
    x: 0,
    y: 0,
    width: 8,
    height: 8,
  });
  assert.equal(pickingBoundsForPolygon([{ x: -5, y: -5 }, { x: -1, y: -5 }, { x: -2, y: -1 }], { width: 10, height: 10 }), null);
});

test("device read rect converts top-left CSS bounds to WebGL bottom-left pixels", () => {
  assert.deepEqual(deviceReadRect({ x: 10, y: 20, width: 30, height: 40 }, 2, 200), {
    x: 20,
    y: 80,
    width: 60,
    height: 80,
    pixelRatio: 2,
  });
});

test("collect IDs from picking pixels with optional shape filtering", () => {
  const readRect = { x: 0, y: 0, width: 3, height: 2, pixelRatio: 1 };
  const cssBounds = { x: 0, y: 0, width: 3, height: 2 };
  const pixels = new Uint8Array(readRect.width * readRect.height * 4);
  const write = (col, row, id) => {
    const [r, g, b] = encodePickingId(id);
    const offset = (row * readRect.width + col) * 4;
    pixels[offset] = r;
    pixels[offset + 1] = g;
    pixels[offset + 2] = b;
    pixels[offset + 3] = 255;
  };
  write(0, 0, 3);
  write(1, 0, 4);
  write(0, 1, 5);

  assert.deepEqual(Array.from(collectPickingIds(pixels, readRect, cssBounds)).sort((a, b) => a - b), [3, 4, 5]);
  const topRowOnly = (_x, y) => y < 1;
  assert.deepEqual(Array.from(collectPickingIds(pixels, readRect, cssBounds, topRowOnly)), [5]);
});

test("polygon point test matches lasso inclusion", () => {
  const poly = [{ x: 0, y: 0 }, { x: 4, y: 0 }, { x: 4, y: 4 }, { x: 0, y: 4 }];
  assert.equal(pointInPickingPolygon(2, 2, poly), true);
  assert.equal(pointInPickingPolygon(6, 2, poly), false);
});

test("brush picking skips dense move events but samples meaningful movement", () => {
  const last = { x: 100, y: 100, subtract: false };
  assert.equal(shouldSampleBrushPicking({ last, point: { x: 104, y: 103 }, radius: 44, subtract: false }), false);
  assert.equal(shouldSampleBrushPicking({ last, point: { x: 118, y: 100 }, radius: 44, subtract: false }), true);
  assert.equal(shouldSampleBrushPicking({ last, point: { x: 104, y: 103 }, radius: 44, subtract: true }), true);
  assert.equal(shouldSampleBrushPicking({ last, point: { x: 104, y: 103 }, radius: 44, subtract: false, force: true }), true);
});

test("picking render key changes when camera or edit state changes", () => {
  const base = {
    editRevision: 1,
    pointCount: 10,
    width: 100,
    height: 80,
    pixelRatio: 2,
    viewMatrix: [1, 0, 0, 0],
    projectionMatrix: [1, 0, 0, 1],
  };
  assert.equal(pickingRenderKey(base), pickingRenderKey({ ...base }));
  assert.notEqual(pickingRenderKey(base), pickingRenderKey({ ...base, editRevision: 2 }));
  assert.notEqual(pickingRenderKey(base), pickingRenderKey({ ...base, viewMatrix: [1, 0, 0, 0.001] }));
});
