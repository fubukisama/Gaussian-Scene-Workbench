import test from "node:test";
import assert from "node:assert/strict";

import { shouldKeepUnsavedMeshTrim } from "../static/mesh-state.mjs";

test("load mesh keeps the current model when mesh trim edits are unsaved", () => {
  assert.equal(shouldKeepUnsavedMeshTrim(true, { faceCount: 10 }), true);
});

test("load mesh may reload from disk when there are no unsaved mesh trim edits", () => {
  assert.equal(shouldKeepUnsavedMeshTrim(false, { faceCount: 10 }), false);
  assert.equal(shouldKeepUnsavedMeshTrim(true, null), false);
});
