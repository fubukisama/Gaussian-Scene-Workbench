import test from "node:test";
import assert from "node:assert/strict";
import { existingTrainingOutputSceneName, trainingOutputSceneName } from "../static/training-targets.mjs";

test("2DGS output keeps the historical _2dgs suffix", () => {
  assert.equal(trainingOutputSceneName("scene_a", "2dgs"), "scene_a_2dgs");
  assert.equal(trainingOutputSceneName("scene_a_2dgs", "2dgs"), "scene_a_2dgs");
});

test("existing-data training avoids cross-backend output collisions", () => {
  const outputs = new Map([["6.22", "2dgs"]]);
  assert.equal(existingTrainingOutputSceneName("6.22", "6.22", "3dgs", outputs), "6.22_3dgs");
});

test("existing-data training preserves explicit distinct output names", () => {
  const outputs = new Map([["6.22", "2dgs"]]);
  assert.equal(existingTrainingOutputSceneName("6.22", "6.22_retry", "3dgs", outputs), "6.22_retry");
});
