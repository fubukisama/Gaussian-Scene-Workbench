import test from "node:test";
import assert from "node:assert/strict";

import { sugarOptionsFromPreset } from "../static/sugar-options.mjs";

test("SuGaR preview preset favors fast low-poly validation", () => {
  assert.deepEqual(sugarOptionsFromPreset("preview"), {
    sugar_quality: "low",
    sugar_refinement_time: "short",
    sugar_regularization: "dn_consistency",
    sugar_surface_level: 0.3,
    sugar_square_size: 8,
    sugar_postprocess: false,
    sugar_max_images: 96,
    sugar_max_image_size: 960,
  });
});

test("SuGaR high and ultra presets opt into heavier reconstruction", () => {
  assert.equal(sugarOptionsFromPreset("high").sugar_quality, "high");
  assert.equal(sugarOptionsFromPreset("high").sugar_refinement_time, "medium");
  assert.equal(sugarOptionsFromPreset("high").sugar_max_images, 256);
  assert.equal(sugarOptionsFromPreset("ultra").sugar_quality, "high");
  assert.equal(sugarOptionsFromPreset("ultra").sugar_refinement_time, "long");
  assert.equal(sugarOptionsFromPreset("ultra").sugar_max_images, 0);
});

test("SuGaR preset lookup falls back to preview", () => {
  assert.deepEqual(sugarOptionsFromPreset("unknown"), sugarOptionsFromPreset("preview"));
});
