import test from "node:test";
import assert from "node:assert/strict";

import { textureOptionsFromQuality } from "../static/texture-options.mjs";

test("texture quality maps original to no face limit", () => {
  assert.equal(textureOptionsFromQuality("fast", 4096).maxFaces, 30000);
  assert.equal(textureOptionsFromQuality("balanced", 4096).maxFaces, 100000);
  assert.equal(textureOptionsFromQuality("smooth", 4096).maxFaces, 100000);
  assert.equal(textureOptionsFromQuality("original", 4096).maxFaces, 0);
});

test("texture quality defaults to balanced for unknown values", () => {
  assert.deepEqual(textureOptionsFromQuality("", 4096), {
    maxFaces: 100000,
    textureRes: 4096,
    localSeamLeveling: false,
    costSmoothnessRatio: 0.3,
    textureSizeMultiple: 0,
  });
  assert.equal(textureOptionsFromQuality("unexpected", 4096).maxFaces, 100000);
});

test("ultra texture quality keeps the original mesh and forces 8192 texture size", () => {
  assert.deepEqual(textureOptionsFromQuality("ultra", 4096), {
    maxFaces: 0,
    textureRes: 8192,
    localSeamLeveling: false,
    costSmoothnessRatio: 0.3,
    textureSizeMultiple: 8192,
  });
  assert.equal(textureOptionsFromQuality("ultra", 8192).textureRes, 8192);
});

test("smooth texture quality decimates to fewer patches and avoids unstable local seam leveling", () => {
  assert.deepEqual(textureOptionsFromQuality("smooth", 4096), {
    maxFaces: 100000,
    textureRes: 8192,
    localSeamLeveling: false,
    costSmoothnessRatio: 0.3,
    textureSizeMultiple: 8192,
  });
});
