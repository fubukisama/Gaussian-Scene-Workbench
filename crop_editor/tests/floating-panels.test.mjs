import test from "node:test";
import assert from "node:assert/strict";
import { clampPanelPosition, clampPanelSize } from "../static/floating-panels.mjs";

test("floating panel positions stay inside the viewport", () => {
  assert.deepEqual(clampPanelPosition({
    left: -200,
    top: -50,
    width: 300,
    height: 180,
    viewportWidth: 1200,
    viewportHeight: 800,
    margin: 8,
  }), { left: 8, top: 8 });

  assert.deepEqual(clampPanelPosition({
    left: 1100,
    top: 760,
    width: 300,
    height: 180,
    viewportWidth: 1200,
    viewportHeight: 800,
    margin: 8,
  }), { left: 892, top: 612 });
});

test("floating panel sizes stay usable inside the viewport", () => {
  assert.deepEqual(clampPanelSize({
    width: 120,
    height: 80,
    minWidth: 280,
    minHeight: 180,
    maxWidth: 900,
    maxHeight: 700,
    viewportWidth: 1200,
    viewportHeight: 800,
    margin: 8,
  }), { width: 280, height: 180 });

  assert.deepEqual(clampPanelSize({
    width: 2000,
    height: 1200,
    minWidth: 280,
    minHeight: 180,
    maxWidth: 1900,
    maxHeight: 1100,
    viewportWidth: 1200,
    viewportHeight: 800,
    margin: 8,
  }), { width: 1184, height: 784 });
});
