import test from "node:test";
import assert from "node:assert/strict";
import { clampPanelPosition } from "../static/floating-panels.mjs";

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
