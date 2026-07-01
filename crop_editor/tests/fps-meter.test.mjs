import test from "node:test";
import assert from "node:assert/strict";

import { createFpsMeter } from "../static/fps-meter.mjs";

test("fps meter reports completed frame rate windows", () => {
  const meter = createFpsMeter({ sampleMs: 500 });

  assert.equal(meter.tick(1000), 0);
  for (let i = 1; i <= 30; i++) meter.tick(1000 + i * (500 / 30));

  assert.equal(meter.value(), 60);
});

test("fps meter reports real requestAnimationFrame cadence, not render throughput", () => {
  const meter = createFpsMeter({ sampleMs: 500 });

  meter.tick(1000, 4);
  for (let i = 1; i <= 30; i++) meter.tick(1000 + i * (500 / 30), 4);

  assert.equal(meter.value(), 60);
});

test("fps meter ignores invalid timestamps", () => {
  const meter = createFpsMeter({ sampleMs: 100 });

  meter.tick(1000);

  assert.equal(meter.tick(Number.NaN), 0);
});
