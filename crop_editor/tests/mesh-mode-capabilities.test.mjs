import test from "node:test";
import assert from "node:assert/strict";
import { meshActionsForMode, meshModeMatchesBackend, meshModeSupportsTextureBake } from "../static/mesh-mode-capabilities.mjs";

test("SuGaR mesh actions are available for loaded 3DGS scenes", () => {
  assert.equal(meshModeMatchesBackend("sugar", "3dgs"), true);
  const actions = meshActionsForMode("sugar", "3dgs", true, false);
  assert.equal(actions.exportMeshDisabled, false);
  assert.equal(actions.loadMeshDisabled, false);
  assert.equal(actions.loadTextureDisabled, false);
  assert.equal(actions.bakeTextureDisabled, true);
});

test("GS2Mesh mesh actions are available for loaded 3DGS scenes", () => {
  assert.equal(meshModeMatchesBackend("gs2mesh", "3dgs"), true);
  assert.equal(meshModeSupportsTextureBake("gs2mesh", "3dgs"), true);
  const actions = meshActionsForMode("gs2mesh", "3dgs", true, false);
  assert.equal(actions.exportMeshDisabled, false);
  assert.equal(actions.loadMeshDisabled, false);
  assert.equal(actions.loadTextureDisabled, false);
  assert.equal(actions.bakeTextureDisabled, false);
});

test("2DGS mesh actions stay available only for 2DGS scenes", () => {
  assert.equal(meshModeMatchesBackend("bounded", "2dgs"), true);
  assert.equal(meshModeMatchesBackend("bounded", "3dgs"), false);
  assert.equal(meshActionsForMode("bounded", "3dgs", true, false).exportMeshDisabled, true);
  assert.equal(meshActionsForMode("bounded", "2dgs", true, false).exportMeshDisabled, false);
});
